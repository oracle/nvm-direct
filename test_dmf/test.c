/*
Copyright (c) 2015, 2016, Oracle and/or its affiliates. All rights reserved.

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
/** \file test.c
 *  \brief Driver to test the Direct Mapped File library
 *
 *  Created on: Dec 17, 2015
 *      Author: bbridge
 *
 *  This driver spawns a child process that runs multiple threads doing
 *  random reads and writes to the same file. Occasionally a thread will
 *  resize the file while others are reading and writing it. A thread
 *  runs until it has completed a set number of I/O's. The parent process
 *  kills the child after various timeouts. Sometimes the child exits on
 *  its own and sometimes it is killed in the middle of I/O or resize. The
 *  parent spawns a new child after waiting for the previous one.
 *
 *  A random number is chosen to fill a block with 64 bit integers starting
 *  at the random number and incrementing. This is written to a block when
 *  writing and verified on read to ensure I/O is atomic even through
 *  process death.
 *
 *  The test can be configured to run with different sizes and thread counts.
 *  Debugging is easier if the I/O is done in the parent without any
 *  children. Edit the constants below to change the configuration.
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
#include <time.h>
#include <error.h>
#include <signal.h>
#define NVM_EXT // this uses the NVM Direct C extensions
#include "nvm.h"
#include "dmf.h"


/*
 * This is a driver to exercise the code. The following are configuration
 * parameters that can be edited to create different tests.
 */

/*
 * The following constants define the shape of the file tested here
 */
#define BLOCK_SIZE  1024 /* block size in bytes */
#define FILE_SIZE 8192   /* file size in blocks */
//#define FILE_SIZE 4096   /* file size in blocks */
//#define FILE_SIZE 1024   /* file size in blocks */

/* resizes keep the minimum and maximum file size within this many blocks of
 * FILE_SIZE. */
#define DELTA 64
//#define DELTA 512

/* Each thread does a resize once this many read or write calls, on average */
#define RESIZE_RATE 1024

/* This is the number of child processes that will be forked and possibly
 * killed. Choose zero to do one run in the initial process. This is easier
 * to debug. Choose 100,000 to let the program run for about a week. */
//#define FORKS (-1) // no children, loop forever
//#define FORKS (0) // no children, one run
#define FORKS (100000) // run forever

/* The parent process sleeps an interval of 1 to maxsleep seconds before
 * killing the child process. This should be tuned so that most children
 * are killed but a significant number complete without being killed. */
const int maxsleep = 12;
//const int maxsleep = 9'';
//const int maxsleep = 5;

/* loops is the number of times each thread goes through the test loop before
 * doing a normal exit. */
//const int loops = 50000;
const int loops = 1024000;

/* Each run spawns THREADS threads to simultaneously run through the
 * test loop. Setting threads to 1 makes debugging easier. */
//#define THREADS 6
#define THREADS 4
//#define THREADS 1

pthread_mutex_t size_mutex; // only one resize at a time
ub4 usable_size; // file blocks that are properly initialized and accessible

/* This is the context for each thread doing allocations */
#define BUF_SIZE (BLOCK_SIZE/sizeof(ub8))
struct counts
{
    int thread;       // owning thread number starting at 1
    dmf_file *dmf;     // dmf of file to operate on
    ub8 writes;       // number of blocks written
    ub8 reads;        // number of blocks read
    ub8 collisions;   // number of times ptr to block changed while reading
    ub8 grows;        // number of successful file grows
    ub8 shrinks;      // number of successful file shrinks
    ub8 buf[BUF_SIZE*3]; // I/O buffer for this thread
};
/*
 * Fill a buffer with the block number and incrementing values based on a
 * random number. This pattern makes it easy to determine if a block is
 * fractured or if it the wrong block
 */
void fill_buf(ub8 *buf, ub8 blkno)
{
    /* fill the block with contents based on a random number */
    ub8 rval = random() * (blkno+1);
    buf[0]  = rval;
    buf[1]  = blkno; // block number in head
    buf[BUF_SIZE - 1]  = blkno; // block number in tail
    int i;
    for (i = 2; i < BUF_SIZE - 1; i++)
        buf[i] = rval+i;
}

/*
 * flag to make test threads stop and wait for debugger to attach
 */
int stop = 0;

/*
 * Verify a buffer has valid contents generated by fill_buf
 */
const char *check_buf(ub8 *buf, ub8 blkno)
{
    /* verify contents constructed by fill_buf  */
    const char *msg = NULL;
    int i = 0;
    ub8 rval =  buf[0];
    if (buf[1] != blkno)
    {
        printf("bad head block number: buf has %llu not %llu\n", buf[1], blkno);
        msg = "bad head block number";
    }
    if (buf[BUF_SIZE - 1] != blkno)
    {
        printf("bad tail block number: buf has %llu not %llu\n",
                buf[BUF_SIZE - 1], blkno);
        msg = "bad tail block number";
    }
    for (i = 2; i < BUF_SIZE - 1; i++)
        if (buf[i] != rval+i)
        {
            printf("bad block contents: buf[%u] has %llu not %llu\n",
                    i, buf[i], rval+i);
            msg = "bad block contents";
        }
    return msg;
}

/*
 * Catch an error and pause until a debugger can be attached.
 */
void catch_err()
{
    stop++;
    signal(SIGUSR1, SIG_IGN); // do not let parent kill us
    error(0,errno,"Got error, pid %d. Waiting until a debugger is attached",
            getpid());
    while(1){
        sleep(1);
    }
}

/*
 * This is the function that each thread executes.
 */
void *thread_func(void *ctx)
{
    struct counts *cnts = ctx;
    int n = loops;

    /* Initialize the thread. */

    nvm_thread_init();

    /* initialize stats */
    cnts->writes = 0;
    cnts->reads = 0;
    cnts->grows = 0;
    cnts->shrinks = 0;

    /*
     * get the stats for the region so we can find the root struct.
     */

    /* do a bunch of random reads and writes. */
    while (n > 0)
    {
        while (stop) sleep(1);// stop activity if an error happened
        ub4 r = random();

        /* If usable_size grows or shrinks in here we can get a bad
         * len, (e.g. 8000). So we capture the current value and use it
         * consistently. If the usable size shrinks immediately after this
         * then the read may not get all the blocks it requests. */
        ub4 us = usable_size;

        /* pick a random block within the usable_size */
        ub8 blkno = r % us; // block to access
        r /= us;

        /* decide if this should be a read or a write */
        int wrt = r % 4 == 0; // 1/4th writes
        r /= 4;

        /* decide how many blocks to read or write */
        int len = r % 3 + 1;  // 1-3 blocks per I/O
        r /= 3;

        /* Do not access past usable_size. */
        if (blkno + len > us)
            len = us - blkno;

        /* adjust count of blocks for ending the thread */
        n -= len;      // terminate loop on attempted I/O count

        /* do one I/O */
        int b;
        if (wrt)
        {
            /* write the block with contents based on a random number. If a
             * resize puts the block outside the file we do not count this
             * write. This returns a zero block count. */
            for (b = 0; b < len; b++)
            {
                fill_buf(cnts->buf + b*BUF_SIZE, blkno + b);
            }
            int nw = dmf_write(cnts->dmf, blkno, cnts->buf, len);
            if (nw < 0)
            {
                error(1, errno, "Error writing block %llu", blkno);
                catch_err();
            }
            cnts->writes += nw;
        } else {
            /* read the block and verify its contents if read succeeds. The
             * read can fail if a shrink starts after we pick a block and
             * before calling dmf_read. This returns a block count less than
             * len. A shrink to below our blocks and a grow back above them
             * can happen between the time we capture usable blocks and when
             * we do the read. This results in a block of all zero being
             * read. This can only happen if reading a block within the
             * minimum and maximum file size. */
            cnts->buf[1] = 0x7ffffff; // set initial value to impossible
            int nr = dmf_read(cnts->dmf, blkno, cnts->buf, len);
            if (nr < 0)
            {
                error(0, errno, "Error reading block %llu", blkno);
                catch_err();
            }
            for (b = 0; b < nr; b++)
            {
                /* A block of all zero can be the result usable size changing
                 * after we save it and before we do the read. Grow leaves
                 * blocks of zero which may not be rewritten yet. However this
                 * can only happen if the file blocks are in the range affected
                 * by resizes. */
                ub8 *buf = cnts->buf + b*BUF_SIZE;
                if ((blkno + b >= FILE_SIZE - DELTA) &&
                        (buf[0] | buf[1] | buf[2] | buf[BUF_SIZE - 1]) == 0)
                    continue;

                const char *msg = check_buf(buf, blkno + b);
                if (msg)
                {
                    printf("loop %u expected block# %llu found %llu\n",
                            loops-n, blkno + b, cnts->buf[1]);
                    errno = EIO; catch_err();
                    error(1, EIO, "%s : ", msg);
                }
                cnts->reads++;
            }
        }

        /* about one time in 1024 resize the file */
        if (r % RESIZE_RATE == 0)
        {

            pthread_mutex_lock(&size_mutex);

            while (stop) sleep(1);// stop activity if an error happened

            /* pick a new file size */
            r = random();
            ub4 change = r % (DELTA*2);
            ub4 newsize = FILE_SIZE - DELTA + change;
            if (newsize < usable_size){
                usable_size = newsize;
                if (dmf_resize(cnts->dmf, newsize))
                {
                    error(1, errno, "Shrinking to %d blocks", newsize);
                    catch_err();
                }
                cnts->shrinks++;
            } else if (newsize > usable_size){
                /* growing so resize and initialize to make space usable */
                if (dmf_resize(cnts->dmf, newsize))
                {
                    error(1, errno, "Growing to %d blocks", newsize);
                    catch_err();
                }

                /* initialize new blocks */
                ub4 blkno;
                ub8 buf[BUF_SIZE];
                for (blkno = usable_size; blkno < newsize; blkno++)
                {
                    fill_buf(buf, blkno);
                    if (dmf_write(cnts->dmf, blkno, buf, 1) < 1)
                    {
                        error(1, errno, "Writing block %d after grow", blkno);
                        catch_err();
                    }
                }

                /* they are now usable */
                usable_size = newsize;
                cnts->grows++;
            }

            while (stop) sleep(1);// stop activity if an error happened

            pthread_mutex_unlock(&size_mutex);
        }

    }

    /* finished with the library */
    nvm_thread_fini();
    return NULL;
}

/*
 * Print a message and check the file for corruption
 */
void test_check(dmf_file *dmf, const char *where)
{
    printf("%s\n", where);
    dmf_check(dmf, 10);
}

/*
 * Catch a signal and pause until a debugger can be attached.
 */
void catch_sig(int sig)
{
    if (sig == SIGUSR1) // Parent asked us to die
        exit(SIGUSR1);

    stop++;
    signal(SIGUSR1, SIG_IGN); // do not let parent kill us
    error(0,0,"Caught signal %d, pid %d. Waiting until a debugger is attached",
            sig, getpid());
    while(1){
        sleep(1);
    }
}

/*
 * Catch a segv signal and pause until a debugger can be attached.
 */
struct sigaction segvact;
siginfo_t segvinfo;
void catch_segv(int sig, siginfo_t *info, void *context)
{
    stop++;
    signal(SIGUSR1, SIG_IGN); // do not let parent kill us
    error(0,0,"Caught signal %d, pid %d. Waiting until a debugger is attached",
            sig, getpid());
    while(1){
        sleep(1);
    }
}

/*
 * This is the main entry point for the driver
 */
int main(int argc, char** argv)
{
    const char *fname = "/tmp/dmf";
    if (argc == 2)
        fname = argv[1];

    /* Fork children to do the real work. Kill them after a few seconds. If
     * FORKS is zero this will only run the parent process. */
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
        kill(pid, SIGUSR1);
        int status;
        wait(&status);
        if (status == 0)
        {
            /* on some successful completions delete the main region so
             * that create file gets tested */
            if (x & 1)
                dmf_delete(fname);
            y++;
        }
        else if (status == SIGUSR1*256)
            z++;
        else
        {
            printf("Unexpected exit status %d\n", status);
            exit(1);
        }
        printf("******************** Processes killed: %d, completed %d\n\n", z, y);
    }

    /*
     * Pause for debugger attach if a signal occurs
     */
    signal(SIGILL, &catch_sig);
    signal(SIGBUS, &catch_sig);
    signal(SIGSTKFLT, &catch_sig);
    signal(SIGUSR1, &catch_sig);
    segvact.sa_flags = SA_SIGINFO;
    segvact.sa_sigaction = catch_segv;
    sigaction(SIGSEGV, &segvact, NULL);

    /* initialize main thread */
    nvm_thread_init();

    /* initialize mutex held while resizing */
    pthread_mutex_init(&size_mutex, NULL);

    /* If opening the file does not work then create a new one. Open will
     * run recovery if needed. */
    int run = 0;
    ub4 blkno;
    ub8 buf[BUF_SIZE];
again:;
    dmf_file *dmf = dmf_open(fname, NULL);
    if (!dmf && run != 0) // could not open after close
        error(1, errno, "could not reopen after close");
    if (!dmf)
    {
        dmf_delete(fname);
        dmf = dmf_create(fname, BLOCK_SIZE, FILE_SIZE, FILE_SIZE*2, NULL,
                THREADS*2, 0777);
        test_check(dmf, "Creation complete");
    } else {
        test_check(dmf, "Opened");

        /* found a good file - insure contents are valid */
        dmf_stat dstat;
        dmf_query(dmf, &dstat);
        if (dstat.blksz != BLOCK_SIZE)
            error(1, 0, ": %s : Block size is %lu not %u", fname,
                    dstat.blksz, BLOCK_SIZE);
        for (blkno = 0; blkno < dstat.filesz; blkno++)
        {
            /* read the next block */
            dmf_read(dmf, blkno, buf, 1);

            /* A block of all zero can be the result of death after grow and
             * before initializing the new block, so allow that.  */
            if ((buf[0] | buf[1] | buf[2] | buf[BUF_SIZE - 1]) == 0)
                continue;

            /* Verify the contents are what we wrote */
            const char *msg = check_buf(buf, blkno);
            if (msg)
            {
                errno = EIO; catch_err();
                error(1, 0, "%s : %s :", fname, msg);
            }
        }
    }

    /* Reset the file size to the initial size and initialize its contents. It
     * is possible that the last run failed after commit of a grow and before
     * the new blocks were initialized here. */
    dmf_resize(dmf, FILE_SIZE);

    /* initialize file to have valid blocks */
    for (blkno = 0; blkno < FILE_SIZE; blkno++)
    {
        fill_buf(buf, blkno);
        dmf_write(dmf, blkno, buf, 1);
    }
    for (blkno = 0; blkno < FILE_SIZE; blkno++)
    {
        dmf_read(dmf, blkno, buf, 1);
        const char *msg = check_buf(buf, blkno);
        if (msg)
        {
            errno = EIO; catch_err();
            error(1, EIO, "%s: %s: ", fname, msg);
        }
    }
    test_check(dmf, "initialization complete");
    usable_size = FILE_SIZE;

    /*
     * Start up the worker threads and wait for them to complete
     */
    printf("Start worker threads\n");
    pthread_t threads[THREADS];
    struct counts tcnts[THREADS];
    memset(tcnts, 0, sizeof(tcnts));
    struct counts cnts;
    memset(&cnts, 0, sizeof(cnts));
    int t;
    for (t = 0; t < THREADS; t++)
    {
        tcnts[t].thread = t + 1;
        tcnts[t].dmf = dmf;
        pthread_create(&threads[t], NULL, &thread_func, &tcnts[t]);
    }
    for (t = 0; t < THREADS; t++)
    {
        pthread_join(threads[t], NULL);
        cnts.writes += tcnts[t].writes;
        cnts.reads += tcnts[t].reads;
        cnts.grows += tcnts[t].grows;
        cnts.shrinks += tcnts[t].shrinks;
    }
    test_check(dmf, "run complete");
    printf("successful run with %llu writes and %llu reads"
            " %llu grows %llu shrinks\n", cnts.writes, cnts.reads,
            cnts.grows, cnts.shrinks);
    dmf_close(dmf);
    if (FORKS < 0)
    {
        printf("%d runs completed. Go again\n", ++run);
         goto again;
    }
    return 0;
}

