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
      nvm_heap0.h - private NVM heap management declarations

    DESCRIPTION\n
      This contains the declarations for NVM heap management that are private
      to the NVM library.

    RELATED DOCUMENTS\n

    EXPORT FUNCTION(S)\n

    INTERNAL FUNCTION(S)\n

    EXAMPLES\n

    NOTES\n

    MODIFIED    (MM/DD/YY)\n
      bbridge    04/21/14 - Creation\n
 */


#ifndef NVM_HEAP0_H
#define	NVM_HEAP0_H

#ifdef	__cplusplus
extern "C"
{
#endif

    /**
     * This is a link in a doubly linked list of nvm_blk structs. It is
     * embedded in other persistent structs.
     */
#ifdef NVM_EXT
    typedef persistent struct nvm_blk nvm_blk;
    persistent struct nvm_link
    tag("Link in doubly linked list of nvm_blk") 
    {
        /** Pointer to the next node in the list */
        nvm_blk ^fwrd;

        /** Pointer to the previous node in the list */
        nvm_blk ^back;
    };
    typedef persistent struct nvm_link nvm_link;
#else
    typedef struct nvm_blk nvm_blk;
    NVM_SRP(nvm_blk);
    struct nvm_link
    {
        /** Pointer to the next node in the list */
        nvm_blk_srp fwrd; // ^nvm_blk    

        /** Pointer to the previous node in the list */
        nvm_blk_srp back; // ^nvm_blk    
    };
    typedef struct nvm_link nvm_link;
    extern const nvm_type nvm_type_nvm_link;
#endif //NVM_EXT

    /**
     * This is a list of doubly linked  nvm_blk structs. It is embedded in
     * other persistent structs. The nvm_blk at the head of the list has a 
     * NULL back pointer, and the tail has a NULL forward pointer.
     */
#ifdef NVM_EXT
    persistent struct nvm_list
    tag("Doubly linked list of nvm_blk")
    {
        /** Pointer to the head of the list */
        nvm_blk ^head;

        /** Pointer to the tail of the list */
        nvm_blk ^tail;
    };
    typedef persistent struct nvm_list nvm_list;
#else
    struct nvm_list
    {
        /** Pointer to the head of the list */
        nvm_blk_srp head; // ^nvm_blk    

        /** Pointer to the tail of the list */
        nvm_blk_srp tail; // ^nvm_blk    
    };
    typedef struct nvm_list nvm_list;
#endif //NVM_EXT
    extern const nvm_type nvm_type_nvm_list; 

    /**\brief Heap block descriptor for one allocated or free block of NVM. 
     *
     * nvm_blk describes one block of memory in an nvm_heap. It is immediately
     * followed by either an allocated block containing data or a completely
     * free block of zeroed NVM. 
     */
#ifdef NVM_EXT
    persistent struct nvm_blk
    USID("4d8a b944 afee 7ddd 06c7 ac66 48d7 e7b3")
    tag("NVM heap control block describing one chunk of NVM")
    version(0)
    alignas(64)
    size(64)
    {
        /**
         * The type usid is first. It is used to verify heap and freelist 
         * linked lists. It can be scanned for in NVM to help reconstuct the 
         * lists if there is corruption.
         */
//        nvm_usid type_usid; //#

        /**
         * All the blocks in a heap managed extent are linked in address 
         * order. The back link points at the descriptor of the block that
         * is just before this block, and the forward pointer points at the
         * descriptor that is just after this block. Thus the block sizes can
         * be calculated from the pointers. The extent ends with a descriptor
         * that has a null forward pointer. Similarly the extent begins with 
         * a descriptor that has a null back pointer. */
        nvm_link neighbors;

        /**
         * Every nvm_blk is always linked on doubly linked list that depends
         * on its allocated status. If it is free then it is linked into its
         * freelist. If it is allocated then is it linked to the list of
         * descriptors in the same nvm_heap. The head of the list has a null
         * back pointer and the tail of the list has a null forward pointer;
         */
        nvm_link group;

        /**
         * This is a pointer to a struct containing the list header for this
         * descriptor's group link. An allocated block points to the nvm_heap
         * it was allocated from. A free block points to the freelist where it
         * can be found. See the allocated field.
         */
        void ^ptr;   // either nvm_heap^ or nvm_freelist^

        /**
         * This is the number of allocated bytes in this block. This may be
         * smaller than the amount of space in the block due to padding or
         * fragmentation. Divide this by the size of the struct allocated to
         * get the array size.
         */
        uint64_t allocated;

    };
#else
    struct nvm_blk
    {
        /**
         * The type usid is first. It is used to verify heap and freelist 
         * linked lists. It can be scanned for in NVM to help reconstuct the 
         * lists if there is corruption.
         */
        nvm_usid type_usid;    

        /**
         * All the blocks in a heap managed extent are linked in address 
         * order. The back link points at the descriptor of the block that
         * is just before this block, and the forward pointer points at the
         * descriptor that is just after this block. Thus the block sizes can
         * be calculated from the pointers. The extent ends with a descriptor
         * that has a null forward pointer. Similarly the extent begins with 
         * a descriptor that has a null back pointer. */
        nvm_link neighbors;

        /**
         * Every nvm_blk is always linked on doubly linked list that depends
         * on its allocated status. If it is free then it is linked into its
         * freelist. If it is allocated then is it linked to the list of
         * descriptors in the same nvm_heap. The head of the list has a null
         * back pointer and the tail of the list has a null forward pointer;
         */
        nvm_link group;

        /**
         * This is a pointer to a struct containing the list header for this
         * descriptor's group link. An allocated block points to the nvm_heap
         * it was allocated from. A free block points to the freelist where it
         * can be found. See the allocated field.
         */
        void_srp ptr; // either nvm_heap* or nvm_freelist* 

        /**
         * This is the number of allocated bytes in this block. This may be
         * smaller than the amount of space in the block due to padding or
         * fragmentation. Divide this by the size of the struct allocated to
         * get the array size.
         */
        uint64_t allocated;

    };
#endif //NVM_EXT
    extern const nvm_type nvm_type_nvm_blk; //#

    /**
     * There are a number of free lists for various block sizes. Each list
     * only contains blocks with usable space within one power of 2. Except
     * for the last list which has the huge blocks of any size.
     * 
     * This constant defines the number of free lists.
     */
#define FREELIST_CNT (25)

    /**
     * A freelist is just a linked list header
     */
    typedef nvm_list nvm_freelist;

    /**
     * An nvm_heap is used for allocating persistent structs. The root heap is 
     * created when the NVM region is created. When an extent of NVM is added
     * to a region it may be formatted as a heap containing all the NVM in the
     * extent. This struct starts at the first byte of a heap managed extent.
     * The managed space begins after this struct.
     * 
     * Multiple heaps are useful for limiting resource consumption and for 
     * grouping multiple allocations for simultaneous deallocation.
     */
#ifdef NVM_EXT
    persistent struct nvm_heap
    USID("f728 4991 8dcf 67f2 c6d4 8417 fe4a bdf6")
    tag("Header of a heap managed extent containing heap metadata")
    version(0)
    size(1024)
    {
        /**
         * This is the ASCII name of the heap. Note that this imposes a 63 
         * byte limit on the name of a heap so that this is a null
         * terminated string.
         */
        char name[64];

        /**
         * This is a pointer back to the nvm_region containing this NVM heap.
         * It allows quickly finding the nvm_region from any nvm struct by 
         * following the heap pointer in the nvm_blk just before the struct.
         */
        nvm_region ^region;
        
        /**
         * This is the address of the extent containing this heap. In the
         * current implementation it is either this nvm_heap or the nvm_region.
         */
        void ^extent;

        /**
         * This is the optional heap local root object pointer
         */
        void ^rootobject;

        /**
         * This is the current size of the extent in bytes including the
         * nvm_heap at the beginning.
         */
        size_t psize;

        /**
         * This points to the first nvm_blk of the NVM managed by this nvm_heap.
         * It follows immediately after the nvm_heap.
         */
        nvm_blk ^first;

        /**
         * This is the size of the managed NVM in bytes. This will be the same
         * as psize except for the root heap.
         */
        size_t size;

        /**
         * This is the number of bytes currently allocated by this heap. This
         * includes the space consumed by the NVblk headers.
         */
        uint64_t consumed;

        /**
         * This is the NVM mutex that protects the contents of this NVM heap. It
         * must be locked exclusive by any transaction that updates the heap.
         */
        nvm_mutex heap_mutex;
        
        /**
         * This is the mutex level for the mutex in a heap.
         */
#define NVM_HEAP_MUTEX_LEVEL ((uint8_t)240)

        /**
         * All blocks allocated to this heap are kept in a doubly linked list.
         * These are the head and tail of that list.
         */
        nvm_list list;

        /**
         * There are a number of free lists for various block sizes. Each 
         * list only contains blocks with usable space within one power of 2, 
         * except for the last list which has the huge blocks of any size. 
         * This array defines the max usable block size for each free list.
         */
        size_t free_size[FREELIST_CNT];

        /**
         * This is the array of free lists. Each free list is a doubly linked 
         * list of nvm_blk structs that are all free.
         */
        nvm_freelist free[FREELIST_CNT];

        /**
         * While non-transactional allocNT is still enabled, this points to
         * the current free nvm_blk where allocation will be done. It is null
         * if non-transactional allocation is finished.
         */
        nvm_blk ^nvb_free;
        
        /**
         * This is zero when the heap is generally available for allocation.
         * It is -1 if the heap has an on commit operation scheduled to
         * delete the heap. If the heap has been created, but the creation
         * has not yet committed then this is the transaction slot number
         * of the creating transaction. Only the creating transaction can
         * allocate or free NVM from this heap. An on commit operation will
         * clear the flag.
         */
        int32_t inuse;

        uint8_t _padding_to_1024[1024 - 772];
    };
#else
    struct nvm_heap
    {
        /**
         * The type usid is first. It is used to verify that this is a heap. 
         */
        nvm_usid type_usid;    

        /**
         * This is the ASCII name of the heap. Note that this imposes a 63 
         * byte limit on the name of a heap so that this is a null
         * terminated string.
         */
        char name[64];

        /**
         * This is a pointer back to the nvm_region containing this NVM heap.
         * It allows quickly finding the nvm_region from any nvm struct by 
         * following the heap pointer in the nvm_blk just before the struct.
         */
        nvm_region_srp region; // nvm_region ^region
        
        /**
         * This is the address of the extent containing this heap. In the
         * current implementation it is either this nvm_heap or the nvm_region.
         */
        void_srp extent; // void ^extent

        /**
         * This is the optional heap local root object pointer
         */
        void_srp rootobject; //void ^rootobject

        /**
         * This is the current size of the extent in bytes including the
         * nvm_heap at the beginning.
         */
        size_t psize;

        /**
         * This points to the first nvm_blk of the NVM managed by this nvm_heap.
         * It follows immediately after the nvm_heap.
         */
        nvm_blk_srp first;

        /**
         * This is the size of the managed NVM in bytes
         */
        size_t size;

        /**
         * This is the number of bytes currently allocated by this heap. This
         * includes the space consumed by the NVblk headers.
         */
        uint64_t consumed;

        /**
         * This is the NVM mutex that protects the contents of this NVM heap. It
         * must be locked exclusive by any transaction that updates the heap.
         */
        nvm_mutex heap_mutex;
        
        /**
         * This is the mutex level for the mutex in a heap.
         */
#define NVM_HEAP_MUTEX_LEVEL (240)

        /**
         * All blocks allocated to this heap are kept in a doubly linked list.
         * These are the head and tail of that list.
         */
        nvm_list list;

        /**
         * There are a number of free lists for various block sizes. Each 
         * list only contains blocks with usable space within one power of 2, 
         * except for the last list which has the huge blocks of any size. 
         * This array defines the max usable block size for each free list.
         */
        size_t free_size[FREELIST_CNT];

        /**
         * This is the array of free lists. Each free list is a doubly linked 
         * list of nvm_blk structs that are all free.
         */
        nvm_freelist free[FREELIST_CNT];

        /**
         * While non-transactional allocNT is still enabled, this points to
         * the current free nvm_blk where allocation will be done. It is null
         * if non-transactional allocation is finished.
         */
        nvm_blk_srp nvb_free; // nvm_blk ^nvb_free
        
        /**
         * This is zero when the heap is generally available for allocation.
         * It is -1 if the heap has an on commit operation scheduled to
         * delete the heap. If the heap has been created, but the creation
         * has not yet committed then this is the transaction slot number
         * of the creating transaction. Only the creating transaction can
         * allocate or free NVM from this heap. An on commit operation will
         * clear the flag.
         */
        int32_t inuse;

        uint8_t _padding_to_1024[1024 - 772];
    };
#endif //NVM_EXT
    
    /**
     * This creates the root heap in the base extent. This is called before
     * there is a transaction table so it is done non-transactionally. The
     * heap is left in a state where nvm_allocNT can be used to do permanent 
     * allocation. This does not need to be transactional because the USID
     * in the nvm_region is zero indicating the region is not yet initialized
     * 
     * @param[in] name
     * This is the name of the root heap which is the same as the name of the
     * region.
     * 
     * @param[in] region
     * This points to the nvm_region at the base of the region.
     * 
     * @param[in] pspace
     * This is the size of the base extent. The root heap follows the 
     * nvm_region.
     * 
     * @return 
     * Pointer to the root heap. Any errors fire an assert.
     */
#ifdef NVM_EXT
    nvm_heap ^nvm_create_rootheap(
        const char *name,
        nvm_region ^region,
        size_t pspace
        );
#else
    nvm_heap *nvm_create_rootheap(
        const char *name,
        nvm_region *region,
        size_t pspace
        );
#endif //NVM_EXT
    
    /**
     * Slice off the indicated size from the front of the heap. This is
     * only allowed up until the transaction table is ready to use
     * during construction of the segment. Space allocated this way 
     * cannot ever be freed. This does not support array allocation or
     * extensible struct allocation.
     * 
     * @param definition of the struct to allocate
     * @return pointer to the allocated bytes, or return NULL if errno 
     * has an error.
     */
#ifdef NVM_EXT
    void ^nvm_allocNT(nvm_heap ^heap, const nvm_type *tp);
#else
    void *nvm_allocNT(nvm_heap *heap, const nvm_type *tp);
#endif //NVM_EXT
    
    /**
     * This is called when the segment is ready for transactions at segment
     * creation. After this is called nvm_allocNT will no longer work.
     * @param heap The root heap of the segment.
     */
#ifdef NVM_EXT
    void nvm_finishNT(nvm_heap ^heap);
#else
    void nvm_finishNT(nvm_heap *heap);
#endif //NVM_EXT

    /**
     * When a struct is allocated it is initialized to a null state. All numeric
     * values are zeroed, and all pointers are set to be null, which is an
     * offset of 1.
     * @param[in] addr 
     * Address of allocated memory
     * 
     * @param[in] tp 
     * Definition of the allocated struct
     * 
     * @param[in] size
     * The number of bytes allocated. This will be larger than tp->size if this
     * was an array allocation or an extensible struct allocation.
     */
#ifdef NVM_EXT
    void nvm_alloc_init(void ^addr, const nvm_type *tp, size_t size);
#else
    void nvm_alloc_init(void *addr, const nvm_type *tp, size_t size);
#endif //NVM_EXT
    


#ifdef	__cplusplus
}
#endif

#endif	/* NVM_HEAP0_H */

