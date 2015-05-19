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
      nvm_heap.c - Non-Volatile Memory heap management

   DESCRIPTION\n
      This file implements NVM heap management including allocating/freeing
      NVM managed by the heap.

    NOTES\n
      When an NVM region is created the base extent is formatted as an NVM 
      heap. The root struct is allocated from the heap in the base extent. 
      Additional application data is allocated as needed. Additional heaps can 
      be created and deleted as other region extents. Each heap can be given a
      name for administrative purposes.

 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include "nvm.h"
#include "nvm_data0.h"
#include "nvm_region0.h"
#include "nvm_transaction0.h"
#include "nvms_region.h"
#include "nvm_heap0.h"
#include "nvm_locks0.h"

#ifdef NVM_EXT
static nvm_blk ^nvm_alloc_blk@(nvm_heap ^heap, size_t request, uint32_t align);
static void nvm_free_blk@(nvm_heap ^heap, nvm_blk ^nvb);
static void nvm_blk_undo@(nvm_blk ^nvb);
static nvm_heap ^nvm_create_baseheap(const char *name, nvm_region ^rg,
        void ^extent, uint8_t ^addr, size_t bytes, uint16_t slot);
static void nvm_freelist_link@(nvm_heap ^heap, nvm_blk ^nvb);
static void nvm_freelist_unlink@(nvm_blk ^nvb);
inline void nvm_blk_clr(nvm_blk ^nvb) { nvm_set(nvb, 0, sizeof(nvm_blk)); }
#else
static nvm_blk *nvm_alloc_blk(nvm_heap *heap, size_t request, uint32_t align);
static void nvm_free_blk(nvm_heap *heap, nvm_blk *nvb);
static void nvm_blk_undo(nvm_blk *nvb);
static nvm_heap *nvm_create_baseheap(const char *name, nvm_region *rg,
        void *extent, uint8_t *addr, size_t bytes, uint16_t slot);
static void nvm_freelist_link(nvm_heap *heap, nvm_blk *nvb);
static void nvm_freelist_unlink(nvm_blk *nvb);
inline void nvm_blk_clr(nvm_blk *nvb) { nvm_set(nvb, 0, sizeof(nvm_blk)); }
#endif //NVM_EXT

/**
 * There are a number of free lists for various block sizes. Each list
 * only contains blocks with usable space within one power of 2. Except
 * for the last list which has the huge blocks of any size.
 * 
 * This array defines the max usable block size for each free list.
 */
static const size_t free_size_init[FREELIST_CNT] = {
    0x100, 0x200, 0x400, 0x800,
    0x1000, 0x2000, 0x4000, 0x8000,
    0x10000, 0x20000, 0x40000, 0x80000,
    0x10000, 0x200000, 0x400000, 0x800000,
    0x100000, 0x2000000, 0x4000000, 0x8000000,
    0x100000, 0x20000000, 0x40000000, 0x80000000,
    0xffffffffffffffff
};

/* All NVM allocations have at least this alignment */
#define NVM_ALIGN (nvm_type_nvm_blk.align)

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
        )
{
    /* This code assumes some struct sizes to create appropriately aligned 
     * data structures, so assert they are OK.
     */
    if (sizeof(nvm_blk) != NVM_ALIGN)
        nvms_assert_fail("Bad sizeof(nvm_blk)");
    if (sizeof(nvm_region) % NVM_ALIGN)
        nvms_assert_fail("Bad sizeof(nvm_region)");
    if (sizeof(nvm_heap) % NVM_ALIGN)
        nvms_assert_fail("Bad sizeof(nvm_heap)");

    /* Construct a base heap right after the nvm_region */
    return nvm_create_baseheap(name, region, region, (uint8_t^)(region + 1),
            pspace - sizeof(nvm_region), 0);
}
#else
nvm_heap *nvm_create_rootheap(
        const char *name,
        nvm_region *region,
        size_t pspace
        )
{
    /* This code assumes some struct sizes to create appropriately aligned 
     * data structures, so assert they are OK.
     */
    if (sizeof(nvm_blk) != NVM_ALIGN)
        nvms_assert_fail("Bad sizeof(nvm_blk)");
    if (sizeof(nvm_region) % NVM_ALIGN)
        nvms_assert_fail("Bad sizeof(nvm_region)");
    if (sizeof(nvm_heap) % NVM_ALIGN)
        nvms_assert_fail("Bad sizeof(nvm_heap)");

    /* Construct a base heap right after the nvm_region */
    return nvm_create_baseheap(name, region, region, (uint8_t*)(region + 1),
            pspace - sizeof(nvm_region), 0);
}
#endif //NVM_EXT
/**
 * This creates the heap in a heap managed extent. This is called
 * before the extent creation commits so it is done non-transactionally.
 * However it must be called in the transaction that allocated the extent.
 * The heap is left in a state where nvm_allocNT can be used to do 
 * permanent allocation. 
 * 
 * @param[in] name
 * This is the name of the heap.
 * 
 * @param[in] region
 * This points to the nvm_region at the base of the region.
 * 
 * @param[in] extent
 * This points to the first byte of the extent containing the NVM managed by
 * this heap.
 * 
 * @param[in] addr
 * The virtual address where the heap NVM starts
 * 
 * @param[in] bytes
 * This is the size of the NVM to be managed as a heap
 * 
 * @param[in] slot
 * The slot number of the transaction creating this heap, or zero if none.
 * @return 
 * Pointer to the base heap. Any errors fire an assert.
 */
#ifdef NVM_EXT
static nvm_heap ^nvm_create_baseheap(
        const char *name,
        nvm_region ^rg,
        void ^extent,
        uint8_t ^addr,
        size_t bytes,
        uint16_t slot
        )
{
    /* The nvm_heap is the first byte of the managed space */
    uint8_t ^next = addr; // the next byte of free NVM
    nvm_heap ^heap = (nvm_heap ^)next;
    next += sizeof(nvm_heap);

    /* The next thing in the heap is the nvm_blk that describes all the
     * free space after the nvm_heap. */
    nvm_blk ^nvb_begin = (nvm_blk ^)next;
    next += sizeof(nvm_blk);

    /* The last thing in a heap is the nvm_blk that marks the end of NVM */
    uint8_t ^end = addr + bytes; // just past the last byte of NVM
    nvm_blk ^nvb_end = (nvm_blk ^)end - 1;

    /* Initialize the nvm_blk for all the free space. It is not linked to its
     * free list until non-transactional allocation is finished. */
    ^(nvm_usid^)nvb_begin ~= nvm_usidof(nvm_blk);
    nvb_begin=>neighbors.fwrd ~= nvb_end;
    nvb_begin=>neighbors.back ~= 0;
    nvb_begin=>group.fwrd ~= 0;
    nvb_begin=>group.back ~= 0;
    nvb_begin=>ptr ~= 0;
    nvb_begin=>allocated ~= 0;

    /* Initialize the nvm_blk at the very end of all NVM. It indicates it is
     * the end. It is not on any linked list, and is not free or allocated. */
    ^(nvm_usid^)nvb_end ~= nvm_usidof(nvm_blk);
    nvb_end=>neighbors.fwrd ~= 0;
    nvb_end=>neighbors.back ~= nvb_begin;
    nvb_end=>group.fwrd ~= 0;
    nvb_end=>group.back ~= 0;
    nvb_end=>ptr ~= 0;
    nvb_end=>allocated ~= 0;

    /* Initialize the NVM heap. We consider the nvm_blk at the end of the 
     * heap to be allocated, but the nvm_blk of any free blocks are considered
     * free. When there is an allocation then all the space in the allocated 
     * block is considered consumed including its nvm_blk. */
    strncpy((void*)heap=>name, name, sizeof(heap=>name) - 1);
    heap=>region ~= rg;
    heap=>extent ~= extent;
    heap=>rootobject ~= 0;
    heap=>psize ~= (addr + bytes) - (uint8_t^)extent;
    heap=>first ~= (nvm_blk^)(heap+1);
    heap=>size ~= bytes;
    nvm_mutex_init1((nvm_amutex ^)%heap=>heap_mutex, NVM_HEAP_MUTEX_LEVEL);
    heap=>consumed ~= sizeof(nvm_blk) + sizeof(nvm_heap);
    heap=>list.head ~= 0; // nothing allocated yet
    heap=>list.tail ~= 0; 
    heap=>inuse ~= slot; // allow access by creating transaction only

    /* copy table of freelist sizes and flush */
    nvm_copy(heap=>free_size, free_size_init, sizeof(heap=>free_size));
            
    /* Make all the freelists empty */
    int i;
    for (i = 0; i < FREELIST_CNT; i++)
    {
        heap=>free[i].head ~= 0;
        heap=>free[i].tail ~= 0;
    }

    /* enable non-transactional allocation */
    heap=>nvb_free ~= nvb_begin;

    /* set the heap usid last so it is not a valid heap until initialized */
    ^(nvm_usid^)heap ~= nvm_usidof(nvm_heap);

    return heap;
}
#else
static nvm_heap *nvm_create_baseheap(
        const char *name,
        nvm_region *rg,
        void *extent,
        uint8_t *addr,
        size_t bytes,
        uint16_t slot
        )
{
    /* The nvm_heap is the first byte of the managed space */
    uint8_t *next = addr; // the next byte of free NVM
    nvm_heap *heap = (nvm_heap *)next;
    next += sizeof(nvm_heap);

    /* The next thing in the heap is the nvm_blk that describes all the
     * free space after the nvm_heap. */
    nvm_blk *nvb_begin = (nvm_blk *)next;
    next += sizeof(nvm_blk);

    /* The last thing in a heap is the nvm_blk that marks the end of NVM */
    uint8_t *end = addr + bytes; // just past the last byte of NVM
    nvm_blk *nvb_end = (nvm_blk *)end - 1;

    /* Initialize the nvm_blk for all the free space. It is not linked to its
     * free list until non-transactional allocation is finished. */
    nvb_begin->type_usid = nvm_usidof(nvm_blk);
    nvm_blk_set(&nvb_begin->neighbors.fwrd, nvb_end);
    nvm_blk_set(&nvb_begin->neighbors.back, 0);
    nvm_blk_set(&nvb_begin->group.fwrd, 0);
    nvm_blk_set(&nvb_begin->group.back, 0);
    void_set(&nvb_begin->ptr, 0);
    nvb_begin->allocated = 0;
    nvm_flush1(nvb_begin);

    /* Initialize the nvm_blk at the very end of all NVM. It indicates it is
     * the end. It is not on any linked list, and is not free or allocated. */
    nvb_end->type_usid = nvm_usidof(nvm_blk);
    nvm_blk_set(&nvb_end->neighbors.fwrd, 0);
    nvm_blk_set(&nvb_end->neighbors.back, nvb_begin);
    nvm_blk_set(&nvb_end->group.fwrd, 0);
    nvm_blk_set(&nvb_end->group.back, 0);
    void_set(&nvb_end->ptr, 0);
    nvb_end->allocated = 0;
    nvm_flush1(nvb_end);

    /* Initialize the NVM heap. We consider the nvm_blk at the end of the 
     * heap to be allocated, but the nvm_blk of any free blocks are considered
     * free. When there is an allocation then all the space in the allocated 
     * block is considered consumed including its nvm_blk. */
    strncpy((void*)heap->name, name, sizeof(heap->name) - 1);
    nvm_region_set(&heap->region, rg);
    void_set(&heap->extent, extent);
    void_set(&heap->rootobject, 0);
    heap->psize = (addr + bytes) - (uint8_t*)extent;
    nvm_blk_set(&heap->first, (nvm_blk*)(heap+1));
    heap->size = bytes;
    nvm_mutex_init1((nvm_amutex*)&heap->heap_mutex, NVM_HEAP_MUTEX_LEVEL);
    heap->consumed = sizeof(nvm_blk) + sizeof(nvm_heap);
    nvm_blk_set(&heap->list.head, 0); // nothing allocated yet
    nvm_blk_set(&heap->list.tail, 0);
    heap->inuse = slot; // allow access by creating transaction only

    /* copy table of freelist sizes */
    nvm_copy(heap->free_size, free_size_init, sizeof(heap->free_size));
            
    /* Make all the freelists empty */
    int i;
    for (i = 0; i < FREELIST_CNT; i++)
    {
        nvm_blk_set(&heap->free[i].head, 0);
        nvm_blk_set(&heap->free[i].tail, 0);
    }

    /* enable non-transactional allocation */
    nvm_blk_set(&heap->nvb_free, nvb_begin);

    /* set the heap usid last so it is not a valid heap until initialized */
    heap->type_usid = nvm_usidof(nvm_heap);

    /* Flush the heap  out of our processor cache. */
    nvm_flush(heap, sizeof(*heap));

    return heap;
}
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
void ^nvm_allocNT(nvm_heap ^heap, const nvm_type *tp)
{
    /* Verify this is a valid heap pointer */
    nvm_verify(heap, shapeof(nvm_heap));

    /* Verify still in non-transactional allocation */
    nvm_blk ^aloc = heap=>nvb_free;
    if (aloc == 0)
        nvms_assert_fail("Non-transactional allocation called after "
            "nvm_finishNT() called");
    nvm_verify(aloc, shapeof(nvm_blk)); // verify USID

    /* Verify 64 byte alignment is OK */
    if (tp->align > sizeof(nvm_blk))
        nvms_assert_fail("Unsupported alignment for non-transactional "
            "allocation");

    /* Determine the number of bytes we will actually allocate. Need to round
     * up to the next alignment boundary, and allow for a new nvm_blk. */
    size_t asz = tp->size + sizeof(nvm_blk);
    if ((tp->size & (NVM_ALIGN - 1)) != 0)
        asz += NVM_ALIGN - (tp->size & (NVM_ALIGN - 1));
    heap=>consumed ~+= asz;

    /* Construct a new nvm_blk for the new free chunk. Note we just made asz
     * be a multiple of sizeof(nvm_blk). */
    nvm_blk ^new_free = aloc + asz / sizeof(nvm_blk);
    nvm_blk ^end = aloc=>neighbors.fwrd;
    ^(nvm_usid^)new_free ~= nvm_usidof(nvm_blk);
    new_free=>neighbors.fwrd ~= end;
    end=>neighbors.back ~= new_free;
    new_free=>neighbors.back ~= aloc;
    new_free=>group.fwrd ~= 0; // only entry on the free list
    new_free=>group.back ~= 0; // only entry on the free list
    new_free=>ptr ~= 0; // not on free list until finalized
    new_free=>allocated ~= 0; // not allocated

    /* Make the old free nvm_blk be the tail of the allocated list. Note that 
     * the neighbor back pointer is still the same. */
    nvm_blk ^old_tail = heap=>list.tail;
    aloc=>neighbors.fwrd ~= new_free;
    aloc=>group.fwrd ~= 0; // new end of allocated list
    aloc=>group.back ~= old_tail;
    aloc=>ptr ~= heap; // point at owning Heap
    aloc=>allocated ~= tp->size; // size requested not space consumed
    heap=>list.tail ~= aloc; // heap list has a new tail
    if (old_tail)
    {   /* if not the first allocation */
        old_tail=>group.fwrd ~= aloc; // point old tail to new tail
    }else {
        /* new allocation is the only one in the heap. */
        heap=>list.head ~= aloc;
    }
    
    /* Advance the free pointer*/
    heap=>nvb_free ~= new_free;

    /* Do the standard initialization of NVM allocated data. It is just after
     * its nvm_blk.*/
    void ^ret = aloc + 1;
    nvm_alloc_init(ret, tp, tp->size);

    /* return the newly allocated space. It is just after its nvm_blk.
     * Note that it is not necessary to zero the space because the 
     * extent was just created and filled with zero. */
    return ret;
}
#else
void *nvm_allocNT(nvm_heap *heap, const nvm_type *tp)
{
    /* Verify this is a valid heap pointer */
    nvm_verify(heap, shapeof(nvm_heap));

    /* Verify still in non-transactional allocation */
    nvm_blk *aloc = nvm_blk_get(&heap->nvb_free);
    if (aloc == NULL)
        nvms_assert_fail("Non-transactional allocation called after "
            "nvm_finishNT() called");
    nvm_verify(aloc, shapeof(nvm_blk)); // verify USID

    /* Verify 64 byte alignment is OK */
    if (tp->align > sizeof(nvm_blk))
        nvms_assert_fail("Unsupported alignment for non-transactional "
            "allocation");

    /* Determine the number of bytes we will actually allocate. Need to round
     * up to the next alignment boundary, and allow for a new nvm_blk. */
    size_t asz = tp->size + sizeof(nvm_blk);
    if ((tp->size & (NVM_ALIGN - 1)) != 0)
        asz += NVM_ALIGN - (tp->size & (NVM_ALIGN - 1));
    heap->consumed += asz;
    nvm_flush1(&heap->consumed);

    /* Construct a new nvm_blk for the new free chunk. Note we just made asz
     * be a multiple of sizeof(nvm_blk). */
    nvm_blk *new_free = aloc + asz / sizeof(nvm_blk);
    nvm_blk *end = nvm_blk_get(&aloc->neighbors.fwrd);
    new_free->type_usid = nvm_usidof(nvm_blk);
    nvm_blk_set(&new_free->neighbors.fwrd, end);
    nvm_blk_set(&end->neighbors.back, new_free);
    nvm_flush1(end);
    nvm_blk_set(&new_free->neighbors.back, aloc);
    nvm_blk_set(&new_free->group.fwrd, 0); // only entry on the free list
    nvm_blk_set(&new_free->group.back, 0); // only entry on the free list
    void_set(&new_free->ptr, 0); // not on free list until finalized
    new_free->allocated = 0; // not allocated
    nvm_flush1(new_free);

    /* Make the old free nvm_blk be the tail of the allocated list. Note that 
     * the neighbor back pointer is still the same. */
    nvm_blk *old_tail = nvm_blk_get(&heap->list.tail);
    nvm_blk_set(&aloc->neighbors.fwrd, new_free);
    nvm_blk_set(&aloc->group.fwrd, 0); // new end of allocated list
    nvm_blk_set(&aloc->group.back, old_tail);
    void_set(&aloc->ptr, heap); // point at owning Heap
    aloc->allocated = tp->size; // size requested not space consumed
    nvm_flush1(aloc);
    nvm_blk_set(&heap->list.tail, aloc); // heap list has a new tail
    nvm_flush1(&heap->list.tail);
    if (old_tail)
    {   /* if not the first allocation */
        nvm_blk_set(&old_tail->group.fwrd, aloc); // point old tail to new tail
        nvm_flush1(&old_tail->group.fwrd);
    }else {
        /* new allocation is the only one in the heap. */
        nvm_blk_set(&heap->list.head, aloc); // heap list has a head
        nvm_flush1(&heap->list.head);
    }

    /* Advance the free pointer*/
    nvm_blk_set(&heap->nvb_free, new_free);
    nvm_flush1(&heap->nvb_free);

    /* Do the standard initialization of NVM allocated data. It is just after
     * its nvm_blk.*/
    void *ret = aloc + 1;
    nvm_alloc_init(ret, tp, tp->size);

    /* return the newly allocated space. It is just after its nvm_blk.
     * Note that it is not necessary to zero the space because the 
     * extent was just created and filled with zero. */
    return ret;
}
#endif //NVM_EXT
/**
 * This is called when the segment is ready for transactions at segment
 * creation. After this is called nvm_allocNT will no longer work.
 * @param heap The root heap of the region.
 */
#ifdef NVM_EXT
void nvm_finishNT(nvm_heap ^heap)
{
    /* Verify this is a valid heap pointer */
    nvm_verify(heap, shapeof(nvm_heap));

    /* Verify still in non-transactional allocation */
    nvm_blk ^free = heap=>nvb_free;
    if (free == 0)
        nvms_assert_fail("nvm_finishNT() called again");
    nvm_verify(free, shapeof(nvm_blk)); // verify USID

    /* Add the block to the appropriate freelist. Pick the free space list to
     * add this to based on size. */
    size_t fs = ((free=>neighbors.fwrd - free) - 1) * sizeof(nvm_blk);
    nvm_freelist ^fl = heap=>free;
    int i;
    for (i = 0; fs > heap=>free_size[i]; i++, fl++)
        if (i >= FREELIST_CNT)
            nvms_corruption("Bad free_size array", heap=>free, heap);

    /*  make the free chunk be the only entry in the freelist */
    fl=>head ~= free;
    fl=>tail ~= free;
    free=>ptr ~= fl;

    /* disable non-transactional allocation */
    heap=>nvb_free ~= 0;
}
#else
void nvm_finishNT(nvm_heap *heap)
{
    /* Verify this is a valid heap pointer */
    nvm_verify(heap, shapeof(nvm_heap));

    /* Verify still in non-transactional allocation */
    nvm_blk *free = nvm_blk_get(&heap->nvb_free);
    if (free == NULL)
        nvms_assert_fail("nvm_finishNT() called again");
    nvm_verify(free, shapeof(nvm_blk)); // verify USID

    /* Add the block to the appropriate freelist. Pick the free space list to
     * add this to based on size. */
    size_t fs = ((nvm_blk_get(&free->neighbors.fwrd) - free) - 1)
                * sizeof(nvm_blk);
    nvm_freelist *fl = heap->free;
    int i;
    for (i = 0; fs > heap->free_size[i]; i++, fl++)
        if (i >= FREELIST_CNT)
            nvms_corruption("Bad free_size array", heap->free, heap);

    /*  make the free chunk be the only entry in the freelist */
    nvm_blk_set(&fl->head, free);
    nvm_flush1(&fl->head);
    nvm_blk_set(&fl->tail, free);
    nvm_flush1(&fl->tail);
    void_set(&free->ptr, fl);
    nvm_flush1(free);

    /* disable non-transactional allocation */
    nvm_blk_set(&heap->nvb_free, 0);
    nvm_flush1(&heap->nvb_free);
}
#endif //NVM_EXT

/*
 * This is a callback and its context for clearing the inuse flag of a
 * nvm_heap when the creation commits.
 */
#ifdef NVM_EXT
persistent
struct nvm_inuse_ctx
{
    nvm_heap ^heap;
};
typedef persistent struct nvm_inuse_ctx nvm_inuse_ctx;
USID("5d8f 8034 4a7e 08dd 1f04 8aa6 98a5 f4c1")
void nvm_inuse_callback@(nvm_inuse_ctx ^ctx)
{
    nvm_heap ^heap = ctx=>heap;
    if (!heap)
        return; // was not prepared
    nvm_xlock(%heap=>heap_mutex);
    ctx=>heap=>inuse @= 0;
}
#else
struct nvm_inuse_ctx
{
    nvm_heap_srp heap;
};
typedef struct nvm_inuse_ctx nvm_inuse_ctx;
void nvm_inuse_callback(nvm_inuse_ctx *ctx)
{
    nvm_heap *heap = nvm_heap_get(&ctx->heap);
    if (!heap)
        return; // was not prepared
    nvm_xlock(&heap->heap_mutex);
    NVM_UNDO(heap->inuse);
    heap->inuse = 0;
    nvm_flush1(&heap->inuse);
}
extern const nvm_type nvm_type_nvm_inuse_ctx;
extern const nvm_extern nvm_extern_nvm_inuse_callback;
#endif //NVM_EXT

/**
 * This function adds a new heap managed extent of NVM to a region that is
 * currently attached. It formats the extent as an NVM heap. The
 * return value is a pointer to the new heap.Note that the heap pointer is
 * not the same address as the extent address passed in. The heap pointer
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
        nvm_desc desc,
        void ^extent,   
        size_t psize,
        const char *name
        )
{
    /* Create the extent to hold the new heap. This also does all the error
     * checking needed, and it creates undo to drop the extent if this does
     * not commit. */
    if (nvm_add_extent(extent, psize) == 0)
    {
        nvm_abort();
        return 0;
    }

    /*  get the region data. */
    nvm_app_data *ad = nvm_get_app_data();
    nvm_region_data *rd = ad->regions[desc];
    nvm_region ^rg = rd->region;

    /* Format it as a heap. This is completely non-transactional.
     * Note that it is also called to format the root heap before there
     * are transactions. */
    nvm_heap ^heap = nvm_create_baseheap(name, rg, extent, extent, psize,
                        nvm_txslot());

    /* Do not allow non-transactional allocate. */
    nvm_finishNT(heap);

    /* Create an on commit operation to make this heap available for
     * general use when the caller commits its transaction. This does not
     * need to be transactional because rollback deletes the extent.
     */
    nvm_inuse_ctx ^ctx = nvm_oncommit(|nvm_inuse_callback);
    ctx=>heap ~= heap;

    return heap;
}
#else
nvm_heap *nvm_create_heap(
        nvm_desc desc,
        void *extent,    
        size_t psize,
        const char *name
        )
{
    /* Create the extent to hold the new heap. This also does all the error
     * checking needed, and it creates undo to drop the extent, and thus the
     * heap, if the current transaction does not commit. */
    if (nvm_add_extent(extent, psize) == NULL)
        return NULL;

    /*  get the region data. */
    nvm_app_data *ad = nvm_get_app_data();
    nvm_region_data *rd = ad->regions[desc];
    nvm_region *rg = rd->region;

    /* Format it as a heap. This is completely non-transactional. Note that it
     * is also called to format the root heap before there are transactions. */
    nvm_heap *heap =
            nvm_create_baseheap(name, rg, extent, extent, psize, nvm_txslot());

    /* Do not allow non-transactional allocate. */
    nvm_finishNT(heap);

    /* Create an on commit operation to make this heap available for
     * general use when the caller commits its transaction. This does not
     * need to be transactional because rollback deletes the extent.
     */
    nvm_inuse_ctx *ctx = nvm_oncommit(nvm_extern_nvm_inuse_callback.usid);
    nvm_heap_ntset(&ctx->heap, heap);

    return heap;
}
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
 * threads could allocate the new space even before nvm_resize_heap
 * returns. Thus the library cannot generate undo to rollback the resize
 * if the caller's transaction aborts.
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
int nvm_resize_heap(nvm_heap ^heap, size_t psize)
{
    /* Verify this is a heap and find the region. */
    nvm_verify(heap, shapeof(nvm_heap));
    nvm_region ^rg = heap=>region;
    nvm_verify(rg, shapeof(nvm_region));

    /* Begin a transaction to do the resize */
    @ rg=>desc { //nvm_txbegin(rg=>desc);

        /* Lock the heap to prevent allocations while we do this. Since we are 
         * going to need the region lock later, lock it now to lock in the
         * correct locking order. */
        nvm_xlock(%rg=>reg_mutex);
        nvm_xlock(%heap=>heap_mutex);

        /* Get the address of the extent. */
        void ^ext = heap=>extent;

        /* Get the nvm_blk at the very end of the extent and the nvm_blk for
         * the last block of NVM. */
        size_t osize = heap=>psize; // old physical size
        nvm_blk ^nvb_end = (nvm_blk^)((uint8_t^)ext + osize) - 1;
        nvm_verify(nvb_end, shapeof(nvm_blk));
        nvm_blk ^nvb_last = nvb_end=>neighbors.back;
        nvm_verify(nvb_last, shapeof(nvm_blk));
        if (nvb_end=>ptr != 0)
            nvms_corruption("End nvm_blk ptr not null", nvb_end, nvb_end=>ptr);

        /* If shrinking make sure there is space available at the end of the
         * heap. If growing check for last being allocated. */
        if (psize < osize)
        {
            size_t shrink = osize - psize;

            /* Ensure the last block has enough free space to shrink and still
             * have our minimum sized block */
            if (nvb_last=>allocated != 0 ||
                (nvb_end - nvb_last) * sizeof(nvm_blk) - heap=>free_size[0]
                < shrink)
            {
                /* shrinking */
                nvm_abort();
                errno = ENOSPC;
//                printf("insufficient space at end:%ld shrink:%ld, last:%ld\n",
//                        (nvb_end - nvb_last - 1) * sizeof(nvm_blk)
//                        - fls=>free_size[0], shrink, nvb_last=>allocated);
                return 0;
            }
        }
        else
        {
            /* growing, if last is allocated then use end as last. */
            if (nvb_last=>allocated != 0)
                nvb_last = nvb_end;
        }

        /* Resize the extent. If shrinking, an on commit operation will release
         * the NVM back to the file system. If growing the new NVM will be
         * mapped in when this returns. This also does a number of error
         * checks. */
        if (!nvm_resize_extent1(ext, psize))
        {
            nvm_abort();
            return 0;
        }

        /* get a pointer to the new end nvm_blk */
        nvm_blk ^nvb_new = (nvm_blk^)((uint8_t^)ext + psize) - 1;

        /* Create undo for the current nvm_blk's that we will change. */
        nvm_blk_undo(nvb_last); // undo before it is unlinked from freelist
        if (psize < osize)
        {
            /* shrinking abandons old end nvm_blk so no undo needed. */
            nvm_blk_undo(nvb_new); // undo to clear new end if we abort
        }
        else
        {
            /*  Growing so clear old end if it will be in the middle of 
             * free space. If it is the new last block then leave it alone. */
            if (nvb_end != nvb_last)
            {
                nvm_blk_undo(nvb_end);
                nvm_blk_clr(nvb_end);
            }
        }

        /* The size of the last block is going to change so remove it from its
         * current freelist, unless it is the current end that is not on a
         * freelist. */
        if (nvb_end != nvb_last)
            nvm_freelist_unlink(nvb_last);

        /* Create a new end nvm_blk to reflect the new size. Note that we
         * created undo above or growing and this space will be released on
         * abort, so this stores non-transactionally */
        ^(nvm_usid^)nvb_new ~= nvm_usidof(nvm_blk); // set USID
        nvb_new=>neighbors.fwrd ~= 0; // no forward from end
        nvb_new=>neighbors.back ~= nvb_last;
        nvb_new=>group.fwrd ~= 0;
        nvb_new=>group.back ~= 0;
        nvb_new=>ptr ~= 0;
        nvb_new=>allocated ~= 0;

        /* last now points to new end. */
        nvb_last=>neighbors.fwrd ~= nvb_new;

        /* put last back into correct freelist. */
        nvm_freelist_link(heap, nvb_last);

        /* adjust the space in the freelists */
        if (psize < osize)
        {
            /* shrink total bytes and free bytes */
            heap=>size @-= osize - psize;
        }
        else
        {
            /* grow total bytes and free bytes */
            heap=>size @+= psize - osize;
        }

        /* Store the new size in the region header if needed */
        if ((void^)rg == ext)
        {
            /* Set size of base extent in region header. */
            rg=>header.psize @= psize;
        }
        
        /* Set the size of the extent in the heap. */
        heap=>psize @= psize;

        /* Resize is complete so commit the changes. */
        nvm_commit();
    } //nvm_txend();
    return 1;
}
#else
int nvm_resize_heap(nvm_heap *heap, size_t psize)
{
    /* Verify this is a heap and find the region. */
    nvm_verify(heap, shapeof(nvm_heap));
    nvm_region *rg = nvm_region_get(&heap->region);
    nvm_verify(rg, shapeof(nvm_region));

    /* Begin a transaction to do the resize */
    nvm_txbegin(rg->desc);

    /* Lock the heap to prevent allocations while we do this. Since we are 
     * going to need the region lock later, lock it now to lock in the correct
     * locking order. */
    nvm_xlock(&rg->reg_mutex);
    nvm_xlock(&heap->heap_mutex);

    /* Get the address of the extent. */
    void *ext = void_get(&heap->extent);

    /* Get the nvm_blk at the very end of the extent and the nvm_blk for
     * the last block of NVM. */
    size_t osize = heap->psize; // old physical size
    nvm_blk *nvb_end = (nvm_blk*)((uint8_t*)ext + osize) - 1;
    nvm_verify(nvb_end, shapeof(nvm_blk));
    nvm_blk *nvb_last = nvm_blk_get(&nvb_end->neighbors.back);
    nvm_verify(nvb_last, shapeof(nvm_blk));
    if (void_get(&nvb_end->ptr) != NULL)
        nvms_corruption("End nvm_blk ptr not null", nvb_end, 
                void_get(&nvb_end->ptr));

    /* If shrinking make sure there is space available at the end of the
     * heap. If growing check for last being allocated. */
    if (psize < osize)
    {
        size_t shrink = osize - psize;

        /* Ensure the last block has enough free space to shrink and still
         * have our minimum sized block */
        if (nvb_last->allocated != 0 ||
           (nvb_end - nvb_last) * sizeof(nvm_blk) - heap->free_size[0] < shrink)
        {
            nvm_abort();
            nvm_txend();
            errno = ENOSPC;
//            printf("insufficient space at end:%ld shrink:%ld, last:%ld\n",
//                    (nvb_end - nvb_last - 1) * sizeof(nvm_blk)
//                    - fls->free_size[0], shrink, nvb_last->allocated);
            return 0;
        }
    }
    else
    {
        /* if last is allocated then use end as last. */
        if (nvb_last->allocated != 0)
            nvb_last = nvb_end;
    }

    /* Resize the extent. If shrinking, an on commit operation will release
     * the NVM back to the file system. If growing the new NVM will be mapped
     * in when this returns. This also does a number of error checks. */
    if (!nvm_resize_extent1(ext, psize))
    {
        nvm_abort();
        nvm_txend();
        return 0;
    }

    /* get a pointer to the new end nvm_blk */
    nvm_blk *nvb_new = (nvm_blk*)((uint8_t*)ext + psize) - 1;

    /* Create undo for the current nvm_blk's that we will change. */
    nvm_blk_undo(nvb_last); // undo before it is unlinked from freelist
    if (psize < osize)
    {
        /* shrinking abandons old end nvm_blk so no undo needed. */
        nvm_blk_undo(nvb_new); // undo to clear new end if we abort
    }
    else
    {
        /*  Growing so clear old end if it will be in the middle of 
         * free space. If it is the new last block then leave it alone. */
        if (nvb_end != nvb_last)
        {
            nvm_blk_undo(nvb_end);
            nvm_blk_clr(nvb_end);
        }
    }

    /* The size of the last block is going to change so remove it from its
     * current freelist, unless it is the current end that is not on a
     * freelist. */
    if (nvb_end != nvb_last)
        nvm_freelist_unlink(nvb_last);

    /* Create a new end nvm_blk to reflect the new size. */
    nvb_new->type_usid = nvm_usidof(nvm_blk); // set USID
    nvm_blk_set(&nvb_new->neighbors.fwrd, NULL); // no forward from end
    nvm_blk_set(&nvb_new->neighbors.back, nvb_last);
    nvm_blk_set(&nvb_new->group.fwrd, NULL);
    nvm_blk_set(&nvb_new->group.back, NULL);
    void_set(&nvb_new->ptr, NULL);
    nvb_new->allocated = 0;
    nvm_flush1(nvb_new);

    /* last now points to new end. */
    nvm_blk_set(&nvb_last->neighbors.fwrd, nvb_new);

    /* put last back into correct freelist. */
    nvm_freelist_link(heap, nvb_last);
    nvm_flush1(nvb_last);

    /* adjust the space in the freelists */
    NVM_UNDO(heap->size);
    if (psize < osize)
    {
        /* shrink total bytes and free bytes */
        heap->size -= osize - psize;
    }
    else
    {
        /* grow total bytes and free bytes */
        heap->size += psize - osize;
    }
    nvm_flush1(&heap->size);

    /* Store the new size in the region header if needed. */
    if ((void*)rg == ext)
    {
        /* Set size of base extent in region header. */
        NVM_UNDO(rg->header.psize);
        rg->header.psize = psize;
        nvm_flush1(&rg->header.psize);
    }
    
    /* Set the size of the extent in the heap. */
    NVM_UNDO(heap->psize);
    heap->psize = psize;
    nvm_flush1(&heap->psize);

    /* Resize is complete so commit the changes. */
    nvm_commit();
    nvm_txend();
    return 1;
}
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
 * @return 
 * 0 is returned on error and 1 is returned on success.
 * 
 * @par Errors:
 */
#ifdef NVM_EXT
int nvm_delete_heap@(
        nvm_heap ^heap
        )
{
    /* Verify this is a heap and find the region. */
    nvm_verify(heap, shapeof(nvm_heap));
    nvm_region ^rg = heap=>region;
    nvm_verify(rg, shapeof(nvm_region));

    /* Get the address of the extent and verify it is not the base extent. */
    void ^ext = heap=>extent;
    if (ext == (void^)rg)
    {
        errno = EINVAL;
        return 0;
    }

    /* Create an on abort operation to make this heap available for
     * general use if this delete is rolled back. The heap pointer is
     * atomically set with marking the heap as deleting.
     */
    nvm_inuse_ctx ^ctx = nvm_onabort(|nvm_inuse_callback);

    /* begin the transaction to mark the heap as deleting. */
    @{
        
        /* Lock the extent to ensure there are no allocations currently in
         * progress. */
        nvm_xlock(%heap=>heap_mutex);

        /* It is an error if the heap is being deleted or in creation by
         * another transaction. However the creating transaction may delete
         * the heap even if not committed. */
        if (heap=>inuse && heap=>inuse != nvm_txslot())
        {
            nvm_abort();
            errno = EBUSY;
            return 0;
        }

        /* Mark the heap as being deleted to prevent any new allocations,
         * unless this transaction rolls back. */
        heap=>inuse @= -1;

        /* clear the inuse flag if the delete rolls back */
        ctx=>heap @= heap;

    }

    /* Remove it. All this really does is add an on commit operation */
    if (!nvm_remove_extent1(ext))
        return 0;

    return 1;
}
#else
int nvm_delete_heap(   
        nvm_heap *heap
        )
{
    /* Verify this is a heap and find the region. */
    nvm_verify(heap, shapeof(nvm_heap));
    nvm_region *rg = nvm_region_get(&heap->region);
    nvm_verify(rg, shapeof(nvm_region));

    /* Get the address of the extent and verify it is not the base extent. */
    void *ext = void_get(&heap->extent);
    if (ext == (void*)rg)
    {
        errno = EINVAL;
        return 0;
    }

    /* Create an on abort operation to make this heap available for
     * general use if this delete is rolled back. The heap pointer is
     * atomically set with marking the heap as deleting.
     */
    nvm_inuse_ctx *ctx = nvm_onabort(nvm_extern_nvm_inuse_callback.usid);

    /* begin the transaction to mark the heap as deleting. */
    nvm_txbegin(0);
        
    /* Lock the extent to ensure there are no allocations currently in
     * progress. */
    nvm_xlock(&heap->heap_mutex);

    /* It is an error if the heap is being deleted or in creation by
     * another transaction. However the creating transaction may delete
     * the heap even if not committed. */
    if (heap->inuse && heap->inuse != nvm_txslot())
    {
        nvm_abort();
        errno = EBUSY;
        return 0;
    }

    /* Mark the heap as being deleted to prevent any new allocations, unless
     * this transaction rolls back. */
    NVM_UNDO(heap->inuse);
    heap->inuse = -1;
    nvm_flush1(&heap->inuse);

    /* clear the inuse flag if this rolls back */
    nvm_heap_txset(&ctx->heap, heap);

    /* commit the inuse update */
    nvm_txend();

    /* Remove it. All this really does is add an on commit operation */
    if (!nvm_remove_extent1(ext))
        return 0;

    return 1;
}
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
void nvm_heap_setroot@(nvm_heap ^heap, void ^root)
{
    /* Verify this is a valid heap pointer */
    nvm_verify(heap, shapeof(nvm_heap));

    /* The root object must be null or within the heap */
    if (root && ((uint8_t^)root < (uint8_t^)heap ||
        (uint8_t^)root >= (uint8_t^)heap + heap=>psize ))
    {
        nvms_assert_fail("Out of range heap root object pointer");
    }

    /* Store the new pointer transactionally. Note that this is not done while
     * holding the heap mutex. The application needs to ensure there is no
     * race condition here between two stores. Most likely the root object
     * pointer is set only once shortly after creation of the heap.
     */
    heap=>rootobject @= root;
}
#else
void nvm_heap_setroot(nvm_heap *heap, void *root)
{
    /* Verify this is a valid heap pointer */
    nvm_verify(heap, shapeof(nvm_heap));

    /* The root object must be within the heap */
    if ((uint8_t*)root < (uint8_t*)heap ||
        (uint8_t*)root >= (uint8_t*)heap + heap->psize )
    {
        nvms_assert_fail("Out of range heap root object pointer");
    }

    /* Store the new pointer transactionally. Note that this is not done while
     * holding the heap mutex. The application needs to ensure there is no
     * race condition here between two stores. Most likely the root object
     * pointer is set only once shortly after creation of the heap.
     */
    void_txset(&heap->rootobject, root);
}
#endif //NVM_EXT

/**
 * This returns the status of a heap Note that the data returned might not
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
        )
{
    /* start off with status of zero so even if there are errors the contents
     * are not garbage. */
    memset(stat, 0, sizeof(*stat));

    /* Verify this is a valid heap pointer */
    nvm_verify(heap, shapeof(nvm_heap));

    /* store stat contents from the heap struct */
    stat->name = heap=>name;
    nvm_region ^rg = heap=>region;
    stat->region = rg=>desc;
    stat->extent = heap=>extent;
    stat->rootobject = heap=>rootobject;
    stat->free = heap=>size - heap=>consumed;
    stat->consumed = heap=>consumed;
    stat->inuse = heap=>inuse;

    return 1;
}
#else
int nvm_query_heap(
        nvm_heap *heap,
        nvm_heap_stat *stat
        )
{
    /* start off with status of zero so even if there are errors the contents
     * are not garbage. */
    memset(stat, 0, sizeof(*stat));

    /* Verify this is a valid heap pointer */
    nvm_verify(heap, shapeof(nvm_heap));

    /* store stat contents from the heap struct */
    stat->name = heap->name;
    nvm_region *rg = nvm_region_get(&heap->region);
    stat->region = rg->desc;
    stat->extent = void_get(&heap->extent);
    stat->rootobject = void_get(&heap->rootobject);
    stat->free = heap->size - heap->consumed;
    stat->consumed = heap->consumed;
    stat->inuse = heap->inuse;

    return 1;
}
#endif //NVM_EXT


/**
 * This is the context for an on abort or on commit operation to return NVM to
 * a freelist. It simply provides the address of the nvm_blk to free.
 */
#ifdef NVM_EXT
persistent
struct nvm_free_ctx
{
    nvm_blk ^blk;
};
typedef persistent struct nvm_free_ctx nvm_free_ctx;
USID("3191 c3be 57cf 5408 3492 134c df53 6df7")
void nvm_free_callback@(nvm_free_ctx ^ctx);
#else
struct nvm_free_ctx
{
    nvm_blk_srp blk;    
};
typedef struct nvm_free_ctx nvm_free_ctx;
extern const nvm_type nvm_type_nvm_free_ctx;
extern const nvm_extern nvm_extern_nvm_free_callback;
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
 * set to the correct values. This includes the USID for the struct being 
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
        const nvm_type *tp, // type definition of struct to allocate
        unsigned count // size of the array
        )
{
    /* Verify the count is reasonable */
    if (count <= 0)
    {
        errno = EINVAL;
        return 0;
    }

    /* Verify this is a valid heap pointer */
    nvm_verify(heap, shapeof(nvm_heap));

    /* verify in transactional allocation */
    if (heap=>nvb_free != 0)
        nvms_assert_fail("nvm_finishNT() not called yet");

    /* Determine how much space is being requested. If this is an extensible
     * struct, then the count is for the extensible array at the end. If
     * not, then it is the number of structs to allocate in an array. */
    size_t request;
    if (tp->xsize)
        request = tp->size + count * tp->xsize;
    else
        request = count * tp->size;

    /* If the count is garbage the request could be huge. */
    if (request > heap=>size)
    {
        errno = EINVAL;
        return 0;
    }

    /* Create an on abort operation to undo this allocation if the base
     * transaction aborts. Create a savepoint to eliminate this record if
     * the allocation fails. */
    if (!nvm_savepoint(heap)) // the heap address is an OK savepoint name
        return 0; // error, probably hit undo limit
    nvm_free_ctx ^ctx = nvm_onabort(|nvm_free_callback);

    /* Begin a nested transaction to do the allocation. This allows the heap
     * and freelist locks to be dropped before returning. */
    @{ // nvm_txbegin(0);    

        /* Get the mutex for the heap so we can examine and modify it */
        nvm_xlock(%heap=>heap_mutex);

        /* It is an error if the heap is being deleted or in creation by
         * another transaction. */
        if (heap=>inuse && heap=>inuse != nvm_txslot())
        {
            nvm_abort();
            errno = EBUSY;
            return 0;
        }

        /* Find an nvm_blk for the allocation if available. This also creates
         * undo for the nvm_blk returned. */
        nvm_blk ^nvb = nvm_alloc_blk(heap, request, tp->align);
        if (nvb == 0)
        {
            nvm_abort();
            errno = ENOMEM;
            goto fail;
        }

        /* The amount consumed is the physical size of the allocated block. */
        nvm_blk ^next = nvb=>neighbors.fwrd; //#
        size_t consume = (next - nvb) * sizeof(nvm_blk); //#
        heap=>consumed @+= consume;

        /* Transactionally store the allocated nvm_blk pointer in the on abort
         * context. If this nested transaction does not commit, undo will clear 
         * the pointer so that the on abort operation will be a no-op. */
        ctx=>blk @= nvb;

        /* Mark the newly allocate block as allocated. Undo already generated
         * in nvm_alloc_blk. */
        nvb=>allocated ~= request; //#

        /* Add the allocation at the head of  this heap's allocated list.
         * Note that nvm_alloc_blk already created undo for *nvb. */
        nvb=>ptr ~= heap;
        nvb=>group.back ~= 0;
        nvm_blk ^olh = heap=>list.head; // old listhead
        heap=>list.head @= nvb;
        nvb=>group.fwrd ~= olh;
        if (olh)
        {
            /* the allocated list is not empty, so point the old head back */
            olh=>group.back @= nvb;
        }
        else
        {
            /* the allocated list is empty, so now it has one entry. */
            heap=>list.tail @= nvb; // this is now the tail
        }

        /* Do the standard initialization of NVM allocated data. It is just
         * after its nvm_blk.*/
        void ^ret = nvb + 1;
        nvm_alloc_init(ret, tp, request);

        /* commit the allocation */
        nvm_commit();    

        /* Return successful allocation. The allocated space immediately follows
         * the nvm_blk describing it. */
        return ret;
    }      


    /* If we fail to allocate, remove the unnecessary on abort undo. This keeps
     * repeated failures from building up undo that is not needed. Note that
     * errno must already be set. */
fail:
    nvm_rollback(heap);
    return 0;
}
#else
void *nvm_alloc(   
        nvm_heap *heap,    
        const nvm_type *tp, // type definition of struct to allocate
        unsigned count // size of the array
        )
{
    /* Verify the count is reasonable */
    if (count <= 0)
    {
        errno = EINVAL;
        return NULL;
    }

    /* Verify this is a valid heap pointer */
    nvm_verify(heap, shapeof(nvm_heap));

    /* verify in transactional allocation */
    if (nvm_blk_get(&heap->nvb_free) != NULL)
        nvms_assert_fail("nvm_finishNT() not called yet");

    /* Determine how much space is being requested. If this is an extensible
     * struct, then the count is for the extensible array at the end. If
     * not, then it is the number of structs to allocate in an array. */
    size_t request;
    if (tp->xsize)
        request = tp->size + count * tp->xsize;
    else
        request = count * tp->size;

    /* If the count is garbage the request could be huge. */
    if (request > heap->size)
    {
        errno = EINVAL;
        return NULL;
    }

    /* Create an on abort operation to undo this allocation if the base
     * transaction aborts. Create a savepoint to eliminate this record if
     * the allocation fails. */
    if (!nvm_savepoint(heap)) // the heap address is an OK savepoint name
        return NULL; // error, probably hit undo limit
    nvm_free_ctx *ctx = nvm_onabort(nvm_extern_nvm_free_callback.usid);

    /* Begin a nested transaction to do the allocation. This allows the heap
     * and freelist locks to be dropped before returning. */
    {    
        nvm_txbegin(0);    

        /* Get the mutex for the heap so we can examine and modify it */
        nvm_xlock(&heap->heap_mutex);

        /* It is an error if the heap is being deleted or in creation by
         * another transaction. */
        if (heap->inuse && heap->inuse != nvm_txslot())
        {
            nvm_abort();
            errno = EBUSY;
            return 0;
        }

        /* Find an nvm_blk for the allocation if available. This also creates
         * undo for the nvm_blk returned. */
        nvm_blk *nvb = nvm_alloc_blk(heap, request, tp->align);
        if (nvb == NULL)
        {
            nvm_abort();
            errno = ENOMEM;
            nvm_txend(); //#
            goto fail;
        }

        /* The amount consumed is the physical size of the allocated block. */
        nvm_blk *next = nvm_blk_get(&nvb->neighbors.fwrd);    
        size_t consume = (next - nvb) * sizeof(nvm_blk);    
        NVM_UNDO(heap->consumed);
        heap->consumed += consume;
        nvm_flush1(&heap->consumed);

        /* Transactionally store the allocated nvm_blk pointer in the on abort
         * context. If this nested transaction does not commit, undo will clear 
         * the pointer so that the on abort operation will be a no-op. */
        NVM_UNDO(ctx->blk);    
        nvm_blk_set(&ctx->blk, nvb);    
        nvm_flush1(&ctx->blk);    

        /* Mark the newly allocate block as allocated. Undo already generated */
        nvb->allocated = request;    

        /* Add the allocation at the head of  this heap's allocated list.
         * Note that nvm_alloc_blk already created undo for *nvb. */
        void_set(&nvb->ptr, heap);    
        nvm_blk_set(&nvb->group.back, 0);    
        nvm_blk *olh = nvm_blk_get(&heap->list.head); // old listhead
        NVM_UNDO(heap->list);    
        nvm_blk_set(&heap->list.head, nvb);    
        nvm_blk_set(&nvb->group.fwrd, olh);    
        if (olh)
        {
            /* the allocated list is not empty, so point the old head back */
            NVM_UNDO(olh->group.back);    
            nvm_blk_set(&olh->group.back, nvb); // old head has back ptr now 
            nvm_flush1(&olh->group.back);
        }
        else
        {
            /* the allocated list is empty, so now it has one entry. */
            nvm_blk_set(&heap->list.tail, nvb); // this is now the tail 
        }
        nvm_flush(&heap->list, sizeof(heap->list)); //#
        nvm_flush1(nvb); // new nvm_blk completely updated so flush. 

        /* Do the standard initialization of NVM allocated data. It is just
         * after its nvm_blk.*/
        void *ret = nvb + 1;
        nvm_alloc_init(ret, tp, request);

        /* commit the allocation */
        nvm_commit();    
        nvm_txend();    

        /* Return successful allocation. The allocated space immediately follows
         * the nvm_blk describing it. */
        return ret;
    }       


    /* If we fail to allocate, remove the unnecessary on abort undo. This keeps
     * repeated failures from building up undo that is not needed. Note that
     * errno must already be set. */
fail:
    nvm_rollback(heap);
    return NULL;
}
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
        )
{
    /* Verify this was allocated by nvm_alloc. It should be preceeded by
     * an nvm_blk struct. If it is not then it is an invalid pointer. */
    nvm_blk ^nvb = (nvm_blk^)ptr - 1;
    if (!nvm_usid_eq(^(nvm_usid^)nvb, nvm_usidof(nvm_blk)))
    {
        errno = EINVAL;
        return 0;
    }

    /* Return an error if it is already free. */
    if (nvb=>allocated == 0)
    {
        errno = EINVAL;
        return 0;
    }

    /* Find the heap this was allocated from and verify it is intact. */
    nvm_heap ^heap = nvb=>ptr;
    nvm_verify(heap, shapeof(nvm_heap));

    /* It is an error if the heap is being deleted or in creation by
     * another transaction. */
    if (heap=>inuse && heap=>inuse != nvm_txslot())
    {
        nvm_abort();
        errno = EBUSY;
        return 0;
    }

    /* Create an on commit operation to free this block when the current
     * transaction commits. */
    nvm_free_ctx ^ctx = nvm_oncommit(|nvm_free_callback);
    ctx=>blk ~= nvb;

    /* Delete successfully queued */
    return 1;
}
#else
int nvm_free(   
        void *ptr    
        )
{
    /* Verify this was allocated by nvm_alloc. It should be preceeded by
     * an nvm_blk struct. If it is not then it is an invalid pointer. */
    nvm_blk *nvb = (nvm_blk*)ptr - 1;
    if (!nvm_usid_eq(nvb->type_usid, nvm_usidof(nvm_blk)))
    {
        errno = EINVAL;
        return 0;
    }

    /* Return an error if it is already free. */
    if (nvb->allocated == 0)
    {
        errno = EINVAL;
        return 0;
    }

    /* Find the heap this was allocated from and verify it is intact. */
    nvm_heap *heap = nvm_heap_get(&nvb->ptr);
    nvm_verify(heap, shapeof(nvm_heap));

    /* It is an error if the heap is being deleted or in creation by
     * another transaction. */
    if (heap->inuse && heap->inuse != nvm_txslot())
    {
        nvm_abort();
        errno = EBUSY;
        return 0;
    }

    /* Create an on commit operation to free this block when the current
     * transaction commits. */
    nvm_free_ctx * ctx = nvm_oncommit(nvm_extern_nvm_free_callback.usid);
    nvm_blk_set(&ctx->blk, nvb);
    nvm_flush1(&ctx->blk);

    /* Delete successfully queued */
    return 1;
}
#endif //NVM_EXT

/**
 * This is the routine to do the actual freeing of an allocation. It is 
 * called from committing when committing the delete of an object, or from
 * aborting when rolling back the allocation of an object.
 * 
 * @param[in] ctx 
 * Pointer to the context created for the on abort or on commit operation.
 */
#ifdef NVM_EXT
void nvm_free_callback@(nvm_free_ctx ^ctx)
{
    /* Get the nvm_blk */
    nvm_blk ^nvb = ctx=>blk;

    /* if nothing to free, we are done. */
    if (nvb == 0)
        return;

    /* Verify the nvm_blk is not corrupt. */
    nvm_verify(nvb, shapeof(nvm_blk));

    /* Verify the allocated size is reasonable */
    nvm_blk ^next = nvb=>neighbors.fwrd;
    size_t space = (next - nvb - 1) * sizeof(nvm_blk);
    if (nvb=>allocated == 0 || nvb=>allocated > space)
        nvms_corruption("Invalid allocated size at free", nvb, next);

    /* Verify the heap pointer */
    nvm_heap ^heap = (nvm_heap ^)nvb=>ptr;
    nvm_verify(heap, shapeof(nvm_heap));

    /* Zero the space being released before getting any locks. This is 
     * idempotent if reapplied. All pointers to the block should have been
     * cleared so there is no need to zero under any lock. */
    nvm_set(nvb + 1, 0, space);

    /* Transactionally clear the pointer in the context so that this
     * operation is idempotent. There is a small window where this 
     * transaction has committed but the on commit operation is not 
     * deleted. Clearing the pointer makes the on commit operation a 
     * no-op so that this is not a problem */
    ctx=>blk @= 0;

    /* Lock the mutex protecting the heap. */
    nvm_xlock(%heap=>heap_mutex);

    /* Create undo for the entire nvm_blk since it will all change */
    nvm_blk_undo(nvb);

    /* unlink this nvm_blk from the heap allocated list */
    nvm_blk ^fwrd = nvb=>group.fwrd;
    nvm_blk ^back = nvb=>group.back;
    if (fwrd)
    {
        /* Not the tail of the list, so fix the nvm_blk that follows */
        fwrd=>group.back @= back;
    }
    else
    {
        /* Is the tail of the list so fix the list tail and new tail */
        heap=>list.tail @= back;
    }
    if (back)
    {
        /* Not the head of the list, so fix the nvm_blk that preceeds */
        back=>group.fwrd @= fwrd;
    }
    else
    {
        /* Is the head of the list so fix the list head */
        heap=>list.head @= fwrd;
    }

    /* Adjust the space consumed by the heap */
    heap=>consumed @-= space + sizeof(nvm_blk);

    /* Return the block to the freelist possibly merging with other blocks. */
    nvm_free_blk(heap, nvb);
}
#else
void nvm_free_callback(nvm_free_ctx *ctx)
{
    /* Get the nvm_blk */
    nvm_blk *nvb = nvm_blk_get(&ctx->blk);

    /* if nothing to free, we are done. */
    if (nvb == NULL)
        return;

    /* Verify the nvm_blk is not corrupt. */
    nvm_verify(nvb, shapeof(nvm_blk));

    /* Verify the allocated size is reasonable */
    nvm_blk *next = nvm_blk_get(&nvb->neighbors.fwrd);
    size_t space = (next - nvb - 1) * sizeof(nvm_blk);
    if (nvb->allocated == 0 || nvb->allocated > space)
        nvms_corruption("Invalid allocated size at free", nvb, next);

    /* Verify the heap pointer */
    nvm_heap *heap = nvm_heap_get(&nvb->ptr);
    nvm_verify(heap, shapeof(nvm_heap));

    /* Zero the space being released before getting any locks. This is 
     * idempotent if reapplied. All pointers to the block should have been
     * cleared so there is no need to zero under any lock. */
    nvm_set(nvb + 1, 0, space);

    /* Transactionally clear the pointer in the context so that this
     * operation is idempotent. There is a small window where this 
     * transaction has committed but the on commit operation is not 
     * deleted. Clearing the pointer makes the on commit operation a 
     * no-op so that this is not a problem */
    NVM_UNDO(ctx->blk);
    nvm_blk_set(&ctx->blk, NULL);
    nvm_flush1(&ctx->blk);

    /* Lock the mutex protecting the heap. */
    nvm_xlock(&heap->heap_mutex);

    /* Create undo for the entire nvm_blk since it will all change */
    nvm_blk_undo(nvb);

    /* unlink this nvm_blk from the heap allocated list */
    nvm_blk *fwrd = nvm_blk_get(&nvb->group.fwrd);
    nvm_blk *back = nvm_blk_get(&nvb->group.back);
    if (fwrd)
    {
        /* Not the tail of the list, so fix the nvm_blk that follows */
        NVM_UNDO(fwrd->group.back);
        nvm_blk_set(&fwrd->group.back, back);
        nvm_flush1(&fwrd->group.back);
    }
    else
    {
        /* Is the tail of the list so fix the list tail and new tail */
        NVM_UNDO(heap->list.tail);
        nvm_blk_set(&heap->list.tail, back);
        nvm_flush1(&heap->list.tail);
    }
    if (back)
    {
        /* Not the head of the list, so fix the nvm_blk that preceeds */
        NVM_UNDO(back->group.fwrd);
        nvm_blk_set(&back->group.fwrd, fwrd);
        nvm_flush1(&back->group.fwrd);
    }
    else
    {
        /* Is the head of the list so fix the list head */
        NVM_UNDO(heap->list.head);
        nvm_blk_set(&heap->list.head, fwrd);
        nvm_flush1(&heap->list.head);
    }

    /* Adjust the space consumed by the heap */
    NVM_UNDO(heap->consumed);
    heap->consumed -= space + sizeof(nvm_blk);
    nvm_flush1(&heap->consumed);

    /* Return the block to the freelist possibly merging with other blocks. */
    nvm_free_blk(heap, nvb);

    /* flush all the changes to *nvb */
    nvm_flush1(nvb);
}
#endif //NVM_EXT

static void nvm_badfl(const char *msg, int i, int cnt)
{
    printf("CORRUPT FREELIST: %s list %d pos %d\n", msg, i, cnt);
    exit(1);
}
#ifdef NVM_EXT
static void nvm_badbk(const char *msg, nvm_blk ^nvb)
{
    printf("CORRUPT nvm_blk: %s at %p\n", msg, nvb);
    exit(1);
}
#else
static void nvm_badbk(const char *msg, nvm_blk *nvb)
{
    printf("CORRUPT nvm_blk: %s at %p\n", msg, nvb);
    exit(1);
}
#endif //NVM_EXT
/**
 * This a check function to look for
 * inconsistencies in an nvm_heap struct .
 * 
 * @param heap 
 * The nvm_heap to check
 * 
 * @return 
 * Amount of allocated space
 */
#ifdef NVM_EXT
size_t nvm_freelists_check(nvm_heap ^heap)
{
    size_t ret = sizeof(nvm_heap);
    nvm_verify(heap, shapeof(nvm_heap));

    nvm_blk ^base = heap=>first;
    nvm_blk ^end = (nvm_blk ^)((uint8_t^)heap + heap=>size - sizeof(nvm_blk));

    if (base == 0 || ((uint64_t)base & (NVM_ALIGN - 1)) != 0)
    {
        printf("CORRUPT FREELISTS: Invalid base pointer %p\n", base);
        exit(1);
    }

    /* Loop through all the nvm_blk structs in the extent. */
    {
        nvm_blk ^cur = (nvm_blk^)base;
        nvm_blk ^last = 0;
        while (cur)
        {
            if (!NVM_USID_EQ(^(nvm_usid^)cur, nvm_usidof(nvm_blk)))
                nvm_badbk("Bad USID", cur);
            nvm_blk ^fw = cur=>neighbors.fwrd;
            nvm_blk ^bk = cur=>neighbors.back;
            if (bk != last)
                nvm_badbk("Bad back pointer", cur);

            /* get the pointer to heap or freelist */
            void ^ptr = cur=>ptr;

            /* validate allocated space and ptr */
            if (cur=>allocated)
            {
                /* If allocated it must point at heap */
                if (ptr != (void^)heap)
                    nvm_badbk("Allocated block not pointing to heap", cur);
                
                /* accumulate amount allocated */
                ret += (fw - cur) * sizeof(nvm_blk);

                /* If allocated it cannot be the end. */
                if (ptr == 0 || fw == 0)
                    nvm_badbk("Allocated with NULL pointer", cur);

                /* check range on fwrd pointer */
                if (fw < cur + 2 || fw > end)
                    nvm_badbk("Bad fwrd pointer", cur);

                /* allocated, so ptr must point to heap using this freelist */
                nvm_heap ^hp = (nvm_heap ^)ptr;
                if (!NVM_USID_EQ(^(nvm_usid^)hp, nvm_usidof(nvm_heap)))
                    nvm_badbk("Not a pointer to a heap", cur);
                if (hp != heap)
                    nvm_badbk("Heap is for wrong freelist", cur);
            }
            else
            {
                /* Free block so it must point to the freelist in this
                 * nvm_heap that manages this size block. Except if this
                 * is the end of the managed space. */
                if (cur != end)
                {
                    /* check range on fwrd pointer */
                    if (fw < cur + 2 || fw > end)
                        nvm_badbk("Bad fwrd pointer", cur);

                    /* not the end so validate the freelist */
                    nvm_list ^fl = (nvm_list ^)ptr;
                    int i = fl - heap=>free;
                    if (i < 0 || i >= FREELIST_CNT)
                        nvm_badbk("Not a pointer to a freelist", cur);

                    /* verify is it on the correct freelist based on size */
                    size_t space = (fw - cur - 1) * sizeof(nvm_blk);
                    if (space > heap=>free_size[i] ||
                        (i > 0 && space <= heap=>free_size[i - 1]))
                        nvm_badbk("On wrong freelist", cur);
                }
                else
                {
                    /* The last nvm_blk must have a null ptr and fw */
                    if (ptr != 0 || fw != 0)
                        nvm_badbk("Ending nvm_blk not the end", cur);

                    /* last block is considered allocated. */
                    ret += sizeof(nvm_blk);
                }
            }

            /* That block looks good so go to the next */
            last = cur;
            cur = fw;
        }
    }

    /* Loop validating the nodes on each freelist. */
    {
        nvm_list ^fl = heap=>free;
        int i;
        for (i = 0; i < FREELIST_CNT; i++)
        {
            nvm_blk ^cur = fl=>head;
            nvm_blk ^last = 0;
            int cnt = 0;
            while (cur)
            {
                if (cur < base || cur > end)
                    nvm_badfl("Out of range fwrd pointer", i, cnt);
                if (!NVM_USID_EQ(^(nvm_usid^)cur, nvm_usidof(nvm_blk)))
                    nvm_badfl("Bad USID in nvm_blk", i, cnt);
                if (cur=>allocated != 0)
                    nvm_badfl("Allocated on freelist", i, cnt);
                if (cur=>group.back != last)
                    nvm_badfl("Bad back pointer", i, cnt);
                if (cur=>ptr != fl)
                    nvm_badfl("Bad pointer to freelist", i, cnt);
                last = cur;
                cur = cur=>group.fwrd;
                cnt++;
            }
            if (fl=>tail != last)
                nvm_badfl("Tail is not tail", i, cnt);
        }
    }

    return ret;
}
#else
size_t nvm_freelists_check(nvm_heap *heap)
{
    size_t ret = sizeof(nvm_heap);
    nvm_verify(heap, shapeof(nvm_heap));

    nvm_blk *base = nvm_blk_get(&heap->first);
    nvm_blk *end = (nvm_blk *)((uint8_t*)heap + heap->size - sizeof(nvm_blk));

    if (base == 0 || ((uint64_t)base & (NVM_ALIGN - 1)) != 0)
    {
        printf("CORRUPT FREELISTS: Invalid base pointer %p\n", base);
        exit(1);
    }

    /* Loop through all the nvm_blk structs in the extent. */
    {
        nvm_blk *cur = (nvm_blk*)base;
        nvm_blk *last = 0;
        while (cur)
        {
            if (!NVM_USID_EQ(cur->type_usid, nvm_usidof(nvm_blk)))
                nvm_badbk("Bad USID", cur);
            nvm_blk * fw = nvm_blk_get(&cur->neighbors.fwrd);
            nvm_blk * bk = nvm_blk_get(&cur->neighbors.back);
            if (bk != last)
                nvm_badbk("Bad back pointer", cur);

            /* get the pointer to heap or freelist */
            void *ptr = void_get(&cur->ptr);

            /* validate allocated space and ptr */
            if (cur->allocated)
            {
                /* If allocated it must point at heap */
                if (ptr != (void*)heap)
                    nvm_badbk("Allocated block not pointing to heap", cur);
                
                /* accumulate amount allocated */
                ret += (fw - cur) * sizeof(nvm_blk);

                /* If allocated it cannot be the end. */
                if (ptr == NULL || fw == NULL)
                    nvm_badbk("Allocated with NULL pointer", cur);

                /* check range on fwrd pointer */
                if (fw < cur + 2 || fw > end)
                    nvm_badbk("Bad fwrd pointer", cur);

                /* allocated, so ptr must point to heap using this freelist */
                nvm_heap *hp = (nvm_heap *)ptr;
                if (!NVM_USID_EQ(hp->type_usid, nvm_usidof(nvm_heap)))
                    nvm_badbk("Not a pointer to a heap", cur);
                if (hp != heap)
                    nvm_badbk("Heap is for wrong freelist", cur);
            }
            else
            {
                /* Free block so it must point to the freelist in this
                 * nvm_heap that manages this size block. Except if this
                 * is the end of the managed space. */
                if (cur != end)
                {
                    /* check range on fwrd pointer */
                    if (fw < cur + 2 || fw > end)
                        nvm_badbk("Bad fwrd pointer", cur);

                    /* not the end so validate the freelist */
                    nvm_list *fl = (nvm_list *)ptr;
                    int i = fl - heap->free;
                    if (i < 0 || i >= FREELIST_CNT)
                        nvm_badbk("Not a pointer to a freelist", cur);

                    /* verify is it on the correct freelist based on size */
                    size_t space = (fw - cur - 1) * sizeof(nvm_blk);
                    if (space > heap->free_size[i] ||
                        (i > 0 && space <= heap->free_size[i - 1]))
                        nvm_badbk("On wrong freelist", cur);
                }
                else
                {
                    /* The last nvm_blk must have a null ptr and fw */
                    if (ptr != NULL || fw != NULL)
                        nvm_badbk("Ending nvm_blk not the end", cur);

                    /* last block is considered allocated. */
                    ret += sizeof(nvm_blk);
                }
            }

            /* That block looks good so go to the next */
            last = cur;
            cur = fw;
        }
    }

    /* Loop validating the nodes on each freelist. */
    {
        nvm_list *fl = heap->free;
        int i;
        for (i = 0; i < FREELIST_CNT; i++)
        {
            nvm_blk *cur = nvm_blk_get(&fl->head);
            nvm_blk *last = 0;
            int cnt = 0;
            while (cur)
            {
                if (cur < base || cur > end)
                    nvm_badfl("Out of range fwrd pointer", i, cnt);
                if (!NVM_USID_EQ(cur->type_usid, nvm_usidof(nvm_blk)))
                    nvm_badfl("Bad USID in nvm_blk", i, cnt);
                if (cur->allocated != 0)
                    nvm_badfl("Allocated on freelist", i, cnt);
                if (nvm_blk_get(&cur->group.back) != last)
                    nvm_badfl("Bad back pointer", i, cnt);
                if (void_get(&cur->ptr) != fl)
                    nvm_badfl("Bad pointer to freelist", i, cnt);
                last = cur;
                cur = nvm_blk_get(&cur->group.fwrd);
                cnt++;
            }
            if (nvm_blk_get(&fl->tail) != last)
                nvm_badfl("Tail is not tail", i, cnt);
        }
    }

    return ret;
}
#endif //NVM_EXT
/* testing routine to verify consistency */
#ifdef NVM_EXT
void nvm_heap_test(nvm_desc desc, nvm_heap ^heap)
{
    /* Begin a transaction so that we can lock the heap and freelists */
    @ desc { //nvm_txbegin(desc);
        nvm_slock(%heap=>heap_mutex);

        /* do the checks */
        size_t alloc = nvm_freelists_check(heap);

        /* consumed must be the same as allocated */
        if (heap=>consumed != alloc)
        {
            printf("Allocated:%ld does not match consumed:%ld\n", alloc,
                    heap=>consumed);
            exit(1);
        }

        /* commit the transaction to release the locks */
        nvm_commit();
    } //nvm_txend();

}
#else
void nvm_heap_test(nvm_desc desc, nvm_heap *heap)
{
    /* Begin a transaction so that we can lock the heap and freelists */
    nvm_txbegin(desc);
    nvm_slock(&heap->heap_mutex);

    /* do the checks */
    size_t alloc = nvm_freelists_check(heap);

    /* consumed must be the same as allocated */
    if (heap->consumed != alloc)
    {
        printf("Allocated:%ld does not match consumed:%ld\n", alloc,
                heap->consumed);
        exit(1);
    }

    /* commit the transaction to release the locks */
    nvm_commit();
    nvm_txend();

}
#endif //NVM_EXT
/**
 * This initializes one instance of a struct. It is called recursively
 * for embedded structs and unions. It is called from nvm_alloc_init which
 * takes care of zeroing and flushing the entire allocation.
 * 
 * @param addr
 * Address in NVM of the struct to initialize
 * 
 * @param tp
 * The nvm_type describing the struct being allocated
 */
#ifdef NVM_EXT
static void nvm_alloc_init_1(void ^addr, const nvm_type *tp)
{
    /* Loop through all the fields doing special initialization as needed */
    const nvm_field *fld = tp->fields;
    size_t bits = 0; // total amount of data initialized in bits.
    int i;
    while (fld->ftype != nvm_field_null)
    {
        switch (fld->ftype)
        {
        case nvm_field_unsigned:
        case nvm_field_signed:
        case nvm_field_float:
        case nvm_field_pad:
        case nvm_field_char:
            /* These are numeric types where desc is the size in bits. No
             * initialization is needed since all the memory was just forced
             * to zero. */
            bits += fld->desc.bits * fld->count;
            break;

        case nvm_field_usid:
            /* This is a USID. If it is our own then store it, otherwise leave
             * it zero to indicate there is none. */
            if (fld->desc.purp == nvm_usid_self)
            {
                /* The self USID must always be at offset zero. */
                if (bits != 0 || fld->count != 1)
                    nvms_assert_fail("type_usid field is wrong");
                ^(nvm_usid^)addr ~= tp->usid;
            }
            bits += 8 * sizeof(nvm_usid) * fld->count;
            break;

        case nvm_field_pointer:
        case nvm_field_struct_ptr:
        {
            /* This field is a self-relative pointer to another NVM address,
             * unless it is a transient pointer that is absolute. */
            if (bits % (sizeof(void*)*8) != 0)
                nvms_assert_fail("Pointer field not properly aligned");
            if (!fld->tflag) // leave transient pointers NULL
            {
                void ^^srp = (void^^)((uint8_t^)addr + (bits / 8));
                for (i = 0; i < fld->count; i++, srp++)
                {
                    /* Store a null self relative pointer which is actually an
                     * offset of 1 to distinguish it from a pointer to self */
                    ^srp ~= 0; 
                }
            }
            bits += 8 * sizeof(void*) * fld->count;
            break;
        }

        case nvm_field_struct:
        {
            /* This is an embedded struct. Do a recursive call to init. An
             * embedded struct cannot itself be extensible. */
            const nvm_type *etp = fld->desc.pstruct;
            if (etp->xsize != 0)
                nvms_assert_fail("Embedded struct is extensible");
            if (bits % (etp->align * 8) != 0)
                nvms_assert_fail("Embedded struct not properly aligned");
            for (i = 0; i < fld->count; i++, bits += 8 * etp->size)
                nvm_alloc_init_1(((uint8_t^)addr + bits / 8), etp);
            break;
        }

        case nvm_field_union:
        {
            /* This field is an embedded union. Since we cannot know which
             * member type the application will choose, we initialize it as
             * if the first type is chosen. */
            const nvm_union *un = fld->desc.punion;
            const nvm_type *utp = un->types[0];
            if (utp->xsize != 0)
                nvms_assert_fail("Embedded struct is extensible");
            if (bits % (utp->align * 8) != 0)
                nvms_assert_fail("Embedded struct not properly aligned");
            for (i = 0; i < fld->count; i++, bits += 8 * utp->size)
                nvm_alloc_init_1(((uint8_t^)addr + bits / 8), utp);
            break;
        }

        }

        /* go to the next field. */
        fld++;
    }

    /* Initialization of the struct is complete. We should have seen the 
     * total number of bits match the size of the struct. */
    if (tp->size * 8 != bits)
        nvms_assert_fail("Sum of field sizes does not match struct size");
    if (tp->field_cnt != fld - tp->fields)
        nvms_assert_fail("Count of fields does not match fields found");
}
#else
static void nvm_alloc_init_1(void *addr, const nvm_type *tp)
{
    /* Loop through all the fields doing special initialization as needed */
    const nvm_field *fld = tp->fields;
    size_t bits = 0; // total amount of data initialized in bits.
    int i;
    while (fld->ftype != nvm_field_null)
    {
        switch (fld->ftype)
        {
        case nvm_field_unsigned:
        case nvm_field_signed:
        case nvm_field_float:
        case nvm_field_pad:
        case nvm_field_char:
            /* These are numeric types where desc is the size in bits. No
             * initialization is needed since all the memory was just forced
             * to zero. */
            bits += fld->desc.bits * fld->count;
            break;

        case nvm_field_usid:
            /* This is a USID. If it is our own then store it, otherwise leave
             * it zero to indicate there is none. */
            if (fld->desc.purp == nvm_usid_self)
            {
                /* The self USID must always be at offset zero. */
                if (bits != 0 || fld->count != 1)
                    nvms_assert_fail("type_usid field is wrong");
                *(nvm_usid*)addr = tp->usid;
                nvm_flush1(addr);
            }
            bits += 8 * sizeof(nvm_usid) * fld->count;
            break;

        case nvm_field_pointer:
        case nvm_field_struct_ptr:
        {
            /* This field is a self-relative pointer to another NVM address,
             * unless it is a transient pointer that is absolute. */
            if (bits % (sizeof(void*)*8) != 0)
                nvms_assert_fail("Pointer field not properly aligned");
            if (!fld->tflag) // leave transient pointers NULL
            {
                void_srp *srp = (void_srp*)((uint8_t*)addr + (bits / 8));
                for (i = 0; i < fld->count; i++, srp++)
                {
                    void_set(srp, 0);
                    nvm_flush1(srp);
                }
            }
            bits += 8 * sizeof(void_srp) * fld->count;
            break;
        }

        case nvm_field_struct:
        {
            /* This is an embedded struct. Do a recursive call to init. An
             * embedded struct cannot itself be extensible. */
            const nvm_type *etp = fld->desc.pstruct;
            if (etp->xsize != 0)
                nvms_assert_fail("Embedded struct is extensible");
            if (bits % (etp->align * 8) != 0)
                nvms_assert_fail("Embedded struct not properly aligned");
            for (i = 0; i < fld->count; i++, bits += 8 * etp->size)
                nvm_alloc_init_1(((uint8_t*)addr + bits / 8), etp);
            break;
        }

        case nvm_field_union:
        {
            /* This field is an embedded union. Since we cannot know which
             * member type the application will choose, we initialize it as
             * if the first type is chosen. */
            const nvm_union *un = fld->desc.punion;
            const nvm_type *utp = un->types[0];
            if (utp->xsize != 0)
                nvms_assert_fail("Embedded struct is extensible");
            if (bits % (utp->align * 8) != 0)
                nvms_assert_fail("Embedded struct not properly aligned");
            for (i = 0; i < fld->count; i++, bits += 8 * utp->size)
                nvm_alloc_init_1(((uint8_t*)addr + bits / 8), utp);
            break;
        }

        }

        /* go to the next field. */
        fld++;
    }

    /* Initialization of the struct is complete. We should have seen the 
     * total number of bits match the size of the struct. */
    if (tp->size * 8 != bits)
        nvms_assert_fail("Sum of field sizes does not match struct size");
    if (tp->field_cnt != fld - tp->fields)
        nvms_assert_fail("Count of fields does not match fields found");
}
#endif //NVM_EXT
/**
 * When a struct is allocated it is initialized to a null state. All numeric
 * values are zeroed, and all pointers are set to be null, which is an offset
 * of 1.
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
void nvm_alloc_init(void ^addr, const nvm_type *tp, size_t size)
{
    /* Free space is cleared when released. However there is the possibility
     * that a bug damaged the space, so we ensure it is still zero. */
    {
        uint64_t cnt = size / 8;
        uint64_t ^p = addr;
        while (cnt--)
        {
            if (^p)
            {
                ^p ~= 0;
            }
            p++;
        }
        if (size % 8)
        {
            /* it is possible but unlikely that there are an odd number of
             * bytes. */
            cnt = size % 8;
            uint8_t ^p1 = (uint8_t^)p;
            while (cnt--)
                ^p1++ ~= 0;
        }
    }

    /* If this is an extensible struct, then initialization is slightly 
     * different. */
    if (tp->xsize == 0)
    {
        /* This is not an extensible struct. If size is greater than tp->size
         * then that indicates an array of structs was allocated. Thus the 
         * size allocated must be an exact multiple of tp->size. */
        if (size % tp->size != 0)
            nvms_assert_fail("Allocated array size is not a multiple of "
                "the element size.");
        uint8_t ^ ptr = addr;
        int count = size / tp->size;
        while (count--)
        {
            nvm_alloc_init_1(ptr, tp);
            ptr += tp->size;
        }
    }
    else
    {
        /* This is an extensible struct so we init the base struct then loop
         * through the extensible array at the end of the struct.  */
        nvm_alloc_init_1(addr, tp);

        /* Find the nvm_field for the extensible array at the end of the 
         * struct being allocated. Verify it is for a zero length array of
         * embedded structs. */
        const nvm_field *fld = &tp->fields[tp->field_cnt - 1];
        if (fld->count != 0)
            nvms_assert_fail(
                "Extensible struct does not end in zero length array");

        /* Array initialization depends on the field type. */
        switch (fld->ftype)
        {
        default:
            /*  Other types are happy just to be zeroed. Note that it is not
             * possible to have an extensible array of self usid since that
             * must be at offset zero in the struct. */
            break;

        case nvm_field_pointer:
        case nvm_field_struct_ptr:
        {
            /* This is an array of self-relative pointers to NVM. They all
             * need to be set to a null pointer unless transient. */
            if (!fld->tflag)
            {
                void ^^srp = (void^^)((uint8_t^)addr + tp->size);
                int count = (size - tp->size) / sizeof(void*);
                int i;
                for (i = 0; i < count; i++, srp++)
                {
                    /* Store a null self relative pointer which is actually an
                     * offset of 1 to distinguish it from a pointer to self */
                    ^srp ~= 0; 
                }
            }
            break;
        }

        case nvm_field_struct:
        {
            /* This is an array of persistent structs. Call nvm_alloc_init1
             * to initialize each struct. An embedded struct cannot itself be
             * extensible. */
            nvm_type *xtp = fld->desc.pstruct;
            uint8_t ^ptr = (uint8_t^)addr + tp->size;
            int count = (size - tp->size) / xtp->size;
            while (count--)
            {
                nvm_alloc_init_1(ptr, xtp);
                ptr += xtp->size;
            }
            break;
        }

        case nvm_field_union:
        {
            /* This field is an array of unions. Since we cannot know which
             * member type the application will choose, we initialize it as
             * if the first type is chosen. */
            const nvm_union *un = fld->desc.punion;
            const nvm_type *xtp = un->types[0];
            uint8_t ^ptr = (uint8_t^)addr + tp->size;
            int count = (size - tp->size) / un->size;
            while (count--)
            {
                nvm_alloc_init_1(ptr, xtp);
                ptr += un->size;
            }
            break;
        }
        }
    }
}
#else
void nvm_alloc_init(void *addr, const nvm_type *tp, size_t size)
{
    /* Free space is cleared when released. However there is the possibility
     * that a bug damaged the space, so we ensure it is still zero. */
    {
        uint64_t cnt = size / 8;
        uint64_t *p = addr;
        while (cnt--)
        {
            if (*p)
            {
                *p = 0;
                nvm_flush1(p);
            }
            p++;
        }
        if (size % 8)
        {
            /* it is possible but unlikely that there are an odd number of
             * bytes. */
            cnt = size % 8;
            uint8_t *p1 = (uint8_t*)p;
            while (cnt--)
                *p1++ = 0;
            nvm_flush1(p);
        }
    }

    /* If this is an extensible struct, then initialization is slightly 
     * different. */
    if (tp->xsize == 0)
    {
        /* This is not an extensible struct. If size is greater than tp->size
         * then that indicates an array of structs was allocated. Thus the 
         * size allocated must be an exact multiple of tp->size. */
        if (size % tp->size != 0)
            nvms_assert_fail("Allocated array size is not a multiple of "
                "the element size.");
        uint8_t * ptr = addr;
        int count = size / tp->size;
        while (count--)
        {
            nvm_alloc_init_1(ptr, tp);
            ptr += tp->size;
        }
    }
    else
    {
        /* This is an extensible struct so we init the base struct then loop
         * through the extensible array at the end of the struct.  */
        nvm_alloc_init_1(addr, tp);

        /* Find the nvm_field for the extensible array at the end of the 
         * struct being allocated. Verify it is for a zero length array of
         * embedded structs. */
        const nvm_field *fld = &tp->fields[tp->field_cnt - 1];
        if (fld->count != 0)
            nvms_assert_fail(
                "Extensible struct does not end in zero length array");

        /* Array initialization depends on the field type. */
        switch (fld->ftype)
        {
        default:
            /*  other types are happy just to be zeroed. */
            break;

        case nvm_field_pointer:
        case nvm_field_struct_ptr:
        {
            /* This is an array of self-relative pointers to NVM. They all
             * need to be set to a null pointer unless transient. */
            if (!fld->tflag) 
            {
                void_srp * srp = (void_srp*)((uint8_t*)addr + tp->size);
                int count = (size - tp->size) / sizeof(void_srp);
                int i;
                for (i = 0; i < count; i++, srp++)
                {
                    void_set(srp, 0);
                    nvm_flush1(srp);
                }
            }
            break;
        }

        case nvm_field_struct:
        {
            /* This is an array of persistent structs. Call nvm_alloc_init1
             * to initialize each struct. An embedded struct cannot itself be
             * extensible. */
            nvm_type *xtp = fld->desc.pstruct;
            uint8_t *ptr = (uint8_t*)addr + tp->size;
            int count = (size - tp->size) / xtp->size;
            while (count--)
            {
                nvm_alloc_init_1(ptr, xtp);
                ptr += xtp->size;
            }
            break;
        }

        case nvm_field_union:
        {
            /* This field is an array of unions. Since we cannot know which
             * member type the application will choose, we initialize it as
             * if the first type is chosen. */
            const nvm_union *un = fld->desc.punion;
            const nvm_type *xtp = un->types[0];
            uint8_t *ptr = (uint8_t*)addr + tp->size;
            int count = (size - tp->size) / un->size;
            while (count--)
            {
                nvm_alloc_init_1(ptr, xtp);
                ptr += un->size;
            }
            break;
        }
        }
    }
}
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
size_t nvm_init_struct(void ^addr, const nvm_type *tp, int count)
{
    /* Calculate the number of bytes to initialize. */
    size_t size = tp->size;
    if (tp->xsize)
        size += count * tp->xsize;
    else
        size *= count;

    /* call standard initializer */
    nvm_alloc_init(addr, tp, size);
    return size;
}
#else
size_t nvm_init_struct(void *addr, const nvm_type *tp, int count)
{
    /* Calculate the number of bytes to initialize. */
    size_t size = tp->size;
    if (tp->xsize)
        size += count * tp->xsize;
    else
        size *= count;

    /* call standard initializer */
    nvm_alloc_init(addr, tp, size);
    return size;
}
#endif //NVM_EXT
/**
 * This creates undo to restore an nvm_blk on abort. It is important to 
 * not store the USID contiguously in the undo since a region browser
 * can scan NVM looking for that 16 byte USID. If it encountered
 * one in undo it would consider it to be a corrupt nvm_blk.
 * 
 * @param nvb 
 * Pointer to the nvm_blk to restore on abort.
 */
#ifdef NVM_EXT
static void nvm_blk_undo@(nvm_blk ^nvb)
{
    uint64_t ^p = (uint64_t^)nvb;

    /* Save the first 8 bytes in a separate undo record from the rest of the
     * nvm_blk. This ensures the USID is broken up. */
    nvm_undo(p, sizeof(uint64_t));
    nvm_undo(p + 1, sizeof(nvm_blk) - sizeof(uint64_t));
}
#else
static void nvm_blk_undo(nvm_blk *nvb)
{
    uint64_t *p = (uint64_t*)nvb;

    /* Save the first 8 bytes in a separate undo record from the rest of the
     * nvm_blk. This ensures the USID is broken up. */
    nvm_undo(p, sizeof(uint64_t));
    nvm_undo(p + 1, sizeof(nvm_blk) - sizeof(uint64_t));
}
#endif //NVM_EXT
/**
 * Link an nvm_blk onto a freelist. The caller takes care of creating
 * nvm_blk undo before this is called.
 * @param nvb
 * The nvm_blk to add to a freelist
 */
#ifdef NVM_EXT
static void nvm_freelist_link@(nvm_heap ^heap, nvm_blk ^nvb)
{
    /* determine the amount of free space in bytes, not including the nvm_blk */
    size_t sz = ((nvb=>neighbors.fwrd - nvb) - 1) * sizeof(nvm_blk);

    /* Pick the free space list to add nvm_blk to based on size. */
    nvm_list ^fl = heap=>free; // array of free lists
    int i;
    for (i = 0; sz > heap=>free_size[i]; i++, fl++)
        if (i >= FREELIST_CNT - 1)
            nvms_corruption("Missing max freelist size", heap=>free, heap);

    /* Put it on the head of the free list. Note that this maybe the same list
     * we just removed another nvm_blk from. In that case applying the undo
     * created here will be overwritten by applying the previously
     * written undo. */
    nvm_blk ^head = fl=>head;
    if (head != 0)
    {
        head=>group.back @= nvb;
        fl=>head @= nvb;
        nvb=>group.fwrd ~= head;
        nvb=>group.back ~= 0;
    }
    else
    {
        /* The freelist is empty so this becomes the only entry */
        fl=>head @= nvb;
        fl=>tail @= nvb;
        nvb=>group.fwrd ~= 0;
        nvb=>group.back ~= 0;
    }

    /* Have the nvm_blk point at the correct freelist, and mark as free. */
    nvb=>ptr ~= fl;
    nvb=>allocated ~= 0;
}
#else
static void nvm_freelist_link(nvm_heap *heap, nvm_blk *nvb)
{
    /* determine the amount of free space in bytes, not including the nvm_blk */
    size_t sz = ((nvm_blk_get(&nvb->neighbors.fwrd) - nvb) - 1) *
            sizeof(nvm_blk);

    /* Pick the free space list to add nvm_blk to based on size. */
    nvm_list *fl = heap->free; // array of free lists
    int i;
    for (i = 0; sz > heap->free_size[i]; i++, fl++)
        if (i >= FREELIST_CNT - 1)
            nvms_corruption("Missing max freelist size", heap, fl);

    /* Put it on the head of the free list. Note that this maybe the same list
     * we just removed another nvm_blk from. In that case applying the undo
     * created here will be overwritten by applying the previously
     * written undo. */
    nvm_blk *head = nvm_blk_get(&fl->head);
    if (head != NULL)
    {
        NVM_UNDO(head->group.back);
        nvm_blk_set(&head->group.back, nvb);
        nvm_flush1(&head->group.back);
        NVM_UNDO(fl->head);
        nvm_blk_set(&fl->head, nvb);
        nvm_flush1(&fl->head);
        nvm_blk_set(&nvb->group.fwrd, head);
        nvm_blk_set(&nvb->group.back, NULL);
    }
    else
    {
        /* The freelist is empty so this becomes the only entry */
        NVM_UNDO(*fl);
        nvm_blk_set(&fl->head, nvb);
        nvm_blk_set(&fl->tail, nvb);
        nvm_flush(fl, sizeof(fl));
        nvm_blk_set(&nvb->group.fwrd, NULL);
        nvm_blk_set(&nvb->group.back, NULL);
    }

    /* Have the nvm_blk point at the correct freelist, and mark as free. */
    void_set(&nvb->ptr, fl);
    nvb->allocated = 0;
}
#endif //NVM_EXT
/**
 * Unlink an nvm_blk from its freelist. The caller takes care of creating
 * nvm_blk undo before this is called.
 * @param nvb
 * The nvm_blk to remove from a freelist
 */
#ifdef NVM_EXT
static void nvm_freelist_unlink@(nvm_blk ^nvb)
{
    /* Get the nvm_blk's before and after it on its freelist. */
    nvm_blk ^fw = nvb=>group.fwrd;
    nvm_blk ^bk = nvb=>group.back;

    /* Unlink from the following nvm_blk. */
    if (fw != 0)
    {
        /* not the tail of its free list  so link back pointer of the
         * next block around this block. */
        fw=>group.back @= bk;
    }
    else
    {
        /* Is the tail of the its freelist, so there is a new tail now. 
         * First verify the freelist agrees this is the tail. */
        nvm_list ^fl = nvb=>ptr;
        if (fl=>tail != nvb)
            nvms_corruption("Block says it is the tail, but the freelist "
                "does not agree", nvb, fl);

        /* set tail pointer */
        fl=>tail @= bk;
    }

    /* Unlink from the prevoius nvm_blk */
    if (bk != 0)
    {
        /* not the tail of its free list  so link fwrd pointer of the
         * previous block around this block. */
        bk=>group.fwrd @= fw;
    }
    else
    {
        /* Is the head of the its freelist, so there is a new head now. 
         * First verify the freelist agrees this is the tail. */
        nvm_list ^fl = nvb=>ptr;
        if (fl=>head != nvb)
            nvms_corruption("Block says it is the head, but the freelist "
                "does not agree", nvb, fl);

        /* set head pointer */
        fl=>head @= fw;
    }

}
#else
static void nvm_freelist_unlink(nvm_blk *nvb)
{
    /* Get the nvm_blk's before and after it on its freelist. */
    nvm_blk *fw = nvm_blk_get(&nvb->group.fwrd);
    nvm_blk *bk = nvm_blk_get(&nvb->group.back);

    /* Unlink from the following nvm_blk. */
    if (fw != NULL)
    {
        /* not the tail of its free list  so link back pointer of the
         * next block around this block. */
        NVM_UNDO(fw->group.back);
        nvm_blk_set(&fw->group.back, bk);
        nvm_flush1(&fw->group.back);
    }
    else
    {
        /* Is the tail of the its freelist, so there is a new tail now. 
         * First verify the freelist agrees this is the tail. */
        nvm_list *fl = void_get(&nvb->ptr);
        if (nvm_blk_get(&fl->tail) != nvb)
            nvms_corruption("Block says it is the tail, but the freelist "
                "does not agree", nvb, fl);

        /* set tail pointer */
        NVM_UNDO(fl->tail);
        nvm_blk_set(&fl->tail, bk);
        nvm_flush1(&fl->tail);
    }

    /* Unlink from the prevoius nvm_blk */
    if (bk != NULL)
    {
        /* not the tail of its free list  so link fwrd pointer of the
         * previous block around this block. */
        NVM_UNDO(bk->group.fwrd);
        nvm_blk_set(&bk->group.fwrd, fw);
        nvm_flush1(&bk->group.fwrd);
    }
    else
    {
        /* Is the head of the its freelist, so there is a new head now. 
         * First verify the freelist agrees this is the tail. */
        nvm_list *fl = void_get(&nvb->ptr);
        if (nvm_blk_get(&fl->head) != nvb)
            nvms_corruption("Block says it is the head, but the freelist "
                "does not agree", nvb, fl);

        /* set head pointer */
        NVM_UNDO(fl->head);
        nvm_blk_set(&fl->head, fw);
        nvm_flush1(&fl->head);
    }

}
#endif //NVM_EXT

/**
 * This finds an available block for the indicated allocation size and 
 * alignment. The return value is a pointer to an nvm_blk for a chunk that is 
 * large enough to hold the desired allocation. The nvm_blk is no longer on a
 * freelist, but is not on any allocated list.
 * 
 * The transaction contains undo to restore the nvm_blk to its original
 * contents as well as the freelist. Thus the caller may modify other
 * fields in the nvm_blk without additional undo. The allocated space will
 * be zeroed on rollback so no undo is needed for initializing the space
 * allocated.
 * 
 * The transaction also holds a lock on the mutex protecting the 
 * free list so that the allocation can be committed or rolled back.
 * 
 * @param request
 * The minimum amount of space needed.
 * 
 * @param align
 * The required alignment for the allocated space.
 * 
 * @return
 * Pointer to an nvm_blk with enough space and alignment, or NULL if no space
 * is available. 
 */
#ifdef NVM_EXT
static nvm_blk ^nvm_alloc_blk@(nvm_heap ^heap, size_t request, uint32_t align)
{
    /* Currently the largest supported alignment is 64. There may be a use case
     * for larger alignment requirements. If so this would need to change. */
    if (align > NVM_ALIGN)
        nvms_assert_fail("Unsupported alignment");

    /* verify heap is valid */
    nvm_verify(heap, shapeof(nvm_heap));

    /* find the free list with the smallest blocks that are big enough, and
     * contain some blocks. */
    nvm_list ^fl = heap=>free; // array of free lists
    int i;
    for (i = 0; request > heap=>free_size[i] || fl=>head == 0;
            i++, fl++)
        /* out of space if nothing found. */
        if (i >= FREELIST_CNT - 1)
        {
            return 0;
        }

    /* Pick a block to allocate from. For now this just takes the first block
     * that is large enough. If there is none large enough look to free lists 
     * with larger block sizes.
     * 
     * I am sure there is a better algorithm, but I do not know what it is. */
    nvm_blk ^nvb = fl=>head;
    size_t space;
    while (1)
    {
        /* Check for corruption. Must point back at its freelist and be free. */
        nvm_verify(nvb, shapeof(nvm_blk));
        if (nvb=>ptr != fl)
            nvms_corruption("Bad pointer in free blk", nvb, fl);
        if (nvb=>allocated != 0)
            nvms_corruption("Allocated  block in freelist", nvb, fl);

        /* Determine the free space in this block. */
        space = ((nvb=>neighbors.fwrd - nvb) - 1) * sizeof(^nvb);

        /* If it is big enough use it */
        if (space >= request)
            break;

        /* go to the next entry, if there is none find a free list with
         * bigger blocks */
        nvb = nvb=>group.fwrd;
        if (nvb == 0)
        {
            /* look for the next freelist with an entry. Note that i and fl
             * were left from previous scan of lists. */
            for (i++, fl++; fl=>head == 0; i++, fl++)
                /* out of space if nothing found. */
                if (i >= FREELIST_CNT - 1)
                {
                    return 0;
                }

            /* all blocks on this list are bigger than what we need so just
             * take the head of the list. */
            nvb = fl=>head;

            /* check for corruption */
            nvm_verify(nvb, shapeof(nvm_blk));

            /* determine available space */
            space = ((nvb=>neighbors.fwrd - nvb) - 1) * sizeof(^nvb);
            if (space < request) // ensure block size is reasonable
                nvms_corruption("Free block too small for its freelist",
                    fl, nvb);
            break;
        }
    }

    /* Determine how much spare space is in this NVblk. Round down for nvm_blk
     * alignment. Note that spare includes space that would be consumed by a
     * new nvm_blk if constructed. */
    size_t spare = (space - request) & ~(NVM_ALIGN - 1);

    /* Create undo for the nvm_blk we are going to modify. This allows 
     * non-transactional stores for updating the nvm_blk. */
    nvm_blk_undo(nvb);

    /* if the spare space is too small then just waste it to avoid
     * lots of small useless free blocks. */
    nvm_blk ^new_nvb = 0;
    if (spare >= heap=>free_size[0])
    {
        /* make a new NVblk to keep the spare space */
        new_nvb = nvb=>neighbors.fwrd - (spare / sizeof(nvm_blk));
    }

    /* Remove the nmv_blk from the free list. Does not change *nvb links.
     * First follow the forward group pointer to link it around the nvm_blk
     * we are removing. Then follow the back pointer to link around. */
    nvm_freelist_unlink(nvb);

    /* If the spare space is big enough to make into a new nvm_blk then
     * create the nvm_blk and add it to the correct freelist. */
    if (new_nvb)
    {
        nvm_blk_undo(new_nvb); // Should be zero, but create undo anyway
        ^(nvm_usid^)new_nvb ~= nvm_usidof(nvm_blk);

        /* link to neighboring nvm_blk's */
        nvm_blk ^next = nvb=>neighbors.fwrd;
        new_nvb=>neighbors.fwrd ~= next;
        new_nvb=>neighbors.back ~= nvb;
        next=>neighbors.back @= new_nvb;
        nvb=>neighbors.fwrd ~= new_nvb; // undo OK

        /* put the spare space back on a freelist. */
        nvm_freelist_link(heap, new_nvb);
    }

    /* Caller will flush all of *nvb when done with it. */
    return nvb;
}
#else
static nvm_blk *nvm_alloc_blk(nvm_heap *heap, size_t request, uint32_t align)
{
    /* Currently the largest supported alignment is 64. There may be a use case
     * for larger alignment requirements. If so this would need to change. */
    if (align > NVM_ALIGN)
        nvms_assert_fail("Unsupported alignment");

    /* verify heap is valid */
    nvm_verify(heap, shapeof(nvm_heap));

    /* find the free list with the smallest blocks that are big enough, and
     * contain some blocks. */
    nvm_list * fl = heap->free; // array of free lists
    int i;
    for (i = 0; request > heap->free_size[i] || nvm_blk_get(&fl->head) == NULL;
            i++, fl++)
        /* out of space if nothing found. */
        if (i >= FREELIST_CNT - 1)
        {
            return NULL;
        }

    /* Pick a block to allocate from. For now this just takes the first block
     * that is large enough. If there is none large enough look to free lists 
     * with larger block sizes.
     * 
     * I am sure there is a better algorithm, but I do not know what it is. */
    nvm_blk *nvb = nvm_blk_get(&fl->head);
    size_t space;
    while (1)
    {
        /* Check for corruption. Must point back at its freelist and be free. */
        nvm_verify(nvb, shapeof(nvm_blk));
        if (void_get(&nvb->ptr) != fl)
            nvms_corruption("Bad pointer in free blk", nvb, fl);
        if (nvb->allocated != 0)
            nvms_corruption("Allocated  block in freelist", nvb, fl);

        /* Determine the free space in this block. */
        space = (nvm_blk_get(&nvb->neighbors.fwrd) - nvb - 1) * sizeof(*nvb);

        /* If it is big enough use it */
        if (space >= request)
            break;

        /* go to the next entry, if there is none find a free list with
         * bigger blocks */
        nvb = nvm_blk_get(&nvb->group.fwrd);
        if (nvb == NULL)
        {
            /* look for the next freelist with an entry. Note that i and fl
             * were left from previous scan of lists. */
            for (i++, fl++; nvm_blk_get(&fl->head) == NULL; i++, fl++)
                /* out of space if nothing found. */
                if (i >= FREELIST_CNT - 1)
                {
                    return NULL;
                }

            /* all blocks on this list are bigger than what we need so just
             * take the head of the list. */
            nvb = nvm_blk_get(&fl->head);

            /* check for corruption */
            nvm_verify(nvb, shapeof(nvm_blk));

            /* determine available space */
            space = (nvm_blk_get(&nvb->neighbors.fwrd) - nvb - 1) *
                    sizeof(*nvb);
            if (space < request) // ensure block size is reasonable
                nvms_corruption("Free block too small for its freelist",
                    fl, nvb);
            break;
        }
    }

    /* Determine how much spare space is in this NVblk. Round down for nvm_blk
     * alignment. Note that spare includes space that would be consumed by a
     * new nvm_blk if constructed. */
    size_t spare = (space - request) & ~(NVM_ALIGN - 1);

    /* Create undo for the nvm_blk we are going to modify. This allows 
     * non-transactional stores for updating the nvm_blk. */
    nvm_blk_undo(nvb);

    /* if the spare space is too small then just waste it to avoid
     * lots of small useless free blocks. */
    nvm_blk *new_nvb = 0;
    if (spare >= heap->free_size[0])
    {
        /* make a new NVblk to keep the spare space */
        new_nvb = nvm_blk_get(&nvb->neighbors.fwrd) - (spare / sizeof(nvm_blk));
    }

    /* Remove the nmv_blk from the free list. Does not change *nvb links.
     * First follow the forward group pointer to link it around the nvm_blk
     * we are removing. Then follow the back pointer to link around. */
    nvm_freelist_unlink(nvb);

    /* If the spare space is big enough to make into a new nvm_blk then
     * create the nvm_blk and add it to the correct freelist. */
    if (new_nvb)
    {
        nvm_blk_undo(new_nvb); // Should be zero, but create undo anyway
        new_nvb->type_usid = nvm_usidof(nvm_blk);

        /* link to neighboring nvm_blk's */
        nvm_blk *next = nvm_blk_get(&nvb->neighbors.fwrd);
        nvm_blk_set(&new_nvb->neighbors.fwrd, next);
        nvm_blk_set(&new_nvb->neighbors.back, nvb);
        NVM_UNDO(next->neighbors.back);
        nvm_blk_set(&next->neighbors.back, new_nvb);
        nvm_flush1(&next->neighbors.back);
        nvm_blk_set(&nvb->neighbors.fwrd, new_nvb); // undo and flush OK

        /* put the spare space back on a freelist. */
        nvm_freelist_link(heap, new_nvb);
        nvm_flush1(new_nvb);
    }

    /* Caller will flush all of *nvb when done with it. */
    return nvb;
}
#endif //NVM_EXT
/**
 * Return a block of memory to the free list. If it has neighboring 
 * nvm_blk blocks that are free, they will be merged into a single
 * free block.
 * 
 * On return the transaction will contain undo to rollback the 
 * deallocation. The caller is expected to have stored undo for
 * the entire nvm_blk, and to have removed the nvm_blk from the heap
 * allocated list. Thus it is not on any freelist.
 * 
 * @param nvb The nvm_blk that is now free.
 */
#ifdef NVM_EXT
static void nvm_free_blk@(nvm_heap ^heap, nvm_blk ^nvb)
{
    /* Verify the heap is valid */
    nvm_verify(heap, shapeof(nvm_heap));

    /* See if the block following this one is free, and not the nvm_blk
     * at the end of the memory managed by this nvm_heap. Note that an
     * allocated block will always have a forward neighbor. */
    nvm_blk ^nvb_next = nvb=>neighbors.fwrd;
    nvm_verify(nvb_next, shapeof(nvm_blk));
    if (nvb_next=>allocated == 0 // if free
        && nvb_next=>neighbors.fwrd != 0) // not end of extent
    {
        /* Merge the block being freed with the free block that follows it. */
        nvm_blk_undo(nvb_next); // undo for entire next block

        /* Link the block being freed with the one after the block being
         * merged so that there is only one block. */
        nvm_blk ^nextnext = nvb_next=>neighbors.fwrd;
        nvb=>neighbors.fwrd ~= nextnext;
        nextnext=>neighbors.back @= nvb;

        /* unlink the nvm_blk being merged in from its free list. */
        nvm_freelist_unlink(nvb_next);

        /* the nvm_blk merged in is now free space so clear it. */
        nvm_blk_clr(nvb_next);

        /* We now have a different nvb_next */
        nvb_next = nextnext;
    }

    /* See if there is a block preceeding this one and it is free. If so 
     * merge them. */
    nvm_blk ^nvb_prev = nvb=>neighbors.back;
    nvm_verify(nvb_prev, shapeof(nvm_blk));
    if (nvb_prev && nvb_prev=>allocated == 0)
    {
        /* merge preceeding block with the block being released. First 
         * create undo for the block. */
        nvm_blk_undo(nvb_prev);

        /* Merge the block  being released with the preceeding block */
        nvb_prev=>neighbors.fwrd ~= nvb_next;
        nvb_next=>neighbors.back @= nvb_prev;

        /* Unlink the nvm_blk being merged in from its free list. It will most
         * likely be put back into a different freelist. */
        nvm_freelist_unlink(nvb_prev);

        /* the nvm_blk being freed is now free space so clear it. */
        nvm_blk_clr(nvb);

        /* we now have a different nvm_blk to put on the freelsit */
        nvb = nvb_prev;
    }

    /* We now have in nvb the address of an nvm_blk that is free, but not on
     * a free list. Put it on the right free list. */
    nvm_freelist_link(heap, nvb);
}
#else
static void nvm_free_blk(nvm_heap *heap, nvm_blk *nvb)
{
    /* Verify the heap is valid */
    nvm_verify(heap, shapeof(nvm_heap));

    /* see if the block following this one is free, and not the nvm_blk
     * at the end of the memory managed by this nvm_heap. Note that an
     * allocated block will always have a forward neighbor. */
    nvm_blk *nvb_next = nvm_blk_get(&nvb->neighbors.fwrd);
    nvm_verify(nvb_next, shapeof(nvm_blk));
    if (nvb_next->allocated == 0 // if free
        && nvm_blk_get(&nvb_next->neighbors.fwrd) != NULL) // not end of extent
    {
        /* Merge the block being freed with the free block that follows it. */
        nvm_blk_undo(nvb_next); // undo for entire next block

        /* Link the block being freed with the one after the block being
         * merged so that there is only one block. */
        nvm_blk *nextnext = nvm_blk_get(&nvb_next->neighbors.fwrd);
        nvm_blk_set(&nvb->neighbors.fwrd, nextnext);
        NVM_UNDO(nextnext->neighbors.back);
        nvm_blk_set(&nextnext->neighbors.back, nvb);
        nvm_flush1(&nextnext->neighbors.back);

        /* unlink the nvm_blk being merged in from its free list. */
        nvm_freelist_unlink(nvb_next);

        /* the nvm_blk merged in is now free space so clear it. */
        nvm_blk_clr(nvb_next);

        /* We now have a different nvb_next */
        nvb_next = nextnext;
    }

    /* See if there is a block preceeding this one and it is free. If so 
     * merge them. */
    nvm_blk *nvb_prev = nvm_blk_get(&nvb->neighbors.back);
    nvm_verify(nvb_prev, shapeof(nvm_blk));
    if (nvb_prev != NULL && nvb_prev->allocated == 0 )
    {
        /* merge preceeding block with the block being released. First 
         * create undo for the block. */
        nvm_blk_undo(nvb_prev);

        /* Merge the block  being released with the preceeding block */
        nvm_blk_set(&nvb_prev->neighbors.fwrd, nvb_next);
        NVM_UNDO(nvb_next->neighbors.back);
        nvm_blk_set(&nvb_next->neighbors.back, nvb_prev);
        nvm_flush1(&nvb_next->neighbors.back);

        /* Unlink the nvm_blk being merged in from its free list. It will most
         * likely be put back into a different freelist. */
        nvm_freelist_unlink(nvb_prev);

        /* the nvm_blk being freed is now free space so clear it. */
        nvm_blk_clr(nvb);

        /* we now have a different nvm_blk to put on the freelsit */
        nvb = nvb_prev;
    }

    /* We now have in nvb the address of an nvm_blk that is free, but not on
     * a free list. Put it on the right free list. We need to flush since this
     * might not be the same nvm_blk the caller passed in. */
    nvm_freelist_link(heap, nvb);
    nvm_flush1(nvb);
}
#endif //NVM_EXT
