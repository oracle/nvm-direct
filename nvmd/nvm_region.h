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
      nvm_region.h - Non-Volatile Memory region management

    DESCRIPTION\n
      This header defines the application interface for managing NVM region
      files and mapping them into an application process.

    NOTES\n
      An NVM region is a file in an NVM file system that is mapped into a
      process's virtual address space for direct load/store access to the NVM
      backing the file. An NVM region is generally a sparse file that contains
      all the related data for a single application. An application can map
      more than one NVM region, but each region is independently managed.

      The application must create a root struct for the region to make it
      fully formed and suitable for later mapping into another process. The
      USID that identifies the region as compatible with this library is not
      set until a root pointer is set.

      An NVM region is created with a virtual size that defines the amount of
      virtual address space it consumes when mapped into a process. The virtual
      size is typically much larger than the amount of NVM that will ever be in
      the file. Creation also takes a physical size for the base extent of NVM
      which starts at byte 0 of the file. Additional extents can be added or
      deleted from the region as needed. The base extent cannot be deleted.

      An extent can be managed as a heap by the NVM library (see nvm_heap.h) or
      it can be managed by the application. An application managed extent is
      initialized to zero when added to the region. After that it is up to the
      application to put data in it and deal with any corruption. The base
      extent starts at offset zero in the region and is always heap managed.

      When an NVM region is mapped into a process, a region descriptor is
      returned to identify the region to NVM library functions. This is a
      small integer much like an open file descriptor.

      NVM is attached to a single server since it is connected as memory. This
      means that the NVM file system is local to that server, and its region
      files can only be mapped into processes executing on that server.

 */
#ifndef NVM_REGION_H
#define	NVM_REGION_H

#include <sys/types.h>

#ifdef	__cplusplus
extern "C"
{
#endif

    /* PUBLIC TYPES AND CONSTANTS */
    /**
     * This type is used to hold a region descriptor. A region is identified by
     * a small integer in a manner similar to an open file descriptor. Zero is
     * not a valid region descriptor. An application can choose the descriptor
     * for each region it attaches or it can let the NVM library choose an
     * unused region descriptor.  An application that attaches a small number
     * of regions for different purposes could have define constants for each
     * region descriptor.
     */
    typedef uint32_t nvm_desc;
    /**\brief Status of an NVM region
     *
     * This struct describes the current status of a region that is mapped
     * into a process. It is used for returning status to the application from
     * nvm_query_region.
     */
#ifdef NVM_EXT
    struct nvm_region_stat
    {
        /**
         * This is the region descriptor passed to nvm_query_region
         */
        nvm_desc desc;

        /**
         * This is the region name stored in NVM region header. It is not the
         * file name.
         */
        const char ^name;

        /**
         * This is the virtual address of the first byte of the base extent
         */
        void ^base;

        /**
         * This is the amount of virtual address space that the region consumes
         * when it is mapped into a process. This puts a limit on where new
         * extents can be placed.
         */
        size_t vsize;

        /**
         * This is the amount of physical non-volatile memory currently consumed
         * by the region. This is the sum of the sizes of all the extents.
         */
        size_t psize;

        /**
         * This is the number of extents currently allocated in the region. The
         * count includes both heap managed and application managed extents.
         */
        uint32_t extent_cnt;

        /**
         * This is the number of times this region has been attached. It can
         * be used to recognize the first access to an object after the
         * region is attached by a new application.
         */
        uint64_t attach_cnt;

        /**
         * This is the NVM address of the root heap. The root heap is the base
         * heap of the base extent. This is where the NVM library allocates its
         * metadata.
         */
        nvm_heap ^rootheap;

        /**
         * This is the application's root object. All allocated space should be
         * reachable by following pointers from this object. If the root object
         * has not yet been set this will be null, and the nvm_region type_usid
         * will still be zero to prevent attaching to the region.
         */
        void ^rootobject;
    };
#else
    struct nvm_region_stat
    {
        /**
         * This is the region descriptor passed to nvm_query_region
         */
        nvm_desc desc;

        /**
         * This is the region name stored in NVM region header. It is not the
         * file name.
         */
        const char *name;

        /**
         * This is the virtual address of the first byte of the base extent
         */
        void *base;

        /**
         * This is the amount of virtual address space that the region consumes
         * when it is mapped into a process. This puts a limit on where new
         * extents can be placed.
         */
        size_t vsize;

        /**
         * This is the amount of physical non-volatile memory currently consumed
         * by the region. This is the sum of the sizes of all the extents.
         */
        size_t psize;

        /**
         * This is the number of extents currently allocated in the region. The
         * count includes both heap managed and application managed extents.
         */
        uint32_t extent_cnt;

        /**
         * This is the number of times this region has been attached. It can
         * be used to recognize the first access to an object after the
         * region is attached by a new application.
         */
        uint64_t attach_cnt;

        /**
         * This is the NVM address of the root heap. The root heap is the base
         * heap of the base extent. This is where the NVM library allocates its
         * metadata.
         */
        nvm_heap *rootheap;

        /**
         * This is the application's root object. All allocated space should be
         * reachable by following pointers from this object. If the root object
         * has not yet been set this will be null, and the nvm_region type_usid
         * will still be zero to prevent attaching to the region.
         */
        void *rootobject;
    };
#endif //NVM_EXT
    typedef struct nvm_region_stat nvm_region_stat;
    /**\brief Status of one extent of an NVM region
     *
     *This struct describes the status of one extent of an NVM region.
     * An ordered array of these can be returned by nvm_query_extents.
     */
#ifdef NVM_EXT
    struct nvm_extent_stat
    {
        /**
         * This is the address of the NVM region containing the extent
         */
        nvm_desc region;

        /**
         * This is the extent address
         */
        void ^extent;

        /**
         * Physical size of the extent in bytes
         */
        size_t psize;

        /**
         * This is the address of the base heap if this is a heap managed
         * extent. This is null if it is an application managed extent.
         */
        nvm_heap ^heap;
    };
#else
    struct nvm_extent_stat
    {
        /**
         * This is the address of the NVM region containing the extent
         */
        nvm_desc region;

        /**
         * This is the extent address
         */
        void *extent;

        /**
         * Physical size of the extent in bytes
         */
        size_t psize;

        /**
         * This is the address of the base heap if this is a heap managed
         * extent. This is null if it is an application managed extent.
         */
        nvm_heap *heap;
    };
#endif //NVM_EXT
    typedef struct nvm_extent_stat nvm_extent_stat;


    /* EXPORT FUNCTIONS */

    /**
     * This returns the minimum NVM overhead consumed by NVM direct for its
     * metadata. This includes the base heap overhead, one transaction table,
     * and the configured number of upgrade mutexes. The extents argument
     * adds the metadata overhead for supporting that many additional extents
     * beyond the base extent. The value includes space for a 64 byte root
     * struct defined by the application and allocated from the base heap.
     * However the root object does not have to be in the base heap, and is
     * often larger than 64 bytes.
     *
     * The size is rounded up to the next page boundary so that it is a valid
     * physical size for creating an NVM region. The physical size passed to
     * nvm_create_region must be at least this large or creation may fail.
     *
     * @param[in] extents
     * The maximum number of additional extents that this much metadata would
     * support. This can be zero if the application will not create additional
     * extents beyond the base extent.
     *
     * @return
     * The return size is the overhead in bytes that must be included in the
     * physical space parameter used to create a region. Generally the region
     * should be created with more physical space to support the application
     * data allocated from the base heap.
     */
    size_t nvm_overhead(uint32_t extents);

    /**
     * This returns the page size in bytes that the NVM Direct library expects.
     * All virtual addresses and extent sizes must be a multiple of this value.
     * This is controlled by a configuration parameter, so it may be different
     * than the normal OS page size.
     */
    size_t nvm_page_size(void);

    /**
     * This function creates a region file in an NVM file system, maps it into
     * the creating process's address space, and initializes the transaction
     * and heap mechanisms for application use. A region descriptor is
     * returned. If the region argument is zero then the library will choose
     * an unused descriptor. If a non-zero region descriptor is passed in then
     * it will be used. The application must ensure it does not attempt to use
     * the same region descriptor twice. The caller will need to call
     * nvm_set_root_object() to complete initialization making the region valid
     * for reattaching.
     *
     * The region descriptor is needed for beginning transactions in the region
     * and for some of the other library functions.
     *
     * It is common for the attach address to be zero so that the OS can choose
     * an available virtual address.
     *
     * All application calls to nvm_usid_register must be done before this is
     * called.
     *
     * This cannot be called in a transaction since it would have to be for
     * another region.
     *
     * If there are any errors then errno is set and the return value is zero.
     *
     * @param[in] region
     * If this is zero then an unused region descriptor will be chosen.
     * Otherwise this region descriptor will be used. It must not be in use
     * already by this application.
     *
     * @param[in] pathname
     * This is the name of the region file to create. It must be a name in
     * an NVM file system.
     *
     * @param[in] regionname
     * This is the name to store in the region. It is reported to users with
     * some messages. It does not change if the region file is renamed or
     * copied. It is not the file name. It is limited to 63 characters.
     *
     * @param[in] attach
     * This is the virtual address where the new region file will be mapped
     * into the process. A value of zero can be passed to let the OS choose
     * an address.
     *
     * @param[in] vspace
     * This is the amount of virtual address space that the region will consume
     * when it is mapped into a process. This needs to be large enough to allow
     * addition of physical NVM while running.
     *
     * @param[in] pspace
     * This is the initial amount of physical NVM to allocate to the base
     * extent. This is used to create the rootheap and allocate metadata used
     * by the NVM library.
     *
     * @param[in] mode
     * This is the same as the mode parameter to the Posix creat system call.
     * See the man page open(2) for the bit definitions.
     *
     * @return
     * The return value is the region descriptor for the new region. It is used
     * as an opaque handle to the region for several NVM library calls. If zero
     * is returned then the call failed and errno contains an error number.
     *
     * @par Errors:
     *
     * - EACCES\n
     * The requested access to the region file is not allowed, or search
     * permission is denied for one of the directories in the path  prefix of
     * pathname,  or write access to the parent directory is not  allowed.
     * - EEXIST \n
     * pathname already exists.
     * - EFAULT \n
     * pathname points outside the accessible address space.
     * - EISDIR \n
     * pathname refers to a directory
     * - ELOOP  \n
     * Too many symbolic links were encountered in resolving  pathname.
     * - EMFILE \n
     * The process already has the maximum number of files open.
     * - ENAMETOOLONG\n
     * pathname was too long.
     * -ENFILE \n
     * The  system  limit  on  the  total number of open files has been reached.
     * - ENOMEM \n
     * Insufficient kernel memory was available, or the process's maximum
     * number of mappings would have been exceeded.
     * - ENOSPC \n
     * The NVM file system has no room for the new region file.
     * - ENOTDIR\n
     * A  component  used as a directory in pathname is not, in fact,
     * a directory
     * - EROFS  \n
     * pathname refers to a file on a read-only filesystem.
     * - ENXIO\n
     * pathname refers to a file system that is not an NVM file system.
     * - EINVAL\n
     * Attach or vspace are too large, or not aligned on a page boundary.
     * Also reported if region descriptor is already in use.
     */
#ifdef NVM_EXT
    nvm_desc nvm_create_region(
        nvm_desc region,
        const char *pathname,
        const char *regionname,
        void *attach,
        size_t vspace,
        size_t pspace,
        mode_t mode
        );
#else
    nvm_desc nvm_create_region(
        nvm_desc region,
        const char *pathname,
        const char *regionname,
        void *attach,
        size_t vspace,
        size_t pspace,
        mode_t mode
        );
#endif //NVM_EXT

    /**
     * All the application data in an NVM region must be reachable through NVM
     * pointers starting from the root object. If there are any application NVM
     * variables that need to be static or global they will be fields in the
     * root object.
     *
     * After creating a region there is no root object so the region is still
     * marked as invalid. To make the new region valid this function must be
     * called to save the root object. The root object is typically allocated
     * from the root heap immediately after the region is successfully created.
     * The allocation and initialization of the new root object must be
     * committed before this is called. There may not be any active
     * transactions in the region when this is called.
     *
     * The root object must have a USID defined for it so that attach can
     * validate the region can be understood by the attaching region.
     *
     * If there are any errors then errno is set and the return value is zero.
     *
     * @param[in] desc
     * This must be a region descriptor returned by nvm_create_region, and no
     * root object has been set
     *
     * @param[in] rootobj
     * This must point to an NVM allocation returned by nvm_alloc, and the
     * allocating transaction has committed.
     *
     * @return
     * 0 is returned on error and 1 is returned on success.
     *
     * @par Errors:
     */
#ifdef NVM_EXT
    int nvm_set_root_object(
        nvm_desc desc,
        void ^rootobj
        );
#else
    int nvm_set_root_object(
        nvm_desc desc,
        void *rootobj
        );
#endif //NVM_EXT

    /**
     * There may be circumstances where a new object needs to become the root
     * object for the region. This must be done in a transaction to ensure the
     * region remains valid. The transaction must create the new root object
     * and pass it to nvm_new_root_object() before committing. The transaction
     * will usually delete the old root object either before or after
     * nvm_new_root_object() is called, but in the same transaction. The return
     * value is the old root object.
     *
     * This operates on the region of the current transaction
     *
     * The root object must have a USID defined for it so that attach can
     * validate the region can be understood by the attaching region.
     *
     * If there are any errors then errno is set and the return value is zero.
     *
     * @param[in] rootobj
     * This is an NVM pointer to the new root object.
     *
     * @return
     * The old root object pointer is returned.
     *
     * @par Errors
     */
#ifdef NVM_EXT
    void ^nvm_new_root_object@(
        void ^rootobj
        );
#else
    void *nvm_new_root_object(
        void *rootobj
        );
#endif //NVM_EXT

    /**
     * This will delete a region returning its NVM to the file system. The
     * region must not be attached to any process. The region may also be
     * deleted via the file system.
     *
     * If there are any errors then errno is set and the return value is zero.
     *
     * @param[in] name
     * name of the region file to destroy
     *
     * @return
     * 1 if successful, 0 on error
     *
     * @par Errors:
     */
    int nvm_destroy_region(
        const char *name
        );

    /**
     * Attach a Non-Volatile Memory region to this application and return a
     * region descriptor. If the region argument is zero then the library will
     * choose an unused descriptor. If a non-zero region descriptor is passed
     * in then it will be used. The application must ensure it does not attempt
     * to use the same region descriptor twice.
     *
     * Only one process at a time may attach a region, and only one attach can
     * be done to the same region. The returned region descriptor is valid for
     * all threads in the application process.
     *
     * If the last detach was not clean, recovery will be run to roll back any
     * active transactions and complete any committing transactions. This must
     * not be called in a transaction.
     *
     * All application calls to nvm_usid_register must be done before this is
     * called.
     *
     * This cannot be called in a transaction since it would have to be for
     * another region.
     *
     * The region descriptor is used for beginning transactions in the region
     * and for a parameter to other library functions that operate on a region.
     *
     * If there are any errors then errno is set and the return value is zero.
     *
     * @param[in] desc
     * If this is zero then an unused region descriptor will be chosen.
     * Otherwise this region descriptor will be used. It must not be in use
     * already by this application.
     *
     * @param[in] pathname
     * This is the name of the region file to attach
     *
     * @param[in] attach
     * This is the virtual address where the region file will be mapped
     * into the process.
     *
     * @return
     * The return value is a pointer to the region. It is used as an
     * opaque handle to the region for several NVM library calls.
     *
     * @par Errors:
     *
     */
#ifdef NVM_EXT
    nvm_desc nvm_attach_region(
        nvm_desc desc,
        const char *pathname,
        void *attach
        );
#else
    nvm_desc nvm_attach_region(
        nvm_desc desc,
        const char *pathname,
        void *attach
        );
#endif //NVM_EXT

    /**
     * This returns the status of a region that is mapped into the current
     * process. Note that the returned status might not be current if some
     * other thread is changing the region.
     *
     * If called within a transaction then the region may be zero to use the
     * region owning the current transaction.
     *
     * If there are any errors then errno is set and the return value is zero.
     *
     * @param[in] desc
     * This is the NVM region descriptor returned by nvm_attach_region or
     * nvm_create_region.
     *
     * @param[out] stat
     * This points to the buffer where the results are stored
     *
     * @return
     * 0 is returned on error and 1 is returned on success.
     *
     * @par Errors
     */
    int nvm_query_region(
        nvm_desc desc,
        nvm_region_stat *stat
        );

    /**
     * This function cleanly detaches this NVM region from this process. The
     * contents of the region are not changed. It is an error to detach while
     * there are transactions active or committing in the application.
     *
     * It is not really necessary to detach a region. Process exit will detach.
     *
     * If there are any errors then errno is set and the return value is zero.
     *
     * @param[in] desc
     * This is the NVM region descriptor returned by nvm_attach_region or
     * nvm_create_region.
     *
     * @return
     * 0 is returned on error and 1 is returned on success.
     *
     * @par Errors
     */
#ifdef NVM_EXT
    int nvm_detach_region(
        nvm_desc desc
        );
#else
    int nvm_detach_region(
        nvm_desc desc
        );
#endif //NVM_EXT

    /**
     * This function adds a new application managed extent of NVM to a region
     * that is currently attached. This must be called within a transaction
     * that saves a pointer to the extent in some application persistent
     * struct. If the transaction rolls back the extent add will also rollback.
     * The return value is the actual extent address. This will be the
     * same address as the extent address passed in.
     *
     * The address range for the new extent must not overlap any existing
     * extent. It must be within the NVM virtual address space reserved for the
     * region. It must be page aligned and the size must be a multiple of a
     * page size.
     *
     * This operates on the region of the current transaction
     *
     * If there are any errors then errno is set and the return value is zero.
     *
     * @param[in] extent
     * This is the desired virtual address for the new extent.
     *
     * @param[in] psize
     * This is the amount of physical NVM to allocate for the extent in bytes.
     *
     * @return
     * The virtual address of the extent is returned. Zero is returned if there
     * is an error.
     *
     * @par Errors:
     */
#ifdef NVM_EXT
void ^nvm_add_extent@(
        void ^extent,
        size_t psize
        );
#else
    void *nvm_add_extent(
        void *extent,
        size_t psize
        );
#endif //NVM_EXT

    /**
     * This function changes the size of an existing application managed
     * extent. This must be called within a transaction. If the transaction
     * rolls back the resize will also rollback.
     *
     * The address range for the extent must not overlap any existing extent.
     * It must be within the NVM virtual address space reserved for the region.
     * The new size must be a multiple of a page size and not zero. This must
     * not be a heap managed extent.
     *
     * This operates on the region of the current transaction
     *
     * If there are any errors then errno is set and the return value is zero.
     *
     * @param[in] extent
     * This is the virtual address of the extent returned by nvm_add_extent.
     *
     * @param[in] psize
     * This is the new physical size of the extent in bytes. It may not be 0.
     *
     * @return
     * The virtual address of the extent is returned. Zero is returned if there
     * is an error.
     *
     * @par Errors:
     */
#ifdef NVM_EXT
    int nvm_resize_extent@(
        void ^extent,
        size_t psize
        );
#else
    int nvm_resize_extent(
        void *extent,
        size_t psize
        );
#endif //NVM_EXT

    /**
     * This removes an extent from an NVM region returning its space to the
     * NVM file system. It must be called within a transaction. The extent is
     * deleted when the transaction commits. The extent must be application
     * managed. Call nvm_delete_heap to remove a heap managed extent. The
     * contents of the extent are ignored so this will work even if the extent
     * is corrupt.  The application is responsible for removing any pointers
     * to the extent as part of the transaction. The base extent cannot be
     * removed.
     *
     * This operates on the region of the current transaction
     *
     * If there are any errors then errno is set and the return value is zero.
     *
     * @param[in] extent
     * This is the virtual address of the extent to delete.
     *
     * @return
     * 0 is returned on error and 1 is returned on success.
     *
     * @par Errors:
     */
#ifdef NVM_EXT
    int nvm_remove_extent@(
        void ^extent
        );
#else
    int nvm_remove_extent(
        void *extent
        );
#endif //NVM_EXT

    /**
     * This returns the status of one or more extents from a region. Extents
     * are ordered by their address within the region.  The base extent is
     * number zero. To query all extents in a region, call nvm_query_region to
     * find the number of extents. Call nvm_query_extents with extent == 0 and
     * count == extent_cnt from the nvm_region_stat. To query extent 42 pass
     * extent==42 and count==1. Note that adding or deleting an extent can
     * change the index to other extents since the index is based on the
     * address order of the extent relative to all existing extents, and
     * extents can be added or deleted between other extents. Note that the
     * returned status might not be current if some other thread is changing
     * the region.
     *
     * If called within a transaction then the region may be zero to use the
     * region owning the current transaction.
     *
     * If there are any errors then errno is set and the return value is zero.
     * It is not an error to query extents that do not exist. The data returned
     * is simply zero.
     *
     * @param[in] region
     * This is the NVM region descriptor returned by nvm_attach_region or
     * nvm_create_region for the region to query.
     *
     * @param[in] extent
     * This is the index of the first extent to query in the ordered array
     * of extents.
     *
     * @param[in] count
     * This is the number of extents to query.
     *
     * @param[out] stat
     * This is the buffer to store the statuses of the extents.
     *
     * @return
     * 0 is returned on error and 1 is returned on success.
     *
     * @par Errors:
     */
#ifdef NVM_EXT
    int nvm_query_extents(
        nvm_desc region,
        int extent, // index of first extent to report
        int count, // number of extents to report
        nvm_extent_stat stat[] // array of count statuses to fill in
        );
#else
    int nvm_query_extents(
        nvm_desc region,
        int extent, // index of first extent to report
        int count, // number of extents to report
        nvm_extent_stat stat[] // array of count statuses to fill in
        );
#endif //NVM_EXT

#ifdef	__cplusplus
}
#endif

#endif	/* NVM_REGION_H */

