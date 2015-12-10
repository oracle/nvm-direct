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
     nvm_misc.c - Miscellaneous NVM services 

   DESCRIPTION\n
      This implements miscellaneous services for managing processor caching and
      dealing with errors.

      Processor caches can hold a dirty version of an NVM cache line. To make 
      the contents of NVM consistent it is necessary to flush the caches to NVM.
      With flush on shutdown nothing is flushed until the system is shutting 
      down. However flush on barrier requires the software to have barriers 
      that ensure previous stores to NVM are persistent. The implementation of 
      barriers is different from one platform to another. Thus there needs to 
      be service functions to implement barriers. 

      There are some error situations that cannot be handled by returning 
      failure and setting errno. These are reported to the service library 
      which exits the process so that recovery can be run.

 */
#include "nvms_misc.h"
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

/**
 * Return a copy of the configuration parameters for this application.
 * 
 * @param[out] params
 * Pointer to parameter struct to fill in.
 */
void nvms_get_params(nvms_parameters *params)
{
    params->cache_line = 64;
    params->flush_cache_sz = 8;
    params->max_regions = 64;
    params->usid_map_size = 1024;
    params->wait_buckets = 1024;
    params->max_nvm_wait = 4;
    params->extent_align = 4 * 1024;
    params->upgrade_mutexes = 1024;
}
/**
 * This starts one cache line on its way from a processor cache to its NVM 
 * DIMM. It is possible that there is nothing to be done. The ptr argument 
 * will be cache line aligned. The cache line may remain in cache after it 
 * is made persistent, but some implementations remove it from the 
 * processor cache.
 * 
 * The pointer is a uint64_t since it is not used as a pointer and it makes
 * the arithmetic easier.
 * 
 * @param[in] ptr
 * Cache line aligned address of NVM to flush.
 */
void nvms_flush(
        uint64_t ptr
        )
{
    return; // need some ASM here
}
/**
 * This does not return until all outstanding stores to NVM by the current 
 * thread are truly persistent. Some platforms require nvms_flush to be 
 * called for each cache line before this will wait for it to be flushed.
 */
void nvms_persist()
{
    return; // need some ASM here
}
/**
 * When the application makes a logical error that cannot be returned as 
 * an error nvms_assert_fail is called to end the thread. A value 
 * indicating the error cause is put in errno, and an error message is 
 * passed in.
 * 
 * A good example of this is beginning a transaction when another 
 * transaction for a different region is still current. Generating too much 
 * undo is another example since it is not reasonable to have every 
 * transactional store check for an error.
 * 
 * @param[in] err
 * An error message describing the problem
 */
void nvms_assert_fail(
        const char *err
        )
{
    fprintf(stderr, "NVMS ASSERT: %s\n", err);
    exit(1);
}
/**
 * When an impossible data structure state is discovered this is called to 
 * report the problem.  The error message describes the problem. Depending 
 * on the problem there may be one or two addresses that help identify 
 * where the corruption is. This exits the process rather than return.
 * 
 * @param[in] err
 * An error message describing the problem
 * 
 * @param[in] ptr1
 * Possibly a relevant address
 * 
 * @param[in] ptr2
 * Possibly a relevant address
 * 
 */
void nvms_corruption(
        const char *err,
        const void *ptr1,    
        const void *ptr2    
        )
{
    fprintf(stderr, "NVMS CORRUPTION: %s, (%p), (%p)\n", err, ptr1, ptr2);
    exit(1);
}

/**
 * Entry point for a recovery thread.
 */
static void *nvms_call_txrecovery(void *ctx)
{
    extern void nvm_txrecover(void *ctx);
    nvm_txrecover(ctx);
    return NULL;
}

/**
 * This is called to spawn a thread to recover a transaction that was not
 * idle at the last detach of this region.
 * 
 * The new thread must call nvm_txrecover passing the ctx to it.
 * 
 * If this service library only supports one single thread of execution
 * in one process, then nvm_txrecover can simply be called from nvms_spawn.
 * The thread private pointer must be saved and restored around the call.
 * However direct call will only work if the region is only accessed by
 * single threaded applications. If region attach discovers two dead
 * transactions, the first one to recover might hang.
 * 
 * @param[in] ctx
 * The ctx argument to pass on to nvm_txrecover in the new thread.
 */
void nvms_spawn_txrecover(void *ctx)
{
    pthread_attr_t      attr;
    pthread_t           thread;

    /* Construct attributes to create a detached thread. */
    int e = pthread_attr_init(&attr);
    if (e) nvms_assert_fail("pthread_attr_init failed");
    e = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (e) nvms_assert_fail("pthread_attr_setdetachstate failed");

    /* spawn the thread. */
    pthread_create(&thread, &attr, &nvms_call_txrecovery, ctx);

    /* destroy the attributes now that the thread is created */
    e = pthread_attr_destroy(&attr);
    if (e) nvms_assert_fail("pthread_attr_destroy failed");
}

/**
 * This copies data into NVM from either NVM or volatile memory. It works
 * just like the standard C function memcpy. It returns the destination
 * address and ensures the data is persistently saved in NVM.
 * 
 * This may be more efficient at forcing persistence than a loop that does
 * assignment.

 * @param dest NVM address to copy data into
 * @param src  Address of data to copy from, may be NVM or volatile memory
 * @param n    Number of bytes to copy
 * @return Returns the destination pointer passed as the first argument
 */
#ifdef NVM_EXT
void ^nvms_copy(
    void ^dest,      // destination in NVM
    const void *src, // source in NVM or volatile memory
    size_t n         // number of bytes to copy
) 
{
    memcpy((void*)dest, src, n);
    nvms_flushi(dest,n);
    return dest;
}
#else
void *nvms_copy(
    void *dest,      // destination in NVM
    const void *src, // source in NVM or volatile memory
    size_t n         // number of bytes to copy
) 
{
    memcpy(dest, src, n);
    nvms_flushi(dest,n);
    return dest;
}
#endif //NVM_EXT

/**
 * This copies a string into NVM from either NVM or volatile memory. It works
 * just like the standard C function strcpy. It returns the destination
 * address and ensures the data is persistently saved in NVM.
 *
 * This may be more efficient at forcing persistence than a loop that does
 * assignment.

 * @param dest NVM address to copy string into
 * @param src  Address of string to copy from, may be NVM or volatile memory
 * @return Returns the destination pointer passed as the first argument
 */
#ifdef NVM_EXT
void ^nvms_strcpy(
    void ^dest,     // destination in NVM
    const void *src // source in NVM or volatile memory
)
{
    size_t n = strlen(src)+1;     // number of bytes that will be copied
    strcpy((void*)dest, src);
    nvms_flushi(dest,n);
    return dest;
}
#else
void *nvms_strcpy(
    void *dest,     // destination in NVM
    const void *src // source in NVM or volatile memory
)
{
    size_t n = strlen(src)+1;     // number of bytes that will be copied
    strcpy(dest, src);
    nvms_flushi(dest,n);
    return dest;
}
#endif //NVM_EXT

/**
 * This copies a string into NVM from either NVM or volatile memory. It works
 * just like the standard C function strncpy. It returns the destination
 * address and ensures the data is persistently saved in NVM.
 *
 * This may be more efficient at forcing persistence than a loop that does
 * assignment.

 * @param dest NVM address to copy string into
 * @param src  Address of string to copy from, may be NVM or volatile memory
 * @param n    Number of bytes to store
 * @return Returns the destination pointer passed as the first argument
 */
#ifdef NVM_EXT
void ^nvms_strncpy(
    void ^dest,      // destination in NVM
    const void *src, // source in NVM or volatile memory
    size_t n         // number of bytes to copy
)
{
    strncpy((void*)dest, src, n);
    nvms_flushi(dest,n);
    return dest;
}
#else
void *nvms_strncpy(
    void *dest,      // destination in NVM
    const void *src, // source in NVM or volatile memory
    size_t n         // number of bytes to copy
)
{
    strncpy(dest, src, n);
    nvms_flushi(dest,n);
    return dest;
}
#endif //NVM_EXT


/**
 * This initializes a range of bytes in NVM and ensures they are
 * persistently stored in NVM. It works just like the standard C function
 * memset. It returns the beginning address of the byte range.
 * 
 * This may be more efficient at forcing persistence than a loop that
 * does assignment.

 * @param dest NVM address to store data into
 * @param c    Value to initialize bytes with
 * @param n    Number of bytes to store
 * @return Returns the destination pointer passed as the first argument
 */
#ifdef NVM_EXT
void ^nvms_set(
    void ^dest,  // destination in NVM
    int c,       // value to store in every byte
    size_t n     // number of bytes to set
)
{
    memset((void*)dest, c, n);
    nvms_flushi(dest,n);
    return dest;
}
#else
void *nvms_set(
    void *dest,  // destination in NVM
    int c,       // value to store in every byte
    size_t n     // number of bytes to set
)
{
    memset(dest, c, n);
    nvms_flushi(dest,n);
    return dest;
}
#endif //NVM_EXT

/**
 * This does an immediate flush of a range of bytes rather than just saving
 * the address for flushing before next persist. This has less overhead
 * than scheduling the flush, but the data is lost from the processor cache.
 *
 * @param[in] ptr
 * The is the first byte of NVM to flush from caches
 *
 * @param[in] bytes
 * The number of bytes to flush. Pass zero to flush just one cache line.
 */
#ifdef NVM_EXT
void nvms_flushi(
        const void ^ptr,
        size_t bytes
        )
{
    /* Optimize case of just one cache line */
    if (bytes == 0)
    {
        nvms_flush((uint64_t)ptr);
        return;
    }

    /* need to get the cache line size to decide how many cache lines need
     * flushing. */
    nvms_parameters nparams;
    nvms_get_params(&nparams);
    uint64_t clsz = nparams.cache_line; // cache line size

    /* Align to a cache line boundary to ensure all cache lines are covered */
    uint64_t addr = (uint64_t)ptr;
    uint64_t off = addr & (clsz - 1);
    addr -= off;
    bytes += off;

    /* loop flushing one cache line each time around. */
    while (1)
    {
        nvms_flush(addr);
        if (bytes <= clsz)
            return;
        addr += clsz;
        bytes -= clsz;
    }
}
#else
void nvms_flushi(
        const void *ptr,
        size_t bytes
        )
{
    /* Optimize case of just one cache line */
    if (bytes == 0)
    {
        nvms_flush((uint64_t)ptr);
        return;
    }

    /* need to get the cache line size to decide how many cache lines need
     * flushing. */
    nvms_parameters nparams;
    nvms_get_params(&nparams);
    uint64_t clsz = nparams.cache_line; // cache line size

    /* Align to a cache line boundary to ensure all cache lines are covered */
    uint64_t addr = (uint64_t)ptr;
    uint64_t off = addr & (clsz - 1);
    addr -= off;
    bytes += off;

    /* loop flushing one cache line each time around. */
    while (1)
    {
        nvms_flush(addr);
        if (bytes <= clsz)
            return;
        addr += clsz;
        bytes -= clsz;
    }
}
#endif //NVM_EXT
