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
      nvm_locks.h - Non-Volatile Memory locking 

    DESCRIPTION\n
      This This defines the application interface to NVM mutexes. They are
      owned by NVM transactions and are kept in non-volatile memory so that
      they survive reboot.

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

      To aid in deadlock avoidance, each NVM lock has a level associated with 
      it. A lock cannot be acquired with waiting if a lock of the same or 
      higher level is already owned by the transaction. It also has a sleep 
      algorithm to indicate how long to spin before going to sleep when there 
      is a lock conflict. There is a lock initialization call that must be made 
      to set the level and sleep algorithm.

 */

#ifndef NVM_LOCKS_H
#define	NVM_LOCKS_H

#ifdef	__cplusplus
extern "C"
{
#endif

    /**\brief A mutex that lives in NVM and can be locked by an NVM transaction.
     * 
     * This is an opaque type that an application can embed in
     * persistent structs as a lockable to coordinate access to NVM data 
     * structures. An nvm_mutex is locked by a transaction.
     */
typedef uint64_t nvm_mutex;
#ifdef NVM_EXT
#else
NVM_SRP(nvm_mutex) // define self-relative pointer operations
#endif //NVM_EXT

    /**
     * This is the highest mutex level that an application can pass to
     * nvm_mutex_init or nvm_create_mutex_array. Higher values are reserved
     * for use by NVM Direct.
     */
#define NVM_MAX_LEVEL (199)

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
     * If there are any invalid parameters then an assert is fired.
     * 
     * @param[in] mutex
     * This is the address in NVM of the mutex to initialize.
     * 
     * @param[in] level
     * This is the lock level for deadlock prevention. It must be less than or
     * equal to NVM_MAX_LEVEL.
     */
#ifdef NVM_EXT
    void nvm_mutex_init@( 
        nvm_mutex ^mutex,
        uint8_t level
        );
#else
    void nvm_mutex_init(    
        nvm_mutex *mutex,
        uint8_t level
        );
#endif //NVM_EXT
    
    /**
     * Finalize use of a mutex before freeing the NVM memory holding it. This
     * releases any volatile memory structures allocated for it, and returns
     * it to the state it was in before nvm_mutex_init was called to 
     * initialize it.
     * 
     * It is not an error to call this multiple times on the same NVM mutex,
     * or to call it on a mutex that was allocated but not initialized.
     * 
     * An assert is fired if the mutex is held or has any waiters.
     * 
     * @param[in] mutex
     * This is the address in NVM of the mutex to finalize.
     */
#ifdef NVM_EXT
    void nvm_mutex_fini@( 
        nvm_mutex ^mutex
        );
#else
    void nvm_mutex_fini(   
        nvm_mutex *mutex
        );
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
     * This is the lock level of all the mutexes. It must be less than or
     * equal to NVM_MAX_LEVEL.
     *
     * @return
     * Return is NULL if array could not be allocated and errno contains the
     * error number. A pointer to the mutex array is returned on success.
     */
#ifdef NVM_EXT
    nvm_mutex_array ^nvm_create_mutex_array@(
        nvm_heap ^heap,
        uint32_t count,
        uint8_t level
        );
#else
    nvm_mutex_array *nvm_create_mutex_array(
        nvm_heap *heap,
        uint32_t count,
        uint8_t level
        );
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
        );
#else
    nvm_mutex *nvm_pick_mutex(
        nvm_mutex_array *array,
        const void *ptr
        );
#endif //NVM_EXT

    /**
     * Get one mutex from a mutex array by an index into the array. A pointer
     * to the mutex is returned. If the index is past the end of the array a
     * null pointer is returned. This must be called in a transaction. Any
     * errors will fire an assert. This is useful for acquiring all the
     * mutexes one at a time.
     *
     * @param[in] array
     * The mutex array to pick from
     *
     * @param[in] index
     * Index into the array of the mutex to return
     *
     * @return
     * Pointer to the chosen mutex or null if there is none
     */
#ifdef NVM_EXT
    nvm_mutex ^nvm_get_mutex@(
        nvm_mutex_array ^array,
        uint32_t index
        );
#else
    nvm_mutex *nvm_get_mutex(
        nvm_mutex_array *array,
        uint32_t index
        );
#endif //NVM_EXT

    /**
     * Destroy a mutex array returning its space to the heap it was allocated
     * from. This must be called in a transaction for the containing region.
     * The array will be usable if the transaction rolls back through this
     * destroy.
     *
     * An assert will fire if any of the mutexes are held or have waiters.
     *
     * @param[in] array
     * A pointer to the array to destroy.
     */
#ifdef NVM_EXT
    void nvm_destroy_mutex_array@(
        nvm_mutex_array ^array
        );
#else
    void nvm_destroy_mutex_array(
        nvm_mutex_array *array
        );
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
#ifdef NVM_EXT
    int nvm_lock@( 
        nvm_mutex ^mutex,
        int excl,
        int timeout
        );
#else
    int nvm_lock(   
        nvm_mutex *mutex,
        int excl,
        int timeout
        );
#endif //NVM_EXT
#define NVM_LOCK_WAIT (-1) // wait forever
    
    /**
     * This is a simplified version of nvm_lock for the most common case of
     * waiting for an exclusive lock.
     * 
     * @param[in] mutex
     * This is the address in NVM of the mutex to lock exclusive.
     */
#ifdef NVM_EXT
    static inline void
    nvm_xlock@(
        nvm_mutex ^mutex    
    )
    {
        nvm_lock(mutex, 1, NVM_LOCK_WAIT);
    }
#else
    static inline void
    nvm_xlock(
        nvm_mutex *mutex
    )
    {
        nvm_lock(mutex, 1, NVM_LOCK_WAIT);
    }
#endif //NVM_EXT

    /**
     * This is a simplified version of nvm_lock for share locking.
     * 
     * @param[in] mutex
     * This is the address in NVM of the mutex to lock shared.
     */
#ifdef NVM_EXT
    static inline void
    nvm_slock@(
        nvm_mutex ^mutex   
        )
    {
        nvm_lock(mutex, 0, NVM_LOCK_WAIT);
    }
#else
    static inline void
    nvm_slock(
        nvm_mutex *mutex
        )
    {
        nvm_lock(mutex, 0, NVM_LOCK_WAIT);
    }
#endif //NVM_EXT


#ifdef	__cplusplus
}
#endif

#endif	/* NVM_LOCKS_H */

