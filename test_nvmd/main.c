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
      main.c - A silly application to exercise the NVM API.

    DESCRIPTION\n
      This is a grossly inadequate test application for exercising the NVM
      library.
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <setjmp.h>
#include <string.h>
#include <pthread.h>
#include "nvm.h"
#include "nvm_region0.h"
#include "nvm_heap0.h"

/*
 * These are the parameters that control how the test is run.
 */

/* The virtual address to attach the main region at */
#define attch ((uint8_t*)(1024 * 1024 * 1024 * 128LL))

/* The virtual address space consumed by the main region */
const size_t vspace = 1024 * 1024 * 1024;

/* the physical size of the base extent */
const size_t pspace = 10 * 1024 * 1024;

/* the physical size of the application managed extent of pointers */
const size_t ptrspace = 1024 * 1024;

/* the offset into the region for the application managed extent of pointers */
const size_t xoff = 128 * 1024 * 1024;

/* the number of pointers in the application managed extent of pointers */
#define ptrs 128

/* The virtual and physical space for a log region */
const size_t logspace = 2*1024*1024;

/* the virtual address of log region 0 */
#define logbase (attch + 2*vspace)

/* the number of log records in a log region */
#define logsz  1024

/* Every allocation from the heap extent is for a random number between one
 * and maxdata of long integers. This is chosen to be large enough that
 * the heap will run out of space at times. */
#define maxdata  ((pspace * 18) / (ptrs * 10 * 8))

/* This is the number of child processes that will be forked and possibly
 * killed. Choose zero to do one run in the initial process. This is easier
 * to debug. Choose 100,000 to let the program run for about a week. */
//#define FORKS (-1) // no children, loop forever
//#define FORKS (0) // no children, one run
#define FORKS (100000) // run forever

/* The parent process sleeps an interval of 1 to maxsleep seconds before
 * killing the child process. This should be tuned so that most children
 * are killed but a significant number complete without being killed. */
//const int maxsleep = 14;
const int maxsleep = 4;

/* loops is the number of times each thread goes through the deallocate
 * then allocate loop before doing a normal exit. */
//const int loops = 50000;
const int loops = 10240;

/* After every loop a thread takes a one in longjump chance of doing a
 * long jump to exit early. */
//const int longjump = (50000*2);
const int longjump = (10240*2);

/* Each run spawns THREADS threads to simultaneously run through the
 * deallocate/allocate loop. Setting threads to 1 makes debugging easier. */
#define THREADS 4
//#define THREADS 1

/*
 * The test repeatedly allocates and deallocates random sized instances
 * of the persistent struct branch. They are allocated from the heap extent.
 */
#ifdef NVM_EXT

/*
 * Current version of branch struct
 */
persistent struct branch
USID("a063 6cdd 76e7 8b42 0c32 eeab d43c 829c") 
{
    uint64_t version; // this is the version number changed by upgrade
    unsigned long data[0];
};
typedef persistent struct branch branch;

/*
 * Second version of branch struct
 */
typedef persistent struct branch_v2 branch_v2;
int upgrade_br_v2@(branch_v2 ^b2); // function to upgrade from v2 to current

persistent struct branch_v2
USID("f8e7 5535 672d 1d12 f534 4786 81e2 1a8c")
upgrade(branch, upgrade_br_v2)
{
    uint64_t version2; // this is the version number changed by upgrade
    unsigned long data[0];
};

/*
 * Upgrade a branch_v2 to branch
 */
int upgrade_br_v2@(branch_v2 ^b2)
{
    nvm_verify(b2, shapeof(branch_v2));
    if (b2=>version2 && b2=>version2 != 2)
        return 0; // fail due to corruption
    branch ^b = (branch^)b2;
    nvm_verify(b, shapeof(branch));
    b=>version @= 3;
    return 1; // success
}

/*
 * Original version of branch struct
 */
typedef persistent struct branch_v1 branch_v1;
int upgrade_br_v1@(branch_v1 ^b1); // function to upgrade from v1 to v2

persistent struct branch_v1
USID("b0a2 29e0 449d 54cb 8bdd 5c52 356e f016")
upgrade(branch_v2, upgrade_br_v1)
{
    uint64_t version1; // this is the version number changed by upgrade
    unsigned long data[0];
};

/*
 * Upgrade a branch_v1 to branch_v2
 */
int upgrade_br_v1@(branch_v1 ^b1)
{
    nvm_verify(b1, shapeof(branch_v1));
    if (b1=>version1 && b1=>version1 != 1)
        return 0; // fail due to corruption
    branch_v2 ^b2 = (branch_v2^)b1;
    nvm_verify(b2, shapeof(branch_v2));
    b2=>version2 @= 2;
    return 1; // success
}
#else
extern const nvm_type nvm_type_branch;
extern const nvm_type nvm_type_branch_v1;
extern const nvm_type nvm_type_branch_v2;
struct branch
{
    nvm_usid type_usid;
    uint64_t version; // this is the version number changed by upgrade, it is 3
    uint64_t data[0];
};
typedef struct branch branch;
NVM_SRP2(struct branch, branch)
NVM_SRP2(branch_srp, branchArray)

/* These are pretend upgradeable versions of branch used for exercising the
 * upgrade code. */
struct branch_v1
{
    nvm_usid type_usid;
    uint64_t version1; // contains 1
    uint64_t data[0];
};
typedef struct branch_v1 branch_v1;
struct branch_v2
{
    nvm_usid type_usid;
    uint64_t version2; //contains 2
    uint64_t data[0];
};
typedef struct branch_v2 branch_v2;

/*
 * Upgrade a branch_v1 to branch_v2
 */
int upgrade_br_v1(branch_v1 *b1)
{
    nvm_verify(b1, shapeof(branch_v1));
    if (b1->version1 && b1->version1 != 1)
        return 0; // fail due to corruption
    branch_v2 *b2 = (branch_v2*)b1;
    nvm_verify(b2, shapeof(branch_v2));
    NVM_TXSTORE(b2->version2, 2);
    return 1; // success
}

/*
 * Upgrade a branch_v2 to branch
 */
int upgrade_br_v2(branch_v2 *b2)
{
    nvm_verify(b2, shapeof(branch_v2));
    if (b2->version2 && b2->version2 != 2)
        return 0; // fail due to corruption
    branch *b = (branch*)b2;
    nvm_verify(b, shapeof(branch));
    NVM_TXSTORE(b->version, 3);
    return 1; // success
}

/* The type descriptions for all 3 versions of branch. */
const nvm_type nvm_type_branch = {
    {0xa0636cdd76e78b42, 0x0c32eeabd43c829c}, // type_usid
    "branch", // name
    "Test branch stuct", // tag
    sizeof(branch), //size
    8, // xsize
    0, // version
    16, // align
    0, // upgrade
    0, // uptype
    3, // field_cnt
    { // field[]]
        {nvm_field_usid, 0, 1, (void*)nvm_usid_self, "type_usid"},
        {nvm_field_unsigned, 0, 1, (void*)64, "version"},
        {nvm_field_unsigned, 0, 0, (void*)64, "data"},
        {0, 0, 0, 0, 0}
    }
};
const nvm_type nvm_type_branch_v2 = {
    {0xf8e75535672d1d12, 0xf534478681e21a8c}, // type_usid
    "branch_v2", // name
    "Test branch_v2 stuct", // tag
    sizeof(branch), //size
    8, // xsize
    0, // version
    16, // align
    &upgrade_br_v2, // upgrade
    &nvm_type_branch, // uptype
    3, // field_cnt
    { // field[]]
        {nvm_field_usid, 0, 1, (void*)nvm_usid_self, "type_usid"},
        {nvm_field_unsigned, 0, 1, (void*)64, "version2"},
        {nvm_field_unsigned, 0, 0, (void*)64, "data"},
        {0, 0, 0, 0, 0}
    }
};
const nvm_type nvm_type_branch_v1 = {
    {0xb0a229e0449d54cb, 0x8bdd5c52356ef016}, // type_usid
    "branch_v1", // name
    "Test branch_v1 stuct", // tag
    sizeof(branch), //size
    8, // xsize
    0, // version
    16, // align
    &upgrade_br_v1, // upgrade
    &nvm_type_branch_v2, // uptype
    3, // field_cnt
    { // field[]]
        {nvm_field_usid, 0, 1, (void*)nvm_usid_self, "type_usid"},
        {nvm_field_unsigned, 0, 1, (void*)64, "version1"},
        {nvm_field_unsigned, 0, 0, (void*)64, "data"},
        {0, 0, 0, 0, 0}
    }
};
#endif //NVM_EXT

/*
 * The persistent struct root is the root struct in the main region
 * It holds the mutexes that protect the pointers.
 */
#ifdef NVM_EXT
persistent struct root
USID("2c04 2fa4 9020 80f9 b90b c3a4 3502 958f")
{
    nvm_mutex mtx[ptrs]; // a bunch of mutexes
    nvm_heap ^heap; // heap in heap extent
    branch ^^ptr; // array of branch pointers
};
typedef persistent struct root root;
#else
struct root
{
    nvm_usid type_usid;
    nvm_mutex mtx[ptrs]; // a bunch of mutexes
    nvm_heap_srp heap; // heap in heap extent
    branchArray_srp ptr; // struct branch ^ptr
};
typedef  struct root root;

const nvm_type nvm_type_root = {
    {0x2c042fa4902080f9, 0xb90bc3a43502958f}, // type_usid
    "root", // name
    "Test root stuct", // tag
    sizeof(root), //size
    0, // xsize
    0, // version
    16, // align
    0, // upgrade
    0, // uptype
    4, // field_cnt
    { // field[]]
        {nvm_field_usid, 0, 1, (void*)nvm_usid_self, "type_usid"},
        {nvm_field_unsigned, 0, ptrs, (void*)64, "mtx"},
        {nvm_field_pointer, 0, 1, NULL, "heap"},
        {nvm_field_pointer, 0, 1, NULL, "ptr"},
        {0, 0, 0, 0, 0}
    }
};
#endif //NVM_EXT

/*
 * This is one pretend log record
 */
struct logrec
{
    uint64_t rec;  // log record number
    uint32_t slot; // slot operated on
    uint32_t oldsz; // zero if slot was empty
    uint32_t newsz; // zero if allocation failed.
};

/*
 * Note that logrec gets an nvm_type struct description even though it is not
 * declared persistent. This is because it is embedded in the persistent
 * struct log.
 */
const nvm_type nvm_type_logrec = {
    {0, 0}, // type_usid
    "logrec", // name
    "Test log record stuct", // tag
    sizeof(struct logrec), //size
    0, // xsize
    0, // version
    8, // align
    0, // upgrade
    0, // uptype
    5, // field_cnt
    { // field[]]
        {nvm_field_unsigned, 0, 1, (void*)64, "rec"},
        {nvm_field_unsigned, 0, 1, (void*)32, "slot"},
        {nvm_field_unsigned, 0, 1, (void*)32, "oldsz"},
        {nvm_field_unsigned, 0, 1, (void*)32, "newsz"},
        {nvm_field_pad, 0, 1, (void*)32, "_pad1"},
        {0, 0, 0, 0, 0}
    }
};

/*
 * This is a pretend write ahead log for testing off region nested transactions.
 * It is the root struct of a log region
 */
#ifdef NVM_EXT
persistent struct log
USID("4f10 c888 407c a083 18f5 75d6 190b 5674")
{
    uint64_t count; // the number of records written to the log
    struct logrec recs[logsz];
};
typedef persistent struct log log;
#else
struct log
{
    nvm_usid type_usid;
    uint64_t count; // the number of records written to the log
    struct logrec recs[logsz];
};
typedef struct log log;

const nvm_type nvm_type_log = {
    {0x4f10c888407ca083, 0x18f575d6190b5674}, // type_usid
    "log", // name
    "Test log stuct", // tag
    sizeof(log), //size
    0, // xsize
    0, // version
    16, // align
    0, // upgrade
    0, // uptype
    3, // field_cnt
    { // field[]]
        {nvm_field_usid, 0, 1, (void*)nvm_usid_self, "type_usid"},
        {nvm_field_unsigned, 0, 1, (void*)64, "count"},
        {nvm_field_struct, 0, logsz, &nvm_type_logrec, "recs"},
        {0, 0, 0, 0, 0}
    }
};

/*
 * The following array of pointers to nvm_type structs is used to initialize
 * The USID map. Every nvm_type that contains a USID should be present in this 
 * array.
 */
const nvm_type *test_usid_types[] = {
    &nvm_type_root,
    &nvm_type_branch,
    &nvm_type_branch_v1,
    &nvm_type_branch_v2,
    &nvm_type_log,
    NULL // NULL terminator
};
#endif //NVM_EXT

/* This is the context for each thread doing allocations */
struct counts
{
    int thread;       // owning thread number starting at 1
    nvm_region_stat logstat; // region stat for the log used by this thread
    size_t alloc;     // total bytes successfully allocated
    int alloc_cnt;    // number of successful allocations
    size_t fail;      // total bytes that failed allocation
    int fail_cnt;     // number of failed allocations
    size_t free;      // total bytes freed
    int free_cnt;     // number of calls to nvm_free
    nvm_jmp_buf env;  // jump buf to exit early
};

/* last stat from the main region */
nvm_region_stat stat;

/*
 * Attach to the log region file for a thread. The region file is created if
 * necessary.
 */
#ifdef NVM_EXT
void attach_log(struct counts *cnts)
{
    /* construct the region file name for this thread */
    char fname[20];
    sprintf(fname, "/tmp/nvmd%d", cnts->thread);

    /* Attempt to attach to an existing log */
    uint8_t *base = logbase+logspace*cnts->thread;
    nvm_desc desc = nvm_attach_region(0, fname, base);
    if (desc == 0)
    {
        /* attach failed so create a new region */
        nvm_destroy_region(fname); // in case incomplete previous creation
        desc = nvm_create_region(0, fname, "LogRegion",
                base, logspace, logspace, 0777);
        if (desc == 0)
        {
            perror("Log Region Create failed");
            exit(1);
        }

        /* Get the root heap and allocate the log struct as the root object */
        nvm_query_region(desc, &cnts->logstat);
        nvm_heap ^rh = cnts->logstat.rootheap;
        log ^rs = 0;
        @ desc {
            rs = nvm_alloc(rh, shapeof(log), 1);
        }
        nvm_set_root_object(desc, rs);
        printf("Created log region %s\n", fname);
    } else {
        printf("Attached log region %s\n", fname);
    }

    /* put final stat of log region in thread data */
    if (!nvm_query_region(desc, &cnts->logstat))
        nvms_assert_fail("nvm_query_region failed");
    if (desc != cnts->logstat.desc)
        nvms_assert_fail("Wrong descriptor in logstat");

}
#else
void attach_log(struct counts *cnts)
{
    /* construct the region file name for this thread */
    char fname[20];
    sprintf(fname, "/tmp/nvmd%d", cnts->thread);

    /* Attempt to attach to an existing log */
    uint8_t *base = logbase+logspace*cnts->thread;
    nvm_desc desc = nvm_attach_region(0, fname, base);
    if (desc == 0)
    {
        /* attach failed so create a new region */
        nvm_destroy_region(fname); // in case incomplete previous creation
        desc = nvm_create_region(0, fname, "TestRegion",
                base, logspace, logspace, 0777);
        if (desc == 0)
        {
            perror("Log Region Create failed");
            exit(1);
        }

        /* Get the root heap and allocate the log struct as the root object */
        nvm_query_region(desc, &cnts->logstat);
        nvm_heap *rh = cnts->logstat.rootheap;
        nvm_txbegin(desc);
        log *rs = nvm_alloc(rh, shapeof(log), 1);
        nvm_txend();
        nvm_set_root_object(desc, rs);
        printf("Created log region %s\n", fname);
    } else {
        printf("Attached log region %s\n", fname);
    }

    /* put final stat of log region in thread data */
    if (!nvm_query_region(desc, &cnts->logstat))
        nvms_assert_fail("nvm_query_region failed");
    if (desc != cnts->logstat.desc)
        nvms_assert_fail("Wrong descriptor in logstat");
}
#endif //NVM_EXT

/*
 * Add a log record to the pretend log. This creates an off region nested
 * transaction to add the record atomically. Note we overwrite records because
 * this is just pretend. We require this to be called in a transaction only
 * because that is what we want to test.
 */
#ifdef NVM_EXT
void write_log(struct counts *cnts, int slot, uint32_t oldsz, uint32_t newsz)
{ @ cnts->logstat.desc {
    /* get the log struct */
    log ^lg = (log^)cnts->logstat.rootobject;
    nvm_verify(lg, shapeof(log));

    /* get the record we are going to overwrite */
    struct logrec ^lr = %lg=>recs[lg=>count % logsz];

    /* update the record */
    lr=>rec @= lg=>count@++;
    lr=>slot @= slot;
    lr=>oldsz @= oldsz;
    lr=>newsz @= newsz;

    /* just for testing tx depth and txstatus */
    @{
        int depth = nvm_txdepth();
        int status = nvm_txstatus(depth-1);
        if (depth != 3 || status != NVM_TX_ACTIVE)
            nvms_assert_fail("bad depth or status");
    }

}}
#else
void write_log(struct counts *cnts, int slot, uint32_t oldsz, uint32_t newsz)
{
    nvm_txbegin(cnts->logstat.desc); // begin nested tx in log region

    /* get the log struct */
    log *lg = (log*)cnts->logstat.rootobject;
    nvm_verify(lg, shapeof(log));

    /* get the record we are going to overwrite */
    struct logrec *lr = &lg->recs[lg->count%logsz];

    /* update the record */
    NVM_UNDO(*lr);
    NVM_UNDO(lg->count);
    lr->rec = lg->count++;
    lr->slot = slot;
    lr->oldsz = oldsz;
    lr->newsz = newsz;
    nvm_flush(lr, sizeof(*lr));
    nvm_flush1(&lg->count);

    /* just for testing tx depth and txstatus */
    nvm_txbegin(0);
    int depth = nvm_txdepth();
    int status = nvm_txstatus(depth-1);
    if (depth != 3 || status != NVM_TX_ACTIVE)
        nvms_assert_fail("bad depth or status");
    nvm_txend();

    nvm_txend();
}
#endif //NVM_EXT

/*
 * Do a random sized allocation and put it in a randomly chosen pointer. If
 * the pointer is already in use delete the current allocation atomically.
 *
 * Each call writes one pretend log record in the threads log region. This
 * is done in an off region nested transaction.
 *
 * Occasionally this will exit the thread early with an nvm_longjmp.
 */
#ifdef NVM_EXT
void alloc1(struct counts *cnts)
{
    root ^rs = (root ^)stat.rootobject;
    nvm_desc desc = stat.desc;
    nvm_heap ^heap = rs=>heap;

    /* pick a slot and size to use */
    int slot = random() % ptrs;
    size_t size = 1 + random() % (maxdata); // enough to run out of space

    /* Begin a base transaction to lock the slot. */
    @ desc {
        uint32_t oldsz = 0;
        uint32_t newsz = 0;

        nvm_xlock(%rs=>mtx[slot % ptrs]);

        /* Delete old ptr if any */
        branch ^^ptr = rs=>ptr;
        branch ^oldp = ptr[slot];
        nvm_verify(oldp, shapeof(branch)); // This will occasionally upgrade
        if (oldp)
        {
            /* it better have been upgraded to the current version */
            if (oldp=>version != 3)
                nvms_assert_fail("bad version - not upgraded");

            /* gather statistics */
            oldsz = oldp=>data[0];
            cnts->free += oldsz;
            cnts->free_cnt++;

        	/* free old allocation and clear pointer */
            nvm_free(oldp);
            ptr[slot] @=  0;
        }

        /* Do new allocation. This might fail for out of space. In order to test
         * upgrade, 10% of the time we allocate an old version, v1 or v2.  */
        const nvm_type *shape = shapeof(branch);
        int v = 3;
        switch (size % 20)
        {
        case 1: // double upgrade needed
            shape = shapeof(branch_v1);
            v = 1;
            break;

        case 2: // single upgrade needed
            shape = shapeof(branch_v2);
            v = 2;
            break;
        default:
            break;
        }
        branch ^leaf = nvm_alloc(heap, shape, size);
        if (leaf)
        {
            newsz = size * sizeof(uint64_t) + sizeof(branch);
            leaf=>data[0] ~= newsz;
            if (v == 3)
                leaf=>version ~= 3;
            cnts->alloc += newsz;
            cnts->alloc_cnt++;
            ptr[slot] @= leaf;
        }
        else
        {
            cnts->fail += size * sizeof(uint64_t) + sizeof(branch);
            cnts->fail_cnt++;
        }
        
        /* Pretend write ahead log for this operation */
        write_log(cnts, slot, oldsz, newsz);

        /* Occasionally long jump out before being done. */
        if (random() % (longjump) == 42)
        {
            nvm_longjmp(&cnts->env, 42);
        }
    }
}
#else
void alloc1(struct counts *cnts)
{
    root *rs = (root *)stat.rootobject;
    nvm_desc desc = stat.desc;
    nvm_heap *heap = nvm_heap_get(&rs->heap);

    /* pick a slot and size to use */
    int slot = random() % ptrs;
    size_t size = 1 + random() % (maxdata); // enough to run out of space

    /* Begin a base transaction to lock the slot. */
    nvm_txbegin(desc); // begin a transaction
    nvm_xlock(&rs->mtx[slot % ptrs]);

    /* sizes for log */
    uint32_t oldsz = 0;
    uint32_t newsz = 0;

    /* Delete old ptr if any */
    branch_srp *ptr = branchArray_get(&rs->ptr);
    branch *oldp = branch_get(&ptr[slot]);
    nvm_verify(oldp, shapeof(branch)); // This will occasionally upgrade
    if (oldp)
    {
        /* it better have been upgraded to the current version */
        if (oldp->version != 3)
            nvms_assert_fail("bad version - not upgraded");

        /* gather statistics */
        oldsz = oldp->data[0];
        cnts->free += oldsz;
        cnts->free_cnt++;

        /* free old allocation and clear pointer */
        nvm_free(oldp);
        branch_txset(&ptr[slot], 0);
    }

    /* Do new allocation. This might fail for out of space. In order to test
     * upgrade, 10% of the time we allocate an old version, v1 or v2.  */
    const nvm_type *shape = shapeof(branch);
    int v = 3;
    switch (size % 20)
    {
    case 1: // double upgrade needed
        shape = shapeof(branch_v1);
        v = 1;
        break;

    case 2: // single upgrade needed
        shape = shapeof(branch_v2);
        v = 2;
        break;
    default:
        break;
    }
    branch *leaf = nvm_alloc(heap, shape, size);
    if (leaf)
    {
        newsz = size * sizeof(uint64_t) + sizeof(branch);
        if (v == 3)
            NVM_NTSTORE(leaf->version, v);
        NVM_NTSTORE(leaf->data[0], newsz);
        cnts->alloc += newsz;
        cnts->alloc_cnt++;
        branch_txset(&ptr[slot], leaf);
    }
    else
    {
        cnts->fail += size * sizeof(uint64_t) + sizeof(branch);
        cnts->fail_cnt++;
    }

    /* Pretend write ahead log for this operation */
    write_log(cnts, slot, oldsz, newsz);

    /* Occasionally long jump out before being done. */
    if (random() % (longjump) == 42)
    {
        nvm_longjmp(&cnts->env, 42);
    }

    nvm_txend(); //commit base transaction releasing the slot lock
}
#endif //NVM_EXT
#ifdef NVM_EXT
static void report(nvm_desc desc, const char *title);
static void report_heap(nvm_heap ^heap);
void nvm_heap_test(nvm_desc desc, nvm_heap ^heap);
#else
static void report(nvm_desc desc, const char *title);
static void report_heap(nvm_heap *heap);
void nvm_heap_test(nvm_desc desc, nvm_heap *heap);
#endif //NVM_EXT
/*
 * This is the function that each thread executes.
 */
void *thread_func(void *ctx)
{
    struct counts *cnts = ctx;
    int n = loops;

    /* Initialize the thread. */
    nvm_thread_init();
    
    /* create a jmp_buf for early exit of a thread. On a long jump  
     * return to exit this thread. */
    if (nvm_setjmp(&cnts->env)) 
    {
        goto end_thread;
    }
    
    /* Map in a log region for this thread to use. This is for testing off
     * region nested transactions.
     */
    attach_log(cnts);

    /* do a bunch of random allocations and deallocations. */
    while (n--)
    {
        alloc1(cnts);
    }

    /* finished with the library */
end_thread:
    if (!nvm_detach_region(cnts->logstat.desc))
    {
        perror("Log region detach failed");
        exit(1);
    }
    nvm_thread_fini();
    return NULL;
}
/*
 * This is the main driver of the test
 */
#ifdef NVM_EXT
void usid_register(void);
int
main(int argc, char** argv)
{
    const char *fname = "/tmp/nvmd";
    if (argc == 2)
        fname = argv[1];

    /* Fork children to do the real work. Kill them after a few seconds*/
    int x = FORKS; //number of children to spawn
    int z = 0;
    int y = 0;
    while (x > 0)
    {
    	x--;
        pid_t pid = fork();
        if (!pid)
            break;
        sleep(1 + x % (maxsleep-1));
        kill(pid, 9);
        int status;
        wait(&status);
        if (status == 0)
        {
            /* on some successful completions delete the main region so
             * that create region gets tested */
            if (x & 1)
                nvm_destroy_region(fname);
            y++;
        }
        else if (status == 9)
            z++;
        else
        {
            printf("Unexpected exit status %d\n", status);
            exit(1);
        }
        printf("******************** Processes killed: %d, completed %d\n\n", z, y);
    }

    /* We get here if we are the child process or there are no children.
     * The first thing is to initialize the NVM library for the main thread */
    nvm_thread_init();
    usid_register();

    again:;
    /* If attaching the region does not work then create a new one. This will
     * run recovery if needed. */
    nvm_desc desc = nvm_attach_region(0, fname, attch);
    int success;
    int created = 0;
    root ^rs;
    if (desc == 0)
    {
        //        printf("Creating a new region\n");

        /* create new region destroying old one first just in case. */
        nvm_destroy_region(fname);
        desc = nvm_create_region(0, fname, "TestRegion",
                attch, vspace, pspace, 0777);
        if (desc == 0)
        {
            perror("Region Create failed");
            exit(1);
        }

        /* Get the root heap */
        success = nvm_query_region(desc, &stat);
        nvm_heap ^rh = (nvm_heap ^)stat.rootheap;
        nvm_heap_test(desc, rh);

        /* Allocate a root struct in a transaction */
        @ desc {
            rs = nvm_alloc(rh, shapeof(root), 1);
            int m;
            for (m = 0; m < ptrs; m++)// initialize the mutexes
                nvm_mutex_init(%rs=>mtx[m], 10);
        }

        nvm_set_root_object(desc, rs);
        printf("Created a new region\n");
        created = 1;
    }
    else
    {
        /* Get the root struct */
        success = nvm_query_region(desc, &stat);
        rs = stat.rootobject;
        printf("Attached existing region\n");
    }

    /* If extents not added then add them. */
    nvm_extent_stat xstat[4];
    nvm_query_extents(desc, 1, 4, xstat);
    if (xstat[0].psize == 0)
    {
        /* add an application managed extent to the region */
        @desc{
            void ^ext = (void ^)nvm_add_extent(attch + xoff, ptrspace);
            if (ext == (void^)0)
            {
                perror("Add extent failed");
                exit(1);
            }

            /* The extent will hold the array of pointers to allocated space.
             * They need to be initialized to be null pointers */
            rs=>ptr @= (branch ^^)ext;
            int i;
            branch ^^ptr = rs=>ptr;
            for (i = 0; i < ptrs; i++)
               ptr[i] ~= 0;
        }
        printf("Added an app managed extent\n");
    }
    else
    {
        /* If pointer array exists, but not the heap, then clear the pointers.
         * There is a window where the heap has been deleted, but not the 
         * pointers. This cleans up if we die there. */
        if (rs=>heap == 0)
        {
            branch ^^ptr = rs=>ptr;
            int i;
            for (i = 0; i < ptrs; i++)
                ptr[i] ~= 0;
            printf("Cleared pointers when heap missing\n");
        }
    }

    if (xstat[1].psize == 0)
    {
        @desc{
            /* add a heap managed extent to the region */
            nvm_heap ^heap = nvm_create_heap(desc, attch + 2 * xoff, pspace,
                    "heap extent");
            rs=>heap @= heap;
            if (heap == 0)
            {
                perror("Add heap failed");
                exit(1);
            }
            /* set the root object to be the heap just to call set heap root */
            nvm_heap_setroot(heap, heap);
        }
        printf("Added a heap extent\n");
    }

    /* report txconfig */
    nvm_txconfig cfg;
    nvm_get_txconfig(desc, &cfg);
    printf("maxtx:%d undo:%d limit:%d\n", cfg.txn_slots, cfg.undo_blocks,
            cfg.undo_limit);
    cfg.txn_slots = 80;
    cfg.undo_blocks = 700;
    cfg.undo_limit = 16;
    nvm_set_txconfig(desc, &cfg);
    nvm_get_txconfig(desc, &cfg);
    printf("maxtx:%d undo:%d limit:%d\n", cfg.txn_slots, cfg.undo_blocks,
            cfg.undo_limit);
    cfg.txn_slots = 31;
    cfg.undo_blocks = 250;
    cfg.undo_limit = 8;
    nvm_set_txconfig(desc, &cfg);
    nvm_get_txconfig(desc, &cfg);
    printf("maxtx:%d undo:%d limit:%d\n", cfg.txn_slots, cfg.undo_blocks,
            cfg.undo_limit);

    nvm_heap_test(desc, rs=>heap);

   if (created)
        report(desc, "Immediately after creation");

    /* Detach and reattach at a different address. */
    success = nvm_detach_region(desc);
    if (!success)
    {
        perror("Detach failed\n");
        exit(1);
    }
    desc = nvm_attach_region(2, fname, attch + vspace);
    if (!success)
    {
        perror("Attach failed\n");
        exit(1);
    }

    success = nvm_query_region(desc, &stat);
    if (!success)
    {
        perror("Query region failed\n");
        exit(1);
    }
    rs = stat.rootobject;
    branch ^^ptr = rs=>ptr;

    /* grow the pointer extent just for fun */
    @ desc {
        nvm_resize_extent(ptr, ptrspace * 2);
    }

    /* Grow the heap just for fun */
    nvm_heap ^heap = rs=>heap;
    if (!nvm_resize_heap(heap, pspace + 1024 * 1024))
    {
        perror("Grow of heap failed");
        exit(1);
    }
    nvm_heap_test(desc, heap);

    /* determine how much is allocated before we start the test */
    int i;
    size_t init_consumed = heap=>consumed;
    int init_cnt = 0;
    for (i = 0; i < ptrs; i++)
    {
        branch ^p = ptr[i];
        if (p)
        {
            nvm_blk ^nvb = ((nvm_blk^)p) - 1;
            nvm_verify(nvb, shapeof(nvm_blk));
            init_consumed -= sizeof(nvm_blk) * (nvb=>neighbors.fwrd - nvb);
            init_cnt++;
        }
    }
//    printf("init_consumed=%ld init_cnt=%d\n", init_consumed, init_cnt);

    struct counts cnts;
    memset(&cnts, 0, sizeof(cnts));


    /* shrink the pointer extent back just for fun */
     @ desc {
        nvm_resize_extent(ptr, ptrspace);
    }

    /* Shrink the heap extent back to its original size. First we free the
     * highest allocation so we do not get caught with too small a last block. */
    branch ^hp = NULL;
    int hi = 0;
    for (i = 0; i < ptrs; i++)
    {
        branch ^p = ptr[i];
        if (p > hp)
        {
            hp = p;
            hi = i;
        }
    }
    if (hp)
    {@ desc {
        nvm_free(hp);
        ptr[hi] @= 0;
    }}
    if (!nvm_resize_heap(heap, pspace))
    {
        perror("Shrink of heap failed");
        exit(1);
    }


    /*
     * Start up the worker threads and wait for them to complete
     */
    report(desc, "Begin Run");
    pthread_t threads[THREADS];
    struct counts tcnts[THREADS];
    memset(tcnts, 0, sizeof(tcnts));
    int t;
    for (t = 0; t < THREADS; t++)
    {
        tcnts[t].thread = t + 1;
        pthread_create(&threads[t], NULL, &thread_func, &tcnts[t]);
    }
    for (t = 0; t < THREADS; t++)
    {
        pthread_join(threads[t], NULL);
        cnts.alloc += tcnts[t].alloc;
        cnts.alloc_cnt += tcnts[t].alloc_cnt;
        cnts.fail += tcnts[t].fail;
        cnts.fail_cnt += tcnts[t].fail_cnt;
        cnts.free += tcnts[t].free;
        cnts.free_cnt += tcnts[t].free_cnt;
    }

    /* See if all is still well */
    nvm_heap_test(desc, heap);

    report(desc, "After run");

    /* report what happened */
    int n = 0;
    size_t sz = 0;
    size_t ovr = 0;
    for (i = 0; i < ptrs; i++)
    {
        branch ^p = ptr[i];
        if (p)
        {
            n++;
            sz += p=>data[0];
            nvm_blk ^nvb = ((nvm_blk^)p) - 1;
            nvm_verify(nvb, shapeof(nvm_blk));
            ovr += sizeof(nvm_blk) * (nvb=>neighbors.fwrd - nvb) - p=>data[0];
        }
    }

    printf("Final: %d allocations of %ld bytes\n", n, sz);
    printf("Overhead %ld, or %ld bytes each\n", ovr, (ovr + n / 2) / n);
    if (heap=>consumed != init_consumed + sz + ovr)
    {
        char msg[80];

        sprintf(msg, "Expected %ld consumed, and got %ld\n",
                init_consumed + sz + ovr,
                heap=>consumed);
        nvms_assert_fail(msg);
    }
    printf("Allocations %5d total bytes %ld\n", cnts.alloc_cnt, cnts.alloc);
    printf("   Failures %5d total bytes %ld\n", cnts.fail_cnt, cnts.fail);
    printf("      Frees %5d total bytes %ld\n", cnts.free_cnt, cnts.free);

    /* Delete the extents and see that they are deleted */
    nvm_query_extents(desc, 0, 4, xstat);
    if (xstat[1].heap || xstat[1].psize == 0 ||
        xstat[2].heap == 0 || xstat[3].psize)
    {
        printf("Bad extent status before delete\n");
        exit(1);
    }

    //delete heap and app extent before return
    @ desc {
        nvm_delete_heap(heap);
        rs=>heap @= 0;
        nvm_remove_extent(xstat[1].extent);
    }
    nvm_query_extents(desc, 0, 4, xstat);
    if (xstat[1].heap || xstat[1].psize ||
        xstat[2].heap || xstat[2].psize)
    {
        printf("Bad extent status after delete\n");
        exit(1);
    }

    /* detach the region and possibly start all over forever */
    success = nvm_detach_region(desc);
    if (!success)
    {
        perror(" Final Detach failed\n");
        exit(1);
    }
    if (x < 0)
    {
        printf("AGAIN %d\n", -1-x--);
        goto again;
    }

    /* All done with the nvm library */
    nvm_thread_fini();

    return(EXIT_SUCCESS);
}
#else
int
main(int argc, char** argv)
{
    const char *fname = "/tmp/nvmd";
    if (argc == 2)
        fname = argv[1];

    /* Fork children to do the real work. Kill them after a few seconds*/
    int x = FORKS; //number of children to spawn
    int z = 0;
    int y = 0;
    while (x > 0)
    {
        x--;
        pid_t pid = fork();
        if (!pid)
            break;
        sleep(1 + x % (maxsleep-1));
        kill(pid, 9);
        int status;
        wait(&status);
        if (status == 0)
        {
            /* on some successful completions delete the main region so
             * that create region gets tested */
            if (x & 1)
                nvm_destroy_region(fname);
            y++;
        }
        else if (status == 9)
            z++;
        else
        {
            printf("Unexpected exit status %d\n", status);
            exit(1);
        }
        printf("******************** Processes killed: %d, completed %d\n\n", z, y);
    }

    /* We get here if we are the child process or there are no children.
     * The first thing is to initialize the NVM library for the main thread */
    nvm_thread_init();
    nvm_usid_register_types(test_usid_types);

    again:;
    /* If attaching the region does not work then create a new one. This will
     * run recovery if needed. */
    nvm_desc desc = nvm_attach_region(0, fname, attch);
    int success;
    int created = 0;
    root *rs;
    if (desc == 0)
    {
        //        printf("Creating a new region\n");

        /* create new region destroying old one first just in case. */
        nvm_destroy_region(fname);
        desc = nvm_create_region(0, fname, "TestRegion",
                attch, vspace, pspace, 0777);
        if (desc == 0)
        {
            perror("Region Create failed");
            exit(1);
        }

        /* Get the root heap */
        success = nvm_query_region(desc, &stat);
        nvm_heap *rh = stat.rootheap;
        nvm_heap_test(desc, rh);

        /* Allocate a root struct in a transaction */
        nvm_txbegin(desc);
        rs = nvm_alloc(rh, shapeof(root), 1);
        int m;
        for (m = 0; m < ptrs; m++)// initialize the mutexes
            nvm_mutex_init(&rs->mtx[m], 10);
        nvm_commit();
        nvm_txend();

        nvm_set_root_object(desc, rs);
        printf("Created a new region\n");
        created = 1;
    }
    else
    {
        /* Get the root heap and root struct */
        success = nvm_query_region(desc, &stat);
        rs = stat.rootobject;
        printf("Attached existing region\n");
    }

    /* If extents not added then add them. */
    nvm_extent_stat xstat[4];
    nvm_query_extents(desc, 1, 4, xstat);
    if (xstat[0].psize == 0)
    {
        /* add an application managed extent to the region */
        nvm_txbegin(desc);
        void *ext = nvm_add_extent(attch + xoff, ptrspace);
        if (ext == NULL)
        {
            perror("Add extent failed");
            exit(1);
        }

        /* The extent will hold the array of pointers to allocated space.
         * They need to be initialized to be null pointers */
        nvm_undo(&rs->ptr, sizeof(rs->ptr));
        branchArray_set(&rs->ptr, ext);
        int i;
        branch_srp *ptr = ext;
        for (i = 0; i < ptrs; i++)
            branch_set(&ptr[i], NULL);
        nvm_flush(ptr, sizeof(ptr[0]) * ptrs);
        nvm_commit();
        nvm_txend();
        printf("Added an app managed extent\n");
    }
    else
    {
        /* If pointer array exists, but not the heap, then clear the pointers.
         * There is a window where the heap has been deleted, but not the 
         * pointers. This cleans up if we die there. */
        if (nvm_heap_get(&rs->heap) == NULL)
        {
            branch_srp *ptr = branchArray_get(&rs->ptr);
            int i;
            for (i = 0; i < ptrs; i++)
                branch_set(&ptr[i], NULL);
            nvm_flush(ptr, sizeof(ptr[0]) * ptrs);
            printf("Cleared pointers when heap missing\n");
        }
    }
    if (xstat[1].psize == 0)
    {
        nvm_txbegin(desc);
        {
            /* add a heap managed extent to the region */
            nvm_heap *heap = nvm_create_heap(desc, attch + 2 * xoff, pspace,
                    "heap extent");
            nvm_heap_txset(&rs->heap, heap);
            if (heap == NULL)
            {
                perror("Add heap failed");
                exit(1);
            }
            /* set the root object to be the heap just to call set root */
            nvm_heap_setroot(heap, heap);
        }
        nvm_txend();
        printf("Added a heap extent\n");
    }

    /* report txconfig */
    nvm_txconfig cfg;
    nvm_get_txconfig(desc, &cfg);
    printf("maxtx:%d undo:%d limit:%d\n", cfg.txn_slots, cfg.undo_blocks,
            cfg.undo_limit);
    cfg.txn_slots = 80;
    cfg.undo_blocks = 700;
    cfg.undo_limit = 16;
    nvm_set_txconfig(desc, &cfg);
    nvm_get_txconfig(desc, &cfg);
    printf("maxtx:%d undo:%d limit:%d\n", cfg.txn_slots, cfg.undo_blocks,
            cfg.undo_limit);
    cfg.txn_slots = 31;
    cfg.undo_blocks = 250;
    cfg.undo_limit = 8;
    nvm_set_txconfig(desc, &cfg);
    nvm_get_txconfig(desc, &cfg);
    printf("maxtx:%d undo:%d limit:%d\n", cfg.txn_slots, cfg.undo_blocks,
            cfg.undo_limit);

    nvm_heap_test(desc, nvm_heap_get(&rs->heap));

    if (created)
        report(desc, "Immediately after creation");

    /* Detach and reattach at a different address. */
    success = nvm_detach_region(desc);
    if (!success)
    {
        perror("Detach failed\n");
        exit(1);
    }
    desc = nvm_attach_region(2, fname, attch + vspace);
    if (!desc)
    {
        perror("Reattach failed\n");
        exit(1);
    }

    success = nvm_query_region(desc, &stat);
    if (!success)
    {
        perror("Query region failed\n");
        exit(1);
    }
    rs = stat.rootobject;
    branch_srp *ptr = branchArray_get(&rs->ptr);

    /* grow the pointer extent just for fun */
    nvm_txbegin(desc);
    nvm_resize_extent(ptr, ptrspace * 2);
    nvm_commit();
    nvm_txend();

    /* Grow the heap just for fun */
    nvm_heap *heap = nvm_heap_get(&rs->heap);
    if (!nvm_resize_heap(heap, pspace + 1024 * 1024))
    {
        perror("Grow of heap failed");
        exit(1);
    }
    nvm_heap_test(desc, heap);

    /* determine how much is allocated before we start the test */
    int i;
    size_t init_consumed = heap->consumed;
    int init_cnt = 0;
    for (i = 0; i < ptrs; i++)
    {
        branch *p = branch_get(&ptr[i]);
        if (p)
        {
            nvm_blk *nvb = ((nvm_blk*)p) - 1;
            nvm_verify(nvb, shapeof(nvm_blk));
            init_consumed -=
                    sizeof(nvm_blk) *
                    (nvm_blk_get(&nvb->neighbors.fwrd) - nvb);
            init_cnt++;
        }
    }
//    printf("init_consumed=%ld init_cnt=%d\n", init_consumed, init_cnt);

    struct counts cnts;
    memset(&cnts, 0, sizeof(cnts));


    /* shrink the pointer extent back just for fun */
    nvm_txbegin(desc);
    nvm_resize_extent(ptr, ptrspace);
    nvm_commit();
    nvm_txend();

    /* Shrink the extent back to its original size. First we free the highest
     * allocation so the we do not get caught with too small a last block. */
    branch *hp = NULL;
    int hi = 0;
    for (i = 0; i < ptrs; i++)
    {
        branch *p = branch_get(&ptr[i]);
        if (p > hp)
        {
            hp = p;
            hi = i;
        }
    }
    if (hp)
    {
        nvm_txbegin(desc);
        nvm_free(hp);
        nvm_undo(&ptr[hi], sizeof(ptr[0]));
        branch_set(&ptr[hi], 0);
        nvm_flush1(&ptr[hi]);
        nvm_commit();
        nvm_txend();

    }
    if (!nvm_resize_heap(heap, pspace))
    {
        perror("Shrink of heap failed");
        exit(1);
    }

    /*
     * Start up the worker threads and wait for them to complete
     */
    report(desc, "Begin Run");
    pthread_t threads[THREADS];
    struct counts tcnts[THREADS];
    memset(tcnts, 0, sizeof(tcnts));
    int t;
    for (t = 0; t < THREADS; t++)
    {
        tcnts[t].thread = t + 1;
        pthread_create(&threads[t], NULL, &thread_func, &tcnts[t]);
    }
    for (t = 0; t < THREADS; t++)
    {
        pthread_join(threads[t], NULL);
        cnts.alloc += tcnts[t].alloc;
        cnts.alloc_cnt += tcnts[t].alloc_cnt;
        cnts.fail += tcnts[t].fail;
        cnts.fail_cnt += tcnts[t].fail_cnt;
        cnts.free += tcnts[t].free;
        cnts.free_cnt += tcnts[t].free_cnt;
    }

    /* See if all is still well */
   nvm_heap_test(desc, heap);

    report(desc, "After run");

    /* report what happened */
    int n = 0;
    size_t sz = 0;
    size_t ovr = 0;
    for (i = 0; i < ptrs; i++)
    {
        branch *p = branch_get(&ptr[i]);
        if (p)
        {
            n++;
            sz += p->data[0];
            nvm_blk *nvb = ((nvm_blk*)p) - 1;
            nvm_verify(nvb, shapeof(nvm_blk));
            ovr += sizeof(nvm_blk)
                    * (nvm_blk_get(&nvb->neighbors.fwrd) - nvb)
                    - p->data[0];
        }
    }

    printf("Final: %d allocations of %ld bytes\n", n, sz);
    printf("Overhead %ld, or %ld bytes each\n", ovr, (ovr + n / 2) / n);
    if (heap->consumed != init_consumed + sz + ovr)
    {
        char msg[80];

        sprintf(msg, "Expected %ld consumed, and got %ld\n",
                init_consumed + sz + ovr,
                heap->consumed);
        nvms_assert_fail(msg);
    }
    printf("Allocations %5d total bytes %ld\n", cnts.alloc_cnt, cnts.alloc);
    printf("   Failures %5d total bytes %ld\n", cnts.fail_cnt, cnts.fail);
    printf("      Frees %5d total bytes %ld\n", cnts.free_cnt, cnts.free);

    /* Delete the extents and see that they are deleted */
    nvm_query_extents(desc, 0, 4, xstat);
    if (xstat[1].heap || xstat[1].psize == 0 ||
        xstat[2].heap == NULL || xstat[3].psize)
    {
        printf("Bad extent status before delete\n");
        exit(1);
    }
    //delete heap and app extent before return
    nvm_txbegin(desc);
    nvm_delete_heap(heap);
    nvm_heap_txset(&rs->heap, 0);
    nvm_remove_extent(xstat[1].extent);
    nvm_commit();
    nvm_txend();
    nvm_query_extents(desc, 0, 4, xstat);
    if (xstat[1].heap || xstat[1].psize ||
        xstat[2].heap || xstat[2].psize)
    {
        printf("Bad extent status after delete\n");
        exit(1);
    }

    /* detach the region and possibly start all over forever */
    success = nvm_detach_region(desc);
    if (!success)
    {
        printf(" Final Detach failed\n");
        exit(1);
    }
    if (x < 0)
    {
        printf("AGAIN %d\n", -1-x--);
        goto again;
    }

    /* All done with the nvm library */
    nvm_thread_fini();

    return(EXIT_SUCCESS);
}
#endif //NVM_EXT
/*
 * Report a region, all the extents and any heaps they contain
 */
void report(nvm_desc desc, const char *title)
{
    nvm_region_stat rstat;
    nvm_extent_stat xstat[64];

    if (title)
        printf("\n%s\n", title);

    /* Region report */
    nvm_query_region(desc, &rstat);
    printf("Region[%d]:\"%s\" addr:%p, vsize:0x%lx, extents:%d\n", desc,
            rstat.name, rstat.base, rstat.vsize, rstat.extent_cnt);

    /* extents report */
    nvm_query_extents(desc, 0, 64, xstat);
    int x;
    for (x = 0; x <= rstat.extent_cnt; x++)
    {
        printf("  Extent[%d] addr:%p, psize:0x%lx, heap:%p\n", x,
                xstat[x].extent, xstat[x].psize, xstat[x].heap);
        if (xstat[x].heap)
            report_heap(xstat[x].heap);
    }
    printf("\n");
}
/*
 * Report the query results for a heap
 */
#ifdef NVM_EXT
void report_heap(nvm_heap ^heap)
{
    nvm_heap_stat hstat;
    nvm_query_heap(heap, &hstat);

    /* Report statistics */
    printf("Heap:\"%s\" Consumed:0x%lx Free:0x%lx inuse:%d\n", hstat.name,
            hstat.consumed, hstat.free, hstat.inuse);
}
#else
void report_heap(nvm_heap *heap)
{
    nvm_heap_stat hstat;
    nvm_query_heap(heap, &hstat);

    /* Report statistics */
    printf("Heap:\"%s\" Consumed:0x%lx Free:0x%lx inuse:%d\n", hstat.name,
            hstat.consumed, hstat.free, hstat.inuse);
}
#endif //NVM_EXT
