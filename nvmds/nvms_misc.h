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
      nvms_misc.h - NVM miscellaneous services

    DESCRIPTION\n
      This defines miscellaneous services for managing processor caching and
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

#ifndef NVMS_MISC_H
#define	NVMS_MISC_H

#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>

#ifdef	__cplusplus
extern "C"
{
#endif
    /**
     * This struct contains the configuration parameters that the NVM library
     * needs to appropriately configure its data structures. Generally the
     * service library has a parameter file that the administrator can edit or
     * environment variables that contain the parameter values for an 
     * application. However some parameters are hardware based.
     */
    struct nvms_parameters
    {
        /**
         * This is the size of a cache line in bytes for the current 
         * environment. Commonly it is 64. This is needed for knowing how many
         * stores are covered by the same cache line flush.
         */
        uint16_t cache_line;

        /**
         * This is the desired size of the flush cache used to reduce repeat
         * flushes of the same NVM cache line. It must be a power of 2. If this
         * is zero then this platform does not require flushes.
         */
        uint16_t flush_cache_sz;

        /**
         * This is the maximum number of region files that an application can 
         * simultaneously map into its address space. This is used to allocate
         * a region table at application start.
         */
        uint16_t max_regions;

        /**
         * This is the desired number of entries in the USID hash map. It
         * should be at least twice the number of USID values defined by the
         * application. Each entry costs about 32 bytes of volatile memory.
         */
        uint32_t usid_map_size;

        /**
         * This is the number of wait lists allocated for threads that need to
         * sleep waiting for an NVM mutex to become available. It should be 
         * larger than the number of threads to avoid unnecessary contention.
         * A table of wait lists is allocated for each attached NVM region.
         */
        uint32_t wait_buckets;

        /**
         * This is the target number of wait structs in an NVM wait list. If
         * a wait bucket gets a longer list then idle nvm_wait structs will
         * be removed from the list. A shorter list makes looking for a wait
         * struct faster. However some should be kept for mutexes that are
         * heavily contended and get lots of waiters.
         */
        uint16_t max_nvm_wait;

        /**
         *  All virtual addresses and extent lengths must be a multiple of this
         * size, which is always a power of 2. This is so that the addresses
         * are on an OS page boundary. This is likely to be a large number to 
         * allow the OS to use large pages to reduce TLB misses. */
        uint64_t extent_align;

        /**
         * This is the number of upgrade mutexes that will be created. When
         * a thread encounters a struct that needs to be upgraded, it hashes
         * the address of the struct to pick one of these mutexes to lock.
         * This prevents two simultaneous upgrades of the same struct. If
         * there are too few of these mutexes then there could be false
         * contention when upgrading.
         */
        uint32_t upgrade_mutexes;
    };
    typedef struct nvms_parameters nvms_parameters;

    /**
     * Return a copy of the configuration parameters for this application.
     * 
     * @param[out] params
     * Pointer to parameter struct to fill in.
     */
    void nvms_get_params(nvms_parameters *params);

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
        );

    /**
     * This does not return until all outstanding stores to NVM by the current 
     * thread are truly persistent. Some platforms require nvms_flush to be 
     * called for each cache line before this will wait for it to be flushed.
     */
    void nvms_persist();

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
        );

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
        );
    
    /**
     * This is called to spawn a thread to recover a transaction that was not
     * idle at the last detach of this region.
     * 
     * The new thread must call nvm_txrecover passing the ctx to it.
     * 
     * If this service library only supports one single thread of execution
     * in one process, then nvm_txrecover can simply be called from nvms_spawn.
     * However this will only work if the region is only accessed by
     * single threaded applications. If region attach discovers two dead
     * transactions, the first one to recover might hang.
     * 
     * @param[in] ctx
     * The ctx argument to pass on to nvm_txrecover in the new thread.
     */
    void nvms_spawn_txrecover(void *ctx);
    
    /**
     * Return the time in micro seconds since the beginning of the epoch
     * @return 
     * Microseconds since 1970.
     */
    static inline uint64_t
    nvms_utime(void)
    {
        struct timespec tv;
        clock_gettime(CLOCK_REALTIME, &tv);
        return((uint64_t)tv.tv_sec)*1000000 + tv.tv_nsec / 1000;
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
    );
#else
    void *nvms_copy(
        void *dest,      // destination in NVM
        const void *src, // source in NVM or volatile memory
        size_t n         // number of bytes to copy
    );
#endif //NVM_EXT

    /**
     * This copies a string into NVM from either NVM or volatile memory. It
     * works just like the standard C function strcpy. It returns the
     * destination address and ensures the data is persistently saved in NVM.
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
    );
    #else
    void *nvms_strcpy(
        void *dest,     // destination in NVM
        const void *src // source in NVM or volatile memory
    );
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
    );
    #else
    void *nvms_strncpy(
        void *dest,      // destination in NVM
        const void *src, // source in NVM or volatile memory
        size_t n         // number of bytes to copy
    );
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
        );
#else
    void *nvms_set(
        void *dest,  // destination in NVM
        int c,       // value to store in every byte
        size_t n     // number of bytes to set
        );
#endif //NVM_EXT
#ifdef	__cplusplus
}
#endif

#endif	/* NVMS_MISC_H */

