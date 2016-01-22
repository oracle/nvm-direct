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
/*
 * dmf.h - Interface to Direct Mapped File
 *
 *  Created on: Dec 17, 2015
 *      Author: bbridge
 */

#ifndef DMF_H_
#define DMF_H_

/* Standard Oracle types */
typedef unsigned char ub1;
typedef unsigned short ub2;
typedef unsigned int ub4;
typedef unsigned long long ub8;
typedef short sb2;
typedef int sb4;
typedef long long sb8;
#define FALSE 0
#define TRUE  1

/**
 * dmf_file is an opaque pointer to a file that the dmf module has open.
 * Most calls pass this handle as the first argument. This is a DRAM address.
 */
typedef struct dmf_file dmf_file;

/**
 * This struct contains statistics about the file. An instance of this is
 * passed to dmf_query to return data about an open file.
 */
struct dmf_stat
{
    /**
     * Name of the file passed to create/open. This will be undefined if
     * the file is closed.
     */
    const char *name;

    /**
     * Block size in bytes. All I/O is in terms of this block size. This is
     * set at creation and cannot be changed.
     */
    size_t blksz;

    /**
     * File size in blocks. Blocks 0 through filesz-1 are available for I/O.
     * This can be changed by resizing the file.
     */
    ub8 filesz;

    /**
     * This is the maximum that filessz can ever become. It is set at creation
     * and cannot be changed. This is used to to determine how much virtual
     * address space the file consumes when mapped into a process. A process
     * may run out of virtual address space if this is too large, but it does
     * not affect the amount of NVM consumed.
     */
    ub8 maxsz;

    /**
     * This is the maximum number of simultaneous writes that the file can
     * support. If more writes are in flight at the same time, then some will
     * block until others complete. Note that a write of multiple blocks only
     * counts as a single write since they are all done one at a time by the
     * same thread. There are no limitations on the number of concurrent reads.
     * This many spare physical block of NVM are allocated for doing out of
     * place writes, so it is best to make this pretty small.
     */
    ub2 writes;

    /**
     * This is the amount of physical NVM bytes consumed by the file. This
     * includes the metadata used to track the blocks, spare blocks for out of
     * place writes, and space consumed by rounding up to a page boundary. It
     * does not include any NVM the file system consumes for managing the file.
     */
    size_t psize;

    /**
     * This is the amount of virtual address space consumed by the file in
     * bytes.
     */
    size_t vsize;
};
typedef struct dmf_stat dmf_stat;

/**
 * Get information about the file shape and return it in a dmf_stat.
 *
 * @param[in] dmf
 * A handle to an open Direct Mapped File returned by dmf_create or dmf_open.
 *
 * @param[in] dstat
 * point to struct to fill with results.
 *
 * @return
 * Zero is returned on success. If there is an error then errno is set and
 * -1 is returned.
 */
int dmf_query(dmf_file *dmf, dmf_stat *dstat);

/**
 * Check the self consistency of the file. This will block any new writes or
 * resizing while it is checking. Inconsistencies are reported on stderr
 * via error(3) in libc.
 *
 * @param[in] dmf
 * A handle to an open Direct Mapped File returned by dmf_create or dmf_open.
 *
 * @param[in] maxerrs
 * Maximum number of errors to report on stderr before returning.
 *
 * @return
 * The return value is the number of problems found. Thus a return of 0
 * indicates a good file.
 */
ub8 dmf_check(dmf_file *dmf, int maxerrs);

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
        mode_t mode);     // permissions

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
dmf_file *dmf_open(const char *name, void *addr);

/**
 * Close an open file unmapping it from the process. There must not be any I/O
 * in progress when this is called. After this is called the handle cannot be
 * used. The DRAM data structures pointed to by the handle are no longer
 * allocated.
 *
 * Note the warnings about ignoring the return value from close do not apply
 * to Direct Mapped Files since they are never cached and the write system
 * call is never used.
 *
 * @param[in] dmf
 * A handle to an open Direct Mapped File returned by dmf_create or dmf_open.
 */
void dmf_close(dmf_file *dmf);

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
int dmf_delete(const char *name);

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
 * @param buffer[out]
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
ub4 dmf_read(dmf_file *dmf, ub8 blkno, void *buffer, ub4 count);

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
ub4 dmf_write(dmf_file *dmf, ub8 blkno, const void *buffer, ub4 count);

/**
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
 * @param dmf[in]
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
int dmf_resize(dmf_file *dmf, ub8 filesz);
#endif /* DMF_H_ */
