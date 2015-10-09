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
      nvm_misc.h - Non-Volatile Memory miscellaneous functions

    DESCRIPTION\n
      This header describes some miscellaneous housekeeping functions for an
      NVM application.

 */



#ifndef NVM_MISC_H
#define	NVM_MISC_H

#include <setjmp.h>
#include <string.h>

#ifndef NVMS_MISC_H
#include "nvms_misc.h"
#endif

#ifdef	__cplusplus
extern "C"
{
#endif

    /* Insert this in code to see if spots in the code are encountered. Also
     * useful for breaking on an event. */
    void report_event(int e);

    /* PUBLIC TYPES AND CONSTANTS */
    /**\brief jmp_buf that includes NVM transaction information.
     * 
     * Long jumps for NVM applications need to abort transactions if they are 
     * going to jump out of a transaction code block. This version of a jump 
     * buffer includes the transaction depth to recognize when there are 
     * transactions that must be aborted before peeling back the stack.  Note 
     * that this is a volatile memory struct not an NVM struct.
     */
    struct nvm_jmp_buf
    {
        /**
         * This is the transaction nesting depth when nvm_setjmp initialized
         * this jump buffer. A level of zero means there is no current 
         * transaction. nvm_longjmp must abort or end transactions until this
         * nesting depth is achieved. Then it can call jmpbuf. 
         */
        int depth; // Transaction nesting depth at nvm_setjmp

        /**
         * This is a standard jmp_buf
         */
        jmp_buf buf; // standard jmp_buf for setjmp/longjmp
    };
    typedef struct nvm_jmp_buf nvm_jmp_buf;

    /* EXPORT FUNCTIONS */

    /**\brief Initialize a thread to use the NVM library.
     * 
     * This initializes a thread to use the NVM library. It must be called 
     * before any other calls.
     */
    void nvm_thread_init();

    /**
     * When a thread is gracefully exiting it needs to call nvm_thread_fini to
     * clean up the data the NVM library is maintaining for it. After this call
     * returns the thread may not make any other calls to the NVM library except
     * nvm_thread_init. If the thread has a current transaction an assert will
     * fire.
     * 
     * This call is only necessary if the application needs to continue
     * execution without this thread.
     */
    void nvm_thread_fini(void);
    
    /**
     * This ensures that all cache lines in the indicated area of NVM will be 
     * scheduled to be made persistent at the next nvm_persist. This is a 
     * no-op if the platform does not need an explicit flush per cache line to 
     * force cached data to NVM.
     * 
     * This is not normally called directly from the application. The 
     * preprocessor adds a flush with every store to an NVM address. An 
     * application only needs to do an explicit flush if it calls a legacy 
     * function to update NVM. The legacy function will not do any flushes 
     * since it is not written to use NVM. The application must do the flushes 
     * when the legacy function returns.
     * 
     * @param[in] ptr
     * The is the first byte of NVM to flush from caches
     * 
     * @param[in] bytes
     * The number of bytes to flush
     */
#ifdef NVM_EXT
    void nvm_flush(
        const void ^ptr,
        size_t bytes
        );
#else
    void nvm_flush(
        const void *ptr,
        size_t bytes
        );
#endif //NVM_EXT
    
    /**
     * This is identical to nvm_flush except that it is slightly faster
     * because it only flushes one cache line.
     * 
     * @param[in] ptr
     * The is an address in the cache line to flush from caches
     */
#ifdef NVM_EXT
    void nvm_flush1(
        const void ^ptr
        );
#else
    void nvm_flush1(
        const void *ptr
        );
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
     * The number of bytes to flush
     */
#ifdef NVM_EXT
    static inline void nvm_flushi(
        const void ^ptr,
        size_t bytes
        )
    { nvms_flushi(ptr, bytes); }
#else
    static inline void nvm_flushi(
        const void *ptr,
        size_t bytes
        )
    { nvms_flushi(ptr, bytes); }
#endif //NVM_EXT
    
    /**
     * This is a barrier that ensures all previous stores to NVM by the calling
     * thread are actually persistent when it returns. On some systems it is
     * necessary to flush each cache line that was modified before executing
     * the barrier. If the system uses Flush On Shutdown (FOS) to force NVM
     * data to be persistent, then this barrier is a no-op. If the system uses
     * Commit On Shutdown (COS) this forces the dirty NVM to the memory
     * controller write queue.
     * 
     * This is not normally called directly from the application. The creation 
     * of each undo record executes a barrier as well as commit or abort of a 
     * transaction. An application may need to do this explicitly if it is 
     * storing data in NVM without using transactions. For example, an 
     * application might copy client data to an NVM buffer then update a 
     * counter to make it visible. A persist barrier is needed after the copy 
     * and before updating the counter. Another persist barrier is needed after 
     * the counter update to make the counter persistent. Note that in this 
     * case a flush is also required if bcopy is used to copy the data. 
     * Presumably the counter update uses a non-transactional store which 
     * automatically includes a flush.
     */
    void nvm_persist();

    /**
     * This combines a flush of a single cache line with a persist barrier. It 
     * is much faster because the flush cache does not get scanned.
     * 
     * @param[in] ptr
     * The is an address in the cache line to flush from caches
     */
#ifdef NVM_EXT
    void nvm_persist1(
        void ^ptr
        );
#else
    void nvm_persist1(
        void *ptr    
        );
#endif //NVM_EXT
    
    extern int nvm_txdepth();

    /**
     * This works just like a standard setjmp except that it preserves the 
     * transaction nesting depth for nvm_longjmp.
     * 
     * @param[out] pbuf
     * Buffer to hold the current context for nmm_longjmp.
     * 
     * @return 
     * Zero if returning from nmv_setjmp and non-zero if returning from 
     * nvm_longjmp.
     */
#define nvm_setjmp(pbuf) (((pbuf)->depth = nvm_txdepth()), setjmp((pbuf)->buf))

    /**
     * This works just like a standard longjmp except that it will first end 
     * NVM transactions as needed to get back to the transaction nesting depth
     * that existed when nvm_set_jmp saved the environment in buf. It then calls
     * the standard longjmp.
     * 
     * @param[in] buf
     * This must contain a context stored by nvm_setjmp in a function that is 
     * still in the call stack.
     * 
     * @param[in] val
     * This is the non-zero value that nvm_setjmp will return
     */
#ifdef NVM_EXT
    void nvm_longjmp(
        nvm_jmp_buf *buf, // jmp_buf equivalent
        int val // setjmp return value
        );
#else
    void nvm_longjmp(
        nvm_jmp_buf *buf, // jmp_buf equivalent
        int val // setjmp return value
        );
#endif //NVM_EXT

    /**
     * This copies data into NVM from either NVM or volatile memory. It works
     * just like the standard C function memcpy. It returns the destination
     * address and ensures the data is persistently saved in NVM.
     * 
     * This may be more efficient at forcing persistence than a loop that does
     * assignment.
     *
     * WARNING! Copying NVM data that contains self-relative pointers will
     * corrupt the pointers. Use the C extensions for persistent struct
     * assignment to correctly copy the pointers. Standard C struct assignment
     * will corrupt the pointers. The pointers can be fixed after copying by
     * copying them one at a time using the inline functions created by
     * NVM_SRP.
     *
     * @param dest NVM address to copy data into
     * @param src  Address of data to copy from, may be NVM or volatile memory
     * @param n    Number of bytes to copy
     * @return Returns the destination pointer passed as the first argument
     */
#ifdef NVM_EXT
    static inline void ^nvm_copy(
        void ^dest,      // destination in NVM
        const void *src, // source in NVM or volatile memory
        size_t n         // number of bytes to copy
    ) { return nvms_copy(dest, src, n); }
#else
    static inline void *nvm_copy(
        void *dest,      // destination in NVM
        const void *src, // source in NVM or volatile memory
        size_t n         // number of bytes to copy
    ) { return nvms_copy(dest, src, n); }
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
    static inline void ^nvm_strcpy(
        void ^dest,      // destination in NVM
        const void *src // source in NVM or volatile memory
    ) { return nvms_strcpy(dest, src); }
    #else
    static inline void *nvm_strcpy(
        void *dest,     // destination in NVM
        const void *src // source in NVM or volatile memory
    ) { return nvms_strcpy(dest, src); }
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
    static inline void ^nvm_strncpy(
        void ^dest,      // destination in NVM
        const void *src, // source in NVM or volatile memory
        size_t n         // number of bytes to copy
    )  { return nvms_strcpy(dest, src); }
    #else
    static inline void *nvm_strncpy(
        void *dest,      // destination in NVM
        const void *src, // source in NVM or volatile memory
        size_t n         // number of bytes to copy
    )  { return nvms_strcpy(dest, src); }
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
    static inline void ^nvm_set(
        void ^dest,  // destination in NVM
        int c,       // value to store in every byte
        size_t n     // number of bytes to set
        ) { return nvms_set(dest, c, n); }
#else
    static inline void *nvm_set(
        void *dest,  // destination in NVM
        int c,       // value to store in every byte
        size_t n     // number of bytes to set
        ) { return nvms_set(dest, c, n); }
#endif //NVM_EXT
    
    /**
     * Compare and swap an 8 byte location in NVM. The return value is the 
     * value of the location before the CAS executed. If it is equal to the old
     * value then the swap succeeded.
     * 
     * A CAS is like a nested transaction that either commits or aborts on its 
     * own. It cannot be part of a larger transaction that updates multiple 
     * locations atomically, and might rollback.
     * 
     * The NVM version of CAS is different than a normal CAS because it first 
     * forces any previous NVM stores to be persistent, and if the swap 
     * happens, it forces the new value to be persistent before returning.
     * 
     * Note that before this returns another thread could see the new value
     * before it is persistent. NVM can become corrupt if the other thread does
     * a store to NVM based on seeing the value set by this thread, unless the
     * other thread ensures the value set by this CAS is persistent. One way
     * of doing that is to CAS it to yet another value by calling this CAS
     * function again.
     * 
     * @param[in] ptr
     * This is the address of the NVM location to CAS
     * 
     * @param[in] oldval
     * This is the expected current value to be replaced.
     * 
     * @param[in] newval
     * This is the value to store if the compare succeeds
     * 
     * @return
     * The value in the location when CAS was executed. 
     */
    uint64_t nvm_cas8(
#//    volatile uint64_t ^ptr,   
        volatile uint64_t *ptr, //# 
        uint64_t oldval,
        uint64_t newval
        );

    /**
     * Compare and swap a 4 byte location in NVM. The return value is the 
     * value of the location before the CAS executed. If it is equal to the old
     * value then the swap succeeded.
     * 
     * A CAS is like a nested transaction that either commits or aborts on its 
     * own. It cannot be part of a larger transaction that updates multiple 
     * locations atomically, and might rollback.
     * 
     * The NVM version of CAS is different than a normal CAS because it first 
     * forces any previous NVM stores to be persistent, and if the swap 
     * happens, it forces the new value to be persistent before returning. 
     * 
     * Note that before this returns another thread could see the new value
     * before it is persistent. NVM can become corrupt if the other thread does
     * a store to NVM based on seeing the value set by this thread, unless the
     * other thread ensures the value set by this CAS is persistent. One way
     * of doing that is to CAS it to yet another value by calling this CAS
     * function again.
     * 
     * @param[in] ptr
     * This is the address of the NVM location to CAS
     * 
     * @param[in] oldval
     * This is the expected current value to be replaced.
     * 
     * @param[in] newval
     * This is the value to store if the compare succeeds
     * 
     * @return
     * The value in the location when CAS was executed. 
     */
    uint32_t nvm_cas4(
#//    volatile uint32_t ^ptr,   
        volatile uint32_t *ptr, //# 
        uint32_t oldval,
        uint32_t newval
        );

    /**
     * Compare and swap a 2 byte location in NVM. The return value is the 
     * value of the location before the CAS executed. If it is equal to the old
     * value then the swap succeeded.
     * 
     * A CAS is like a nested transaction that either commits or aborts on its 
     * own. It cannot be part of a larger transaction that updates multiple 
     * locations atomically, and might rollback.
     * 
     * The NVM version of CAS is different than a normal CAS because it first 
     * forces any previous NVM stores to be persistent, and if the swap 
     * happens, it forces the new value to be persistent before returning. 
     * 
     * Note that before this returns another thread could see the new value
     * before it is persistent. NVM can become corrupt if the other thread does
     * a store to NVM based on seeing the value set by this thread, unless the
     * other thread ensures the value set by this CAS is persistent. One way
     * of doing that is to CAS it to yet another value by calling this CAS
     * function again.
     * 
     * @param[in] ptr
     * This is the address of the NVM location to CAS
     * 
     * @param[in] oldval
     * This is the expected current value to be replaced.
     * 
     * @param[in] newval
     * This is the value to store if the compare succeeds
     * 
     * @return
     * The value in the location when CAS was executed. 
     */
    uint16_t nvm_cas2(
#//    volatile uint16_t ^ptr,  
        volatile uint16_t *ptr, //# 
        uint16_t oldval,
        uint16_t newval
        );

#ifdef NVM_EXT
    /* NVM store macros not needed with compiler extensions. */
#else
    extern void nvm_undo( const void *data, size_t bytes);
    extern void nvm_flush(const void *ptr, size_t bytes);
    extern void nvm_flush1(const void *ptr);

    /**
     * This macro does a non-transactional store to NVM. It includes code to
     * force the stored data to NVM on the next nvm_persist.
     *
     * This is mostly used for storing into a struct that was allocated in the
     * current transaction. No undo is needed because the allocation will be
     * undone if the transaction does not commit.
     *
     * Note that unlike the assignment operator this is not an expression, and
     * the lvalue is evaluated more than once so it must not have any side
     * effects. For example it must not be ptr++.
     */
#define NVM_NTSTORE(lvalue, rvalue) do { \
    (lvalue) = (rvalue); \
    if (sizeof(lvalue) <= 8) \
        nvm_flush1(&(lvalue)); \
    else \
        nvm_flush(&(lvalue), sizeof(lvalue)); \
    } while (0)

    /**
     * This macro does a transactional store to NVM. It includes code to
     * generate undo and force the stored data to NVM on the next nvm_persist.
     *
     * This ensures the store will be undone if the transaction does not commit.
     *
     * Note that unlike the assignment operator this is not an expression, and
     * the lvalue is evaluated more than once so it must not have any side
     * effects. For example it must not be ptr++.
     */
#define NVM_TXSTORE(lvalue, rvalue) do { \
    nvm_undo(&(lvalue), sizeof(lvalue)); \
    (lvalue) = (rvalue); \
    if (sizeof(lvalue) <= 8) \
        nvm_flush1(&(lvalue)); \
    else \
        nvm_flush(&(lvalue), sizeof(lvalue)); \
    } while (0)

#endif //NVM_EXT


#ifdef NVM_EXT
    /* Self relative pointer functions not needed with compiler extensions. */
#else

    /**
     * This macro provides for typed self-relative pointers. It creates a type
     * for the pointer, an inline function to set an absolute pointer into a
     * self-relative pointer of the correct type, and an inline function to
     * get the absolute pointer from a self-relative one.
     *
     * The null pointer is represented as 1 so that a self relative
     * pointer can point at itself.
     *
     * The type argument must be a single identifier giving the type to be
     * pointed at by the pointer. If the type needs to be multiple identifiers
     * then use NVM_SRP2.
     *
     * For example if you have a type mystruct then NVM_SRP(mystruct) defines:
     *
     * typedef int64_t mystruct_srp;
     * void mystruct_set(mystruct_srp *srp, mystruct *prt);
     * void mystruct_txset(mystruct_srp *srp, mystruct *prt);
     * void mystruct_ntset(mystruct_srp *srp, mystruct *prt);
     * mystruct *mystruct_get(mystruct_srp *srp);
     */
#define NVM_SRP(type) \
    \
    /*
     * An NVM region is not necessarily always mapped in at the same address.
     * Thus pointers within a region are self-relative. This type represents
     * a self-relative pointer to the desired type.
     */\
    typedef int64_t type##_srp; \
    \
    /*
     * Store a virtual address into a typed self relative pointer. The null
     * pointer is represented as 1 so that a self relative pointer can point
     * at itself.
     *
     * The store is not flushed, and no undo is generated. There must be
     * code that does the flush after this executes.
     *
     * @param[in] srp  pointer to a self - relative pointer
     * @param[in] ptr address to store as a self relative pointer
     */ \
    static inline void type##_set(type##_srp *srp, type *ptr) \
    { *srp = ptr ? (int64_t)ptr - (int64_t)srp : 1; } \
    \
    /*
     * Store a virtual address into a typed self relative pointer. The null
     * pointer is represented as 1 so that a self relative pointer can point
     * at itself.
     *
     * The store is transactional so that it will be undone if the transaction
     * does not commit.
     *
     * @param[in] srp  pointer to a self - relative pointer
     * @param[in] ptr address to store as a self relative pointer
     */ \
    static inline void type##_txset(type##_srp *srp, type *ptr) \
    { NVM_TXSTORE(*srp, ptr ? (int64_t)ptr - (int64_t)srp : 1); } \
    \
    /*
     * Store a virtual address into a typed self relative pointer. The null
     * pointer is represented as 1 so that a self relative pointer can point
     * at itself.
     *
     * The store is non-transactional so no undo will be generated.
     *
     * @param[in] srp  pointer to a self - relative pointer
     * @param[in] ptr address to store as a self relative pointer
     */ \
    static inline void type##_ntset(type##_srp *srp, type *ptr) \
    { NVM_NTSTORE(*srp, ptr ? (int64_t)ptr - (int64_t)srp : 1); } \
    \
    /**
     * Fetch the virtual address out of a typed self relative pointer.
     * @param[in] srp pointer to a self - relative pointer
     * @return The virtual address that was stored
     */\
    static inline type *type##_get(type##_srp *srp) \
    { return *srp != 1 ? (void*)((int64_t)srp + *srp) : 0; }
#/* END OF NVM_SRP MACRO */


    /**
     * This macro is the same as NVM_SRP except that the type may be multiple
     * identifiers and a single identifier must be given for constructing the
     * names of the pointer type, the set functions, and the get function.
     *
     * For example NVM_SRP2(struct mine, mystruct) defines:
     *
     * typedef int64_t mystruct_srp;
     * void mystruct_set(mystruct_srp *srp, struct mine *prt);
     * void mystruct_txset(mystruct_srp *srp, struct mine *prt);
     * void mystruct_ntset(mystruct_srp *srp, struct mine *prt);
     * struct mine *mystruct_get(mystruct_srp *srp);
     */
#define NVM_SRP2(type, name) \
    typedef int64_t name##_srp; \
    \
    static inline void name##_set(name##_srp *srp, type *ptr) \
    { *srp = ptr ? (int64_t)ptr - (int64_t)srp : 1; } \
    \
    static inline void name##_txset(name##_srp *srp, type *ptr) \
    { NVM_TXSTORE(*srp, ptr ? (int64_t)ptr - (int64_t)srp : 1); } \
    \
    static inline void name##_ntset(name##_srp *srp, type *ptr) \
    { NVM_NTSTORE(*srp, ptr ? (int64_t)ptr - (int64_t)srp : 1); } \
    \
    static inline type *name##_get(name##_srp *srp) \
    { return *srp != 1 ? (void*)((int64_t)srp + *srp) : 0; }
#/* END OF NVM_SRP2 MACRO */


    /* define the void self-relative pointer */
    NVM_SRP(void)
#endif //NVM_EXT

#ifdef	__cplusplus
}
#endif

#endif	/* NVM_MISC_H */

