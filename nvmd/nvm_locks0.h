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
      nvm_locks0.h - private declarations for NVM locking

    DESCRIPTION\n
      This contains the declarations for NVM locking that are
      private to the NVM library

 */
#ifndef NVMS_LOCKING_H
#include "nvms_locking.h"
#endif

#ifndef NVMS_SLEEPING_H
#include "nvms_sleeping.h"
#endif

#ifndef NVM_DATA0_H
#include "nvm_data0.h"
#endif

#ifndef NVM_REGION0_H
#include "nvm_region0.h"
#endif


#ifndef NVM_LOCKS0_H
#define	NVM_LOCKS0_H

#ifdef	__cplusplus
extern "C"
{
#endif
/**
 * This is the actual struct for an NVM mutex. The opaque type nvm_mutex in
 * nvm_locks.h is what applications use to declare mutexes in persistent
 * structs.
 */
#ifdef NVM_EXT
persistent struct nvm_amutex
tag("Mutex to lock NVM data structures for a transaction")
alignas(8)
size(8)
{
    /** Force 8 byte alignment without precompiler support. */
    uint64_t _align[0];

    /**
     * This describes how the mutex is currently locked. If it is zero,
     * then there is no lock. If it is positive, then it holds the
     * transaction slot number of the exclusive holder. If it is negative,
     * then it is the count of share lock holders.
     */
    int16_t owners;

    /**
     * This is the number of threads that are blocked waiting to acquire
     * an exclusive lock on this nvm_amutex.
     */
    transient
    uint16_t x_waiters;

    /**
     * This is the number of threads that are blocked waiting to acquire
     * a share lock on this nvm_amutex.
     */
    transient
    uint16_t s_waiters;

    /**
     * This is the lock level that the nvm_amutex was initialized with. It
     * never changes after initialization. A level of zero means only
     * no-wait locking is allowed. Otherwise it is an error to do a wait
     * lock if another lock is held on an nvm_amutex with a greater than or
     * equal level.
     */
    uint8_t level;

    /**
     * This is 0 if nvm_mutex_init has not been called to initialize this
     * mutex. Attempting to lock an uninitialized mutex will fire an
     * assert.
     */
    uint8_t initialized;
};
typedef persistent struct nvm_amutex nvm_amutex;
#else
struct nvm_amutex
{
    /** Force 8 byte alignment without precompiler support. */
    uint64_t _align[0]; //#

    /**
     * This describes how the mutex is currently locked. If it is zero,
     * then there is no lock. If it is positive, then it holds the
     * transaction slot number of the exclusive holder. If it is negative,
     * then it is the count of share lock holders.
     */
    int16_t owners;

    /**
     * This is the number of threads that are blocked waiting to acquire
     * an exclusive lock on this nvm_amutex.
     */
    uint16_t x_waiters;

    /**
     * This is the number of threads that are blocked waiting to acquire
     * a share lock on this nvm_amutex.
     */
    uint16_t s_waiters;

    /**
     * This is the lock level that the nvm_amutex was initialized with. It
     * never changes after initialization. A level of zero means only
     * no-wait locking is allowed. Otherwise it is an error to do a wait
     * lock if another lock is held on an nvm_amutex with a greater than or
     * equal level.
     */
    uint8_t level;

    /**
     * This is 0 if nvm_mutex_init has not been called to initialize this
     * mutex. Attempting to lock an uninitialized mutex will fire an
     * assert.
     */
    uint8_t initialized;
};
typedef struct nvm_amutex nvm_amutex;
NVM_SRP(nvm_amutex)
#endif //NVM_EXT

/**
 * This is an array of NVM mutexes that can cover a large number of
 * persistent structs. The address of a struct is hashed to pick one
 * mutex from the array. The size of the array is specified when it
 * is created. The larger the array the fewer false collisions there
 * will be.
 */
#ifdef NVM_EXT
persistent struct nvm_mutex_array
USID("334f c14d 262d a28c e5e0 4e9e 3750 9c23")
tag("Array of mutexes to lock objects by their address")
alignas(8)
size(24)
{
    /**
     * This is the number of mutexes in the array.
     */
    uint32_t count;

    /**
     * This is used to track the number of destroyed mutexes in
     * nvm_destroy_mutex_array so that rollback can reinitialize them
     */
    uint32_t destroyed;

    /**
     * This is the extensible array of the actual mutexes
     */
    nvm_amutex mutexes[];
};
#else
struct nvm_mutex_array
{
    /**
     * The USID that maps to the nvm_type for nvm_mutex_array
     */
    nvm_usid type_usid;

    /**
     * This is the number of mutexes in the array.
     */
    uint32_t count;

    /**
     * This is used to track the number of destroyed mutexes in
     * nvm_destroy_mutex_array so that rollback can reinitialize them
     */
    uint32_t destroyed;

    /**
     * This is the extensible array of the actual mutexes
     */
    nvm_amutex mutexes[];
};
NVM_SRP(nvm_mutex_array);
#endif //NVM_EXT

/* Internal version of nvm_create_mutex_array that allows levels over 200 */
#ifdef NVM_EXT
nvm_mutex_array ^nvm_create_mutex_array1@(
    nvm_heap ^heap,
    uint32_t count,
    uint8_t level
    );
#else
nvm_mutex_array *nvm_create_mutex_array1(
    nvm_heap *heap,
    uint32_t count,
    uint8_t level
    );
#endif

    /**
     * There is an nvm_wait struct for every NVM mutex that has a waiter or
     * recently had waiters.
     */
#ifdef NVM_EXT
    struct nvm_wait
    {
        struct nvm_wait *link;
        nvms_cond cond;
        nvm_amutex ^nvmx;
    };
#else
    struct nvm_wait
    {
        struct nvm_wait *link;
        nvms_cond cond;
        nvm_amutex *nvmx;
    };
#endif //NVM_EXT

    /**
     * The nvm_wait structures are distributed into hash buckets based on the
     * address of the nvm_amutex. An nvm_wait_list struct is one bucket in the
     * hash table.
     */
    struct nvm_wait_list
    {
        nvms_mutex mutex;
        nvm_wait *head;
    };

    /**
     * This defines the hash table used to wait for an nvm_amutex to be available
     * for locking. There is one of these in application global memory for
     * each region.
     */
    struct nvm_wait_table
    {
        uint32_t buckets; // the number of buckets in the hash table
        nvm_wait_list table[];
    };
    typedef struct nvm_wait_table nvm_wait_table;

    /**
     * Allocate a wait table for threads that need to block on NVM mutexes in
     * this region. It is allocated in volatile application global memory. The
     * volatile mutex in each bucket is initialized. When this returns success,
     * the region data will point at the allocated waiter table.
     *
     * @param[in] rd
     * Pointer to the region data for the region containing the NVM mutexes
     *
     * @param[in] sz
     * The number of buckets to allocate for the wait table.
     *
     * @return
     * Returns 1 on success and 0 if allocation or service mutex creation fails
     */
    int nvm_wait_table_create(nvm_region_data *rd, unsigned sz);

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
    int nvm_wait_table_destroy(nvm_region_data *rd);

    /**
     * Internal version of nvm_mutex_init that allows levels over 200
     */
#ifdef NVM_EXT
    void nvm_mutex_init1(
        nvm_amutex ^mutex,
        uint8_t level
        );
#else
    void nvm_mutex_init1(
        nvm_amutex *mutex,
        uint8_t level
        );
#endif //NVM_EXT

    /**
     * This is called to unlock an NVM mutex when a transaction commits or an
     * NVM lock undo record is applied for rollback or commit. It is given the
     * undo record that was constructed when the lock was acquired.
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
    void nvm_unlock@(
        nvm_transaction ^tx,
        nvm_thread_data *td,
        nvm_lkrec ^lk
        );
#else
    void nvm_unlock(
        nvm_transaction *tx,
        nvm_thread_data *td,
        nvm_lkrec *lk
        );
#endif //NVM_EXT

    /**
     * After recovery discovers all the transactions threads and before doing
     * recovery, it needs to resolve the state of any NVM locks that were in
     * flux when the NVM region was last detached. For every dead transaction
     * that had a lock in the middle of acquisition or release, this routine
     * is called to resolve its state.
     *
     * @param ttd[in]
     * Transaction table volatile data for the region being recovered.
     *
     * @param lk[in]
     * Address of a lock that might need its state repaired.
     */
#ifdef NVM_EXT
    void nvm_recover_lock(nvm_trans_table_data *ttd, nvm_lkrec ^lk);
#else
    void nvm_recover_lock(nvm_trans_table_data *ttd, nvm_lkrec *lk);
#endif //NVM_EXT
#ifdef	__cplusplus
}
#endif

#endif	/* NVM_LOCKS0_H */

