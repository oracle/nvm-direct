
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
/**\file
    NAME\n
      nvm_transaction.c - Non-Volatile Memory atomic transactions

    DESCRIPTION\n
      This file implements NVM transactions that provide the ability to
      atomically update multiple NVM locations.

    NOTES\n
      Unlike volatile memory, NVM retains its contents through a power failure
      or processor reset. This can happen at any time. If it happens in the
      middle of updating a data structure such as a doubly linked list, then a
      mechanism is needed to restore the linked list to a consistent state. This
      problem can also arise when an application process throws an error or dies
      while manipulating persistent data structures.

      A transaction mechanism is needed to atomically modify an arbitrary set of
      locations in NVM. Before a transactional store to NVM, the old value of
      the location will be saved in an NVM transaction data structure. This is
      the undo to apply if the transaction aborts or partially rolls back. When
      all stores for the transaction are complete, the undo is deleted thus
      committing the transaction. The application may choose to abort a
      transaction applying all the undo generated so far. An application may
      also create a savepoint and later apply all undo generated after the
      savepoint was created. This is a partial rollback of the transaction.

      In addition to the above description of saving old values, we need the
      ability to associate NVM locks with the transaction. A multi-threaded
      application uses NVM locks to ensure only one thread at a time is
      modifying an NVM data structure. Such a lock needs to be held until any
      undo that modifies the data structure is either applied by rollback/abort
      or discarded by transaction commit. Thus the lock is associated with the
      transaction rather than the thread that acquired the lock. Since the
      transactions survive process death the locks need to be in NVM just as
      the transaction data.

      When an application attaches to an NVM region, the region is checked to
      see if there are any transactions that need recovery. Recovery is
      completed before the attach function returns to the application. This
      ensures the application only sees a consistent state in NVM after a
      failure of the previous process to attach to the region. If recovery
      cannot be completed due to some error, then the attach fails. Note that
      this means a transaction can only modify one NVM region at a time because
      recovery happens when the region is attached and regions are attached one
      at a time. It is not possible to atomically modify data in two different
      regions.

\par  On abort operations

      An application can create an operation to be executed if the active
      transaction rolls back through the current point in the undo. This
      creates an undo record with a pointer to a persistent application
      function and a persistent struct to pass to the function. If the undo is
      applied, the function is called in a new nested transaction. The nested
      transaction is committed when the function returns.

      Creating an on abort operation returns a pointer to the struct in the
      undo record so that arguments to the function can be modified later. It
      is common to have a nested transaction do a transactional store into the
      on abort operation so that it does nothing unless the nested transaction
      commits. This can be used to implement logical undo to reverse the
      effects of a nested transaction that committed.

      Note that the function must not signal any errors, exit the process, or
      abort the nested transaction. This could result in an NVM region that
      could never be attached since every time recovery is attempted it would
      fail. Thus the only errors must be the result of serious NVM corruption.

      Note that the function could be called at recovery time so it must not
      attempt to access any volatile data structures created by the process
      that created the on abort operation.

\par  On commit operations

      An application can create an on commit operation using a mechanism that
      is the same as creating an on abort operation except that the undo record
      does nothing if rolled back. All the on commit operations in a
      transaction are executed after the transaction has committed and all its
      locks released. While undo is applied in the reverse of the order it is
      created in, on commit operations are executed in the same order as they
      are created. Each on commit function is called in its own nested
      transaction that is committed when it returns.

      An on commit operation must not signal any errors, exit the process, or
      abort the nested transaction. This would make it impossible to complete
      the commit of a committed transaction. If an error is signaled or the
      nested transaction aborts, the application process exits. Recovery will
      first rollback the nested transaction then resume commit of the parent
      transaction beginning with the on commit operation that failed. If it
      fails again the region cannot be attached.

      Note that the function could be called at recovery time so it must not
      attempt to access any volatile data structures created by the process
      that created the on commit operation.

\par  On unlock operations

      An application can create an on unlock operation using a mechanism that
      is the same as creating an on abort or on commit operation except that
      the undo record is applied both when its transaction aborts or when it
      commits. When a transaction aborts all undo records including on unlock
      records are applied in reverse order. When a transaction commits all its
      locks are released in reverse order. An on unlock operation is treated
      like an nvm_mutex unlock record and applied while previously acquired
      locks are still held. Each on unlock function is called in its own nested
      transaction that is committed when it returns.

      An on unlock operation must not signal any errors, exit the process, or
      abort the nested transaction. This would make it impossible to complete
      the commit or abort of a transaction. If an error is signaled or the
      nested transaction aborts, the application process exits. Recovery will
      first rollback the nested transaction then resume commit or abort of the
      parent transaction beginning with the on unlock operation that failed.
      If it fails again the region cannot be attached.

      Note that the function could be called at recovery time so it must not
      attempt to access any volatile data structures created by the process
      that created the on commit operation.

\par  Nested transactions

      Nested transactions suspend operation of the current transaction so that
      all transactional operations happen in the nested transaction. The nested
      transaction can commit or abort independently from its parent
      transaction. When it commits its changes are persistent even if the
      parent transaction aborts. The parent of a nested transaction can itself
      be a nested transaction.

      Locks acquired by a nested transaction are released when the nested
      transaction commits/aborts. This makes nested transactions useful for
      avoiding deadlocks and for reducing the time a lock is held so that there
      is less contention. For example allocation from a heap is done in a
      nested transaction. An on abort operation is created in the parent
      transaction before the nested transaction is started. If the allocation
      commits, but the parent transaction aborts, then the on abort operation
      undoes the committed allocation.

      On commit operations created during a nested transaction are executed
      when the nested transaction commits. The parent transaction does not
      become the current transaction until all the on commit operations
      complete and the nested transaction code block is exited.

\par  Explicit Commit/Abort

      The current transaction can be explicitly committed or aborted by calling
      a library function. . The abort function returns when all undo has been
      applied up to the beginning of the transaction. The commit function
      returns when all on commit operations have completed. In either case all
      locks acquired by the current transaction are released.

      Once a transaction is explicitly committed/aborted it is still the
      current transaction, but it cannot be used for any more transactional
      operations. Explicit commit/abort of a nested transaction does not make
      the parent transaction current until the nested transaction is ended. A
      transaction is defined by a specially annotated code block. The
      transaction stays current until the code block is exited either by an
      explicit transfer, executing  out the bottom of the block, or an NVM
      longjmp to an NVM setjmp outside the code block.

      There is a library routine to report the current transaction nesting
      level and the current transaction status. A current transaction can be
      active, committed, or aborted. A nesting level of 0 means there is no
      current transaction. A nesting level of 1 means the base transaction is
      current.

 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include "nvm.h"
#include "nvm_data0.h"
#include "nvm_region0.h"
#include "nvm_transaction0.h"
#include "nvms_misc.h"
#include "nvms_region.h"
#include "nvms_sleeping.h"
#include "nvms_locking.h"
#include "nvm_heap0.h"

#ifdef NVM_EXT
static void nvm_prep_undo(nvm_transaction ^tx);
static void nvm_count_undo(nvm_transaction ^tx);
static nvm_trans_state nvm_get_state(nvm_transaction ^tx);
static void nvm_add_undo@(nvm_transaction ^tx, nvm_thread_data *td);
static void nvm_drop_undo@(nvm_transaction ^tx, nvm_undo_blk ^ub,
        nvm_thread_data *td);
static void nvm_add_oper@(nvm_transaction ^tx, nvm_opcode op, size_t bytes);
#else
static void nvm_prep_undo(nvm_transaction *tx);
static void nvm_count_undo(nvm_transaction *tx);
static nvm_trans_state nvm_get_state(nvm_transaction *tx);
static void nvm_add_undo(nvm_transaction *tx, nvm_thread_data *td);
static void nvm_drop_undo(nvm_transaction *tx, nvm_undo_blk *ub,
        nvm_thread_data *td);
static void nvm_add_oper(nvm_transaction *tx, nvm_opcode op, size_t bytes);
#endif //NVM_EXT
/**
 * This begins a new transaction. If there is no current transaction then
 * a new base transaction is started in the indicated NVM region. If there
 * is a current transaction then a nested transaction is begun. For
 * beginning a nested transaction the region descriptor can be zero to
 * begin it in the same region as the current transaction. If it is not
 * zero and not the same as the current transaction then the nested
 * transaction is an off region nested transaction in that region.
 *
 * This is not normally called directly from the application. Normally "@"
 * is used to begin a transactional code block. The precompiler inserts
 * the appropriate nvm_txbegin call at the beginning of the code block.
 *
 * If there are already too many allocated transactions, this call may have
 * to block until some other transaction commits or aborts. Note that a
 * nested transaction in the same region as the current transaction uses
 * the same transaction slot as the base transaction so it never sleeps
 * waiting for a slot. An off region nested transaction has to allocate
 * a transaction slot in the new region.
 *
 * An assert is fired if there is an error such as passing a region
 * descriptor of zero when starting a base transaction.
 *
 * @param[in] region
 * This is the region descriptor returned by nvm_create_region or
 * nvm_attach_region for the region to modify in the transaction.
 *
 * @param[in] region
 * This is the region descriptor returned by nvm_create_region or
 * nvm_attach_region for the region to modify in the transaction.
 */
#ifdef NVM_EXT
void nvm_txbegin(
        nvm_desc desc
        )
{
    /* get app, thread and process data pointers */
    nvm_app_data *ad = nvm_get_app_data();
    nvm_thread_data *td = nvm_get_thread_data();

    /* Region and transaction pointers */
    nvm_region_data *rd = NULL;
    nvm_transaction ^ptx = td->transaction; // parent transaction

    /* Decide if a new transaction slot is needed. If this is a base transaction
     * or an off region nested transaction then a new slot is needed. */
    if (ptx == 0 || (desc != 0 && ptx=>desc != desc))
    {
        /* Error if requested descriptor is too large or zero */
        nvm_desc rmax = ad->params.max_regions;
        if (desc > rmax || desc == 0)
            nvms_assert_fail("Invalid descriptor for begin transaction");

        /* Error if desc is not for a valid region. Note that we do not hold
         * the app data mutex here so it is possible for another thread to
         * do a detach that releases the region data while we are beginning
         * a transaction. There are some checks to assert if this happens,
         * but they are not reliable. It is an app bug to simultaneously
         * begin a transaction and detach a region so it does not seem worth
         * it to get the app data mutex on every transaction begin. */
        rd = ad->regions[desc];
        if (rd == NULL)
            nvms_assert_fail("Invalid region for begin transaction");

        /* Error if desc is for the same region as an existing ancestor
         * transaction. This is disallowed because I have not thought of a
         * mechanism to ensure the decendent transaction is recovered first
         * when they are in the same region. Between regions the recovery
         * order does not matter, but it could matter in the same region.
         *
         * Another way of describing this restriction is to say that a thread
         * can only own at most one transaction slot per region.
         *
         * The main use case for off region nested transactions is to run
         * an upgrade when reading data from a different region than the
         * current transaction region. Upgrade should not need to look at
         * another region so this is not a significant restriction.
         *
         * Note that allowing this would also require adding the ancestor
         * undo block count to the new transaction for comparison with max
         * undo blocks.
         */
        nvm_transaction ^atx = ptx; // ancestor transaction
        while (atx)
        {
            if (atx=>desc == desc)
                nvms_assert_fail("Duplicate child transaction region");
            atx = atx=>parent;
        }

        /* allocate a transaction slot for this base transaction */
        nvm_trans_table_data *ttd = rd->trans_table;

        /* lock mutex that protects Transaction allocate/deallocate */
        nvms_lock_mutex(ttd->trans_mutex, 1);

        /* wait if there are none free.*/
        while (ttd->free_trans == 0)
            nvms_cond_wait(ttd->trans_cond, ttd->trans_mutex, (uint64_t)0);

        /* If the region is marked for detach then fail */
        if (ttd->detaching)
        {
            nvms_unlock_mutex(ttd->trans_mutex);
            nvms_assert_fail("Begin transaction in detaching region");
        }

        /* use the nvm_transaction on the head of the free list */
        nvm_transaction ^tx = ttd->free_trans;
        tx=>desc = desc; // just in case it was trashed

        /* Remove it from the free list */
        ttd->free_trans = tx=>link;
        tx=>link = 0;
        tx=>txnum ~= ++(ttd->txnum);
        ttd->atrans_cnt++;

        /* Ensure the transaction is empty */
        tx=>undo ~= 0;
        tx=>commit_ops ~= 0;
        tx=>held_locks ~= 0;
        tx=>savept_last ~= 0;
        tx=>nstd_last ~= 0;
        tx=>dead = 0;
        tx=>spawn_cnt = 0;
        tx=>parent = ptx;
        tx=>undo_ops = 0;
        tx=>max_undo_blocks ~= rd->region=>max_undo_blocks;
        tx=>cur_undo_blocks = 0;
        nvm_prep_undo(tx);

        /* Ensure the transaction state is persistently stored after the
         * transaction is persistently initialized */
        nvm_persist();
        tx=>state ~= nvm_active_state;
        nvm_persist();
        td->persist_tx += 2;

        /* Set this as the current transaction for this thread. */
        td->transaction = tx;
        td->region = ttd->region;
        td->txdepth = ptx ? td->txdepth + 1 : 1;
        td->trans_cnt++;

        /* Transaction is ready unlock the mutex. */
        nvms_unlock_mutex(ttd->trans_mutex);
    }
    else
    { extern @ {
        /* This is beginning a nested transaction in parent region. */
        nvm_transaction ^tx = ptx;

        /* The current transaction must be active, aborting or committing */
        switch (nvm_get_state(tx))
        {
        default:
            nvms_assert_fail("Nested transaction in inactive transaction");
            break;
        case nvm_active_state:
        case nvm_aborting_state:
        case nvm_committing_state:
        case nvm_rollback_state:
            break;
        }

        /* If we cannot get the struct in this block, allocate a new undo
         * block. */
        const size_t size = sizeof(nvm_nested);
        if (tx=>undo_bytes < size)
            nvm_add_undo(tx, td); // add a new empty undo block

        /* Get the address to store the data for the operation. */
        nvm_nested ^nt = (nvm_nested ^)(tx=>undo_data - size);

        /* Initialize the contents of the nvm_nested. Set the link to the
         * other nested transactions in this transaction, and the new state */
        nt=>undo_ops ~= tx=>undo_ops;
        nt=>prev ~= tx=>nstd_last;
        nt=>state ~= nvm_active_state;

        /* add the nvm_nested undo record to the transaction. */
        nvm_add_oper(tx, nvm_op_nested, size);

        /* The new nested transaction record is persistently part of the undo
         * but it is not in the linked list of nested transactions. If we die
         * here, rollback will ignore the record. By changing the pointer to
         * the head of the list, we atomically start the nested transaction. */
        tx=>nstd_last ~= nt;
        nvm_persist();

        /* 2 from nvm_add_oper and 1 here. */
        td->persist_tx += 3;

        /* one more transaction deep */
        td->txdepth++;
    }}
}
#else
void nvm_txbegin(
        nvm_desc desc
        )
{
    /* get app, thread and process data pointers */
    nvm_app_data *ad = nvm_get_app_data();
    nvm_thread_data *td = nvm_get_thread_data();

    /* Region and transaction pointers */
    nvm_region_data *rd = NULL;
    nvm_transaction *ptx = td->transaction; // parent transaction
    nvm_verify(ptx, shapeof(nvm_transaction));

    /* Decide if a new transaction slot is needed. If this is a base transaction
     * or an off region nested transaction then a new slot is needed. */
    if (ptx == 0 || (desc != 0 && ptx->desc != desc))
    {
        /* Error if requested descriptor is too large or zero */
        nvm_desc rmax = ad->params.max_regions;
        if (desc > rmax || desc == 0)
            nvms_assert_fail("Invalid descriptor for begin transaction");

        /* Error if desc is not for a valid region. Note that we do not hold
         * the app data mutex here so it is possible for another thread to
         * do a detach that releases the region data while we are beginning
         * a transaction. There are some checks to assert if this happens,
         * but they are not reliable. It is an app bug to simultaneously
         * begin a transaction and detach a region so it does not seem worth
         * it to get the app data mutex on every transaction begin. */
        rd = ad->regions[desc];
        if (rd == NULL)
            nvms_assert_fail("Invalid region for begin transaction");

        /* Error if desc is for the same region as an existing ancestor
         * transaction. This is disallowed because I have not thought of a
         * mechanism to ensure the decendent transaction is recovered first
         * when they are in the same region. Between regions the recovery
         * order does not matter, but it could matter in the same region.
         *
         * Another way of describing this restriction is to say that a thread
         * can only own at most one transaction slot per region.
         *
         * The main use case for off region nested transactions is to run
         * an upgrade when reading data from a different region than the
         * current transaction region. Upgrade should not need to look at
         * another region so this is not a significant restriction.
         *
         * Note that allowing this would also require adding the ancestor
         * undo block count to the new transaction for comparison with max
         * undo blocks.
         */
        nvm_transaction *atx = ptx; // ancestor transaction
        while (atx)
        {
            if (atx->desc == desc)
                nvms_assert_fail("Duplicate child transaction region");
            atx = atx->parent;
            nvm_verify(atx, shapeof(nvm_transaction));
        }

        /* allocate a transaction slot for this base transaction */
        nvm_trans_table_data *ttd = rd->trans_table;

        /* lock mutex that protects Transaction allocate/deallocate */
        nvms_lock_mutex(ttd->trans_mutex, 1);

        /* wait if there are none free.*/
        while (ttd->free_trans == NULL)
            nvms_cond_wait(ttd->trans_cond, ttd->trans_mutex, (uint64_t)0);

        /* If the region is marked for detach then fail */
        if (ttd->detaching)
        {
            nvms_unlock_mutex(ttd->trans_mutex);
            nvms_assert_fail("Begin transaction in detaching region");
        }

        /* use the nvm_transaction on the head of the free list */
        nvm_transaction *tx = ttd->free_trans;
        nvm_verify(tx, shapeof(nvm_transaction));
        tx->desc = desc; // just in case it was trashed

        /* Remove it from the free list */
        ttd->free_trans = tx->link;
        tx->link = 0;
        tx->txnum = ++(ttd->txnum);
        ttd->atrans_cnt++;

        /* Ensure the transaction is empty */
        nvm_undo_blk_set(&tx->undo, NULL);
        nvm_on_commit_set(&tx->commit_ops, NULL);
        nvm_lkrec_set(&tx->held_locks, NULL);
        nvm_savepnt_set(&tx->savept_last, NULL);
        nvm_nested_set(&tx->nstd_last, NULL);
        tx->dead = 0;
        tx->spawn_cnt = 0;
        tx->parent = ptx;
        tx->undo_ops = 0;
        tx->max_undo_blocks = rd->region->max_undo_blocks;
        tx->cur_undo_blocks = 0;
        nvm_prep_undo(tx);

        /* Ensure the transaction state is persistently stored after the
         * transaction is persistently initialized */
        nvm_flush(tx, sizeof(*tx));
        nvm_persist();
        tx->state = nvm_active_state;
        nvm_persist1(&tx->state);
        td->persist_tx += 2;

        /* Set this as the current transaction for this thread. */
        td->transaction = tx;
        td->region = ttd->region;
        td->txdepth = ptx ? td->txdepth + 1 : 1;
        td->trans_cnt++;

        /* Transaction is ready unlock the mutex. */
        nvms_unlock_mutex(ttd->trans_mutex);
    }
    else
    {
        /* This is beginning a nested transaction in parent region. First
         * verify the nvm_transaction is valid */
        nvm_transaction *tx = ptx;
        nvm_verify(tx, shapeof(nvm_transaction));

        /* The current transaction must be active, aborting or committing */
        switch (nvm_get_state(tx))
        {
        default:
            nvms_assert_fail("Nested transaction in inactive transaction");
            break;
        case nvm_active_state:
        case nvm_aborting_state:
        case nvm_committing_state:
        case nvm_rollback_state:
            break;
        }

        /* If we cannot get the struct in this block, allocate a new undo block.
         * Otherwise verify current undo block */
        const size_t size = sizeof(nvm_nested);
        if (tx->undo_bytes < size)
            nvm_add_undo(tx, td); // add a new empty undo block
        else
            nvm_verify(nvm_undo_blk_get(&tx->undo), shapeof(nvm_undo_blk));

        /* Get the address to store the data for the operation. */
        nvm_nested *nt = (nvm_nested *)(tx->undo_data - size);

        /* Initialize the contents of the nvm_nested. Set the link to the
         * other nested transactions in this transaction, and the new state */
        nt->undo_ops = tx->undo_ops;
        nvm_nested_set(&nt->prev, nvm_nested_get(&tx->nstd_last));
        nt->state = nvm_active_state;
        nvm_flush(nt, sizeof(*nt));

        /* add the nvm_nested undo record to the transaction. */
        nvm_add_oper(tx, nvm_op_nested, size);

        /* The new nested transaction record is persistently part of the undo
         * but it is not in the linked list of nested transactions. If we die
         * here, rollback will ignore the record. By changing the pointer to
         * the head of the list, we atomically start the nested transaction. */
        nvm_nested_set(&tx->nstd_last, nt);
        nvm_persist1(&tx->nstd_last);

        /* 2 from nvm_add_oper and 1 here. */
        td->persist_tx += 3;

        /* one more transaction deep */
        td->txdepth++;
    }
}
#endif //NVM_EXT

/**
 * This ends the current transaction. If this is a nested transaction then
 * the parent becomes the current transaction. If this is a base
 * transaction without a parent, then the transaction slot is made
 * available for a new transaction, and there is no current transaction any
 * more.
 *
 * This is not normally called directly from the application. Normally the
 * preprocessor inserts the nvm_txend call at the end of the transaction
 * code block or before any transfer of control out of the transaction code
 * block.
 *
 * If the current transaction is not committed or aborted then it is committed.
 */
#ifdef NVM_EXT
void nvm_txend@()
{
    /* get app and thread data pointers */
    nvm_app_data *ad = nvm_get_app_data();
    nvm_thread_data *td = nvm_get_thread_data();

    /* Verify the current base transaction is valid. */
    nvm_transaction ^tx = td->transaction;
    if (tx == 0)
        nvms_assert_fail("Ending transactions when there is none");

    /* If ending a local nested transaction  */
    nvm_nested ^nt = tx=>nstd_last;
    if (nt != 0)
    {
        /* If transaction is still active then commit it. */
        if (nt=>state == nvm_active_state)
            nvm_commit();

        /* verify the state is reasonable */
        if (nt=>state != nvm_committed_state
            && nt=>state != nvm_aborted_state)
            nvms_assert_fail(
                "Ending nested transaction before commit or abort");

        /* Get the undo block containing the nvm_nested record. The last
         * record had better be our record. */
        nvm_undo_blk ^ub = tx=>undo;
        if (tx=>undo_data != (uint8_t^)nt)
            nvms_corruption("Undo inconsistent with nested transaction state",
                tx=>undo_data, nt);

        /* First remove the nvm_nested undo record from the list of nested
         * transactions. This can leave the undo record in the transaction,
         * but it is no longer the current transaction. */
        tx=>nstd_last ~= nt=>prev;

        /* Adjust the transient data to remove the undo record for the nested
         * transaction */
        tx=>undo_ops--;
        tx=>undo_bytes += sizeof(nvm_nested);
        tx=>undo_data += sizeof(nvm_nested);

        /* Persistently remove this operation from the transaction. This might
         * leave the first block of undo empty. This is not a problem and
         * can save an immediate undo block allocation */
        ub=>count~--;
        nvm_persist();
        td->persist_tx += 2;

        /* The nested transaction is no longer current. */
        td->txdepth--;
        return;
    }

    /* If transaction is still active then commit it. */
    if (tx=>state == nvm_active_state)
        nvm_commit();

    /* End the transaction since there is no local nested transaction. First
     * verify it is in a valid state for ending. All undo blocks should have
     * been returned to the freelist due to commit/abort. */
    if (tx=>state != nvm_committed_state && tx=>state != nvm_aborted_state)
        nvms_assert_fail("Ending base transaction before commit or abort");
    if (tx=>undo != 0)
        nvms_corruption("Ended transaction still has undo", tx, NULL);

    /* lock mutex that protects Transaction allocate/deallocate */
    nvm_region_data *rd = ad->regions[tx=>desc];
    nvm_trans_table_data *ttd = rd->trans_table;
    nvms_lock_mutex(ttd->trans_mutex, 1);

    /* Get the current head of the freelist */
    nvm_transaction ^ntx = ttd->free_trans;

    /* Decide if this transaction should be returned to the freelist. If
     * this transaction slot number indicates it should be reserved then
     * it does not go on the freelist. This can only happen if the
     * transaction was in use when the maximum number of transactions is
     * reduced by nvm_set_txconfig. */
    int reserved = tx=>slot > td->region=>max_transactions;

    /* Determine if this transaction is acutally an off region nested
     * transaction for a parent transaction in another region.
     */
    nvm_transaction ^ptx = tx=>parent;

    /* Mark the transaction as idle if it can go back to the freelist. If
     * this transaction slot number indicates it should be reserved then set
     * its state to reserved so it does not go on the freelist. This can only
     * happen if the transaction was in use when the maximum number of
     * transactions is reduced by nvm_set_txconfig. */
    tx=>state ~= reserved ? nvm_reserved_state : nvm_idle_state;
    nvm_persist();
    td->persist_tx++;

    /* Return the transaction to the freelist if idle. Note that the freelist
     * is all transient because it is rebuilt at region recovery. */
    if (tx=>state == nvm_idle_state)
    {
        tx=>link = ntx;
        ttd->free_trans = tx;
    }
    else
    {
        tx=>link = 0;
    }

    /* Update transaction statistics. */
    if (!tx=>dead)
    {
        /* one less active transaction */
        ttd->atrans_cnt--;
    }
    else
    {
        /* One less dead transaction. */
        ttd->dtrans_cnt--;

        /* If this brings the count to zero there could be a thread waiting
         * for this. Post the condition just in case. This is pretty rare
         * so no need to worry about excess posts. */
        if (ttd->dtrans_cnt == 0)
            nvms_cond_post(ttd->trans_cond, 1);

        tx=>dead = 0; // no longer dead
    }

    /* transaction is ended so we can drop the mutex */
    nvms_unlock_mutex(ttd->trans_mutex);

    /* Fix up thread private data */
    if (ptx)
    {
        td->transaction = ptx;
        td->region = ad->regions[ptx=>desc]->region;
        td->txdepth--;
    } else {
        td->transaction = 0;
        td->region = 0; // next transaction could be a different region
        td->txdepth = 0;
    }
}
#else
void nvm_txend()
{
    /* get app and thread data pointers */
    nvm_app_data *ad = nvm_get_app_data();
    nvm_thread_data *td = nvm_get_thread_data();

    /* Verify the current base transaction is valid. */
    nvm_transaction *tx = td->transaction;
    if (tx == NULL)
        nvms_assert_fail("Ending transactions when there is none");
    nvm_verify(tx, shapeof(nvm_transaction));

    /* If ending a local nested transaction  */
    nvm_nested *nt = nvm_nested_get(&tx->nstd_last);
    if (nt != NULL)
    {
        /* If transaction is still active then commit it. */
        if (nt->state == nvm_active_state)
            nvm_commit();

        /* verify the state is reasonable */
        if (nt->state != nvm_committed_state
            && nt->state != nvm_aborted_state)
            nvms_assert_fail(
                "Ending nested transaction before commit or abort");

        /* Get the undo block containing the nvm_nested record. The last
         * record had better be our record. */
        nvm_undo_blk *ub = nvm_undo_blk_get(&tx->undo);
        nvm_verify(ub, shapeof(nvm_undo_blk)); // verify undo block
        if (tx->undo_data != (uint8_t*)nt)
            nvms_corruption("Undo inconsistent with nested transaction state",
                tx->undo_data, nt);

        /* First remove the nvm_nested undo record from the list of nested
         * transactions. This can leave the undo record in the transaction,
         * but it is no longer the current transaction. */
        nvm_nested_set(&tx->nstd_last, nvm_nested_get(&nt->prev));
        nvm_flush1(&tx->nstd_last);

        /* Adjust the transient data to remove the undo record for the nested
         * transaction */
        tx->undo_ops--;
        tx->undo_bytes += sizeof(nvm_nested);
        tx->undo_data += sizeof(nvm_nested);

        /* Persistently remove this operation from the transaction. This might
         * leave the first block of undo empty. This is not a problem and
         * can save an immediate undo block allocation */
        ub->count--;
        nvm_flush1(&ub->count);
        nvm_persist();
        td->persist_tx += 2;

        /* The nested transaction is no longer current. */
        td->txdepth--;
        return;
    }

    /* If transaction is still active then commit it. */
    if (tx->state == nvm_active_state)
        nvm_commit();

    /* End the transaction since there is no local nested transaction. First
     * verify it is in a valid state for ending. All undo blocks should have
     * been returned to the freelist due to commit/abort. */
    if (tx->state != nvm_committed_state && tx->state != nvm_aborted_state)
        nvms_assert_fail("Ending base transaction before commit or abort");
    if (nvm_undo_blk_get(&tx->undo) != NULL)
        nvms_corruption("Ended transaction still has undo", tx, NULL);

    /* lock mutex that protects Transaction allocate/deallocate */
    nvm_region_data *rd = ad->regions[tx->desc];
    nvm_trans_table_data *ttd = rd->trans_table;
    nvms_lock_mutex(ttd->trans_mutex, 1);

    /* Verify the current head of the freelist is valid */
    nvm_transaction *ntx = ttd->free_trans;
    nvm_verify(ntx, shapeof(nvm_transaction));

    /* Decide if this transaction should be returned to the freelist. If
     * this transaction slot number indicates it should be reserved then
     * it does not go on the freelist. This can only happen if the
     * transaction was in use when the maximum number of transactions is
     * reduced by nvm_set_txconfig. */
    int reserved = tx->slot > td->region->max_transactions;

    /* Determine if this transaction is actually an off region nested
     * transaction for a parent transaction in another region.
     */
    nvm_transaction *ptx = tx->parent;

    /* Mark the transaction as idle if it can go back to the freelist. If
     * this transaction slot number indicates it should be reserved then set
     * its state to reserved so it does not go on the freelist. This can only
     * happen if the transaction was in use when the maximum number of
     * transactions is reduced by nvm_set_txconfig. */
    tx->state = reserved ? nvm_reserved_state : nvm_idle_state;
    nvm_persist();
    td->persist_tx++;

    /* Return the transaction to the freelist if idle. Note that the freelist
     * is all transient because it is rebuilt at region recovery. */
    if (tx->state == nvm_idle_state)
    {
        tx->link = ntx;
        ttd->free_trans = tx;
    }
    else
    {
        tx->link = 0;
    }

    /* Update transaction statistics. */
    if (!tx->dead)
    {
        /* one less active transaction */
        ttd->atrans_cnt--;
    }
    else
    {
        /* One less dead transaction. */
        ttd->dtrans_cnt--;

        /* If this brings the count to zero there could be a thread waiting
         * for this. Post the condition just in case. This is pretty rare
         * so no need to worry about excess posts. */
        if (ttd->dtrans_cnt == 0)
            nvms_cond_post(ttd->trans_cond, 1);

        tx->dead = 0; // no longer dead
    }

    /* transaction is ended so we can drop the mutex */
    nvms_unlock_mutex(ttd->trans_mutex);

    /* Fix up thread private data */
    if (ptx)
    {
        td->transaction = ptx;
        td->region = ad->regions[ptx->desc]->region;
        td->txdepth--;
    } else {
        td->transaction = 0;
        td->region = 0; // next transaction could be a different region
        td->txdepth = 0;
    }
}
#endif //NVM_EXT

/**
 * This returns the current transaction nesting depth. If there is no
 * current transaction then the depth is zero. If the current transaction
 * is the base transaction then the depth is one. If the current
 * transaction is a nested transaction then the depth is two or more
 * depending on how deep it is nested. There is no specific limit on
 * nesting, but there is a limit on the amount of undo retained for a base
 * transaction plus all of its nested children.
 *
 * @return
 * The current transaction nesting depth
 */
int nvm_txdepth()
{
    nvm_thread_data *td = nvm_get_thread_data();
    return td ? td->txdepth : 0;
}
/**
 * This returns the status of the current transaction or one of its
 * parents. If the parent value is zero then the current transaction is
 * being queried. If the parent value is 1 then the immediate parent is
 * being queried. If greater than 1 then an earlier parent is being
 * queried. Negative values are an error.
 *
 * The return value is one of the following:
 *
 * - NVM_TX_NONE: There is no such transaction. The value of parent is
 * greater than or equal to the current transaction depth.
 *
 * - NVM_TX_ACTIVE: The transaction is active. Its fate is not yet
 * resolved.
 *
 * - NVM_TX_ROLLBACK: The transaction is still active but is currently
 * being rolled back to a savepoint. Once the rollback completes the
 * transaction will return to status active.
 *
 * - NVM_TX_ABORTING: The transaction is in the middle of aborting.
 * Undo application is not yet complete. The transaction will eventually
 * become aborted when all undo is applied.
 *
 * - NVM_TX_ABORTED: The transaction was successfully aborted, but
 * execution is still within the transaction code block. In other words
 * nvm_txend() has not yet been called.
 *
 * - NVM_TX_COMMITTING: The transaction is in the middle of committing.
 * It may be releasing locks or calling oncommit operations as part of
 * committing. The transaction will eventually become committed when all
 * oncommit operations are complete.
 *
 * - NVM_TX_COMMITTED: The transaction was successfully committed, but
 * execution is still within the transaction code block. In other words
 * nvm_txend() has not yet been called.
 *
 * Note that the callback function for an onabort or oncommit operation is
 * called within its own nested transaction. Thus the status of the current
 * transaction will always be active when the function is entered. Pass a
 * count of 1 to find out the status of the transaction that the operation
 * is part of.
 *
 * @param[in] parent
 * How many parents back to report
 *
 * @return
 * One of the NVM_TX_* values giving the transaction state.
 */
#ifdef NVM_EXT
int nvm_txstatus(
        int parent
        )
{
    nvm_thread_data *td = nvm_get_thread_data();
    nvm_trans_state state;

    /* Validate parent value is reasonable. */
    if (parent < 0)
        nvms_assert_fail("Negative parent count passed to nvm_txstatus");

    /* If asking about a parent that does not exist return none */
    if (td->txdepth <= parent)
        return NVM_TX_NONE;

    /* Get the current base transaction */
    nvm_transaction ^tx = td->transaction;

    /* Loop back through all transactions until the requested parent is
     * reached. */
    int d = parent;
    nvm_nested ^nt = tx=>nstd_last;
    while (d-- >= 0)
    {
        if (!nt)
        {
            /* No more nested transactions so save the state of the base
             * transaction in this region.
             */
            state = (nvm_trans_state)tx=>state;

            /* If there is a parent transaction in another region, start
             * scanning it.
             */
            tx = tx=>parent;
            if (tx)
                nt = tx=>nstd_last;
        } else {
            /* Save the state of a nested transaction */
            state = nt=>state;
            nt = nt=>prev;
        }
    }

    /* Translate transaction state to return value */
    switch (state)
    {
    case nvm_active_state: return NVM_TX_ACTIVE;
    case nvm_rollback_state: return NVM_TX_ROLLBACK;
    case nvm_aborting_state: return NVM_TX_ABORTING;
    case nvm_aborted_state: return NVM_TX_ABORTED;
    case nvm_committing_state: return NVM_TX_COMMITTING;
    case nvm_committed_state: return NVM_TX_COMMITTED;
    default:
        /* any other state indicates the current transaction state is bad */
        nvms_assert_fail("Impossible transaction state"); // never returns
        return NVM_TX_NONE; // not reachable
    }
}
#else
int nvm_txstatus(
        int parent
        )
{
    nvm_thread_data *td = nvm_get_thread_data();
    nvm_trans_state state;

    /* Validate parent value is reasonable. */
    if (parent < 0)
        nvms_assert_fail("Negative parent count passed to nvm_txstatus");

    /* If asking about a parent that does not exist return none */
    if (td->txdepth <= parent)
        return NVM_TX_NONE;

    /* Verify the current base transaction is valid */
    nvm_transaction *tx = td->transaction;
    nvm_verify(tx, shapeof(nvm_transaction));

    /* Loop back through all transactions until the requested parent is
     * reached. */
    int d = parent;
    nvm_nested *nt = nvm_nested_get(&tx->nstd_last);
    while (d-- >= 0)
    {
        if (!nt)
        {
            /* No more nested transactions so save the state of the base
             * transaction in this region.
             */
            state = (nvm_trans_state)tx->state;

            /* If there is a parent transaction in another region, start
             * scanning it.
             */
            tx = tx->parent;
            nvm_verify(tx, shapeof(nvm_transaction));
            if (tx)
                nt = nvm_nested_get(&tx->nstd_last);
        } else {
            /* Save the state of a nested transaction */
            state = nt->state;
            nt = nvm_nested_get(&nt->prev);
        }
    }

    /* Transalate transaction state to return value */
    switch (state)
    {
    case nvm_active_state: return NVM_TX_ACTIVE;
    case nvm_rollback_state: return NVM_TX_ROLLBACK;
    case nvm_aborting_state: return NVM_TX_ABORTING;
    case nvm_aborted_state: return NVM_TX_ABORTED;
    case nvm_committing_state: return NVM_TX_COMMITTING;
    case nvm_committed_state: return NVM_TX_COMMITTED;
    default:
        /* any other state indicates the current transaction state is bad */
        nvms_assert_fail("Impossible transaction state"); // never returns
        return NVM_TX_NONE; // not reachable
    }
}
#endif //NVM_EXT

/**
 * This returns the region descriptor for the current transaction. It is
 * useful for on abort or on commit callbacks to know which region they
 * are in. It must be called from within a transaction.
 *
 * @return region descriptor of current transaction.
 */
#ifdef NVM_EXT
nvm_desc nvm_txdesc@()
{
    /* Get the current base transaction */
    nvm_thread_data *td = nvm_get_thread_data();
    nvm_transaction ^tx = td->transaction;
    return tx=>desc;
}
#else
nvm_desc nvm_txdesc()
{
    /* Verify the current base transaction is valid */
    nvm_thread_data *td = nvm_get_thread_data();
    nvm_transaction *tx = td->transaction;
    nvm_verify(tx, shapeof(nvm_transaction));
    return tx->desc;
}
#endif //NVM_EXT

/**
 * This returns the transaction slot for the current transaction. It is
 * useful for setting a flag that limits access to a single transaction.
 *
 * It must be called from within a transaction.
 *
 * @return slot number of current transaction.
 */
#ifdef NVM_EXT
uint16_t nvm_txslot@()
{
    /* Get the current base transaction */
    nvm_thread_data *td = nvm_get_thread_data();
    nvm_transaction ^tx = td->transaction;
    return tx=>slot;
}
#else
uint16_t nvm_txslot()
{
    /* Verify the current base transaction is valid */
    nvm_thread_data *td = nvm_get_thread_data();
    nvm_transaction *tx = td->transaction;
    nvm_verify(tx, shapeof(nvm_transaction));
    return tx->slot;
}
#endif //NVM_EXT

/**
 * Add a new block of undo to this transaction and adjust the allocation
 * data. Note this does not need to be persistently atomic because the undo
 * free list is rebuilt if this application dies. However it does need to
 * protect against this thread dying.
 *
 * @param tx
 * The transaction that needs another block of undo
 *
 * @param td
 * The thread data for the thread that owns the transaction.
 */
#ifdef NVM_EXT
void nvm_add_undo@(nvm_transaction ^tx, nvm_thread_data *td)
{
    /* Verify this transaction has not consumed the maximum number of undo
     * blocks it is allowed. */
    if (tx=>cur_undo_blocks >= tx=>max_undo_blocks)
        nvms_assert_fail("Transaction exceeded undo block limit");

    /* Locate the transaction table in volatile memory */
    nvm_app_data *ad = nvm_get_app_data();
    nvm_trans_table_data *ttd = ad->regions[tx=>desc]->trans_table;

    /* Lock the undo free list so we can take a block off it, and wait until
     * there is a block on the free list */
    nvms_lock_mutex(ttd->undo_mutex, 1);
    while (ttd->free_undo == 0)
        nvms_cond_wait(ttd->undo_cond, ttd->undo_mutex, (uint64_t)0);

    /* take the nvm_undo_blk on the head of the free list */
    nvm_undo_blk ^ub = ttd->free_undo;

    /* If the count of undo records is not zero then something awful has
     * happened and there is an in use block on the freelist. */
    if (ub=>count != 0)
        nvms_corruption("Free undo block contains undo", ub, tx);

    /* Unlink the block at the head of the freelist */
    ttd->free_undo = ub=>link;

    /* Link the new block at the head of the transaction's undo block list.
     * Carefully order the updates so that the undo block might be left
     * off the transaction, but never in the transaction without pointing
     * to all the older undo. */
    ub=>txnum ~= tx=>txnum; // debugging data only
    ub=>count ~= 0; // make block empty just in case
    ub=>link ~= tx=>undo; // new block points to old
    nvm_persist(); // persists txnum and count too
    tx=>undo ~= ub; // transaction points at new block
    nvm_persist();
    td->persist_undo += 2;

    /* Release the mutex,committing the allocation */
    nvms_unlock_mutex(ttd->undo_mutex);

    /* Allocating the first operation would consume the first uint64_t in the
     * data area for the first Operation. Save the initial number of bytes
     * of data available and the address at the end of that space. There is
     * no need to force these to be persistent since they are transient. */
    tx=>undo_bytes = sizeof(ub=>data) - sizeof(uint64_t);
    tx=>undo_data = ub=>data + sizeof(ub=>data); // end of block
    tx=>cur_undo_blocks++;
}
#else
void nvm_add_undo(nvm_transaction *tx, nvm_thread_data *td)
{
    /* Verify this transaction has not consumed the maximum number of undo
     * blocks it is allowed. */
    if (tx->cur_undo_blocks >= tx->max_undo_blocks)
        nvms_assert_fail("Transaction exceeded undo block limit");

    /* Locate the transaction table in volatile memory */
    nvm_app_data *ad = nvm_get_app_data();
    nvm_trans_table_data *ttd = ad->regions[tx->desc]->trans_table;

    /* Lock the undo free list so we can take a block off it, and wait until
     * there is a block on the free list */
    nvms_lock_mutex(ttd->undo_mutex, 1);
    while (ttd->free_undo == NULL)
        nvms_cond_wait(ttd->undo_cond, ttd->undo_mutex, (uint64_t)0);

    /* take the nvm_undo_blk on the head of the free list */
    nvm_undo_blk *ub = ttd->free_undo;
    nvm_verify(ub, shapeof(nvm_undo_blk)); // verify it is a valid undo block

    /* If the count of undo records is not zero then something awful has
     * happened and there is an in use block on the freelist. */
    if (ub->count != 0)
        nvms_corruption("Free undo block contains undo", ub, tx);

    /* Unlink the block at the head of the freelist */
    ttd->free_undo = nvm_undo_blk_get(&ub->link);

    /* Link the new block at the head of the transaction's undo block list.
     * Carefully order the updates so that the undo block might be left
     * off the transaction, but never in the transaction without pointing
     * to all the older undo. */
    ub->txnum = tx->txnum; // debugging data only
    ub->count = 0; // make block empty just in case
    nvm_undo_blk_set(&ub->link, nvm_undo_blk_get(&tx->undo)); // new to old
    nvm_persist1(&ub->link); // persists txnum and count too
    nvm_undo_blk_set(&tx->undo, ub); // transaction points at new block
    nvm_persist1(&tx->undo);
    td->persist_undo += 2;

    /* Release the mutex,committing the allocation */
    nvms_unlock_mutex(ttd->undo_mutex);

    /* Allocating the first operation would consume the first uint64_t in the
     * data area for the first Operation. Save the initial number of bytes
     * of data available and the address at the end of that space. There is
     * no need to force these to be persistent since they are transient. */
    tx->undo_bytes = sizeof(ub->data) - sizeof(uint64_t);
    tx->undo_data = ub->data + sizeof(ub->data); // end of block
    tx->cur_undo_blocks++;
}
#endif //NVM_EXT

/**
 * Drop an empty block of undo from this transaction and adjust the allocation
 * data. Note this does not need to be persistently atomic because the undo
 * free list is rebuilt if this application dies. However it does need to
 * protect against this thread dying.
 *
 * @param tx
 * The transaction that has an unused empty block of undo
 *
 * @param ub
 * The undo block to make free
 *
 * @param td
 * The thread data for the thread that owns the transaction.
 */
#ifdef NVM_EXT
static void nvm_drop_undo@(nvm_transaction ^tx, nvm_undo_blk ^ub,
        nvm_thread_data *td)
{
    /* Locate the transaction table in volatile memory */
    nvm_app_data *ad = nvm_get_app_data();
    nvm_trans_table_data *ttd = ad->regions[tx=>desc]->trans_table;

    /* Lock the undo free list so we can add a block to it */
    nvms_lock_mutex(ttd->undo_mutex, 1);

    /* Decrement the count of undo blocks. If the application dies,
     * recovery will recalculate the undo block count. */
    tx=>cur_undo_blocks--;

    /* Unlink the block from the transaction before adding it to the freelist.
     * This must commit first since recovery uses the linked list of undo blocks
     * on the transaction, but rebuilds the freelist based on count of undo
     * records in undo blocks. A block with a zero count will not be lost,
     * but a transaction pointing to a block on the freelist will not
     * recover properly. */
    tx=>undo ~= ub=>link;
    nvm_persist();

    /* Link the block to the free list. */
    ub=>link ~= ttd->free_undo;
    nvm_persist();
    ttd->free_undo = ub;
    td->persist_undo += 2;

    /* In case someone is waiting for a free block, post the condition. This
     * will do many unnecessary posts, but is guaranteed to work. */
    nvms_cond_post(ttd->undo_cond, 0);

    /* Release the mutex. */
    nvms_unlock_mutex(ttd->undo_mutex);
}
#else
static void nvm_drop_undo(nvm_transaction *tx, nvm_undo_blk *ub,
        nvm_thread_data *td)
{
    /* Locate the transaction table in volatile memory */
    nvm_app_data *ad = nvm_get_app_data();
    nvm_trans_table_data *ttd = ad->regions[tx->desc]->trans_table;

    /* Lock the undo free list so we can add a block to it */
    nvms_lock_mutex(ttd->undo_mutex, 1);

    /* Decrement the count of undo blocks. If the application dies,
     * recovery will recalculate the undo block count. */
    tx->cur_undo_blocks--;

    /* Unlink the block from the transaction before adding it to the freelist.
     * This must commit first since recovery uses the linked list of undo blocks
     * on the transaction, but rebuilds the freelist based on count of undo
     * records in undo blocks. A block with a zero count will not be lost,
     * but a transaction pointing to a block on the freelist will not
     * recover properly. */
    nvm_undo_blk_set(&tx->undo, nvm_undo_blk_get(&ub->link));
    nvm_persist1(&tx->undo);

    /* Link the block to the free list. */
    nvm_undo_blk_set(&ub->link, ttd->free_undo);
    nvm_persist1(&ub->link);
    ttd->free_undo = ub;
    td->persist_undo += 2;

    /* In case someone is waiting for a free block, post the condition. This
     * will do many unnecessary posts, but is guaranteed to work. */
    nvms_cond_post(ttd->undo_cond, 0);

    /* Release the mutex. */
    nvms_unlock_mutex(ttd->undo_mutex);
}
#endif //NVM_EXT

/**
 * This adds an operation to the current undo block. The operation
 * data must already be stored in the undo block data area.
 * @param op An operation to add
 */
#ifdef NVM_EXT
void nvm_add_oper@(nvm_transaction ^tx, nvm_opcode op, size_t bytes)
{
    /* get the undo block */
    nvm_undo_blk ^ub = tx=>undo;

    /* Add the opcode to the undo block and ensure it and the data are
     * persistently stored in NVM */
    nvm_operation nop;
    nop.opcode = op;
    nop.size = (uint16_t)bytes;
    nvm_operation ^opaddr = %((nvm_operation^)(ub=>data))[ub=>count];
    ^opaddr ~= nop;
    nvm_persist();

    /* Atomically and persistently commit the undo operation to exist by
     * incrementing the count of undo operations in this undo block. */
    ub=>count~++;
    ub=>hi_count ~= ub=>count; // debugging data
    nvm_persist();


    /* Adjust transient data used to optimize undo allocation. This does not
     * need to be flushed since it is reconstructed by reattach. */
    tx=>undo_ops++; // one more operation in the transaction
    bytes = (bytes + sizeof(uint64_t) - 1) & ~(sizeof(uint64_t) - 1);
    tx=>undo_bytes -= bytes;
    tx=>undo_data -= bytes;

    /* reduce undo_bytes by 8 if the next opcode allocation will be in a new
     * uint64_t. */
    if ((ub=>count % (sizeof(uint64_t) / sizeof(nvm_operation))) == 0)
        tx=>undo_bytes -= sizeof(uint64_t);
}
#else
void nvm_add_oper(nvm_transaction *tx, nvm_opcode op, size_t bytes)
{
    /* get the undo block */
    nvm_undo_blk *ub = nvm_undo_blk_get(&tx->undo);

    /* Add the opcode to the undo block and ensure it and the data are
     * persistently stored in NVM */
    nvm_operation nop;
    nop.opcode = op;
    nop.size = (uint16_t)bytes;
    nvm_operation *opaddr = &((nvm_operation*)(ub->data))[ub->count];
    *opaddr = nop;
    nvm_flush1(opaddr);
    nvm_persist();

    /* Atomically and persistently commit the undo operation to exist by
     * incrementing the count of undo operations in this undo block. */
    ub->count++;
    ub->hi_count = ub->count; // debugging data
    nvm_persist1(&ub->count);


    /* Adjust transient data used to optimize undo allocation. This does not
     * need to be flushed since it is reconstructed by reattach. */
    tx->undo_ops++; // one more operation in the transaction
    bytes = (bytes + sizeof(uint64_t) - 1) & ~(sizeof(uint64_t) - 1);
    tx->undo_bytes -= bytes;
    tx->undo_data -= bytes;

    /* reduce undo_bytes by 8 if the next opcode allocation will be in a new
     * uint64_t. */
    if ((ub->count % (sizeof(uint64_t) / sizeof(nvm_operation))) == 0)
        tx->undo_bytes -= sizeof(uint64_t);
}
#endif //NVM_EXT
/**
 * Count the number of undo operations in a transaction to correct undo_ops
 * when its value is lost. Note that this is a transient fields that does
 * not need flushing. It is transient because it cannot be atomically
 * updated with the count fields in undo blocks.
 */
#ifdef NVM_EXT
void nvm_count_undo(nvm_transaction ^tx)
{
    tx=>undo_ops = 0;
    tx=>cur_undo_blocks = 0;
    nvm_undo_blk ^ub = tx=>undo;
    while (ub)
    {
        tx=>undo_ops += ub=>count;
        tx=>cur_undo_blocks++;
        ub = ub=>link;
    }
}
#else
void nvm_count_undo(nvm_transaction *tx)
{
    tx->undo_ops = 0;
    tx->cur_undo_blocks = 0;
    nvm_undo_blk *ub = nvm_undo_blk_get(&tx->undo);
    while (ub)
    {
        tx->undo_ops += ub->count;
        tx->cur_undo_blocks++;
        ub = nvm_undo_blk_get(&ub->link);
    }
}
#endif //NVM_EXT

/**
 * This prepares for either applying or generating undo in the current
 * undo block, if any. The operation table is scanned to calculate
 * the values for undo_data and undo_bytes in the transaction. Note that
 * these are transient fields that do not need flushing.
 */
#ifdef NVM_EXT
void nvm_prep_undo(nvm_transaction ^tx)
{
    nvm_undo_blk ^ub = tx=>undo;

    /* If there is no undo then make undo_bytes and undo_data zero */
    if (ub == 0)
    {
        tx=>undo_bytes = 0;
        tx=>undo_data = 0;
        return;
    }

    /* Find the pointer to the latest undo data stored in the block. The undo
     * operation data builds down from the end of data[]. Thus the
     * sum of the operation sizes must be subtracted from the address of the
     * end of the block to find the latest data. Note that we cannot just
     * maintain this pointer in NVM because it cannot be atomically updated
     * with the count. (Well we could pack them in a uint64_t, but ...) */
    tx=>undo_data = ub=>data + sizeof(ub=>data); // end of block

    /* calculate the number of bytes of data still available for storing new
     * undo. We start with all of data[] available and decrement for undo
     * operations found.
     */
    tx=>undo_bytes = sizeof(ub=>data);

    /* Scan through the Operation objects subtracting their sizes to get
     * the pointer to the latest operation data. */
    nvm_operation ^opr = (nvm_operation^)ub=>data;
    int i;
    for (i = 0; i < ub=>count; i++, opr++)
    {
        /* reduce undo_bytes by 8 more if this opcode is in a new uint64_t. */
        if ((i % (sizeof(uint64_t) / sizeof(nvm_operation))) == 0)
            tx=>undo_bytes -= sizeof(uint64_t);

        /* round up the size from the Operation because the data for an
         * operation is always uint64_t aligned. */
        uint32_t bytes = (opr=>size + sizeof(uint64_t) - 1) &
                ~(sizeof(uint64_t) - 1);
        tx=> undo_data -= bytes; // decrement to next operation
        tx=>undo_bytes -= bytes; // bytes consumed for undo data
    }

    /* reduce undo_bytes by 8 more if the next opcode allocation will be
     * in a new uint64_t. */
    if ((ub=>count % (sizeof(uint64_t) / sizeof(nvm_operation))) == 0)
        tx=>undo_bytes -= sizeof(uint64_t);
}
#else
void nvm_prep_undo(nvm_transaction *tx)
{
    nvm_undo_blk *ub = nvm_undo_blk_get(&tx->undo);

    /* If there is no undo then make undo_bytes and undo_data zero */
    if (ub == NULL)
    {
        tx->undo_bytes = 0;
        tx->undo_data = 0;
        return;
    }

    /* make sure we are actually looking at an undo block */
    nvm_verify(ub, shapeof(nvm_undo_blk));

    /* Find the pointer to the latest undo data stored in the block. The undo
     * operation data builds down from the end of data[]. Thus the
     * sum of the operation sizes must be subtracted from the address of the
     * end of the block to find the latest data. Note that we cannot just
     * maintain this pointer in NVM because it cannot be atomically updated
     * with the count. (Well we could pack them in a uint64_t, but ...) */
    tx->undo_data = ub->data + sizeof(ub->data); // end of block

    /* calculate the number of bytes of data still available for storing new
     * undo. We start with all of data[] available and decrement for undo
     * operations found.
     */
    tx->undo_bytes = sizeof(ub->data);

    /* Scan through the Operation objects subtracting their sizes to get
     * the pointer to the latest operation data. */
    nvm_operation *opr = (nvm_operation*)ub->data;
    int i;
    for (i = 0; i < ub->count; i++, opr++)
    {
        /* reduce undo_bytes by 8 more if this opcode is in a new uint64_t. */
        if ((i % (sizeof(uint64_t) / sizeof(nvm_operation))) == 0)
            tx->undo_bytes -= sizeof(uint64_t);

        /* round up the size from the Operation because the data for an
         * operation is always uint64_t aligned. */
        uint32_t bytes = (opr->size + sizeof(uint64_t) - 1) &
                ~(sizeof(uint64_t) - 1);
        tx-> undo_data -= bytes; // decrement to next operation
        tx->undo_bytes -= bytes; // bytes consumed for undo data
    }

    /* reduce undo_bytes by 8 more if the next opcode allocation will be
     * in a new uint64_t. */
    if ((ub->count % (sizeof(uint64_t) / sizeof(nvm_operation))) == 0)
        tx->undo_bytes -= sizeof(uint64_t);
}
#endif //NVM_EXT
/**
 * Return the state of the current transaction. If in a nested transaction this
 * is the state of the nested transaction. The caller is presumed to have
 * verified the transaction USID is valid.
 *
 * @param tx Pointer to the current base transaction
 *
 * @return State of current transaction.
 */
#ifdef NVM_EXT
static nvm_trans_state nvm_get_state(nvm_transaction ^tx)
{
    /* If this is a base transaction the state is in the nvm_transaction.
     * Otherwise it is in the description of the most recent nested
     * transaction. */
    nvm_nested ^nt = (nvm_nested ^)tx=>nstd_last;
    if (nt == 0)
        return(nvm_trans_state)tx=>state;
    else
        return(nvm_trans_state)nt=>state;
}
#else
static nvm_trans_state nvm_get_state(nvm_transaction *tx)
{
    /* If this is a base transaction the state is in the nvm_transaction.
     * Otherwise it is in the description of the most recent nested
     * transaction. */
    nvm_nested *nt = nvm_nested_get(&tx->nstd_last);
    if (nt == 0)
        return(nvm_trans_state)tx->state;
    else
        return(nvm_trans_state)nt->state;
}
#endif //NVM_EXT

/**
 * This saves the current contents of a contiguous piece of NVM in one or
 * more undo records of the current transaction. If the transaction aborts
 * or rolls back to a previously created savepoint, then the contents will
 * be restored to the values saved here. Rollback may also be the result of
 * process death. Recovery by another process will apply the undo.
 *
 * Usually undo is automatically created via a transactional store to an
 * NVM address. The preprocessor will insert calls to nvm_undo before
 * storing in NVM. An application might need an explicit call for creating
 * undo before passing an NVM address to a legacy function that will store
 * results in NVM. Explicitly generating undo for an entire struct before
 * multiple non-transactional stores into it, may be more efficient than
 * multiple transactional stores.
 *
 * It is possible that there are not enough undo blocks available to hold
 * the new undo. In that case the thread will sleep until more undo space
 * is available.
 *
 * There is a region specific limit on the total amount of undo that a
 * single base transaction and all its nested transactions can consume.
 * This is necessary to prevent a runaway loop from consuming all available
 * undo locking up the system. If this limit is exceeded then an assert is
 * fired.
 *
 * @param[in] data
 * This is the address in NVM where the data to preserve begins
 *
 * @param[in] bytes
 * This is the amount of data to save as undo.
 */
#ifdef NVM_EXT
    void nvm_undo@(
        void ^data,
        size_t size
        )
{
    /* Get the current transaction and verify its type and state. */
    nvm_thread_data *td = nvm_get_thread_data();
    nvm_transaction ^tx = td->transaction;
    if (tx == 0)
        nvms_assert_fail("Attempt to generate undo without a transaction");
    if (nvm_get_state(tx) != nvm_active_state)
        nvms_assert_fail("Generating undo in transaction that is not active ");

    /* Verify the undo is for data in the region */
    if (!nvm_region_rng(td->region, data, size))
        nvms_assert_fail("Undo data not in region");

    /* loop storing into one or more undo blocks until all restore
     * data is saved. */
    uint8_t ^pt = (uint8_t^)data; // address of next byte to save
    size_t cnt = size; // bytes remaining to be saved
    while (cnt > 0)
    {
        /* If we cannot even get the smallest struct in this block, allocate
         * a new undo block. */
        if (tx=>undo_bytes < sizeof(nvm_restore) + 8)
            nvm_add_undo(tx, td);

        /* decide how much we can store in the current block. */
        nvm_restore ^r;
        size_t bytes = sizeof(^r) + cnt; // max total data size
        if (tx=>undo_bytes < bytes)
            bytes = tx=>undo_bytes; // will need another block after this

        /* calculate a pointer to our operation data. It must be 8
         * byte aligned */
        r = (nvm_restore^)(tx=>undo_data - ((bytes + 7) & ~7));

        /* Build the operation data and ensure it is persistent */
        r=>addr ~= pt;
        nvm_copy(r=>data, pt, bytes - sizeof(^r));

        /* add the undo operation */
        nvm_add_oper(tx, nvm_op_restore, bytes);
        td->persist_undo += 2;

        /* less data to save now so adjust loop control vairables */
        pt += bytes - sizeof(^r);
        cnt -= bytes - sizeof(^r);
    }
}
#else
void nvm_undo(
        const void *data,
        size_t size
        )
{
    /* Get the current transaction and verify its type and state. */
    nvm_thread_data *td = nvm_get_thread_data();
    nvm_transaction *tx = td->transaction;
    if (tx == NULL)
        nvms_assert_fail("Attempt to generate undo without a transaction");
    nvm_verify(tx, shapeof(nvm_transaction));
    if (nvm_get_state(tx) != nvm_active_state)
        nvms_assert_fail("Generating undo in transaction that is not active ");

    /* Verify the undo is for data in the region */
    if (!nvm_region_rng(td->region, data, size))
        nvms_assert_fail("Undo data not in region");

    /* loop storing into one or more undo blocks until all restore
     * data is saved. */
    uint8_t *pt = (uint8_t*)data; // address of next byte to save
    size_t cnt = size; // bytes remaining to be saved
    while (cnt > 0)
    {
        /* If we cannot even get the smallest struct in this block, allocate
         * a new undo block. If some of this can be saved then verify the
         * current undo block is valid. */
        if (tx->undo_bytes < sizeof(nvm_restore) + 8)
            nvm_add_undo(tx, td);
        else
            nvm_verify(nvm_undo_blk_get(&tx->undo), shapeof(nvm_undo_blk));

        /* decide how much we can store in the current block. */
        nvm_restore *r;
        size_t bytes = sizeof(*r) + cnt; // max total data size
        if (tx->undo_bytes < bytes)
            bytes = tx->undo_bytes; // will need another block after this

        /* calculate a pointer to our operation data. It must be 8
         * byte aligned */
        r = (nvm_restore*)(tx->undo_data - ((bytes + 7) & ~7));

        /* Build the operation data and ensure it is persistent */
        void_set(&r->addr, pt);
        nvm_copy(r->data, pt, bytes - sizeof(*r));
        nvm_flush(r, bytes);

        /* add the undo operation */
        nvm_add_oper(tx, nvm_op_restore, bytes);
        td->persist_undo += 2;

        /* less data to save now so adjust loop control vairables */
        pt += bytes - sizeof(*r);
        cnt -= bytes - sizeof(*r);
    }
}
#endif //NVM_EXT

/**
 * This adds a new NVM lock undo record to the current transaction and
 * returns a pointer to it. It also returns the slot number of the
 * current transaction to store in the owners field if acquired X. The
 * lock state will be set to free before the record is added to the
 * transaction.
 *
 * If the current max lock level will be set in the nvm_lkrec so that the
 * caller can decide if there is a deadlock problem.
 *
 * @param[in] tx
 * The transaction that is acquiring a lock
 *
 * @param[in] td
 * The thread data for the thread that owns the transaction.
 *
 * @param[in] mx
 * Address of the nvm_amutex that will be locked.
 *
 * @return
 * Pointer to the new nvm_lkrec struct
 */
#ifdef NVM_EXT
nvm_lkrec ^nvm_add_lock_op@(
        nvm_transaction ^tx,
        nvm_thread_data *td,
        nvm_amutex ^mx,
        nvm_lock_state st)
{
    /* Verify transaction is in the correct state. */
    if (nvm_get_state(tx) != nvm_active_state)
        nvms_assert_fail("Acquiring a lock for non-active transaction");

    /* Verify the mutex being locked is in the region */
    if (!nvm_region_rng(td->region, mx, sizeof(mx)))
        nvms_assert_fail("Mutex being locked not in region");

    /* If we cannot get the struct in this block, allocate a new undo block. */
    if (tx=>undo_bytes < sizeof(nvm_lkrec))
        nvm_add_undo(tx, td); // add a new empty undo block

    /* Get the address to store the data for the operation. */
    nvm_lkrec ^lk = (nvm_lkrec ^)
            (tx=>undo_data - sizeof(nvm_lkrec));

    /* Initialize the contents of the lock operation. We link to the previous
     * lock in this region's transaction since we cannot point to another
     * region even if there is a parent transaction there. */
    nvm_lkrec ^plk = tx=>held_locks; // previous lock
    lk=>prev ~= plk;
    lk=>undo_ops ~= tx=>undo_ops;

    /* The old lock level is the new level from the most recent lock
     * acquisition, if any. Note that the most recent lock could be in a
     * parent transaction in another region.  */
    nvm_transaction ^atx = tx=>parent; // ancestor transaction
    while (!plk && atx)
    {
        plk = atx=>held_locks; // ancestor lock
        atx = atx=>parent; // ancestor transaction
    }
    lk=>old_level ~= plk ? plk=>new_level : 0;

    /* Since the new lock has not yet been acquired, for now the new level
     * is the same as the old. */
    lk=>new_level ~= lk=>old_level;

    lk=>state ~= st;
    lk=>mutex ~= mx;

    /* Add to the transaction. This makes it persistently part of the
     * transaction. */
    nvm_add_oper(tx, nvm_op_lock, sizeof(nvm_lkrec));

    /* The operation is persistently recorded as a lock operation.
     * However it is not on the list of held locks yet. */
    tx=>held_locks ~= lk;
    nvm_persist();

    /* Update persists for locking. */
    td->persist_lock += 3;

    /* return the new lock operation */
    return lk;
}
#else
nvm_lkrec *nvm_add_lock_op(
        nvm_transaction *tx,
        nvm_thread_data *td,
        nvm_amutex *mx,
        nvm_lock_state st)
{
    /* Verify transaction is in the correct state. */
    if (nvm_get_state(tx) != nvm_active_state)
        nvms_assert_fail("Acquiring a lock for non-active transaction");

    /* Verify the mutex being locked is in the region */
    if (!nvm_region_rng(td->region, mx, sizeof(mx)))
        nvms_assert_fail("Mutex being locked not in region");

    /* If we cannot get the struct in this block, allocate a new undo block.
     * Otherwise verify current undo block. */
    if (tx->undo_bytes < sizeof(nvm_lkrec))
        nvm_add_undo(tx, td); // add a new empty undo block
    else
        nvm_verify(nvm_undo_blk_get(&tx->undo), shapeof(nvm_undo_blk));

    /* Get the address to store the data for the operation. */
    nvm_lkrec *lk = (nvm_lkrec *)
            (tx->undo_data - sizeof(nvm_lkrec));

    /* Initialize the contents of the lock operation. We link to the previous
     * lock in this region's transaction since we cannot point to another
     * region even if there is a parent transaction there. */
    nvm_lkrec *plk = nvm_lkrec_get(&tx->held_locks); // previous lock
    nvm_lkrec_set(&lk->prev, plk);
    lk->undo_ops = tx->undo_ops;

    /* The old lock level is the new level from the most recent lock
     * acquisition, if any. Note that the most recent lock could be in a
     * parent transaction in another region.  */
    nvm_transaction *atx = tx->parent; // ancestor transaction
    while (!plk && atx)
    {
        plk = nvm_lkrec_get(&atx->held_locks); // ancestor lock
        atx = atx->parent; // ancestor transaction
    }
    lk->old_level = plk ? plk->new_level : 0;

    /* Since the new lock has not yet been acquired, for now the new level
     * is the same as the old. */
    lk->new_level = lk->old_level;

    lk->state = st;
    nvm_amutex_set(&lk->mutex, mx);

    /* flush the lock data*/
    nvm_flush(lk, sizeof(*lk));

    /* Add to the transaction. This makes it persistently part of the
     * transaction. */
    nvm_add_oper(tx, nvm_op_lock, sizeof(nvm_lkrec));

    /* The operation is persistently recorded as a lock operation.
     * However it is not on the list of held locks yet. */
    nvm_lkrec_set(&tx->held_locks, lk);
    nvm_persist1(&tx->held_locks);

    /* Update persists for locking. */
    td->persist_lock += 3;

    /* return the new lock operation */
    return lk;
}
#endif //NVM_EXT


/**
 * This creates an undo record in the current transaction. The function
 * must be a persistent callback, meaning it has a pointer to a persistent
 * struct as its only argument. The record contains an instance of the
 * persistent struct. A pointer to the argument struct is returned so that
 * the caller can configure the arguments. The struct will be initialized
 * as if was allocated from an NVM heap.
 *
 * Note that the thread could die immediately after the undo record is
 * added to the transaction and before configuring the argument struct
 * is complete. Thus the callback must tolerate a partially constructed
 * argument. Unless there is a persist barrier, the stores into the
 * argument may complete in any order.
 *
 * If the undo record is applied, the indicated function is called with a
 * pointer to the persistent struct allocated in the undo record. The
 * function is called inside its own nested transaction. The nested
 * transaction is committed when the function returns. The nested
 * transaction must not be aborted because this would make the NVM region
 * unrecoverable. Note that during recovery the function may be called in
 * another process.
 *
 * This allows arbitrary application code to be called during rollback or
 * transaction abort. The onabort operation can implement logical undo
 * which acquires its own locks rather than holding locks until the parent
 * transaction commits. A nested transaction can be started once the undo
 * is created. The nested transaction can acquire locks update NVM and
 * transactionally store in the undo record the information needed to undo
 * the update. The nested transaction commits releasing any locks it holds.
 * This reduces lock contention, and can be used to avoid deadlocks.
 *
 * If a new software release changes the callback function to take an
 * argument with a new field, then the old function should be kept with
 * the same USID and a new USID defined for the new version of the function.
 * The old version of the function may be registered under a new function
 * name as long as it has the same USID.  The old callback is only needed
 * if attaching to a region that was last in use by the previous software
 * version. However there could potentially be region files that have not
 * been attached, and thus recovered, in years. Thus the old function needs
 * to be kept for multiple software releases.
 *
 * If there are any errors then errno is set and the return value is zero.
 *
 * @param[in] func
 * This is a USID that nvm_usid_volatile can map to the address of the
 * undo function.
 *
 * @return
 * If the undo is successfully constructed, then the address of the undo
 * argument struct is returned so that the caller can initialize it. If
 * there is an error then zero is returned and errno is set.
 *
 * @par Errors:\n
 */
#ifdef NVM_EXT
void ^nvm_onabort@(
        void (|func@)()
        )
{
    /* lookup the nvm_extern for the function to get its arg type. */
    const nvm_extern *nx = nvm_callback_find(func);
    if (nx == NULL)
        nvms_assert_fail("Unregistered callback USID passed to nvm_onabort");
    const nvm_type *arg = nx->arg;
    if (arg == NULL)
        nvms_assert_fail("Function passed to nvm_onabort is not a callback");

    /* Get the current transaction and verify its type and state. */
    nvm_thread_data *td = nvm_get_thread_data();
    nvm_transaction ^tx = td->transaction;
    if (tx == 0)
        nvms_assert_fail("Generating on abort without a transaction");
    if (nvm_get_state(tx) != nvm_active_state)
        nvms_assert_fail("Generating on abort in non-active transaction");

    /* Determine size of this operation and verify it fits in an undo block */
    if (arg->xsize)
        nvms_assert_fail("Extendable struct used in on abort operation");
    size_t size = sizeof(nvm_on_abort) +arg->size;
    if (size > sizeof(tx=>undo=>data) - sizeof(uint64_t))
        nvms_assert_fail("Arg for on abort operation too large");

    /* If we cannot get the struct in this block, allocate a new undo block. */
    if (tx=>undo_bytes < size)
        nvm_add_undo(tx, td); // add a new empty undo block

    /* Get the address to store the data for the operation. */
    nvm_on_abort ^oa = (nvm_on_abort ^)(tx=>undo_data - ((size + 7) & ~7));

    /* Set the USID of the function to call if applied */
    oa=>func ~= func;

    /* Do the default initialization for the argument. This flushes oa=>data. */
    nvm_alloc_init(oa=>data, arg, arg->size);

    /* Add to the transaction */
    nvm_add_oper(tx, nvm_op_on_abort, size);

    /* Return the address of the argument data. */
    return oa=>data;
}
#else
void *nvm_onabort(
        nvm_usid func
        )
{
    /* lookup the nvm_extern for the function to get its arg type. */
    const nvm_extern *nx = nvm_usid_find(func);
    if (nx == NULL)
        nvms_assert_fail("Unregistered callback USID passed to nvm_onabort");
    const nvm_type *arg = nx->arg;
    if (arg == NULL)
        nvms_assert_fail("Function passed to nvm_onabort is not a callback");

    /* Get the current transaction and verify its type and state. */
    nvm_thread_data *td = nvm_get_thread_data();
    nvm_transaction *tx = td->transaction;
    if (tx == NULL)
        nvms_assert_fail("Generating on abort without a transaction");
    nvm_verify(tx, shapeof(nvm_transaction));
    if (nvm_get_state(tx) != nvm_active_state)
        nvms_assert_fail("Generating on abort in non-active transaction");

    /* Determine size of this operation and verify it fits in an undo block */
    if (arg->xsize)
        nvms_assert_fail("Extendable struct used in on abort operation");
    size_t size = sizeof(nvm_on_abort) +arg->size;
    if (size > sizeof(((nvm_undo_blk*)0)->data) - sizeof(uint64_t))
        nvms_assert_fail("Arg for on abort operation too large");

    /* If we cannot get the struct in this block, allocate a new undo block.
     * Otherwise verify current undo block */
    if (tx->undo_bytes < size)
        nvm_add_undo(tx, td); // add a new empty undo block
    else
        nvm_verify(nvm_undo_blk_get(&tx->undo), shapeof(nvm_undo_blk));

    /* Get the address to store the data for the operation. */
    nvm_on_abort *oa = (nvm_on_abort *)
                                    (tx->undo_data - ((size + 7) & ~7));

    /* Set the USID of the function to call if applied */
    oa->func = func;
    nvm_flush1(&oa->func);

    /* Do the default initialization for the argument. This flushes oa->data. */
    nvm_alloc_init(oa->data, arg, arg->size);

    /* Add to the transaction */
    nvm_add_oper(tx, nvm_op_on_abort, size);

    /* Return the address of the argument data. */
    return oa->data;
}
#endif //NVM_EXT

/**
 * This creates an undo record in the current transaction. The function
 * must be a persistent callback, meaning it has a pointer to a persistent
 * struct as its only argument. The record contains an instance of the
 * persistent struct. A pointer to the argument struct is returned so that
 * the caller can configure the arguments. The struct will be initialized
 * as if was allocated from an NVM heap.
 *
 * If the current transaction commits, the indicated function is called
 * with a pointer to the persistent struct allocated in the undo record.
 * The function is called after all locks have been released by the
 * containing transaction. It is called inside its own nested transaction.
 * The nested transaction is committed when the function returns. The
 * nested transaction must not be aborted because this would make the NVM
 * region unrecoverable. Note that during recovery the function may be
 * called in another process.
 *
 * This allows arbitrary application code to be called as part of
 * transaction commit. Once the transaction is put in state committing and
 * all locks are released, then all the oncommit operations are executed.
 * Thus the functions are only called if the transaction is guaranteed not
 * to abort or do any rollback. This is useful for avoiding deadlocks and
 * reducing lock contention. If the process executing the function dies,
 * then recovery will rollback the nested transaction and re-execute the
 * oncommit function.
 *
 * If a new software release changes the callback function to take an
 * argument with a new field, then the old function should be kept with
 * the same USID and a new USID defined for the new version of the function.
 * The old version of the function may be registered under a new function
 * name as long as it has the same USID.  The old callback is only needed
 * if attaching to a region that was last in use by the previous software
 * version. However there could potentially be region files that have not
 * been attached, and thus recovered, in years. Thus the old function needs
 * to be kept for multiple software releases.
 *
 * If there are any errors then errno is set and the return value is zero.
 *
 * @param[in] func
 * This is a USID that nvm_usid_volatile can map to the address of the
 * commit function.
 *
 * @return
 * If the undo is successfully constructed, then the address of the commit
 * argument struct is returned so that the caller can initialize it. If
 * there is an error then zero is returned and errno is set.
 *
 * @par Errors:\n
 */
#ifdef NVM_EXT
void ^nvm_oncommit@(
        void (|func@)()
        )
{
    /* lookup the nvm_extern for the function to get its arg type. */
    const nvm_extern *nx = nvm_callback_find(func);
    if (nx == NULL)
        nvms_assert_fail("Unregistered callback USID passed to nvm_oncommit");
    const nvm_type *arg = nx->arg;
    if (arg == NULL)
        nvms_assert_fail("Function passed to nvm_oncommit is not a callback");

    /* Get the current transaction and verify its type and state. */
    nvm_thread_data *td = nvm_get_thread_data();
    nvm_transaction ^tx = td->transaction;
    if (tx == 0)
        nvms_assert_fail("Generating on commit without a transaction");
    if (nvm_get_state(tx) != nvm_active_state)
        nvms_assert_fail("Generating on commit in non-active transaction");

    /* Determine size of this operation and verify it fits in an undo block */
    if (arg->xsize)
        nvms_assert_fail("Extendable struct used in on commit operation");
    size_t size = sizeof(nvm_on_commit) +arg->size;
    if (size > sizeof(tx=>undo=>data) - sizeof(uint64_t))
        nvms_assert_fail("Arg for on commit operation too large");

    /* If we cannot get the struct in this block, allocate a new undo block. */
    if (tx=>undo_bytes < size)
        nvm_add_undo(tx, td); // add a new empty undo block

    /* Get the address to store the data for the operation. */
    nvm_on_commit ^oc = (nvm_on_commit ^)(tx=>undo_data - ((size + 7) & ~7));

    /* Set the links for the other on commit operations in this transaction */
    oc=>undo_ops ~= tx=>undo_ops;
    oc=>prev ~= tx=>commit_ops;

    /* Set the USID of the function to call if applied */
    oc=>func ~= func;

    /* Do the default initialization for the argument. This flushes oc=>data. */
    nvm_alloc_init(oc=>data, arg, arg->size);

    /* Add to the transaction. This makes it persistently part of the
     * transaction. */
    nvm_add_oper(tx, nvm_op_on_commit, size);

    /* The operation is persistently recorded as an on commit operation.
     * However it is not on the list of on commit operations so a commit now
     * would not call the operation function. Linking it to the list makes
     * it functional. */
    tx=>commit_ops ~= oc;

    td->persist_tx += 3;

    /* Return the address of the argument data. */
    return oc=>data;
}
#else
void *nvm_oncommit(
        nvm_usid func
        )
{
    /* lookup the nvm_extern for the function to get its arg type. */
    const nvm_extern *nx = nvm_usid_find(func);
    if (nx == NULL)
        nvms_assert_fail("Unregistered callback USID passed to nvm_oncommit");
    const nvm_type *arg = nx->arg;
    if (arg == NULL)
        nvms_assert_fail("Function passed to nvm_oncommit is not a callback");

    /* Get the current transaction and verify its type and state. */
    nvm_thread_data *td = nvm_get_thread_data();
    nvm_transaction *tx = td->transaction;
    if (tx == NULL)
        nvms_assert_fail("Generating on commit without a transaction");
    nvm_verify(tx, shapeof(nvm_transaction));
    if (nvm_get_state(tx) != nvm_active_state)
        nvms_assert_fail("Generating on commit in non-active transaction");

    /* Determine size of this operation and verify it fits in an undo block */
    if (arg->xsize)
        nvms_assert_fail("Extendable struct used in on commit operation");
    size_t size = sizeof(nvm_on_commit) +arg->size;
    if (size > sizeof(((nvm_undo_blk*)0)->data) - sizeof(uint64_t))
        nvms_assert_fail("Arg for on commit operation too large");

    /* If we cannot get the struct in this block, allocate a new undo block.
     * Otherwise verify current undo block. */
    if (tx->undo_bytes < size)
        nvm_add_undo(tx, td); // add a new empty undo block
    else
        nvm_verify(nvm_undo_blk_get(&tx->undo), shapeof(nvm_undo_blk));

    /* Get the address to store the data for the operation. */
    nvm_on_commit *oc = (nvm_on_commit *)
                                    (tx->undo_data - ((size + 7) & ~7));

    /* Set the links for the other on commit operations in this transaction */
    oc->undo_ops = tx->undo_ops;
    nvm_on_commit_set(&oc->prev, nvm_on_commit_get(&tx->commit_ops));

    /* Set the USID of the function to call if applied */
    oc->func = func;

    /* flush all but the argument data */
    nvm_flush(oc, sizeof(*oc));

    /* Do the default initialization for the argument. This flushes oc->data. */
    nvm_alloc_init(oc->data, arg, arg->size);

    /* Add to the transaction. This makes it persistently part of the
     * transaction. */
    nvm_add_oper(tx, nvm_op_on_commit, size);

    /* The operation is persistently recorded as an on commit operation.
     * However it is not on the list of on commit operations so a commit now
     * would not call the operation function. Linking it to the list makes
     * it functional. */
    nvm_on_commit_set(&tx->commit_ops, oc);
    nvm_persist1(&tx->commit_ops);

    td->persist_tx += 3;

    /* Return the address of the argument data. */
    return oc->data;
}
#endif //NVM_EXT

/**
 * This creates an undo record in the current transaction. The function
 * must be a persistent callback, meaning it has a pointer to a persistent
 * struct as its only argument. The record contains an instance of the
 * persistent struct. A pointer to the argument struct is returned so that
 * the caller can configure the arguments. The struct will be initialized
 * as if was allocated from an NVM heap.
 *
 * Note that the thread could die immediately after the undo record is
 * added to the transaction and before configuring the argument struct
 * is complete. Thus the callback must tolerate a partially constructed
 * argument. Unless there is a persist barrier, the stores into the
 * argument may complete in any order.
 *
 * When the current transaction either commits or aborts, all the locks
 * acquired by the transaction are released in the reverse order they were
 * acquired. Similarly, rollback to a savepoint releases locks acquired
 * since the savepoint was established. For abort or rollback, all undo
 * records are applied in reverse order so that the locks are released
 * after all undo generated while the lock is held is applied.
 *
 * The undo record created by this call is treated like a lock. The
 * indicated function is called with a pointer to the persistent struct
 * allocated in the undo record. The function is called at commit, abort,
 * or rollback that applies the record. When the callback is called, locks
 * that were held when nvm_onunlock() was called are still held. It is
 * called inside its own nested transaction. The callback function can
 * call nvm_txstatus(1) to determine if the parent transaction, containing
 * the unlock record, committed, aborted, or is being rolled back to a
 * savepoint.
 *
 * The nested transaction is committed when the function returns. The
 * callback must not call nvm_abort because this would make the NVM region
 * unrecoverable. Note that during recovery the function may be called in
 * another process. If the thread executing the function dies, then
 * recovery will rollback the nested transaction and re-execute the
 * onunlock function.
 *
 * This allows arbitrary application code to be called while locks
 * previously acquired are still held, even if the transaction is
 * committing. This is useful for executing code that only runs if the
 * transaction commits, and runs before relevant locks are released. This
 * mechanism can also be used to have application implemented locks that
 * are different than nvm_amutex locks.
 *
 * The callback function must not call longjmp to return to a setjmp that
 * was set before it was called. This would leave the transaction in a
 * partially committed state that is unlikely to be consistent. An assert
 * will fire if this is attempted.
 *
 * If a new software release changes the callback function to take an
 * argument with a new field, then the old function should be kept with
 * the same USID and a new USID defined for the new version of the function.
 * The old version of the function may be registered under a new function
 * name as long as it has the same USID.  The old callback is only needed
 * if attaching to a region that was last in use by the previous software
 * version. However there could potentially be region files that have not
 * been attached, and thus recovered, in years. Thus the old function needs
 * to be kept for multiple software releases.
 *
 * If there are any errors then errno is set and the return value is zero.
 *
 * @param[in] func
 * This is a USID that nvm_usid_volatile can map to the address of the
 * unlock function.
 *
 * @return
 * If the undo is successfully constructed, then the address of the unlock
 * argument struct is returned so that the caller can initialize it. If
 * there is an error then zero is returned and errno is set.
 *
 * @par Errors:\n
 */
#ifdef NVM_EXT
void ^nvm_onunlock@(
        void (|func@)()
        )
{
    /* lookup the nvm_extern for the function to get its arg type. */
    const nvm_extern *nx = nvm_callback_find(func);
    if (nx == NULL)
        nvms_assert_fail("Unregistered callback USID passed to nvm_onunlock");
    const nvm_type *arg = nx->arg;
    if (arg == NULL)
        nvms_assert_fail("Function passed to nvm_onunlock is not a callback");

    /* Get the current transaction and verify its type and state. */
    nvm_thread_data *td = nvm_get_thread_data();
    nvm_transaction ^tx = td->transaction;
    if (tx == 0)
        nvms_assert_fail("Generating on unlock without a transaction");
    if (nvm_get_state(tx) != nvm_active_state)
        nvms_assert_fail("Generating on unlock in non-active transaction");

    /* Determine size of this operation and verify it fits in an undo block */
    if (arg->xsize)
        nvms_assert_fail("Extendable struct used in on unlock operation");
    size_t size = sizeof(nvm_on_unlock) + arg->size;
    if (size > sizeof(tx=>undo=>data) - sizeof(uint64_t))
        nvms_assert_fail("Arg for on unlock operation too large");

    /* If we cannot get the struct in this block, allocate a new undo block. */
    if (tx=>undo_bytes < size)
        nvm_add_undo(tx, td); // add a new empty undo block

    /* Get the address to store the data for the operation. */
    nvm_on_unlock ^ou = (nvm_on_unlock ^)(tx=>undo_data - ((size + 7) & ~7));

    /* Initialize header in common with nvm_lkrec as an on unlock operation. */
    ou=>prev ~= tx=>held_locks;
    ou=>undo_ops ~= tx=>undo_ops;
    ou=>state ~= nvm_lock_callback; // indicates this is an on unlock

    /* Set the USID of the function to call at unlock */
    ou=>func ~= func;

    /* Do the default initialization for the argument. This flushes ou=>data. */
    nvm_alloc_init(ou=>data, arg, arg->size);

    /* Add to the transaction. This makes it persistently part of the
     * transaction. */
    nvm_add_oper(tx, nvm_op_on_unlock, size);

    /* The operation is persistently recorded as a on unlock operation.
     * However it is not on the list of held locks yet. */
    tx=>held_locks ~= (nvm_lkrec ^)ou;
    nvm_persist();

    /* Update persists for transaction. */
    td->persist_tx += 3;

    return ou=>data;
}
#else
void *nvm_onunlock(
        nvm_usid func
        )
{
    /* lookup the nvm_extern for the function to get its arg type. */
    const nvm_extern *nx = nvm_usid_find(func);
    if (nx == NULL)
        nvms_assert_fail("Unregistered callback USID passed to nvm_onunlock");
    const nvm_type *arg = nx->arg;
    if (arg == NULL)
        nvms_assert_fail("Function passed to nvm_onunlock is not a callback");

    /* Get the current transaction and verify its type and state. */
    nvm_thread_data *td = nvm_get_thread_data();
    nvm_transaction *tx = td->transaction;
    if (tx == NULL)
        nvms_assert_fail("Generating on unlock without a transaction");
    nvm_verify(tx, shapeof(nvm_transaction));
    if (nvm_get_state(tx) != nvm_active_state)
        nvms_assert_fail("Generating on unlock in non-active transaction");

    /* Determine size of this operation and verify it fits in an undo block */
    if (arg->xsize)
        nvms_assert_fail("Extendable struct used in on unlock operation");
    size_t size = sizeof(nvm_on_unlock) + arg->size;
    if (size > sizeof(((nvm_undo_blk*)0)->data) - sizeof(uint64_t))
        nvms_assert_fail("Arg for on unlock operation too large");

    /* If we cannot get the struct in this block, allocate a new undo block.
     * Otherwise verify current undo block. */
    if (tx->undo_bytes < size)
        nvm_add_undo(tx, td); // add a new empty undo block
    else
        nvm_verify(nvm_undo_blk_get(&tx->undo), shapeof(nvm_undo_blk));

    /* Get the address to store the data for the operation. */
    nvm_on_unlock *ou = (nvm_on_unlock *)(tx->undo_data - ((size + 7) & ~7));

    /* Initialize header in common with nvm_lkrec as an on unlock operation. */
    nvm_lkrec_set(&ou->prev, nvm_lkrec_get(&tx->held_locks));
    ou->undo_ops = tx->undo_ops;
    ou->state = nvm_lock_callback; // indicates this is an on unlock

    /* Set the USID of the function to call at unlock */
    ou->func = func;

    /* flush all but the argument data */
    nvm_flush(ou, sizeof(*ou));

    /* Do the default initialization for the argument. This flushes ou->data. */
    nvm_alloc_init(ou->data, arg, arg->size);

    /* Add to the transaction. This makes it persistently part of the
     * transaction. */
    nvm_add_oper(tx, nvm_op_on_unlock, size);

    /* The operation is persistently recorded as a on unlock operation.
     * However it is not on the list of held locks yet. */
    nvm_lkrec_set(&tx->held_locks, (nvm_lkrec *)ou);
    nvm_persist1(&tx->held_locks);

    /* Update persists for transaction. */
    td->persist_tx += 3;

    return ou->data;
}
#endif //NVM_EXT

/**
 * This executes an on unlock callback either at commit or rollback of a
 * transaction.
 */
#ifdef NVM_EXT
void nvm_unlock_callback@(nvm_on_unlock ^ou)
{
    /* If the callback previously committed do not call again. We clear the
     * second half of the usid within the transaction to indicate it
     * committed. */
    nvm_usid ^ui = (nvm_usid^)%ou=>func;
    if (ui=>d2 == 0)
        return;

    /* lookup the function address and assert if there is none */
    void (*f@)() = *ou=>func;
    if (f == 0)
        nvms_assert_fail("On unlock operation function missing");

    /* call the function in a nested transaction */
    @{ //nvm_txbegin(0);

        /* clear function usid in the transaction to prevent recall.
         * This is done before calling callback so a commit in the
         * callback commits this too. */
        ui=>d2 @= 0;

        /* Call the application function */
        (*f)(ou=>data);

    /* commit the nested transaction unless already committed */
    } // nvm_txend();
}
#else
void nvm_unlock_callback(nvm_on_unlock *ou)
{
    /* If the callback previously committed do not call again. We clear the
     * second half of the usid within the transaction to indicate it
     * committed. */
    if (ou->func.d2 == 0)
        return;

    /* lookup the function address and assert if there is none */
    void (*f)(void*) = (void(*)(void*))nvm_usid_volatile(ou->func);
    if (f == 0)
        nvms_assert_fail("On unlock operation function missing");

    /* call the function in a nested transaction */
    nvm_txbegin(0);

    /* clear function usid in the transaction to prevent recall.
     * This is done before calling callback so a commit in the
     * callback commits this too. */
    nvm_undo(&ou->func.d2, sizeof(ou->func.d2));
    ou->func.d2 = 0;
    nvm_flush1(&ou->func.d2);

    /* Call the application function */
    (*f)(ou->data);

    /* commit the nested transaction unless already committed */
    nvm_txend();
}
#endif //NVM_EXT

/**
 * This commits the current transaction. The current transaction is
 * committed releasing all its locks and executing any oncommit operations.
 * The transaction remains the current transaction until its code block is
 * exited. No more transactional operations are allowed in this transaction.
 *
 * It is an error if there is no current transaction or it is already
 * committed or aborted. If there are any errors then errno is set and the
 * return value is zero.
 *
 * @return
 * One if successful and zero if errno has an error
 *
 * @par Errors : \n
 */
#ifdef NVM_EXT
void nvm_commit@()
{
    /* Get the current transaction and verify its type and state. */
    nvm_thread_data *td = nvm_get_thread_data();
    nvm_transaction ^tx = td->transaction;
    if (tx == 0)
        nvms_assert_fail("Commit without a transaction");

    /* Make sure any updates done by the transaction are persistent before
     * we start making any state changes. */
    nvm_persist();

    /* Decide how much undo needs to be discarded, and set the transaction
     * to committing. This is done differently for a base transaction vs.
     * an nested transaction. Once the transaction state is committing
     * this transaction will commit even if this process dies immediately. */
    uint32_t stop;
    nvm_nested ^nt = tx=>nstd_last;
    if (nt == 0)
    {
        /* Verify state is valid */
        if (tx=>state != nvm_active_state && tx=>state != nvm_committing_state)
            nvms_assert_fail("Commit of non-active transaction");

        /* Committing a base transaction. Delete all undo and set base
         * transaction to committing */
        stop = 0;
        tx=>state ~= nvm_committing_state;
        nvm_persist();
    }
    else
    {
        /* Verify state is valid */
        if (nt=>state != nvm_active_state && nt=>state != nvm_committing_state)
            nvms_assert_fail("Commit of non-active transaction");

        /* Committing a nested transaction.
         * Set stop to the nested undo operation itself. Until nvm_txend is
         * called, we want the operation to be part of the transaction with
         * nstd_last pointing at it. Once everything except the nested
         * operation itself has been applied, the effect of the commit is
         * complete, */
        stop = nt=>undo_ops + 1;
        nt=>state ~= nvm_committing_state;
        nvm_persist();
    }
    td->persist_commit += 2;

    /* Release all the locks held by this transaction. Each unlock atomically
     * releases the lock and clears the lock state of the nvm_lkrec so that
     * unlock is idempotent. */
    nvm_lkrec ^lk = tx=>held_locks;
    while (lk != 0 && lk=>undo_ops >= stop)
    {
        /* If this is actually an on unlock operation then do the callback */
        if (lk=>state == nvm_lock_callback)
            nvm_unlock_callback((nvm_on_unlock^)lk);
        else
            nvm_unlock(tx, td, lk); // normal lock release
        lk = lk=>prev;
    }

    /* We unlocked all locks for the transaction. Set the new head of the
     * held locks list. Note that since we do not keep the list head
     * updated after every lock release, we could die in the middle of
     * releasing a lock and the lock will not be the head of the lock list.
     * Thus nvm_recover_lock needs to be called on all lock undo records, not
     * just the head of the list. */
    tx=>held_locks ~= lk;
    nvm_persist();
    td->persist_commit++;

    /* If there are any on commit operations, execute them in the order they
     * were created. This is the reverse of the order that they are linked in.
     * To execute in order created we use the transient next pointer in the
     * nvm_on_commit records to construct a list in execution order. Then we
     * use that list to execute in the correct order.
     *
     * You might think it would be better to link them in the right order
     * but correctly maintaining the list tail atomically during roll back
     * of an nested transaction is difficult. */
    nvm_on_commit ^hoc = tx=>commit_ops; //head on commit
    if (hoc != 0)
    {
        /* link the on commit records for this transaction in execution order.
         * Leaves pointer to the first record to execute in loc. Leaves the
         * record just before it in hoc. This is the new list head after
         * all operations have be executed. */
        nvm_on_commit ^loc = 0; // last on commit undo record
        while (hoc && hoc=>undo_ops >= stop)
        {
            hoc=>next = loc;
            loc = hoc;
            hoc = hoc=>prev;
        }

        /* Execute operations in creation order. Each operation is done in
         * its own nested transaction. The function USID is cleared when the
         * operation is committed so that it will not be called again. */
        while (loc)
        {
            /* execute this record if not previously executed */
            nvm_usid ^ui = (nvm_usid^)%loc=>func;
            if (ui=>d2)
            {
                /* lookup the func*tion address and assert if there is none */
                void (*f@)(void^) = *loc=>func;
                if (f == 0)
                    nvms_assert_fail("On commit operation function missing");

                /* call the function in a nested transaction */
                @{ //nvm_txbegin(0);

                    /* clear function usid in the transaction to prevent recall.
                     * This is done before calling callback so a commit in the
                     * callback commits this too. */
                    ui=>d2 @= 0;

                    /* Call the application function */
                    (*f)(loc=>data);

                    /* commit the nested transaction unless already committed */
                } //nvm_txend();
            }
            loc = loc=>next;
        }
    }

    /* All on commit operations executed. Remove the ones executed from
     * the transaction. This is makes commit_ops no longer point into the
     * undo that will be thrown away. This must be persisted before
     * releasing any undo. */
    tx=>commit_ops ~= hoc;
    nvm_persist();
    td->persist_commit++;

    /* Throw away all the undo back to stop. First throw away whole
     * blocks. */
    nvm_undo_blk ^ub = tx=>undo;
    while (ub && (tx=>undo_ops - ub=>count) >= stop)
    {
        /* reduce total undo_ops by the count in the block we are
         * discarding. Note that undo_ops is transient and does not
         * need to be flushed. Recovery will recalculate it. */
        tx=>undo_ops -= ub=>count;

        /* Mark block as empty. This means application death will result
         * in recovery removing it from the transaction. Thread death will
         * complete the commit throwing this away if necessary. */
        ub=>count ~= 0;
        nvm_persist();
        td->persist_commit++;

        /* Atomically move the block from the transaction to the
         * undo freelist. */
        nvm_drop_undo(tx, ub, td);

        /* get the next undo block to consider dropping */
        ub = tx=>undo;
}

    /* If this is a nested transaction then throw away the undo up to the
     * nvm_nested record itself. The record stays until nvm_txend is called.
     * We also set the state to committed so that no more undo can be
     * added to the transaction. */
    if (stop != 0)
    {
        /* nested transaction commit. Keep nvm_nested record. */
        ub=>count ~-= tx=>undo_ops - stop;
        nvm_persist();
        tx=>undo_ops = stop;

        /* Transaction is now committed */
        nt=>state ~= nvm_committed_state;
        nvm_persist();
        td->persist_commit += 2;
    }
    else
    {
        /* Base transaction committed and all undo released so change state. */
        tx=>undo_ops = 0;
        tx=>state ~= nvm_committed_state;
        nvm_persist();
        td->persist_commit++;
    }

    /* fix up undo_data and undo_bytes now that there is a different current
     * undo block, or none. */
    nvm_prep_undo(tx);
}
#else
void nvm_commit()
{
    /* Get the current transaction and verify its type and state. */
    nvm_thread_data *td = nvm_get_thread_data();
    nvm_transaction *tx = td->transaction;
    if (tx == NULL)
        nvms_assert_fail("Commit without a transaction");
    nvm_verify(tx, shapeof(nvm_transaction));

    /* Make sure any updates done by the transaction are persistent before
     * we start making any state changes. */
    nvm_persist();

    /* Decide how much undo needs to be discarded, and set the transaction
     * to committing. This is done differently for a base transaction vs.
     * an nested transaction. Once the transaction state is committing
     * this transaction will commit even if this process dies immediately. */
    uint32_t stop;
    nvm_nested *nt = nvm_nested_get(&tx->nstd_last);
    if (nt == 0)
    {
        /* Verify state is valid */
        if (tx->state != nvm_active_state && tx->state != nvm_committing_state)
            nvms_assert_fail("Commit of non-active transaction");

        /* Committing a base transaction. Delete all undo and set base
         * transaction to committing */
        stop = 0;
        tx->state = nvm_committing_state;
        nvm_persist1(&tx->state);
    }
    else
    {
        /* Verify state is valid */
        if (nt->state != nvm_active_state && nt->state != nvm_committing_state)
            nvms_assert_fail("Commit of non-active transaction");

        /* Committing a nested transaction.
         * Set stop to the nested undo operation itself. Until nvm_txend is
         * called, we want the operation to be part of the transaction with
         * nstd_last pointing at it. Once everything except the nested
         * operation itself has been applied, the effect of the commit is
         * complete, */
        stop = nt->undo_ops + 1;
        nt->state = nvm_committing_state;
        nvm_persist1(&nt->state);
    }
    td->persist_commit += 2;

    /* Release all the locks held by this transaction. Each unlock atomically
     * releases the lock and clears the lock state of the nvm_lkrec so that
     * unlock is idempotent. */
    nvm_lkrec *lk = nvm_lkrec_get(&tx->held_locks);
    while (lk != NULL && lk->undo_ops >= stop)
    {
        /* If this is actually an on unlock operation then do the callback */
        if (lk->state == nvm_lock_callback)
            nvm_unlock_callback((nvm_on_unlock*)lk);
        else
            nvm_unlock(tx, td, lk); // normal lock release
        lk = nvm_lkrec_get(&lk->prev);
    }

    /* We unlocked all locks for the transaction. Set the new head of the
     * held locks list. Note that since we do not keep the list head
     * updated after every lock release, we could die in the middle of
     * releasing a lock and the lock will not be the head of the lock list.
     * Thus nvm_recover_lock needs to be called on all lock undo records, not
     * just the head of the list. */
    nvm_lkrec_set(&tx->held_locks, lk);
    nvm_persist1(&tx->held_locks);
    td->persist_commit++;

    /* If there are any on commit operations, execute them in the order they
     * were created. This is the reverse of the order that they are linked in.
     * To execute in order created we use the transient next pointer in the
     * nvm_on_commit records to construct a list in execution order. Then we
     * use that list to execute in the correct order.
     *
     * You might think it would be better to link them in the right order
     * but correctly maintaining the list tail atomically during roll back
     * of an nested transaction is difficult. */
    nvm_on_commit *hoc = nvm_on_commit_get(&tx->commit_ops); //list head
    if (hoc != 0)
    {
        /* link the on commit records for this transaction in execution order.
         * Leaves pointer to the first record to execute in loc. Leaves the
         * record just before it in oc. This is the new list head after
         * all operations have be executed. */
        nvm_on_commit *loc = NULL; // last on commit undo record
        while (hoc && hoc->undo_ops >= stop)
        {
            hoc->next = loc;
            loc = hoc;
            hoc = nvm_on_commit_get(&hoc->prev);
        }

        /* Execute operations in creation order. Each operation is done in
         * its own nested transaction. The function USID is cleared when the
         * operation is committed so that it will not be called again. */
        while (loc)
        {
            /* execute  this record if not previously executed */
            if (loc->func.d2)
            {
                /* lookup the function address and assert if there is none */
                void (*f)(void*) = (void(*)(void*))nvm_usid_volatile(loc->func);
                if (f == 0)
                    nvms_assert_fail("On commit operation function missing");

                /* call the function in a nested transaction */
                nvm_txbegin(0);

                /* clear function usid in the transaction to prevent recall.
                 * This is done before calling callback so a commit in the
                 * callback commits this too. */
                nvm_undo(&loc->func.d2, sizeof(loc->func.d2));
                loc->func.d2 = 0;
                nvm_flush1(&loc->func.d2);

                /* Call the application function */
                (*f)(loc->data);

                /* commit the nested transaction unless already committed */
                nvm_txend();
            }
            loc = loc->next;
        }
    }

    /* All on commit operations executed. Remove the ones executed from
     * the transaction. This is makes commit_ops no longer point into the
     * undo that will be thrown away. This must be persisted before
     * releasing any undo. */
    nvm_on_commit_set(&tx->commit_ops, hoc);
    nvm_persist1(&tx->commit_ops);
    td->persist_commit++;

    /* Throw away all the undo back to stop. First throw away whole
     * blocks. */
    nvm_undo_blk *ub = nvm_undo_blk_get(&tx->undo);
    nvm_verify(ub, shapeof(nvm_undo_blk));
    while (ub && (tx->undo_ops - ub->count) >= stop)
    {
        /* reduce total undo_ops by the count in the block we are
         * discarding. Note that undo_ops is transient and does not
         * need to be flushed. Recovery will recalculate it. */
        tx->undo_ops -= ub->count;

        /* Mark block as empty. This means application death will result
         * in recovery removing it from the transaction. Thread death will
         * complete the commit throwing this away if necessary. */
        ub->count = 0;
        nvm_persist1(&ub->count);
        td->persist_commit++;

        /* Atomically move the block from the transaction to the
         * undo freelist. */
        nvm_drop_undo(tx, ub, td);

        /* get the next undo block to consider dropping */
        ub = nvm_undo_blk_get(&tx->undo);

        /* Verify this is a valid undo block */
        nvm_verify(ub, shapeof(nvm_undo_blk));
    }

    /* If this is a nested transaction then throw away the undo up to the
     * nvm_nested record itself. The record stays until nvm_txend is called.
     * We also set the state to committed so that no more undo can be
     * added to the transaction. */
    if (stop != 0)
    {
        /* nested transaction commit. Keep nvm_nested record. */
        ub->count -= tx->undo_ops - stop;
        nvm_persist1(&ub->count);
        tx->undo_ops = stop;

        /* Transaction is now committed */
        nt->state = nvm_committed_state;
        nvm_persist1(&nt->state);
        td->persist_commit += 2;
    }
    else
    {
        /* Base transaction committed and all undo released so change state. */
        tx->undo_ops = 0;
        tx->state = nvm_committed_state;
        nvm_persist1(&tx->state);
        td->persist_commit++;
    }

    /* fix up undo_data and undo_bytes now that there is a different current
     * undo block, or none. */
    nvm_prep_undo(tx);
}
#endif //NVM_EXT

/**
 * Apply undo from the transaction until the desired undo_ops is
 * reached. Undo blocks will be returned to the free list if they
 * become empty.
 *
 * Note that this relies on undo_data and undo_bytes being correct.
 */
#ifdef NVM_EXT
static void nvm_apply_undo@(nvm_transaction ^tx, uint32_t stop,
        nvm_thread_data *td)
{
    /* Apply until we get to stop or there is no undo at all */
    nvm_undo_blk ^ub = tx=>undo;
    while (ub != 0)
    {
        /* If this undo block is empty remove it from the transaction. */
        if (ub=>count == 0)
        {
            /* Atomically move the block from the transaction to the
             * undo freelist. */
            nvm_drop_undo(tx, ub, td);
            nvm_prep_undo(tx); // prepare for new undo block
            ub = tx=>undo;
            continue;
        }

        /* If we have applied all undo up to our stop record then exit. This
         * is done here rather than at the top of the loop so that all undo
         * blocks will be released if aborting a base transaction. */
        if (tx=>undo_ops <= stop)
            break;

        /* Process based on opcode */
        nvm_operation opr = ((nvm_operation^)(ub=>data))[ub=>count - 1];
        switch (opr.opcode)
        {
        default:
            nvms_assert_fail("Invalid opcode in undo");
            break;

        case nvm_op_ignore:
            /* Someone must have patched this record to prevent it from
             * being applied. */
            break;

        case nvm_op_restore:
        {
            /* copy saved data to the destination.  */
            nvm_restore ^rst = (nvm_restore ^)tx=>undo_data;
            void ^addr = rst=>addr;
            size_t bytes = opr.size - sizeof(^rst);

            /* Verify the undo is for data in the region */
            if (!nvm_region_rng(td->region, addr, bytes))
                nvms_assert_fail("Applying undo data not in region");

            /* Restore the data */
            nvm_copy(addr, rst=>data, bytes);
            nvm_persist();
            td->persist_abort++;
            break;
        }

        case nvm_op_lock:
        {
            /* Release a lock clearing it from the struct nvm_lkrec. */
            nvm_lkrec ^lk = (nvm_lkrec ^)tx=>undo_data;
            nvm_unlock(tx, td, lk);

            /* remove the lock from the held locks list before discarding
             * the undo persistently. */
            tx=>held_locks ~= lk=>prev;
            nvm_persist();
            td->persist_abort++;
            break;
        }

        case nvm_op_on_unlock:
        {
            /* Call an application on unlock operation. */
            nvm_on_unlock ^ou = (nvm_on_unlock ^)tx=>undo_data;
            nvm_unlock_callback(ou);

            /* remove the lock from the held locks list before discarding
             * the undo persistently. */
            tx=>held_locks ~= ou=>prev;
            nvm_persist();
            td->persist_abort++;
            break;
        }

        case nvm_op_savepoint:
        {
            /* A savepoint is just a marker in the undo. Just remove it from
             * the head of the savepoint list. It is possible that the thread
             * died just after creating this undo but before linking this to
             * the list of savepoints, or just after unlinking but before
             * removing from the transaction. */
            nvm_savepnt ^sp = (nvm_savepnt ^)tx=>undo_data;
            nvm_savepnt ^psp = sp=>prev; // previous sp
            nvm_savepnt ^lsp = tx=>savept_last; //last sp
            if (sp != lsp)
            {
                /* This is the very rare case where it is not the head. If
                 * this happens then the previous should already be the last */
                if (psp != lsp)
                    nvms_corruption("Bad savepoint pointer", psp, lsp);
            }
            else
            {
                tx=>savept_last ~= psp;
            }

            /* in either case persist it since it might not be persistent yet */
            nvm_persist();
            td->persist_abort++;
            break;
        }

        case nvm_op_nested:
        {
            /* We normally never encounter this during rollback since it should
             * be removed by nvm_txend. However it is possible for the record
             * to exist, but not linked to the list of nested transactions.
             * This can happen if this record was created, but before it was
             * made the current transaction, the tread died. This can also
             * happen if a thread dies in nvm_txend after removing the record
             * from the list, but before removing the undo record. In either
             * case the head of the nested transaction list should be the
             * nvm_nested previous to this one. */
            nvm_nested ^nt = (nvm_nested ^)tx=>undo_data;
            nvm_nested ^pnt = nt=>prev; // previous nt
            nvm_nested ^lnt = tx=>nstd_last; // last nt
            if (lnt != pnt)
                nvms_corruption("Applying bad nvm_nested undo record",
                    pnt, lnt);
            break;
        }

        case nvm_op_on_abort:
        {
            /* Call an on abort function in its own nested transaction, unless
             * it was already called. */
            nvm_on_abort ^oa = (nvm_on_abort ^)tx=>undo_data;
            nvm_usid ^ui = (nvm_usid^)%oa=>func;
            if (ui=>d2 == 0)
                break; // already called

            /* Get the function pointer */
            void (*f@)() = *oa=>func;

            /* Call the function in its own nested transaction */
            @ { //nvm_txbegin(0);

                /* clear function usid in the undo to prevent recall if we die
                 * before decrementing undo block record count. This is done
                 * before calling callback so a commit in the callback commits
                 * this too. */
                ui=>d2 @= 0;

                /* Call the application function */
                (*f)(oa=>data);

                /* commit the nested tx unless already committed. */
            } //nvm_txend();

            break;
        }

        case nvm_op_on_commit:
        {
            /* Remove on commit operation from the transaction since it
             * will not be committing this record. Note that it is possible
             * that thread death left this record off the linked list of
             * commit operations. However even in that case pointing the
             * list to the operation before this one is fine since it should
             * be a no-op. */
            nvm_on_commit ^oc = (nvm_on_commit ^)tx=>undo_data;
            tx=>commit_ops ~= oc=>prev;
            nvm_persist();
            td->persist_abort++;
            break;
        }
        }

        /* Adjust the transient data describing the transaction undo */
        tx=>undo_ops--;
        uint32_t sz = (opr.size + sizeof(uint64_t) - 1)
                & ~(sizeof(uint64_t) - 1);
        tx=>undo_data += sz;
        tx=>undo_bytes += sz;
        if (ub=>count % (sizeof(uint64_t) / sizeof(nvm_operation)) == 0)
            tx=>undo_bytes += sizeof(uint64_t); // opcode slot available

        /* Persistently remove this operation from the transaction. */
        ub=>count~--;
        nvm_persist();
        td->persist_abort++;
    }
}
#else
static void nvm_apply_undo(nvm_transaction *tx, uint32_t stop,
        nvm_thread_data *td)
{
    /* Apply until we get to stop or there is no undo at all */
    nvm_undo_blk *ub = nvm_undo_blk_get(&tx->undo);
    nvm_verify(ub, shapeof(nvm_undo_blk)); // verify undo block
    while (ub != NULL)
    {
        /* If this undo block is empty remove it from the transaction. */
        if (ub->count == 0)
        {
            /* Atomically move the block from the transaction to the
             * undo freelist. */
            nvm_drop_undo(tx, ub, td);
            nvm_prep_undo(tx); // prepare for new undo block
            ub = nvm_undo_blk_get(&tx->undo);
            if (ub != NULL)
                nvm_verify(ub, shapeof(nvm_undo_blk)); // verify undo block
            continue;
        }

        /* If we have applied all undo up to our stop record then exit. This
         * is done here rather than at the top of the loop so that all undo
         * blocks will be released if aborting a base transaction. */
        if (tx->undo_ops <= stop)
            break;

        /* Process based on opcode */
        nvm_operation opr = ((nvm_operation*)(ub->data))[ub->count - 1];
        switch (opr.opcode)
        {
        default:
            nvms_assert_fail("Invalid opcode in undo");
            break;

        case nvm_op_ignore:
            /* Someone must have patched this record to prevent it from
             * being applied. */
            break;

        case nvm_op_restore:
        {
            /* copy saved data to the destination.  */
            nvm_restore *rst = (nvm_restore *)tx->undo_data;
            void *addr = void_get(&rst->addr);
            size_t bytes = opr.size - sizeof(*rst);

            /* Verify the undo is for data in the region */
            if (!nvm_region_rng(td->region, addr, bytes))
                nvms_assert_fail("Applying undo data not in region");

            /* Restore the data */
            nvm_copy(addr, rst->data, bytes);
            nvm_persist();
            td->persist_abort++;
            break;
        }

        case nvm_op_lock:
        {
            /* Release a lock clearing it from the struct nvm_lkrec. */
            nvm_lkrec *lk = (nvm_lkrec *)tx->undo_data;
            nvm_unlock(tx, td, lk);

            /* remove the lock from the held locks list before discarding
             * the undo persistently. */
            lk = nvm_lkrec_get(&lk->prev);
            nvm_lkrec_set(&tx->held_locks, lk);
            nvm_persist1(&tx->held_locks);
            td->persist_abort++;
            break;
        }

        case nvm_op_on_unlock:
        {
            /* Call an application on unlock operation. */
            nvm_on_unlock *ou = (nvm_on_unlock *)tx->undo_data;
            nvm_unlock_callback(ou);

            /* remove the lock from the held locks list before discarding
             * the undo persistently. */
            ou = (nvm_on_unlock *)nvm_lkrec_get(&ou->prev);
            nvm_lkrec_set(&tx->held_locks, (nvm_lkrec *)ou);
            nvm_persist1(&tx->held_locks);
            td->persist_abort++;
            break;
        }

        case nvm_op_savepoint:
        {
            /* A savepoint is just a marker in the undo. Just remove it from
             * the head of the savepoint list. It is possible that the thread
             * died just after creating this undo but before linking this to
             * the list of savepoints, or just after unlinking but before
             * removing from the transaction. */
            nvm_savepnt *sp = (nvm_savepnt *)tx->undo_data;
            nvm_savepnt *psp = nvm_savepnt_get(&sp->prev); // previous sp
            nvm_savepnt *lsp = nvm_savepnt_get(&tx->savept_last); //last
            if (sp != lsp)
            {
                /* This is the very rare case where it is not the head. If
                 * this happens then the previous should already be the last */
                if (psp != lsp)
                    nvms_corruption("Bad savepoint pointer", psp, lsp);
            }
            else
            {
                nvm_savepnt_set(&tx->savept_last, psp);
            }

            /* in either case persist it since it might not be persistent yet */
            nvm_persist1(&tx->savept_last);
            td->persist_abort++;
            break;
        }

        case nvm_op_nested:
        {
            /* We normally never encounter this during rollback since it should
             * be removed by nvm_txend. However it is possible for the record
             * to exist, but not linked to the list of nested transactions.
             * This can happen if this record was created, but before it was
             * made the current transaction, the tread died. This can also
             * happen if a thread dies in nvm_txend after removing the record
             * from the list, but before removing the undo record. In either
             * case the head of the nested transaction list should be the
             * nvm_nested previous to this one. */
            nvm_nested *nt = (nvm_nested *)tx->undo_data;
            nvm_nested *pnt = nvm_nested_get(&nt->prev); // previous nt
            nvm_nested *lnt = nvm_nested_get(&tx->nstd_last); // last nt
            if (lnt != pnt)
                nvms_corruption("Applying bad nvm_nested undo record",
                    pnt, lnt);
            break;
        }

        case nvm_op_on_abort:
        {
            /* Call an on abort function in its own nested transaction, unless
             * it was already called. */
            nvm_on_abort *oa = (nvm_on_abort *)tx->undo_data;
            if (oa->func.d2 == 0)
                break; // already called

            /* Get the function pointer */
            void (*f)(void *) = (void (*)(void *))nvm_usid_volatile(oa->func);

            /* Call the function in its own nested transaction */
            nvm_txbegin(0);

            /* clear function usid in the undo to prevent recall if we die
             * before decrementing undo block record count. This is done before
             * calling callback so a commit in the callback commits this too. */
            nvm_undo(&oa->func.d2, sizeof(oa->func.d2));
            oa->func.d2 = 0;
            nvm_flush1(&oa->func.d2);

            /* Call the application function */
            (*f)(oa->data);

            /* commit the nested tx unless already committed. */
            nvm_txend();

            break;
        }

        case nvm_op_on_commit:
        {
            /* Remove on commit operation from the transaction since it
             * will not be committing this record. Note that it is possible
             * that thread death left this record off the linked list of
             * commit operations. However even in that case pointing the
             * list to the operation before this one is fine since it should
             * be a no-op. */
            nvm_on_commit *oc = (nvm_on_commit *)tx->undo_data;
            nvm_on_commit_set(&tx->commit_ops, nvm_on_commit_get(&oc->prev));
            nvm_persist1(&tx->commit_ops);
            td->persist_abort++;
            break;
        }
        }

        /* Adjust the transient data describing the transaction undo */
        tx->undo_ops--;
        uint32_t sz = (opr.size + sizeof(uint64_t) - 1)
                & ~(sizeof(uint64_t) - 1);
        tx->undo_data += sz;
        tx->undo_bytes += sz;
        if (ub->count % (sizeof(uint64_t) / sizeof(nvm_operation)) == 0)
            tx->undo_bytes += sizeof(uint64_t); // opcode slot available

        /* Persistently remove this operation from the transaction. */
        ub->count--;
        nvm_persist1(&ub->count);
        td->persist_abort++;
    }
}
#endif //NVM_EXT

/**
 * Apply the last undo record in the current transaction. This is generally
 * used to throw away an unneeded undo record that is a no-op. For example
 * it is used to remove the nvm_lkrec record for a lock operation that timed
 * out.
 */
#ifdef NVM_EXT
void nvm_apply_one@(nvm_transaction ^tx, nvm_thread_data *td)
{
    /* This is only used by the library itself so error checking is not done */
    nvm_apply_undo(tx, tx=>undo_ops - 1, td);
}
#else
void nvm_apply_one(nvm_transaction *tx, nvm_thread_data *td)
{
    /* This is only used by the library itself so error checking is not done */
    nvm_apply_undo(tx, tx->undo_ops - 1, td);
}
#endif //NVM_EXT

/**
 * This aborts the current transaction applying all undo and releasing
 * locks as the undo is applied. The transaction remains the current
 * transaction until its code block is exited. No more transactional
 * operations are allowed in this transaction.
 *
 * It is an error if there is no current transaction or it is already
 * committed or aborted. If there are any errors then errno is set and the
 * return value is zero.
 *
 * @return
 * One if successful and zero if errno has an error
 *
 * @par Errors:\n
 */
#ifdef NVM_EXT
void nvm_abort@()
{
    /* Get the current transaction and verify its type and state. */
    nvm_thread_data *td = nvm_get_thread_data();
    nvm_transaction ^tx = td->transaction;
    if (tx == 0)
        nvms_assert_fail("Abort without a transaction");

    /* Decide how much undo needs to be applied, and set the transaction
     * to aborting. This is done differently for a base transaction vs.
     * an nested transaction. Once the transaction state is aborting
     * this transaction will abort even if this process dies immediately. */
    uint32_t stop;
    nvm_nested ^nt = tx=>nstd_last;
    if (nt == 0)
    {
        /* Verify state is valid */
        if (tx=>state != nvm_active_state &&
            tx=>state != nvm_aborting_state &&
            tx=>state != nvm_rollback_state)
            nvms_assert_fail("Abort of non-active transaction");

        /* Aborting a base transaction. Apply all undo and set base
         * transaction to aborting */
        stop = 0;
        tx=>state ~= nvm_aborting_state;
        nvm_persist();
        td->persist_abort++;
    }
    else
    {
        /* Verify state is valid */
        if (nt=>state != nvm_active_state &&
            nt=>state != nvm_aborting_state &&
            nt=>state != nvm_rollback_state)
            nvms_assert_fail("Abort of non-active transaction");

        /* Aborting a nested transaction. Set stop to the nested undo
         * operation itself. Until nvm_txend is called, we want the operation
         * to be part of the transaction with nstd_last pointing at it.
         * Once everything except the nested operation itself has been
         * applied, the effect of the abort is complete, */
        stop = nt=>undo_ops + 1;
        nt=>state ~= nvm_aborting_state;
        nvm_persist();
        td->persist_abort++;
    }

    /* Roll back to the transaction beginning. This releases locks as
     * soon as the undo generated after them is applied. */
    nvm_apply_undo(tx, stop, td);

    /* All the effects of the transaction have been rolled back. The state
     * can now be set to aborted so that nvm_txend can be called.  */
    if (nt == 0)
    {
        /* This is a base transaction so the state is in *tx */
        tx=>state ~= nvm_aborted_state;
        nvm_persist();
        td->persist_abort++;
    }
    else
    {
        /* This is a nested transaction so the state is in *nt */
        nt=>state ~= nvm_aborted_state;
        nvm_persist();
        td->persist_abort++;
    }
}
#else
void nvm_abort()
{
    /* Get the current transaction and verify its type and state. */
    nvm_thread_data *td = nvm_get_thread_data();
    nvm_transaction *tx = td->transaction;
    if (tx == NULL)
        nvms_assert_fail("Abort without a transaction");
    nvm_verify(tx, shapeof(nvm_transaction));

    /* Decide how much undo needs to be applied, and set the transaction
     * to aborting. This is done differently for a base transaction vs.
     * an nested transaction. Once the transaction state is aborting
     * this transaction will abort even if this process dies immediately. */
    uint32_t stop;
    nvm_nested *nt = nvm_nested_get(&tx->nstd_last);
    if (nt == NULL)
    {
        /* Verify state is valid */
        if (tx->state != nvm_active_state &&
            tx->state != nvm_aborting_state &&
            tx->state != nvm_rollback_state)
            nvms_assert_fail("Abort of non-active transaction");

        /* Aborting a base transaction. Apply all undo and set base
         * transaction to aborting */
        stop = 0;
        tx->state = nvm_aborting_state;
        nvm_persist1(&tx->state);
        td->persist_abort++;
    }
    else
    {
        /* Verify state is valid */
        if (nt->state != nvm_active_state &&
            nt->state != nvm_aborting_state &&
            nt->state != nvm_rollback_state)
            nvms_assert_fail("Abort of non-active transaction");

        /* Aborting a nested transaction. Set stop to the nested undo
         * operation itself. Until nvm_txend is called, we want the operation
         * to be part of the transaction with nstd_last pointing at it.
         * Once everything except the nested operation itself has been
         * applied, the effect of the abort is complete, */
        stop = nt->undo_ops + 1;
        nt->state = nvm_aborting_state;
        nvm_persist1(&nt->state);
        td->persist_abort++;
    }

    /* Roll back to the transaction beginning. This releases locks as
     * soon as the undo generated after them is applied. */
    nvm_apply_undo(tx, stop, td);

    /* All the effects of the transaction have been rolled back. The state
     * can now be set to aborted so that nvm_txend can be called.  */
    if (nt == NULL)
    {
        /* This is a base transaction so the state is in *tx */
        tx->state = nvm_aborted_state;
        nvm_persist1(&tx->state);
        td->persist_abort++;
    }
    else
    {
        /* This is a nested transaction so the state is in *nt */
        nt->state = nvm_aborted_state;
        nvm_persist1(&nt->state);
        td->persist_abort++;
    }
}
#endif //NVM_EXT

/**
 * This creates a savepoint in the current transaction. The savepoint is an
 * undo record that defines a named point in the undo of a transaction. The
 * name for the save point is an NVM address of some location in the NVM
 * region the transaction modifies. The return value is a pointer to the
 * NVM location where the name is stored. This allows a null pointer to be
 * passed when creating the savepoint and then setting the real name later
 * when it has been chosen. For example the name could be the address of an
 * object that is allocated after setting the savepoint. Then rollback to
 * savepoint will delete the object.
 *
 * Setting a savepoint allows subsequent changes to be rolled back without
 * aborting the entire transaction. Unlike a nested transaction, the
 * changes under a savepoint do not commit until the current transaction
 * commits and locks acquired after the savepoint are held until the
 * current transaction commits.
 *
 * There may be multiple savepoints in the same transaction. They may even
 * have the same name. Rollback to savepoint stops at the first matching s
 * avepoint.
 *
 * If there are any errors then errno is set and the return value is zero.
 *
 * @param[in] name
 * This is an address in NVM that is used as a name for the savepoint. It
 * can be changed after the savepoint is created.
 *
 * @return
 * If successful a pointer to the savepoint name in the undo record is
 * returned. Store through the pointer to change the name.
 *
 * @par Errors:\n
 */
#ifdef NVM_EXT
void ^^nvm_savepoint@(
        void ^name
        )
{
    /* Get the current transaction and verify its type and state. */
    nvm_thread_data *td = nvm_get_thread_data();
    nvm_transaction ^tx = td->transaction;
    if (tx == 0)
        nvms_assert_fail("Generating savepoint without a transaction");
    if (nvm_get_state(tx) != nvm_active_state)
        nvms_assert_fail("Generating savepoint in non-active transaction");

    /* If we cannot get the struct in this block, allocate a new undo block. */
    if (tx=>undo_bytes < sizeof(nvm_savepnt))
        nvm_add_undo(tx, td); // add a new empty undo block

    /* Get the address to store the data for the operation. */
    nvm_savepnt ^sp = (nvm_savepnt ^)
            (tx=>undo_data - sizeof(nvm_savepnt));

    /* Set the links for the other savepoint operations in this transaction */
    sp=>undo_ops ~= tx=>undo_ops;
    sp=>prev ~= tx=>savept_last;

    /* Store the name in the savepoint */
    sp=>name ~= name;

    /* Add to the transaction. This makes it persistently part of the
     * transaction. */
    nvm_add_oper(tx, nvm_op_savepoint, sizeof(nvm_savepnt));

    /* The operation is persistently recorded as a savepoint operation.
     * However it is not on the list of savepoints yet. */
    tx=>savept_last ~= sp;
    nvm_persist();

    /* return the address of the name so that the client can change it.*/
    return %sp=>name;
}
#else
void_srp *nvm_savepoint(
        void *name
        )
{
    /* Get the current transaction and verify its type and state. */
    nvm_thread_data *td = nvm_get_thread_data();
    nvm_transaction *tx = td->transaction;
    if (tx == NULL)
        nvms_assert_fail("Generating savepoint without a transaction");
    nvm_verify(tx, shapeof(nvm_transaction));
    if (nvm_get_state(tx) != nvm_active_state)
        nvms_assert_fail("Generating savepoint in non-active transaction");

    /* If we cannot get the struct in this block, allocate a new undo block.
     * Otherwise verify current undo block. */
    if (tx->undo_bytes < sizeof(nvm_savepnt))
        nvm_add_undo(tx, td); // add a new empty undo block
    else
        nvm_verify(nvm_undo_blk_get(&tx->undo), shapeof(nvm_undo_blk));

    /* Get the address to store the data for the operation. */
    nvm_savepnt *sp = (nvm_savepnt *)
            (tx->undo_data - sizeof(nvm_savepnt));

    /* Set the links for the other savepoint operations in this transaction */
    sp->undo_ops = tx->undo_ops;
    nvm_savepnt_set(&sp->prev, nvm_savepnt_get(&tx->savept_last));

    /* Store the name in the savepoint */
    void_set(&sp->name, name);

    /* flush the savepoint data*/
    nvm_flush(sp, sizeof(*sp));

    /* Add to the transaction. This makes it persistently part of the
     * transaction. */
    nvm_add_oper(tx, nvm_op_savepoint, sizeof(nvm_savepnt));

    /* The operation is persistently recorded as a savepoint operation.
     * However it is not on the list of savepoints yet. */
    nvm_savepnt_set(&tx->savept_last, sp);
    nvm_persist1(&tx->savept_last);

    /* return the address of the name so that the client can change it.*/
    return &sp->name;
}
#endif //NVM_EXT

/**
 * This rolls back the current transaction until the first savepoint with
 * the given name is encountered. This is the most recently created
 * savepoint with the given name since undo is applied in reverse of the
 * order it is generated. It is not an error to have multiple savepoints
 * with the same name. An error is returned if there is no savepoint with
 * the given name in the current transaction. A nested transaction cannot
 * rollback to a savepoint in its parent transaction.
 *
 * If there are any errors then errno is set and the return value is zero.
 * No rollback is done if there is an error.
 *
 * @param[in] name
 * This is the name of the savepoint to rollback to. It is an address in
 * NVM.
 *
 * @return
 * One on success and zero on error
 *
 * @par Errors:\n
 */
#ifdef NVM_EXT
int nvm_rollback@(
        void ^name
        )
{
    /* Get the current transaction and verify its type and state. */
    nvm_thread_data *td = nvm_get_thread_data();
    nvm_transaction ^tx = td->transaction;
    if (tx == 0)
        nvms_assert_fail("Rolling back without a transaction");
    if (nvm_get_state(tx) != nvm_active_state)
        nvms_assert_fail("Rolling back in non-active transaction");

    /* Find the save point to ensure it exists, and to know how far
     * back it is. Note that it must be within this transaction and not in
     * a parent. */
    nvm_savepnt ^sp = tx=>savept_last;
    while (1)
    {
        if (sp == 0)
            nvms_assert_fail("Rollback to non-existing save point");
        if (sp=>name == name)
            break;
        sp = sp=>prev;
    }

    /* fire error if save point is in parent transaction */
    uint32_t stop = sp=>undo_ops;
    nvm_nested ^nt = tx=>nstd_last;
    if (nt && nt=>undo_ops > sp=>undo_ops)
        nvms_assert_fail("Rollback to save point in parent transaction");

    /* Do the rollback. This will also apply the savepoint itself. We set the
     * state to rollback so any on abort callbacks can tell if they are in
     * rollback to savepoint or abort. */
    if (nt)
        nt=>state ~= nvm_rollback_state;
    else
        tx=>state ~= nvm_rollback_state;
    nvm_apply_undo(tx, stop, td); // rollback
    if (nt)
        nt=>state ~= nvm_active_state;
    else
        tx=>state ~= nvm_active_state;

    return 1;
}
#else
int
nvm_rollback(
        void *name
        )
{
    /* Get the current transaction and verify its type and state. */
    nvm_thread_data *td = nvm_get_thread_data();
    nvm_transaction *tx = td->transaction;
    if (tx == NULL)
        nvms_assert_fail("Rolling back without a transaction");
    nvm_verify(tx, shapeof(nvm_transaction));
    if (nvm_get_state(tx) != nvm_active_state)
        nvms_assert_fail("Rolling back in non-active transaction");

    /* Find the save point to ensure it exists, and to know how far
     * back it is. Note that it must be within this transaction and not in
     * a parent. */
    nvm_savepnt *sp = nvm_savepnt_get(&tx->savept_last);
    while (1)
    {
        if (sp == NULL)
            nvms_assert_fail("Rollback to non-existing save point");
        if (void_get(&sp->name) == name)
            break;
        sp = nvm_savepnt_get(&sp->prev);
    }

    /* fire error if save point is in parent transaction */
    uint32_t stop = sp->undo_ops;
    nvm_nested *nt = nvm_nested_get(&tx->nstd_last);
    if (nt && nt->undo_ops > sp->undo_ops)
        nvms_assert_fail("Rollback to save point in parent transaction");

    /* Do the rollback. This will also apply the savepoint itself. We set the
     * state to rollback so any on abort callbacks can tell if they are in
     * rollback to savepoint or abort. */
    if (nt)
        nt->state = nvm_rollback_state;
    else
        tx->state = nvm_rollback_state;
    nvm_apply_undo(tx, stop, td); // rollback
    if (nt)
        nt->state = nvm_active_state;
    else
        tx->state = nvm_active_state;

    return 1;
}
#endif //NVM_EXT

/**
 * This returns the current  transaction configuration for a region.
 *
 * @param[in] desc
 * This is the region descriptor returned by nvm_create_region or
 * nvm_attach_region for the region to query.
 *
 * @param[out] cfg
 * This points to a buffer in volatile memory where the configuration is
 * returned.
 *
 * If the descriptor is bad then the return value is zero.
 */
#ifdef NVM_EXT
int nvm_get_txconfig(
        nvm_desc desc,
        nvm_txconfig *cfg
        )
{
    /* return all zero if invalid descriptor*/
    cfg->txn_slots = 0;
    cfg->undo_blocks = 0;
    cfg->undo_limit = 0;

    /* Error if requested descriptor is too large or zero */
    nvm_app_data *ad = nvm_get_app_data();
    nvm_desc rmax = ad->params.max_regions;
    if (desc > rmax || desc == 0)
        return 0;

    /* Error if desc is not for a valid region */
    nvm_region_data *rd = ad->regions[desc];
    if (rd == NULL)
        return 0;

    /* Get data from region */
    nvm_region ^rg = rd->region;
    cfg->txn_slots = rg=>max_transactions;
    cfg->undo_limit = rg=>max_undo_blocks;

    /* count entries in linked list of transaction tables. */
    nvm_trans_table ^tt = rg=>nvtt_list;
    cfg->undo_blocks = 0;
    while (tt)
    {
        cfg->undo_blocks += TRANS_TABLE_UNDO_BLKS;
        tt = tt=>link;
    }
    return 1;
}
#else
int nvm_get_txconfig(
        nvm_desc desc,
        nvm_txconfig *cfg
        )
{
    /* return all zero if invalid descriptor*/
    cfg->txn_slots = 0;
    cfg->undo_blocks = 0;
    cfg->undo_limit = 0;

    /* Error if requested descriptor is too large or zero */
    nvm_app_data *ad = nvm_get_app_data();
    nvm_desc rmax = ad->params.max_regions;
    if (desc > rmax || desc == 0)
        return 0;

    /* Error if desc is not for a valid region */
    nvm_region_data *rd = ad->regions[desc];
    if (rd == NULL)
        return 0;

    /* Get data from region */
    nvm_region *rg = rd->region;
    cfg->txn_slots = rg->max_transactions;
    //    cfg->undo_blocks = rg->nvtt_cnt * TRANS_TABLE_UNDO_BLKS;
    cfg->undo_limit = rg->max_undo_blocks;

    /* count entries in linked list of transaction tables. */
    nvm_trans_table *tt = nvm_trans_table_get(&rg->nvtt_list);
    cfg->undo_blocks = 0;
    while (tt)
    {
        cfg->undo_blocks += TRANS_TABLE_UNDO_BLKS;
        tt = nvm_trans_table_get(&tt->link);
    }
    return 1;
}
#endif //NVM_EXT

/**
 * This is the context for the callback function to modify the transaction
 * configuration of a region.
 */
#ifdef NVM_EXT
persistent struct nvm_txconfig_ctx
{
    /**
     * This is the head of the linked list on nvm_trans_table structs to add
     * to the region. If only the maximum transaction count and maximum undo
     * blocks per transaction are being changed then this is null.
     */
    nvm_trans_table ^tt_append;

    /**
     * This is the new limit on the maximum number of simultaneous transactions
     */
    uint32_t txn_slots;

    /**
     * This is the new limit on the maximum number of undo blocks that a single
     * transaction, including nested transactions, can consume.
     */
    uint32_t undo_limit;
};
typedef persistent struct nvm_txconfig_ctx nvm_txconfig_ctx;
#else
struct nvm_txconfig_ctx
{
    /**
     * This is the head of the linked list on nvm_trans_table structs to add
     * to the region. If only the maximum transaction count and maximum undo
     * blocks per transaction are being changed then this is null.
     */
    nvm_trans_table_srp tt_append; // nvm_trans_table ^tt_append

    /**
     * This is the new limit on the maximum number of simultaneous transactions
     */
    uint32_t txn_slots;

    /**
     * This is the new limit on the maximum number of undo blocks that a single
     * transaction, including nested transactions, can consume.
     */
    uint32_t undo_limit;
};
typedef struct nvm_txconfig_ctx nvm_txconfig_ctx;
#endif //NVM_EXT
extern const nvm_extern nvm_extern_nvm_txconfig_callback;
/**
 * This is the on unlock callback function for setting the new transaction
 * configuration for a region. It is called after the commit of any new
 * transaction table allocations and before the NVM mutex for the region is
 * released. It uses careful ordering of its stores to maintain consistence
 * rather than undo since the transaction and undo freelists are constructed
 * before any recovery rollback. The freelists would be wrong if we died just
 * after this operation appended the new transaction tables and before the
 * nested transaction committed. By not generating undo the tables list will
 * not be rolled back after it is used.
 *
 * @param ctx
 * The new transaction configuration to save
 */
#ifdef NVM_EXT
USID("7ac9 8b1f 63f3 5abb 5a50 281f 9047 07e2")
void nvm_txconfig_callback@(nvm_txconfig_ctx ^ctx)
{
    /* If this is not for a successful commit of the owning transaction, then
     * ignore this operation. This ensures that death before the context is
     * populated with correct values will not save bogus results. */
    if (nvm_txstatus(1) != nvm_committing_state)
        return;

    /* Get the current transaction's region */
    nvm_thread_data *td = nvm_get_thread_data();
    nvm_region ^rg = td->region;

    /* First save the new limits before adding the new tables. This means
     * that if the transaction table list is updated then we are sure that
     * the limits were updated. This is also idempotent in case it is caled
     * more than once. */
    rg=>max_transactions ~= ctx=>txn_slots;
    rg=>max_undo_blocks ~= ctx=>undo_limit;
    nvm_persist(); // ensure this becomes persistent

    /* This may have reduced the maximum number of concurrent transactions
     * allowed leaving some transactions marked as idle which should be
     * reserved. This is resolved by going through the free transaction
     * list looking for transaction slots that need their state changed.
     * When found the state is persistently changed and the transaction
     * removed from the free list.
     * There could be slots that are in use which need to be reserved. They
     * will be marked reserved when the transaction is ended. */
    nvm_app_data *ad = nvm_get_app_data();
    nvm_transaction ^tx = td->transaction; // this thread's current transaction
    nvm_trans_table_data *ttd = ad->regions[tx=>desc]->trans_table;

    /* lock the transaction freelist. */
    nvms_lock_mutex(ttd->trans_mutex, 1);

    /* scan the freelist. */
    nvm_transaction ^ftx = ttd->free_trans; // free transaction
    nvm_transaction ^ptx = 0; // previous free transaction
    while (ftx)
    {
        if (ftx=>slot > ctx=>txn_slots && ftx=>state == nvm_idle_state)
        {
            /* This transaction should be reserved. No undo is needed since
             * the new config is committed. If we die here this will be rerun
             * at recovery and complete fixing the freelist. */
            ftx=>state ~= nvm_reserved_state;
            nvm_persist();
            ttd->rtrans_cnt++; // one more reserved

            /* Remove from free list */
            if (ptx)
            {
                ptx=>link = ftx=>link; // link previous around it
            }
            else
            {
                ttd->free_trans = ftx=>link; // new head of freelist
            }
        }

        ptx = ftx;
        ftx = ftx=>link; // next transaction in freelist
    }

    /* This may have increased the maximum number of concurrent transactions
     * requiring some transactions to be changed from state reserved to idle,
     * and added to the transaction freelist. We scan the existing transaction
     * tables looking for them. */
    nvm_trans_table ^htt = rg=>nvtt_list; // head trans table
    nvm_trans_table ^tt = htt; // loop variable
    while (tt)
    {
        int t;
        for (t = 0; t < TRANS_TABLE_SLOTS; t++)
        {
            /* get the next transaction */
            nvm_transaction ^tx = %tt=>transactions[t];

            /* if reserved now but slot should be idle */
            if (tx=>slot <= ctx=>txn_slots && tx=>state == nvm_reserved_state)
            {
                /* Make idle and put at head of freelist. The new state will
                 * persist as part of committing the current transaction. */
                tx=>state ~= nvm_idle_state;
                tx=>link = ttd->free_trans;
                ttd->free_trans = tx;
                ttd->rtrans_cnt--; // one less reserved
            }

        }

        /* go to next table */
        tt = tt=>link;
    }

    /* Done with transaction freelist for now */
    nvms_unlock_mutex(ttd->trans_mutex);

    /* If there are no transaction tables to link in then we are done. */
    nvm_trans_table ^att = ctx=>tt_append;
    if (!att)
    {
        return;
    }

    /* Scan the list of transaction tables to see if this is already in the
     * list, and to find the end if it is not. */
    tt = htt; // loop variable
    nvm_trans_table ^ltt = NULL; // last trans table for linking new list to
    while (tt)
    {

        /* Already done if new tables are in the list. Note this also means
         * the new transactions are in the freelist and the new undo blocks
         * are in their freelist since if we die after linking in the new
         * tables the next recovery will rebuild. */
        if (tt == att)
        {
            return;
        }

        /* Save last trans table in case we need to link in new tables */
        ltt = tt;

        /* go to next table */
        tt = tt=>link;
    }

    /* Add new transaction tables to the region and ensure it is persistent.
     * This is the effective commit of this transaction. */
    ltt=>link ~= att;
    nvm_persist();

    /* Scan the new transaction tables adding new transaction slots to the
     * freelist. First lock the transaction freelist. */
    nvms_lock_mutex(ttd->trans_mutex, 1);
    tt = att;
    while (tt)
    {
        int t;
        for (t = 0; t < TRANS_TABLE_SLOTS; t++)
        {
            /* If slot is idle, add to freelist. Note that the state was
             * previously set to either reserved or idle based on the
             * maximum number of allowed transactions being configured. */
            nvm_transaction ^tx = %tt=>transactions[t];
            if (tx=>state == nvm_idle_state)
            {
                /* put at head of freelist */
                tx=>link = ttd->free_trans;
                ttd->free_trans = tx;
            }
            else
                ttd->rtrans_cnt++; // one more reserved transaction
        }

        /* Increase total transaction count to include this transaction table */
        ttd->ttrans_cnt += TRANS_TABLE_SLOTS;

        /* go to next table */
        tt = tt=>link;
    }

    /* Done with transaction freelist */
    nvms_unlock_mutex(ttd->trans_mutex);

    /* Scan the new transaction tables adding all new undo blocks to the undo
     * freelist. Note that all undo blocks are put in use, even if there are
     * more than requested. First lock the undo block freelist. */
    nvms_lock_mutex(ttd->undo_mutex, 1);
    tt = att;
    while (tt)
    {
        int u;
        for (u = 0; u < TRANS_TABLE_UNDO_BLKS; u++)
        {
            /* If slot is idle, add to freelist. Note that the state was
             * previously set to either reserved or idle based on the
             * maximum number of allowed transactions being configured. */
            nvm_undo_blk ^ub = %tt=>undo_blks[u];

            /* put at head of freelist */
            ub=>link ~= ttd->free_undo;
            ttd->free_undo = ub;
        }

        /* go to next table */
        tt = tt=>link;
    }

    /* Done with undo block freelist */
    nvms_unlock_mutex(ttd->undo_mutex);

}
#else
void nvm_txconfig_callback(nvm_txconfig_ctx *ctx)
{
    /* If this is not for a successful commit of the owning transaction, then
     * ignore this operation. This ensures that death before the context is
     * populated with correct values will not save bogus results. */
    if (nvm_txstatus(1) != nvm_committing_state)
        return;

    /* Get the current transaction's region */
    nvm_thread_data *td = nvm_get_thread_data();
    nvm_region *rg = td->region;
    nvm_verify(rg, shapeof(nvm_region));

    /* First save the new limits before adding the new tables. This means
     * that if the transaction table list is updated then we are sure that
     * the limits were updated. This is also idempotent in case it is caled
     * more than once. */
    rg->max_transactions = ctx->txn_slots;
    nvm_flush1(&rg->max_transactions);
    rg->max_undo_blocks = ctx->undo_limit;
    nvm_flush1(&rg->max_undo_blocks);
    nvm_persist(); // ensure this becomes persistent

    /* This may have reduced the maximum number of concurrent transactions
     * allowed leaving some transactions marked as idle which should be
     * reserved. This is resolved by going through the free transaction
     * list looking for transaction slots that need their state changed.
     * When found the state is persistently changed and the transaction
     * removed from the free list.
     * There could be slots that are in use which need to be reserved. They
     * will be marked reserved when the transaction is ended. */
    nvm_app_data *ad = nvm_get_app_data();
    nvm_transaction *tx = td->transaction; // this thread's current transaction
    nvm_trans_table_data *ttd = ad->regions[tx->desc]->trans_table;

    /* lock the transaction freelist. */
    nvms_lock_mutex(ttd->trans_mutex, 1);

    /* scan the freelist. */
    nvm_transaction *ftx = ttd->free_trans; // free transaction
    nvm_transaction *ptx = 0; // previous free transaction
    while (ftx)
    {
        //        printf("ftx=%p ftx->link=%p ptx=%p ptx->link=%p\n",
        //                 ftx, ftx->link, ptx, ptx ? ptx->link : NULL);
        nvm_verify(ftx, shapeof(nvm_transaction));
        if (ftx->slot > ctx->txn_slots && ftx->state == nvm_idle_state)
        {
            /* This transaction should be reserved. No undo is needed since
             * the new config is committed. If we die here this will be rerun
             * at recovery and complete fixing the freelist. */
            ftx->state = nvm_reserved_state;
            nvm_persist1(&ftx->state);
            ttd->rtrans_cnt++; // one more reserved

            /* Remove from free list */
            if (ptx)
            {
                ptx->link = ftx->link; // link previous around it
            }
            else
            {
                ttd->free_trans = ftx->link; // new head of freelist
            }
        }

        ptx = ftx;
        ftx = ftx->link; // next transaction in freelist
    }

    /* This may have increased the maximum number of concurrent transactions
     * requiring some transactions to be changed from state reserved to idle,
     * and added to the transaction freelist. We scan the existing transaction
     * tables looking for them. */
    nvm_trans_table *htt = nvm_trans_table_get(&rg->nvtt_list); // list head
    nvm_trans_table *tt = htt; // loop variable
    while (tt)
    {
        nvm_verify(tt, shapeof(nvm_trans_table));
        int t;
        for (t = 0; t < TRANS_TABLE_SLOTS; t++)
        {
            /* get the next transaction */
            nvm_transaction *tx = &tt->transactions[t];
            nvm_verify(tx, shapeof(nvm_transaction));

            /* if reserved now but slot should be idle */
            if (tx->slot <= ctx->txn_slots && tx->state == nvm_reserved_state)
            {
                /* Make idle and put at head of freelist */
                tx->state = nvm_idle_state;
                tx->link = ttd->free_trans;
                ttd->free_trans = tx;
                ttd->rtrans_cnt--; // one less reserved
            }

        }

        /* go to next table */
        tt = nvm_trans_table_get(&tt->link);
    }

    /* Done with transaction freelist for now */
    nvms_unlock_mutex(ttd->trans_mutex);

    /* If there are no transaction tables to link in then we are done. */
    nvm_trans_table *att = nvm_trans_table_get(&ctx->tt_append);
    if (!att)
    {
        return;
    }
    nvm_verify(att, shapeof(nvm_trans_table));

    /* Scan the list of transaction tables to see if this is already in the
     * list, and to find the end if it is not. */
    tt = htt; // loop variable
    nvm_trans_table *ltt = NULL; // last trans table for linking new list to
    while (tt)
    {

        /* Already done if new tables are in the list. Note this also means
         * the new transactions are in the freelist and the new undo blocks
         * are in their freelist since if we die after linking in the new
         * tables the next recovery will rebuild. */
        if (tt == att)
        {
            return;
        }

        /* Save last trans table in case we need to link in new tables */
        ltt = tt;

        /* go to next table */
        tt = nvm_trans_table_get(&tt->link);
    }

    /* Add new transaction tables to the region and ensure it is persistent.
     * This is the effective commit of this transaction. */
    nvm_trans_table_set(&ltt->link, att);
    nvm_persist1(&ltt->link);

    /* Scan the new transaction tables adding new transaction slots to the
     * freelist. First lock the transaction freelist. */
    nvms_lock_mutex(ttd->trans_mutex, 1);
    tt = att;
    while (tt)
    {
        nvm_verify(tt, shapeof(nvm_trans_table));
        int t;
        for (t = 0; t < TRANS_TABLE_SLOTS; t++)
        {
            /* If slot is idle, add to freelist. Note that the state was
             * previously set to either reserved or idle based on the
             * maximum number of allowed transactions being configured. */
            nvm_transaction *tx = &tt->transactions[t];
            nvm_verify(tx, shapeof(nvm_transaction));
            if (tx->state == nvm_idle_state)
            {
                /* put at head of freelist */
                tx->link = ttd->free_trans;
                ttd->free_trans = tx;
            }
            else
                ttd->rtrans_cnt++; // one more reserved transaction
        }

        /* Increase total transaction count to include this transaction table */
        ttd->ttrans_cnt += TRANS_TABLE_SLOTS;

        /* go to next table */
        tt = nvm_trans_table_get(&tt->link);
    }

    /* Done with transaction freelist */
    nvms_unlock_mutex(ttd->trans_mutex);

    /* Scan the new transaction tables adding all new undo blocks to the undo
     * freelist. Note that all undo blocks are put in use, even if there are
     * more than requested. First lock the undo block freelist. */
    nvms_lock_mutex(ttd->undo_mutex, 1);
    tt = att;
    while (tt)
    {
        nvm_verify(tt, shapeof(nvm_trans_table));
        int u;
        for (u = 0; u < TRANS_TABLE_UNDO_BLKS; u++)
        {
            /* If slot is idle, add to freelist. Note that the state was
             * previously set to either reserved or idle based on the
             * maximum number of allowed transactions being configured. */
            nvm_undo_blk *ub = &tt->undo_blks[u];
            nvm_verify(ub, shapeof(nvm_undo_blk));

            /* put at head of freelist */
            nvm_undo_blk_set(&ub->link, ttd->free_undo);
            ttd->free_undo = ub;
        }

        /* go to next table */
        tt = nvm_trans_table_get(&tt->link);
    }

    /* Done with undo block freelist */
    nvms_unlock_mutex(ttd->undo_mutex);

}
#endif //NVM_EXT

/**
 * This changes the transaction configuration for a region. The new values
 * may be greater than or less than the old values. If there are fewer
 * transaction slots than application threads then there is the possibility
 * that a thread that wants to begin a transaction may have to wait until a
 * transaction commits or aborts. If the number of transaction slots times
 * the limit of undo blocks per transaction is greater than the total
 * number of undo blocks, then there is the possibility that a transaction
 * may have to wait when generating undo until another transaction releases
 * an undo block. However this may be unlikely.
 *
 * Transaction slots and undo blocks are allocated in megabyte chunks of 63
 * slots and 254 undo blocks (this could change). Thus requesting 64 slots
 * will actually allocate 126 slots. However the slot limit will be honored
 * and the extra slots left unused. Similarly, requesting 255 undo blocks
 * will actually allocate 508 undo blocks. Since extra undo blocks cannot
 * hurt, all available undo blocks will be usable and reported by
 * nvm_get_txconfig.
 *
 * The configuration is changed in a transaction that commits before
 * returning. If this is called inside a transaction then a nested
 * transaction is created and committed. The configuration is changed
 * atomically.
 *
 * If there are any errors then errno is set and the return value is zero.
 * The new configuration must be reasonable.
 *
 * @param[in] region
 * This is the region descriptor returned by nvm_create_region or
 * nvm_attach_region for the region to change.
 *
 * @param[in] cfg
 * This is a buffer containing the new configuration data.
 *
 * @return
 * One on success and zero on error.
 *
 * @par Errors:\n
 */
#ifdef NVM_EXT
int nvm_set_txconfig(
        nvm_desc desc,
        nvm_txconfig *cfg
        )
{
    /* Check the configuration for being reasonable. */
    if (cfg->txn_slots < 2 || cfg->undo_blocks < 8 || cfg->undo_limit < 2 ||
        cfg->undo_blocks < (cfg->txn_slots * cfg->undo_limit) / 2)
    {
        errno = EINVAL;
        return 0;
    }

    /* get thread data pointers */
    nvm_thread_data *td = nvm_get_thread_data();

    /* First decide how many nvm_trans_table structs are needed to satisfy this
     * configuration. */
    uint32_t tbls = (cfg->txn_slots + TRANS_TABLE_SLOTS - 1)
            / TRANS_TABLE_SLOTS;
    if (cfg->undo_blocks > tbls * TRANS_TABLE_UNDO_BLKS)
        tbls = (cfg->undo_blocks + TRANS_TABLE_UNDO_BLKS - 1)
        / TRANS_TABLE_UNDO_BLKS;

    /* Begin a transaction to modify the region. */
    @ desc { //nvm_txbegin(desc);
        /* X lock the region to stabilize the transaction table list. */
        nvm_region ^rg = td->region;
        nvm_xlock(%rg=>reg_mutex);

        /* Count the current number of transaction tables. Due to the unusual
         * mechanism for atomically updating the linked list, it is not
         * possible to atomically maintain the list length in the nvm_region. */
        nvm_trans_table ^htt = rg=>nvtt_list; // head trans table
        nvm_trans_table ^tt = htt; // loop variable
        uint32_t xcount = 0; // count of existing transaction tables
        while (tt)
        {
            xcount++;
            tt = tt=>link;
        }

        nvm_trans_table ^att = 0; // list of trans tables to append
        nvm_trans_table ^ett = 0; // current end of list

        /* If there are not enough trans tables allocate new ones to add.
         * Note that there is no attempt to release excess trans tables. It
         * is too difficult to deal with the possibility that some of the
         * undo blocks to delete are in use. */
        if (xcount < tbls)
        {
            /* Allocate the new trans tables, initialize them, and link to the
             * append list for later linking to the existing trans tables. Link
             * to the end of the list to keep the slot numbers in order. */
            uint16_t slots = xcount*TRANS_TABLE_SLOTS;
            uint32_t n;
            for (n = xcount; n < tbls; n++, slots += TRANS_TABLE_SLOTS)
            {
                /* Allocate another trans table */
                nvm_trans_table ^ntt = nvm_alloc(rg=>rootHeap,
                        shapeof(nvm_trans_table), 1);
                if (ntt == 0)
                {
                    errno = ENOSPC;
                    nvm_abort();
                    return 0;
                }

                /* Initialize the new transaction slots. Undo not needed since
                 * they were just allocated. The state is initialized to
                 * reserved or idle based on the new maximum concurrent
                 * transactions. */
                int s;
                nvm_transaction ^tx = %ntt=>transactions[0];
                for (s = 0; s < TRANS_TABLE_SLOTS; s++, tx++)
                {
                    tx=>slot ~= slots + s + 1;
                    tx=>state ~= tx=>slot > cfg->txn_slots ?
                            nvm_reserved_state : nvm_idle_state;
                }

                /* link new trans table to append list. */
                if (att)
                {
                    /* add at end of existing list. */
                    ett=>link ~= ntt;
                    ett = ntt;
                }
                else
                {
                    /* First allocation so just init list pointers. */
                    ett = att = ntt;
                }
            }
        }

        /* Create an on unlock operation to update the transaction parameters
         * in the nvm_region. This cannot be done with normal undo because
         * recovery scans the transaction tables for transactions to recover
         * and constructs the freelists before any undo is applied. Thus we
         * cannot use undo to make the data structures consistent before they
         * are used. We must ensure the new allocations are committed before
         * we link them to the list of transaction tables, and we must ensure
         * that they are linked to the list if the allocation is committed.
         * This must be done with careful ordering rather than with undo.
         * This is done in an on unlock operation to ensure the updates are
         * made while the region lock is still held and the transaction is
         * guaranteed committed. If we die while initializing the context,
         * then the transaction does not commit and the context is not used. */
        nvm_txconfig_ctx ^ctx =
                nvm_onunlock(|nvm_txconfig_callback);
        ctx=>tt_append ~= att;
        ctx=>txn_slots ~= cfg->txn_slots;
        ctx=>undo_limit ~= cfg->undo_limit;
    } //nvm_txend();
    return 1;
}
#else
int nvm_set_txconfig(
        nvm_desc desc,
        nvm_txconfig *cfg
        )
{
    /* Check the configuration for being reasonable. */
    if (cfg->txn_slots < 2 || cfg->undo_blocks < 8 || cfg->undo_limit < 2 ||
        cfg->undo_blocks < (cfg->txn_slots * cfg->undo_limit) / 2)
    {
        errno = EINVAL;
        return 0;
    }

    /* get thread data pointers */
    nvm_thread_data *td = nvm_get_thread_data();

    /* First decide how many nvm_trans_table structs are needed to satisfy this
     * configuration. */
    uint32_t tbls = (cfg->txn_slots + TRANS_TABLE_SLOTS - 1)
            / TRANS_TABLE_SLOTS;
    if (cfg->undo_blocks > tbls * TRANS_TABLE_UNDO_BLKS)
        tbls = (cfg->undo_blocks + TRANS_TABLE_UNDO_BLKS - 1)
        / TRANS_TABLE_UNDO_BLKS;

    /* Begin a transaction to modify the region. */
    nvm_txbegin(desc);
    {
        /* X lock the region to stabilize the transaction table list. */
        nvm_region *rg = td->region;
        nvm_xlock(&rg->reg_mutex);

        /* Count the current number of transaction tables. Due to the unusual
         * mechanism for atomically updating the linked list, it is not
         * possible to atomically maintain the list length in the nvm_region. */
        nvm_trans_table *htt = nvm_trans_table_get(&rg->nvtt_list); // list head
        nvm_trans_table *tt = htt; // loop variable
        uint32_t xcount = 0; // count of existing transaction tables
        while (tt)
        {
            nvm_verify(tt, shapeof(nvm_trans_table));
            xcount++;
            tt = nvm_trans_table_get(&tt->link);
        }

        nvm_trans_table *att = 0; // list of trans tables to append
        nvm_trans_table *ett = 0; // current end of list

        /* If there are not enough trans tables allocate new ones to add.
         * Note that there is no attempt to release excess trans tables. It
         * is too difficult to deal with the possibility that some of the
         * undo blocks to delete are in use. */
        if (xcount < tbls)
        {
            /* Allocate the new trans tables, initialize them, and link to the
             * append list for later linking to the existing trans tables. Link
             * to the end of the list to keep the slot numbers in order. */
            uint16_t slots = xcount*TRANS_TABLE_SLOTS;
            uint32_t n;
            for (n = xcount; n < tbls; n++, slots += TRANS_TABLE_SLOTS)
            {
                /* Allocate another trans table */
                nvm_trans_table *ntt = nvm_alloc(nvm_heap_get(&rg->rootHeap),
                        shapeof(nvm_trans_table), 1);
                if (ntt == 0)
                {
                    errno = ENOSPC;
                    nvm_abort();
                    nvm_txend();
                    return 0;
                }

                /* Initialize the new transaction slots. Undo not needed since
                 * they were just allocated. The state is initialized to
                 * reserved or idle based on the new maximum concurrent
                 * transactions. */
                int s;
                nvm_transaction *tx = &ntt->transactions[0];
                for (s = 0; s < TRANS_TABLE_SLOTS; s++, tx++)
                {
                    tx->slot = slots + s + 1;
                    tx->state = tx->slot > cfg->txn_slots ?
                            nvm_reserved_state : nvm_idle_state;
                    nvm_flush1(&tx->slot);
                }

                /* link new trans table to append list. */
                if (att)
                {
                    /* add at end of existing list. */
                    nvm_trans_table_set(&ett->link, ntt);
                    ett = ntt;
                    nvm_flush1(&ett->link);
                }
                else
                {
                    /* First allocation so just init list pointers. */
                    ett = att = ntt;
                }
            }
        }

        /* Create an on unlock operation to update the transaction parameters
         * in the nvm_region. This cannot be done with normal undo because
         * recovery scans the transaction tables for transactions to recover
         * and constructs the freelists before any undo is applied. Thus we
         * cannot use undo to make the data structures consistent before they
         * are used. We must ensure the new allocations are committed before
         * we link them to the list of transaction tables, and we must ensure
         * that they are linked to the list if the allocation is committed.
         * This must be done with careful ordering rather than with undo.
         * This is done in an on unlock operation to ensure the updates are
         * made while the region lock is still held and the transaction is
         * guaranteed committed. If we die while initializing the context,
         * then the transaction does not commit and the context is not used. */
        nvm_txconfig_ctx *ctx =
                nvm_onunlock(nvm_extern_nvm_txconfig_callback.usid);
        nvm_trans_table_set(&ctx->tt_append, att);
        ctx->txn_slots = cfg->txn_slots;
        ctx->undo_limit = cfg->undo_limit;
        nvm_flush(ctx, sizeof(*ctx));

        nvm_txend();
    }
    return 1;
}
#endif //NVM_EXT

/**
 * Create the initial nvm_trans_table at region creation time.
 *
 * @param[in] rg
 * The nvm_region to store the table in.
 *
 * @param[in] rh
 * The root heap to do a non-transactional allocation of the table
 *
 * @return
 * The initial nvm_trans_table in NVM. Never returns error.
 */
#ifdef NVM_EXT
void nvm_create_trans_table(
        nvm_region ^rg,
        nvm_heap ^rh
        )
{
    /* Do a non-transactional allocate of the initial table. Note that the
     * default initialization of all pointers to NULL, data to zero, and
     * correct USID values is adequate for nvm_trans_table, except for the
     * slot numbers and state. */
    nvm_trans_table ^tt = nvm_allocNT(rh, shapeof(nvm_trans_table));
    if (tt == 0)
        nvms_assert_fail("Insufficent space to create initial trans table");
    int s;
    for (s = 0; s < TRANS_TABLE_SLOTS; s++)
    {
        tt=>transactions[s].slot ~= s + 1;
        tt=>transactions[s].state ~= s <= TRANS_TABLE_SLOTS / 2 ?
                nvm_idle_state : nvm_reserved_state;
    }

    /* Update nvm_region to describe this transaction table. This configuration
     * ensures a transaction never waits for undo because there is enough
     * undo for all transactions to hit maximum. */
    rg=>nvtt_list ~= tt;
    rg=>max_transactions ~= TRANS_TABLE_SLOTS / 2;
    rg=>max_undo_blocks ~= TRANS_TABLE_UNDO_BLKS / (TRANS_TABLE_SLOTS / 2);
}
#else
void nvm_create_trans_table(
        nvm_region *rg,
        nvm_heap * rh
        )
{
    /* Do a non-transactional allocate of the initial table. Note that the
     * default initialization of all pointers to NULL, data to zero, and
     * correct USID values is adequate for nvm_trans_table, except for the
     * slot numbers and state. */
    nvm_trans_table *tt = nvm_allocNT(rh, shapeof(nvm_trans_table));
    if (tt == NULL)
        nvms_assert_fail("Insufficent space to create initial trans table");
    int s;
    for (s = 0; s < TRANS_TABLE_SLOTS; s++)
    {
        tt->transactions[s].slot = s + 1;
        tt->transactions[s].state = s <= TRANS_TABLE_SLOTS / 2 ?
                nvm_idle_state : nvm_reserved_state;
    }

    /* Update nvm_region to describe this transaction table. This configuration
     * ensures a transaction never waits for undo because there is enough
     * undo for all transactions to hit maximum. */
    nvm_trans_table_set(&rg->nvtt_list, tt);
    //    rg->nvtt_cnt = 1; // only one table initially
    rg->max_transactions = TRANS_TABLE_SLOTS / 2;
    rg->max_undo_blocks = TRANS_TABLE_UNDO_BLKS / (TRANS_TABLE_SLOTS / 2);
}
#endif //NVM_EXT

/**
 * This spawns recovery threads to take over dead transactions and end them.
 * It scans the dead transaction list in the transaction table and spawns
 * a thread to recover any that have not yet had a recovery thread spawned,
 * or had a thread spawned that apparently died before taking over the
 * transaction.
 *
 * The caller must hold the trans_mutex for the transaction table data. It
 * is still held on return.
 *
 * @param ttd
 * Pointer to the transaction table to scan.
 */
#ifdef NVM_EXT
void nvm_recover_dead(nvm_trans_table_data * ttd)
{
    /* nothing to do if there are no dead transactions. */
    nvm_transaction ^tx = ttd->dead_trans;
    if (tx == 0)
        return; // nothing to do

    /* Loop through the list of dead transactions. The caller acquired the
     * transaction mutex so that the list will not change while we scan. We
     * keep the low bits of time in seconds so that we can decide when failure
     * to dequeue a dead transaction implies the spawned thread died. */
    uint16_t now = (uint16_t)(nvms_utime() / 1000000);
    nvm_transaction ^link = NULL;
    for (; tx; tx = link)
    {
        /* If a thread was already spawned see if it has been long enough to
         * expect the thread died before removing the transaction from the
         * dead list. */
        if (tx=>spawn_cnt)
        {

            /* Decide how many seconds it has been since the thread was
             * spawned. This works for intervals less than 9 hours even though
             * we only use the low 16 bits of the time stamp. Two's complement
             * arithmetic is the same for signed and unsigned values. */
            uint16_t delta = now - tx=>spawn_time;

            /* Decide if it has been long enough to be concerned. We use
             * exponential back off starting with one second and doubling the
             * time with each attempt to do the recovery. */
            uint16_t limit = 1 << (tx=>spawn_cnt - 1);
            if (delta <= limit)
                continue;

            /* It seems that we need to do another attempt at recovery. There
             * is no harm in starting another thread since only one thread
             * will remove the transaction from the dead list and recover it.
             * However if we have spawned 6 threads over one minute with no
             * success in simply unlinking the transaction then something is
             * seriously wrong. In this case kill the application. */
            if (tx=>spawn_cnt >= 6)
                nvms_assert_fail("Unable to spawn recovery threads");
        }

        /* In single thread mode spawning the recovery thread actually does
         * the recovery, and there is no mutex to stabilize this linked list.
         * We solve this by capturing the link before recovery removes the
         * transaction. */
        link = tx=>link;

        /* Spawn a thread to recover this transaction. Update transient data
         * in the transaction to record this spawning. */
        tx=>spawn_cnt++;
        tx=>spawn_time = now;
        nvms_spawn_txrecover((void*)tx);
    }
}
#else
void nvm_recover_dead(nvm_trans_table_data * ttd)
{
    /* nothing to do if there are no dead transactions. */
    nvm_transaction *tx = ttd->dead_trans;
    if (tx == NULL)
        return; // nothing to do

    /* Loop through the list of dead transactions. The caller acquired the
     * transaction mutex so that the list will not change while we scan. We
     * keep the low bits of time in seconds so that we can decide when failure
     * to dequeue a dead transaction implies the spawned thread died. */
    uint16_t now = (uint16_t)(nvms_utime() / 1000000);
    nvm_transaction *link = NULL;
    for (; tx; tx = link)
    {
        /* If a thread was already spawned see if it has been long enough to
         * expect the thread died before removing the transaction from the
         * dead list. */
        if (tx->spawn_cnt)
        {

            /* Decide how many seconds it has been since the thread was
             * spawned. This works for intervals less than 9 hours even though
             * we only use the low 16 bits of the time stamp. Two's complement
             * arithmetic is the same for signed and unsigned values. */
            uint16_t delta = now - tx->spawn_time;

            /* Decide if it has been long enough to be concerned. We use
             * exponential back off starting with one second and doubling the
             * time with each attempt to do the recovery. */
            uint16_t limit = 1 << (tx->spawn_cnt - 1);
            if (delta <= limit)
                continue;

            /* It seems that we need to do another attempt at recovery. There
             * is no harm in starting another thread since only one thread
             * will remove the transaction from the dead list and recover it.
             * However if we have spawned 6 threads over one minute with no
             * success in simply unlinking the transaction then something is
             * seriously wrong. In this case kill the application. */
            if (tx->spawn_cnt >= 6)
                nvms_assert_fail("Unable to spawn recovery threads");
        }

        /* In single thread mode spawning the recovery thread actually does
         * the recovery, and there is no mutex to stabilize this linked list.
         * We solve this by capturing the link before recovery removes the
         * transaction. */
        link = tx->link;

        /* Spawn a thread to recover this transaction. Update transient data
         * in the transaction to record this spawning. */
        tx->spawn_cnt++;
        tx->spawn_time = now;
        nvms_spawn_txrecover(tx);
    }
}
#endif //NVM_EXT

/**
 * This function recovers one base transaction and all its nested
 * transactions. Each transaction is either aborted or its commit is
 * completed, depending on its state. It is called in a new thread
 * created by nvms_spawn_txrecovery. When it returns the thread should
 * exit gracefully.
 *
 * @param ctx
 * This is the context pointer passed to nvms_spawn_txrecover. It is an
 * application global address.
 */
#ifdef NVM_EXT
void nvm_txrecover(void *ctx)
{
    /* Initialize this thread to use the nvm library. */
    nvm_thread_init();

    /* get the pointers we need to find the transaction in its dead list. */
    nvm_app_data *ad = nvm_get_app_data();
    nvm_thread_data *td = nvm_get_thread_data();
    nvm_transaction ^tx = (void^)ctx; // the transaction we are recovering
    nvm_trans_table_data *ttd = ad->regions[tx=>desc]->trans_table;

    /* Lock the dead list so we can scan it */
    nvms_lock_mutex(ttd->trans_mutex, 1);
    nvm_transaction ^ltx = 0; // last transaction seen in dead list
    nvm_transaction ^ntx = ttd->dead_trans; // next transaction in dead list
    while (ntx != tx)
    {
        /* Nothing to do if transaction is not in the dead list. Some other
         * thread must have claimed it for recovery. */
        if (ntx == 0)
        {
            nvms_unlock_mutex(ttd->trans_mutex);
            nvm_thread_fini();
            return;
        }
        ltx = ntx;
        ntx = ntx=>link;
    }

    if (ltx != 0)
        ltx=>link = tx=>link;
    else
        ttd->dead_trans = tx=>link;
    tx=>link = 0;
    nvms_unlock_mutex(ttd->trans_mutex);

    /* Fix the transaction transient data to be current. */
    nvm_count_undo(tx);
    nvm_prep_undo(tx);
    tx=>spawn_cnt = 0;

    /* Fix our thread private data to look like we created this transaction. */
    td->transaction = tx;
    td->region = ttd->region;
    td->txdepth = 1;
    nvm_nested ^nt = tx=>nstd_last;
    while (nt != 0)
    {
        td->txdepth++;
        nt = nt=>prev;
    }

    uint32_t txnum = tx=>txnum;
    printf("Recovering transaction %d with %d undo records\n",
            txnum, tx=>undo_ops);

    /* Keep ending transactions until they are all gone. */
    while (td->txdepth != 0)
    { extern @ {
        switch (nvm_get_state(tx))
        {
        default:
            nvms_corruption("Impossible tx state in tx recovery", tx, ttd);
            break; // unreachable

        case nvm_active_state:
        case nvm_rollback_state:
        case nvm_aborting_state:
            /* Never got committed so abort */
            nvm_abort();
            nvm_txend();
            break;

        case nvm_committing_state:
            /* In the middle of commit so complete commit */
            nvm_commit();
            nvm_txend();
            break;

        case nvm_aborted_state:
        case nvm_committed_state:
            /* Transaction is done, but not yet ended */
            nvm_txend();
            break;
        }
    }}

    /* Transaction is now recovered and might already be in use by another
     * thread. We have done our job so exit this thread. */
    nvm_thread_fini();
    //    printf("Transaction %d recovered\n", txnum);
}
#else
void nvm_txrecover(void *ctx)
{
    /* Initialize this thread to use the nvm library. */
    nvm_thread_init();

    /* get the pointers we need to find the transaction in its dead list. */
    nvm_app_data *ad = nvm_get_app_data();
    nvm_thread_data *td = nvm_get_thread_data();
    nvm_transaction *tx = ctx; // the transaction we are recovering
    nvm_verify(tx, shapeof(nvm_transaction));
    nvm_trans_table_data *ttd = ad->regions[tx->desc]->trans_table;

    /* Lock the dead list so we can scan it */
    nvms_lock_mutex(ttd->trans_mutex, 1);
    nvm_transaction *ltx = NULL; // last transaction seen in dead list
    nvm_transaction *ntx = ttd->dead_trans; // next transaction in dead list
    while (ntx != tx)
    {
        /* Nothing to do if transaction is not in the dead list. Some other
         * thread must have claimed it for recovery. */
        if (ntx == NULL)
        {
            nvms_unlock_mutex(ttd->trans_mutex);
            nvm_thread_fini();
            return;
        }
        nvm_verify(ntx, shapeof(nvm_transaction));
        ltx = ntx;
        ntx = ntx->link;
    }

    if (ltx != NULL)
        ltx->link = tx->link;
    else
        ttd->dead_trans = tx->link;
    tx->link = 0;
    nvms_unlock_mutex(ttd->trans_mutex);

    /* Fix the transaction transient data to be current. */
    nvm_count_undo(tx);
    nvm_prep_undo(tx);
    tx->spawn_cnt = 0;

    /* Fix our thread private data to look like we created this transaction. */
    td->transaction = tx;
    td->region = ttd->region;
    td->txdepth = 1;
    nvm_nested *nt = nvm_nested_get(&tx->nstd_last);
    while (nt != NULL)
    {
        td->txdepth++;
        nt = nvm_nested_get(&nt->prev);
    }

    uint32_t txnum = tx->txnum;
    printf("Recovering transaction %d with %d undo records\n",
            txnum, tx->undo_ops);

    /* Keep ending transactions until they are all gone. */
    while (td->txdepth != 0)
    {
        switch (nvm_get_state(tx))
        {
        default:
            nvms_corruption("Impossible tx state in tx recovery", tx, ttd);
            break; // unreachable

        case nvm_active_state:
        case nvm_rollback_state:
        case nvm_aborting_state:
            /* Never got committed so abort */
            nvm_abort();
            nvm_txend();
            break;

        case nvm_committing_state:
            /* In the middle of commit so complete commit */
            nvm_commit();
            nvm_txend();
            break;

        case nvm_aborted_state:
        case nvm_committed_state:
            /* Transaction is done, but not yet ended */
            nvm_txend();
            break;
        }
    }

    /* Transaction is now recovered and might already be in use by another
     * thread. We have done our job so exit this thread. */
    nvm_thread_fini();
}
#endif //NVM_EXT

/**
 * Ensure the NVM transaction system for this region is ready for use. This
 * may be called multiple times by multiple threads. If initialization is
 * already done then this will return quickly. It is possible that the
 * thread doing recovery is not the same thread that did the attach. It may
 * have died or lost the race to lock the region.
 *
 * Construct the nvm_trans_table_data in volatile memory. If there are any
 * active or committing transactions they will be recovered. The volatile
 * transaction table data is allocated from application global memory.
 *
 * This must be called with the application data mutex held to ensure the
 * region is not detached while looking at the region. However the mutex
 * is unlocked and relocked if recovery is actually spawned.
 *
 * @param[in] desc
 * This is the region descriptor for the region to recover.
 */
#ifdef NVM_EXT
void nvm_recover(nvm_desc desc)
{
    /* Get the nvm_region for this region. */
    nvm_app_data *ad = nvm_get_app_data(); //get application data
    nvm_region_data *rd = ad->regions[desc]; // get region data
    nvm_region ^rg = rd->region;

    /* lock the mutex protecting the region data and check to see if the
     * region is already recovered. */
    nvms_lock_mutex(rd->mutex, 1);
    if (rd->trans_table != NULL)
    {
        /* recovery already done */
        nvms_unlock_mutex(rd->mutex);
        return;
    }

    /* Allocate a nvm_trans_table_data for this region. */
    nvm_trans_table_data *ttd = nvms_app_alloc(sizeof(nvm_trans_table_data));
    ttd->region = rg;
    ttd->trans_mutex = nvms_create_mutex();
    ttd->trans_cond = nvms_create_cond();
    ttd->undo_mutex = nvms_create_mutex();
    ttd->undo_cond = nvms_create_cond();

    /* Loop through all the nvm_trans_table structs reconstructing the
     * volatile data in each nvm_transaction in NVM. Idle transactions
     * are added to the free transaction list and active/committing
     * transactions are added to the dead transaction list. This may require
     * examining multiple transaction tables. */
    nvm_trans_table ^tt; // non-volatile transaction table loop variable
    for (tt = rg=>nvtt_list; tt; tt = tt=>link)
    {
        /* Look through all the transactions reinitializing them and linking
         * them to the correct nvm_trans_table_data list. This also unlinks
         * any empty undo blocks so that all empty blocks can be put on the
         * freelist of undo blocks. */
        int t;
        for (t = 0; t < TRANS_TABLE_SLOTS; t++)
        {
            /* get and verify the next transaction */
            nvm_transaction ^tx = %tt=>transactions[t];

            /* If there are empty undo blocks associated with this transaction,
             * unlink them so they can be added to the free list. */
            nvm_undo_blk ^ub = tx=>undo;
            while (ub && ub=>count == 0)
            {
                ub = ub=>link;
                tx=>undo ~= ub;
            }

            /* find the highest txnum */
            if (ttd->txnum < tx=>txnum)
                ttd->txnum = tx=>txnum;

            /* set the correct descriptor */
            tx=>desc = desc;

            /* count the total transactions that exist*/
            ttd->ttrans_cnt++;

            /* Recovery at attach does not recover parent transactions in
             * other regions since they might not be attached yet. Thus we
             * clear the parent pointer.
             */
            tx=>parent = 0;

            /* add to correct list*/
            switch (tx=>state)
            {
            case nvm_committed_state:
            case nvm_aborted_state:
            case nvm_idle_state:
                /* ensure state is idle */
                tx=>state ~= nvm_idle_state;

                /* Cleanup transient data */
                tx=>undo_ops = 0;
                tx=>undo_bytes = 0;
                tx=>undo_data = 0;
                tx=>link = 0;

                /* Add to the freelist of transactions. */
                tx=>link = ttd->free_trans;
                ttd->free_trans = tx;
                tx=>dead = 0; // not on dead list
                break;

            case nvm_active_state:
            case nvm_committing_state:
            case nvm_rollback_state:
            case nvm_aborting_state:
                /* Add to the list of transactions needing recovery */
                tx=>link = ttd->dead_trans;
                ttd->dead_trans = tx; // he's dead Jim
                tx=>spawn_cnt = 0; // ignore previous attempts to spawn
                tx=>dead = 1;
                ttd->dtrans_cnt++; // on the dead list
                break;

            case nvm_reserved_state:
                /* This transaction should not be used so simply forget it. */
                ttd->rtrans_cnt++;
                tx=>dead = 0; // not on dead list
                break;

            default:
                nvms_assert_fail("Impossible transaction state to recover");
                break;
            }
        }
    } // end of for each nvm_trans_table

    /* All the empty undo blocks have been removed from the transactions.
     * We can now put them all on the freelist. Those that are not empty
     * must be linked to a transaction that needs recovery. */
    for (tt = rg=>nvtt_list; tt; tt = tt=>link)
    {
        /* Loop through all the undo blocks making the empty ones free. */
        int u;
        for (u = 0; u < TRANS_TABLE_UNDO_BLKS; u++)
        {
            nvm_undo_blk ^ub = %tt=>undo_blks[u];

            /* Skip the undo blocks that have undo to apply. They will be
             * released after recovery. */
            if (ub=>count != 0)
                continue;

            /* add to the list of free undo blocks */
            ub=>link ~= ttd->free_undo;
            ttd->free_undo = ub;

        }
    }

    /* Find any nvm_lkrec undo records that might have been changing when the
     * last detach happened. Pass them to nvm_recover_lock to do any needed
     * repairs. */
    nvm_transaction ^tx = ttd->dead_trans;
    while (tx)
    {
        /* Call lock recovery for every lock owned by the transaction. You
         * might think that only the last nvm_lkrec constructed could be in
         * flux. However commit does not unlink locks as they are unlocked,
         * so locks in the middle of the list could need recovery. */
        nvm_lkrec ^lk = tx=>held_locks;
        while (lk)
        {
            if (lk=>state != nvm_lock_callback)
                nvm_recover_lock(ttd, lk);
            lk = lk=>prev;
        }
        tx = tx=>link;
    }

    /* Make sure all the changes are persistent. */
    nvm_persist();

    /* Recovery started so we can put the nvm_trans_table_data in the
     * region data for transactions to use. */
    rd->trans_table = ttd;
    nvms_unlock_mutex(rd->mutex);

    /* Lock the transaction table while we still have the application data
     * mutex locked. While there are dead transactions the region cannot
     * be detached because nvm_detach_trans will block. Thus we can
     * drop the app data lock so that transaction recovery can run. If
     * no recovery is needed we can just return with the app data still
     * locked. This is important for region create to ensure the region
     * is not visible until construction is complete. */
    nvms_lock_mutex(ttd->trans_mutex, 0);
    if (ttd->dtrans_cnt == 0)
    {
        nvms_unlock_mutex(ttd->trans_mutex);
        return;
    }
    ttd->recovering = 1;
    nvms_unlock_mutex(ad->mutex);

    /* For every transaction that needs recovery we spawn a recovery thread
     * to either abort the transaction or complete committing it. Once the
     * thread is running it will remove its transaction from the dead list. */
    nvm_recover_dead(ttd);

    /* Wait until all recovery is done. This returns the region to a state
     * that the application expects. It might seem OK to allow the
     * application to start using the region immediately, however
     * experience shows this is unwise. An application might avoid locking
     * by ensuring only one thread touches a data structure. However this
     * does not work if there is a recovery thread touching it. */
    while (ttd->dtrans_cnt != 0)
        nvms_cond_wait(ttd->trans_cond, ttd->trans_mutex, (uint64_t)0);

    /* no longer in recovery so detach is allowed */
    ttd->recovering = 0;
    nvms_unlock_mutex(ttd->trans_mutex);
    printf("Recovery complete\n");

    /* Our caller expects us to return with the app data mutex locked */
    nvms_lock_mutex(ad->mutex, 1);
}
#else
void nvm_recover(nvm_desc desc)
{
    /* Get the nvm_region for this region. */
    nvm_app_data *ad = nvm_get_app_data(); //get application data
    nvm_region_data *rd = ad->regions[desc]; // get region data
    nvm_region *rg = rd->region;

    /* lock the mutex protecting the region data and check to see if the
     * region is already recovered. */
    nvms_lock_mutex(rd->mutex, 1);
    if (rd->trans_table != NULL)
    {
        /* recovery already done */
        nvms_unlock_mutex(rd->mutex);
        return;
    }

    /* Allocate a nvm_trans_table_data for this region. */
    nvm_trans_table_data *ttd = nvms_app_alloc(sizeof(nvm_trans_table_data));
    ttd->region = rg;
    ttd->trans_mutex = nvms_create_mutex();
    ttd->trans_cond = nvms_create_cond();
    ttd->undo_mutex = nvms_create_mutex();
    ttd->undo_cond = nvms_create_cond();

    /* Loop through all the nvm_trans_table structs reconstructing the
     * volatile data in each nvm_transaction in NVM. Idle transactions
     * are added to the free transaction list and active/committing
     * transactions are added to the dead transaction list. This may require
     * examining multiple transaction tables. */
    nvm_trans_table *tt; // non-volatile transaction table loop variable
    //    uint32_t ublks = 0; // count of undo blocks needing recovery
    for (tt = nvm_trans_table_get(&rg->nvtt_list);
            tt;
            tt = nvm_trans_table_get(&tt->link))
    {
        /* verify the nvm_trans_table */
        nvm_verify(tt, shapeof(nvm_trans_table));

        /* Look through all the transactions reinitializing them and linking
         * them to the correct nvm_trans_table_data list. This also unlinks
         * any empty undo blocks so that all empty blocks can be put on the
         * freelist of undo blocks. */
        int t;
        for (t = 0; t < TRANS_TABLE_SLOTS; t++)
        {
            /* get and verify the next transaction */
            nvm_transaction *tx = &tt->transactions[t];
            nvm_verify(tx, shapeof(nvm_transaction));

            /* If there are empty undo blocks associated with this transaction,
             * unlink them so they can be added to the free list. */
            nvm_undo_blk *ub = nvm_undo_blk_get(&tx->undo);
            while (ub && ub->count == 0)
            {
                nvm_verify(ub, shapeof(nvm_undo_blk));
                ub = nvm_undo_blk_get(&ub->link);
                nvm_undo_blk_set(&tx->undo, ub);
            }

            /* find the highest txnum */
            if (ttd->txnum < tx->txnum)
                ttd->txnum = tx->txnum;

            /* set the correct descriptor */
            tx->desc = desc;

            /* count the total transactions that exist*/
            ttd->ttrans_cnt++;

            /* Recovery at attach does not recover parent transactions in
             * other regions since they might not be attached yet. Thus we
             * clear the parent pointer.
             */
            tx->parent = 0;

            /* add to correct list*/
            switch (tx->state)
            {
            case nvm_committed_state:
            case nvm_aborted_state:
            case nvm_idle_state:
                /* ensure state is idle */
                tx->state = nvm_idle_state;

                /* Cleanup transient data */
                tx->undo_ops = 0;
                tx->undo_bytes = 0;
                tx->undo_data = NULL;
                tx->link = NULL;

                /* Add to the freelist of transactions. */
                tx->link = ttd->free_trans;
                ttd->free_trans = tx;
                tx->dead = 0; // not on dead list
                break;

            case nvm_active_state:
            case nvm_committing_state:
            case nvm_rollback_state:
            case nvm_aborting_state:
                /* Add to the list of transactions needing recovery */
                tx->link = ttd->dead_trans;
                ttd->dead_trans = tx; // he's dead Jim
                tx->spawn_cnt = 0; // ignore previous attempts to spawn
                tx->dead = 1;
                ttd->dtrans_cnt++; // on the dead list
                break;

            case nvm_reserved_state:
                /* This transaction should not be used so simply forget it. */
                ttd->rtrans_cnt++;
                tx->dead = 0; // not on dead list
                break;

            default:
                nvms_assert_fail("Impossible transaction state to recover");
                break;
            }
        }
    } // end of for each nvm_trans_table

    /* All the empty undo blocks have been removed from the transactions.
     * We can now put them all on the freelist. Those that are not empty
     * must be linked to a transaction that needs recovery. */
    for (tt = nvm_trans_table_get(&rg->nvtt_list);
            tt;
            tt = nvm_trans_table_get(&tt->link))
    {
        /* verify the nvm_trans_table */
        nvm_verify(tt, shapeof(nvm_trans_table));

        /* Loop through all the undo blocks making the empty ones free. */
        int u;
        for (u = 0; u < TRANS_TABLE_UNDO_BLKS; u++)
        {
            nvm_undo_blk *ub = &tt->undo_blks[u];
            nvm_verify(ub, shapeof(nvm_undo_blk));

            /* Skip the undo blocks that have undo to apply. They will be
             * released after recovery. */
            if (ub->count != 0)
                continue;

            /* add to the list of free undo blocks */
            nvm_undo_blk_set(&ub->link, ttd->free_undo);
            ttd->free_undo = ub;
        }
    }

    /* Find any nvm_lkrec undo records that might have been changing when the
     * last detach happened. Pass them to nvm_recover_lock to do any needed
     * repairs. */
    nvm_transaction *tx = ttd->dead_trans;
    while (tx)
    {
        /* Call lock recovery for every lock owned by the transaction. You
         * might think that only the last nvm_lkrec constructed could be in
         * flux. However commit does not unlink locks as they are unlocked,
         * so locks in the middle of the list could need recovery. */
        nvm_lkrec *lk = nvm_lkrec_get(&tx->held_locks);
        while (lk)
        {
            if (lk->state != nvm_lock_callback)
                nvm_recover_lock(ttd, lk);
            lk = nvm_lkrec_get(&lk->prev);
        }
        tx = tx->link;
    }

    /* Make sure all the changes are persistent. */
    nvm_persist();

    /* Recovery started so we can put the nvm_trans_table_data in the
     * region data for transactions to use. */
    rd->trans_table = ttd;
    nvms_unlock_mutex(rd->mutex);

    /* Lock the transaction table while we still have the application data
     * mutex locked. While there are dead transactions the region cannot
     * be detached because nvm_detach_trans will block. Thus we can
     * drop the app data lock so that transaction recovery can run. If
     * no recovery is needed we can just return with the app data still
     * locked. This is important for region create to ensure the region
     * is not visible until construction is complete. */
    nvms_lock_mutex(ttd->trans_mutex, 0);
    if (ttd->dtrans_cnt == 0)
    {
        nvms_unlock_mutex(ttd->trans_mutex);
        return;
    }
    ttd->recovering = 1;
    nvms_unlock_mutex(ad->mutex);

    /* For every transaction that needs recovery we spawn a recovery thread
     * to either abort the transaction or complete committing it. Once the
     * thread is running it will remove its transaction from the dead list. */
    nvm_recover_dead(ttd);

    /* Wait until all recovery is done. This returns the region to a state
     * that the application expects. It might seem OK to allow the
     * application to start using the region immediately, however
     * experience shows this is unwise. An application might avoid locking
     * by ensuring only one thread touches a data structure. However this
     * does not work if there is a recovery thread touching it. */
    while (ttd->dtrans_cnt != 0)
        nvms_cond_wait(ttd->trans_cond, ttd->trans_mutex, (uint64_t)0);

    /* no longer in recovery so detach is allowed */
    ttd->recovering = 0;
    nvms_unlock_mutex(ttd->trans_mutex);
    printf("Recovery complete\n");

    /* Our caller expects us to return with the app data mutex locked */
    nvms_lock_mutex(ad->mutex, 1);
}
#endif //NVM_EXT

/**
 * This counts the number of share locks on an NVM mutex before beginning
 * recovery of the dead transactions at NVM region attach time.
 *
 * @param[in] ttd
 * The volatile memory transaction table data. It has the linked list of dead
 * transactions to scan.
 *
 * @param[in] mutex
 * The mutex to search for.
 *
 * @return
 */
#ifdef NVM_EXT
int nvm_find_shares(nvm_trans_table_data *ttd, nvm_amutex ^mutex)
{
    int cnt = 0;
    nvm_transaction ^tx = ttd->dead_trans;
    while (tx)
    {
        nvm_lkrec ^lk = tx=>held_locks;
        while (lk)
        {
            if (lk=>state == nvm_lock_held_s && lk=>mutex == mutex)
                cnt++;
            lk = lk=>prev;
        }
        tx = tx=>link;
    }
    return cnt;
}
#else
int nvm_find_shares(nvm_trans_table_data *ttd, nvm_amutex *mutex)
{
    int cnt = 0;
    nvm_transaction *tx = ttd->dead_trans;
    while (tx)
    {
        nvm_lkrec *lk = nvm_lkrec_get(&tx->held_locks);
        while (lk)
        {
            if (lk->state == nvm_lock_held_s &&
                nvm_amutex_get(&lk->mutex) == mutex)
                cnt++;
            lk = nvm_lkrec_get(&lk->prev);
        }
        tx = tx->link;
    }
    return cnt;
}
#endif //NVM_EXT

/**
 * This checks to see if the region is ready for a clean detach. It must
 * not have any active transactions. Once this is verified, future
 * transactions are prevented from starting. If there are dead transactions
 * in recovery this will block until the recovery is done.
 *
 * This is called and returns with the application data mutex locked
 *
 * @param[in] rd
 * The region data for the region to detach
 *
 * @return
 * 1 if no errors, 0 if failed and errno has an error.
 */
int nvm_trans_detach(nvm_region_data * rd)
{
    nvm_trans_table_data *ttd = rd->trans_table;
    nvms_lock_mutex(ttd->trans_mutex, 1);

    /* wait for any recovery to finish */
    while (ttd->dtrans_cnt != 0 || ttd->recovering)
        nvms_cond_wait(ttd->trans_cond, ttd->trans_mutex, (uint64_t)0);

    /* Error if transactions are active. */
    int ret = ttd->atrans_cnt == 0;
    if (ret)
        ttd->detaching = 1;
    else
        errno = EBUSY;
    nvms_unlock_mutex(ttd->trans_mutex);
    return ret;
}

/**
 * This frees up a nvm_trans_table_data allocated by nvm_recover.
 * @param trans_table points to trans_table pointer in region data
 * @return
 */
int nvm_trans_free_table(nvm_trans_table_data **trans_table)
{
    nvm_trans_table_data *ttd = *trans_table;
    if (!ttd)
        return 1;
    if (ttd->trans_mutex)
    {
        nvms_destroy_mutex(ttd->trans_mutex);
    }
    if (ttd->trans_cond)
    {
        nvms_destroy_cond(ttd->trans_cond);
    }
    if (ttd->undo_mutex)
    {
        nvms_destroy_mutex(ttd->undo_mutex);
    }
    if (ttd->undo_cond)
    {
        nvms_destroy_cond(ttd->undo_cond);
    }
    nvms_app_free(ttd);
    *trans_table = 0;

    return 1;
}

