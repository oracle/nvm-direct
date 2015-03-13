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
      nvms_memory.h - NVM services for volatile memory management

    DESCRIPTION\n
      This defines the interface required by the NVM library for allocating
      and freeing volatile memory.

    NOTES\n
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

 */

#ifndef NVMS_MEMORY_H
#define	NVMS_MEMORY_H
#include <stddef.h>
#include <stdint.h>

#ifdef	__cplusplus
extern "C"
{
#endif
    /**
     * This is called once before any other calls to the service library. It
     * provides the library a means of doing any one time initialization such
     * as creating a pthread key. It is called from nvm_init when the library
     * is loaded.
     */
    void nvms_init();

    /**
     * This is called at the beginning of thread initialization of the NVM
     * library. It provides the service layer an opportunity to do per thread
     * initialization.
     */
    void nvms_thread_init();

    /**
     * This is called once after all other calls to the service library. It
     * provides the library a means of doing any cleanup such as deleting  a
     * pthread key. This does not terminate the thread. It could be followed by
     * another call to nvms_thread_init() in the same thread.
     */
    void nvms_thread_fini();

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
        );

    /**
     * This returns the root pointer for this thread. If nvms_thread_set_ptr 
     * was never called in this thread, then zero is returned.
     * 
     * @return 
     * The root pointer or zero if there is none.
     */
    void *nvms_thread_get_ptr();

    /**
     * This allocates size bytes of uninitialized memory that is visible to the
     * calling thread.

     * If there are any errors then errno is set and the return value is zero.

     * @param[in] size
     * amount of memory to allocate in bytes
     * 
     * @return 
     * A pointer to the newly allocated memory or zero
     */
    void *nvms_thread_alloc(
        size_t size
        );

    /**
     * This releases memory that was previously allocated to the calling thread.
     * 
     * @param[in] ptr
     * pointer previously returned by nvms_thread_alloc
     */
    void nvms_thread_free(
        void *ptr
        );

    /**
     * This returns the address of the application root pointer for the current
     * application. The application root pointer itself must be zero when the 
     * application is started. It is in application global memory so that it is
     * automatically released when all threads in an application exit. 
     * 
     * This routine is used both to get and set the application root pointer. 
     * It is only called once per thread in nvm_thread_init.
     * 
     * @return 
     * Address in application global memory of the application root pointer.
     */
    static inline void **nvms_app_ptr_addr()
    {
        extern void *nvms_app_ptr;
        return &nvms_app_ptr;
    }


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
    void *nvms_app_alloc(size_t size);

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
    int nvms_app_free(void *ptr);


#ifdef	__cplusplus
}
#endif

#endif	/* NVMS_MEMORY_H */

