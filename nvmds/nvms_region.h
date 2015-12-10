/*
Copyright (c) 2015, 2015, Oracle and/or its affiliates. All rights reserved.

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
      nvms_region.h - Non-Volatile Memory Services for region file management

    DESCRIPTION\n
      These services are for managing region files that contain the NVM managed 
      by the library. A region file can be sparse with multiple extents of NVM. 
      On Linux this will be implemented by OS calls to an NVM file system.

 */
#ifndef NVMS_REGION_H
#define	NVMS_REGION_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef	__cplusplus
extern "C"
{
#endif
    /**
     * An nvms_file holds the data about an open region file. It is allocated 
     * in process local memory by the service library.  A pointer to an nvms_
     * file is used as an opaque handle to an open file. 
     */
    typedef struct nvms_file nvms_file;

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
        );

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
        );

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
        );

    /**
     * This will close a region file opened by nvms_open_region or nvms_create_
     * region.  This will also unmap the entire region file if nvms_map_region
     * was called to map it into this process. After this returns the handle is
     * no longer usable.
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
        );

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
        );

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
        );

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
        );

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
     * nvms_map_range.
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
        nvms_file *handle,
        void *attach,
        size_t vspace,
        size_t pspace
        );

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
            );

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
        );

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
        );

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
        nvms_file *handle,
        size_t offset,
        size_t len
        );

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
        nvms_file *handle,
        size_t offset,
        size_t len
        );

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
        nvms_file *handle
        );

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
    uint64_t nvms_unique_id();
#ifdef	__cplusplus
}
#endif

#endif	/* NVMS_REGION_H */

