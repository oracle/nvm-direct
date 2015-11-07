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
      nvm_region.c - Non-Volatile Memory region management definitions

    DESCRIPTION\n
      This contains the code to manage region files and map them into a process

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
#define NVM_REGION_C
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include "nvm.h"
#include "nvm_data0.h"
#include "nvm_region0.h"
#include "nvm_transaction0.h"
#include "nvms_region.h"
#include "nvms_misc.h"
#include "nvm_heap0.h"
#include "nvm_locks0.h"


#ifdef NVM_EXT
static nvm_desc nvm_map_region(
        const char *pathname,
        nvms_file *handle,
        void *attach,
        nvm_desc desc,
        int create
        );
#else
static nvm_desc nvm_map_region(
        const char *pathname,
        nvms_file *handle,
        void *attach,
        nvm_desc desc,
        int create
        );
#endif //NVM_EXT

static void nvm_unmap_region(nvm_desc desc);

#ifdef NVM_EXT
#else
/**
 * replace precompiler check for size matching
 */
static void nvm_region_sizeof_check()
{
    if (sizeof(nvm_extent) != shapeof(nvm_extent)->size)
        nvms_assert_fail("nvm_extent size wrong");
    if (sizeof(nvm_region) != shapeof(nvm_region)->size)
        nvms_assert_fail("nvm_region size wrong");
}
#endif //NVM_EXT
/**
 * Check a virtual address or length for being properly aligned
 */
static int nvm_align_check(uint64_t val)
{
    static uint64_t align = 0;

    /* The first call gets the alignment value from the service layer */
    if (align == 0)
    {
        nvm_app_data *ad = nvm_get_app_data();
        align = ad->params.extent_align;
    }

    /* return if good alignment */
    if (val % align == 0)
        return 1;
    else
        return 0;
}
#ifdef NVM_EXT
inline static int nvm_addr_align(void ^adr)
{
    return nvm_align_check((uint64_t)adr);
}
#else
inline static int nvm_addr_align(void *adr)
{
    return nvm_align_check((uint64_t)adr);
}
#endif //NVM_EXT

/*--------------------------- nvm_create_region -----------------------------*/
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
 * The virtual address for attach must be non-zero, page aligned, and not in 
 * use. It is an error if this address cannot be used in mmap.
 * 
 * All application calls to nvm_usid_register must be done before this is 
 * called.
 * 
 * If there are any errors then errno is set and the return value is zero.
 * 
 * @param[in] desc
 * The descriptor to use for this region, or zero to let the library choose an
 * available descriptor.
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
 * @param[in] attach
 * This is the virtual address where the new region file will be mapped 
 * into the process.
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
 * - EBUSY\n
 * requested region descriptor is already in use.
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
 * attach or vspace are too large, or not aligned on a page boundary.
 * Also reported if region descriptor is greater than maximum number of
 * regions configured.
 */
#ifdef NVM_EXT
nvm_desc nvm_create_region(//#
        nvm_desc desc,
        const char *pathname,
        const char *regionname,
        void *attach,
        size_t vspace,
        size_t pspace,
        mode_t mode
        )
{
    /* Return an error if attach address is zero or misaligned. Also verify
     * alignment of sizes. */
    if (attach == 0 || !nvm_addr_align((void^)attach) ||
        !nvm_align_check(vspace) || !nvm_align_check(pspace))
    {
        errno = EINVAL;
        return 0;
    }

    /* Create the file for the region in a persistent memory file system.
     * fill it with zeroes to allocate the desired amount of space. */
    nvms_file *handle = nvms_create_region(pathname, vspace, pspace, mode);
    if (handle == 0)
        return 0;

    //TODO+ verify it is really an NVM file system

    /* Lock the new region file. Note that no other process can map the file 
     * yet because it has a zero nvm_region usid. */
    if (!nvms_lock_region(handle, 1))
    {
        int serr = errno;
        nvms_close_region(handle); // ignore any close error
        errno = serr;
        return 0;
    }

    /* Write the header so that it can be mapped in. */
    {
        /* allocate and zero the struct to write. */
        nvm_region_init init;
        memset(&init, 0, sizeof(init));

        /* store the USID */
        init.usid = nvm_usidof(nvm_region);

        /* There is no root object yet. Note that since init is in volatile
         * memory, rootObject is an absolute pointer here. Thus this puts a
         * zero in rootObject rather than a null self-relative pointer,
         * i.e. one. This works fine since the check for being initialized
         * also reads init from the region file into volatile memory. */
        init.header.rootObject = 0;

        /* set sizes */
        init.header.vsize = vspace;
        init.header.psize = pspace;

        /* set region name */
        strncpy(init.header.name, regionname, sizeof(init.header.name) - 1);

        /* write it to the file */
        if (!nvms_write_region(handle, &init, sizeof(init)))
        {
            int serr = errno;
            nvms_close_region(handle); // ignore any close error
            errno = serr;
            return 0;
        }
    }

    /* Map the file into our address space so we can initialize it. This 
     * allocates a descriptor if necessary. If successful the application
     * data mutex will be locked on return. */
    desc = nvm_map_region(pathname, handle, attach, desc, 1);
    if (!desc)
    {
        int serr = errno;
        nvms_close_region(handle); // ignore any close error
        errno = serr;
        return 0;
    }

    /* Initialize the nvm_region header. Note that there is no undo for this
     * since transactions are not supported yet. */
    nvm_region ^region = (nvm_region^)attach;

    /* set the persistent region name. */
    strncpy((char*)region=>header.name, regionname, 
            sizeof(region=>header.name) - 1);
    nvm_flush(region=>header.name, sizeof(region=>header.name));
    
    /* This NVM segment has just been created so its attach count is set to
     * one. Attach count of 0 is used to ensure no match with attach_count. */
    region=>attach_cnt ~= 1;

    /* Initialize the region mutex */
    nvm_mutex_init1((nvm_amutex^)%region=>reg_mutex, (uint8_t)210);

    /* Create the base heap in the base extent. It is initialized for 
     * non-transactional allocation. Note that any errors are assert failures,
     * so there is no need to check for errors. */
    nvm_heap ^rh;
    rh = (nvm_heap ^)nvm_create_rootheap(regionname, region, pspace);
    region=>rootHeap ~= rh;

    /* no extent table in the nvm_region yet. */
    region=>extent_count ~= 0;
    region=>extents ~= 0;

    /* Create the initial transaction table in NVM, and save it. It will
     * be created with all transactions and undo free. */
    nvm_create_trans_table(region, rh);

    /* Run unnecessary recovery to construct the volatile transaction table. */
    nvm_recover(desc);

    /* All non-transactional allocation has been done. Disable any more
     * non-transactional allocation */
    nvm_finishNT(rh);

    /* Create the upgrade mutex array used for locking upgrades */
    nvm_app_data *ad = nvm_get_app_data();
    @ desc {
        nvm_mutex_array ^ma = nvm_create_mutex_array1(rh,
                ad->params.upgrade_mutexes, 255);
        if (!ma)
            nvms_assert_fail("No space in root heap for upgrade mutexes");
        region=>upgrade_mutexes @= ma;
    }
    
    /* We can now release the app data lock acquired by nvm_map_region. */
    nvms_unlock_mutex(ad->mutex);

    /* The region is now ready for transactional allocate/deallocate from
     * the root heap. All that remains is for the application to create its
     * root struct and the region will be valid. */
    return desc;
}
#else
nvm_desc nvm_create_region(//#
        nvm_desc desc,
        const char *pathname,
        const char *regionname,
        void *attach,
        size_t vspace,
        size_t pspace,
        mode_t mode
        )
{
    nvm_region_sizeof_check();

    /* Return an error if attach address is zero or misaligned. Also verify
     * alignment of sizes. */
    if (attach == 0 || !nvm_addr_align(attach) ||
        !nvm_align_check(vspace) || !nvm_align_check(pspace))
    {
        errno = EINVAL;
        return 0;
    }

    /* Create the file for the region in a persistent memory file system.
     * fill it with zeroes to allocate the desired amount of space. */
    nvms_file *handle = nvms_create_region(pathname, vspace, pspace, mode);
    if (handle == 0)
        return 0;

    //TODO+ verify it is really an NVM file system

    /* Lock the new region file. Note that no other process can map the file 
     * yet because it has a zero nvm_region usid. */
    if (!nvms_lock_region(handle, 1))
    {
        int serr = errno;
        nvms_close_region(handle); // ignore any close error
        errno = serr;
        return 0;
    }

    /* Write the header so that it can be mapped in. */
    {
        /* allocate and zero the struct to write. */
        nvm_region_init init;
        memset(&init, 0, sizeof(init));

        /* store the USID */
        init.usid = nvm_usidof(nvm_region);

        /* There is no root object yet. Note that since init is in volatile
         * memory, rootObject is an absolute pointer here. Thus this puts a
         * zero in rootObject rather than a null self-relative pointer,
         * i.e. one. This works fine since the check for being initialized
         * also reads init from the region file into volatile memory. */
        init.header.rootObject = 0;

        /* set sizes */
        init.header.vsize = vspace;
        init.header.psize = pspace;

        /* set region name */
        strncpy(init.header.name, regionname, sizeof(init.header.name) - 1);

        /* write it to the file */
        if (!nvms_write_region(handle, &init, sizeof(init)))
        {
            int serr = errno;
            nvms_close_region(handle); // ignore any close error
            errno = serr;
            return 0;
        }
    }

    /* Map the file into our address space so we can initialize it. This 
     * allocates a descriptor if necessary. If successful the application
     * data mutex will be locked on return. */
    desc = nvm_map_region(pathname, handle, attach, desc, 1);
    if (!desc)
    {
        int serr = errno;
        nvms_close_region(handle); // ignore any close error
        errno = serr;
        return 0;
    }

    /* Initialize the nvm_region header. Note that there is no undo for this
     * since transactions are not supported yet. */
    nvm_region *region;    
    region = attach;

    /* set the persistent region name. We will flush later. */
    strncpy(region->header.name, regionname,    
            sizeof(region->header.name) - 1);    

    /* This NVM segment has just been created so its attach count is set to
     * one. Attach count of 0 is used to ensure no match with attach_count. */
    region->attach_cnt = 1;    

    /* Initialize the region mutex */
    nvm_mutex_init1((nvm_amutex*)&region->reg_mutex, 210);

    /* Create the base heap in the base extent. It is initialized for 
     * non-transactional allocation. Note that any errors are assert failures,
     * so there is no need to check for errors. */
    nvm_heap *rh;    
    rh = nvm_create_rootheap(regionname, region, pspace);
    nvm_heap_set(&region->rootHeap, rh);

    /* no extent table in the nvm_region yet. */
    region->extent_count = 0;    
    nvm_extent_set(&(region->extents), 0);    

    /* Create the initial transaction table in NVM, and save it. It will
     * be created with all transactions and undo free. */
    nvm_create_trans_table(region, rh);

    /* Run unnecessary recovery to construct the volatile transaction table. */
    nvm_recover(desc);

    /* All non-transactional allocation has been done. Disable any more
     * non-transactional allocation */
    nvm_finishNT(rh);

    /* flush the region now that it is initialized. */
    nvm_flush(region, sizeof(*region));

    /* Create the upgrade mutex array used for locking upgrades */
    nvm_app_data *ad = nvm_get_app_data();
    nvm_txbegin(desc);
    nvm_mutex_array *ma = nvm_create_mutex_array1(rh,
            ad->params.upgrade_mutexes, 255);
    if (!ma)
        nvms_assert_fail("No space in root heap for upgrade mutexes");
    nvm_mutex_array_txset(&region->upgrade_mutexes, ma);
    nvm_txend();
    
    /* We can now release the app data lock acquired by nvm_map_region. */
    nvms_unlock_mutex(ad->mutex);

    /* The region is now ready for transactional allocate/deallocate from
     * the root heap. All that remains is for the application to create its
     * root struct and the region will be valid. */
    return desc;
}
#endif //NVM_EXT

/*-------------------------- nvm_set_root_object ----------------------------*/
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
        )
{
    /* get application data */
    nvm_app_data *ad = nvm_get_app_data();

    /* validate the descriptor is good, and get the region data. */
    if (desc < 1 || desc > ad->params.max_regions)
    {
        errno = EINVAL;
        return 0;
    }
    nvm_region_data *rd = ad->regions[desc];
    if (rd == 0)
    {
        errno = EINVAL;
        return 0;
    }

    /* verify root object is in the right region. */
    nvm_region ^region = (nvm_region ^)rd->region;
    if (!nvm_region_ptr(region, (void^)rootobj))
    {
        errno = EINVAL;
        return 0;
    }

    /* This thread must not have an active transaction. If it did that would
     * imply the creation of the rood object is not yet committed. */
    if (nvm_txdepth())
    {
        errno = EINVAL;
        return 0;
    }

    /* Verify that the region has not already been made valid by setting a
     * root object. Note that creation sets an absolute value of zero in
     * the root pointer which is actually a self relative pointer to self */
    if (region=>header.rootObject != %region=>header.rootObject)
    {
        errno = EBUSY;
        return 0;
    }

    /* Persistently store the new root object making the region valid
     * for attaching. This is not done in a transaction because we do not 
     * want recovery to make a valid region become invalid by rolling back
     * this store. */
    region=>header.rootObject ~= rootobj;
    nvm_persist();

    /* Clear the no root flag now. */
    rd->noRoot = 0;
    return 1;
}
#else
int nvm_set_root_object(   
        nvm_desc desc,
        void *rootobj    
        )
{
    /* get application data */
    nvm_app_data *ad = nvm_get_app_data();

    /* validate the descriptor is good, and get the region data. */
    if (desc < 1 || desc > ad->params.max_regions)
    {
        errno = EINVAL;
        return 0;
    }
    nvm_region_data *rd = ad->regions[desc];
    if (rd == 0)
    {
        errno = EINVAL;
        return 0;
    }

    /* verify root object is in the right region. */
    nvm_region *region = rd->region;
    nvm_verify(region, shapeof(nvm_region));
    if (!nvm_region_ptr(region, rootobj))
    {
        errno = EINVAL;
        return 0;
    }

    /* This thread must not have an active transaction. If it did that would
     * imply the creation of the root object is not yet committed. */
    if (nvm_txdepth())
    {
        errno = EINVAL;
        return 0;
    }

    /* Verify that the region has not already been made valid by setting a
     * root object. Note that creation sets an absolute value of zero in
     * the root pointer which is actually a self relative pointer to self */
    if (region->header.rootObject)
    {
        errno = EBUSY;
        return 0;
    }

    /* Persistently store the new root object, then make the region valid
     * for attaching. This is not done in a transaction because we do not 
     * want recovery to make a valid region become invalid by rolling back
     * this store. */
    void_set(&region->header.rootObject, rootobj);
    nvm_persist1(&region->header.rootObject);

    /* Clear the no root flag now. */
    rd->noRoot = 0;

    return 1;
}
#endif //NVM_EXT

/*-------------------------- nvm_new_root_object ----------------------------*/
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
 * If there are any errors then errno is set and the return value is zero.
 * 
 * @param[in] rootobj
 * This is an NVM pointer to the new root object.
 * 
 * @return 
 * The old root object pointer is returned if success. NULL returned on error.
 * 
 * @par Errors
 */
#ifdef NVM_EXT
void ^nvm_new_root_object@(
        void ^rootobj
        )
{
    /* get application data */
    nvm_app_data *ad = nvm_get_app_data();

    /* Get the region descriptor from the transaction */
    nvm_desc desc = nvm_txdesc();

    /*  get the region data. */
    nvm_region_data *rd = ad->regions[desc];

    /* verify new root object is in the right region. */
    nvm_region ^region = (nvm_region ^)rd->region;
    if (!nvm_region_ptr(region, (void^)rootobj))
    {
        errno = EINVAL;
        return 0;
    }

    /* get the current root object for the return value */
    void ^old = region=>header.rootObject;

    /* transactionally store the new pointer */
    region=>header.rootObject @= rootobj;

    return old;
}
#else
void *nvm_new_root_object(
        void *rootobj
        )
{
    /* get application data */
    nvm_app_data *ad = nvm_get_app_data();

    /* Get the region descriptor from the transaction */
    nvm_desc desc = nvm_txdesc();

    /*  get the region data. */
    nvm_region_data *rd = ad->regions[desc];

    /* verify region USID is valid. */
    nvm_region *region = rd->region;
    nvm_verify(region, shapeof(nvm_region)); //#

    /* verify new root object is in the right region. */
    if (!nvm_region_ptr(region, rootobj))
    {
        errno = EINVAL;
        return 0;
    }

    /* get the current root object for the return value */
    void *old = void_get(&region->header.rootObject);

    /* transactionally store the new pointer */
    nvm_undo(&region->header.rootObject, sizeof(void*));
    void_set(&region->header.rootObject, rootobj);
    nvm_flush1(&region->header.rootObject);

    return old;
}
#endif //NVM_EXT

/*-------------------------- nvm_destroy_region ----------------------------*/
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
        )
{
    return nvms_destroy_region(name);
}


/*--------------------------- nvm_attach_region -----------------------------*/
/**
 * Attach a Non-Volatile Memory region to this application and return a 
 * region descriptor. If the region argument is zero then the library will 
 * choose an unused descriptor. If a non-zero region descriptor is passed 
 * in then it will be used. The application must ensure it does not attempt
 * to use the same region descriptor twice.  If exclusive is true then the 
 * region file will be locked exclusive, otherwise it is locked shared. 
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
 * @param[in] region
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
 * The return value is a descriptor for the region. It is used as an
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
        )
{
    /* Return an error if attach address is zero. */
    if (attach == 0 || !nvm_addr_align((void^)attach))
    {
        errno = EINVAL;
        return 0;
    }

    /* Open the region file. */
    nvms_file *handle = nvms_open_region(pathname);
    if (handle == 0)
        return 0;

    //TODO+ verify it is really an NVM file system

    /* Lock the region file exclusive to ensure it is not in use by some
     * other application.  */
    if (!nvms_lock_region(handle, 1))
    {
        int serr = errno;
        nvms_close_region(handle); // ignore any close error
        errno = serr;
        return 0;
    }

    /* Map the file into our address space. When this is done the region is
     * part of the application. */
    desc = nvm_map_region(pathname, handle, attach, desc, 0);
    if (!desc)
    {
        int serr = errno;
        nvms_close_region(handle); // ignore any close error
        errno = serr;
        return 0;
    }

    /* Ensure the region is consistent by running recovery. */
    nvm_recover(desc);

    //TODO if upgrade mutex array size paramater is different then reallocate

    /* we can now release the app data lock acquired by nvm_map_region */
    nvm_app_data *ad = nvm_get_app_data();
    nvms_unlock_mutex(ad->mutex);

    /* return with success */
    return desc;
}
#else
nvm_desc nvm_attach_region(
        nvm_desc desc,
        const char *pathname,
        void *attach
        )
{
    nvm_region_sizeof_check();

    /* Return an error if attach address is zero. */
    if (attach == 0 || !nvm_addr_align(attach))
    {
        errno = EINVAL;
        return 0;
    }

    /* Open the region file. */
    nvms_file *handle = nvms_open_region(pathname);
    if (handle == 0)
        return 0;

    //TODO+ verify it is really an NVM file system

    /* Lock the region file exclusive to ensure it is not in use by some
     * other application.  */
    if (!nvms_lock_region(handle, 1))
    {
        int serr = errno;
        nvms_close_region(handle); // ignore any close error
        errno = serr;
        return 0;
    }

    /* Map the file into our address space. When this is done the region is
     * part of the application. */
    desc = nvm_map_region(pathname, handle, attach, desc, 0);
    if (!desc)
    {
        int serr = errno;
        nvms_close_region(handle); // ignore any close error
        errno = serr;
        return 0;
    }

    /* Ensure the region is consistent by running recovery. */
    nvm_recover(desc);

    //TODO if upgrade mutex array size paramater is different then reallocate

    /* we can now release the app data lock acquired by nvm_map_region */
    nvm_app_data *ad = nvm_get_app_data();
    nvms_unlock_mutex(ad->mutex);

    /* return with success */
    return desc;
}
#endif //NVM_EXT

/**
 * This takes an address and determines which currently attached region
 * contains it. This may require looping through the list of attached
 * regions. If a region is not found then a null pointer is returned,
 * and errno is set to ERANGE
 *
 * Note that this only checks the virtual address space owned by the
 * region. It does not ensure the address is in an extent of NVM.
 *
 * @param[in] ptr
 * The NVM address to verify.
 *
 * @return
 * Pointer to the region in NVM or zero if none
 */
#ifdef NVM_EXT
nvm_region ^nvm_region_find(const void ^ptr)
{
    /* if there is a current transaction then there is a good chance that
     * the pointer is in the transaction"s region. */
    nvm_thread_data *td = nvm_get_thread_data();
    nvm_region ^rg = td->region;
    if (rg && nvm_region_rng(rg, ptr, 0))
    {
        return rg;
    }

    /* We need to lock the application data since we are reading it. */
    nvm_app_data *ad = nvm_get_app_data();
    nvms_lock_mutex(ad->mutex, 1);

    /* Loop through the region data looking for a region that contains ptr */
    nvm_desc desc;
    for (desc = 1; desc <= td->params.max_regions; desc++)
    {
        /* get the application region data. */
        nvm_region_data *rd = ad->regions[desc];
        if (!rd)
            continue;

        /* See if this is the region */
        rg = rd->region;
        if (rg && nvm_region_rng(rg, ptr, 0))
        {
            nvms_unlock_mutex(ad->mutex);
            errno = 0;
            return rg;
        }
    }

    /* This is an invalid pointer so return a null region pointer and leave
     * errno as ERANGE. */
    nvms_unlock_mutex(ad->mutex);
    return 0;


}
#else
nvm_region *nvm_region_find(const void *ptr)
{
    /* if there is a current transaction then there is a good chance that
     * the pointer is in the transaction"s region. */
    nvm_thread_data *td = nvm_get_thread_data();
    nvm_region *rg = td->region;
    if (rg && nvm_region_rng(rg, ptr, 0))
    {
        return rg;
    }

    /* We need to lock the application data since we are reading it. */
    nvm_app_data *ad = nvm_get_app_data();
    nvms_lock_mutex(ad->mutex, 1);

    /* Loop through the region data looking for a region that contains ptr */
    nvm_desc desc;
    for (desc = 1; desc <= td->params.max_regions; desc++)
    {
        /* get the application region data. */
        nvm_region_data *rd = ad->regions[desc];
        if (!rd)
            continue;

        /* See if this is the region */
        rg = rd->region;
        if (rg && nvm_region_rng(rg, ptr, 0))
        {
            nvms_unlock_mutex(ad->mutex);
            errno = 0;
            return rg;
        }
    }

    /* This is an invalid pointer so return a null region pointer and leave
     * errno as ERANGE. */
    nvms_unlock_mutex(ad->mutex);
    return 0;
}
#endif //NVM_EXT

/*--------------------------- nvm_query_region -----------------------------*/
/**
 * This returns the status of a region that is mapped into the current
 * process. Note that the returned status might not be current if some
 * other thread is changing the region.
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
#ifdef NVM_EXT
int nvm_query_region(
        nvm_desc desc,
        nvm_region_stat *stat
        )
{
    /* start off with status of zero so even if there are errors the contents
     * are not garbage. */
    memset(stat, 0, sizeof(*stat));

    /* get app and thread data pointers */
    nvm_app_data *ad = nvm_get_app_data();
    nvm_thread_data *td = nvm_get_thread_data();

    /* if desc is zero then use the current transaction's region. */
    if (td->txdepth && desc == 0)
    {
        desc = td->transaction=>desc;
    }

    /* Error if requested descriptor is too large or zero outside transaction */
    nvm_desc rmax = ad->params.max_regions;
    if (desc > rmax || desc == 0)
    {
        errno = EINVAL;
        return 0;
    }

    /* Lock the application mutex to get a consistent status */
    nvms_lock_mutex(ad->mutex, 1);

    /* If the region data is no longer, then desc is bad */
    nvm_region_data *rd = ad->regions[desc];
    if (rd == 0)
    {
        nvms_unlock_mutex(ad->mutex);
        errno = EINVAL;
        return 0;
    }

    /* store status values */
    nvm_region ^rg = rd->region;
    if (rg=>desc != desc)
        nvms_assert_fail("nvm_desc in region is different from region data");

    stat->desc = desc;
    stat->name = rg=>header.name;
    stat->base = rg;
    stat->vsize = rd->vsize;
    stat->psize = rg=>header.psize;
    int x = rg=>extents_size;
    nvm_extent ^ext = rg=>extents;
    while (x--)
    {
        stat->psize += ext=>size;
        ext++;
    }
    stat->extent_cnt = rg=>extent_count;
    stat->attach_cnt = rg=>attach_cnt;
    stat->rootheap = rg=>rootHeap;
    stat->rootobject = rg=>header.rootObject;

    /* return success */
    nvms_unlock_mutex(ad->mutex);
    return 1;
}
#else
int nvm_query_region(
        nvm_desc desc,
        nvm_region_stat *stat
        )
{
    /* start off with status of zero so even if there are errors the contents
     * are not garbage. */
    memset(stat, 0, sizeof(*stat));

    /* get app and thread data pointers */
    nvm_app_data *ad = nvm_get_app_data();
    nvm_thread_data *td = nvm_get_thread_data();

    /* if desc is zero then use the current transaction's region. */
    if (td->txdepth && desc == 0)
    {
        desc = td->transaction->desc;
    }

    /* Error if requested descriptor is too large or zero outside transaction */
    nvm_desc rmax = ad->params.max_regions;
    if (desc > rmax || desc == 0)
    {
        errno = EINVAL;
        return 0;
    }

    /* Lock the application mutex to get a consistent status */
    nvms_lock_mutex(ad->mutex, 1);

    /* If the region data is no longer, then desc is bad */
    nvm_region_data *rd = ad->regions[desc];
    if (rd == 0)
    {
        nvms_unlock_mutex(ad->mutex);
        errno = EINVAL;
        return 0;
    }

    /* store status values */
    nvm_region *rg = rd->region;
    if (rg->desc != desc)
        nvms_assert_fail("nvm_desc in region is different from region data");
    stat->desc = desc;
    stat->name = rg->header.name;
    stat->base = rg;
    stat->vsize = rd->vsize;
    stat->psize = rg->header.psize;
    int x = rg->extents_size;
    nvm_extent *ext = nvm_extent_get(&rg->extents);
    while (x--)
    {
        nvm_verify(ext, shapeof(nvm_extent));
        stat->psize += ext->size;
        ext++;
    }
    stat->extent_cnt = rg->extent_count;
    stat->attach_cnt = rg->attach_cnt;
    stat->rootheap = nvm_heap_get(&rg->rootHeap);
    stat->rootobject = void_get(&rg->header.rootObject);

    /* return success */
    nvms_unlock_mutex(ad->mutex);
    return 1;
}
#endif //NVM_EXT

/*--------------------------- nvm_detach_region -----------------------------*/
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
        )
{
    /* get app and thread data pointers */
   nvm_app_data *ad = nvm_get_app_data();
    nvm_thread_data *td = nvm_get_thread_data();

    /* Error if there is an active tansaction */
    if (td->txdepth)
    {
        errno = EINVAL;
        return 0;
    }

    /* Error if requested descriptor is too large or zero */
    nvm_desc rmax = ad->params.max_regions;
    if (desc > rmax || desc == 0)
    {
        errno = EINVAL;
        return 0;
    }

    /* If the region data is no longer, then desc is bad */
    nvm_region_data *rd = ad->regions[desc];
    if (rd == 0)
    {
        errno = EINVAL;
        return 0;
    }

    /* Let the transaction system verify there is no activity and prevent
     * any new transactions.  */
    if (!nvm_trans_detach(rd))
    {
        return 0;
    }

    /* remove the region from this application. */
    nvm_unmap_region(desc);

    /* return success */
    return 1;
}
#else
int nvm_detach_region(
        nvm_desc desc
        )
{
    /* get app and thread data pointers */
    nvm_app_data *ad = nvm_get_app_data();
    nvm_thread_data *td = nvm_get_thread_data();

    /* Error if there is an active tansaction */
    if (td->txdepth)
    {
        errno = EINVAL;
        return 0;
    }

    /* Error if requested descriptor is too large or zero */
    nvm_desc rmax = ad->params.max_regions;
    if (desc > rmax || desc == 0)
    {
        errno = EINVAL;
        return 0;
    }

    /* If the region data is no longer, then desc is bad */
    nvm_region_data *rd = ad->regions[desc];
    if (rd == 0)
    {
        errno = EINVAL;
        return 0;
    }

    /* Let the transaction system verify there is no activity and prevent
     * any new transactions. */
    if (!nvm_trans_detach(rd))
    {
        return 0;
    }

    /* remove the region from this application. */
    nvm_unmap_region(desc);

    /* return success */
    return 1;
}
#endif //NVM_EXT

/**
 * This is the context for an on abort or on commit operation to remove or
 * shrink an extent in a region. It is used by two different callbacks.
 */
#ifdef NVM_EXT
persistent struct nvm_remx_ctx
{
    /**
     * offset into the region file where the NVM begins
     */
    off_t offset;

    /**
     * This is a pointer to the NVM. Subtracting the offset from the
     * virtual address gets the address of the nvm_region.
     */
    uint8_t ^addr;

    /**
     * This is the number of bytes to be deleted.
     */
    size_t bytes;
};
typedef persistent struct nvm_remx_ctx nvm_remx_ctx;
#else
struct nvm_remx_ctx
{
    /**
     * offset into the region file where the NVM begins
     */
    off_t offset;

    /**
     * This is a pointer to the NVM. Subtracting the offset from the
     * virtual address gets the address of the nvm_region.
     */
    void_srp addr;

    /**
     * This is the number of bytes to be deleted.
     */
    size_t bytes;
};
typedef struct nvm_remx_ctx nvm_remx_ctx;
#endif //NVM_EXT
extern const nvm_type nvm_type_nvm_remx_ctx;
extern const nvm_extern nvm_extern_nvm_remx_callback;
extern const nvm_extern nvm_extern_nvm_freex_callback;
/**
 * Unmap NVM and free it from the region file. This is a callback for an
 * on abort operation of an attempt to add an extent or grow an extent.
 * It is an on commit operation for shrinking or deleting an extent. It does
 * not affect any of the region metadata describing the extent since it is used
 * during the nested transaction to create the metadata changes. It also
 * presumes that the region mutex is still locked by the transaction doing
 * rollback.
 *
 * @param ctx Context describing the NVM to remove.
 */
#ifdef NVM_EXT
USID("009e 1ed4 1f0d 1ea0 9f9e 0dbc abf5 6ae0")
void nvm_freex_callback@(nvm_remx_ctx ^ctx)
{
    if (ctx=>offset == 0 || ctx=>bytes == 0)
        return; // callback not activated, or nothing to do.

    /* get app data pointer */
    nvm_app_data *ad = nvm_get_app_data();

    /* Get the file handle for deleting the NVM */
    nvm_desc desc = nvm_txdesc();
    nvms_file *fh = ad->regions[desc]->handle;

    /* unmap then delete the NVM from the file. */
    nvms_unmap_range(fh, ctx=>offset, ctx=>bytes);
    nvms_free_range(fh, ctx=>offset, ctx=>bytes);
}
#else
void nvm_freex_callback(nvm_remx_ctx *ctx)
{
    if (ctx->offset == 0 || ctx->bytes == 0)
        return; // callback not activated, or nothing to do.

    /* get app data pointer */
    nvm_app_data *ad = nvm_get_app_data();

    /* Get the file handle for deleting the NVM */
    nvm_desc desc = nvm_txdesc();
    nvms_file *fh = ad->regions[desc]->handle;

    /* unmap then delete the NVM from the file. */
    nvms_unmap_range(fh, ctx->offset, ctx->bytes);
    nvms_free_range(fh, ctx->offset, ctx->bytes);
}
#endif //NVM_EXT
/**
 * This on abort/commit callback removes/shrinks an extent from the application.
 * It is used to back out an extent add/grow if the parent transaction rolls
 * back, and it is used to shrink/delete an extent at commit.
 * 
 * @param ctx Context describing the NVM to remove.
 */
#ifdef NVM_EXT
USID("2732 fd0f 0a13 11e2 f304 ce0c 9fd5 33f8")
void nvm_remx_callback@(nvm_remx_ctx ^ctx)
{   
    if (ctx=>offset == 0 || ctx=>bytes == 0)
        return; // callback not activated, or nothing to do.

    /* get app data pointer */
    nvm_app_data *ad = nvm_get_app_data();

    /* Get the file handle for deleting the allocated NVM, if any. */
    nvm_desc desc = nvm_txdesc();
    nvms_file *fh = ad->regions[desc]->handle;

    /* Get the nvm_region address based on the descriptor. */
    nvm_region ^rg = ad->regions[desc]->region;
    uint8_t ^addr = ctx=>addr;
    if ((void*)rg != (void*)(addr - ctx=>offset))
        nvms_corruption("Inconsistent offset/addr in undo", rg, ctx);

    /* Modify the metadata to show the extent is deleted in a nested 
     * transaction. This lets us commit the change and unmap the NVM before
     * actually deleting it from the file. */
    @{ //begin nested TX

        /* Acquire the region mutex to ensure no other transaction is changing
         * the region metadata. This is released when the nested transaction
         * surrounding the callback is committed. */
        nvm_xlock(%rg=>reg_mutex);

        /* If offset is in the base extent, then shrink the base extent. */
        if (ctx=>offset < rg=>header.psize)
        {
            /* assert that the NVM to delete is the end of the base extent. This
             * can only be wrong if the undo was corrupted. */
            if (ctx=>offset + ctx=>bytes != rg=>header.psize)
                nvms_corruption("Shrink of base extent not at end", rg, ctx);
            rg=>header.psize @= ctx=>offset;
        }
        else
        {
            /* Must be deleting space from an extent. Find the extent and shrink
             * or remove it from the region metadata. */
            int x;
            nvm_extent ^ext = rg=>extents;
            for (x = 0; x < rg=>extents_size; x++, ext++)
            {
                uint8_t ^base = ext=>addr;
                if (addr >= base && addr < base + ext=>size)
                {
                    /* This is the extent being shrunk or deleted. First assert
                     * that the end of the shrink matches the end of the
                     * extent. */
                    if (addr + ctx=>bytes != base + ext=>size)
                        nvms_corruption("Shrink of extent not at end", rg, ctx);

                    /* If deleting the extent, remove the entry from the 
                     * extent array. Otherwise just shorten the extent. */
                    if (addr == base)
                    {
                        /* Delete extent. */
                        ext=>addr @= 0;
                        ext=>size @= 0;
                        rg=>extent_count@--;
                    }
                    else
                    {
                        /* Shrink extent */
                        ext=>size @-= ctx=>bytes;
                    }
                    break;
                    ;
                }
            }

            /* Note that it is possible for this loop to exit without finding any
             * extent metadata to update. This happens if the nested transaction
             * to update the metadata commits, but thread death results in this 
             * callback being called again. */
        }

        /* Commit the metadata change so we can unmap the NVM globally. */
    }
    
    /* Unmap the NVM before deleting it */
    nvms_unmap_range(fh, ctx=>offset, ctx=>bytes);

    /* Now that it is unmapped, delete the NVM from the file. */
    nvms_free_range(fh, ctx=>offset, ctx=>bytes);
}
#else
void nvm_remx_callback(nvm_remx_ctx *ctx)
{
    if (ctx->offset == 0 || ctx->bytes == 0)
        return; // callback not activated, or nothing to do.

    /* get app data pointer */
    nvm_app_data *ad = nvm_get_app_data();

    /* Get the file handle for deleting the allocated NVM, if any. */
    nvm_desc desc = nvm_txdesc();
    nvms_file *fh = ad->regions[desc]->handle;

    /* Get the nvm_region address based on the descriptor. */
    nvm_region *rg = ad->regions[desc]->region;
    nvm_verify(rg, shapeof(nvm_region));
    uint8_t *addr = void_get(&ctx->addr);
    if ((void*)rg != (void*)(addr - ctx->offset))
        nvms_corruption("Inconsistent offset/addr in undo", rg, ctx);

    /* Modify the metadata to show the extent is deleted in a nested 
     * transaction. This lets us commit the change and unmap the NVM before
     * actually deleting it from the file. */
    nvm_txbegin(0);

    /* Acquire the region mutex to ensure no other transaction is changing the
     * region metadata. This is released when the nested transaction surrounding
     * the callback is committed. */
    nvm_xlock(&rg->reg_mutex);

    /* If offset is in the base extent, then shrink the base extent. */
    if (ctx->offset < rg->header.psize)
    {
        /* assert that the NVM to delete is the end of the base extent. This
         * can only be wrong if the undo was corrupted. */
        if (ctx->offset + ctx->bytes != rg->header.psize)
            nvms_corruption("Shrink of base extent not at end", rg, ctx);
        NVM_UNDO(rg->header.psize);
        rg->header.psize = ctx->offset;
        nvm_flush1(&rg->header.psize);
    }
    else
    {
        /* Must be deleting space from an extent. Find the extent and shrink
         * or remove it from the region metadata. */
        int x;
        nvm_extent *ext = nvm_extent_get(&rg->extents);
        for (x = 0; x < rg->extents_size; x++, ext++)
        {
            nvm_verify(ext, shapeof(nvm_extent));
            uint8_t *base = void_get(&ext->addr);
            if (addr >= base && addr < base + ext->size)
            {
                /* This is the extent being shrunk or deleted. First assert
                 * that the end of the shrink matches the end of the
                 * extent. */
                if (addr + ctx->bytes != base + ext->size)
                    nvms_corruption("Shrink of extent not at end", rg, ctx);

                /* If deleting the extent, remove the entry from the 
                 * extent array. Otherwise just shorten the extent. */
                if (addr == base)
                {
                    /* Delete extent. */
                    NVM_UNDO(*ext);
                    NVM_UNDO(rg->extent_count);
                    void_set(&ext->addr, 0);
                    ext->size = 0;
                    rg->extent_count--;
                    nvm_flush(ext, sizeof(*ext));
                    nvm_flush1(&rg->extent_count);
                }
                else
                {
                    /* Shrink extent */
                    NVM_UNDO(ext->size);
                    ext->size -= ctx->bytes;
                    nvm_flush1(&ext->size);
                }
                break;
                ;
            }
        }

        /* Note that it is possible for this loop to exit without finding any
         * extent metadata to update. This happens if the nested transaction
         * to update the metadata commits, but thread death results in this 
         * callback being called again. */
    }

    /* Commit the metadata change so we can free the NVM. */
    nvm_commit();
    nvm_txend();

    /* Unmap the NVM before deleting it */
    nvms_unmap_range(fh, ctx->offset, ctx->bytes);

    /* Now that it is unmapped, delete the NVM from the file. */
    nvms_free_range(fh, ctx->offset, ctx->bytes);
}
#endif //NVM_EXT

/*---------------------------- nvm_add_extent ------------------------------*/
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
        )
{
    /* get app, thread and process data pointers */
    nvm_app_data *ad = nvm_get_app_data();
    nvm_thread_data *td = nvm_get_thread_data();

    /* Verify extent is in the same region as the current transaction */
    if (!nvm_region_ptr((void^)td->region, extent))
        return 0;

    /* validate alignment of the requested virtual addresses */
    if (extent == 0 || !nvm_addr_align(extent) || !nvm_align_check(psize))
    {
        errno = EINVAL;
        return 0;
    }

    /* Get the region descriptor from the transaction */
    nvm_desc desc = nvm_txdesc();

    /*  get the region data. */
    nvm_region_data *rd = ad->regions[desc];

    /* Validate the extent address is within the file, after the base extent,
     * and calculate the offset within the file. */
    if ((uint8_t^)extent < (uint8_t^)rd->region + rd->psize)
    {
        /* Begins before the end of the base extent. */
        errno = EINVAL;
        return 0;
    }
    off_t offset = (uint8_t^)extent - (uint8_t^)rd->region;
    if (offset + psize > rd->vsize)
    {
        /* Ends after the end of the file */
        errno = EINVAL;
        return 0;
    }

    /* Create an on abort operation to remove the extent if it is not
     * successfully added to the region. This will be applied if the caller's
     * transaction aborts after extent add succeeds. Until it is initialized
     * in a nested transaction commit, applying this operation is a no-op. */
    nvm_remx_ctx ^rctx = nvm_onabort(|nvm_remx_callback);

    /* Begin a nested transaction to add the extent. This will commit the 
     * extent to exist so we can map it in before we return. */
    @{
        /* lock the region mutex to ensure only one region reconfiguration can 
         * happen at a time. */
        nvm_region ^rg = rd->region;
        nvm_xlock(%rg=>reg_mutex);

        /* Before allocating the space in the file system we add the extent
         * description to the region. If we die or abort the nested transaction
         * it will roll it back. */
        nvm_extent ^ext = 0; // save an available slot to use

        /* Verify this extent does not overlap with an existing extent. Also
         * find an available slot if any. Note that the loop is never entered
         * if the extent pointer in the nvm_region is null */
        int cnt;
        nvm_extent ^xp = rg=>extents;
        if ((xp == 0 && rg=>extents_size != 0) ||
            rg=>extent_count > rg=>extents_size)
            nvms_corruption("Corrupt region extent count or array",
                rg, rg=>extents);
        for (cnt = 0; cnt < rg=>extents_size; xp++, cnt++)
        {
            uint8_t ^addr = xp=>addr;
            if (addr == 0)
            {
                if (ext == 0)
                    ext = xp; //save first unused slot
                continue; // nothing to overlap
            }

            /* Check for overlap. New extent must end before or start after
             * existing extent. */
            uint8_t ^newaddr = extent;
            if (newaddr + psize <= addr || addr + xp=>size <= newaddr)
                continue;

            /* return error for overlap */
            errno = EINVAL;
            return 0;
        }

        /* Ensure there is room in the extents array for a new entry. */
        if (rg=>extent_count == rg=>extents_size)
        {
            /* The current array is full or does not exist yet. If it is full
             * then reallocate it at twice the current size. If this is the
             * first  extent allocated in the region then allocate a new array.
             * This is done in a nested transaction since there is no harm in
             * having more unused entries. */
            @{ //nvm_txbegin(0);
                if (rg=>extents_size == 0)
                {
                    /* the first entry is available */
                    ext = nvm_alloc((void*)rg=>rootHeap,
                            shapeof(nvm_extent), NVM_INITIAL_EXTENTS);

                    /* store the array address transactionally */
                    rg=>extents @= ext;

                    /* set the new array size. */
                    rg=>extents_size @= NVM_INITIAL_EXTENTS;
                }
                else
                {
                    /* double the size of the array */
                    nvm_extent ^old_extents = rg=>extents;
                    nvm_extent ^new_extents = nvm_alloc((void*)rg=>rootHeap,
                            shapeof(nvm_extent), 2 * rg=>extents_size);

                    /* store the new array address transactionally */
                    rg=>extents @= new_extents;

                    /* Copy the extent descriptions from the old array. Just in
                     * case we validate the contents as we copy. Note that
                     * there should not be any unallocated entries since the
                     * array was full. */
                    int cnt;
                    nvm_extent ^ox = old_extents; // old extent
                    nvm_extent ^nx = new_extents; // new extent
                    for (cnt = 0; cnt < rg=>extents_size; ox++, nx++, cnt++)
                    {
                        uint8_t ^addr = ox=>addr;
                        if (!nvm_addr_align(addr))
                            nvms_corruption("Bad pointer to NVM extent",
                                    ox, addr);
                        if (!nvm_align_check(ox=>size))
                            nvms_corruption("Bad extent size", 
                                    ox, (void^)ox=>size);

                        /* Initialize the new record. This needs no undo because
                         * it is in freshly allocated space.  */
                        nx=>addr ~= addr;
                        nx=>size ~= ox=>size;
                    }

                    /* Return the old extent array to the heap. */
                    nvm_free(old_extents);

                    /* The first new entry is available */
                    ext = new_extents + rg=>extents_size;

                    /* set the new array size. */
                    rg=>extents_size @*= 2;
                }

                /* Commit the allocation of the new extent array. */
                nvm_commit();
            } //nvm_txend();
        }

        /* we must have found an empty slot by now. */
        if (ext == 0)
            nvms_corruption("Corrupt region extent array",
                rg, rg=>extents);

        /* We now have an available slot to use for this extent. */
        ext=>addr @= extent;
        ext=>size @= psize;

        /* Increment the count of allocated extents */
        rg=>extent_count@++;

        /* Update the parent on abort operation to remove the new extent if this
         * transaction commits but the parent rolls back. */
        rctx=>offset @= offset;
        rctx=>addr @= extent;
        rctx=>bytes @= psize;

        /* Create an on abort operation to delete this extent if we die
         * here. You might think that the on abort operation in the parent
         * transaction would be sufficient to cleanup if we die. However there
         * is a race condition. If the undo is not applied during the same
         * locking of the region mutex, some other thread could allocate the
         * same area of the region file after this nested transaction aborts
         * and before the parent on abort undo locks the region. However the
         * following on abort operation will execute before the lock is
         * dropped. */
        nvm_remx_ctx ^fctx = nvm_onabort(|nvm_freex_callback);
        fctx=>offset ~= offset;
        fctx=>addr ~= extent;
        fctx=>bytes ~= psize;


        /* Call the service layer to add the NVM extent to the region file. */
        if (nvms_alloc_range(rd->handle, offset, psize) == 0)
        {
            nvm_abort(); // abort the nested transaction
            return 0;
        }

        /* Map the new extent into this process. */
        if (nvms_map_range(rd->handle, offset, psize) == 0)
        {
            /* Aborting this nested transaction will delete and unmap the
             * new extent when the on_abort operation is called. */
            nvm_abort(); // abort the nested transaction
            return 0;
        }

        /* Commit the nested transaction to make the new extent visible to all
         * threads. */
        nvm_commit();
    }

    /* return success. */
    return extent;
}
#else
void *nvm_add_extent(   
        void *extent,    
        size_t psize
        )
{
    /* get app, thread and process data pointers */
    nvm_app_data *ad = nvm_get_app_data();
    nvm_thread_data *td = nvm_get_thread_data();

    /* Verify extent is in the same region as the current transaction */
    if (!nvm_region_ptr(td->region, extent))
        return NULL;

    /* validate alignment of the requested virtual addresses */
    if (extent == 0 || !nvm_addr_align(extent) || !nvm_align_check(psize))
    {
        errno = EINVAL;
        return NULL;
    }

    /* Get the region descriptor from the transaction */
    nvm_desc desc = nvm_txdesc();

    /*  get the region data. */
    nvm_region_data *rd = ad->regions[desc];

    /* Validate the extent address is within the file, after the base extent,
     * and calculate the offset within the file. */
    if ((uint8_t*)extent < (uint8_t*)rd->region + rd->psize)
    {
        /* Begins before the end of the base extent. */
        errno = EINVAL;
        return NULL;
    }
    off_t offset = (uint8_t*)extent - (uint8_t*)rd->region;
    if (offset + psize > rd->vsize)
    {
        /* Ends after the end of the file */
        errno = EINVAL;
        return NULL;
    }

    /* Create an on abort operation to remove the extent if it is not
     * successfully added to the region. This will be applied if the caller's
     * transaction aborts after extent add succeeds. Until it is initialized
     * in a nested transaction commit, applying this operation is a no-op. */
    nvm_remx_ctx *rctx = nvm_onabort(nvm_extern_nvm_remx_callback.usid);

    /* Begin a nested transaction to add the extent. This will commit the 
     * extent to exist so we can map it in before we return. */
    nvm_txbegin(0);

    /* lock the region mutex to ensure only one region reconfiguration can 
     * happen at a time. */
    nvm_region *rg = rd->region;
    nvm_verify(rg, shapeof(nvm_region));
    nvm_xlock(&rg->reg_mutex);

    /* Before allocating the space in the file system we add the extent
     * description to the region. If we die or abort the nested transaction
     * it will roll it back. */
    nvm_extent *ext = NULL; // save an available slot to use

    /* Verify this extent does not overlap with an existing extent. Also find
     * an available slot if any. Note that the loop is never entered if the
     * extent pointer is null */
    int cnt;
    nvm_extent *xp = nvm_extent_get(&rg->extents);
    if ((xp == NULL && rg->extents_size != 0) ||
        rg->extent_count > rg->extents_size)
        nvms_corruption("Corrupt region extent count or array",
            rg, nvm_extent_get(&rg->extents));
    for (cnt = 0; cnt < rg->extents_size; xp++, cnt++)
    {
        nvm_verify(xp, shapeof(nvm_extent));
        uint8_t *addr = void_get(&xp->addr);
        if (addr == NULL)
        {
            if (ext == NULL)
                ext = xp;
            continue; // nothing to overlap
        }

        /* Check for overlap. New extent must end before or start after
         * existing extent. */
        uint8_t *newaddr = extent;
        if (newaddr + psize <= addr || addr + xp->size <= newaddr)
            continue;

        /* return error for overlap */
        errno = EINVAL;
        return NULL;
    }

    /* Ensure there is room in the extents array for a new entry. */
    if (rg->extent_count == rg->extents_size)
    {
        /* The current array is full or does not exist yet. If it is full then
         * reallocate it at twice the current size. If this is the first 
         * extent allocated in the region then allocate a new array. This is
         * done in a nested transaction since there is no harm in having more
         * unused entries. */
        nvm_txbegin(0);
        if (rg->extents_size == 0)
        {
            /* the first entry is available */
            ext = nvm_alloc(nvm_heap_get(&rg->rootHeap),
                    shapeof(nvm_extent), NVM_INITIAL_EXTENTS);

            /* store the array address transactionally */
            NVM_UNDO(rg->extents);
            nvm_extent_set(&rg->extents, ext);
            nvm_flush1(&rg->extents);

            /* set the new array size. */
            NVM_UNDO(rg->extents_size);
            rg->extents_size = NVM_INITIAL_EXTENTS;
            nvm_flush1(&rg->extents_size);
        }
        else
        {
            /* double the size of the array */
            nvm_extent *old_extents = nvm_extent_get(&rg->extents);
            nvm_extent *new_extents = nvm_alloc(nvm_heap_get(&rg->rootHeap),
                    shapeof(nvm_extent), 2 * rg->extents_size);

            /* store the new array address transactionally */
            NVM_UNDO(rg->extents);
            nvm_extent_set(&rg->extents, new_extents);
            nvm_flush1(&rg->extents);

            /* Copy the extent descriptions from the old array. Just in case
             * we validate the contents as we copy. Note that there should not
             * be any unallocated entries since the array was full. */
            int cnt;
            nvm_extent *ox = old_extents; // old extent
            nvm_extent *nx = new_extents; // new extent
            for (cnt = 0; cnt < rg->extents_size; ox++, nx++, cnt++)
            {
                nvm_verify(ox, shapeof(nvm_extent));
                uint8_t *addr = void_get(&ox->addr);
                if (!nvm_addr_align(addr))
                    nvms_corruption("Bad pointer to NVM extent", ox, addr);
                if (!nvm_align_check(ox->size))
                    nvms_corruption("Bad extent size", ox, (void*)ox->size);

                /* Initialize the new record. This needs no undo because it is
                 * in freshly allocated space. The entire array will be flushed
                 * at the end. */
                void_set(&nx->addr, addr);
                nx->size = ox->size;
            }

            /* Ensure all we copied becomes persistent. */
            nvm_flushi(new_extents, rg->extents_size * sizeof(nvm_extent));

            /* Return the old extent array to the heap. */
            nvm_free(old_extents);

            /* The first new entry is available */
            ext = new_extents + rg->extents_size;

            /* set the new array size. */
            NVM_UNDO(rg->extents_size);
            rg->extents_size *= 2;
            nvm_flush1(&rg->extents_size);
        }

        /* Commit the allocation of the new extent array. */
        nvm_commit();
        nvm_txend();
    }

    /* we must have found an empty slot by now. */
    if (ext == NULL)
        nvms_corruption("Corrupt region extent array",
            rg, nvm_extent_get(&rg->extents));

    /* We now have an available slot to use for this extent. */
    NVM_UNDO(*ext);
    nvm_extent_set(&ext->addr, extent);
    ext->size = psize;
    nvm_flush(ext, sizeof(*ext));

    /* Increment the count of allocated extents */
    NVM_UNDO(rg->extent_count);
    rg->extent_count++;
    nvm_flush1(&rg->extent_count);

    /* Update the parent on abort operation to remove the new extent if this
     * transaction commits but the parent rolls back. */
    NVM_UNDO(*rctx);
    rctx->offset = offset;
    void_set(&rctx->addr, extent);
    rctx->bytes = psize;
    nvm_flush(rctx, sizeof(*rctx));

    /* Create an on abort operation to delete this extent if we die
     * here. You might think that the on abort operation in the parent
     * transaction would be sufficient to cleanup if we die. However there
     * is a race condition. If the undo is not applied during the same
     * locking of the region mutex, some other thread could allocate the
     * same area of the region file after this nested transaction aborts
     * and before the parent on abort undo locks the region. However the
     * following on abort operation will execute before the lock is
     * dropped. */
    nvm_remx_ctx *fctx = nvm_onabort(nvm_extern_nvm_freex_callback.usid);
    fctx->offset = offset;
    void_set(&fctx->addr, extent);
    fctx->bytes = psize;
    nvm_flush(fctx, sizeof(*fctx));


    /* Call the service layer to add the NVM extent to the region file. */
    if (nvms_alloc_range(rd->handle, offset, psize) == 0)
    {
        nvm_abort(); // abort the nested transaction
        nvm_txend(); // and end it
        return NULL;
    }

    /* Map the new extent into this process */
    if (nvms_map_range(rd->handle, offset, psize) == 0)
    {
        /* Aborting this nested transaction will delete and unmap the
         * new extent when the on_abort operation is called. */
        nvm_abort(); // abort the nested transaction
        nvm_txend(); // and end it
        return NULL;
    }

    /* Commit the nested transaction to make the new extent visible to all
     * threads. */
    nvm_commit();
    nvm_txend();

    /* return success. */
    return extent;
}
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
 * @param[in] addr
 * This is a virtual address somewhere in the extent to delete.
 * 
 * @return 
 * 0 is returned on error and 1 is returned on success.
 * 
 * @par Errors:
 */
#ifdef NVM_EXT
int nvm_remove_extent@( 
        void ^addr   
        )
{
    /* It is an error to use this on a heap managed extent since it does not
     * prevent use of the heap on return. */
    if (nvm_usid_eq(*(nvm_usid*)addr, nvm_usidof(nvm_heap)))
    {
        errno = EINVAL;
        return 0;
    }
    return nvm_remove_extent1(addr);
}
int nvm_remove_extent1@( 
    void ^addr 
    )
{
    /* Get the region address from the transaction */
    nvm_thread_data *td = nvm_get_thread_data();
    nvm_region ^rg = (void^)td->region;

    /* Verify extent is in the same region as the current transaction */
    if (!nvm_region_ptr(rg, addr))
        return 0;

    /* Do not allow the base extent to be deleted */
    if (addr == (void^)rg)
    {
        errno = EINVAL;
        return 0;
    }

    /* Share lock the region to prevent the extent table from being reallocated
     * while we are looking at it. Create a savepoint to let us drop the lock */
    nvm_savepoint(addr);
    nvm_slock((void*)%rg=>reg_mutex);

    /* Scan the extent array to verify the extent address is valid. */
    int cnt;
    nvm_extent ^ext = 0;
    nvm_extent ^xp = rg=>extents;
    if ((xp == 0 && rg=>extents_size != 0) ||
        rg=>extent_count > rg=>extents_size)
        nvms_corruption("Corrupt region extent count or array",
            rg, rg=>extents);
    for (cnt = 0; cnt < rg=>extents_size; xp++, cnt++)
    {
        uint8_t ^xbase = xp=>addr;
        if (xbase == addr)
            ext = xp;
    }

    /* If no extent found return an error */
    if (ext == 0)
    {
        nvm_rollback(addr);
        errno = EINVAL;
        return 0;
    }

    /* All done reading extent table so drop the lock */
    nvm_rollback(addr);

    /* The extent was found so create an on commit operation to delete it.
     * We do not simply delete it now because abort of the current transaction
     * expects the extent to exist including its current contents. */
    uint8_t ^extent = addr;
    nvm_remx_ctx ^ctx = nvm_oncommit(|nvm_remx_callback);
    ctx=>offset ~= extent - (uint8_t^)rg;
    ctx=>addr ~= extent;
    ctx=>bytes ~= ext=>size; // delete whole extent
    return 1;
}
#else
int nvm_remove_extent(   
        void *addr    
        )
{
    /* It is an error to use this on a heap managed extent since it does not
     * prevent use of the heap on return. */
    if (nvm_usid_eq(*(nvm_usid*)addr, nvm_usidof(nvm_heap)))
    {
        errno = EINVAL;
        return 0;
    }
    return nvm_remove_extent1(addr);
}
int nvm_remove_extent1(   
        void *addr    
        )
{
    /* Get the region address from the transaction */
    nvm_thread_data *td = nvm_get_thread_data();
    nvm_region *rg = td->region;
    nvm_verify(rg, shapeof(nvm_region));

    /* Verify extent is in the same region as the current transaction */
    if (!nvm_region_ptr(td->region, addr))
        return 0;

    /* Do not allow the base extent to be deleted */
    if (addr == (void*)rg)
    {
        errno = EINVAL;
        return 0;
    }

    /* Share lock the region to prevent the extent table from being reallocated
     * while we are looking at it. Create a savepoint to let us drop the lock */
    nvm_savepoint(addr);
    nvm_slock(&rg->reg_mutex);

    /* Scan the extent array to verify the extent address is valid. */
    int cnt;
    nvm_extent *ext = NULL;
    nvm_extent *xp = nvm_extent_get(&rg->extents);
    if ((xp == NULL && rg->extents_size != 0) ||
        rg->extent_count > rg->extents_size)
        nvms_corruption("Corrupt region extent count or array",
            rg, nvm_extent_get(&rg->extents));
    for (cnt = 0; cnt < rg->extents_size; xp++, cnt++)
    {
        nvm_verify(xp, shapeof(nvm_extent));
        uint8_t *xbase = void_get(&xp->addr);
        if (xbase == addr)
            ext = xp;
    }

    /* If no extent found return an error */
    if (ext == NULL)
    {
        nvm_rollback(addr);
        errno = EINVAL;
        return 0;
    }
    
    /* All done reading extent table so drop the lock */
    nvm_rollback(addr);

    /* The extent was found so create an on commit operation to delete it.
     * We do not simply delete it now because abort of the current transaction
     * expects the extent to exist including its current contents. */
    uint8_t *extent = addr;
    nvm_remx_ctx *ctx = nvm_oncommit(nvm_extern_nvm_remx_callback.usid);
    ctx->offset = extent - (uint8_t*)rg;
    void_set(&ctx->addr, extent);
    ctx->bytes = ext->size; // delete whole extent
    nvm_flush(ctx, sizeof(*ctx));
    return 1;
}
#endif //NVM_EXT
/**
 * This function changes the size of an existing application managed 
 * extent. This must be called within a transaction. If the transaction
 * rolls back the resize will also rollback.
 * 
 * The address range for the extent must not overlap any existing extent. 
 * It must be within the NVM virtual address space reserved for the region. 
 * The new size must be a multiple of a page size and not zero. This must not
 * be a heap managed extent.
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
 * 0 is returned on error and 1 is returned on success.
 * 
 * @par Errors:
 */
#ifdef NVM_EXT
int nvm_resize_extent@( 
        void ^extent,   
        size_t psize
        )
{
    /* It is an error to use this on a heap managed extent since it does not
     * adjust the freelist data or check for available space. */
    if (nvm_usid_eq(^(nvm_usid^)extent, nvm_usidof(nvm_heap)))
    {
        errno = EINVAL;
        return 0;
    }
    return nvm_resize_extent1(extent, psize);
}
int nvm_resize_extent1@( 
        void ^extent,   
        size_t psize
        )
{
    /* validate alignment of the requested size */
    if (!nvm_align_check(psize))
    {
        errno = EINVAL;
        return 0;
    }

    /* get app, thread and process data pointers */
    nvm_app_data *ad = nvm_get_app_data();
    nvm_thread_data *td = nvm_get_thread_data();

    /* Verify extent is in the same region as the current transaction */
    if (!nvm_region_ptr((void^)td->region, extent))
        return 0;

    /* Get the region descriptor from the transaction */
    nvm_desc desc = nvm_txdesc();

    /*  get the region data. */
    nvm_region_data *rd = ad->regions[desc];

    /* We do not yet know if this is going to grow or shrink the extent. We
     * act like it is going to grow because that requires making changes here
     * in a nested transaction that has the region locked. If it turns out that
     * this is actually a shrink, the transaction can be cleaned up. We would
     * have to start all over again if we acted like a shrink, but it was a 
     * grow. Note that growing is much more common than shrinking. */

    /* Create an on abort operation to restore the current size if growing.
     * This will be applied if the caller's transaction aborts after extent
     * grow succeeds. If we do not grow then this will be a no-op because the
     * arguments will be left as zero. */
    nvm_remx_ctx ^rctx = nvm_onabort(|nvm_remx_callback);

    /* Begin a nested transaction to grow the extent. This will commit the 
     * extent to grow so we  map it in before we return. If this turns out to
     * be a shrink, this nested transaction will be aborted without modifying
     * anything. */
    nvm_region ^rg = rd->region;
    nvm_extent ^ext = 0;
    @{ //nvm_txbegin(0);

        /* Lock the region to prevent the extent table from being reallocated
         * while we are looking at it. We get an X lock because we will modify
         * the extent in this transaction if it is growing. We only need an 
         *S lock if shrinking since the shrink is done in an on commit
         * operation, but we cannot tell that now. */
        nvm_xlock(%rg=>reg_mutex);

        /* Scan the extent array to verify the extent address is valid, and the
         * new size does not overlap any other extents. */
        {
            int cnt;
            nvm_extent ^xp = rg=>extents;
            if ((xp == 0 && rg=>extents_size != 0) ||
                rg=>extent_count > rg=>extents_size)
                nvms_corruption("Corrupt region extent count or array",
                    rg, rg=>extents);
            for (cnt = 0; cnt < rg=>extents_size; xp++, cnt++)
            {
                uint8_t ^addr = xp=>addr;
                if (addr == extent)
                    ext = xp;

                /* check for overlap */
                if (addr > (uint8_t^)extent && addr < (uint8_t^)extent + psize)
                {
                    nvm_abort(); // drop the lock
                    errno = EINVAL;
                    return 0;
                }
            }
        }

        /* If ext not set, then the extent address is not valid. */
        if (ext == 0)
        {
            nvm_abort(); // drop the lock
            errno = EINVAL;
            return 0;
        }

        /* The extent was found so we can resize it. Resizing to grow is done
         * differently than resize to shrink. If it is already the requested
         * size then nothing to do. */
        if (ext=>size == psize)
        {
            /* Size is already correct */
            nvm_abort(); // drop the lock
            return 1;
        }

        /* If this is a shrink then add an on commit operation to shrink the 
         * extent when the caller's transaction commits. */
        if (ext=>size > psize)
        {
            /* Abort the nested transaction and exit the transaction code 
             * block so the caller's transaction is current. */
            nvm_abort();
            goto shrink;
        }

        /* The extent is growing. Get the offset, address and size of the new
         * NVM to be added */
        uint8_t ^addr = (uint8_t^)extent + ext=>size;
        off_t offset = addr - (uint8_t^)rg;
        size_t bytes = psize - ext=>size;

        /* Update the parent on abort operation to shrink back the extent if this
         * transaction commits but the parent rolls back. */
        rctx=>offset @= offset;
        rctx=>addr @= addr;
        rctx=>bytes @= bytes;

        /* Create an on abort operation to shrink back this extent if we die
         * here. You might think that the on abort operation in the parent
         * transaction would be sufficient to cleanup if we die. However there
         * is a race condition. If the undo is not applied during the same
         * locking of the region mutex, some other thread could allocate the
         * same area of the region file after this nested transaction aborts
         * and before the parent on abort undo locks the region. However the
         * following on abort operation will execute before the lock is
         * dropped. */
        nvm_remx_ctx ^fctx = (void^)nvm_onabort(|nvm_freex_callback);
        fctx=>offset ~= offset;
        fctx=>addr ~= addr;
        fctx=>bytes ~= bytes;

        /* Set the new size. */
        ext=>size @= psize;

        /* Call the service layer to add the NVM to the region file. This could
         * fail for lack of space. */
        if (nvms_alloc_range(rd->handle, offset, bytes) == 0)
        {
            nvm_abort(); // abort the nested transaction
            return 0;
        }

        /* Now that it is part of the region map it into this process */
        if (nvms_map_range(rd->handle, offset, bytes) == 0)
        {
            nvm_abort(); // abort the nested transaction
            return 0;
        }

        /* Commit the nested transaction to make the new extent size visible. */
        nvm_commit();
    } //nvm_txend();

    /* return success. */
    return 1;
    
    /* Create the on commit operation in the caller's transaction
     * to shrink the extent when it commits. We put this code here to
     * end the nested transaction. */
shrink:;
    nvm_remx_ctx ^ctx = nvm_oncommit(|nvm_remx_callback);
    ctx=>offset ~= (uint8_t^)extent - (uint8_t^)rg + psize;
    ctx=>addr ~= (uint8_t^)extent + psize;
    ctx=>bytes ~= ext=>size - psize; // delete excess bytes

    return 1;
}
#else
int nvm_resize_extent(   
        void *extent,    
        size_t psize
        )
{
    /* It is an error to use this on a heap managed extent since it does not
     * adjust the freelist data or check for available space. */
    if (nvm_usid_eq(*(nvm_usid*)extent, nvm_usidof(nvm_heap)))
    {
        errno = EINVAL;
        return 0;
    }
    return nvm_resize_extent1(extent, psize);
}
int nvm_resize_extent1(   
        void *extent,    
        size_t psize
        )
{
    /* validate alignment of the requested size */
    if (!nvm_align_check(psize))
    {
        errno = EINVAL;
        return 0;
    }

    /* get app, thread and process data pointers */
    nvm_app_data *ad = nvm_get_app_data();
    nvm_thread_data *td = nvm_get_thread_data();

    /* Verify extent is in the same region as the current transaction */
    if (!nvm_region_ptr(td->region, extent))
        return 0;

    /* Get the region descriptor from the transaction */
    nvm_desc desc = nvm_txdesc();

    /*  get the region data. */
    nvm_region_data *rd = ad->regions[desc];

    /* We do not yet know if this is going to grow or shrink the extent. We
     * act like it is going to grow because that requires making changes here
     * in a nested transaction that has the region locked. If it turns out that
     * this is actually a shrink, the transaction can be cleaned up. We would
     * have to start all over again if we acted like a shrink, but it was a 
     * grow. Note that growing is much more common than shrinking. */

    /* Create an on abort operation to restore the current size if growing.
     * This will be applied if the caller's transaction aborts after extent
     * grow succeeds. If we do not grow then this will be a no-op because the
     * arguments will be left as zero. */
    nvm_remx_ctx *rctx = nvm_onabort(nvm_extern_nvm_remx_callback.usid);

    /* Begin a nested transaction to grow the extent. This will commit the 
     * extent to grow so we can map it in before we return. If this turns out
     * to be a shrink, this nested transaction will be aborted without
     * modifying anything. */
    nvm_txbegin(0);

    /* Lock the region to prevent the extent table from being reallocated
     * while we are looking at it. We get an X lock because we will modify
     * the extent in this transaction if it is growing. We only need an S lock
     * if shrinking since the shrink is done in an on commit operation, but we
     * cannot tell that now. */
    nvm_region *rg = rd->region;
    nvm_verify(rg, shapeof(nvm_region));
    nvm_xlock(&rg->reg_mutex);

    /* Scan the extent array to verify the extent address is valid, and the
     * new size does not overlap any other extents. */
    nvm_extent *ext = NULL;
    {
        int cnt;
        nvm_extent *xp = nvm_extent_get(&rg->extents);
        if ((xp == NULL && rg->extents_size != 0) ||
            rg->extent_count > rg->extents_size)
            nvms_corruption("Corrupt region extent count or array",
                rg, nvm_extent_get(&rg->extents));
        for (cnt = 0; cnt < rg->extents_size; xp++, cnt++)
        {
            nvm_verify(xp, shapeof(nvm_extent));
            uint8_t *addr = void_get(&xp->addr);
            if (addr == extent)
                ext = xp;

            /* check for overlap */
            if (addr > (uint8_t*)extent && addr < (uint8_t*)extent + psize)
            {
                nvm_abort();
                nvm_txend();
                errno = EINVAL;
                return 0;
            }
        }
    }

    /* If ext not set, then the extent address is not valid. */
    if (ext == NULL)
    {
        nvm_abort();
        nvm_txend();
        errno = EINVAL;
        return 0;
    }

    /* The extent was found so we can resize it. Resizing to grow is done
     * differently than resize to shrink. If it is already the requested
     * size then nothing to do. */
    if (ext->size == psize)
    {
        /* Size is already correct */
        nvm_abort(); // drop the lock
        nvm_txend();
        return 1;
    }

    /* If this is a shrink then add an on commit operation to shrink the 
     * extent when the caller's transaction commits. */
    if (ext->size > psize)
    {
        /* Abort the nested transaction so the caller's transaction is
         * current. */
        nvm_abort();
        nvm_txend();

        /* Create the on commit operation to shrink */
        nvm_remx_ctx *ctx = nvm_oncommit(nvm_extern_nvm_remx_callback.usid);
        ctx->offset = (uint8_t*)extent - (uint8_t*)rg + psize;
        void_set(&ctx->addr, (uint8_t*)extent + psize);
        ctx->bytes = ext->size - psize; // delete excess bytes

        return 1;
    }

    /* The extent is growing. Get the offset, address and size of the new
     * NVM to be added */
    uint8_t *addr = (uint8_t*)extent + ext->size;
    off_t offset = addr - (uint8_t*)rg;
    size_t bytes = psize - ext->size;

    /* Update the parent on abort operation to shrink back the extent if this
     * transaction commits but the parent rolls back. */
    NVM_UNDO(*rctx);
    rctx->offset = offset;
    void_set(&rctx->addr, addr);
    rctx->bytes = bytes;
    nvm_flush(rctx, sizeof(*rctx));

    /* Create an on abort operation to shrink back this extent if we die
     * here. You might think that the on abort operation in the parent
     * transaction would be sufficient to cleanup if we die. However there
     * is a race condition. If the undo is not applied during the same
     * locking of the region mutex, some other thread could allocate the
     * same area of the region file after this nested transaction aborts
     * and before the parent on abort undo locks the region. However the
     * following on abort operation will execute before the lock is
     * dropped. */
    nvm_remx_ctx *fctx = nvm_onabort(nvm_extern_nvm_freex_callback.usid);
    fctx->offset = offset;
    void_set(&fctx->addr, addr);
    fctx->bytes = bytes;
    nvm_flush(fctx, sizeof(*fctx));

    /* Set the new size. */
    NVM_UNDO(ext->size);
    ext->size = psize;
    nvm_flush1(&ext->size);

    /* Call the service layer to add the NVM to the region file. This could
     * fail for lack of space. */
    if (nvms_alloc_range(rd->handle, offset, bytes) == 0)
    {
        nvm_abort(); // abort the nested transaction
        nvm_txend(); // and end it
        return 0;
    }

    /* Now that it is part of the region map it into this process */
    if (nvms_map_range(rd->handle, offset, bytes) == 0)
    {
        nvm_abort(); // abort the nested transaction
        nvm_txend(); // and end it
        return 0;
    }

    /* Commit the nested transaction to make the new extent size visible. */
    nvm_commit();
    nvm_txend();

    /* return success. */
    return 1;
}
#endif //NVM_EXT
/* qsort callback */
static int compar(const void *v1, const void *v2)
{
    const nvm_extent_stat *x1 = v1;
    const nvm_extent_stat *x2 = v2;
    if (x1->extent < x2->extent)
        return -1;
    if (x1->extent > x2->extent)
        return 1;
    return 0;
}
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
        nvm_desc desc,
        int first, // index of first extent to report
        int count, // number of extents to report
        nvm_extent_stat stat[] // array of count statuses to fill in
        )
{
    /* validate arguments */
    if (first < 0 || count < 0)
    {
        errno = EINVAL;
        return 0;
    }

    /* if desc is zero then use the current transaction's region. */
    nvm_thread_data *td = nvm_get_thread_data();
    if (td->txdepth && desc == 0)
    {
        desc = td->transaction=>desc;
    }

    /* validate the descriptor is good, and get the region data. */
    nvm_app_data *ad = nvm_get_app_data();
    if (desc < 1 || desc > ad->params.max_regions)
    {
        errno = EINVAL;
        return 0;
    }
    nvm_region_data *rd = ad->regions[desc];
    if (rd == 0)
    {
        errno = EINVAL;
        return 0;
    }

    /* Get the nvm_region being queried. */
    nvm_region ^rg = rd->region;

    /* Share lock the region to ensure the extent status is stable. We begin a
     * transaction just to hold the lock. This might be a nested transaction
     * or a base transaction. */
    nvm_extent_stat *all;
    size_t saved = 1;
    int x;
    @ desc { //nvm_txbegin(desc);
        nvm_slock(%rg=>reg_mutex);

        /* Allocate a thread local array for all the extent stats. */
        all = (nvm_extent_stat *)nvms_thread_alloc(
                sizeof(nvm_extent_stat) * (1 + rg=>extents_size));

        /* Construct the stat for the base extent. */
        all[0].region = desc;
        all[0].extent = rg;
        all[0].psize = rg=>header.psize;
        all[0].heap = (void*)rg=>rootHeap;

        /* get all the stats from the extent data in NVM */
        nvm_extent ^ext = rg=>extents;
        for (x = 0; x < rg=>extents_size; x++, ext++)
        {
            /* skip extents that are not in use. */
            if (ext=>size == 0)
                continue;

            /* ensure we do not overrun the stat buffer. */
            if (saved == 1 + rg=>extent_count)
                nvms_corruption("More than extent_count extents found", rg, ext);

            /* create an nvm_extent_stat for the extent */
            all[saved].region = desc;
            nvm_heap ^addr = ext=>addr;
            all[saved].extent = addr;
            all[saved].psize = ext=>size;

            /* If the extent starts with an nvm_heap then it is heap
             * managed, and the extent address is the  heap address. */
            if (nvm_usid_eq(^(nvm_usid^)addr, nvm_usidof(nvm_heap)))
            {
                /* get the base heap address  */
                all[saved].heap = addr;
            }
            else
            {
                /* application managed */
                all[saved].heap = 0;
            }
            saved++;
        }

        /* End the transaction so that the lock is released. */
        nvm_commit();
    } //nvm_txend();

    /* Sort the extent stats by address */
    qsort(all, saved, sizeof(nvm_extent_stat), &compar);

    /* Copy the requested entries to the caller. First zero entries in case
     * there are some that do not exist. */
    memset(stat, 0, sizeof(nvm_extent_stat) * count);
    if (count + first > saved) // if more extents requested than exist
        count = saved - first; // might be negative if nothing to return
    for (x = 0; x < count; x++)
        stat[x] = all[x + first];

    /* done with temp array */
    nvms_thread_free(all);
    return 1;
}
#else
int nvm_query_extents(
        nvm_desc desc,
        int first, // index of first extent to report
        int count, // number of extents to report
        nvm_extent_stat stat[] // array of count statuses to fill in
        )
{
    /* validate arguments */
    if (first < 0 || count < 0)
    {
        errno = EINVAL;
        return 0;
    }

    /* if desc is zero then use the current transaction's region. */
    nvm_thread_data *td = nvm_get_thread_data();
    if (td->txdepth && desc == 0)
    {
        desc = td->transaction->desc;
    }

    /* validate the descriptor is good, and get the region data. */
    nvm_app_data *ad = nvm_get_app_data();
    if (desc < 1 || desc > ad->params.max_regions)
    {
        errno = EINVAL;
        return 0;
    }
    nvm_region_data *rd = ad->regions[desc];
    if (rd == 0)
    {
        errno = EINVAL;
        return 0;
    }

    /* Share lock the region to ensure the extent status is stable. This might
     * start a nested transaction or a base transaction. */
    nvm_region *rg = rd->region;
    nvm_verify(rg, shapeof(nvm_region));
    nvm_txbegin(desc);
    nvm_slock(&rg->reg_mutex);

    /* Allocate a thread local array for all the extent stats. */
    nvm_extent_stat *all = (nvm_extent_stat *)nvms_thread_alloc(
            sizeof(nvm_extent_stat) * (1 + rg->extents_size));

    /* Construct the stat for the base extent. */
    all[0].region = desc;
    all[0].extent = rg;
    all[0].psize = rg->header.psize;
    all[0].heap = nvm_heap_get(&rg->rootHeap);

    /* get all the stats from the extent data in NVM */
    nvm_extent *ext = nvm_extent_get(&rg->extents);
    int saved = 1;
    int x;
    for (x = 0; x < rg->extents_size; x++, ext++)
    {
        /* skip extents that are not in use. */
        nvm_verify(ext, shapeof(nvm_extent));
        if (ext->size == 0)
            continue;

        /* ensure we do not overrun the stat buffer. */
        if (saved == 1 + rg->extent_count)
            nvms_corruption("More than extent_count extents found", rg, ext);

        /* create an nvm_extent_stat for the extent */
        all[saved].region = desc;
        nvm_heap *addr = void_get(&ext->addr);
        all[saved].extent = addr;
        all[saved].psize = ext->size;

        /* If the extent starts with an nvm_heap then it is heap
         * managed, and the extent address is the  heap address. */
        if (nvm_usid_eq(addr->type_usid, nvm_usidof(nvm_heap)))
        {
            /* get the base heap address from the header */
            all[saved].heap = addr;
        }
        else
        {
            /* application managed */
            all[saved].heap = NULL;
        }
        saved++;
    }

    /* End the transaction so that the lock is released. */
    nvm_commit();
    nvm_txend();

    /* Sort the extent stats by address */
    qsort(all, saved, sizeof(nvm_extent_stat), &compar);

    /* Copy the requested entries to the caller. First zero entries in case
     * there are some that do not exist. */
    memset(stat, 0, sizeof(nvm_extent_stat) * count);
    if (count + first > saved) // if more extents requested than exist
        count = saved - first; // might be negative if nothing to return
    for (x = 0; x < count; x++)
        stat[x] = all[x + first];

    /* done with temp array */
    nvms_thread_free(all);
    return 1;
}
#endif //NVM_EXT

/*---------------------------- nvm_map_region ------------------------------*/
/**
 * This is the code that is common to create region and attach region. It maps
 * the base extent of the region file into the current process and adds 
 * nvm_region_data to the application.
 * 
 * The application data mutex is locked if this call returns with success.
 * The caller is responsible for unlocking the mutex.
 *
 * @param[in] pathname
 * This is the pathname used to create the handle.
 * 
 * @param[in] handle
 * This it the handle returned by the service library on open or create region.
 * 
 * @param[in] attach
 * The address where the region file is to be mapped.
 * 
 * @param[in] desc
 * The descriptor to use for this region, or zero to choose an available 
 * descriptor.
 * 
 * @param[in] create
 * True if called from create region so the USID should only be half there
 * 
 * @return 
 * The descriptor for the region if successful and 0 on failure
 */
#ifdef NVM_EXT
nvm_desc nvm_map_region(
        const char *pathname,
        nvms_file *handle,
        void *attach,
        nvm_desc desc,
        int create
        )
{
    /* get application data */
    nvm_app_data *ad = nvm_get_app_data();

    /* Error if requested descriptor is too large */
    nvm_desc rmax = ad->params.max_regions;
    if (desc > rmax)
    {
        errno = EINVAL;
        return 0;
    }

    /* Read the region file header. */
    nvm_region_init init;
    if (nvms_read_region(handle, &init, sizeof(init)) == 0)
        return 0;

    /* Verify this is a region file starting with the nvm_region USID */
    if (!nvm_usid_eq(init.usid, nvm_usidof(nvm_region)))
    {
        errno = EBADF;
        return 0;
    }

    /* If this is creation, then the root object pointer must be zero,
     * otherwise it must be set. */
    if (create)
    {
        if (init.header.rootObject)
        {
            errno = EBADF;
            return 0;
        }
    }
    else
    {
        if (!init.header.rootObject)
        {
            errno = EBADF;
            return 0;
        }
    }
    //TODO add code to verify this is not a double map by looking at attach_id

    /* hold the nvm_app_data mutex while updating the region data */
    nvms_lock_mutex(ad->mutex, 1);

    /* Use the descriptor requested if it is not zero, else allocate a 
     * descriptor for this region */
    if (desc != 0)
    {
        /* verify requested descriptor is available */
        if (ad->regions[desc] != 0)
        {
            nvms_unlock_mutex(ad->mutex);
            errno = EBUSY;
            return 0;
        }
    }
    else
    {
        /* find first available descriptor */
        while (ad->regions[++desc] != 0)
        {
            if (desc > rmax)
            {
                nvms_unlock_mutex(ad->mutex);
                errno = EMFILE;
                return 0;
            }
        }

    }

    /* Allocate region data. */
    nvm_region_data *rd = (nvm_region_data *)
            nvms_app_alloc(sizeof(nvm_region_data));
    if (rd == 0)
    {
        int serr = errno;
        nvms_unlock_mutex(ad->mutex);
        errno = serr;
        return 0;
    }
    rd->attach_id = nvms_unique_id();
    rd->region = (nvm_region^)attach;
    rd->desc = desc;
    rd->vsize = init.header.vsize;
    rd->psize = init.header.psize;
    rd->handle = handle; // handle to open region file
    rd->trans_table = 0; // recovery not complete
    rd->noRoot = create; // no root object if just creating

    /* Create the mutex for the region data. We do not need to lock it since
     * no other thread knows about this region data yet. */
    rd->mutex = nvms_create_mutex();

    /* Add the path name to the region data. */
    rd->pathname = nvms_app_alloc(strlen(pathname) + 1);
    if (rd->pathname == 0)
    { /* cleanup allocations and return failure */
        int serr = errno;
        nvms_destroy_mutex(rd->mutex);
        nvms_app_free(rd);
        nvms_unlock_mutex(ad->mutex);
        errno = serr;
        return 0;
    }
    strcpy(rd->pathname, pathname);

    /* Add a waiter table to the region data for threads that need to block
     * waiting for an nvm_mutex. */
    if (!nvm_wait_table_create(rd, ad->params.wait_buckets))
    {
        /* cleanup allocations and return failure */
        int serr = errno;
        nvms_destroy_mutex(rd->mutex);
        nvms_app_free(rd->pathname);
        nvms_app_free(rd);
        nvms_unlock_mutex(ad->mutex);
        errno = serr;
        return 0;
    }

    /* Attempt to map the base extent into this process so that we can
     * complete create/attach. */
    if (!nvms_map_region(handle, attach, init.header.vsize, init.header.psize))
    {
        /* return mapping error to caller. */
        int serr = errno;
        nvms_destroy_mutex(rd->mutex);
        nvms_app_free(rd->pathname);
        nvms_app_free(rd);
        nvms_unlock_mutex(ad->mutex);
        errno = serr;
        return 0;
    }

    /* Increment attach count so that the application can recognize stale
     * transient data. */
    nvm_region ^rg = rd->region;
    rg=>attach_cnt~++;

    /* Store a new attach id in the nvm_region so that we can detect a
     * duplicate attach. */
    rg=>header.attach_id = rd->attach_id;

    /* Save the descriptor in a transient for reporting to application */
    rg=>desc = desc;

    /* If this is not creation of a new region then there may be extents
     * that need to be mapped in.
     */
    int x;
    nvm_extent ^rext = rg=>extents;
    if (rext) for (x = 0; x < rg=>extents_size; x++, rext++)
    {
        uint8_t ^addr = rext=>addr;
        if (!addr)
            continue;
        if (nvms_map_range(rd->handle, addr - (uint8_t^)rg, rext=>size) == NULL)
        {
            /* Return mapping error to caller after unmapping anything that
             * was mapped in and freeing all allocations. */
            int serr = errno;
            nvms_unmap_range(rd->handle, 0, rd->vsize); // unmap all
            nvms_destroy_mutex(rd->mutex);
            nvms_app_free(rd->pathname);
            nvms_app_free(rd);
            nvms_unlock_mutex(ad->mutex);
            errno = serr;
            return 0;
        }
    }

    /* make sure descriptor is not in use now */
    if (ad->regions[desc])
        nvms_assert_fail("Descriptor became used while attaching");

    /* Make the region data globally visible by storing it in the
     * application data. */
    ad->regions[desc] = rd;

    /* Return success with the application data mutex still locked */
    return desc;
}
#else
nvm_desc nvm_map_region(
        const char *pathname,
        nvms_file *handle,
        void *attach,
        nvm_desc desc,
        int create
        )
{
    /* get application data */
    nvm_app_data *ad = nvm_get_app_data();

    /* Error if requested descriptor is too large */
    nvm_desc rmax = ad->params.max_regions;
    if (desc > rmax)
    {
        errno = EINVAL;
        return 0;
    }

    /* Read the region file header. */
    nvm_region_init init;
    if (nvms_read_region(handle, &init, sizeof(init)) == 0)
        return 0;


    /* Verify this is a region file starting with the nvm_region USID */
    if (!nvm_usid_eq(init.usid, nvm_usidof(nvm_region)))
    {
        errno = EBADF;
        return 0;
    }

    /* If this is creation, then the root object pointer must be null,
     * otherwise it must be set. */
    if (create)
    {
        if (init.header.rootObject)
        {
            errno = EBADF;
            return 0;
        }
    }
    else
    {
        if (!init.header.rootObject)
        {
            errno = EBADF;
            return 0;
        }
    }
    //TODO add code to verify this is not a double map by looking at attach_id

    /* hold the nvm_app_data mutex while updating the region data */
    nvms_lock_mutex(ad->mutex, 1);

    /* Use the descriptor requested if it is not zero, else allocate a 
     * descriptor for this region */
    if (desc != 0)
    {
        /* verify requested descriptor is available */
        if (ad->regions[desc] != 0)
        {
            nvms_unlock_mutex(ad->mutex);
            errno = EBUSY;
            return 0;
        }
    }
    else
    {
        /* find first available descriptor */
        while (ad->regions[++desc] != 0)
        {
            if (desc > rmax)
            {
                nvms_unlock_mutex(ad->mutex);
                errno = EMFILE;
                return 0;
            }
        }

    }

    /* Allocate region data. */
    nvm_region_data *rd = (nvm_region_data *)
         nvms_app_alloc(sizeof(nvm_region_data));
    if (rd == 0)
    {
        int serr = errno;
        nvms_unlock_mutex(ad->mutex);
        errno = serr;
        return 0;
    }
    rd->attach_id = nvms_unique_id();
    rd->region = attach;
    rd->desc = desc;
    rd->vsize = init.header.vsize;
    rd->psize = init.header.psize;
    rd->handle = handle; // handle to open region file
    rd->trans_table = 0; // recovery not complete
    rd->noRoot = create; // no root object if just creating

    /* Create the mutex for the region data. We do not need to lock it since
     * no other thread knows about this region data yet. */
    rd->mutex = nvms_create_mutex();

    /* Add the path name to the region data. */
    rd->pathname = nvms_app_alloc(strlen(pathname) + 1);
    if (rd->pathname == 0)
    { /* cleanup allocations and return failure */
        int serr = errno;
        nvms_destroy_mutex(rd->mutex);
        nvms_app_free(rd);
        nvms_unlock_mutex(ad->mutex);
        errno = serr;
        return 0;
    }
    strcpy(rd->pathname, pathname);

    /* Add a waiter table to the region data for threads that need to block
     * waiting for an nvm_mutex. */
    if (!nvm_wait_table_create(rd, ad->params.wait_buckets))
    {
        /* cleanup allocations and return failure */
        int serr = errno;
        nvms_destroy_mutex(rd->mutex);
        nvms_app_free(rd->pathname);
        nvms_app_free(rd);
        nvms_unlock_mutex(ad->mutex);
        errno = serr;
        return 0;
    }

    /* Attempt to map the base extent into this process so that we can
     * complete create/attach. */
    if (!nvms_map_region(handle, attach, init.header.vsize, init.header.psize))
    {
        /* return mapping error to caller */
        int serr = errno;
        nvms_destroy_mutex(rd->mutex);
        nvms_app_free(rd->pathname);
        nvms_app_free(rd);
        nvms_unlock_mutex(ad->mutex);
        errno = serr;
        return 0;
    }

    /* Increment attach count so that the application can recognize stale
     * transient data. */
    nvm_region *rg = rd->region;
    rg->attach_cnt++;
    nvm_flush1(&rg->attach_cnt);

    /* Store a new attach id in the nvm_region so that we can detect a
     * duplicate attach. */
    rg->header.attach_id = rd->attach_id;

    /* Save the descriptor in a transient for reporting to application */
    rg->desc = desc;

    /* If this is not creation of a new region then there may be extents
     * that need to be mapped in.
     */
    int x;
    nvm_extent *rext = nvm_extent_get(&rg->extents);
    if (rext) for (x = 0; x < rg->extents_size; x++, rext++)
    {
        nvm_verify(rext, shapeof(nvm_extent));
        uint8_t *addr = void_get(&rext->addr);
        if (!addr)
            continue;
        if (nvms_map_range(rd->handle, addr - (uint8_t*)rg, rext->size) == NULL)
        {
            /* Return mapping error to caller after unmapping anything that
             * was mapped in and freeing all allocations. */
            int serr = errno;
            nvms_unmap_range(rd->handle, 0, rd->vsize); // unmap all
            nvms_destroy_mutex(rd->mutex);
            nvms_app_free(rd->pathname);
            nvms_app_free(rd);
            nvms_unlock_mutex(ad->mutex);
            errno = serr;
            return 0;
        }
    }

    /* make sure descriptor is not in use now */
    if (ad->regions[desc])
        nvms_assert_fail("Descriptor became used while attaching");

    /* Make the region data globally visible by storing it in the
     * application data. */
    ad->regions[desc] = rd;

    /* Return success with the application data mutex still locked */
    return desc;
}
#endif //NVM_EXT


/*--------------------------- nvm_unmap_region -----------------------------*/
/**
 * This removes a region from the application and unmaps it from the
 * process. Nothing is done to ensure the region is not in use. The region may
 * contain active transactions that need recovery at next attach.
 * 
 * No errors are returned since this is releasing resources not acquiring.
 * 
 * @param[in] desc
 * The descriptor for the region to unmap
 */
void nvm_unmap_region(nvm_desc desc)
{
    nvm_app_data *ad = nvm_get_app_data();

    /* We are modifying the application data so we need the application
     * mutex */
    nvms_lock_mutex(ad->mutex, 1);

    /* If the region data is no longer, then nothing to do */
    nvm_region_data *rd = ad->regions[desc];
    if (rd == 0)
    {
        nvms_unlock_mutex(ad->mutex);
        return;
    }

    /* Clear the pointer to the region data. So it is no longer accessible */
    ad->regions[desc] = 0;

    /* done with application data mutex */
    nvms_unlock_mutex(ad->mutex);

    /* Remove the region file from this process's address space */
    if (!nvms_unmap_range(rd->handle, (size_t)0, rd->vsize))
        nvms_assert_fail("Error unmapping a region file");

    /* Release the pathname  */
    if (!nvms_app_free(rd->pathname))
        nvms_assert_fail("Unable to free volatile region data");

    /* Release the transaction table if allocated */
    if (!nvm_trans_free_table(&rd->trans_table))
        nvms_assert_fail("Unable to free volatile region data");

    /* Release the wait table used to wait for NVM mutexes. */
    if (!nvm_wait_table_destroy(rd))
        nvms_assert_fail("Unable to free volatile region data");

    /* Destroy the region mutex  */
    nvms_destroy_mutex(rd->mutex);

    /* Release the region data itself */
    if (!nvms_app_free(rd))
        nvms_assert_fail("Unable to free volatile region data");
}
