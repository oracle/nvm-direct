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
      nvms_locking.h - NVM services for volatile memory locking

    DESCRIPTION\n
      The NVM library needs mutexes for coordinating access to volatile memory 
      between threads. This header defines the interface that the NVM library
      needs implemented.

 */

#ifndef NVMS_LOCKING_H
#define	NVMS_LOCKING_H
#include <stddef.h>
#include <stdint.h>

#ifdef	__cplusplus
extern "C"
{
#endif

    /**
     * This defines an opaque type used as a handle to an application global
     * mutex that can be used to coordinate between threads within the same 
     * application.
     */
    typedef void *nvms_mutex;

    /**
     * This allocates and initializes an application global mutex.
     * 
     * If there are any errors then errno is set and the return value is zero.
     * 
     * @return
     * An opaque handle to the mutex, or zero if there is an error.
     */
    nvms_mutex nvms_create_mutex(void);

    /**
     * This destroys an application global mutex freeing the memory it consumed.
     * The mutex must not be locked when destroyed.
     * 
     * @param[in] mutex
     * Handle to the mutex to destroy.
     */
    void nvms_destroy_mutex(nvms_mutex mutex);

    /**
     * When this returns 1 the calling thread has an exclusive lock on the 
     * indicated mutex. If the wait parameter is true then the thread might 
     * sleep waiting for another thread to release its lock. If wait is false 
     * then 0 is returned rather than wait for the holder to release its lock. 
     * 
     * @param[in] mutex
     * Handle to the mutex to lock.
     * 
     * @param[in] wait
     * If 0 thread returns 0 rather than wait for a conflicting lock to be 
     * released.
     * 
     * @return 
     * 1 if lock acquired, 0 if not acquired due to conflicting lock.
     */
    int nvms_lock_mutex(
        nvms_mutex mutex,
        int wait
        );

    /**
     * When this returns the calling thread no longer has a lock on the 
     * indicated mutex. This might wakeup another thread waiting for a lock. 
     * The calling thread must have locked the mutex via nvms_lock_mutex.
     * If the caller previously registered a recovery operation, the opcode is 
     * cleared to zero before the mutex is unlocked.
     * 
     * @param[in] mutex
     * Handle to the mutex to unlock.
     */
    void nvms_unlock_mutex(
        nvms_mutex mutex
        );


    /**
     * This does a 2 byte compare and swap of a location in NVM or volatile 
     * memory. The return value is the value of the location when the CAS 
     * executed. If it is equal to the old value then the swap succeeded.
     * 
     * @param[in] ptr
     * The address of location to modify
     * 
     * @param[in] oldval
     * The expected value to change
     * 
     * @param[in] newval
     * The new value to store if compare is successful
     * 
     * @return 
     * The value that was in memory when the CAS fetched the location
     */
    uint16_t nvms_cas2(
        volatile uint16_t *ptr, // 
        uint16_t oldval, // 
        uint16_t newval // new value to store if compare is successful
        );

    /**
     * This does a 4 byte compare and swap of a location in NVM or volatile 
     * memory. The return value is the value of the location when the CAS 
     * executed. If it is equal to the old value then the swap succeeded.
     * 
     * @param[in] ptr
     * The address of location to modify
     * 
     * @param[in] oldval
     * The expected value to change
     * 
     * @param[in] newval
     * The new value to store if compare is successful
     * 
     * @return 
     * The value that was in memory when the CAS fetched the location
     */
    uint32_t nvms_cas4(
        volatile uint32_t *ptr, // 
        uint32_t oldval, // 
        uint32_t newval // new value to store if compare is successful
        );

    /**
     * This does an 8 byte compare and swap of a location in NVM or volatile 
     * memory. The return value is the value of the location when the CAS 
     * executed. If it is equal to the old value then the swap succeeded.
     * 
     * @param[in] ptr
     * The address of location to modify
     * 
     * @param[in] oldval
     * The expected value to change
     * 
     * @param[in] newval
     * The new value to store if compare is successful
     * 
     * @return 
     * The value that was in memory when the CAS fetched the location
     */
    uint64_t nvms_cas8(
        volatile uint64_t *ptr, // 
        uint64_t oldval, // 
        uint64_t newval // new value to store if compare is successful
        );

#ifdef	__cplusplus
}
#endif

#endif	/* NVMS_MUTEX_H */

