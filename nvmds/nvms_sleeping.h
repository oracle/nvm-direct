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
      nvms_sleeping.h - NVM services for sleeping and waking up threads.

    DESCRIPTION\n
      The NVM library needs a mechanism similar to pthread condition variables 
      for blocking a thread when it needs to sleep until another thread releases
      a resource. Condition variables must be allocated in application global 
      memory so that they are available to all threads in an application.

 */

#ifndef NVMS_SLEEPING_H
#define	NVMS_SLEEPING_H

#ifdef	__cplusplus
extern "C"
{
#endif

    /**
     * This type represents a opaque handle to a condition variable provided by 
     * the services library. The handle must be usable by any thread in the 
     * application. Thus it is a pointer to application global memory.
     */
    typedef void *nvms_cond;

    /**
     * This function allocates and initializes one condition variable in 
     * application global memory and returns a handle to it.
     * 
     * If there are any errors then errno is set and the return value is zero.
     * 
     * @param[out] destroy
     * This points to a null handle that is set to the return value atomically
     * with allocation of the conditiona variable.
     * 
     * @return
     * An opaque handle to the condition variable, or zero if there is an error.
     */
    nvms_cond nvms_create_cond(void);

    /**
     * This destroys one condition variable making its handle no longer usable 
     * and releasing any memory associated with it.  There must not be any 
     * threads waiting on the condition variable when it is destroyed.
     * 
     * @param[in] cond
     * Handle for the condition variable to destroy
     */
    void nvms_destroy_cond(nvms_cond cond);

    /**
     * The calling thread will block until the condition variable is posted to 
     * wake it up. The application global mutex passed in must be locked by the 
     * calling thread. It will be unlocked while the thread is blocked and 
     * locked when this returns. The same mutex must be held by all waiters and 
     * posters of the same condition variable. Passing a zero stop time blocks
     * until posted or a signal is received. A non-zero stop time will stop
     * waiting when that time arrives. The current time can be acquired through
     * nvms_utime(). Using an absolute stop time allows multiple waits to be
     * aggregated into one stop time. 
     * 
     * @param[in] cond
     * Handle for the condition variable to wait on.
     * 
     * @param[in] mutex
     * The mutex to hold while checking the condition and release while 
     * sleeping.
     * 
     * @param[in] stop
     * This is the absolute time to wakeup if not posted. It is in microseconds
     * since the beginning of the epoch (usually 1970). A stop time of zero
     * means there is no timeout.
     */
    void nvms_cond_wait(
        nvms_cond cond,
        nvms_mutex mutex,
        uint64_t stop
        );

    /**
     * This function will wake up one thread waiting on the condition variable 
     * or all threads waiting on the condition variable. If there are no 
     * threads waiting then nothing happens. To avoid starvation, the thread 
     * waiting the longest should be posted first. The caller must hold the 
     * same mutex that the waiters held when calling nvms_cond_wait.
     * 
     * @param[in] cond
     * Handle for the condition variable to wait on.
     * 
     * @param[in] all
     * True to post all threads rather than one
     */
    void nvms_cond_post(
        nvms_cond cond, // condition variable to post
        int all // true to post all threads rather than one
        );

#ifdef	__cplusplus
}
#endif

#endif	/* NVMS_SLEEPING_H */

