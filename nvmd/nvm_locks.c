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
     nvm_locks.c - Management of transaction locks in non-volatile memory 

   DESCRIPTION\n
     This implements mutexes that are owned by NVM transactions and are kept
     in non-volatile memory so that they survive reboot.

   NOTES\n
     The NVM library introduces the concept of NMV locks for coordinating
     access to NVM data in the same region as the lock. NVM locks are similar
     to mutexes except that they are owned by a transaction rather than a
     thread of execution. They are released when the owning transaction
     commits, or if it rolls back all the undo generated after the lock was
     acquired. Rollback can be the result of an abort or rollback to savepoint.
    
     Since NVM locks are in NVM and associated with a transaction rather than
     a thread of execution, they survive process death and/or unmap of a
     region. When the region is reattached recovery will release all NVM locks
     as part of completing all transactions.
    
     To aid in deadlock avoidance, each NVM lock has a level associated with it.
     A lock cannot be acquired with waiting if a lock of the same or higher
     level is already owned by the transaction. It also has a sleep algorithm
     to indicate how long to spin before going to sleep when there is a lock
     conflict. There is a lock initialization call that must be made to set
     the level and sleep algorithm.

   MODIFIED    (MM/DD/YY)
     bbridge    06/10/14 - Creation\n
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include "nvm.h"
#include "nvm_data0.h"
#include "nvm_locks0.h"
#include "nvm_region0.h"
#include "nvm_transaction0.h"
#ifdef NVM_EXT
static nvm_wait_list *nvm_get_wait_list(nvm_transaction ^tx, nvm_amutex ^mx);
#else
static nvm_wait_list *nvm_get_wait_list(nvm_transaction *tx, nvm_amutex *mx);
#endif //NVM_EXT

/**
 * Internal version of nvm_mutex_init that allows levels over 200
 */
#ifdef NVM_EXT
void nvm_mutex_init1(
        nvm_amutex ^mutex,
        uint8_t level
        )
{
    mutex=>owners ~= 0;
    mutex=>x_waiters = 0;
    mutex=>s_waiters = 0;
    mutex=>level ~= level;
    mutex=>initialized ~= 1;
}
#else
void nvm_mutex_init1(
        nvm_amutex *mutex,
        uint8_t level
        )
{
    mutex->owners = 0;
    mutex->x_waiters = 0;
    mutex->s_waiters = 0;
    mutex->level = level;
    mutex->initialized = 1;
    nvm_flush1(mutex);
}
#endif //NVM_EXT
/**
 * This initializes an NVM mutex so it can be locked. This must be called 
 * in the transaction that allocates the NVM for the mutex. No undo is 
 * created for the initialization since rollback is presumed to release the 
 * mutex back to free memory.
 * 
 * The level is used for ensuring there are no deadlocks in the code. A 
 * transaction can only do a wait get of a lock on the mutex if all the 
 * locks it currently holds are at a lower lock level. A lock level of zero 
 * can be given, but that means that all locking must be no-wait.
 * 
 * If there are any errors then errno is set and the return value is zero.
 * 
 * @param[in] mutex
 * This is the address in NVM of the mutex to initialize.
 * 
 * @param[in] level
 * This is the lock level for deadlock prevention. It must be less than 200.
 */
#ifdef NVM_EXT
void nvm_mutex_init@(
        nvm_mutex ^mutex,
        uint8_t level
        )
{
    if (level >= 200)
        nvms_assert_fail("Invalid lock level");
    nvm_mutex_init1((nvm_amutex^)mutex, level);
}
#else
void nvm_mutex_init(
        nvm_mutex *mutex,     
        uint8_t level
        )
{
    if (level >= 200)
        nvms_assert_fail("Invalid lock level");
    nvm_mutex_init1((nvm_amutex*)mutex, level);
}
#endif //NVM_EXT

/**
 * Finalize use of a mutex before freeing the NVM memory holding it. This
 * must be called in the transaction that is freeing the memory. This
 * releases any volatile memory structures allocated for it. When the 
 * transaction commits the delete, the mutex is zeroed making it invalid.
 * 
 * An assert is fired if the mutex is held or has any waiters.
 * 
 * @param[in] mutex
 * This is the address in NVM of the mutex to finalize.
 */
#ifdef NVM_EXT
    void nvm_mutex_fini@( 
        nvm_mutex ^omutex
        )
{
    /* convert opaque nvm_mutex to actual NVM mutex */
    nvm_amutex ^mutex = (nvm_amutex^)omutex;

   /* assert if mutex owned or has waiters */
    if (mutex=>owners || mutex=>s_waiters || mutex=>x_waiters)
        nvms_assert_fail("Finalizing busy NVM mutex");

    /* Nothing to do if already finalized or never initialized. */
    if (mutex=>initialized == 0)
        return;
    
    /* Get the wait list for this mutex. */
    nvm_thread_data *td = nvm_get_thread_data();
    nvm_wait_list *wl = nvm_get_wait_list(td->transaction, mutex);

    /* Search the list for a wait struct that will need to be deleted. */
    nvm_wait *w = wl->head;
    nvm_wait *prev = NULL;
    while (w != NULL)
    {
        if (w->nvmx == mutex)
            break;
        prev = w;
        w = w->link;
    }

    /* Delete it so there is no stale pointer in a wait struct */
    if (w != NULL)
    {
        /* destroy the condition variable if any */
        if (w->cond != NULL)
        {
            nvms_destroy_cond(w->cond);
            w->cond = NULL;
        }

        /* unlink from the waiter bucket */
        if (prev)
            prev->link = w->link;
        else
            wl->head = w->link;

        /* free the wait struct itself */
        nvms_app_free(w);
    }
    
    /* No longer initialized */
    mutex=>initialized @= 0;

    /* done with the wait list */
    nvms_unlock_mutex(wl->mutex);
}
#else
void nvm_mutex_fini(    
        nvm_mutex *omutex
        )
{
    /* convert opaque nvm_mutex to actual NVM mutex */
    nvm_amutex *mutex = (nvm_amutex*)omutex;

    /* assert if mutex owned or has waiters */
    if (mutex->owners || mutex->s_waiters || mutex->x_waiters)
        nvms_assert_fail("Finalizing busy NVM mutex");

    /* Nothing to do if already finalized or never initialized. */
    if (mutex->initialized == 0)
        return;
    
    /* Get the wait list for this mutex. */
    nvm_thread_data *td = nvm_get_thread_data();
    nvm_wait_list *wl = nvm_get_wait_list(td->transaction, mutex);

    /* Search the list for a wait struct that will need to be deleted. */
    nvm_wait *w = wl->head;
    nvm_wait *prev = NULL;
    while (w != NULL)
    {
        if (w->nvmx == mutex)
            break;
        prev = w;
        w = w->link;
    }

    /* Delete it so there is no stale pointer in a wait struct */
    if (w != NULL)
    {
        /* destroy the condition variable if any */
        if (w->cond != NULL)
        {
            nvms_destroy_cond(w->cond);
            w->cond = NULL;
        }

        /* unlink from the waiter bucket */
        if (prev)
            prev->link = w->link;
        else
            wl->head = w->link;

        /* free the wait struct itself */
        nvms_app_free(w);
    }
    
    /* No longer initialized */
    NVM_UNDO(*mutex);
    mutex->initialized = 0;
    nvm_flush1(mutex);

    /* done with the wait list */
    nvms_unlock_mutex(wl->mutex);
}
#endif //NVM_EXT

/* Internal version of nvm_create_mutex_array that allows levels over 200 */
#ifdef NVM_EXT
nvm_mutex_array ^nvm_create_mutex_array1@(
    nvm_heap ^heap,
    uint32_t count,
    uint8_t level
    )
{
    /* allocate the array */
    nvm_mutex_array ^ma = nvm_alloc(heap, shapeof(nvm_mutex_array), count);
    if (!ma)
        return 0;

    /* initialize the mutexes, note this does not generate any undo */
    ma=>count ~= count;
    ma=>destroyed ~= 0;
    int i;
    for (i = 0; i < count; i++)
        nvm_mutex_init1(%ma=>mutexes[i], level);

    return ma;
}
#else
nvm_mutex_array *nvm_create_mutex_array1(
    nvm_heap *heap,
    uint32_t count,
    uint8_t level
    )
{
    /* allocate the array */
    nvm_mutex_array *ma = nvm_alloc(heap, shapeof(nvm_mutex_array), count);
    if (!ma)
        return 0;

    /* initialize the mutexes, note this does not generate any undo */
    ma->count = count;
    ma->destroyed = 0;
    int i;
    for (i = 0; i < count; i++)
        nvm_mutex_init1(&ma->mutexes[i], level);

    return ma;
}
#endif //NVM_EXT

/**
 * Create and initialize an array of mutexes. This must be called in a
 * transaction. If the transaction rolls back then the mutex array will
 * be released. A pointer to the array is returned. A null pointer is
 * returned if there is insufficient memory to allocate the array. Other
 * errors will fire an assert.
 *
 * @param[in] heap
 * This is the heap to allocate the array from.
 *
 * @param[in] count
 * This is the number of mutexes to allocate.
 *
 * @param[in] level
 * This is the lock level of all the mutexes. It must be less than 200.
 */
#ifdef NVM_EXT
nvm_mutex_array ^nvm_create_mutex_array@(
    nvm_heap ^heap,
    uint32_t count,
    uint8_t level
    )
{
    if (level >= 200)
        nvms_assert_fail("Invalid lock level");
    return nvm_create_mutex_array1(heap, count, level);
}
#else
nvm_mutex_array *nvm_create_mutex_array(
    nvm_heap *heap,
    uint32_t count,
    uint8_t level
    )
{
    if (level >= 200)
        nvms_assert_fail("Invalid lock level");
    return nvm_create_mutex_array1(heap, count, level);
}
#endif //NVM_EXT

/**
 * Pick one mutex from a mutex array based on an NVM address in the same
 * region. A pointer to the chosen mutex is returned. This must be called
 * in a transaction. Any errors will fire an assert.
 *
 * @param[in] array
 * The mutex array to pick from
 *
 * @param[in] ptr
 * An NVM address used as a key for picking
 *
 * @return
 * Pointer to the chosen mutex
 */
#ifdef NVM_EXT
nvm_mutex ^nvm_pick_mutex@(
    nvm_mutex_array ^array,
    const void ^ptr
    )
{
    /* get the region for the array and ptr from the current transaction */
    nvm_thread_data *td = nvm_get_thread_data();
    nvm_region ^rg = td->region;
    if (!rg)
        nvms_assert_fail(
                "Attempt to pick a mutex from an array outside a transaction");

    /* verify the array and pointer are in the tx region */
    if (!nvm_region_rng(rg, ptr, 0))
        nvms_assert_fail("Pointer for picking mutex not in region");
    if (!nvm_region_rng(rg, array, 0))
        nvms_assert_fail("Mutex array not in region");

    /* Convert the address into an offset into the region. This ensures the
     * same mutex is chosen even if the region is mapped at a different
     * address. */
    size_t offset = (uint8_t^)ptr - (uint8_t^)rg;

    /* divide offset by a prime that is less than the alignment of most
     * pointers to get an even distribution across the mutexes. Use that
     * to select the mutex. */
    return (nvm_mutex ^)%array=>mutexes[(offset/7) % array=>count];
}
#else
nvm_mutex *nvm_pick_mutex(
    nvm_mutex_array *array,
    const void *ptr
    )
{
    /* get the region for the array and ptr from the current transaction */
    nvm_thread_data *td = nvm_get_thread_data();
    nvm_region *rg = td->region;
    if (!rg)
        nvms_assert_fail(
                "Attempt to pick a mutex from an array outside a transaction");

    /* verify the array and pointer are in the tx region */
    if (!nvm_region_rng(rg, ptr, 0))
        nvms_assert_fail("Pointer for picking mutex not in region");
    if (!nvm_region_rng(rg, array, 0))
        nvms_assert_fail("Mutex array not in region");

    /* Convert the address into an offset into the region. This ensures the
     * same mutex is chosen even if the region is mapped at a different
     * address. */
    size_t offset = (uint8_t*)ptr - (uint8_t*)rg;

    /* divide offset by a prime that is less than the alignment of most
     * pointers to get an even distribution across the mutexes. Use that
     * to select the mutex. */
    return (nvm_mutex *)&array->mutexes[(offset/7) % array->count];
}
#endif //NVM_EXT

/**
 * Find the wait table bucket for an NVM mutex and acquire its mutex.
 * @param tx
 * The current transaction.
 * 
 * @param mx
 * The mutex being looked up.
 * 
 * @return 
 * The wait list for the mutex.
 */
#ifdef NVM_EXT
static nvm_wait_list *nvm_get_wait_list(
        nvm_transaction ^tx,
        nvm_amutex ^mx)
{
    /* Get the wait table for the region the current transaction is for */
    nvm_app_data *ad = nvm_get_app_data();
    nvm_region_data *rd = ad->regions[tx=>desc];
    nvm_wait_table *wt = rd->waiter;

    /* Use the mutex address divided by 7 as a hash key. This is not the
     * same on every attach since the region can be mapped to a different
     * address. However a new wait table is constructed for every attach. */
    nvm_wait_list *wl = &wt->table[(((uint64_t)mx) / 7) % wt->buckets];

    /* Lock the list for the caller. */
    nvms_lock_mutex(wl->mutex, 1);

    /* return the wait list */
    return wl;
}
#else
static nvm_wait_list *nvm_get_wait_list(
        nvm_transaction *tx,
        nvm_amutex *mx)
{
    /* Get the wait table for the region the current transaction is for */
    nvm_app_data *ad = nvm_get_app_data();
    nvm_region_data *rd = ad->regions[tx->desc];
    nvm_wait_table *wt = rd->waiter;

    /* Use the mutex address divided by 7 as a hash key. This is not the
     * same on every attach since the region can be mapped to a different
     * address. However a new wait table is constructed for every attach. */
    nvm_wait_list *wl = &wt->table[(((uint64_t)mx) / 7) % wt->buckets];

    /* Lock the list for the caller. */
    nvms_lock_mutex(wl->mutex, 1);

    /* return the wait list */
    return wl;
}
#endif //NVM_EXT

/**
 * This acquires an NVM lock and hangs it on the current transaction.  The
 * lock may be a shared or exclusive lock. If the lock is successfully
 * acquired the return value is 1.  If the lock cannot be granted
 * immediately, then the thread will sleep until the lock is granted or
 * timeout microseconds have passed. A negative timeout sleeps forever. If
 * the timeout expires then the return value is zero and errno is EBUSY.
 * 
 * The lock will be released when the current transaction commits, or if
 * all undo generated while the lock was held is applied by rollback or
 * abort. Releasing the lock may wake up one or more waiters. Note that
 * there is no means to explicitly release the lock without applying the
 * undo or committing the transaction.
 * 
 * An assert fires if the locking order for the mutex is violated and
 * timeout is negative.
 * 
 * @param[in] mutex
 * This is the address in NVM of the mutex to lock.
 * 
 * @param[in] excl
 * If this is true then an exclusive lock will be acquired. Otherwise a 
 * shared lock is acquired.
 * 
 * @param[in] timeout
 * If  the lock cannot be granted, then the thread will sleep up to
 * timeout microseconds waiting for the lock to be available.
 * A negative timeout will never give up. If the acquisition times out,
 * then error EBUSY is set in errno and 0 is returned.
 * 
 * @return 
 * 1 if successful, 0 if wait timed out and errno is EBUSY
 */
#define NVM_LOCK_WAIT (-1) // wait forever
#ifdef NVM_EXT
int nvm_lock@( 
    nvm_mutex ^omutex,
    int excl,
    int timeout
    )
{
    /* TODO+ This should be redesigned to use a compare and swap to avoid any
     * service library calls when there is no contention. */

    /* convert opaque nvm_mutex to actual NVM mutex */
    nvm_amutex ^mutex = (nvm_amutex^)omutex;

    /* Verify the mutex has been initialized. */
    if (mutex=>initialized != 1)
        nvms_assert_fail("Locking an uninitialized mutex");

    /* If there is a timeout this will get the absolute time when it ends.
     * We do not waste time calculating the stop time unless we have to wait. */
    uint64_t stop = 0;

    /* This is 1 if we have waited once and thus no longer need to defer to
     * other waiters for fairness. */
    int waited = 0;

    /* If we wait, this will point to the wait struct. We may need this to post
     * other share waiters when we get a share lock. */
    nvm_wait *w = NULL;

    /* Get the current transaction and verify its type and state. */
    nvm_thread_data *td = nvm_get_thread_data();
    nvm_transaction ^tx = td->transaction;
    if (tx == 0)
        nvms_assert_fail("Attempt to lock NVM mutex without a transaction");
    nvm_verify(tx, shapeof(nvm_transaction));

    /* verify the mutex is in the same region that owns the transaction */
    if (!nvm_region_rng(td->region, mutex, sizeof(nvm_amutex)))
        nvms_assert_fail("NVM mutex not in transaction region");

    /* If this transaction already has this mutex locked X then nothing
     * needs to be done. */
    if (mutex=>owners == tx=>slot)
        return 1;

    /* Get a lock operation record added to the current transaction. It starts
     * in state free if getting a share lock. State exclusive is set for an
     * exclusive lock since the mutex will indicate if the lock was acquired.
     * This also verifies the transaction state. */
    nvm_lock_state st = excl ? nvm_lock_x : nvm_lock_free_s;
    nvm_lkrec ^lk = nvm_add_lock_op(tx, td, mutex, st);

    /* Verify the lock level is increasing, or there is a timeout to prevent
     * deadlocks. A mutex at level zero can only be acquired with a timeout. */
    if (timeout < 0 && lk=>old_level >= mutex=>level)
        nvms_assert_fail("Out of order locking can deadlock");

    /* Find and lock the wait list that this mutex hashes to. */
    nvm_wait_list *wl = nvm_get_wait_list(tx, mutex);

    /* Verify the mutex is still initialized */
again:
    if (mutex=>initialized != 1)
        nvms_assert_fail("Locking an uninitialized mutex");


    /* Check to see if the lock can be granted. For fairness we do not 
     * acquire a lock that is available if there are conflicting waiters
     * and we have never waited. */
    if ((mutex=>owners == 0 || (!excl && mutex=>owners < 0)) &&
        (waited || (mutex=>x_waiters == 0 && (mutex=>s_waiters == 0 || !excl))))
    {
        /* Lock the mutex by changing its owners field. This is more complex
         * for a share lock since it is not possible to determine if the lock
         * was acquired by looking at the mutex itself. */
        if (excl)
        {
            /* Acquiring an exclusive lock is simply a matter of setting the
             * transaction slot number in the owners field. The lock state is
             * always nvm_lock_x for an exclusive lock. */
            mutex=>owners ~= tx=>slot;

            /* If necessary, advance the new level to reflect this lock being
             * held. If we die here with owners and new_level being
             * inconsistent, there will not be a problem since the first thing
             * recovery will do is release this lock. */
            if (mutex=>level > lk=>new_level)
                lk=>new_level ~= mutex=>level;
            nvm_persist();

            /* Update persists for locking. */
            td->persist_lock += 1;
        }
        else
        {
            /* Share locking is tricky because we want to atomically change
             * the mutex owners and the state of the lock operation record.
             * This wants to be atomic so we can tell if the count of shared
             * owners has been decremented by this lock. However there is a
             * small window where application death can leave them out of sync.
             * We solve this by persistently setting the lock operation state
             * to acquiring, decrement the owners field of the mutex
             * persistently, and finally setting the lock operation to held.
             * Before recovery does any recovery it looks through all the lock
             * operations in all the transactions needing recovery to see if
             * any are in state acquiring shared. If this happens then all the
             * lock operations must be scanned to calculate the real share
             * count. Note that there can be at most one lock in this state on
             * a given NVM mutex since an exclusive volatile mutex is held
             * when acquiring a lock on an NVM mutex. */
            lk=>state ~= nvm_lock_acquire_s;
            nvm_persist();
            mutex=>owners~--;
            nvm_persist();
            if (mutex=>level > lk=>new_level)
                lk=>new_level ~= mutex=>level;
            lk=>state ~= nvm_lock_held_s;
            nvm_persist();

            /* If we waited and acquired a share lock when there were no other
             * locks and there is a X waiter, then the last release most likely
             * just posted us. If there are any share waiters, then wake them
             * since they are compatible with the lock we just acquired. */
            if (w != NULL && mutex=>owners == -1 && mutex=>x_waiters)
                if (mutex=>s_waiters)
                    nvms_cond_post(w->cond, 1);

            /* Update persists for locking. */
            td->persist_lock += 3;
        }

        /* Return success */
        nvms_unlock_mutex(wl->mutex);
        return 1;
    }

    /* We have to wait for the lock to be available. If the timeout is zero
     * we have already failed. */
    if (timeout == 0)
        goto fail;

    /* If we waited once already check to see if our timeout is up */
    if (timeout > 0 && waited && nvms_utime() >= stop)
    {
fail:
        nvms_unlock_mutex(wl->mutex);
        nvm_apply_one(tx, td); // throw away the unnecessary lock op undo record
        return 0;
    }

    /* If this is a timed wait then determine the stop time if not already 
     * determined. */
    if (timeout > 0 && stop == 0)
        stop = nvms_utime() + timeout;

    /* Look for the nvm_wait struct associated with this mutex. If there is a
     * nvm_wait without any waiters then we can use it for this mutex. If there
     * are none available then we will need to make one. */
    w = wl->head;
    nvm_wait *idle = NULL;
    while (w != NULL)
    {
        if (w->nvmx == mutex)
            break;

        /* save the first idle wait struct that we find */
        if (idle == NULL && w->nvmx=>x_waiters == 0 && w->nvmx=>s_waiters == 0)
            idle = w;

        w = w->link; // next wait struct
    }

    /* See if we can use an existing wait struct that is no longer in use */
    if (w == NULL && idle != NULL)
    {
        idle->nvmx = mutex;
        w = idle;
    }

    /* if w is null then we need to construct a new nvm_wait for this mutex */
    if (w == NULL)
    {
        /* If there is no wait struct, but there are wait counts on the mutex,
         * then something is seriously wrong since recovery should have cleared
         * them. */
        if (mutex=>s_waiters || mutex=>x_waiters)
            nvms_corruption("NVM mutex has waiter, but no nvm_wait", mutex, wl);

        /* Allocate a wait struct for our nvm_amutex and add it to this
         * bucket. */
        w = nvms_app_alloc(sizeof(nvm_wait));
        w->nvmx = mutex;
        w->link = wl->head;
        w->cond = nvms_create_cond();
        wl->head = w;
    }

    /* increment the correct waiter count. */
    if (excl)
        mutex=>x_waiters++;
    else
        mutex=>s_waiters++;

    /* Sleep until time is up or we get posted. */
    nvms_cond_wait(w->cond, wl->mutex, stop);

    /* restore waiter count */
    if (excl)
        mutex=>x_waiters--;
    else
        mutex=>s_waiters--;

    /* See if we can get it now. Note that we give it one last check even if
     * our sleep timed out. */
    waited = 1;
    goto again;
}
#else
int nvm_lock(   
        nvm_mutex *omutex,
        int excl,
        int timeout
        )
{
    /* TODO+ This should be redesigned to use a compare and swap to avoid any
     * service library calls when there is no contention. */

    /* convert opaque nvm_mutex to actual NVM mutex */
    nvm_amutex *mutex = (nvm_amutex*)omutex;

    /* Verify the mutex has been initialized. */
    if (mutex->initialized != 1)
        nvms_assert_fail("Locking an uninitialized mutex");

    /* If there is a timeout this will get the absolute time when it ends.
     * We do not waste time calculating the stop time unless we have to wait. */
    uint64_t stop = 0;

    /* This is 1 if we have waited once and thus no longer need to defer to
     * other waiters for fairness. */
    int waited = 0;

    /* If we wait, this will point to the wait struct. We may need this to post
     * other share waiters when we get a share lock. */
    nvm_wait *w = NULL;

    /* Get the current transaction and verify its type and state. */
    nvm_thread_data *td = nvm_get_thread_data();
    nvm_transaction *tx = td->transaction;
    if (tx == NULL)
        nvms_assert_fail("Attempt to lock NVM mutex without a transaction");
    nvm_verify(tx, shapeof(nvm_transaction));

    /* verify the mutex is in the same region that owns the transaction */
    if (!nvm_region_rng(td->region, mutex, sizeof(nvm_amutex)))
        nvms_assert_fail("NVM mutex not in transaction region");

    /* If this transaction already has this mutex locked X then nothing
     * needs to be done. */
    if (mutex->owners == tx->slot)
        return 1;

    /* Get a lock operation record added to the current transaction. It starts
     * in state free if getting a share lock. State exclusive is set for an
     * exclusive lock since the mutex will indicate if the lock was acquired.
     * This also verifies the transaction state. */
    nvm_lock_state st = excl ? nvm_lock_x : nvm_lock_free_s;
    nvm_lkrec *lk = nvm_add_lock_op(tx, td, mutex, st);

    /* Verify the lock level is increasing, or there is a timeout to prevent
     * deadlocks. A mutex at level zero can only be acquired with a timeout. */
    if (timeout < 0 && lk->old_level >= mutex->level)
        nvms_assert_fail("Out of order locking can deadlock");

    /* Find and lock the wait list that this mutex hashes to. */
    nvm_wait_list *wl = nvm_get_wait_list(tx, mutex);

    /* Verify the mutex is still initialized */
again:
    if (mutex->initialized != 1)
        nvms_assert_fail("Locking an uninitialized mutex");

    /* Check to see if the lock can be granted. For fairness we do not 
     * acquire a lock that is available if there are conflicting waiters
     * and we have never waited. */
    if ((mutex->owners == 0 || (!excl && mutex->owners < 0)) &&
        (waited || (mutex->x_waiters == 0 && (mutex->s_waiters == 0 || !excl))))
    {
        /* Lock the mutex by changing its owners field. This is more complex
         * for a share lock since it is not possible to determine if the lock
         * was acquired by looking at the mutex itself. */
        if (excl)
        {
            /* Acquiring an exclusive lock is simply a matter of setting the
             * transaction slot number in the owners field. The lock state is
             * always nvm_lock_x for an exclusive lock. */
            mutex->owners = tx->slot;
            nvm_persist1(&mutex->owners);

            /* If necessary, advance the new level to reflect this lock being
             * held. If we die here with owners and new_level being
             * inconsistent, there will not be a problem since the first thing
             * recovery will do is release this lock. */
            if (mutex->level > lk->new_level)
                lk->new_level = mutex->level;
            nvm_persist1(&lk->new_level);

            /* Update persists for locking. */
            td->persist_lock += 1;
        }
        else
        {
            /* Share locking is tricky because we want to atomically change
             * the mutex owners and the state of the lock operation record.
             * This wants to be atomic so we can tell if the count of shared
             * owners has been decremented by this lock. However there is a
             * small window where application death can leave them out of sync.
             * We solve this by persistently setting the lock operation state
             * to acquiring, decrement the owners field of the mutex
             * persistently, and finally setting the lock operation to held.
             * Before recovery does any recovery it looks through all the lock
             * operations in all the transactions needing recovery to see if
             * any are in state acquiring shared. If this happens then all the
             * lock operations must be scanned to calculate the real share
             * count. Note that there can be at most one lock in this state on
             * a given NVM mutex since an exclusive volatile mutex is held
             * when acquiring a lock on an NVM mutex. */
            lk->state = nvm_lock_acquire_s;
            nvm_persist1(&lk->state);
            mutex->owners--;
            nvm_persist1(&mutex->owners);
            if (mutex->level > lk->new_level)
                lk->new_level = mutex->level;
            lk->state = nvm_lock_held_s;
            nvm_persist1(&lk->state);

            /* If we waited and acquired a share lock when there were no other
             * locks and there is a X waiter, then the last release most likely
             * just posted us. If there are any share waiters, then wake them
             * since they are compatible with the lock we just acquired. */
            if (w != NULL && mutex->owners == -1 && mutex->x_waiters)
                if (mutex->s_waiters)
                    nvms_cond_post(w->cond, 1);

            /* Update persists for locking. */
            td->persist_lock += 3;
        }

        /* Return success */
        nvms_unlock_mutex(wl->mutex);
        return 1;
    }

    /* We have to wait for the lock to be available. If the timeout is zero
     * we have already failed. */
    if (timeout == 0)
        goto fail;

    /* If we waited once already check to see if our timeout is up */
    if (timeout > 0 && waited && nvms_utime() >= stop)
    {
fail:
        nvms_unlock_mutex(wl->mutex);
        nvm_apply_one(tx, td); // throw away the unnecessary lock op undo record
        return 0;
    }

    /* If this is a timed wait then determine the stop time if not already 
     * determined. */
    if (timeout > 0 && stop == 0)
        stop = nvms_utime() + timeout;

    /* Look for the nvm_wait struct associated with this mutex. If there is
     * none then we will need to make one. */
    w = wl->head;
    nvm_wait *idle = NULL;
    while (w != NULL)
    {
        if (w->nvmx == mutex)
            break;

        /* save the first idle wait struct that we find */
        if (idle == NULL && w->nvmx->x_waiters == 0 && w->nvmx->s_waiters == 0)
            idle = w;

        w = w->link;
    }

    /* See if we can use an existing wait struct that is no longer in use */
    if (w == NULL && idle != NULL)
    {
        idle->nvmx = mutex;
        w = idle;
    }

    /* if w is null then we need to construct a new nvm_wait for this mutex */
    if (w == NULL)
    {
        /* If there is no wait struct, but there are wait counts on the mutex,
         * then something is seriously wrong since recovery should have cleared
         * them. */
        if (mutex->s_waiters || mutex->x_waiters)
            nvms_corruption("NVM mutex has waiter, but no nvm_wait", mutex, wl);

        /* Allocate a wait struct for our nvm_amutex and add it to this
         * bucket. */
        w = nvms_app_alloc(sizeof(nvm_wait));
        w->nvmx = mutex;
        w->link = wl->head;
        w->cond = nvms_create_cond();
        wl->head = w;
    }

    /* increment the correct waiter count. */
    if (excl)
        mutex->x_waiters++;
    else
        mutex->s_waiters++;

    /* Sleep until time is up or we get posted. */
    nvms_cond_wait(w->cond, wl->mutex, stop);

    /* restore waiter count */
    if (excl)
        mutex->x_waiters--;
    else
        mutex->s_waiters--;

    /* See if we can get it now. Note that we give it one last check even if
     * our sleep timed out. */
    waited = 1;
    goto again;
}
#endif //NVM_EXT
/**
 * This is called to unlock an NVM mutex when a transaction commits or an NVM
 * lock undo record is applied for rollback or commit. It is given the undo
 * record that was constructed when the lock was acquired.
 * 
 * @param tx[in] 
 * The transaction that owns the lock.
 * 
 * @param td[in]
 * The thread private data for the thread owning the transaction.
 * 
 * @param lk[in]
 * The lock undo record created by nvm_lock.
 */
#ifdef NVM_EXT
void nvm_unlock@(nvm_transaction ^tx, nvm_thread_data *td, nvm_lkrec ^lk)
{
    /* Get a pointer to the nvm_amutex we are unlocking  */
    nvm_amutex ^mutex = lk=>mutex;

    /* Determine if lock is exclusive or shared. */
    int excl = lk=>state == nvm_lock_x;

    /* If the lock is not held then nothing to do. */
    if (lk=>state == nvm_lock_free_s || (excl && mutex=>owners != tx=>slot))
        return;

    /* Find and lock the wait list that this mutex hashes to. This locks the
     * mutex that protects this NVM mutex. */
    nvm_wait_list *wl = nvm_get_wait_list(tx, mutex);

    /* If there are any waiters then find the wait struct they are waiting on */
    nvm_wait *w = NULL;
    if (mutex=>x_waiters || mutex=>s_waiters)
    {
        w = wl->head;
        while (w->nvmx != mutex)
        {
            if (w == NULL)
                nvms_corruption("Waiters, but no nvm_wait struct", mutex, wl);
            w = w->link;
        }
    }

    if (excl)
    {
        /* persistently release the lock */
        mutex=>owners ~= 0;
        nvm_persist();

        /* Update persists for locking. */
        td->persist_lock += 1;
    }
    else
    {
        /* We should not be attempting to unlock a mutex we did not lock. */
        if (mutex=>owners >= 0)
            nvms_corruption("Unlocking an S lock we do not own", tx, mutex);

        /* release the lock */
        lk=>state ~= nvm_lock_release_s;
        nvm_persist();
        mutex=>owners~++; // incrementing makes less negative i.e. less waiters
        nvm_persist();
        lk=>state ~= nvm_lock_free_s;
        nvm_persist();

        /* Update persists for locking. */
        td->persist_lock += 3;
    }

    /* If there are any X waiters and the mutex is no longer locked then wake
     * only one thread. This allows the sleep order on the condition to
     * implement fairness. If it gets an S lock then it will wake other shared
     * waiters. If it gets an X lock then it will wake other waiters when it
     * releases the lock. Otherwise if there are share waiters then wake them
     * all. This may wake some X waiters that will just go back to sleep, but
     * at least all the share waiters will get a lock. */
    if (mutex=>x_waiters != 0 || mutex=>s_waiters != 0)
    {
        if (mutex=>x_waiters && mutex=>owners == 0)
            nvms_cond_post(w->cond, 0);
        else
            if (mutex=>s_waiters)
            nvms_cond_post(w->cond, 1);
    }

    /* Done with the wait list */
    nvms_unlock_mutex(wl->mutex);
}
#else
void nvm_unlock(nvm_transaction *tx, nvm_thread_data *td, nvm_lkrec *lk)
{
    /* Get a pointer to the nvm_amutex we are unlocking  */
    nvm_amutex *mutex = nvm_amutex_get(&lk->mutex);

    /* Determine if lock is exclusive or shared. */
    int excl = lk->state == nvm_lock_x;

    /* If the lock is not held then nothing to do. */
    if (lk->state == nvm_lock_free_s || (excl && mutex->owners != tx->slot))
        return;

    /* Find and lock the wait list that this mutex hashes to. This locks the
     * mutex that protects this NVM mutex. */
    nvm_wait_list *wl = nvm_get_wait_list(tx, mutex);

    /* If there are any waiters then find the wait struct they are waiting on */
    nvm_wait *w = NULL;
    if (mutex->x_waiters || mutex->s_waiters)
    {
        w = wl->head;
        while (w->nvmx != mutex)
        {
            if (w == NULL)
                nvms_corruption("Waiters, but no nvm_wait struct", mutex, wl);
            w = w->link;
        }
    }

    if (excl)
    {
        /* persistently release the lock */
        mutex->owners = 0;
        nvm_persist1(&mutex->owners);

        /* Update persists for locking. */
        td->persist_lock += 1;
    }
    else
    {
        /* We should not be attempting to unlock a mutex we did not lock. */
        if (mutex->owners >= 0)
            nvms_corruption("Unlocking an S lock we do not own", tx, mutex);

        /* release the lock */
        lk->state = nvm_lock_release_s;
        nvm_persist1(&lk->state);
        mutex->owners++; // incrementing makes less negative i.e. less waiters
        nvm_persist1(&mutex->owners);
        lk->state = nvm_lock_free_s;
        nvm_persist1(&lk->state);

        /* Update persists for locking. */
        td->persist_lock += 3;
    }

    /* If there are any X waiters and the mutex is no longer locked then wake
     * only one thread. This allows the sleep order on the condition to
     * implement fairness. If it gets an S lock then it will wake other shared
     * waiters. If it gets an X lock then it will wake other waiters when it
     * releases the lock. Otherwise if there are share waiters then wake them
     * all. This may wake some X waiters that will just go back to sleep, but
     * at least all the share waiters will get a lock. */
    if (mutex->x_waiters != 0 || mutex->s_waiters != 0)
    {
        if (mutex->x_waiters && mutex->owners == 0)
            nvms_cond_post(w->cond, 0);
        else
            if (mutex->s_waiters)
            nvms_cond_post(w->cond, 1);
    }

    /* Done with the wait list */
    nvms_unlock_mutex(wl->mutex);
}
#endif //NVM_EXT
/**
 * Allocate a wait table for threads that need to block on NVM mutexes in
 * this region. It is allocated in volatile application global memory. The
 * volatile mutex in each bucket is initialized. When this returns success,
 * the region data will point at the allocated waiter table.
 * 
 * @param rd
 * Pointer to the region data for the region containing the NVM mutexes
 *     
 * @param[in] sz
 * The number of buckets to allocate for the wait table.

 * @return 
 * Returns 1 on success and 0 if allocation or service mutex creation fails
 */
int nvm_wait_table_create(nvm_region_data *rd, unsigned sz)
{
    /* Allocate a new wait table. */
    nvm_wait_table *wt = nvms_app_alloc(
            sizeof(nvm_wait_table) + sizeof(nvm_wait) * sz);
    if (wt == NULL)
        return 0; // allocation failed
    rd->waiter = wt; // set table address in region data

    /* Initialize the mutexes for each bucket */
    wt->buckets = sz;
    int b;
    for (b = 0; b < wt->buckets; b++)
    {
        nvms_mutex *mx = &wt->table[b].mutex;
        *mx = nvms_create_mutex();
        if (*mx == NULL)
            return 0;
    }
    return 1;
}
/**
 * This is called to cleanup the waiter table when detaching a region or
 * cleaning up after a failed attach. An assert fires if there are any
 * threads waiting on any mutexes in this region when this is called. The
 * easiest way to ensure this is to verify there are no active transactions.
 * 
 * This has no effect on any persistent data so there is no need to call 
 * this if the entire application is terminating.
 * 
 * On return the pointer to the waiter table in the region data will be
 * cleared. If it is already cleared then this will return without doing
 * anything.
 * 
 * @param[in] rd
 * Pointer to the region data for the region containing the waiter table
 * 
 * @return
 * 1 if successful and 0 if we had to give up because we could not wait
 */
int nvm_wait_table_destroy(nvm_region_data *rd)
{
    /* find the waiter table and return if there is none. */
    nvm_wait_table *wt = rd->waiter;
    if (wt == NULL)
        return 1;

    /* Go through each hash bucket removing the wait structs linked to it, 
     * and clenaing up its mutex. */
    int b;
    for (b = 0; b < wt->buckets; b++)
    {
        /* the next list to destroy */
        nvm_wait_list *wl = &wt->table[b];

        /* If the mutex was not created or was deleted, then nothing to do for
         * this list. */
        if (wl->mutex == NULL)
            continue;

        /* Destroy wait structures until they are all gone. */
        while (wl->head != NULL)
        {
            /* Destroy the service condition variable if it still exists.*/
            nvm_wait *w = wl->head;
            if (w->cond != NULL)
            {
                nvms_destroy_cond(w->cond);
                w->cond = NULL; // no more cond
            }

            /* Release the wait struct and remove it from the list. */
            wl->head = w->link; // remove from list
            if (!nvms_app_free(w))
                return 0;
        }

        /* This wait list is empty so destroy its mutex */
        nvms_destroy_mutex(wl->mutex);
        wl->mutex = NULL;
    }

    /* The table is empty so it can be freed now. */
    if (!nvms_app_free(wt))
        return 0;
    rd->waiter = NULL;
    return 1;
}
/**
 * After recovery discovers all the dead transactions and before doing
 * recovery, it needs to resolve the state of any NVM locks that were in
 * flux when the NVM region was last detached. For every dead transaction
 * that had a lock in the middle of acquisition or release, this routine
 * is called to resolve its state.
 * @param ttd
 * @param lk
 */
#ifdef NVM_EXT
void nvm_recover_lock(nvm_trans_table_data *ttd, nvm_lkrec ^lk)
{
    /* Find the NVM mutex this lock refers to. */
    nvm_amutex ^mutex = lk=>mutex;

    /* If there are any waiters, they are no longer waiting. Note that only
     * mutexes with a dead transaction attempting to lock them could have
     * waiters. Thus when all calls to nvm_recover_lock are complete this
     * will guarantee there are no mutexes with non-zero waiter counts. */
    if (mutex=>s_waiters || mutex=>x_waiters)
    {
        mutex=>s_waiters = 0;
        mutex=>x_waiters = 0;
    }

    /* If the lock state indicates it was in the middle of share locking or
     * unlocking the mutex, then we need to ensure the mutex owner is
     * correct. We always return this lock to the unlocked state when
     * this happens since that is valid. */
    switch (lk=>state)
    {
    default:
        break; // all other states are OK

    case nvm_lock_acquire_s:
    case nvm_lock_release_s:
        /* Died while acquiring or releasing an S lock. Since mutex owners
         * are only updated under the wait list volatile mutex, there can
         * only be one transaction in this lock state for this NVM mutex. 
         * If the owners is zero then we clearly do not have the lock, so 
         * all that is needed is to fix the lock state. */
        if (mutex=>owners)
        {
            /* Since the lock was available for share locking we cannot
             * determine if the owner count includes our lock just by looking 
             * at the lock. We need to scan all the dead transactions counting
             * the number of share locks on this NVM mutex in state held. */
            int cnt = nvm_find_shares(ttd, mutex);
            if (mutex=>owners != -cnt)
            {
                /* Our lock got into the owners count so fix it. Note that
                 * the new owners value must be persistent before we change 
                 * the lock state. */
                mutex=>owners ~= -cnt;
                nvm_persist1(mutex);
            }
        }
        lk=>state ~= nvm_lock_free_s;
        break;
    }
}
#else
void nvm_recover_lock(nvm_trans_table_data *ttd, nvm_lkrec *lk)
{
    /* Find the NVM mutex this lock refers to. */
    nvm_amutex *mutex = nvm_amutex_get(&lk->mutex);

    /* If there are any waiters, they are no longer waiting. Note that only
     * mutexes with a dead transaction attempting to lock them could have
     * waiters. Thus when all calls to nvm_recover_lock are complete this
     * will guarantee there are no mutexes with non-zero waiter counts. */
    if (mutex->s_waiters || mutex->x_waiters)
    {
        mutex->s_waiters = 0;
        mutex->x_waiters = 0;
        nvm_flush1(mutex);
    }

    /* If the lock state indicates it was in the middle of share locking or
     * unlocking the mutex, then we need to ensure the mutex owner is
     * correct. We always return this lock to the unlocked state when
     * this happens since that is valid. */
    switch (lk->state)
    {
    default:
        break; // all other states are OK

    case nvm_lock_acquire_s:
    case nvm_lock_release_s:
        /* Died while acquiring or releasing an S lock. Since mutex owners
         * are only updated under the wait list volatile mutex, there can
         * only be one transaction in this lock state for this NVM mutex. 
         * If the owners is zero then we clearly do not have the lock, so 
         * all that is needed is to fix the lock state. */
        if (mutex->owners)
        {
            /* Since the lock was available for share locking we cannot
             * determine if the owner count includes our lock just by looking 
             * at the lock. We need to scan all the dead transactions counting
             * the number of share locks on this NVM mutex in state held. */
            int cnt = nvm_find_shares(ttd, mutex);
            if (mutex->owners != -cnt)
            {
                /* Our lock got into the owners count so fix it. Note that
                 * the new owners value must be persistent before we change 
                 * the lock state. */
                mutex->owners = -cnt;
                nvm_persist1(mutex);
            }
        }
        lk->state = nvm_lock_free_s;
        nvm_flush1(&lk->state);
        break;
    }
}
#endif //NVM_EXT
