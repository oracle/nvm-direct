
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
      nvms_memory.c - NVM services for volatile memory management

    DESCRIPTION\n
      This implements volatile memory services for the NVM library.

    NOTES|
      Memory management services provide 2 classes of volatile memory based on 
      the visibility to different threads of execution:

      1. Thread local memory is only accessed by a single thread of execution. 
      It may be accessible to other threads, but the NVM library does not do 
      that. If the owning thread exits the memory is automatically released
      by the services layer.

      2. Application global memory can be accessed by any thread via the same
      virtual address.

      For each class of memory, there is an allocate and a free service. Since
      the service library only supports a single process, this could be
      malloc/free for both classes.

      All volatile memory used by the NVM library is dynamically allocated from
      one of the two classes. The library does not declare any global or static
      variables other than those that are const. In order to maintain its 
      state, it needs the services library to maintain root pointers. The NVM 
      library allocates a root struct and stores a pointer to it in a root 
      pointer. There are two kinds of root pointers :

      1. Application root pointer: There is only one application root pointer 
      in an application. Every thread sees the same value. It is in application
      global memory.

      3. Thread root pointer: Each thread has its own private thread root 
      pointer. It is in thread local memory so that it disappears if the thread
      calls nvm_fini.

   EXPORT FUNCTION(S)\n
      nvms_thread_set_ptr - set the thread local root pointer\n
      nvms_thread_get_ptr - get the thread local root pointer\n
      nvms_thread_alloc - allocate thread local memory\n
      nvms_thread_free - free thread local memory\n
      nvms_app_ptr_addr - get the application root pointer address\n
      nvms_app_set_ptr - set the application global root pointer\n
      nvms_app_alloc - allocate application global memory\n
      nvms_app_free - free application global memory\n

   INTERNAL FUNCTION(S)\n

   STATIC FUNCTION(S)\n

   NOTES\n

   MODIFIED    (MM/DD/YY)
     bbridge    05/06/14 - Creation\n
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <string.h>
#include "errno.h"
#include "nvms_misc.h"
#include "nvms_memory.h"

/* The key for getting the per thread pointer */
static pthread_key_t nvms_ptr_key;

/* The key for getting the per thread allocations */
static pthread_key_t nvms_alloc_key;

/* the one and only app pointer */
void *nvms_app_ptr;

/**
 * This is called once before any other calls to the service library. It
 * provides the library a means of doing any one time initialization such
 * as creating a pthread key.
 */
void nvms_init()
{
//    pthread_once(&nvms_proc_once, &nvms_init1);
    pthread_key_create(&nvms_ptr_key, NULL);
    pthread_key_create(&nvms_alloc_key, NULL);
}
/**
 * This is called at the beginning of thread initialization of the NVM
 * library. It provides the service layer an opportunity to do per thread
 * initialization.
 */
void nvms_thread_init() { }

/**
 * This is called once after all other calls to the service library. It
 * provides the library a means of doing any cleanup such as deleting  a
 * pthread key. This does not terminate the thread. It could be followed by
 * another call to nvms_thread_init() in the same thread.
 */
void nvms_thread_fini()
{
    /* Release all the per thread allocations */
    void **cur = pthread_getspecific(nvms_alloc_key);
    void **nxt;
    while (cur)
    {
        nxt = *cur;
//        printf("%p f free++\n", cur);
        free(cur);
        cur = nxt;
        pthread_setspecific(nvms_alloc_key, cur);
    }
}
/**
 * This saves a root pointer for this thread. Subsequent calls made from
 * this thread to get the thread root pointer will return this value. The 
 * root pointer must be a value returned by nvms_thread_alloc.
 * 
 * @param[in] ptr
 * The root pointer to save.
 */
void nvms_thread_set_ptr(
        void *ptr
        )
{
    int ret = pthread_setspecific(nvms_ptr_key, ptr);
    if (ret)
        nvms_assert_fail("Error setting per thread pointer");
}
/**
 * This returns the root pointer for this thread. If nvms_thread_set_ptr 
 * was never called in this thread, then zero is returned.
 * 
 * @return 
 * The root pointer or zero if there is none.
 */
void *nvms_thread_get_ptr()
{
    void *ptr = pthread_getspecific(nvms_ptr_key);
    return ptr;
}
/**
 * This allocates size bytes of zeroed memory that is visible to the
 * calling thread. If the thread exits the memory will be automatically
 * released as if it was passed to nvms_thread_free.
 * 
 * If there are any errors then errno is set and the return value is zero.
 * 
 * @param[in] size
 * amount of memory to allocate in bytes
 * 
 * @return 
 * A pointer to the newly allocated memory or zero
 */
void *nvms_thread_alloc(
        size_t size
        )
{
    /* malloc the memory and put it on the list of chunks allocated to this
     * thread. */
    void **ret = malloc(size + sizeof(void*));
    memset(ret+1, 0, size);
    *ret = pthread_getspecific(nvms_alloc_key);
    pthread_setspecific(nvms_alloc_key, ret);
    return ret + 1;
}
/**
 * This releases memory that was previously allocated to the calling thread.
 * 
 * @param[in] ptr
 * pointer previously returned by nvms_thread_alloc
 */
void nvms_thread_free(
        void *ptr
        )
{
    /* Find the address on the list of allocations to this thread */
    void **prev = NULL;
    void **cur = pthread_getspecific(nvms_alloc_key);
    while ((cur + 1) != ptr)
    {
        prev = cur;
        cur = *cur;
        if (!cur)
            nvms_assert_fail("Freeing thread memory that was not allocated");
    }

    /* remove from allocated list */
    if (prev)
        *prev = *cur;
    else
        pthread_setspecific(nvms_alloc_key, *cur);

//    printf("%p t free++\n", cur);
    free(cur);
}

///**
// * This returns the address of the application root pointer for the current
// * application. The application root pointer itself must be zero when the
// * application is started. It is in application global memory so that it is
// * automatically released when all threads in an application exit.
// *
// * This routine is used both to get and set the application root pointer.
// * It is only called once per thread in nvm_thread_init.
// *
// * @return
// * Address in application global memory of the application root pointer.
// */
//void **nvms_app_ptr_addr()
//{
//    return &nvms_app_ptr;
//}
/**
 * This allocates size bytes of zeroed memory that is visible to the
 * calling application.
 * 
 * If there are any errors then an assert fires.
 * 
 * @param[in] size
 * amount of memory to allocate in bytes
 * 
 * @return 
 * A pointer to the newly allocated memory
 */
void *nvms_app_alloc(size_t size)
{
    void *mem = malloc(size);
//    printf("%p a aloc++\n", mem);
    if (mem == 0)
        nvms_assert_fail("Failure in malloc");
    memset(mem, 0, size);
    return mem;
}
/**
 * This releases memory that was previously allocated to the calling 
 * application.
 * 
 * @param[in] ptr
 * pointer previously returned by nvms_app_alloc
 * 
 * @return 
 * 1 if successfully freed, and 0 if not freed because some lock could not
 * be acquired.
 */
int nvms_app_free(void *ptr)
{
//    printf("%p a free++\n", ptr);
    free(ptr);
    return 1;
}
