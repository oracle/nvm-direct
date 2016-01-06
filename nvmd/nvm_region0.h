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
      nvm_region0.h - private NVM region management declarations

    DESCRIPTION\n
      This contains the declarations for NVM region management that are private
      to the NVM library.
 */

#ifndef NVM_REGION0_H
#define	NVM_REGION0_H

#ifndef NVM_LOCKS0_H
#include "nvm_locks0.h"
#endif

#ifndef NVMS_REGION_H
#include "nvms_region.h"
#endif

#ifndef NVMS_LOCKING_H
#include "nvms_locking.h"
#endif

#include <errno.h>

#ifdef	__cplusplus
extern "C"
{
#endif

    /* forward declarations */
#ifdef NVM_EXT
    typedef persistent struct nvm_trans_table nvm_trans_table;
#else
    extern const nvm_type nvm_type_nvm_trans_table; //
    extern const nvm_type nvm_type_nvm_heap; //
    extern const nvm_type nvm_type_nvm_mutex; //
    extern const nvm_type nvm_type_nvm_mutex_array; //
    typedef struct nvm_trans_table nvm_trans_table;
    NVM_SRP(nvm_trans_table)
#endif //NVM_EXT
    

    /**\brief One contiguous extent of NVM 
     *
     * An NVM region always starts with an initial extent of NVM at offset
     * zero in the region file. There can be additional extents at other
     * offsets within the region. An extent can be managed as a heap, or it
     * can be used to hold client data such as file data. This is called 
     * application managed.
     */
#ifdef NVM_EXT
    persistent struct nvm_extent
    USID("93dd 163c 10b1 0691 fdf7 80fe ccc9 5b0a")
    tag("One extent of a region file")
    version(0)
    {
        /**
         * This points to the beginning of the extent
         */
        void ^addr;

        /**
         * This is the size of the extent in bytes
         */
        size_t size;
    };
    typedef persistent struct nvm_extent nvm_extent;
#else
    struct nvm_extent
    {
        /**
         * The type usid is first
         */
        nvm_usid type_usid; //

        /**
         * This points to the beginning of the extent
         */
        void_srp addr; // void*

        /**
         * This is the size of the extent in bytes
         */
        size_t size;
    };
    typedef struct nvm_extent nvm_extent;
    extern const nvm_type nvm_type_nvm_extent;
    NVM_SRP(nvm_extent);
#endif //NVM_NOEXT

    /**
     * The initial size of the extent map
     */
#define NVM_INITIAL_EXTENTS (64)

    /**
     * This is the minimum amount of information needed to map an NVM region
     * file into a process. It is declared as a separate struct so that it can
     * be allocated in volatile or non-volatile memory. The volatile version
     * is written to the file at creation time so that it can subsequently 
     * mapped into the process to complete creation.
     */
    struct nvm_region_header
    {
        /**
         * This is the minimum amount of virtual address space needed to
         * map this region into a process. This is typically larger than
         * the end of the last extent. Thus there is room for growth.
         */
        uint64_t vsize;

        /**
         * This is the physical size of the base extent which includes the 
         * nvm_region struct. This is in leu of an nvm_extent for extent 0.
         * If the base extent is resized then this value is changed.
         */
        uint64_t psize;

        /**
         * This is the ASCII name of the region. Note that this imposes a 63 
         * byte limit on the name of a region so that this is a null
         * terminated string. Note that this is not the name of the file
         * containing the region. It does not change when the file is copied.
         */
        char name[64];

        /**
         * This is the USID of the root object. It is used to ensure the
         * region was initialized by an application that is compatible with
         * the attaching application. It is a bad file error to attach a
         * region to an application that cannot understand the root object.
         */
        nvm_usid rootUSID;

        /**
         * This is the pointer to the root object. The application knows
         * what type this really is. The root object leads to all the
         * application data. A region file cannot be attached unless the
         * root object has been set to indicate initialization is complete.
         */
#ifdef NVM_EXT
        void ^rootObject;
#else
        void_srp rootObject;
#endif //NVM_EXT

        /**
         * Every time an application attaches to a region a random id is 
         * chosen to identify this attach. The ID is written into the
         * nvm_region and the region data in application global memory. It is
         * used to detect attempts to attach the same region more than once.
         * An assert fires if there is another region that already has the
         * same attach id.
         */
#ifdef NVM_EXT
        transient
        uint64_t attach_id;
#else
        uint64_t attach_id;
#endif //NVM_EXT
        
    };
    typedef struct nvm_region_header nvm_region_header;

    /**
     * This is the portion of the region that is accessed with read/write
     * system calls before it is mapped into a process. It only appears in
     * volatile memory. Note that reading/writing this from/to a region file
     * does not convert the rootObject pointer between absolute and
     * self-relative as one would expect when copying from/to NVM to/from
     * volatile memory.
     */
    struct nvm_region_init
    {
        /**
         * The USID of nvm_region.
         */
        nvm_usid usid;

        /**
         * The header with data needed to map in region
         */
        nvm_region_header header;
    };
    typedef struct nvm_region_init nvm_region_init;

    /**\brief Every NVM region starts with a struct nvm_region
     * 
     * This struct describes the NVM data that appears at the beginning of a
     * region. All data in the NVM region can be reached from this object.
     */
#ifdef NVM_EXT
    persistent struct nvm_region
    USID("eeae 207b 7891 7578 d7ae b6aa 1294 d9a6")
    tag("An NVM library managed region begins with an nvm_region")
    version(0)
    size(1024)
    {
        /**
         * The first bytes of every NVM region created by nvm is the type
         * USID for nvm_region. This is zero until the initialization of the
         * region is complete. This is not in nvm_header because there is no
         * need to check for the volatile pointers being current.
         */
//        nvm_usid type_usid; //# this should not be needed

        /**
         * Immediately following the USID is the header used for mapping in
         * the region file. It is a separate struct so that it can be 
         * initialized with a file write from volatile memory when creating a
         * region.
         */
        nvm_region_header header;

        /**
         * This is the number of times this region has been attached. It can 
         * be used to recognize the first access to an object after the
         * region is attached by a new process.
         */
        uint64_t attach_cnt;

        /**
         * This is no longer in use.
         */
        uint64_t spare;

        /**
         * This is a pointer to an array containing the descriptions of all 
         * the extents that have been allocated in this region other than the 
         * base extent. If there is only the base extent, then this is null.
         * 
         * There are usually some unused entries which have a NULL pointer.
         * Note that the entries are not sorted.
         */
        nvm_extent ^extents;

        /**
         * This is the number of entries in the extents array, but not the 
         * number of extents currently allocated. There may be null entries
         * that are unused.
         */
        uint32_t extents_size;

        /* This is the actual number of extents allocated. It is never greater
         * than extents _size. */
        uint32_t extent_count;

        /**
         * This points to the linked list of nvm_trans_table objects
         * allocated in this region. The nvm_trans_data objects contain
         * the persistent data required to recover the region contents to a
         * consistent state when the owning process dies in the middle of
         * modifying NVM.
         */
        nvm_trans_table ^nvtt_list;

        /**
         * This is the maximum number of simultaneous transactions supported
         * in this region. If this is less than the number of nvm_transaction
         * structs in the linked list of transaction tables then the higher
         * slot numbers will not be used. If the application has more threads
         * than this then it is possible that beginning a transaction may have
         * to block waiting for some transaction to end so its slot can be
         * reused.
         */
        uint32_t max_transactions;
        
        /**
         * This is the maximum number of undo blocks that any one transaction
         * can consume. If a transaction attempts to fill more blocks then it
         * will fire an assert killing its thread and aborting its transaction.
         * The goal is to prevent a runaway transaction from consuming all the
         * undo blocks.
         */
        uint32_t max_undo_blocks;
        
        /**
         * This is the pointer to the root nvm_heap. This is the nvm_heap
         * formed from the NVM following this nvm_region. All other base heaps
         * are siblings of the root heap. The root heap has the same
         * name as the region.
         */
        nvm_heap ^rootHeap;

        /**
         * This mutex is held to protect the extent list, root heap list, and
         * transaction table list.
         */
        nvm_mutex reg_mutex; 
        
        /**
         * This is the mutex level for the region mutex .
         */
#define NVM_REGION_MUTEX_LEVEL (NVM_MAX_LEVEL + 11)

        /**
         * This is an array of mutexes used to ensure only one thread at a time
         * upgrades a persistent struct. The offset from the beginning of the
         * region is used to pick a mutex to lock before beginning an upgrade.
         * A configuration parameter defines the size of the array.
         */
        nvm_mutex_array ^upgrade_mutexes;

        /**
         * This is the mutex level for the upgrade mutexes. It is the highest
         * mutex value possible so that upgrades are always possible without
         * a deadlock.
         */
#define NVM_UPGRADE_MUTEX_LEVEL (255)

        /**
         * When a region is mapped into an application it is assigned a
         * nvm_desc for passing to NVM library calls.
         */
        transient
        nvm_desc desc;

        /*
         * Leave some padding for growth. Assumes nvm_mutex is 8 bytes
         */
        uint8_t _padding_to_1024[1024 - 204];
    };
#else
    struct nvm_region
    {
        /**
         * The first bytes of every NVM region created by nvm is the type
         * USID for nvm_region. This is zero until the initialization of the
         * region is complete. This is not an nvm_header because there is no
         * need to check for the volatile pointers being current.
         */
        nvm_usid type_usid;    

        /**
         * Immediately following the USID is the header used for mapping in
         * the region file. It is a separate struct so that it can be 
         * initialized with a file write from volatile memory when creating a
         * region.
         */
        nvm_region_header header;

        /**
         * This is the number of times this region has been attached. It can 
         * be used to recognize the first access to an object after the
         * region is attached by a new process.
         */
        uint64_t attach_cnt;

        /**
         * This is no longer in use.
         */
        uint64_t spare;

        /**
         * This is a pointer to an array containing the descriptions of all 
         * the extents that have been allocated in this region other than the 
         * base extent. If there is only the base extent, then this is null.
         * 
         * There are usually some unused entries which have a NULL pointer.
         * Note that the entries are not sorted.
         */
        nvm_extent_srp extents; //# nvm_extent *

        /**
         * This is the number of entries in the extents array, but not the 
         * number of extents currently allocated. There may be null entries
         * that are unused.
         */
        uint32_t extents_size;

        /* This is the actual number of extents allocated. It is never greater
         * than extents_size. */
        uint32_t extent_count;

        /**
         * This points to the linked list of nvm_trans_table objects
         * allocated in this region. The nvm_trans_data objects contain
         * the persistent data required to recover the region contents to a
         * consistent state when the owning process dies in the middle of
         * modifying NVM.
         */
        nvm_trans_table_srp nvtt_list; //# nvm_trans_table *

        /**
         * This is the maximum number of simultaneous transactions supported
         * in this region. If this is less than the number of nvm_transaction
         * structs in the linked list of transaction tables then the higher
         * slot numbers will not be used. If the application has more threads
         * than this then it is possible that beginning a transaction may have
         * to block waiting for some transaction to end so its slot can be
         * reused.
         */
        uint32_t max_transactions;
        
        /**
         * This is the maximum number of undo blocks that any one transaction
         * can consume. If a transaction attempts to fill more blocks then it
         * will fire an assert killing its thread and aborting its transaction.
         * The goal is to prevent a runaway transaction from consuming all the
         * undo blocks.
         */
        uint32_t max_undo_blocks;
        
        /**
         * This is the pointer to the root nvm_heap. This is the nvm_heap
         * formed from the NVM following this nvm_region. All other base heaps
         * are siblings of the root heap. The root heap has the same
         * name as the region.
         */
        nvm_heap_srp rootHeap; //# nvm_heap *

        /**
         * This mutex is held to protect the extent list, and
         * transaction table list.
         */
        nvm_mutex reg_mutex;

        /**
         * This is the mutex level for the region mutex .
         */
#define NVM_REGION_MUTEX_LEVEL (NVM_MAX_LEVEL + 11)

        /**
         * This is an array of mutexes used to ensure only one thread at a time
         * upgrades a persistent struct. The offset from the beginning of the
         * region is used to pick a mutex to lock before beginning an upgrade.
         * A configuration parameter defines the size of the array.
         */
        nvm_mutex_array_srp upgrade_mutexes;
        
        /**
         * This is the mutex level for the upgrade mutexes. It is the highest
         * mutex value possible so that upgrades are always possible without
         * a deadlock.
         */
#define NVM_UPGRADE_MUTEX_LEVEL (255)

        /**
         * When a region is mapped into an application it is assigned a
         * nvm_desc for passing to NVM library calls.
         */
        nvm_desc desc;

        /*
         * Leave some padding for growth. Assumes nvm_mutex is 8 bytes
         */
        uint8_t _padding_to_1024[1024 - 204];
    };
    extern const nvm_type nvm_type_nvm_region;
#endif //NVM_EXT
    /**
     * This is the per region data kept in volatile memory. There is an array
     * of pointers to these that is maintained in the application data. A
     * region descriptor is an index into the array.
     */
    struct nvm_region_data
    {
        /**
         * This is a mutex that is held when manipulating the data in this
         * struct.
         */
        nvms_mutex mutex;

        /**
         * This is the virtual address where the region is mapped
         */
#ifdef NVM_EXT
        nvm_region ^region;
#else
        nvm_region *region; //
#endif //NVM_EXT

        /* This is the attach id stored in the nvm_region when this application
         * attached this region. Finding this id in a new attach implies a
         * duplicate attach. */
        uint64_t attach_id;//TODO use to see if same region mapped twice

        /**
         * This is the size of the virtual address space consumed by the region
         */
        size_t vsize;

        /**
         * This is the amount of physical NVM in the base extent.
         */
        size_t psize;

        /**
         * This is the service library handle to the open region file.
         */
        nvms_file *handle;

        /**
         * This is the descriptor for this region for convenience.
         */
        nvm_desc desc;

        /**
         * This is the path name used to open the region file.It is allocated
         * in application global memory.
         */
        char *pathname;

        /**
         * This is true if the region does not yet have a root object and thus
         * should not be mapped into any other process.
         */
        int noRoot;

        /**
         * This is the hash table of wait structs for threads that need to
         * block until an nvm_amutex in this region becomes available.
         */
        nvm_wait_table *waiter;

        /**
         * This is the volatile transaction table data. The region data is
         * not ready until this is set.
         */
        nvm_trans_table_data *trans_table;
    };

    /**
     * Verify that a pointer is within a region. Returns 1 if OK and 0 if not. 
     * On 0 retrun errno is set to ERANGE. Note that a pointer may be valid
     * even if it is not pointing to a struct allocated by nvm_alloc, or it is
     * an unmapped virtual address.
     * 
     * @param[in] region
     * Pointer to the region in NVM
     * 
     * @param[in] ptr
     * The NVM address to verify.
     * 
     * @return 
     * 1 if OK and 0 if not. If error then errno is set.
     */
    static inline int
#ifdef NVM_EXT
    nvm_region_ptr(nvm_region ^region, const void ^ptr)
    {
        if ((uint8_t ^)ptr > (uint8_t ^)region &&
            (uint8_t ^)ptr < (uint8_t ^)region + region=>header.vsize)
            return 1;
        errno = ERANGE;
        return 0;
    }
#else
    nvm_region_ptr(nvm_region *region, const void *ptr) //
    {
        if ((uint8_t *)ptr > (uint8_t *)region &&
            (uint8_t *)ptr < (uint8_t *)region + region->header.vsize)
            return 1;
        errno = ERANGE;
        return 0;
    }
#endif //NVM_EXT
    
    /**
     * Verify that a range is within a region. Returns 1 if OK and 0 if not. 
     * On 0 return errno is set to ERANGE. Note that a range may be valid
     * even if it is not pointing to a struct allocated by nvm_alloc, or it is
     * an unmapped virtual address.
     * 
     * @param[in] region
     * Pointer to the region in NVM
     * 
     * @param[in] ptr
     * The NVM address to verify.
     * 
     * @return 
     * 1 if OK and 0 if not. If error then errno is set.
     */
    static inline int
#ifdef NVM_EXT
    nvm_region_rng(nvm_region ^region, const void ^ptr, size_t bytes)
    {
        if ((uint8_t ^)ptr > (uint8_t ^)region &&
            (uint8_t ^)ptr + bytes < (uint8_t ^)region + region=>header.vsize)
            return 1;
        errno = ERANGE;
        return 0;
    }
#else
    nvm_region_rng(nvm_region *region, const void *ptr, size_t bytes) //
    {
        if ((uint8_t *)ptr > (uint8_t *)region &&
            (uint8_t *)ptr + bytes < (uint8_t *)region + region->header.vsize)
            return 1;
        errno = ERANGE;
        return 0;
    }
#endif //NVM_EXT
    
    /**
     * This takes an address and determines which currently attached region
     * contains it. This may require looping through the list of attached
     * regions. If a region is not found then a null pointer is returned,
     * and errno is set to ERANGE.
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
    nvm_region ^nvm_region_find(const void ^ptr);
#else
    nvm_region *nvm_region_find(const void *ptr);
#endif //NVM_EXT

    /**
     * This is the internal version of nvm_remove_extent. It does the real work
     * of nvm_remove_extent, but does not require the extent be application 
     * managed.
     * 
     * @param extent
     * Address of the extent to delete.
     * @return 
     */
#ifdef NVM_EXT
    int nvm_remove_extent1@( 
        void ^extent  
        );
#else
    int nvm_remove_extent1(   
        void *extent    
        );
#endif //NVM_EXT
    
    /**
     * This is the internal version of nvm_resize_extent. It does the real work
     * of nvm_resize_extent, but does not require the extent be application 
     * managed.
     * 
     * @param extent
     * Address of the extent to delete.
     * 
     * @param psize
     * New size for the extent.
     * 
     * @return 
     */
#ifdef NVM_EXT
    int nvm_resize_extent1@( 
        void ^extent,   
        size_t psize
        );
#else
    int nvm_resize_extent1(   
        void *extent,    
        size_t psize
        );
#endif //NVM_EXT

#ifdef	__cplusplus
}
#endif

#endif	/* NVM_REGION0_H */
