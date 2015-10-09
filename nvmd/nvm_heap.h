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
      nvm_heap.h - Non-Volatile Memory heap management

    DESCRIPTION\n
      This header file defines the application interface for managing NVM
      heaps and allocating/freeing NVM managed by the heap.

    NOTES\n
      When an NVM region is created the base extent is formatted as an NVM 
      heap. The root struct is allocated from the heap in the base extent. 
      Additional application data is allocated as needed. Additional heaps can 
      be created and deleted as other region extents. Each heap can be given a
      name for administrative purposes.

 */

#ifndef NVM_HEAP_H
#define	NVM_HEAP_H

#ifdef	__cplusplus
extern "C"
{
#endif

#ifdef NVM_EXT
#else
    NVM_SRP(nvm_heap); // self relative pointer to heap
#endif //NVM_EXT
    
    
    /**\brief Status of one heap.
     * 
     * This struct describes the current state of a heap. 
     * It is used for returning status to the application. 
     */
#ifdef NVM_EXT
    struct nvm_heap_stat
    {
        /**
         * This is the name of the heap given when it was created. This is
         * used in reports, but is otherwise ignored.
         */
        const char *name;

        /**
         * This is the descriptor for the NVM region containing the heap
         */
        nvm_desc region;

        /**
         * This is the address of the containing extent.
         */
        void ^extent;

        /**
         * This is the optional heap local root object pointer
         */
        void ^rootobject;

        /**
         * This is the size of the extent in bytes
         */
        size_t psize;

        /**
         * This is the amount of free space in the entire extent in bytes. 
         */
        size_t free;

        /**
         * This is the amount of space  currently consumed and
         * thus not available for allocation.
         */
        size_t consumed;


        /**
         * This is zero when the heap is generally available for allocation.
         * It is -1 if the heap has an on commit operation scheduled to
         * delete the heap. If the heap has been created, but the creation
         * has not yet committed then this is the transaction slot number
         * of the creating transaction. Only the creating transaction can
         * allocate or free NVM from this heap.
         */
        int32_t inuse;
    };
#else
    struct nvm_heap_stat
    {
        /**
         * This is the name of the heap given when it was created. This is
         * used in reports, but is otherwise ignored.
         */
        const char *name;

        /**
         * This is the descriptor for the NVM region containing the heap
         */
        nvm_desc region;

        /**
         * This is the address of the containing extent.
         */
        void *extent;

        /**
         * This is the optional heap local root object pointer
         */
        void *rootobject;

        /**
         * This is the size of the extent in bytes
         */
        size_t psize;

        /**
         * This is the amount of free space in the entire extent in bytes. 
         */
        size_t free;

        /**
         * This is the amount of space  currently consumed and
         * thus not available for allocation.
         */
        size_t consumed;

        /**
         * This is zero when the heap is generally available for allocation.
         * It is -1 if the heap has an on commit operation scheduled to
         * delete the heap. If the heap has been created, but the creation
         * has not yet committed then this is the transaction slot number
         * of the creating transaction. Only the creating transaction can
         * allocate or free NVM from this heap.
         */
        int32_t inuse;
    };
#endif //NVM_EXT
    typedef struct nvm_heap_stat nvm_heap_stat;

    /**
     * This function adds a new heap managed extent of NVM to a region that is
     * currently attached. It formats the extent as an NVM heap. The
     * return value is a pointer to the new heap.Note that the heap pointer is
     * the same address as the extent address passed in. The heap pointer
     * can be retrieved later via nvm_query_extents if it is not saved in NVM
     * by the application.
     * 
     * This must be called in a transaction for the region containing the heap.
     * If the transaction aborts or rolls back through this call, then the
     * heap and its extent will be deleted. The transaction that creates this
     * heap may allocate and free from the heap even before the heap creation
     * itself commits. However other transactions may not use the heap until
     * the creating transaction successfully commits.
     *
     * The address range for the new extent must not overlap any existing 
     * extent. It must be within the NVM virtual address space reserved for the 
     * region. It must be page aligned and the size must be a multiple of a 
     * page size.
     * 
     * If there are any errors then errno is set and the return value is zero.
     * 
     * @param[in] region
     * This is the region descriptor for the region to create the heap in. It
     * was returned by either nvm_create_region or nvm_attach_region.
     * 
     * @param[in] extent
     * This is the desired virtual address for the new extent.
     * 
     * @param[in] psize
     * This is the amount of physical NVM to allocate for the extent in bytes.
     * 
     * @param[in] name
     * An administrative name for the heap. It is used for reporting. It may not
     * be more than 63 characters long.
     * 
     * @return 
     * A pointer to the new heap is returned. Zero is returned if there
     * is an error.
     * 
     * @par Errors:
     */
#ifdef NVM_EXT
    nvm_heap ^nvm_create_heap@(
        nvm_desc region,
        void ^extent,   
        size_t psize,
        const char *name
       );
#else
    nvm_heap *nvm_create_heap(   
        nvm_desc region,
        void *extent,    
        size_t psize,
        const char *name
       );
#endif //NVM_EXT
    
    /**
     * This function changes the size of an existing heap managed extent. If
     * the new size is larger, then the heap in the extent will have more
     * memory available for allocation. If the new size is smaller, then there
     * must not be any allocations in the area to be removed. The root heap may
     * be resized.
     * 
     * This may be called in a transaction for the same region or outside a
     * transaction. In either case a successful heap resize is persistently
     * committed, even if called in a transaction that later aborts. Other
     * threads could allocate the new space even before nvm_ resize _heap
     * returns. Thus the library cannot generate undo to rollback the resize
     * if the callers transaction aborts.
     * 
     * An error will be returned if the extent cannot be resized. The address
     * range for the extent must not overlap any existing extent. It must be
     * within the NVM virtual address space reserved for the region. The new
     * size must be a multiple of a page size and not zero.
     * 
     * If there are any errors then errno is set and the return value is zero.
     * 
     * @param heap
     * Heap pointer returned from nvm_create_heap
     * 
     * @param psize
     * New physical size of heap extent in bytes
     * 
     * @return 
     * 0 is returned on error and 1 is returned on success.
     */
#ifdef NVM_EXT
    int nvm_resize_heap( 
        nvm_heap ^heap, // Heap pointer returned from nvm_create_heap 
        size_t psize // New physical size of heap extent in bytes
        );
#else
    int nvm_resize_heap(
        nvm_heap *heap, // Heap pointer returned from nvm_create_heap 
        size_t psize // New physical size of heap extent in bytes
        );
#endif //NVM_EXT
    
    /**
     * Delete a heap and remove its extent returning its space to the
     * file system. The contents of the extent are ignored, except the nvm_heap
     * itself, and the extent is simply removed from the region. Thus a corrupt
     * heap can be deleted without encountering any corruption. The caller must
     * ensure all pointers to the heap or to any objects in the heap are
     * removed before committing.
     * 
     * This must be called in a transaction for the region containing the heap.
     * If the transaction aborts or rolls back through this call, then the
     * heap and its extent will be not be deleted. However any attempt to
     * allocated, or free from the heap will fail until the transaction
     * commits removing the heap extent. If the transaction rolls back through
     * this deletion, then the heap will return to normal operation.
     * 
     * If there are any errors then errno is set and the return value is zero.
     * 
     * @param[in] heap
     * This points to the heap to delete
     * 
     * @param[out] ret
     * This points to a pointer to clear atomically with the delete of the heap
     * 
     * @return 
     * 0 is returned on error and 1 is returned on success.
     * 
     * @par Errors:
     */
#ifdef NVM_EXT
    int nvm_delete_heap@(
        nvm_heap ^heap
        );
#else
    int nvm_delete_heap(   
        nvm_heap *heap
        );
#endif //NVM_EXT

    /**
     * Every heap extent can have a root object. This routine will store a
     * pointer to a root object that will be saved in the heap metadata. It
     * is returned as part of the heap status from nvm_heap_query. The root
     * object pointer is null when a heap is first created.
     *
     * This must be called in a transaction so that it will be undone if the
     * calling transaction aborts. The caller is responsible for ensuring only
     * one thread at a time sets a root object pointer.
     *
     * Having a root object is useful for creating a heap that is not pointed
     * to by any other heap. The heap and its contents can be found through
     * nvm_query_extents, then the root object in the heap can be found to
     * find all other heap contents. If there is a corruption in the heap, then
     * the heap can be deleted removing it from the region without having to
     * update any other data structures.
     *
     * An assert will fire if the root object is not in the heap where the
     * pointer is stored.
     *
     * @param heap
     * Heap to change root object
     *
     * @param root
     * Address of the new root object
     */
#ifdef NVM_EXT
    void nvm_heap_setroot@(nvm_heap ^heap, void ^root);
#else
    void nvm_heap_setroot(nvm_heap *heap, void *root);
#endif //NVM_EXT

    /**
     * This returns the status of a heap. Note that the data returned might not
     * be current if some other thread has used the heap.
     * 
     * By querying all extents all heaps can be found and queried.
     * 
     * If there are any errors then errno is set and the return value is zero.
     * 
     * @param heap 
     * Heap to query
     * 
     * @param stat 
     * Stat buffer for return data
     * 
     * @return 
     * 0 is returned on error and 1 is returned on success.
     */
#ifdef NVM_EXT
    int nvm_query_heap(
        nvm_heap ^heap, 
        nvm_heap_stat *stat
        );
#else
    int nvm_query_heap(
        nvm_heap *heap,
        nvm_heap_stat *stat
        );
#endif //NVM_EXT
    
    /**
     * This allocates one or more instances of the persistent struct described 
     * by the nvm_type, from the nvm_heap passed in. The struct must have a 
     * USID defined. This must be called in a transaction that stores the 
     * returned pointer in an NVM location that is reachable from the root 
     * struct. 
     * 
     * The count is the number of instances of a struct to allocate in an 
     * array. This must be at least one. A persistent struct is extensible if 
     * its last field is a zero length array. If the persistent struct defined 
     * by the nmv_type is extensible, then the count describes the actual size 
     * of the empty array at the end of the struct. This allocates one instance
     * of the persistent struct defined by *def with a non-zero array size for 
     * the last field in the struct. The allocation will be for 
     * (def->size+(def->xsize*count)) bytes. If the persistent struct defined 
     * by *def is not extensible, then the count argument is the array size of 
     * structs defined by *def. The allocation will be for (def->size*count) 
     * bytes.
     * 
     * The space returned is cleared to zero except that all USID fields are 
     * set to the correct values and self-relative pointers are set to NULL 
     * which actually is a one. This includes the USID for the struct being 
     * allocated as well as any embedded structs. An embedded union is given 
     * the USID of its first member.
     * 
     * If the transaction rolls back through this allocation, the allocated 
     * space will be released back to the free list. Thus it is not necessary 
     * to create undo for any stores to the new space until the transaction 
     * commits.
     * 
     * If there are any errors then errno is set and the return value is zero.
     * 
     * @param[in] heap
     * This is the heap to allocate from
     * 
     * @param[in] def
     * This points to the nvm_type of the persistent struct to allocate. It must
     * have a USID defined for it.
     * 
     * @param[in] count
     * This is the size of the array to allocate. For an extensible struct this
     * is the array size of the last field. For a non-extensible struct this is
     * the number of structs to allocate. One is passed if this is not
     * an array allocation.
     * 
     * @return 
     * A pointer to the newly allocated space unless there is an error. A zero
     * is returned when there is an error
     * 
     * @par Errors:
     */
#ifdef NVM_EXT
    void ^nvm_alloc@( 
        nvm_heap ^heap, 
        const nvm_type *def, // type definition of struct to allocate
        unsigned count // size of the array
        );
#else
    void *nvm_alloc(   
        nvm_heap *heap,    
        const nvm_type *def, // type definition of struct to allocate
        unsigned count // size of the array
        );
#endif //NVM_EXT
    
    /**
     * This is a macro that calls nvm_alloc based on the struct type. It casts 
     * the return value to the correct type and finds the nvm_type address 
     * based on the struct type.
     */
#ifdef NVM_EXT
#define NVM_ALLOC(heap, type, count) \
        ((type^)nvm_alloc((heap), shapeof(type), (count)))
#else
#define NVM_ALLOC(heap, type, count) \
    ((type*)nvm_alloc((heap), shapeof(type), (count)))
#endif //NVM_EXT
    
    /**
     * This frees memory that was allocated by nvm_alloc. This must be called 
     * in a transaction. The space is not available for other allocations until 
     * the transaction successfully commits. Releasing the space and clearing 
     * the memory is done as an oncommit operation in the current transaction. 
     * The caller is obligated to clear any pointers to the allocated space.
     * 
     * If there are any errors then errno is set and the return value is zero.
     * 
     * @param[in] ptr
     * This is a pointer that was returned by nvm_alloc, and has not been
     * freed since.
     * 
     * @return 
     * 0 is returned on error and 1 is returned on success.
     * 
     * @par Errors:
     */
#ifdef NVM_EXT
    int nvm_free@( 
        void ^ptr 
        );
#else
    int nvm_free(   
        void *ptr    
        );
#endif //NVM_EXT
    
    /**
     * This initializes one or more instances of the persistent struct described
     * by the nvm_type and located at addr. If the struct is extensible (last
     * member is a zero length array), then count is the actual size of the
     * array. If the struct is not extensible, then count is the number of
     * instances of the struct in the array starting at addr.
     * 
     * The memory is cleared just as if it had been allocated from an NVM heap.
     * All numeric fields and transient fields are set to zero. All self-
     * relative pointers are set to the null pointer value which is 1. If the
     * struct or any embedded structs have a USID defined for them, then the
     * correct USID is stored. If there is a union then it is initialized as
     * if it was the first persistent struct in the union.
     * 
     * This may be called in a transaction or not in a transaction. Since it
     * does not require a transaction it does not generate undo. Usually this
     * is called on freshly allocated space that will be freed if the current
     * transaction does not commit.
     * 
     * @param addr
     * persistent memory location to initialize
     * 
     * @param def
     * type definition of struct to initialize
     * 
     * @param count
     * size of the array
     * 
     * @return The number of bytes initialized
     */
#ifdef NVM_EXT
    size_t nvm_init_struct(
        void ^addr,
        const nvm_type *def, // type definition of struct to initialize
        int count // size of the array
        );
#else
    size_t nvm_init_struct(
        void *addr,
        const nvm_type *def, // type definition of struct to initialize
        int count // size of the array
        );
#endif //NVM_EXT
    

#ifdef	__cplusplus
}
#endif

#endif	/* NVM_HEAP_H */

