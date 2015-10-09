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
     nvm_misc.c - Non-Volatile Memory miscellaneous functions

   DESCRIPTION\n
     This implements miscellaneous housekeeping functions for an NVM application.

 */

#include <string.h>
#include <stdio.h>

#ifndef NVM_H
#include "nvm.h"
#endif

#ifndef NVM_DATA0_H
#include "nvm_data0.h"
#endif

#ifndef NVMS_MISC_H
#include "nvms_misc.h"
#endif

#ifndef NVMS_MEMORY_H
#include "nvms_memory.h"
#endif

#ifndef NVMS_LOCKING_H
#include "nvms_locking.h"
#endif

#ifndef NVM_REGION0_H
#include "nvm_region0.h"
#endif

#ifndef NVM_TRANSACTION0_H
#include "nvm_transaction0.h"
#endif

/* Insert this in code to see if spots in the code are encountered. Also
 * useful for breaking on an event. */
void report_event(int e)
{
    printf("Event %d\n", e);
}

extern void nvm_usid_init(nvm_app_data *ad, uint32_t ents);

/*------------------------------- nvm_init ----------------------------------*/
/**
 * This is called when the shared library is loaded before main is
 * called. It does the one time initializations needed by the library.
 */
void nvm_init(void)
{
    /* Initialize the service layer */
    nvms_init();

    /* We need the parameters to complete initialization. */
    nvms_parameters params;
    nvms_get_params(&params);

    /* Create the application global data. The app data ends with an array of
     * region data pointers that depends on the max_regions parameter. */
    nvm_app_data **adp = (nvm_app_data**)nvms_app_ptr_addr();
    nvm_app_data *ad = *adp;
    size_t sz = sizeof(nvm_app_data) +
            params.max_regions * sizeof(nvm_region_data*);
    ad = nvms_app_alloc(sz);
    *adp = ad; // save for future use

    /* Create the mutex for the app data. */
    ad->mutex = nvms_create_mutex();
    if (!ad->mutex)
        nvms_assert_fail("Unable to allocate app data mutex");

    /* Get the configuration parameters for this application */
    nvms_get_params(&ad->params);

    /* Create the usid map and init it with the nvm library USID values. */
    nvm_usid_init(ad, params.usid_map_size);

    /*----- Put any additional app data initialization here -----*/

}

/*---------------------------- nvm_thread_init ------------------------------*/
/**\brief Initialize a thread to use the NVM library.
 * 
 * This initializes a thread to use the NVM library. It must be called 
 * before any other calls.
 */
void nvm_thread_init()
{
    /* Initialize the service library for use with this thread. */
    nvms_thread_init();

    /* Verify this thread has not already been initialized. Kill thread
     * with an assert if double initialization. */
    if (nvms_thread_get_ptr())
    {
        errno = EBUSY;
        nvms_assert_fail("Two calls to nvm_thread_init in same thread");
    }

    /* Allocate the thread local data, and initialize it. */
    void *mem = nvms_thread_alloc(sizeof(nvm_thread_data));
    if (!mem)
        nvms_assert_fail("Unable to allocate thread root struct");
    nvms_thread_set_ptr(mem);
    nvm_thread_data *td = (nvm_thread_data *)mem;

    /* Get a copy of the parameters from the service library and save it in
     * the thread data. This is to avoid finding the application root
     * struct at times. */
    nvms_get_params(&td->params);

    /* If this platform needs cache flushing of NVM memory, then allocate
     * a flush cache and calculate constants to make it faster to use. */
    nvm_app_data *ad = nvm_get_app_data();
    if (ad->params.flush_cache_sz)
    {
        unsigned sz = ad->params.flush_cache_sz;

        /* allocate the cache itself */
        mem = nvms_thread_alloc(sizeof(uint64_t) * sz);
        if (!mem)
            nvms_assert_fail("Unable to allocate flush cache");
        td->flush_cache = (uint64_t*)mem;

        /* calculate shift to extract index into the flush cache from an
         * NVM address. */
        unsigned shft = 8; // at least 4 entries and 64 byte cache line
        unsigned addr = (sz * ad->params.cache_line) >> shft;
        while (addr > 1)
        {
            shft++;
            addr >>= 1;
        }
        td->flush_cache_shift = shft;

        /* the mask is one less than the size. */
        td->flush_cache_mask = sz - 1;

        /* the XOR is the high bit of the mask */
        td->flush_cache_xor =
                td->flush_cache_mask ^ (td->flush_cache_mask >> 1);
    }

}
/**
 * When a thread is gracefully exiting it needs to call nvm_thread_fini to
 * clean up the data the NVM library is maintaining for it. After this call
 * returns the thread may not make any other calls to the NVM library except
 * nvm_thread_init. If the thread has a current transaction an assert will fire.
 * 
 * This call is only necessary if the application needs to continue
 * execution without this thread.
 */
void nvm_thread_fini(void)
{
    /* get thread private data for this thread */
    nvm_thread_data *td = nvm_get_thread_data();

    /* The application should not call this if there is a transaction running */
    if (td->txdepth != 0)
        nvms_assert_fail("Calling nvm_thread_fini in a transaction");

    /*  Report persist statistics */
    if (td->trans_cnt)
    {
        double tc = td->trans_cnt;
        printf("Thread end: tx_cnt %.0f Avg persist %.1f 1store %.1f flushes "
                "%.1f undo %.1f lock %.1f tx %.1f commit %.1f abort %.1f\n",
                tc, td->persist_cnt / tc, td->persist1_cnt / tc,
                td->flush_cnt / tc, td->persist_undo / tc,
                td->persist_lock / tc, td->persist_tx / tc,
                td->persist_commit / tc, td->persist_abort / tc);
    }

    /* Release the thread data itself */
    if (td->flush_cache)
        nvms_thread_free(td->flush_cache);
    nvms_thread_free(td);

    /* Clear the pointer to the per thread data
     * so nvm_thread_init may be called again. */
    nvms_thread_set_ptr(NULL);

    /* Tell the service library we are done */
    nvms_thread_fini();
}

/**
 * This ensures that all cache lines in the indicated area of NVM will be 
 * scheduled to be made persistent at the next nvm_persist. This is a 
 * no-op if the platform does not need an explicit flush per cache line to 
 * force cached data to NVM.
 * 
 * This is not normally called directly from the application. The 
 * preprocessor adds a flush with every store to an NVM address. An 
 * application only needs to do an explicit flush if it calls a legacy 
 * function to update NVM. The legacy function will not do any flushes 
 * since it is not written to use NVM. The application must do the flushes 
 * when the legacy function returns.
 * 
 * @param[in] ptr
 * The is the first byte of NVM to flush from caches
 * @param[in] bytes
 * The number of bytes to flush
 */
#ifdef NVM_EXT
void nvm_flush(
        const void ^ptr,
        size_t bytes
        )
{
    /* get the pointer to the flush cache */
    nvm_thread_data *td = nvm_get_thread_data();
    uint64_t *fc = td->flush_cache;

    /* Nothing to do if no flushing needed. It is very wasteful to get this
     * far and do nothing. Should have compiled with no flush calls. */
     if (!fc)
        return;

    /* get the address of the cache line as a uint64_t and the number of
     * cache lines to flush. */
    uint64_t clsz = td->params.cache_line; // cache line size
    uint64_t cl = (uint64_t)ptr & ~(clsz - 1); // cache line address
    uint64_t cnt = ((uint64_t)ptr + bytes - cl + clsz - 1) / clsz;

    /* Get the primary index into the cache */
    unsigned index = (unsigned)(cl >> td->flush_cache_shift) &
            td->flush_cache_mask;

    /* loop flushing cache lines until done. */
    while (cnt--)
    {
        /* get the primary and secondary entries in the cache */
        uint64_t *pe = fc + index;
        uint64_t *se = fc + (index ^ td->flush_cache_xor);

        if (*pe == 0)
        {
            *pe = cl; // save in unused primary
        }
        else
        {
            if (*pe != cl) // done if already in primary
            {
                if (*se == 0)
                {
                    *se = cl; // save in unused secondary
                }
                else
                {
                    if (*se != cl) // done if already in secondary
                    {
                        /* Both slots full so flush the primary and use it. */
                        nvms_flush(*pe);
                        td->flush_cnt++;
                        *pe = cl;
                    }
                }
            }
        }

        /* next slot. May wrap to slot zero */
        if (++index >= td->params.flush_cache_sz)
            index = 0;
    }
}
#else
void nvm_flush(
        const void *ptr,
        size_t bytes
        )
{
    /* get the pointer to the flush cache */
    nvm_thread_data *td = nvm_get_thread_data();
    uint64_t *fc = td->flush_cache;

    /* Nothing to do if no flushing needed. It is very wasteful to get this
     * far and do nothing. Should have compiled with no flush calls. */
     if (!fc)
        return;

    /* get the address of the cache line as a uint64_t and the number of
     * cache lines to flush. */
    uint64_t clsz = td->params.cache_line; // cache line size
    uint64_t cl = (uint64_t)ptr & ~(clsz - 1); // cache line address
    uint64_t cnt = ((uint64_t)ptr + bytes - cl + clsz - 1) / clsz;

    /* Get the primary index into the cache */
    unsigned index = (unsigned)(cl >> td->flush_cache_shift) &
            td->flush_cache_mask;

    /* loop flushing cache lines until done. */
    while (cnt--)
    {
        /* get the primary and secondary entries in the cache */
        uint64_t *pe = fc + index;
        uint64_t *se = fc + (index ^ td->flush_cache_xor);

        if (*pe == 0)
        {
            *pe = cl; // save in unused primary
        }
        else
        {
            if (*pe != cl) // done if already in primary
            {
                if (*se == 0)
                {
                    *se = cl; // save in unused secondary
                }
                else
                {
                    if (*se != cl) // done if already in secondary
                    {
                        /* Both slots full so flush the primary and use it. */
                        nvms_flush(*pe);
                        td->flush_cnt++;
                        *pe = cl;
                    }
                }
            }
        }

        /* next slot. May wrap to slot zero */
        if (++index >= td->params.flush_cache_sz)
            index = 0;
    }
}
#endif //NVM_EXT

/**
 * This is identical to nvm_flush except that it is slightly faster
 * because it only flushes one cache line.
 * 
 * @param[in] ptr
 * The is an address in the cache line to flush from caches
 */
#ifdef NVM_EXT
void nvm_flush1(
        const void ^ptr
        )
{
    nvm_thread_data *td = nvm_get_thread_data();
    uint64_t *fc = td->flush_cache;

    /* Nothing to do if no flushing needed. It is very wasteful to get this
     * far and do nothing. Should have compiled with no flush calls. */
    if (!fc)
        return;

    /* get the address of the cache line as a uint64_t. */
    uint64_t clsz = td->params.cache_line; // cache line size
    uint64_t cl = (uint64_t)ptr & ~(clsz - 1); // cache line address

    /* Get the primary index into the cache */
    unsigned index = (unsigned)(cl >> td->flush_cache_shift) &
            td->flush_cache_mask;

    /* get the primary and secondary entries in the cache */
    uint64_t *pe = fc + index;
    uint64_t *se = fc + (index ^ td->flush_cache_xor);

    if (*pe == 0)
    {
        *pe = cl; // save in unused primary
    }
    else
    {
        if (*pe != cl) // done if already in primary
        {
            if (*se == 0)
            {
                *se = cl; // save in unused secondary
            }
            else
            {
                if (*se != cl) // done if already in secondary
                {
                    /* Both slots full so flush the primary and use it. */
                    nvms_flush(*pe);
                    td->flush_cnt++;
                    *pe = cl; // save in flushed entry
                }
            }
        }
    }
}
#else
void nvm_flush1(
        const void *ptr
        )
{
    nvm_thread_data *td = nvm_get_thread_data();
    uint64_t *fc = td->flush_cache;

    /* Nothing to do if no flushing needed. It is very wasteful to get this
     * far and do nothing. Should have compiled with no flush calls. */
    if (!fc)
        return;

    /* get the address of the cache line as a uint64_t. */
    uint64_t clsz = td->params.cache_line; // cache line size
    uint64_t cl = (uint64_t)ptr & ~(clsz - 1); // cache line address

    /* Get the primary index into the cache */
    unsigned index = (unsigned)(cl >> td->flush_cache_shift) &
            td->flush_cache_mask;

    /* get the primary and secondary entries in the cache */
    uint64_t *pe = fc + index;
    uint64_t *se = fc + (index ^ td->flush_cache_xor);

    if (*pe == 0)
    {
        *pe = cl; // save in unused primary
    }
    else
    {
        if (*pe != cl) // done if already in primary
        {
            if (*se == 0)
            {
                *se = cl; // save in unused secondary
            }
            else
            {
                if (*se != cl) // done if already in secondary
                {
                    /* Both slots full so flush the primary and use it. */
                    nvms_flush(*pe);
                    td->flush_cnt++;
                    *pe = cl; // save in flushed entry
                }
            }
        }
    }
}
#endif //NVM_EXT

/**
 * This is a barrier that ensures all previous stores to NVM by the calling
 * thread are actually persistent when it returns. On some systems it is
 * necessary to flush each cache line that was modified before executing
 * the barrier. If the system uses Flush On Shutdown (FOS) to force NVM
 * data to be persistent, then this barrier is a no-op. If the system uses
 * Commit On Shutdown (COS) this forces the dirty NVM to the memory
 * controller write queue.
 * 
 * This is not normally called directly from the application. The creation 
 * of each undo record executes a barrier as well as commit or abort of a 
 * transaction. An application may need to do this explicitly if it is 
 * storing data in NVM without using transactions. For example, an 
 * application might copy client data to an NVM buffer then update a 
 * counter to make it visible. A persist barrier is needed after the copy 
 * and before updating the counter. Another persist barrier is needed after 
 * the counter update to make the counter persistent. Note that in this 
 * case a flush is also required if bcopy is used to copy the data. 
 * Presumably the counter update uses a non-transactional store which 
 * automatically includes a flush.
 */
void nvm_persist()
{
    nvm_thread_data *td = nvm_get_thread_data();
    uint64_t *fc = td->flush_cache;

    /* flush the processor caches if necessary */
    if (fc != 0)
    {
        /* Any non-zero entries in the flush cache need to be flushed */
        int i;
        for (i = 0; i < td->params.flush_cache_sz; i++, fc++)
        {
            if (*fc)
            {
                nvms_flush(*fc); // flush one line
                td->flush_cnt++;
                *fc = 0; // clear line for next persist
            }
        }
    }

    /* ensure all stores are persistent */
    nvms_persist();
    td->persist_cnt++;
}
/**
 * This combines a flush of a single cache line with a persist barrier. It 
 * is much faster because the flush cache does not get scanned.
 * 
 * @param[in] ptr
 * The is an address in the cache line to flush from caches
 */
#ifdef NVM_EXT
void nvm_persist1(
    void ^ptr 
    )
#else
void nvm_persist1(
    void *ptr    
    )
#endif //NVM_EXT
{
    nvms_flush((uint64_t)ptr);
    nvms_persist();
    nvm_thread_data *td = nvm_get_thread_data();
    td->flush_cnt++;
    td->persist_cnt++;
    td->persist1_cnt++;
}
/**
 * This works just like a standard longjmp except that it will first end 
 * NVM transactions as needed to get back to the transaction nesting depth
 * that existed when nvm_set_jmp saved the environment in buf. It then calls
 * the standard longjmp.
 * 
 * @param[in] buf
 * This must contain a context stored by nvm_setjmp in a function that is 
 * still in the call stack.
 * 
 * @param[in] val
 * This is the non-zero value that nvm_setjmp will return
 */
#ifdef NVM_EXT
void nvm_longjmp(
        nvm_jmp_buf *buf, // jmp_buf equivalent
        int val // setjmp return value
        )
#else
void nvm_longjmp(
        nvm_jmp_buf *buf, // jmp_buf equivalent
        int val // setjmp return value
        )
#endif //NVM_EXT
{
    int depth = nvm_txdepth();

    /* If current depth is shallower than the nvm_jmpbuf, then it is not on
     * our stack. */
    if (depth < buf->depth)
    {
        errno = EINVAL;
        nvms_assert_fail("Bad nvm_jmp_buf for nmv_longjmp");
    }

    /* Abort and end transactions until we are at the same depth as the
     * nvm_setjmp. */
    while (depth > buf->depth)
    {
#ifdef NVM_EXT
    extern @ { // depth is > 0 so there is a current transaction
#endif //NVM_EXT
        switch (nvm_txstatus(0))
        {
        default:
            errno = EINVAL;
            nvms_assert_fail("Impossible transaction status");
            break;

        case NVM_TX_ACTIVE:
            /* abort an active transaction */
            nvm_abort();
            nvm_txend();
            depth--;
            break;

        case NVM_TX_COMMITTING:
            errno = EINVAL;
            nvms_assert_fail("nvm_longjmp not allowed in oncommit operation");
            break;

        case NVM_TX_ABORTING:
        case NVM_TX_ROLLBACK:
            errno = EINVAL;
            nvms_assert_fail("nvm_longjmp not allowed in onabort operation");
            break;

        case NVM_TX_COMMITTED:
        case NVM_TX_ABORTED:
            /* The current transaction completed but not yet ended so end it */
            nvm_txend();
            depth--;
            break;
        }
    }
#ifdef NVM_EXT
    } // end transactional block
#endif //NVM_EXT

    /* The transaction state is back where it belongs so we can do a standard
     * longjmp. */
    longjmp(buf->buf, val);
}

#if 0 //TODO+ the CAS routines are not used as of now
/**
 * Compare and swap an 8 byte location in NVM. The return value is the 
 * value of the location before the CAS executed. If it is equal to the old
 * value then the swap succeeded.
 * 
 * A CAS is like a nested transaction that either commits or aborts on its 
 * own. It cannot be part of a larger transaction that updates multiple 
 * locations atomically, and might rollback.
 * 
 * The NVM version of CAS is different than a normal CAS because it first 
 * forces any previous NVM stores to be persistent, and if the swap 
 * happens, it forces the new value to be persistent before returning. 
 * 
 * @param[in] ptr
 * This is the address of the NVM location to CAS
 * 
 * @param[in] oldval
 * This is the expected current value to be replaced.
 * 
 * @param[in] newval
 * This is the value to store if the compare succeeds
 * 
 * @return
 * The value in the location when CAS was executed. 
 */
uint64_t nvm_cas8(
#//    volatile uint64_t ^ptr,  
        volatile uint64_t *ptr, //# 
        uint64_t oldval,
        uint64_t newval
        )
{
    /* ensure NVM is consistent since there is no undo for this operation */
    nvm_persist();

    /* do the compare and swap */
    uint64_t ret = nvms_cas8(ptr, oldval, newval);

    /* If it succeeded then ensure it is persistent. */
    if (ret == oldval)
    {
        nvm_persist1((void*)ptr);
    }

    return ret;
}
/**
 * Compare and swap a 4 byte location in NVM. The return value is the 
 * value of the location before the CAS executed. If it is equal to the old
 * value then the swap succeeded.
 * 
 * A CAS is like a nested transaction that either commits or aborts on its 
 * own. It cannot be part of a larger transaction that updates multiple 
 * locations atomically, and might rollback.
 * 
 * The NVM version of CAS is different than a normal CAS because it first 
 * forces any previous NVM stores to be persistent, and if the swap 
 * happens, it forces the new value to be persistent before returning. 
 * 
 * @param[in] ptr
 * This is the address of the NVM location to CAS
 * 
 * @param[in] oldval
 * This is the expected current value to be replaced.
 * 
 * @param[in] newval
 * This is the value to store if the compare succeeds
 * 
 * @return
 * The value in the location when CAS was executed. 
 */
uint32_t nvm_cas4(
#//    volatile uint32_t ^ptr,  
        volatile uint32_t *ptr, //# 
        uint32_t oldval,
        uint32_t newval
        )
{
    /* ensure NVM is consistent since there is no undo for this operation */
    nvm_persist();

    /* do the compare and swap */
    uint32_t ret = nvms_cas4(ptr, oldval, newval);

    /* If it succeeded then ensure it is persistent. */
    if (ret == oldval)
    {
        nvm_persist1((void*)ptr);
    }

    return ret;
}
/**
 * Compare and swap a 2 byte location in NVM. The return value is the 
 * value of the location before the CAS executed. If it is equal to the old
 * value then the swap succeeded.
 * 
 * A CAS is like a nested transaction that either commits or aborts on its 
 * own. It cannot be part of a larger transaction that updates multiple 
 * locations atomically, and might rollback.
 * 
 * The NVM version of CAS is different than a normal CAS because it first 
 * forces any previous NVM stores to be persistent, and if the swap 
 * happens, it forces the new value to be persistent before returning. 
 * 
 * @param[in] ptr
 * This is the address of the NVM location to CAS
 * 
 * @param[in] oldval
 * This is the expected current value to be replaced.
 * 
 * @param[in] newval
 * This is the value to store if the compare succeeds
 * 
 * @return
 * The value in the location when CAS was executed. 
 */
uint16_t nvm_cas2(
#//    volatile uint16_t ^ptr,  
        volatile uint16_t *ptr, //# 
        uint16_t oldval,
        uint16_t newval
        )
{
    /* ensure NVM is consistent since there is no undo for this operation */
    nvm_persist();

    /* do the compare and swap */
    uint16_t ret = nvms_cas2(ptr, oldval, newval);

    /* If it succeeded then ensure it is persistent. */
    if (ret == oldval)
    {
        nvm_persist1((void*)ptr);
    }

    return ret;
}
#endif
//TODO+ before calling nvms_corrupt mark the nvm_region as failed for
//TODO+ corruption. Have a status that is the mechanism of the last detach:
//TODO+ clean, assert, or corruption
