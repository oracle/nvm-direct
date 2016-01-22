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
/**\file
   NAME\n
     nvms_region.c - NVM services for NVM region management

   DESCRIPTION\n
     These services are for managing region files that contain the NVM managed 
     by the library. A region file can be sparse with multiple extents of NVM. 
     On Linux this will be implemented by OS calls to an NVM file system.

 */
#define _XOPEN_SOURCE 600
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include "errno.h"
#include "nvms_misc.h"
/**
 * This is a struct describing one single region file. A pointer to one of these
 * is used as a file handle by the NVM library.
 */
struct nvms_file
{
    /**
     * Index into the array of open region files where this struct is found.
     */
    int index;

    /**
     * The OS file descriptor for this file
     */
    int osfd;

    /**
     * Virtual address where the region is mapped, or zero if not mapped
     */
    uint8_t *attach;

    /**
     * Virtual address space reserved for the file
     */
    size_t vsize;
};
typedef struct nvms_file nvms_file;

/* the maximum number of regions supported */
#define MAX_REGIONS 64

/**
 * This is the array of all open region files. Available entries are 0
 */
nvms_file *nvms_open_files[64];

/**
 * This is the mutex used to coordinate access to the open files table
 */
static pthread_mutex_t nvms_file_mutex;

/**
 * This is the once control for initializing the mutex
 */
pthread_once_t nvms_file_once = PTHREAD_ONCE_INIT;
/**
 * Initialize the mutex
 */
static void nvms_init_file()
{
    pthread_mutex_init(&nvms_file_mutex, NULL);
}
/**
 * This creates a region file and returns a handle to it. Initially it 
 * contains pspace bytes of NVM. An error is returned if there is 
 * insufficient NVM to allocate pspace bytes. The file size is set to vspace
 * so that the file consumes vspace bytes of virtual address space when 
 * mapped. The file contents are zero.
 * 
 * It is an error if the region file already exists.
 * 
 * If there are any errors then errno is set and the return value is zero.
 * 
 * @param[in] pathname
 * The pathname of the region file to create. It must be in an NVM
 * file system.
 * 
 * @param[in] vspace
 * The file size for consuming virtual address space when mapped.
 * 
 * @param[in] pspace
 * Initial physical memory for the base extent, in bytes.
 * 
 * @param[in] mode
 * This is the same as the mode parameter to the Posix creat system call.
 * See the man page open(2) for the bit definitions.
 * 
 * @return 
 * Open region file handle or zero on error
 */
nvms_file *nvms_create_region(
        const char *pathname,
        size_t vspace,
        size_t pspace,
        mode_t mode
        )
{
    /* Create the requested file */
    int flags = O_RDWR | O_CREAT | O_TRUNC | O_EXCL;
    int fd = open(pathname, flags, mode);
    if (fd < 0)
        return NULL; // open set errno

    /* allocate and zero the base extent */
    int e = posix_fallocate(fd, 0, pspace);
    if (e != 0)
    {
        close(fd);
        unlink(pathname);
        errno = e;
        return NULL;
    }

    /* fill it with zeroes to allocate the desired amount of space. Note that
     * this will always round up to the next megabyte. */
//    const size_t bufsz = 1024 * 1024;
//    void *buf = malloc(bufsz);
//    memset(buf, 0, bufsz);
//    size_t cleared;
//    for (cleared = 0; cleared < pspace; cleared += bufsz)
//    {
//        int cnt = pwrite(fd, buf, bufsz);
//        if (cnt < 0)
//        {
//            int e = errno;
//            free(buf);
//            close(fd);
//            unlink(pathname);
//            errno = e;
//            return NULL;
//        }
//    }
//    free(buf);

    /* set the file size to the amount of virtual space it should consume. */
    if (ftruncate(fd, vspace) < 0)
        nvms_assert_fail("ftruncate failed");

    /* Acquire the mutex that protects the open file table. It may need
     * initialization. */
    pthread_once(&nvms_file_once, &nvms_init_file);
    pthread_mutex_lock(&nvms_file_mutex);

    /* find an available slot in the open region table */
    int index;
    nvms_file *fh = NULL; // file handle
    for (index = 0; index < MAX_REGIONS; index++)
    {
        if (nvms_open_files[index] == NULL) // found a slot
        {
            /* create the open file struct */
            fh = malloc(sizeof(*fh));
            nvms_open_files[index] = fh;

            /* initialize it. Even though we know the psize and vsize, they
             * are set to zero since we did not map it in yet. */
            fh->index = index;
            fh->osfd = fd;
            fh->attach = NULL;
            fh->vsize = 0;
            pthread_mutex_unlock(&nvms_file_mutex);
            return fh;
        }
    }
    pthread_mutex_unlock(&nvms_file_mutex);

    /* error if too many open region files */
    close(fd);
    unlink(pathname);
    errno = EMFILE; // too many open files
    return NULL;
}
/**
 * This will delete a region returning its NVM to the file system. The 
 * region must not be attached to any process. The region may also be 
 * deleted via the file system. 
 * 
 * If there are any errors then errno is set and the return value is zero.
 * 
 * @param[in] pathname
 * The name of the region file to destroy.
 * 
 * @return 
 * One if successful and zero if there is an error.
 */
int nvms_destroy_region(
        const char *pathname
        )
{
    int ret = (unlink(pathname) == 0);
    return ret;
}
/**
 * This will return a handle to an existing region file. It is an error if 
 * the file is not in an NVM file system.
 * 
 * If there are any errors then errno is set and the return value is zero.
 * 
 * @param[in] pathname
 * The name of the region file to open
 * 
 * @return 
 * Open region file handle or zero on error
 */
nvms_file *nvms_open_region(
        const char *pathname // 
        )
{
    /* Open the requested file */
    int fd = open(pathname, O_RDWR);
    if (fd < 0)
        return NULL; // open set errno

    /* Acquire the mutex that protects the open file table. It may need
     * initialization. */
    pthread_once(&nvms_file_once, &nvms_init_file);
    pthread_mutex_lock(&nvms_file_mutex);

    /* find an available slot in the open region table */
    int index;
    nvms_file *fh = NULL; // file handle
    for (index = 0; index < MAX_REGIONS; index++)
    {
        if (nvms_open_files[index] == NULL)
        {
            /* create the open file struct */
            fh = malloc(sizeof(*fh));
            nvms_open_files[index] = fh;

            /* initialize it */
            fh->index = index;
            fh->osfd = fd;
            fh->attach = NULL;
            fh->vsize = 0;
            pthread_mutex_unlock(&nvms_file_mutex);
            return fh;
        }
    }
    pthread_mutex_unlock(&nvms_file_mutex);

    /* error if too many open region files */
    close(fd);
    errno = EMFILE; // too many open files
    return NULL;
}
/**
 * This will close a region file opened by nvms_open_region or nvms_create_
 * region. This will also unmap the entire region file if nvms_map_region was
 * called to map it into this process. After this returns the handle is no
 * longer usable.
 * 
 * If there are any errors then errno is set and the return value is zero.	
 * 
 * @param[in] handle
 * Open region file handle from open or create region.
 * 
 * @return  
 * One if successful and zero if there is an error.
 */
int nvms_close_region(
        nvms_file *handle
        )
{
    /* make sure this is a valid handle */
    int index = handle->index;
    if (index < 0 || index >= MAX_REGIONS || nvms_open_files[index] != handle)
    {
        /* not a valid file handle */
        errno = EINVAL;
        return 0;
    }

    /* If the region was mapped in, then unmap it. */
    if (handle->attach != NULL)
        munmap(handle->attach, handle->vsize);

    /* close the file descriptor - ignore errors */
    close(handle->osfd);

    /* release the nvms_file */
    free(handle);

    /* make the open region slot available */
    nvms_open_files[index] = 0;

    /* success */
    return 1;
}
/**
 * This writes the beginning of a region file from a volatile memory buffer
 * and returns the number of bytes written. It is an error to write beyond 
 * the end of the base extent.
 * 
 * If there are any errors then errno is set and the return value is zero. 
 * 
 * @param[in] handle
 * Open region file handle from open or create region.
 * 
 * @param[out] buffer
 * The memory location to write the data from.
 * 
 * @param[in] bytes
 * The number of bytes to write.
 * 
 * @return 
 * The number of bytes read. Zero means there is an error.
 */
size_t nvms_write_region(
        nvms_file *handle,
        void *buffer,
        size_t bytes
        )
{
    /* make sure this is a valid handle */
    int index = handle->index;
    if (index < 0 || index >= MAX_REGIONS || nvms_open_files[index] != handle)
    {
        /* not a valid file handle */
        errno = EINVAL;
        return 0;
    }

    /* do the write */
    if (pwrite(handle->osfd, buffer, bytes, (off_t)0) < 0)
        return 0;
    return 1;
}
/**
 * This reads the beginning of a region file into a volatile memory buffer 
 * and returns the number of bytes read. This could be less than the amount
 * requested if the file is shorter.
 * 
 * If there are any errors then errno is set and the return value is zero. 
 * Note that reading a zero length file will return zero bytes read and 
 * errno will still be zero.
 * 
 * @param[in] handle
 * Open region file handle from open or create region.
 * 
 * @param[out] buffer
 * The memory location to read the data into.
 * 
 * @param[in] bytes
 * The number of bytes to read.
 * 
 * @return 
 * The number of bytes read. Zero means there is an error or zero length
 * file
 */
size_t nvms_read_region(
        nvms_file *handle,
        void *buffer,
        size_t bytes
        )
{
    /* make sure this is a valid handle */
    int index = handle->index;
    if (index < 0 || index >= MAX_REGIONS || nvms_open_files[index] != handle)
    {
        /* not a valid file handle */
        errno = EINVAL;
        return 0;
    }

    /* do the read */
    errno = 0; // just in case the file is zero length
    ssize_t n = pread(handle->osfd, buffer, bytes, (off_t)0);
    if (n < 0)
        return 0;
    return n;
}
/**
 * Acquire an advisory lock on the region file. If the process already owns
 * a lock on the region then convert the lock mode of the existing lock. A 
 * lock conflict is returned as an EACCES error rather than blocking for 
 * the conflict to be resolved. The lock is owned by the calling process on
 * behalf of all threads in the process.
 * 
 * If there are any errors then errno is set and the return value is zero.
 * 
 * @param[in] handle
 * Open region file handle from open or create region.
 * 
 * @param[in] exclusive
 * If true, lock exclusive else lock shared
 * 
 * @return 
 * One if successful and zero if there is an error such as lock conflict.
 */
int nvms_lock_region(
        nvms_file *handle,
        int exclusive
        )
{
    /* make sure this is a valid handle */
    int index = handle->index;
    if (index < 0 || index >= MAX_REGIONS || nvms_open_files[index] != handle)
    {
        /* not a valid file handle */
        errno = EINVAL;
        return 0;
    }

    /* get a  lock on the file */
    struct flock lock;
    lock.l_type = (exclusive ? F_WRLCK : F_RDLCK);
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0x7fffffff; // any len would work, this one is safe
    int result = fcntl(handle->osfd, F_SETLK, &lock);
    return result == 0;
}
/**
 * This read/write maps an open region file into the current process and 
 * returns the virtual address where it is mapped. The attach parameter is 
 * the virtual address to attach at. An error is returned if the file cannot
 * be mapped at this address.
 * 
 * The vspace parameter is the same value passed to nvms_create_region. 
 * This much process virtual address space must be  reserved for potential 
 * extension of the file. However, only the first pspace bytes should 
 * actually appear in the address space. Any access to the virtual 
 * addresses between pspace and vspace must result in a memory fault - not 
 * NVM allocation.
 * 
 * The other extents of the region file will be mapped by calls to 
 * nvms_map_extent.
 * 
 * If there are any errors then errno is set and the return value is zero.
 * 
 * @param[in] handle
 * Open region file handle from open or create region.
 * 
 * @param[in] attach
 * This is the virtual address where the region file will be mapped 
 * into the calling process. It may be zero to let the OS choose the 
 * address.
 * 
 * @param[in] vspace
 * This is the amount of virtual address space to be consumed by the region.
 * 
 * @param[in] pspace
 * This is the current size of the base extent, and the amount of memory
 * to be mapped.
 * 
 * @return 
 * Address attached at if successful, or NULL if there is an error.
 */
void *nvms_map_region(
        nvms_file *fh,
        void *attach,
        size_t vspace,
        size_t pspace
        )
{
    /* make sure this is a valid handle */
    int index = fh->index;
    if (index < 0 || index >= MAX_REGIONS || nvms_open_files[index] != fh)
    {
        /* not a valid file handle */
        errno = EINVAL;
        return 0;
    }

    /* Save vsize and attach before mmap to ensure it is valid if we close.
     * Setting them first can result in an unnecessary munmap, unless we are
     * letting the OS choose the virtual address. */
    fh->vsize = vspace;
    fh->attach = attach;

    /* Map in the entire virtual address space to reserve it for this file.
     * This makes the entire region mapped into the address space. However
     * just the base extent will be accessible until nvms_map_range is called
     * to make other extents accessible. */
    int flags = attach ? MAP_SHARED | MAP_FIXED : MAP_SHARED;

    uint8_t *base = mmap(attach, vspace, PROT_READ | PROT_WRITE,
                flags, fh->osfd, 0);
    if (base == MAP_FAILED)
        return 0;
    if (attach && base != attach)
        nvms_assert_fail("fixed mapping not fixed");

    /* Only allow access to base extent for now. */
    if (mprotect(base+pspace, vspace-pspace, PROT_NONE))
        return 0;

    /* Save the real attach address in case attach was zero. Note that thread
     * death between the mmap call and here could result in munmap not being
     * called even though the mmap call succeeded. */
    fh->attach = base;

    return base;
}

/**
 * Return the virtual address where a region file was mapped. NULL is
 * returned if the region is not mapped, or if the file handle is bad.
 * A bad file handle will set errno to EINVAL.
 *
 * @param[in] handle
 * Open region file handle from open or create region.
 *
 * @return
 * Address attached at if successful, or NULL if there is an error.
 */
void *nvms_region_addr(
        nvms_file *handle
        )
{
    /* make sure this is a valid handle */
    int idx = handle->index;
    if (idx < 0 || idx >= MAX_REGIONS || nvms_open_files[idx] != handle)
    {
        /* not a valid file handle */
        errno = EINVAL;
        return 0;
    }

    /* return the address */
    errno = 0;
    return handle->attach;
}

/**
 * This adds NVM to an open region file. This must be done before the
 * address range can be mapped into any application process. The NVM to
 * add must be within the vspace parameter passed to nvms_create_region.
 * 
 * If there are any errors then errno is set and the return value is zero.
 * 
 * @param handle
 * Open region file handle from open or create region.
 * 
 * @param offset
 * The offset into the region file of the first NVM byte to add.
 * 
 * @param len
 * The number of bytes to add to the region file.
 * 
 * @return 
 * One if successful and zero if it fails.
 */
int nvms_alloc_range(
        nvms_file *handle,
        off_t offset,
        size_t len
        )
{
    /* make sure this is a valid handle */
    int index = handle->index;
    if (index < 0 || index >= MAX_REGIONS || nvms_open_files[index] != handle)
    {
        /* not a valid file handle */
        errno = EINVAL;
        return 0;
    }

    /* Ensure the range is contained within the file */
    if (offset + len > handle->vsize)
    {
        /* not a valid range */
        errno = EINVAL;
        return 0;
    }

    /* Allocate NVM bytes for the range. Fail if allocation fails. */
    if ((errno = posix_fallocate(handle->osfd, offset, len)) < 0)
        return 0;

    /* Zero the newly allocated memory just in case it had been used before
     * as part of this file. Note that this will always round up to the next
     * megabyte. */
    size_t bufsz = 1024 * 1024;
    void *buf = malloc(bufsz);
    memset(buf, 0, bufsz);
    size_t cleared;
    for (cleared = 0; cleared < len; cleared += bufsz)
    {
        /* do not write beyond the allocated space */
        if (cleared + bufsz > len)
            bufsz = len-cleared;

        int cnt = pwrite(handle->osfd, buf, bufsz, offset + cleared);
        if (cnt < 0)
        {
            int e = errno;
            free(buf);
            errno = e;
            return 0;
        }
    }
    free(buf);

    return 1;
}
/**
 * This maps a range of NVM into the process address space. The NVM must
 * be previously added to the file by nvms_alloc_range. The base extent
 * must have been mapped by nvm_map_region.
 * 
 * It would be an indication of corruption if there was no NVM allocated
 * to the specified portion of the region file. It would be an error if  
 * offset and offset+len are not within the vspace parameter passed to 
 * nvms_map_region,
 * 
 * If there are any errors then errno is set and the return value is zero.
 * 
 * @param[in] handle
 * Open region file handle from open or create region.
 * 
 * @param[in] offset
 * The offset into the region file of the first NVM byte to map.
 * 
 * @param[in] len
 * The amount of NVM to add to the current process address space.
 * 
 * @return
 * The virtual address of the memory or zero if there is an error. 
 */
void *nvms_map_range(
        nvms_file *fh,
        size_t offset,
        size_t len
        )
{
    /* make sure this is a valid handle */
    int index = fh->index;
    if (index < 0 || index >= MAX_REGIONS || nvms_open_files[index] != fh)
    {
        /* not a valid file handle */
        errno = EINVAL;
        return 0;
    }

    /* Ensure the range is contained within the file */
    if (offset + len > fh->vsize)
    {
        /* not a valid range */
        errno = EINVAL;
        return 0;
    }

#if 0 //make this true if mprotect alone does not make the memory available
    /* map in the range */
    if (mmap(fh->attach + offset, len, PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_FIXED, fh->osfd, (off_t)offset) == MAP_FAILED)
        return 0; // failed
#else
    /* Since the entire virtual address space of the region was mapped at
     * attach, all that is needed now is to allow access to the extent. */
    if (mprotect(fh->attach + offset, len, PROT_READ | PROT_WRITE))
        return 0; // failed
#endif

    return fh->attach + offset;
}
/**
 * This unmaps a range of NVM from the current process. The address range
 * must be within the vspace parameter passed to nvms_create_region. It is
 * not an error to unmap a range that is not mapped. This does not make
 * the virtual address space available for other region mappings.
 * 
 * @param handle
 * Open region file handle from open or create region.
 * 
 * @param offset
 * The offset into the region file of the first NVM byte to unmap.
 * 
 * @param len
 * The amount of NVM to remove from the current process address space.
 * 
 * @return 
 * The virtual address of the memory or zero if there is an error. 
 */
void *nvms_unmap_range(
        nvms_file *fh,
        size_t offset,
        size_t len
        )
{
    /* make sure this is a valid handle */
    int index = fh->index;
    if (index < 0 || index >= MAX_REGIONS || nvms_open_files[index] != fh)
    {
        /* not a valid file handle */
        errno = EINVAL;
        return 0;
    }

    /* Ensure the range is contained within the file */
    if (offset + len > fh->vsize)
    {
        /* not a valid range */
        errno = EINVAL;
        return 0;
    }

    /* Disallow access to the range so space cannot be reallocated by a
     * wild store. */
    if (mprotect(fh->attach + offset, len, PROT_NONE))
        return 0; // failed

    return fh->attach + offset;
}

/**
 * This unmaps the entire region from the current process. The virtual
 * address range allocated to the region becomes available for other
 * regions. It is not an error to unmap a region that is not mapped.
 *
 * @param handle
 * Open region file handle from open or create region.
 *
 * @return
 * If successful one. Zero if there is an error.
 */
int nvms_unmap_region(
    nvms_file *fh
    )
{
    /* make sure this is a valid handle */
    int idx = fh->index;
    if (idx < 0 || idx >= MAX_REGIONS || nvms_open_files[idx] != fh)
    {
        /* not a valid file handle */
        errno = EINVAL;
        return 0;
    }

    /* return success if already unmapped */
    if (!fh->attach)
        return 1;

    /* unmap the region */
    if (munmap(fh->attach, fh->vsize))
        return 0; // failed

    /* no longer mapped */
    fh->attach = 0;
    fh->vsize = 0;

    return 1;
}

/**
 * This frees NVM from an open region file. The address range must not be
 * mapped into any application process when this is called.
 * 
 * If there are any errors then errno is set and the return value is zero.
 * 
 * @param handle
 * Open region file handle from open or create region.
 * 
 * @param offset
 * The offset into the region file of the first NVM byte to free.
 * 
 * @param len
 * The number of bytes to free from the region file.
 * 
 * @return 
 * One if successful and zero if it fails.
 */
int nvms_free_range(
        nvms_file *handle,
        off_t offset,
        size_t len
        )
{
    /* make sure this is a valid handle */
    int index = handle->index;
    if (index < 0 || index >= MAX_REGIONS || nvms_open_files[index] != handle)
    {
        /* not a valid file handle */
        errno = EINVAL;
        return 0;
    }

    /* Ensure the range is contained within the file */
    if (offset + len > handle->vsize)
    {
        /* not a valid range */
        errno = EINVAL;
        return 0;
    }

    /* Create a hole in the file for the given range. */
    //TODO when running Linix 2.6.38 or better uncomment the following:
    //    if (fallocate(handle->osfd, 
    //            FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
    //            offset, len))
    //        return 0;
    return 1;
}

/**
 * This returns a 64 bit number to use as a unique id for ensuring the
 * same region is not attached twice at the same time. It needs to be
 * reasonably unique within the server. For example it could be the
 * current process id in the upper 32 bits and microseconds since boot
 * in the lower 32 bits.
 * 
 * @return 
 * A server unique id.
 */
uint64_t nvms_unique_id()
{
    struct timeval now;
    gettimeofday(&now, NULL);

    pid_t pid = getpid();
    uint64_t id = (nvms_utime() & 0xffffffff) | ((uint64_t)pid << 32);
    return id;
}
