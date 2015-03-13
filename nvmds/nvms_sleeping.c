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
      nvms_sleeping.c - NVM services for sleeping and waking up threads.

   DESCRIPTION\n
      The NVM library needs a mechanism similar to pthread condition variables 
      for blocking a thread when it needs to sleep until another thread releases
      a resource. Condition variables must be allocated in application global 
      memory so that they are available to all threads in an application.

 */
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include "nvms_locking.h"
#include "nvms_sleeping.h"
#include "nvms_misc.h"
/**
 * This function allocates and initializes one condition variable in 
 * application global memory and returns a handle to it.
 * 
 * If there are any errors then errno is set and the return value is zero.
 * 
 * @return
 * An opaque handle to the condition variable, or zero if there is an error.
 */
nvms_cond nvms_create_cond(void)
{
    pthread_cond_t *cv = malloc(sizeof(pthread_cond_t));
    int ret = pthread_cond_init(cv, NULL);
    if (ret != 0)
    {
        errno = ret;
        return NULL;
    }
    return cv;
}
/**
 * This destroys one condition variable making its handle no longer usable 
 * and releasing any memory associated with it.  There must not be any 
 * threads waiting on the condition variable when it is destroyed.
 * 
 * @param[in] cond
 * Handle for the condition variable to destroy
 */
void nvms_destroy_cond(nvms_cond cond)
{
    pthread_cond_t *cv = cond;
    int ret = pthread_cond_destroy(cv);
    if (ret != 0)
        nvms_assert_fail("Invalid nvms_cond on destroy");
    free(cv);
}
/**
 * The calling thread will block until the condition variable is posted to 
 * wake it up. The application global mutex passed in must be locked by the 
 * calling thread. It will be unlocked while the thread is blocked and 
 * locked when this returns. The same mutex must be held by all waiters and 
 * posters of the same condition variable. Passing a zero stop time blocks
 * until posted or a signal is received. 
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
        )
{
    pthread_cond_t *cv = cond;

    if (stop == 0)
    {
        /* infinite timeout */
        int ret = pthread_cond_wait(cv, (pthread_mutex_t*)mutex);
        if (ret != 0)
            nvms_assert_fail("Invalid nvms_cond or nvms_mutex on wait");
    }
    else
    {
        struct timespec when;
        when.tv_sec = stop / 1000000;
        when.tv_nsec = (stop % 1000000)*1000;
        int ret = pthread_cond_timedwait(cv, (pthread_mutex_t*)mutex, &when);
        if (ret && ret != ETIMEDOUT)
            nvms_assert_fail("Invalid nvms_cond or nvms_mutex on timed wait");
        errno = ret;
    }
}
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
        )
{
    pthread_cond_t *cv = cond;
    if (all)
        pthread_cond_broadcast(cv);
    else
        pthread_cond_signal(cv);
}
