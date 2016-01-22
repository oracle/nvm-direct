/*
Copyright (c) 2015, 2016, Oracle and/or its affiliates. All rights reserved.

The Universal Permissive License (UPL), Version 1.0

Subject to the condition set forth below, permission is hereby granted to any
person obtaining a copy of this software, associated documentation and/or data
(collectively the "Software"), free of charge and under any and all copyright
rights in the Software, and any and all patent rights owned or freely
licensable by each licensor hereunder covering either (i) the unmodified
Software as contributed to or provided by such licensor, or (ii) the Larger
Works (as defined below), to deal in both

(a) the Software, and

(b) any piece of software and/or hardware listed in the lrgrwrks.txt file if
one is included with the Software (each a "Larger Work" to which the Software
is contributed by such licensors),

without restriction, including without limitation the rights to copy, create
derivative works of, display, perform, and distribute the Software and make,
use, sell, offer for sale, import, export, have made, and have sold the
Software and the Larger Work(s), and to sublicense the foregoing rights on
either these or other terms.

This license is subject to the following condition:

The above copyright notice and either this complete permission notice or at a
minimum a reference to the UPL must be included in all copies or substantial
portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
/** \file dmf.c
 *  \brief Direct Mapped File providing atomic block read/write to NVM
 *
 *  Created on: Dec 17, 2015
 *      Author: bbridge
 *
 * This example uses NVM Direct to create a region file that supports atomic
 * block I/O to NVM using memcpy. It supports an arbitrary number of threads
 * simultaneously doing reads and writes. The size of the file can be changed
 * concurrently with I/O. A process may have many Direct Mapped files open
 * simultaneously.This initial version does not support sparse files.
 *
 * This is similar to the functionality provided by Block Translation Table.
 * BTT source is at https://github.com/pmem/nvml/tree/master/src/libpmemblk
 *
 * This must be linked with the NVM Direct shared library. Currently this only
 * works on 64 bit Linux.
 *
 * A Direct Mapped File has 4 values that define its layout:
 *   - Block size: All I/O copies an integral number of blocks of this size.
 *   - File size: The file contains this many file blocks of NVM
 *   - Max file size: This is the largest value file size can be set to.
 *   - Concurrent writes: Performance is best if there are less writes than this
 *
 * The block size, file size, and concurrent writes determine how much NVM the
 * file consumes. The file size can be changed while running. The max file
 * size determines how much virtual address space the file consumes when it
 * is opened.
 *
 * A Direct Mapped File contains 3 NVM extents.
 *   - The base extent contains the NVM Direct metadata, the DMF root struct,\n
 *     an array of pointers to free physical blocks, and an array of mutexes \n
 *     to prevent simultaneous writes to the same block
 *   - The pointer extent is an application managed extent that contains a \n
 *     pointer to a physical block for every file block.
 *   - The block extent is an application managed extent that is  an \n
 *     array of physical blocks that are used to hold file block contents. \n
 *     Each block has some metadata to indicate which file block it contains \n
 *     and a reused counter to detect collisions with writes.
 *
 * Writing a block basically does the following. See dmf_write for details.
 *   1. Lock a write mutex based on a hash of the file block number.
 *   2. Reserve one of the free block slots using DRAM data structures.
 *   3. Make the reused count negative to indicate the free block is busy.
 *   4. Copy the new data to the free block.
 *   5. Begin a new NVM transaction.
 *   6. Create an on-commit operation to make the reused count be the next \n
 *      positive integer.
 *   7. Set the file block number in the header of the new block.
 *   8. Switch the old physical block with the new one so that the block \n
 *      with the new data is now current and the old one is now free.
 *   9. Commit the NVM transaction running the on-unlock operation to \n
 *      advance the reused count if and only if the block switch commits \n
 *      and finally unlocking the write mutex.
 *   10.Release the reservation on the free block slot.
 *   11.Release the lock on the write mutex.
 *
 * Reading a block basically does the following. See dmf_read for details.
 *   1. Index into the pointer extent to find the current physical block. \n
 *      Keep the pointer to this physical block.
 *   2. Wait until the reused count is positive. (This is very rare)
 *   3. Save the positive reused count.
 *   4. If the block is no longer for the correct physical block start over
 *   5. Copy the data out of the block into the read buffer.
 *   6. If the reused count in the physical block does not match the value \n
 *      saved in step 3 then start over.
 *
 * Growing a file basically does the following. See dmf_grow for details.
 *   1. Begin an NVM transaction.
 *   2. Grow the block and pointer extents to the new size.
 *   3. Make the new pointers point at the new blocks.
 *   4. Store the new file size.
 *   5. Commit the NVM transaction.
 *
 * Shrinking a file basically does the following. See dmf_shrink for details.
 *   1. Set the new smaller file size
 *   2. Get and drop every write mutex to ensure no more writes past the new \n
 *      file size.
 *   3.Reorganize the mapping of file blocks to physical blocks so that the \n
 *     file blocks to be deleted use physical blocks at the end of the block \n
 *     extent. Each block moved to a new physical block is done atomically.
 *   4.Resize the block and pointer extents to the new size.
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <setjmp.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <error.h>
#include <sched.h>
#define NVM_EXT // this uses the NVM Direct C extensions
#include "nvm.h"
#include "dmf.h"

/*
 * Incomplete types for easy reference
 */
typedef persistent struct dmf_block dmf_block;
typedef persistent struct dmf_free dmf_free;
typedef persistent struct dmf_root dmf_root;


/*
 * This struct holds one physical block of NVM. Pointers in the pointer extent
 * and pointers in the free array point to these block structs. Over the life
 * of the file a physical block may contain data for different file blocks.
 *
 * This struct ends in a zero length array that is actually the size of the
 * block in bytes. It contains the data for the current file block.
 *
 * The block extent is an array of these structs rather than a heap. Since
 * these structs are not allocated from a heap there is no requirement for
 * them to have a USID. However there is one to catch corruption.
 */
persistent struct dmf_block
USID("588b  4348    a236    382a    43bb    fc65    4ecb    1524")
tag("One physical block")
{
    /**
     * This contains the file block number that is currently in this physical
     * block of NVM. This will usually change every time this physical block
     * is reused for a new write (but not always). This is atomically changed
     * when switching the block to be current instead of free. It is needed
     * for verification when reading a block. It provides the potential to
     * scan physical blocks finding the file block contained in each. Note
     * that there may be multiple physical blocks with the same blkno. One
     * is current, but there may be multiple free blocks with the same blkno
     * if there are a lot of writes to the file block.
     */
    volatile ub8 blkno;

    /**
     * This counter is used to coordinate readers and witers so that readers
     * never see a fractured block. It is possible for a reader to get the
     * pointer to a block just as a writer is switching it into the free
     * array. The free block might get reused for a different write before
     * the reader completes the copy out of the block. The writer sets this
     * counter to the negative of its value before changing any other block
     * data. When the writer completes the copy the counter is set to one
     * more than its previous positive value. A reader will not start a
     * copy if the counter is negative, and will redo the read if it is not
     * the same value after the copy. Note that this counter is associated
     * with this physical block, not the block within the file.
     */
    volatile sb8 reused;

    /**
     * The file data stored in this block.
     */
    ub1      data[0];
};

/**
 * This is an array of pointers to physical blocks that are not associated
 * with any file blocks. There is one of these in the root heap.
 */
persistent struct dmf_free
USID("d649  a3a9    e13f    86ed    9af8    29af    5cd2    d75a")
tag("DMF free block array")
{
    /*
     * The array of pointers
     */
    dmf_block ^ptrs[0];
};

/**
 * This is the root struct containing file metadata. Spare space is left in
 * the root in case additional fields are needed later. It has a USID since
 * the root struct must have a USID to validate at attach.
 */
persistent struct dmf_root
USID("f70d  b204    7972    9af4    d7d5    8f50    37b0    cfb7")
tag("DMF Root struct")
size(256)
{
    /**
     * Block size in bytes. All I/O is in terms of this block size. This is
     * set at creation and cannot be changed.
     */
    size_t blksz;

    /**
     * Size fo the free block array. This is the same as the maximum number
     * of simultaneous writes. Each write needs to reserve a free block for
     * an out of place write.
     */
    ub2 freecnt;

    /**
     * File size in blocks. Blocks 0 through filesz-1 are available for I/O.
     * This times the size of a pointer is the size of the pointer extent.
     * This can be changed by resizing the file.
     */
    ub8 filesz;

    /**
     * Maximum file size in blocks. The file size cannot get bigger than this
     * because that is all the virtual space available for adding NVM. The
     * virtual address space was laid out based on this size.
     */
    ub8 maxsz;

    /**
     * This is the total number of NVM blocks allocated. It is usually equal
     * to filesz+freecnt but it may be greater during a grow or shrink.
     * This times the size of a block is the size of the block extent.
     */
    ub8 allocated;

    /**
     * This is a pointer to the block extent. It contains all of the physical
     * blocks including the free blocks. They are not in any particular order.
     */
    dmf_block ^blocks;

    /**
     * This is a pointer to the pointer extent. It is an array of pointers to
     * physical blocks indexed by the file block number. This is the map from
     * file block numbers to physical blocks.
     */
    dmf_block ^^ptrs;

    /**
     * This is a pointer to the array of pointers to free physical blocks.
     * Writing a file block picks one of these to do an out of place write.
     */
    dmf_free ^free_array;
};

/**
 * This describes one open and mapped file. Some values that are in the root
 * struct are replicated here for performance. Opening or creating a Direct
 * Mapped File allocates an instance of this struct and returns a pointer to
 * it as an opaque handle to the file.
 */
struct dmf_file
{
    /**
     * Name of the file for error reporting. This is a copy of the name
     * passed either to dmf_open or dmf_create.
     */
    char *name;

    /**
     * Region descriptor returned by nvm_attach or nvm_create
     */
    nvm_desc desc;

    /**
     * This is initially constructed from the root struct. It is used for
     * validating block numbers rather than checking the root struct. This
     * is so that shrink can eliminate
     */
    volatile ub8 filesz;

    /**
     * Usually this points to just past the last physical block. However when
     * shrinking a file, it points to the first physical block that will be
     * deleted as a result of the shrink. When the shrink completes it will
     * again point just past the last physical block.
     */
    volatile dmf_block ^endblk;

    /**
     * This holds the count of active  reads validated by the current file
     * size. It must be zero to release NVM at the end of shrinking.
     */
    volatile ub8 read_cnt;

    /**
     * This is a lock for coordinating access to the file size by read requests
     * and for setting a new smaller size. When it is negative the size is
     * being changed to a smaller value. When the change is done and there are
     * no more reads that were validated against the old size, the count is
     * set to the next positive value. Note that this will never wrap since
     * it would take 256 years to overflow if incremented once a nanosecond.
     */
    volatile sb8 shrink_cnt;

    /**
     * Maximum file size in blocks. The file size cannot get bigger than this
     * because that is all the virtual space available for adding NVM. The
     * virtual address space was laid out based on this size. Copied from root
     * struct.
     */
    ub8 maxsz;

    /**
     * The block size in bytes for this file. Copied from root struct.
     */
    size_t blksz;

    /**
     * Number of blocks in free block array. Set at creation. Copied from
     * root struct.
     */
    ub2 freecnt;

    /*
     * Pointer to the root struct for this region.
     */
    dmf_root ^root;

    /*
     * Pointer to extent of physical blocks. Copied from root struct.
     */
    dmf_block ^blocks;

    /*
     * Pointer to extent of pointers to file blocks. Copied from root struct.
     */
    dmf_block ^^ptrs;

    /**
     * This is a pointer to the array of pointers to free physical blocks.
     * Writing a file block picks one of these to do an out of place write.
     */
    dmf_block ^^free;

    /**
     * This is an array of mutexes that are used to prevent two threads
     * from simultaneously writing the same block. The address of the pointer
     * to the physical block is hashed to pick the mutex for a block. Without
     * this locking, two simultaneous writes to the same block could corrupt
     * the file metadata.
     *
     * You might think this needs to be NVM mutexes. This is not necessary
     * since we do not support calls to dmf_write or dmf_resize from a
     * recovery callback. Process death while holding one of these mutexes
     * will only have at most one transaction touching any specific block.
     * There will be no attempts during recovery to lock one of theses
     * mutexes. Indeed they will not even exist at that time.
     */
    pthread_mutex_t *write_mutexes;

    /**
     * This is the size of the write mutex array.
     */
    int write_mutex_cnt;

    /*
     * Mutex to protect free block allocation for write or relocate. Also
     * used to halt writes while checking consistency.
     */
    pthread_mutex_t free_mutex;

    /*
     * Index in free array of next freelist block to use. This is used to
     * create a tendency for free blocks to be a LIFO queue.
     */
    ub2 nextfree;

    /*
     * Mutex to ensure only one resize at a time is done to a file.
     */
    pthread_mutex_t size_mutex;

    /*
     * This is true when any of the free physical blocks are beyond endblk.
     * Shrinking will not look for free blocks to remove from the free array
     * when this is false.
     */
    ub1 pastend;

    /*
     * This is an array of flags per free physical block. It is parallel to
     * the free array in the root struct. If true, the corresponding free block
     * is being used to do an out of place write to some file block.
     */
    ub1 *inuse;
};

/*
 * The pointer and block extents are laid out to accommodate the maximum file
 * size. They are rounded up to this size and alignment to allow for large
 * pages to be used by the OS.
 */
#define DMF_PAGE_SIZE (4*1024) // extents are a multiple of this

void dmf_usid_init();
/**
 * This is called when the shared library is loaded before main is
 * called. It does the one time initializations needed by the library.
 */
void dmf_init(void)
{
    /* Register the USID's defined for Direct Mapped Files */
    dmf_usid_init();
}

/**
 * Get information about the file shape and return it in a dmf_stat.
 *
 * @param dmf[in]
 * A handle to an open Direct Mapped File returned by dmf_create or dmf_open.
 *
 * @param dstat[out]
 * point to struct to fill with results.
 *
 * @return
 * Zero is returned on success. If there is an error then errno is set and
 * -1 is returned.
 */
int dmf_query(dmf_file *dmf, dmf_stat *dstat)
{
    /* Save file related information */
    dstat->name = dmf->name;
    dstat->blksz = dmf->blksz;
    dstat->filesz = dmf->filesz;
    dstat->maxsz = dmf->maxsz;
    dstat->writes = dmf->freecnt;

    /* Save region related information. */
    nvm_region_stat rstat;
    if (nvm_query_region(dmf->desc, &rstat))
            return -1;
    dstat->psize = rstat.psize;
    dstat->vsize = rstat.vsize;

    return 0;
}

/**
 * Check the self consistency of the file. This will block any new writes or
 * resizing while it is checking. Inconsistencies are reported on stderr
 * via error(3) in libc.
 *
 * Note that the values in the dmf handle which are copied from the root
 * struct are not used. The values in the root struct are used instead so
 * this is really checking the persistent data consistency.
 *
 * @param dmf[in]
 * A handle to an open Direct Mapped File returned by dmf_create or dmf_open.
 *
 * @param maxerrs[in]
 * Maximum number of errors to report on stderr before returning.
 *
 * @return
 * The return value is the number of problems found. Thus a return of 0
 * indicates a good file.
 */
ub8 dmf_check(dmf_file *dmf, int maxerrs)
{
    int errcnt = 0; // number of errors reported

    /*
     * Lock the resize mutex to ensure no resize is in progress. This must
     * be done before stopping writes because resize uses the free array
     * mutex.
     */
    if ((errno = pthread_mutex_lock(&dmf->size_mutex)))
        error(1, errno, ": %s: Error locking resize mutex", dmf->name);

    /*
     * Now that resizes are done verify the data in dmf is consistent with
     * data in the root struct.
     */
    dmf_root ^root = dmf->root;  // point to root struct
    if (dmf->filesz != root=>filesz)
        error(0, 0, "%d: %s: dmf->filesz:%llu != root=>filesz:%llu",
                ++errcnt, dmf->name, dmf->filesz, root=>filesz);
    if (dmf->maxsz != root=>maxsz)
        error(0, 0, "%d: %s: dmf->maxsz:%lld != root=>maxsz:%lld",
                ++errcnt, dmf->name, dmf->maxsz, root=>maxsz);
    if (dmf->blksz != root=>blksz)
        error(0, 0, "%d: %s: dmf->blksz:%ld != root=>blksz:%ld",
                ++errcnt, dmf->name, dmf->blksz, root=>blksz);
    if (dmf->freecnt != root=>freecnt)
        error(0, 0, "%d: %s: dmf->freecnt:%d != root=>freecnt:%d",
                ++errcnt, dmf->name, dmf->freecnt, root=>freecnt);
    if (dmf->blocks != root=>blocks)
        error(0, 0, "%d: %s: dmf->blocks:%p != root=>blocks:%p",
                ++errcnt, dmf->name, dmf->blocks, root=>blocks);
    if (dmf->ptrs != root=>ptrs)
        error(0, 0, "%d: %s: dmf->ptrs:%p != root=>ptrs:%p",
                ++errcnt, dmf->name, dmf->ptrs, root=>ptrs);
    if (dmf->free != root=>free_array=>ptrs)
        error(0, 0, "%d: %s: dmf->free:%p != root=>free:%p",
                ++errcnt, dmf->name, dmf->free, root=>free_array=>ptrs);

    /*
     * Calculate some values for efficiency
     */
    size_t blksz = root=>blksz;
    dmf_block ^eoa =            // end of allocated physical blocks
            (dmf_block ^)((ub1^)root=>blocks +
            (root=>allocated)*(sizeof(dmf_block) + blksz));
    dmf_block ^blocks = root=>blocks; // block extent beginning
    dmf_block ^^pp = root=>ptrs; // pointer to pointer extent

    /*
     * Check the end of the block extent
     */
    if (dmf->endblk  != eoa)
        error(0, 0,
                "%d: %s: dmf->endblk:%p root=>blocks:%p root=>allocated:%llu",
                ++errcnt, dmf->name, dmf->endblk, root=>blocks,
                root=>allocated);

    /*
     * Lock the free block allocation mutex to stop new writes and wait for all
     * active writes to complete. This will ensure there are no active writes
     * while checking the data structures. Writes are complete when all inuse
     * flags are zero.
     */
    if ((errno = pthread_mutex_lock(&dmf->free_mutex)))
        error(1, errno, ": %s: Error locking free block mutex", dmf->name);
    int i;
    for (i = 0; i < root=>freecnt; i++)
    {
        /* If this slot is in use then wait until it is freed. Do exponential
         * backoff for a total max of 4 seconds. */
        int busy = 0;
        while (dmf->inuse[i])
        {
            if (busy > 21)
                error(1, 0, ": %s: busy timeout waiting for write completion",
                        dmf->name);
            struct timespec ts;
            ub8 microseconds = (1<<busy++);
            ts.tv_sec = microseconds / 1000000;
            ts.tv_nsec = (microseconds % 1000000)*1000;
            nanosleep(&ts, NULL);
        }
    }

    /*
     * Loop through the extent of pointers to physical blocks ensuring they are
     * all consistent. Note that if any bad block numbers are found, then it
     * is possible that there are two pointers to the same physical block.
     */
    ub8 blkno;                   // block number being examined
    for (blkno = 0; blkno < root=>filesz && errcnt < maxerrs; blkno++, pp++)
    {
        /*
         * Get the physical block address and number for this file block
         */
        dmf_block ^blk = ^pp;
        sb8 pbno = ((ub1^)blk - (ub1^)blocks) / (sizeof(dmf_block) + blksz);

        /*
         * Error if not a valid pointer.
         */
        if (!blk)
        {
            error(0, 0, "%d: %s: file block %lld has a null physical block"
                    " pointer", ++errcnt, dmf->name, blkno);
            continue;
        }
        if (((ub1^)blk - (ub1^)blocks) % (sizeof(dmf_block) + blksz))
        {
            error(0, 0, "%d: %s: file block %lld has misaligned physical "
                    "block pointer %p", ++errcnt, dmf->name, blkno, blk);
            continue;
        }
        if (blk < blocks || blk >= eoa ||
                ((ub1^)blk - (ub1^)blocks) % (sizeof(dmf_block) + blksz))
        {
            error(0, 0, "%d: %s: file block %lld has out of range physical "
                    "block pointer %p", ++errcnt, dmf->name, blkno, blk);
            continue;
        }

        /*
         * Check for a valid USID before we look in the block header. This
         * ensures nvm_verify will not assert.
         */
        nvm_usid ^usid = (nvm_usid ^)blk;
        if (!nvm_usid_eq(^usid, shapeof(dmf_block)->usid))
        {
            error(0, 0, "%d: %s: file block %lld physical block %lld "
                    "has bad USID %lx:%lx", ++errcnt, dmf->name, blkno, pbno,
                    usid=>d1, usid=>d2);
            continue;
        }

        /*
         * Check for the correct file block number in the physical block
         */
        if (blk=>blkno != blkno)
            error(0, 0, "%d: %s: file block %lld physical block %lld "
                    "contains wrong blkno %lld", ++errcnt, dmf->name,
                    blkno, pbno, blk=>blkno);

        /*
         * Check for reasonable reused count in the physical block
         */
        if (blk=>reused <= 0)
            error(0, 0, "%d: %s: file block %lld physical block %lld "
                    "contains bad reused count %lld", ++errcnt, dmf->name,
                    blkno, pbno, blk=>reused);
    }

    /*
     * Now that the current blocks have been verified, verify the free
     * blocks.
     */
    pp = root=>free_array=>ptrs;
    for (i= 0; i < root=>freecnt && errcnt < maxerrs; i++, pp++)
    {
        /*
         * Get the physical block address and number for this free block
         */
        dmf_block ^blk = ^pp;
        sb8 pbno = ((ub1^)blk - (ub1^)blocks) / (sizeof(dmf_block) + blksz);

        /*
         * Error if not a valid pointer.
         */
        if (!blk)
        {
            error(0, 0, "%d: %s: free block %d has a null physical block"
                    " pointer", ++errcnt, dmf->name, i);
            continue;
        }

        if (((ub1^)blk - (ub1^)blocks) % (sizeof(dmf_block) + blksz))
        {
            error(0, 0, "%d: %s: free block %d has misaligned physical "
                    "block pointer %p", ++errcnt, dmf->name, i, blk);
            continue;
        }
        if (blk < blocks || blk >= eoa ||
                ((ub1^)blk - (ub1^)blocks) % (sizeof(dmf_block) + blksz))
        {
            error(0, 0, "%d: %s: free block %d has out of range physical "
                    "block pointer %p", ++errcnt, dmf->name, i, blk);
            continue;
        }

        /*
         * Check for a valid USID before we look in the block header. This
         * ensures nvm_verify will not assert.
         */
        nvm_usid ^usid = (nvm_usid ^)blk;
        if (!nvm_usid_eq(^usid, shapeof(dmf_block)->usid))
        {
            error(0, 0, "%d: %s: free block %d physical block %lld "
                    "has bad USID %lx:%lx", ++errcnt, dmf->name, i, pbno,
                    usid=>d1, usid=>d2);
            continue;
        }

        /*
         * Check for a reasonable file block number in the physical block.
         * Free blocks can have block numbers that are no longer within the
         * file size, but must be within max possible size. However we allow
         * block maxsz since free blocks are set to block number maxsz while
         * they are being updated.
         */
        blkno = blk=>blkno;
        if (blkno > dmf->maxsz)
        {
            error(0, 0, "%d: %s: free block %d physical block %lld "
                    "contains invalid blkno %lld", ++errcnt, dmf->name,
                    i, pbno, blkno);
            continue;
        }

        /*
         * Ensure this block is not still the current block as well. This
         * check means that if there are no errors, then every physical
         * block has exactly one pointer to it either as a current block or
         * as a free block. Note if the free block has a block number beyond
         * the current file size we cannot do this check.
         */
        if (blkno < root=>filesz && root=>ptrs[blkno] == blk)
            error(0, 0, "%d: %s: free block %d physical block %lld "
                    "is also current file block %lld", ++errcnt, dmf->name,
                    i, pbno, blkno);
    }

    /*
     * release mutexes so file can be used again.
     */
    if (pthread_mutex_unlock(&dmf->size_mutex))
        error(1, errno, ": %s: Error unlocking resize mutex", dmf->name);
    if (pthread_mutex_unlock(&dmf->free_mutex))
        error(1, errno, ": %s: Error unlocking free block mutex", dmf->name);

    /*
     * Return the number of erors reported
     */
    return errcnt;
}

/**
 * Set a new file size for the file. Future file size checks will use this
 * size, until another size is set. Note this does not affect the the
 * persistent file size saved in NVM. However all reads and writes honor
 * this size.
 *
 * This also sets a new end block so that future free block reservations will
 * not allocate blocks that will be deleted by shrink, and reads from
 * physical blocks beyond the new end will prevent shrink from deleting the
 * NVM until they complete.
 *
 * The caller must be holding the size_mutex to ensure only one thread at
 * a time can change the size.
 *
 * @param dmf[in/out]
 * A handle to an open Direct Mapped File returned by dmf_create or dmf_open.
 */
void dmf_new_size(dmf_file *dmf, ub8 filesz)
{
    /* Store the new end block before changing the size. This ensures that
     * when shrinking the new end block will be visible if the new size is
     * visible. The endblk is used to avoid writing to physical blocks that
     * are destined to be deleted by a shrink that is in progress. It is
     * also used to prevent shrink from deleting NVM during a memcpy to a
     * reader's buffer. */
    dmf->endblk = (dmf_block ^)((ub1^)dmf->blocks +
            (filesz + dmf->freecnt)*(sizeof(dmf_block) + dmf->blksz));

    /* Store the new file size in file blocks */
    dmf->filesz = filesz;
}

/**
 * When a block read is done, decrement the read count. This may take the
 * read count to zero allowing a shrink to release NVM physical blocks back to
 * the file system.
 *
 * @param dmf[in/out]
 * A handle to an open Direct Mapped File returned by dmf_create or dmf_open.
 */
void dmf_read_done(dmf_file *dmf)
{
    /* decrement read count since we are done, but the read count should never
     * go negative. If read_cnt goes to zero then shrink can finish. */
    while(TRUE)
    {
        ub8 old = dmf->read_cnt;
        if (old == 0)
        {
            error(1, EINVAL, "decrementing zero read count: "
                    "shrink_cnt:%lld size:%llu", dmf->shrink_cnt, dmf->filesz);
        }

        if (__sync_val_compare_and_swap(&dmf->read_cnt, old, old - 1) == old)
            break;
    }
}

/*
 * Validate the block is within the curent file size and bump the count of
 * active reads based on this file size. This interlocks with dmf_wait_reads
 * to not finish shrink after a block has been validated until the read
 * is done.
 *
 * A null pointer is returned if the block is beyond the file size. If the
 * block is within the file, a pointer to the current physical block is
 * returned.
 *
 * @param dmf[in]
 * A handle to an open Direct Mapped File returned by dmf_create or dmf_open.
 */
dmf_block ^dmf_read_check(dmf_file *dmf, ub8 blkno)
{
restart:;
    /* If a shrink is blocking new reads until all reads are complete,
     * then spin until until reads allowed. */
    sb8 scnt;
    while ((scnt = dmf->shrink_cnt) < 0)
        sched_yield();

    /* Increment the use count so resize will not free NVM until we are
     * done. We spin until a compare and swap succeeds in bumping the count. */
    while(TRUE)
    {
        ub8 old = dmf->read_cnt;
        if (__sync_val_compare_and_swap(&dmf->read_cnt, old, old + 1) == old)
            break;
    }

    /* Validate the block is in the file. We must do this before fetching
     * the pointer out of the pointer extent since the pointer blocks could
     * be no longer in our address space if the block number is too big. */
    if (blkno >= dmf->filesz)
    {
        /* undo increment of read_cnt */
        dmf_read_done(dmf);
        return (dmf_block^)0;
    }

    /* It is possible that a new file size was set after we validated
     * shrink_cnt was positive and before we successfully validated the
     * read. If this happened we could allow an access that should
     * not be allowed. Thus we need to undo the increment we did, and start
     * over again. */
    if (scnt != dmf->shrink_cnt)
    {
        /* undo increment of read_cnt */
        dmf_read_done(dmf);
        goto restart;
    }

    /* Return success. */
    return dmf->ptrs[blkno];
}

/**
 * Wait until all read requests from physical blocks beyond endblk have
 * completed. This is only used for shrinking, not in the I/O path.
 * This is called after shrink has shuffled all file blocks to be deleted
 * to be using physical blocks that will be deleted. It actually waits
 * for all reads since it is hard to just wait for those still reading
 * from physical blocks that will be deleted.
 *
 * Shrink will not release space back to the OS until all accesses have
 * completed. However grow can advance the file size without waiting for
 * it to be recognized before adding space.
 *
 * @param dmf[in/out]
 * A handle to an open Direct Mapped File returned by dmf_create or dmf_open.
 */
void dmf_wait_reads(dmf_file *dmf)
{
    /* Set the shrink count negative so that no new reads can start */
    dmf->shrink_cnt = -dmf->shrink_cnt;

    /* Spin until the active I/O count goes to zero */
    while (dmf->read_cnt)
        sched_yield();

    /* advance the shrink count to next positive value so I/O's can resume */
    dmf->shrink_cnt = 1 - dmf->shrink_cnt;
}

/**
 * Create a Direct Mapped File and map it into the process. The file must not
 * exist already. The file is open on return.
 *
 * @param[in] name
 * This is the name of the new file to create. If creation is successful a
 * copy of this is made so the caller can release the space.
 *
 * @param[in] blksz
 * Block size in bytes. All I/O is in multiples of the block size. This cannot
 * be changed.
 *
 * @param[in] filesz
 * Initial file size in blocks. Blocks 0 through filesz-1 are available for I/O.
 * This can be changed by resizing the file. This determines how much NVM the
 * file consumes.
 *
 * @param[in] maxsz
 * The maximum size in blocks that file can ever become. It cannot be
 * changed. This is used to to determine how much virtual address space
 * the file consumes when mapped into a process. A process may run out of
 * virtual address space if this is too large, but it does not affect the
 * amount of NVM consumed.
 *
 * @param[in] addr
 * This is the virtual address where the new file should be mapped. Usually
 * this is zero to let the OS pick an address.
 *
 * param[in] writes
 * This is the maximum number of simultaneous writes that the file can
 * support. If more writes are in flight at the same time, then some will
 * block until others complete. If the application might read a block while
 * it is being written then this should be a bit larger than the expected
 * number of simultaneous writes so that there will be fewer reads that need
 * to be restarted due to a read/write collision. Note that a write of multiple
 * blocks only counts as a single write since they are all done one at a time
 * by the same thread. There are no limitations on the number of concurrent
 * reads. This many spare physical block of NVM are allocated for doing out of
 * place writes, so it is best to make this pretty small. It must be at least
 * two, and no more than 4 times the number of processors.
 *
 * @param[in] mode
 * This is the mode to pass to the system call that creates the NVM file.
 *
 * @return
 * A successful creation returns a handle to use with subsequent calls. If
 * creation fails then errno is set and a null handle is returned
 */
dmf_file *dmf_create(
        const char *name, // file name
        size_t blksz,     // block size in bytes
        ub8 filesz,       // initial file size in blocks
        ub8 maxsz,        // maximum file size
        void *addr,       // map address
        ub2 writes,       // max simultaneous writes
        mode_t mode)      // permissions
{
    /*
     * The following int is used to hold the errno value that caused an error
     * while allocations done here are freed.
     */
    int saverr;

    /*
     * Validate the writes parameter. It is an error if it is less than 2 or
     * greater than 4 times the number of processors. If the number of
     * processors is not available allow up to 1024 writes.
     */
    int numCPU = sysconf( _SC_NPROCESSORS_CONF );
    if (numCPU <= 0)
        numCPU = 256;
    if (writes < 2 || writes > numCPU*4)
    {
        saverr = errno = EINVAL;
        goto err0;
    }

    /*
     * Decide how much address space is needed for the base extent.
     * It needs to hold the NVM Direct metadata, a pointer array for the free
     * blocks, and a mutex array for write exclusion. The mutex array is the
     * square of the max concurrent writes to ensure there are few false
     * collisions.
     */

    /*
     * NVM Direct metadata is a bit over a megabyte presuming 63 transaction
     * slots are sufficient. We add 128K to cover heap metadata.
     */
    size_t metadata = 1024*(1024+128);

    /*
     * The free block pointers are one pointer per concurrent write
     */
    size_t freeptrs = writes * sizeof(dmf_block^);

    /*
     * Calculate the number of write mutexes to hash into.
     */
    ub4 mutexes = writes*writes;

    /*
     * Add these up to be the physical space for the base extent.
     * The size has to be rounded up to a  dmf page boundary.
     * Choose the virtual address space to be twice the physical space.
     * This allows the base extent to double in size someday.
     */
    size_t basesz = metadata + freeptrs + DMF_PAGE_SIZE - 1;
    basesz -= basesz % DMF_PAGE_SIZE;
    size_t vbasesz = 2 * basesz;

    /*
     * Calculate the physical and virtual size of the pointer extent.
     * We leave virtual address space to hold maxsz pointers.
     */
    size_t ptrsz = filesz * sizeof(dmf_block^) + DMF_PAGE_SIZE - 1;
    ptrsz -= ptrsz % DMF_PAGE_SIZE;
    size_t vptrsz = maxsz * sizeof(dmf_block^) + DMF_PAGE_SIZE - 1;
    vptrsz -= vptrsz % DMF_PAGE_SIZE;

    /*
     * Calculate the physical and virtual size of the block extent.
     * We leave virtual address space to hold maxsz blocks.
     */
    size_t bexsz = (filesz + writes) * (sizeof(dmf_block) + blksz) +
            DMF_PAGE_SIZE - 1;
    bexsz -= bexsz % DMF_PAGE_SIZE;
    size_t vbexsz = (maxsz + writes) * (sizeof(dmf_block) + blksz) +
            DMF_PAGE_SIZE - 1;
    vbexsz -= vbexsz % DMF_PAGE_SIZE;

    /*
     * Allocate a dmf_file for managing the Direct Mapped File.
     * It is zeroed on allocation.
     */
    dmf_file *dmf = calloc(1, sizeof(dmf_file));
    if (!dmf)
    {
        saverr = errno;
        goto err0;
    }

    /*
     * Store the file name in dmf
     */
    dmf->name = malloc(strlen(name)+1);
    if (!dmf->name)
    {
        saverr = errno;
        goto err1;
    }
    strcpy(dmf->name, name);

    /*
     * allocate the in use flags for reserving free blocks.
     */
    dmf->inuse = calloc(writes, 1);
    if (!dmf->inuse)
    {
        saverr = errno;
        goto err2;
    }

    /*
     * Allocate the array of write mutexes for preventing simultaneous
     * writes to the same block.
     */
    dmf->write_mutexes =
            (pthread_mutex_t*)calloc(mutexes, sizeof(pthread_mutex_t));
    if (!dmf->write_mutexes)
    {
        saverr = errno;
        goto err3;
    }
    dmf->write_mutex_cnt = mutexes;

    /*
     * Create and map the region file base extent. Let NVM Direct pick the
     * descriptor. The virtual size is the sum of the virtual sizes of all
     * three extents.
     */
    dmf->desc = nvm_create_region(0, name, "Direct Mapped File", addr,
            vbasesz + vptrsz + vbexsz, basesz, mode);

    /*
     * Return failure if nvm_create_region failed. It sets errno if it fails.
     */
    if (!dmf->desc)
    {
        saverr = errno;
        goto err4;
    }

    /*
     * Query the region so we can get the heap for allocating the root struct
     * and other DMF metadata.
     */
    nvm_region_stat rstat;
    if (!nvm_query_region(dmf->desc, &rstat))
    {
        saverr = errno;
        goto err5;
    }
    nvm_heap ^heap = rstat.rootheap; // root heap pointer used for allocation
    ub1 *region = rstat.base; // virtual address where region is attached

    /*
     * In one transaction create and initialize all the extents and NVM
     * metadata. Stores are all non-transactional because the region cannot
     * be mapped into another process unless the root pointer is set. That
     * has to happen after the transaction commits. The root pointer must be
     * set outside of a transaction to ensure the root struct allocation is
     * committed before the region is made mappable.
     */

    /* Begin a transaction in the new region we created. Since the region
     * has not yet been made valid by setting a root pointer, atomicity
     * is not important. However some of the NVM Direct functions we need
     * to call require a transaction. */
    { @ dmf->desc {
        /*
         * Allocate the DMF root struct from the base heap created when
         * the region was created.
         */
        dmf_root ^root = nvm_alloc(heap, shapeof(dmf_root), 1);
        if (!root)
        {
            saverr = errno;
            nvm_abort();
            goto err5;
        }

        /*
         * Store sizes in the root and dmf if replicated there
         */
        dmf->root = root;
        dmf->blksz = root=>blksz ~= blksz;     // block size in bytes
        dmf->filesz = root=>filesz ~= filesz;  // file size in blocks
        dmf->shrink_cnt = 1;                   // start at 1 since -0 == 0
        root=>allocated ~= filesz + writes;    // physical blocks allocated
        dmf->freecnt = root=>freecnt ~= writes;// number of free blocks
        dmf->maxsz = root=>maxsz ~= maxsz;    // maximum file size

        /* Allocate free block pointer array */
        root=>free_array ~= nvm_alloc(heap, shapeof(dmf_free), writes);
        if (!root=>free_array)
        {
            saverr = errno;
            nvm_abort();
            goto err5;
        }

        /* Keep a pointer to the actual free pointers rather than the struct
         * they are a member of.*/
        dmf->free = root=>free_array=>ptrs;

        /* Allocate an extent to hold the array of pointers to physical
         * blocks indexed by file block number. There is enough virtual
         * address space to hold maxsz pointers. Allocate physical
         * space rounded up to next page size. */
        root=>ptrs ~= nvm_add_extent((void^)(region+vbasesz), ptrsz);
        dmf->ptrs = root=>ptrs;
        if (!dmf->ptrs)
        {
            saverr = errno;
            nvm_abort();
            goto err5;
        }

        /* Allocate an extent to hold the physical blocks for the file
         * including the free blocks for out of place writes. We cannot use
         * dmf_block^ pointers until the NVM is formatted as a dmf_block.
         * Compiler inserted verify calls would fail. */
        ub1 ^blocks = nvm_add_extent((void^)(region+vbasesz+vptrsz), bexsz);
        if (!blocks)
        {
            saverr = errno;
            nvm_abort();
            goto err5;
        }

        /* Initialize every physical block and save a pointer to it */
        dmf_block ^^ptr = dmf->ptrs; // pointer 0
        ub1 ^blk = blocks;// block 0
        ub8 cnt = filesz + writes; // physical blocks to initialize
        ub8 bno = 0;
        while(cnt--)
        {
            /* Format the block so that it will verify. We lie about the
             * data size to save time. The reused count will ensure the
             * block reads as zero until it is written. */
            nvm_init_struct(blk, shapeof(dmf_block), 0);

            /* Set pointer to the formatted block */
            ^ptr ~= (dmf_block^)blk;

            /* Initialize the reused count to 1 since zero does not work.
             * A reused count of 1 is used to indicate the block has never
             * been written. */
            ((dmf_block^)blk)=>reused ~= 1;

            /* Initialize the block number. Meaningless for free blocks, but
             * to let dmf_check validate the block number set it to maxsz. */
            ((dmf_block^)blk)=>blkno ~= cnt < writes ? maxsz : bno++;

            /* go to next block */
            blk += sizeof(dmf_block) + blksz;

            /* Go to next pointer. Switch to free array when current block
             * pointer array is full. */
            if (cnt != writes)
                ptr++;
            else
                ptr = dmf->free;
        }

        /* Now that the blocks are initialized save pointers to them */
        root=>blocks ~= (dmf_block^)blocks;
        dmf->blocks = root=>blocks;
        dmf->endblk = (dmf_block^)(blocks +
                root=>allocated * (sizeof(dmf_block) + blksz));

        /* root construction is complete */
    }} // commit root struct

    /* Initialize the array of write mutexes to prevent simultaneous writes
     * to the same block. */
    int m;
    for (m = 0; m<mutexes; m++)
        if ((errno = pthread_mutex_init(dmf->write_mutexes + m, NULL)))
            error(1, errno, ": %s: Error initializing a write mutex",
                    dmf->name);

    /* Initialize data used to reserve a free block slot */
    dmf->nextfree = 0;
    dmf->pastend = FALSE;
    if ((errno = pthread_mutex_init(&dmf->free_mutex, NULL)))
        error(1, errno, ": %s: Error initializing free block mutex",
                dmf->name);

    /* Initialize the DRAM mutex to ensure only one resize at a time is done. */
    if ((errno = pthread_mutex_init(&dmf->size_mutex, NULL)))
        error(1, errno, ": %s: Error initializing resize mutex", dmf->name);

    /*
     * File is initialized. All that is needed is to set the root pointer.
     * This makes the region file usable for access through DMF. If we die
     * before this is set then the file will not be usable.
     */
    nvm_set_root_object(dmf->desc, dmf->root);

    /*
     * Successfully created the region file so return its handle
     */
    return dmf;

    /*
     * The following code cleans up allocations that have been done and
     * returns an error. There are goto statements that jump here to
     * clean up allocations made at the point where the error happens.
     * Note that aborting the transaction has to be done in the transaction
     * code block. The value of errno that caused the error must be preserved
     * in saverr before jumping here.
     */
    err5: nvm_detach_region(dmf->desc);
          nvm_destroy_region(dmf->name);
    err4: free(dmf->write_mutexes);
    err3: free(dmf->inuse);
    err2: free(dmf->name);
    err1: free(dmf);
    err0: errno = saverr;
    return NULL;
}

/**
 * Open a Direct Mapped File and map it into this process. The file must have
 * been previously created by dmf_create.
 *
 * @param[in] name
 * This is the name of the existing file to open. If open is successful a
 * copy of this is made so the caller can release the space.
 *
 * @param[in] addr
 * This is the virtual address where the file should be mapped. Usually
 * this is zero to let the OS pick an address.
 *
 * @return
 * A successful creation returns a handle to use with subsequent calls. If
 * open fails then errno is set and a null handle is returned.
 */
dmf_file *dmf_open(const char *name, void *addr)
{
    /*
     * The following int is used to hold the errno value that caused an error
     * while allocations done here are freed.
     */
    int saverr;

    /* attach the region to this process */
    int desc = nvm_attach_region(0, name, addr);
    if (!desc)
    {
        saverr = errno;
        goto err0;
    }

    /*
     * get the stats for the region so we can get the root pointer
     */
    nvm_region_stat rstat;
    if (!nvm_query_region(desc, &rstat))
    {
        saverr = errno;
        goto err1;
    }
    dmf_root ^root = (dmf_root^)rstat.rootobject;

    /*
     * Allocate a dmf_file for managing the Direct Mapped File.
     * It is zeroed on allocation.
     */
    dmf_file *dmf = calloc(1, sizeof(dmf_file));
    if (!dmf)
    {
        saverr = errno;
        goto err1;
    }

    /*
     * Store the file name in dmf
     */
    dmf->name = malloc(strlen(name)+1);
    if (!dmf->name)
    {
        saverr = errno;
        goto err2;
    }
    strcpy(dmf->name, name);


    /* copy heavily used data out of root into dmf */
    dmf->root =  root;
    dmf->desc = desc;
    dmf->filesz = root=>filesz;
    dmf->shrink_cnt = 1;                   // start at 1 since -0 == 0
    dmf->maxsz = root=>maxsz;
    dmf->blksz = root=>blksz;
    dmf->freecnt = root=>freecnt;
    dmf->blocks = root=>blocks;
    dmf->ptrs = root=>ptrs;
    dmf->free = root=>free_array=>ptrs;

    /* Set endblk to number of allocated physical blocks. */
    dmf->endblk = (dmf_block^)((ub1^)dmf->blocks +
            root=>allocated * (sizeof(dmf_block) + dmf->blksz));

    /*
     * allocate the in use flags for reserving free blocks.
     */
    dmf->inuse = calloc(dmf->freecnt, 1);
    if (!dmf->inuse)
    {
        saverr = errno;
        goto err3;
    }

    /*
     * Allocate the array of write mutexes for preventing simultaneous
     * writes to the same block.
     */
    ub4 mutexes = dmf->freecnt * dmf->freecnt;
    dmf->write_mutexes =
            (pthread_mutex_t*)calloc(mutexes, sizeof(pthread_mutex_t));
    if (!dmf->write_mutexes)
    {
        saverr = errno;
        goto err4;
    }
    dmf->write_mutex_cnt = mutexes;

    /* Initialize the array of write mutexes to prevent simultaneous writes
     * to the same block. */
    int m;
    for (m = 0; m<mutexes; m++)
        if ((errno = pthread_mutex_init(dmf->write_mutexes + m, NULL)))
            error(1, errno, ": %s: Error initializing a write mutex",
                    dmf->name);

    /* Initialize the data for reserving a free block for a write */
    dmf->nextfree = 0;
    dmf->pastend = FALSE;
    if ((errno = pthread_mutex_init(&dmf->free_mutex, NULL)))
        error(1, errno, ": %s: Error initializing free block mutex",
                dmf->name);

    /* Initialize the DRAM mutex to ensure only one resize at a time is done. */
    if ((errno = pthread_mutex_init(&dmf->size_mutex, NULL)))
        error(1, errno, ": %s: Error initializing resize mutex", dmf->name);

    /*
     * The Direct Mapped File is open and ready for use.
     */
    return dmf;

    /*
     * The following code cleans up allocations that have been done and
     * returns an error. There are goto statements that jump here to
     * clean up allocations made at the point where the error happens.
     * The value of errno that caused the error must be preserved
     * in saverr before jumping here.
     */
    err4: free(dmf->inuse);
    err3: free(dmf->name);
    err2: free(dmf);
    err1: nvm_detach_region(desc);
    err0: errno = saverr;
    return NULL;
}

/**
 * Close an open file unmapping it from the process. There must not be any I/O
 * in progress when this is called. After this is called the handle cannot be
 * used. The DRAM data structures pointed to by the handle are no longer
 * allocated.
 *
 * Note the warnings about ignoring the return value from close do not apply
 * to Direct Mapped Files since they are never cached and the write system
 * call is never used, once the region is created.
 *
 * If there are errors detaching the region, then the process is killed since
 * this indicates corruption in the DMF data.
 *
 * @param dmf[in/out]
 * A handle to an open Direct Mapped File returned by dmf_create or dmf_open.
 */
void dmf_close(dmf_file *dmf)
{
    /*
     * The only errors that nvm_detach_region can return indicate dmf
     * corruption. We call error to report the problem and kill the process.
     */
    if (!nvm_detach_region(dmf->desc))
        error(1, errno, ": %s: Error detaching the region file.", dmf->name);

    /* Release all resources held by dmf. */
    int m;
    for (m = 0; m < dmf->write_mutex_cnt; m++)
        if ((errno = pthread_mutex_destroy(dmf->write_mutexes + m)))
            error(1, errno, ": %s: Error destroying a write mutex",
                    dmf->name);
    if ((errno = pthread_mutex_destroy(&dmf->free_mutex)))
        error(1, errno, ": %s: Error destroying free block mutex",
                dmf->name);
    if ((errno = pthread_mutex_destroy(&dmf->size_mutex)))
        error(1, errno, ": %s: Error destroying resize mutex", dmf->name);
    free(dmf->inuse);
    free(dmf->write_mutexes);
    free(dmf->name);
    free(dmf);
}

/**
 * Unlink a Direct Mapped File. The file must not be mapped into any process.
 * This is really nothing but a call to unlink with a different return value
 * to indicate failure. It is only here for completeness.
 *
 * @param[in] name
 * This is the name of the file to unlink.
 *
 * @return
 * Zero is returned on success. If there is an error then errno is set and
 * -1 is returned.
 */
int dmf_delete(const char *name)
{
    if (!nvm_destroy_region(name) && errno != ENOENT)
        return 0;
    return -1;
}

/**
 * Read a sequence of blocks from a Direct Mapped File into a DRAM buffer.
 * Blocks beyond the end of file are not returned. The count of blocks
 * actually read is returned. This could be zero if the first block to  read
 * is past the EOF. A partial read indicates the read extends beyond the EOF.
 * Reading a block that was never written returns a block of all zero. A read
 * that is concurrent with a write to the same block might return either the
 * previous or new version of the block, but never a mixture of them.
 * Resize an open Direct Mapped File. The new size may be larger or smaller
 * than the current size. The file may be in active use during the resize.
 * Two simultaneous resizes of the same file will be serialized so that one
 * is done before the other begins. The final size could be either of the
 * two sizes.
 *
 * It is expected that this will not be called in an NVM transaction. However
 * it would work fine.
 *
 * This cannot be called in an on-abort, on-commit, or on-unlock callback.
 * These callbacks are called to make the make the NVM region file consistent
 * before it is opened. Thus there is no dmf_file struct to pass to the
 * dmf entry points. The dmf_file handle available when the on-abort,
 * on-commit, or on-unlock operation is created will not exist when the
 * operation is called in recovery. Thus it cannot be saved in the callback
 * context. It would be possible to create a version of this library that does
 * support for rollback of write and doing read/write in a callback, but
 * it would require a different API and incur additional overhead.
 *
 * @param dmf[in]
 * A handle to an open Direct Mapped File returned by dmf_create or dmf_open.
 *
 * @param blkno[in]
 * The number of the first block to read. The first block in a file is block
 * zero. The last block is file size minus one. No data is returned for blocks
 * beyond the last block.
 *
 * @param buffer[in]
 * The address of a buffer in DRAM which is large enough to hold count blocks.
 * Reading into NVM does not work because the data is not flushed out of the
 * processor cache.
 *
 * @param count[in]
 * The number of blocks to read. If blkno+count is greater than the file size
 * then only the blocks up to the file size are returned.
 *
 * @return
 * If successful the number of blocks read is returned. This may be less than
 * the number requested if reading past the file size.
 * If there is an error then -1 is returned and errno is set.
 */
ub4 dmf_read(dmf_file *dmf, ub8 blkno, void *buffer, ub4 count)
{
    /* Return if nothing to do */
    if (count == 0)
        return 0;

    /* Loop copying one block from NVM every time around. Initialize pointers
     * to point at first block. */
    ub4 bno = blkno;
    ub4 cnt = count;
    ub1 *buf = buffer;
    while (TRUE)
    {
        /*
         *  Copy one block to the buffer. If it is reused while copying, then
         * copy the new version. Note that retry happens when the block is
         * written after the block pointer was fetched. This puts the physical
         * block that was being read into the free block array. If the read
         * completes before the free block is chosen for another write, then
         * the data is good. It is the version that was current when the read
         * was started and thus is a valid version to return.
         */
        int retries = 0;
        int busy = 0;
        while(TRUE) {

            /* Verify the file block is within the file, and get the physical
             * block with the current contents. Note that if there is a shrink
             * going on the physical block may be beyond endblk, but shrink
             * will not delete the physical block until we are done reading.
             * If the file block is outside the file size a null physical
             * block pointer is returned. */
            dmf_block ^block = dmf_read_check(dmf, bno);
            if (!block)
               return count-cnt; // not within the file

            /* If another thread is storing into this physical block then the
             * reused count will be a negative value. This could mean that
             * between the above fetch of the block address and this fetch of
             * the reuse count, another thread switched this physical block
             * with one in the free array with new contents for the block.
             * Then some other block write chose this block to do another out
             * of place write.
             *
             * A more likely case is that another thread just stored a new
             * block pointer and has not yet set the new positive value in
             * reused. It is possible that commit processing is currently going
             * on and the positive value will be set soon. */
            sb8 reused = block=>reused;
            if (reused < 0)
            {
                /* Drop the read lock since we will need to revalidate the
                 * block number. */
                dmf_read_done(dmf);

                /* if this is the first check then just do it again */
                if (!busy)
                {
                    busy++;
                    continue;
                }

                /* We do not want to spin here since that could prevent the
                 * writer from aborting or committing the transaction. So we
                 * sleep with exponential backoff starting at one microsecond
                 * and doubling on each sleep. Something must be terribly
                 * wrong if we get stuck here for 2 seconds - 20 iterations.
                 * We will treat this like corruption and return EIO.
                 */
                if (busy > 20)
                {
                    /* Inability to read implies corruption */
                    errno = EIO;
                    return -1;
                }
                struct timespec ts;
                ub8 microseconds = (1<<busy++);
                ts.tv_sec = microseconds / 1000000;
                ts.tv_nsec = (microseconds % 1000000)*1000;
                nanosleep(&ts, NULL);
                continue; // try again
            }

            /* If the block has never been written then just return zeroes.
             * A reused count of 1 means the physical block was added to the
             * file and the file block it was assigned to has not been written.
             * If it had been written the physical block would have become
             * free, and its reused count incremented to 2 on its first reuse
             * giving it real contents. */
            if (reused == 1)
            {
                /* Drop the size lock. */
                dmf_read_done(dmf);

                /* clear the buffer */
                memset(buf, 0, dmf->blksz);
                break;
            }

            /* if not busy start over on busy timeout */
            busy = 0;

            /* We have a version of the block that was current at or after the
             * read call was made. It might be free, but the contents have not
             * changed since it was current. Copy the block contents to the
             * callers buffer */
            sb8 shrinks_reuseok = dmf->shrink_cnt;
            memcpy(buf, block=>data, dmf->blksz);

            /* It is possible that after we fetched the pointer to this
             * physical block it became reused for another file block and
             * completed the write. Thus just not being busy is not good
             * enough to ensure we got the right block. We need to verify
             * the block number is still the same. This will be sufficient
             * because copying data into a physical block will first set the
             * file block number in the physical block to the maximum file
             * block number before negating the reused count.
             */
            if (block=>blkno != bno)
                goto retry;

            /* If the reused count has not changed then no one messed with
             * the data while we were copying it. */
            if (reused == block=>reused)
            {
                /* Drop the read lock since we are done with this block. */
                dmf_read_done(dmf);
                break; // success
            }

            /* If there are too many retries then most likely it is not a
             * race condition, but is a corrupted file. For example the
             * physical block contains the wrong block number. This returns
             * an I/O error. */
        retry:
            /* Drop the read lock since we will need to revalidate the
             * block number. */
            dmf_read_done(dmf);

            /* Something is wrong if there are too many retries */
            if (retries++ > 100)
            {
                errno = EIO;
                return -1;
            }
      }

        /* return if done */
        if (--cnt == 0)
            return count;

        /* advance pointers to the next block */
        buf += dmf->blksz;
        bno++;
    }
}

/**
 * Local routine to reserve one of the free blocks for an out of place write.
 * On return one of the slots in the free block array is reserved for this
 * thread. This may require sleeping until a block is available.
 *
 * This interlocks with shrinking by not returning a physical block that needs
 * to be deleted for a shrink that is in progress. If called from shrink it
 * will return dmf->freecnt + slot number rather than wait when there are only
 * blocks scheduled for deletion.
 *
 * @param dmf[in/out]
 * A handle to an open Direct Mapped File returned by dmf_create or dmf_open.
 *
 * @param shrink[in]
 * This is true when called from shrink.
 *
 * @return
 * The slot number is returned. It is an integer between 0 and freecnt-1.
 * When called from dmf_shrink, freecnt is added to the slot number if the
 * free physical block is above the new end of physical blocks. No error
 * return is possible.
 */
int dmf_reserve(dmf_file *dmf, int shrink)
{
    /* Find a free block that we can use to construct the new block
     * version in. Note that we attempt to use the block that has been
     * free the longest. This reduces the chances of a reader having to
     * do a retry because of a read during write. */
    if ((errno = pthread_mutex_lock(&dmf->free_mutex)))
        error(1, errno, ": %s: Error locking free block mutex", dmf->name);
    int slot = dmf->nextfree;
    int offset = 0; // add to slot for the return value
    while (TRUE) // loop until a free slot is found
    {
        /* If the slot is not in use and either there is no shrink in
         * progress, or this free block is not due to be deleted by the
         * shrink, then use it. */
        if (!dmf->inuse[slot])
        {
            /* Use the free block if not being shrunk out. When a shrink is
             * in progress endblk points to the first physical block that will
             * be deleted from the file at the end of shrinking. */
            if (dmf->free[slot] < dmf->endblk)
                break;

            /* Indicate that there is at least one free block past the
             * end of the new smaller file size. This is an optimization
             * to avoid pointless scans of the free array looking for a
             * physical block that will be deleted to assign to a file
             * block that will be deleted. */
            dmf->pastend = TRUE;

            /*
             * If this is a call from shrink tell it to use this slot to
             * move the file block to a physical block that will be deleted.
             * dmf_shrink understands a value of freecnt or more is a free
             * block above the new end.
             */
            if  (shrink)
            {
                offset = dmf->freecnt;
                break;
            }
        }

        /* Try the next slot. */
        if (++slot >= dmf->freecnt)
            slot = 0;

        /* If we scanned all the free blocks but found none to use then
         * we should sleep briefly to allow a block to be freed. However
         * if there was a free block that we avoided because a shrink
         * will delete it, then we use it anyway. */
        if (slot == dmf->nextfree)
        {
            /* All free blocks are in use so sleep 10 microseconds to
             * allow one to become free. This should almost never
             * happen. Note that we give up the mutex while sleeping.
             * This is needed because we may need shrink to make some
             * free blocks that are not above the new end. It needs to
             * get the mutex to use a free block. */
            if ((errno = pthread_mutex_unlock(&dmf->free_mutex)))
                error(1, errno, ": %s: Error unlocking free block mutex", dmf->name);
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 10000;
            nanosleep(&ts, NULL);
            if ((errno = pthread_mutex_lock(&dmf->free_mutex)))
                error(1, errno, ": %s: Error locking free block mutex", dmf->name);
        }
    }

    /* Mark slot as in use. This flag is cleared when there is a new free
     * block in this slot. */
    dmf->inuse[slot] = TRUE;

    /* Set nextfree to the slot after this one. This makes it the most likely
     * slot to reserve next. This encourages the block that has been free the
     * longest to be the next block to be reserved. */
    dmf->nextfree = slot + 1;
    if (dmf->nextfree >= dmf->freecnt)
        dmf->nextfree = 0; // wrap to first slot

    /* Successfully reserved a slot so unlock free array. */
    if ((errno = pthread_mutex_unlock(&dmf->free_mutex)))
        error(1, errno, ": %s: Error unlocking free block mutex", dmf->name);

    /* return the slot that was reserved plus freecnt if shrink wants it. */
    return slot+offset;
}

/**
 * This is a local function to lock one write mutex. It will block until the
 * mutex is available. It returns a pointer to the mutex to pass to unlock.
 *
 * @param dmf[in/out]
 * A handle to an open Direct Mapped File returned by dmf_create or dmf_open.

 * @param blkno[in]
 * The file block number being locked
 *
 * @return
 * A pointer to the chosen mutex for passing to dmf_unlock_write
 */
pthread_mutex_t *dmf_lock_write(dmf_file *dmf, ub8 blkno)
{
    /* Pick the mutex to lock. We multiply the blkno by 7 before going modulo
     * the size of the array. This reduces the chance of false collisions if
     * the blocks are accessed in groups that are a multiple of the array
     * size. */
    pthread_mutex_t *wmtx = dmf->write_mutexes +
            ((blkno*7) % dmf->write_mutex_cnt);

    /* Acquire the lock or die */
    if ((errno = pthread_mutex_lock(wmtx)))
        error(1, errno, ": %s: Error locking a write mutex", dmf->name);

    return wmtx;
}

/**
 * Unlock a mutex that was locked by dmf_lock_write.
 *
 * @param dmf
 * A handle to an open Direct Mapped File returned by dmf_create or dmf_open.
 *
 * @param wmtx
 * return value from dmf_lock_write
 */
void dmf_unlock_write(dmf_file *dmf, pthread_mutex_t *wmtx)
{
    if ((errno = pthread_mutex_unlock(wmtx)))
        error(1, errno, ": %s: Error unlocking a write mutex", dmf->name);
}

/**
 * This is the context for an on-commit operation to ensure the reused count
 * becomes positive if and only if a block write commits.
 */
persistent struct dmf_write_ctx
{
    dmf_block ^block; // physical block to fix
    sb8 reused;       // new reuse count
};
typedef persistent struct dmf_write_ctx dmf_write_ctx;

/*
 * This is an on-commit operation to ensure the reused count becomes
 * positive when a block write commits. Even if the thread dies
 * immediately after committing the write, this will be run by recovery.
 *
 * When run during recovery the write mutex will not be locked. However
 * since dmf_write and dmf_resize cannot be called during recovery, the
 * lock is not necessary.
 *
 * Death during this callback will result in it being called again after the
 * first call is rolled back. Since reused is examined by readers without
 * getting a lock, we do a non-transactional store of the new value. This
 * prevents a reader from seeing the count as positive then having it go back
 * to being negative. The new reused value is in the context rather than being
 * calculated from the existing count so that the non-transactional store is
 * idempotent.
 */
USID("c7a1  f025    717d    a657    fd9a    2f72    5ec9    b570")
void dmf_write_callback@(dmf_write_ctx ^ctx)
{
        ctx=>block=>reused ~= ctx=>reused;
}

/**
 * write a sequence of blocks to a Direct Mapped File from a DRAM or NVM buffer.
 * When the write returns, all data written has been flushed to its NVM DIMM.
 * Blocks beyond the end of file are not written. The count of blocks
 * actually written is returned. This could be zero if the first block to write
 * is past the EOF. A partial write indicates the write extends beyond the EOF.
 *
 * A write that is concurrent with another write to the same block might
 * persist either version of the block, but never a mixture of them. A
 * concurrent read with two concurrent writes might return either version.
 *
 * Process death during a write might write some blocks but not others. The
 * blocks are written in ascending order, and each block written is atomically
 * committed to be persistent before the next block write begins. No partial
 * block writes will ever be returned by a read.
 *
 * It is expected that this will not be called in an NVM transaction. However
 * there is no problem in doing so except that the write will be committed
 * before this returns even if the caller's transaction does not commit. The
 * committed write will be visible to other threads immediately.
 *
 * This cannot be called in an on-abort, on-commit, or on-unlock callback.
 * These callbacks are called to make the make the NVM region file consistent
 * before it is opened. Thus there is no dmf_file struct to pass to the
 * dmf entry points. The dmf_file handle available when the on-abort,
 * on-commit, or on-unlock operation is created will not exist when the
 * operation is called in recovery. Thus it cannot be saved in the callback
 * context. It would be possible to create a version of this library that does
 * support for rollback of writes and doing reads/writes in a callback, but
 * it would require a different API and incur additional overhead.
 *
 * @param dmf[in]
 * A handle to an open Direct Mapped File returned by dmf_create or dmf_open.
 *
 * @param blkno[in]
 * The number of the first block to write. The first block in a file is block
 * zero. The last block is file size minus one. No data is written for blocks
 * beyond the file size.
 *
 * @param buffer[in]
 * The address of a buffer in DRAM or NVM which contains count blocks of data
 * to be written.
 *
 * @param count[in]
 * The number of blocks to write. If blkno+count is greater than the file size
 * then only the blocks up to the file size are written.
 *
 * @return
 * If successful the number of blocks written is returned. This may be less
 * than the number requested if writing past the file size. If there is an
 * error then -1 is returned and errno is set.
 */
ub4 dmf_write(dmf_file *dmf, ub8 blkno, const void *buffer, ub4 count)
{
    /* return if nothing to do */
    if (count == 0)
        return 0;

    /* Loop writing one block to NVM every time around. Each block
     * is done in its own transaction.
     * Initialize pointers to point at first block. */
    dmf_root ^root = dmf->root;
    ub4 bno = blkno;
    ub4 cnt = count;
    const ub1 *buf = buffer;
    dmf_block ^^blkptr = dmf->ptrs + bno;
    while (TRUE)
    {
        /* Find a free block that we can use to construct the new block
         * version in. This is done before locking the write mutex and file
         * size to avoid deadlocks with shrink. */
         int slot = dmf_reserve(dmf, FALSE);

        /* Lock the write mutex for this block. This ensures only one thread
         * at a time can write a specific block. There can be false collisions
         * but that should be rare. This interlocks with shrinking the file
         * since the write mutex is held here and shrink gets and drops every
         * write mutex after changing the file size in dmf.
         */
        pthread_mutex_t *wmtx = dmf_lock_write(dmf, bno);

        /* Verify the file block is still within the file. We return with the
         * count of blocks that were written. Note that since this check is
         * done while holding a write mutex, it is not possible that the
         * file size in dmf will shrink after this check. */
         if (bno >= dmf->filesz)
         {
             dmf_unlock_write(dmf, wmtx); // unlock the write mutex
             dmf->inuse[slot] = FALSE;
             return count-cnt;
         }

        /* Get the free block we are going to overwrite. It is possible that
         * a shrink started after we reserved this free block, and this is a
         * physical block that is going to be deleted by shrink. In this very
         * rare case we need to restart the write so this block can be
         * deleted and we reacquire the locks in the correct order to
         * avoid a deadlock. */
        dmf_block ^fb = dmf->free[slot];  // free block pointer
        if (fb >= dmf->endblk)
        {
            dmf_unlock_write(dmf, wmtx); // unlock the write mutex
            dmf->inuse[slot] = FALSE; // release the slot
            continue;
        }

        /* Local variable to point at the old block we are going to free.
         * It cannot change since we have the write mutex for this file block.
         * Note the old physical block could be beyond the new end if there
         * is a shrink in progress. */
        dmf_block ^ob;

        /* Since we are going to change the block contents we need to change
         * the file block number in the physical block. This must be done
         * before changing the reused count because readers check the
         * reused count and then the block number. This is to catch a reader
         * that is reading a free physical block that is being reused to write
         * the same file block it previously contained and is still being
         * read. */
        fb=>blkno ~= dmf->maxsz;

        /* Make the reused count negative so any reader in the middle of
         * a copy will retry. The reused count is negative while changes are
         * being made to the block or the new version is uncommitted. It is
         * possible that a process died leaving the reuse count negative
         * already. This is OK since no current blocks point here in that
         * case. We can just use the existing value. A persist barrier is
         * needed to ensure reused is persistently stored before any data
         * is copied into the free block. */
        if (fb=>reused > 0)
            fb=>reused ~= -fb=>reused;
        nvm_persist();

        /* Copy the user data to the new block. No undo is needed since the
         * contents of the block are meaningless when in the free array.
         * This uses nvm_copy so that the data will be flushed out of the
         * processor cache. */
        nvm_copy(fb=>data, buf, dmf->blksz);

        /* Switch pointers so the old block becomes free and the new block
         * becomes the current version. This is done in an NVM transaction
         * so that it is atomic. */
        { @ dmf->desc { //begin transaction

            /* Construct an on-commit operation to fix the reuse count to
             * allow readers to see the new block if and only if this
             * transaction commits. We do not want readers to temporarily
             * see the new version then have it rolled back. We do not want
             * to have the value left negative for ever if we die here just
             * after committing.
             *
             * The new value is one more than the last positive value.
             * If the reused count wraps then start over at 2. A reused
             * count of 1 means the block was never written so start at 2.
             *
             * Since we locked the write mutex this is the only transaction
             * that will touch this physical block, and the only transaction
             * with an on-commit operation to advance the reused count.
             *
             * Creating an on-commit operation returns a pointer to its
             * NVM context. The context is cleared when created. We store
             * a pointer to the physical block and its new reused count. The
             * callback uses these to make the new current block available. */
            dmf_write_ctx ^ctx = nvm_oncommit(|dmf_write_callback);
            ctx=>block ~= fb;
            ctx=>reused ~= 1 - fb=>reused;
            if (ctx=>reused < 0)
                ctx=>reused ~= 2; // wrapped after 2 billion writes

            /* Verify the block number in the old current block is correct.
             * If it is wrong then the file is corrupted. Return EIO error
             * for a corrupted region file. */
            ob = ^blkptr;
            if (ob=>blkno != bno)
            {
                dmf->inuse[slot] = 0; // release slot
                dmf_unlock_write(dmf, wmtx); // unlock the write mutex
                nvm_abort();
                errno = EIO;
                return -1;
            }

            /* Set the new block number. Readers verify this so it must be
             * set before the block becomes current. Readers will not look
             * at this until the reused count becomes positive at the commit
             * of this transaction. */
             fb=>blkno @= bno;

            /* Now that we have the mutex we can be certain which block will
             * be moved to the free array. If we are shrinking this file and
             * the old block will be deleted at the end of shrink, then set
             * the past end flag so that the thread reorganizing the blocks
             * to be deleted will look in the free array. */
            if (ob >= dmf->endblk)
                dmf->pastend = TRUE;

            /* Store old block in free array. It is still usable for a read
             * that started before this write transaction commits. */
            dmf->free[slot] @= ob;

            /* Store new block in the pointer array. This means readers may
             * see the new block pointer even if this does not commit.
             * However the block is still marked as busy with a negative
             * reuse count. Thus a reader will spin looking for the reuse
             * count to go positive until the on commit operation makes
             * it positive or roll back restores the old block. */
            ^blkptr @= fb;

            /* end the transaction code block to commit the write setting
             * the reused count to a new positive value. */
        }}

        /* Release the slot since we are done with it. Note that we do not
         * reuse the same slot over and over because that would make it more
         * likely that a reader would have to retry. This does not need to
         * hold the mutex since we are the only one that can release it. */
        dmf->inuse[slot] = FALSE;

        /* Unlock the write mutex so writes can proceed on this block. This
         * is done after the transaction commits making the reuse count
         * be a new positive value. */
        dmf_unlock_write(dmf, wmtx); // unlock the write mutex

        /* If we are shrinking this file and just made a block free that will
         * be deleted at the end of shrink, then set the past end flag again.
         * This is in case the flag was cleared after being set in the
         * above transaction and before the transaction committed. Having
         * pastend be TRUE when it shouldn't is not a problem. Having it FALSE
         * when it should be TRUE can make shrinking spin. */
        if (ob >= dmf->endblk)
            dmf->pastend = TRUE;

        /* return if done */
        if (--cnt == 0)
            return count;

        /* advance pointers to the next block */
        buf += root=>blksz;
        bno++;
        blkptr++;
    }
}

/**
 * Grow the block and pointer arrays. This is done atomically with adding
 * NVM to the file. Return 0 on success and -1 on error with errno set.
 *
 * @param dmf[in/out]
 * A handle to an open Direct Mapped File returned by dmf_create or dmf_open.
 *
 * @return
 * Return 0 on success and -1 on error with errno set.
 */
int dmf_grow(dmf_file *dmf, ub4 filesz)
{
    /* Stat the file to get the sizes */
    nvm_region_stat rstat;
    if (!nvm_query_region(dmf->desc, &rstat))
    {
        return -1;
    }

    /* calculate the new size for the block extent */
    size_t extsz = (filesz + dmf->freecnt) * (sizeof(dmf_block) + dmf->blksz)
            + DMF_PAGE_SIZE - 1;
    extsz -= extsz % DMF_PAGE_SIZE;

    /* calculate the new size for the pointer extent */
    size_t ptrsz = filesz * sizeof(dmf_block^) + DMF_PAGE_SIZE - 1;
    ptrsz -= ptrsz % DMF_PAGE_SIZE;

    /* Verify the new size is not greater than the available virtual space. */
    size_t newsize = ((ub1*)dmf->blocks - (ub1*)rstat.base) + extsz;
    if (newsize > rstat.vsize)
    {
        errno = EINVAL;
        return -1;
    }

    /* Transaction to add and initialize space */
    { @ dmf->desc {
        /* Transactionally resize the block and pointer extents. */
        if (!nvm_resize_extent(dmf->blocks, extsz))
        {
            int saverr = errno;
            nvm_abort();
            errno = saverr;
            return -1;
        }
        if (!nvm_resize_extent(dmf->ptrs, ptrsz))
        {
            int saverr = errno;
            nvm_abort();
            errno = saverr;
            return -1;
        }

        /* Initialize every new physical block and save a pointer to it.
         * Start at the first block past allocated blocks. */
        dmf_root ^root = dmf->root;
        ub4 alloc = root=>allocated;
        ub4 bno = root=>allocated - dmf->freecnt;
        dmf_block ^^ptr = %dmf->ptrs[bno]; // first new pointer
        ub1 ^blk = (ub1^)dmf->blocks + alloc*(sizeof(dmf_block) + dmf->blksz);
        dmf_block ^block = (dmf_block^)blk;
        ub4 cnt = filesz + dmf->freecnt - alloc; // physical blocks to initialize
        while(cnt--)
        {
            /* Format the next new physical block be the next new file block.
             * No undo is needed since failure to complete the grow will
             * return the extents to their original size, and there cannot
             * be any writes to these blocks above the current file size. */

            /* Format the block so that it will verify. We lie about the
             * data size to save time. */
            nvm_init_struct(blk, shapeof(dmf_block), 0);
            block = (dmf_block^)blk;

            /* Set pointer to the formatted block */
            ^ptr ~= block;

            /* Initialize the reused count to 1 since zero does not work.
             * A reused count of 1 is used to indicate the block has never
             * been written. */
            block=>reused ~= 1;

            /* Initialize the block number */
            block=>blkno ~= bno;

            /* go to next block */
            blk += sizeof(dmf_block) + dmf->blksz;
            bno++;

            /* Go to next pointer.  */
            ptr++;
        }

        /* Now that it is properly initialized we can advance allocated and
         * set the new file size. The code uses the file size in the dmf
         * so the new file size does not allow writes in the new blocks
         * until the file size is set in the dmf after commit. */
        root=>allocated @= filesz + dmf->freecnt;
        root=>filesz @= filesz;

        /* commit the grow by exiting the transaction code block */
    }}

    /* Set the new size. There is no need to wait for all reads/writes under
     * the old size to complete since this will not invalidate any of those
     * reads or writes. It is OK to do this after the transaction commits
     * because failure to store these means the application died. The next
     * attach will use the file size in the root struct.*/
    dmf_new_size(dmf, filesz); // allow access to new blocks

    /* return success */
    return 0;
}

/**
 * Get and drop every write mutex in the Direct Mapped File. This ensures
 * every writer will see any dmf_file state that was set before this was
 * called. This is only used when shrinking a file. It ensures no more writes
 * are done to file blocks beyond the new end.
 *
 * @param dmf[in/out]
 * A handle to an open Direct Mapped File returned by dmf_create or dmf_open.
 */
void dmf_sync_writes(dmf_file *dmf)
{
    pthread_mutex_t *wmtx = dmf->write_mutexes;
    ub4 cnt = dmf->write_mutex_cnt;

    /* Loop getting and dropping each mutex. This could be more efficient if
     * there was a first pass the did try gets and kept a per mutex flag of
     * tries that failed. Then did another pass of wait gets as needd. */
    while (cnt--)
    {
        if ((errno = pthread_mutex_lock(wmtx)))
            error(1, errno, ": %s: Error locking a write mutex", dmf->name);
        if ((errno = pthread_mutex_unlock(wmtx)))
            error(1, errno, ": %s: Error unlocking a write mutex", dmf->name);

        wmtx++; // next mutex
    }
}

/**
 * This is used to make one file block that is going to be deleted by shrink
 * have its contents in a physical block that is also going to be deleted by
 * shrink. This is done atomically in a transaction so the file state remains
 * consistent A free physical block that is going to be deleted must already
 * be reserved. The reservation is released when this returns.
 *
 * This is essentially a write of file block bno with its current
 * contents. We do not need a write mutex because it is past endblk.
 * See dmf_write for more rational on how this works.
 *
 * @param dmf[in/out]
 * A handle to an open Direct Mapped File returned by dmf_create or dmf_open.
 *
 * @param bno[in]
 * The file block number which is to be deleted that needs a physical block
 * that will be deleted.
 *
 * @param slot[in]
 * A reserved free physical block slot that has the physical block that will
 * be deleted by shrink.
 */
void dmf_make_above(dmf_file *dmf, ub8 bno, int slot)
{
    /* Pointer to free physical block to move contents into. It is above the
     * new end of physical blocks. */
    dmf_block ^fb = dmf->free[slot];

    /* Pointer to old physical block to make free. It is below the
     * new end of physical blocks. */
    dmf_block ^ob = dmf->ptrs[bno];

    /* Since we are going to change the block contents we need to change
     * the file block number in the physical block. This must be done
     * before changing the reused count because readers check the
     * reused count and then the block number. This is to catch a reader
     * that is reading a free physical block that is being reused to write
     * the same file block it previously contained and is now being
     * read. */
    fb=>blkno ~= dmf->maxsz;

    /* Make the reused count negative so any reader in the middle of
     * a copy will retry. The reused count is negative while changes are
     * being made to the block or the new version is uncommitted. It is
     * possible that a thread died leaving the reuse count negative
     * already. This is OK since no current blocks point here in that
     * case. We can just use the existing value. A persist barrier is
     * needed to ensure reused is persistently stored before any data
     * is copied into the free block. */
    if (fb=>reused > 0)
        fb=>reused ~= -fb=>reused;
    nvm_persist();

    /* Copy the current data to the new block. No undo is needed since the
     * contents of the block are meaningless when in the free array.
     * This uses nvm_copy so that the data will be flushed out of the
     * processor cache. */
    nvm_copy(fb=>data, ob=>data, dmf->blksz);

    /* Switch pointers so the old block becomes free and the free block
     * becomes the current version. This is done in an NVM transaction
     * so that it is atomic. */
    { @ dmf->desc { //begin transaction

        /* Construct an on-commit operation to fix the reuse count to
         * allow readers to see the new block if and only if this
         * transaction commits. We do not want readers to temporarily
         * see the new version then have it rolled back. We do not want
         * to have the value left negative for ever if we die here just
         * after committing.
         *
         * The new value is one more than the last positive value.
         * If the reused count wraps then start over at 2. A reused
         * count of 1 means the block was never written so start at 2.
         *
         * Creating an on-commit operation returns a pointer to its
         * NVM context. The context is cleared when created. We store
         * a pointer to the physical block and its new reused count. The
         * callback uses these to make the new current block available. */
        dmf_write_ctx ^ctx = nvm_oncommit(|dmf_write_callback);
        ctx=>block ~= fb;
        ctx=>reused ~= 1 - fb=>reused;
        if (ctx=>reused < 0)
            ctx=>reused ~= 2; // wrapped after 2 billion writes

        /* Set the new block number. Readers verify this so it must be
         * set before the block becomes current. Readers will not look
         * at this until the reused count becomes positive at the commit
         * of this transaction. */
         fb=>blkno @= bno;

         /* Store old block in free array. It is still usable for a read
          * that started before this write transaction commits. */
         dmf->free[slot] @= ob;

         /* Store new block in the pointer array. This means readers may
          * see the new block pointer even if this does not commit.
          * However the block is still marked as busy with a negative
          * reuse count. Thus a reader will spin looking for the reuse
          * count to go positive until the on commit operation makes
          * it positive or roll back restores the old block. */
         dmf->ptrs[bno] @= fb;

         /* end the transaction code block to commit the switch */
     }}

    /* Release the slot since we are done with it. Note that we do not
     * reuse the same slot over and over because that would make it more
     * likely that a reader would have to retry. This does not need to
     * hold the mutex since we are the only one that can release it. */
    dmf->inuse[slot] = FALSE;
}

/**
 * This is used to make one file block that is going to be kept by shrink
 * have its contents in a physical block that is also going to be kept by
 * shrink. This is done when the file block currently is in a physical
 * block that shrink is going to delete. This makes the physical block
 * free and available for a file block that is above the new file size.
 *
 * This is done atomically in a transaction so the file state remains
 * consistent A free physical block that is going to be kept must already
 * be reserved. The reservation is released when this returns.
 *
 * This is essentially a write of file block bno with its current
 * contents. This may not be possible if a client wrote the file block
 * after we saw its physical block was past the new end.
 *
 * @param dmf[in/out]
 * A handle to an open Direct Mapped File returned by dmf_create or dmf_open.
 *
 * @param kept[in]
 * The file block number which is to be kept that needs a physical block
 * that will be kept.
 *
 * @param slot[in]
 * A reserved free physical block slot that has the physical block that will
 * be deleted by shrink.
 *
 * @return
 * Zero is returned if successful and -1 if the file block is no longer
 * in a physical block that is past the new end.
 */
int dmf_make_below(dmf_file *dmf, ub8 kept, int slot)
{
    /* get the free block we are going to overwrite. */
    dmf_block ^fb = dmf->free[slot];  // free block pointer

    /* Lock the write mutex to ensure no one else is doing a write to
     * this file block. */
    pthread_mutex_t *wmtx = dmf_lock_write(dmf, kept);

    /* Get the old current block that we are going to make free. This
     * must be done after we get the write lock to ensure the physical
     * block for this file block does not change. */
    dmf_block ^ob = dmf->ptrs[kept];

    /*
     * Now that we have the write lock we check to see if this is still
     * the same physical block we saw before we locked out writes to
     * this block. If it isn't then it was freed and pastend must be
     * true now. Thus we should retry the scan through the free
     * blocks. What actually matters is that the physical block we
     * are going to free is above the new end of physical blocks. So
     * that is what we check.
     */
    if (ob < dmf->endblk)
    {
        dmf_unlock_write(dmf, wmtx); // unlock the write mutex
        return -1; // not the same block we saw without the write mutex locked
    }

    /* Since we are going to change the block contents we need to change
     * the file block number in the physical block. This must be done
     * before changing the reused count because readers check the
     * reused count and then the block number. This is to catch a reader
     * that is reading a free physical block that is being reused to write
     * the same file block it previously contained and is now being
     * read. */
    fb=>blkno ~= dmf->maxsz;

    /* Make the reused count negative so any reader in the middle of
     * a copy will retry.  */
    if (fb=>reused > 0)
        fb=>reused ~= -fb=>reused;
    nvm_persist();

    /*
     * Copy the current block contents to the free block.
     */
    nvm_copy(fb=>data, ob=>data, dmf->blksz);

    /*
     * Switch the physical blocks used by the kept file block in a
     * transaction.
     */
    { @ dmf->desc {

        /* Construct an on-commit operation to fix the reuse count to
         * allow readers to see the new block if and only if this
         * transaction commits. */
        dmf_write_ctx ^ctx = nvm_oncommit(|dmf_write_callback);
        ctx=>block ~= fb;
        ctx=>reused ~= 1 - fb=>reused;
        if (ctx=>reused < 0)
            ctx=>reused ~= 2; // wrapped after 2 billion writes

        /*
         * Set the new block number in the free block.
         */
        fb=>blkno @= kept;

        /* Store old block in free array. It is still usable for a
         * read that started before this transaction commits. */
        dmf->free[slot] @= ob;

        /* Store new block in the pointer array. */
        dmf->ptrs[kept] @= fb;

        /* commit the switch to a physical block that will be kept */
    }}

    /* Unlock the write mutex so writes can proceed on this block. This
     * is done after the transaction commits making the reuse count
     * be a new positive value. */
    dmf_unlock_write(dmf, wmtx); // unlock the write mutex

    return 0; // success
}

/**
 * Shrink the block and pointer arrays. First all the file blocks that are
 * going to be deleted are moved to physical blocks that are beyond the new
 * allocated size and thus will be deleted when shrinking the extents.
 * Then the pointer and block extents are resized to be smaller atomically
 * with shrinking the extents.
 *
 * @param dmf[in/out]
 * A handle to an open Direct Mapped File returned by dmf_create or dmf_open.
 *
 *
 * @return
 * Return 0 on success and -1 on error with errno set.
 */
int dmf_shrink(dmf_file *dmf, ub4 filesz)
{
    dmf_root ^root = dmf->root; // point to root struct
    ub2 freecnt = dmf->freecnt; // entries in free block array

    /* Set the new file size in the dmf to be the new smaller size. This makes
     * new reads see the new file size. This also sets the new end physical
     * block to prevent writes to physical blocks being deleted. */
    dmf_new_size(dmf, filesz);

    /*
     * Cycle through all the write mutexes. This ensures all writers have seen
     * the new size, and will not write on any of the file blocks we are
     * deleting or use any physical blocks that will be deleted.
     */
    dmf_sync_writes(dmf);

    /* Keep a pointer to the first physical block we are going to delete.
     * Since we have the resize mutex it cannot change and a local variable
     * will be more efficient. */
    dmf_block ^endblk = (dmf_block^)dmf->endblk;

    /* Set the pastend flag so that the first search for a physical block that
     * will be deleted will look in the free array. */
    dmf->pastend = TRUE;

    /*
     * Now comes the hard part. We have to rearrange all the blocks so the
     * file blocks that are being deleted all use physical blocks that
     * are being deleted. This must be done in a way that maintains the
     * integrity of the data structure so that our death here will leave the
     * file consistent and at the original size. Note that since we have
     * exactly enough physical blocks for each file block and each free
     * block, If all the file blocks to delete point to physical blocks
     * to delete, Then all other pointers to physical blocks must point
     * to blocks that will not be deleted. Thus we loop through each of the
     * file blocks that will be deleted making them point at a physical
     * block that will be deleted.
     */

    /* If there are no free blocks that are going to be deleted, then we need
     * to look through the kept file blocks to find ones that point to
     * physical blocks we are going to delete. We keep the block number of
     * the last kept block that we checked to avoid redundant scans. */
    ub4 kept = 0;

    /* This is used to look for free blocks that are above the end of the
     * physical blocks when shrink commits. The next scan starts where the
     * last one ended. */
    int slot = 0;

    /*
     * loop through all the file blocks that are being deleted to make them
     * use physical blocks we are deleting. Note that the file size in the
     * NVM root struct is still the old size.
     */
    ub4 bno; // file block to be deleted
    for (bno = filesz; bno < root=>filesz; bno++)
    {
        /* Pointer to block pointer of a file block to be deleted */
        dmf_block ^^blkptr = dmf->ptrs + bno;

        /* Pointer to the physical block owned by a file block to be deleted.
         * It contains the data that must be preserved when making the file
         * block point to a physical block past the new end. */
        dmf_block ^ob = ^blkptr;  // old physical block pointer

        /* Pointer to a physical block that is beyond the new end and will
         * be deleted by this shrink operation. It can be used to hold
         * contents of the file block that will be deleted. */
        dmf_block ^sb;            // shrinking physical block pointer

        /* If this pointer already points to one of the physical blocks to be
         * deleted then nothing needs to be done. */
        if (ob >= endblk)
            continue;

        /* If we find a physical block that we can use, but it is used for a
         * write before we can get a lock on its write mutex, we come back
         * here to start over. */
    retry:

        /*
         * Look through the free array unless we are sure there are no
         * candidates there. dmf->pastend was set true above, and will be set
         * to true again if a physical block above the new size is freed. It
         * is only cleared if all the free blocks are scanned and all are
         * physical blocks below the new end.
         */
        if (dmf->pastend)
        {
            /* lock the mutex that protects the inuse array */
            if ((errno = pthread_mutex_lock(&dmf->free_mutex)))
                error(1, errno, ": %s: Error locking free block mutex",
                        dmf->name);

            /* Loop through the free block array looking for a physical block
             * that is going to be deleted. Also calculate a new value for
             * pastend. This is the only place it is zeroed. */
        rescan:
            dmf->pastend = FALSE;
            int cnt = freecnt; // count of slots to look at before giving up
            do {
                /* Set pastend if we find a free block that is to be deleted
                 * even if it has been reserved for a write. If it is
                 * available then choose it to switch with the physical block
                 * for block bno. */
                if (dmf->free[slot] >= endblk){
                    dmf->pastend = TRUE;
                    if (!dmf->inuse[slot])
                    {
                        dmf->inuse[slot] = TRUE; // reserve the block
                        break; // found a physical block we can use
                    }
                }

                /* go to next slot */
                if (++slot >= freecnt)
                    slot = 0; // wrapped
            } while (--cnt);

            /* If there are any free blocks that are past the new file end but
             * are inuse, then we sleep a bit so they get used or left free.
             * We do not want to leave any free blocks past the new end when
             * we start scanning the file blocks that are being kept. That
             * scan might spin never finding a block to switch to. */
            if (dmf->pastend && cnt == 0)
            {
                /* Sleep 5 microseconds. Note that we do not give up the mutex
                 * while sleeping. This is OK since the mutex is not acquired
                 * to free up a slot, except for shrinking. */
                struct timespec ts;
                ts.tv_sec = 0;
                ts.tv_nsec = 5000;
                nanosleep(&ts, NULL);
                goto rescan;
            }

            /* Done with free block scan so release mutex lock */
            if ((errno = pthread_mutex_unlock(&dmf->free_mutex)))
                error(1, errno, ": %s: Error unlocking free block mutex",
                        dmf->name);

            /* If we found a free block use it */
            if (cnt)
            {
                /* Make file block bno use a physical block above the new end */
                dmf_make_above(dmf, bno, slot);

                /* The file block bno is now associated with a physical block
                 * that is past the new file size. We can go to the next
                 * file block. */
                continue;
            }
            /* No free physical block beyond the new size was found, so fall
             * through to scan kept  file blocks. */
        }

        /* No free physical block beyond the new size was found so we
         * need to look for a file block that is being kept but uses
         * a physical block above the new file size. We do not lock the write
         * mutex for every block as we scan. This is faster, but there is
         * a small probability that we will miss a block that is being
         * written with a physical block beyond the new file size. This
         * means it may take more than one pass to find all the blocks. */
        ub8 limit = filesz;
        while ((sb = dmf->ptrs[kept]) < endblk)
        {
            /* If a usable block becomes free while we are searching, then
             * go back to use it. */
            if (dmf->pastend)
                goto retry;

            /* advance to next file block */
            if (++kept >= filesz)
                kept = 0;

            /* If we scanned every block, sleep for a bit to avoid hogging
             * the processor. */
            if (--limit == 0)
            {
                limit = filesz;
                struct timespec ts;
                ts.tv_sec = 0;
                ts.tv_nsec = 20000;  // 20 microseconds
                nanosleep(&ts, NULL);
                goto retry;
            }
        }

        /*
         * The file block being kept needs to have its current contents
         * moved to a physical block that is being kept, and have its
         * current shrinking block made free. This will allow us to
         * use the free block to make the file block being deleted use
         * a physical block that is being deleted. This is essentially
         * the same as a write of the block with its current contents.
         */

        /* First reserve a free block that is below the new end */
        slot = dmf_reserve(dmf, TRUE);

        /* If dmf_reserve found a free blocks that is past endblk then
         * use that block. Note that kept still names the file block with
         * a physical block past the new end and will be the first block
         * checked when kept blocks are looked at again. */
        if (slot >= freecnt)
        {
            /* the slot number returned by dmf_reserved is actually freecnt
             * more than it should be. */
            slot -= freecnt;

            /* Make file block bno use a physical block above the new end */
            dmf_make_above(dmf, bno, slot);

            /* The file block bno is now associated with a physical block
             * that is past the new file size. We can go to the next
             * file block. */
            continue;
        }

        /*
         * Switch the free physical block that is below the new end with
         * the current physical block that is above the new end. This
         * cannot be done if a write happens before the switch transacation
         * begins since the current block will no longer be above the
         * new end of physical blocks.
         */
        if (dmf_make_below(dmf, kept, slot))
        {
            /* Switch failed. The block we saw as current was moved to the
             * free array by a write to the block. Now that there is a free
             * block above the new end go to retry to find it. */
            dmf->inuse[slot] = FALSE; // release slot reservation
            goto retry;
        }

        /*
         * We now have a free block that is going to be deleted by this shrink.
         * Switch file block bno to use the newly freed block for its contents.
         * We still have its slot reserved so the slot has the same block we
         * just freed.
         */
        dmf_make_above(dmf, bno, slot);

        /* The file block bno is now associated with a physical block
         * that is past the new file size. We can go to the next
         * file block that will be deleted by shrink. */
    }

#if DMF_VERIFY_SHUFFLE
    /* If this code is enabled, then every shrink verifies that the shuffle
     * successfully made all file blocks to be deleted use a physical block
     * that is to be deleted. This should not be needed. */
    errno = 0;

    /* Verify all file blocks not being deleted are using physical blocks
     * that are not being deleted. */
    for (bno = 0; bno < filesz; bno++)
    {
        dmf_block ^block = dmf->ptrs[bno];
        if (block >= endblk)
        {
            errno = EIO;
            error(0,errno, "Shrink left kept block with above physical block");
            errno = EIO;
        }
    }

    /* Verify all file blocks being deleted are using physical blocks
     * that are being deleted. */
    for (bno = filesz; bno < root=>filesz; bno++)
    {
        dmf_block ^block = dmf->ptrs[bno];
        if (block < endblk)
        {
            errno = EIO;
            error(0,errno, "Shrink left delete block with below physical block");
            errno = EIO;
        }
    }

    /* Verify all free blocks are using physical blocks that are not
     * being deleted. */
    for (bno = 0; bno < dmf->freecnt; bno++)
    {
        dmf_block ^block = dmf->free[bno];
        if (block >= endblk)
        {
            errno = EIO;
            error(0,errno, "Shrink left free block with above physical block");
            errno = EIO;
        }
    }

    /* kill process on failure */
    if (errno)
        error(1, errno, "Block shuffle for shrink did not work correctly");

#endif

    /* We now have all the file blocks after the new EOF pointing at physical
     * blocks that are after the new physical size. Since we changed the file
     * size used for checking reads, there should be no new reads of file
     * blocks that are going to be deleted, and thus no new reads of physical
     * blocks that are going to be deleted. However there could be active
     * reads from physical blocks that are going to be deleted. This happens
     * if the read started before the kept file block was reassigned to a
     * physical block that we are keeping. By stopping all new reads and
     * waiting for all active reads to complete, we ensure there are no reads
     * from physical blocks we are deleting. Thus we can then release NVM
     * back to the file system. */
     dmf_wait_reads(dmf);

     /* All access to the physical blocks and file block pointers above the
      * new file size have ended. This means we can release the space
      * back to the file system by changing the extent sizes */

    /* calculate the new size for the block extent */
    size_t extsz = (filesz + freecnt) * (sizeof(dmf_block) + dmf->blksz)
            + DMF_PAGE_SIZE - 1;
    extsz -= extsz % DMF_PAGE_SIZE;

    /* calculate the new size for the pointer extent */
    size_t ptrsz = filesz * sizeof(dmf_block^) + DMF_PAGE_SIZE - 1;
    ptrsz -= ptrsz % DMF_PAGE_SIZE;

    /* Create a transaction to resize the extents and sizes atomically.
     * This makes shrink atomic. */
    { @ dmf->desc {
        /* Transactionally resize the block and pointer extents. */
        if (!nvm_resize_extent(dmf->blocks, extsz))
        {
            int saverr = errno;
            nvm_abort();
            errno = saverr;
            return -1;
        }
        if (!nvm_resize_extent(dmf->ptrs, ptrsz))
        {
            int saverr = errno;
            nvm_abort();
            errno = saverr;
            return -1;
        }

        /* Set new persistent sizes. Note that endblk and filesz in dmf
         * were set to these values at the beginning of this routine. */
        root=>filesz @= filesz;
        root=>allocated @= filesz + freecnt;
    }}

    /* return success */
    return 0;
}

/*
 * Resize an open Direct Mapped File. The new size may be larger or smaller
 * than the current size. The file may be in active use during the resize.
 * Two simultaneous resizes of the same file will be serialized so that one
 * is done before the other begins. The final size could be either of the
 * two sizes.
 *
 * If shrinking a file, the old blocks after the new size will have their
 * contents discarded. If growing a file the new blocks after the old size
 * will read as all zero unless they are written.
 *
 * Resize is atomic. A failure during file resize will either complete the
 * resize or or leave the file at the same size.
 *
 * The new size must be less than or equal to the maximum size specified at
 * creation. There is not enough virtual address space to make the file larger
 * than the maximum size.
 *
 * It is expected that this will not be called in an NVM transaction. However
 * there is no problem in doing so except that the resize will be committed
 * before this returns even if the caller's transaction does not commit. The
 * committed resize will be visible to other threads immediately.
 *
 * This cannot be called in an on-abort, on-commit, or on-unlock callback.
 * These callbacks are called to make the make the NVM region file consistent
 * before it is opened. Thus there is no dmf_file struct to pass to the
 * dmf entry points. The dmf_file handle available when the on-abort,
 * on-commit, or on-unlock operation is created will not exist when the
 * operation is called in recovery. Thus it cannot be saved in the callback
 * context. It would be possible to create a version of this library that does
 * support for rollback of resize and doing resize in a callback, but
 * it would require a different API and incur additional overhead.
 *
 * @param dmf[in/out]
 * A handle to an open Direct Mapped File returned by dmf_create or dmf_open.
 *
 * @param filesz[in]
 * This is the new file size in blocks. It may be larger or smaller than the
 * current file size.
 *
 * @return
 * Zero is returned on success. If there is an error then errno is set and
 * -1 is returned.
 */
int dmf_resize(dmf_file *dmf, ub8 filesz)
{
    /* there is not enough virtual address space to grow beyond maxsz */
    if (filesz > dmf->maxsz)
    {
        errno = EFBIG;
        return -1;
    }

    /* lock the file resize mutex to ensure only one resize at a time */
    if ((errno = pthread_mutex_lock(&dmf->size_mutex)))
        error(1, errno, ": %s: Error locking resize mutex", dmf->name);

    /* Decide if this is growing or shrinking and call the appropriate code. */
    int err = 0;
    if (filesz > dmf->filesz)
    {
        if (dmf_grow(dmf, filesz))
            err = errno;
    }
    else if (filesz < dmf->filesz)
    {
        if (dmf_shrink(dmf, filesz))
            err = errno;
    }

    /* unlock the file resize mutex to allow another resize */
    if ((errno = pthread_mutex_unlock(&dmf->size_mutex)))
        error(1, errno, ": %s: Error unlocking resize mutex", dmf->name);

    errno = err;
    return err ? -1 : 0;
}
