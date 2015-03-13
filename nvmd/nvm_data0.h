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
      nvm_data0.h - NVM library private volatile data

    DESCRIPTION\n
      This file defines the application global and thread local data
      structures used by the library. All the volatile data used by the
      library can be navigated to through these structures.

      Theses declarations are private to the NVM library.
 */


#ifndef NVM_DATA0_H
#define	NVM_DATA0_H

#ifndef NVMS_MISC_H
#include "nvms_misc.h"
#endif

#ifndef NVMS_LOCKING_H
#include "nvms_locking.h"
#endif

#ifndef NVMS_SLEEPING_H
#include "nvms_sleeping.h"
#endif

#ifndef NVMS_MEMORY_H
#include "nvms_memory.h"
#endif

#ifdef	__cplusplus
extern "C"
{
#endif
    typedef struct nvm_proc_data nvm_proc_data;
    typedef struct nvm_app_data nvm_app_data;
    typedef struct nvm_region_data nvm_region_data;
    typedef struct nvm_mapped_region nvm_mapped_region;
    typedef struct nvm_usid_map nvm_usid_map;
    typedef struct nvm_trans_table_data nvm_trans_table_data;
    typedef struct nvm_wait nvm_wait;
    typedef struct nvm_wait_list nvm_wait_list;
    
#ifdef NVM_EXT
    /* Persistent struct types */
    typedef persistent struct nvm_transaction nvm_transaction;
    typedef persistent struct nvm_region nvm_region;
    typedef persistent struct nvm_undo_blk nvm_undo_blk;
#else
    /* Persistent struct types */
    typedef struct nvm_transaction nvm_transaction;
    typedef struct nvm_region nvm_region;
    typedef struct nvm_undo_blk nvm_undo_blk;

    /* Self relative pointers for persistent types. */
    NVM_SRP(nvm_transaction)
    NVM_SRP(nvm_region)
    NVM_SRP(nvm_undo_blk)
#endif //NVM_EXT

    /*
     * These are the forward incomplete type definitions for all the undo 
     * record types
     */
#ifdef NVM_EXT
    typedef persistent struct nvm_restore nvm_restore;
    typedef persistent struct nvm_lkrec nvm_lkrec;
    typedef persistent struct nvm_on_unlock nvm_on_unlock;
    typedef persistent struct nvm_savepnt nvm_savepnt;
    typedef persistent struct nvm_nested nvm_nested;
    typedef persistent struct nvm_on_abort nvm_on_abort;
    typedef persistent struct nvm_on_commit nvm_on_commit;
#else
    typedef struct nvm_restore nvm_restore;
    typedef struct nvm_lkrec nvm_lkrec;
    typedef struct nvm_on_unlock nvm_on_unlock;
    typedef struct nvm_savepnt nvm_savepnt;
    typedef struct nvm_nested nvm_nested;
    typedef struct nvm_on_abort nvm_on_abort;
    typedef struct nvm_on_commit nvm_on_commit;
    NVM_SRP(nvm_restore)
    NVM_SRP(nvm_lkrec)
    NVM_SRP(nvm_on_unlock)
    NVM_SRP(nvm_savepnt)
    NVM_SRP(nvm_nested)
    NVM_SRP(nvm_on_abort)
    NVM_SRP(nvm_on_commit)
#endif //NVM_EXT

    /**
     * This contains the thread private data. There is one copy of this struct
     * for each thread. The owning thread is the only thread that touches any
     * data in this struct. Thus there are no locks to cover the contents. The
     * thread root pointer points at an instance of this struct.
     */
    struct nvm_thread_data
    {

        /**
         * If this thread is in the middle of upgrading a persistent struct
         * to a new type, then this points to the struct being upgraded.
         */
#ifdef NVM_EXT
        void ^upgrade_ptr;
#else
        void *upgrade_ptr;
#endif //NVM_EXT

        /**
         * If this thread is in the middle of upgrading a persistent struct
         * to a new type, then this points to the old type definition.
         */
        const nvm_type *upgrade_type;

        /**
         * If this thread has an active transaction, then this points to
         * the NVM structure that manages the transaction. If there is
         * no current transaction then this is a null pointer.
         */
#ifdef NVM_EXT
        nvm_transaction ^transaction;
#else
        nvm_transaction *transaction;
#endif //NVM_EXT
        
        /**
         * If this thread has an active transaction, then this points to
         * the NVM region that the transaction operates on.
         */
#ifdef NVM_EXT
        nvm_region ^region;
#else
        nvm_region *region;
#endif //NVM_EXT
        
        /**
         * This is the current transaction nesting depth. It is maintained 
         * here rather than calculating on every request to improve performance
         */
        int txdepth;

        /**
         * If this platform requires flushing of each NVM cache line that is
         * modified, then this points to an array of addresses that need
         * flushing. This is used to reduce multiple flushes of the same
         * cache line by postponing the flush with a cache of pending flushes.
         * If this platform does not need flushes then this will be null.
         */
        uint64_t *flush_cache;

        /**
         * If there is a flush cache then this is the right shift count to 
         * convert an NVM address into an index into the flush cache.
         */
        unsigned flush_cache_shift;

        /**
         * If there is a flush cache then this is a mask to extract the index
         * after the right shift.
         */
        unsigned flush_cache_mask;

        /**
         * If there is a flush cache then this can be XOR'ed with an index to 
         * get the alternate cache entry. This gets a 2 way associative cache.
         */
        unsigned flush_cache_xor;
        
        /**
         * This is the number of base transactions executed by this thread.
         */
        uint64_t trans_cnt;
                
        /**
         *  This is the number of cache line flushes. 
         */
        uint64_t flush_cnt;

        /**
         * This is the number of persists done by this thread.
         */
        uint64_t persist_cnt;
        
        /**
         * This is the number of persists done by this thread which only 
         * persist a single store.
         */
        uint64_t persist1_cnt;
        
        /**
         * This is the number of persists done for the purpose of generating
         * undo.
         */
        uint64_t persist_undo;
        
        /**
         * This is the number of persists done for NVM mutex lock/unlock.
         */
        uint64_t persist_lock;
        
        /**
         * This is the number of perisists done for tx begin/end, including
         * nested transaction begin/end.
         */
        uint64_t persist_tx;
        
        /**
         * This is the number of persists for transaction commit excluding 
         * any on commit call backs.
         */
        uint64_t persist_commit;
        
        /**
         * This is the number of persists for transaction abort excluding 
         * any on abort call backs.
         */
        uint64_t persist_abort;

        /**
         * This is a local copy of the parameters from the service library to 
         * speed up access.
         */
        nvms_parameters params;
    };
    typedef struct nvm_thread_data nvm_thread_data;

    /**
     * Return a pointer to the current thread data
     */
    static inline nvm_thread_data *
    nvm_get_thread_data()
    {
        return(nvm_thread_data *)nvms_thread_get_ptr();
    }

    /**
     * This contains the application global data. There is exactly one copy
     * of this struct shared by all threads. All application global data
     * can be reached from this struct except for the thread context created
     * by nvm_thread_init. The application root pointer points at this struct.
     */
    struct nvm_app_data
    {
        /** 
         * This is the mutex used to protect the app root struct, and 
         * coordinate allocation of process root structs.
         */
        nvms_mutex mutex;

        /**
         * This is the parameter data for this application returned by the
         * service library. This also appears in nvm_thread_data for access
         * when it is easier to find.
         */
        nvms_parameters params;

        /**
         * This is the hash map used for looking up USID to global symbol
         * mappings.
         */
        nvm_usid_map *usid_map;

        /**
         * The app_data ends with an array of pointers to region data. This 
         * array is indexed by the nvm_desc descriptor return when a region
         * is attached to an application. Zero entries are available 
         * descriptors. The actual size of the array is determined by the 
         * max_regions parameter.
         */
        nvm_region_data *regions[1];

    };

    /**
     * This is called when the shared library is loaded before main is
     * called. It does the one time initializations needed by the library.
     */
    void nvm_init(void);

    /**
     * Return a pointer to the current application root struct
     */
    static inline nvm_app_data *
    nvm_get_app_data()
    {
        return *(nvm_app_data**)nvms_app_ptr_addr();
    }

    /* This is the array of pointers to nvm_types defined for this library. */
    extern const nvm_type *nvm_usid_types[];

    /* This is the array of pointers to nvm_externs defined for this library. */
    extern const nvm_extern *nvm_usid_externs[];

    /* These are the nvm_extern declarations for this library */
    extern const nvm_extern nvm_extern_nvm_free_callback;

#ifdef	__cplusplus
}
#endif

#endif	/* NVM_DATA0_H */

