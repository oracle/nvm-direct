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
      nvm_transaction.h - Non-Volatile Memory atomic transactions

    DESCRIPTION\n
      This header defines the application interface for NVM transactions that
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

#ifndef NVM_TRANSACTION_H
#define	NVM_TRANSACTION_H

#ifndef NVM_USID_H
#include "nvm_usid.h"
#endif

#ifndef NVM_REGION_H
#include "nvm_region.h"
#endif

#ifdef	__cplusplus
extern "C"
{
#endif


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
     * @param[in] desc
     * This is the region descriptor returned by nvm_create_region or
     * nvm_attach_region for the region to modify in the transaction.
     */
#ifdef NVM_EXT
    void nvm_txbegin(
        nvm_desc region
        );
#else
    void nvm_txbegin(
        nvm_desc region
        );
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
     * If the current transaction is not committed or aborted then it is
     * committed.
     */
#ifdef NVM_EXT
    void nvm_txend@();
#else
    void nvm_txend();    
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
    int nvm_txdepth();

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
        );
#else
    int nvm_txstatus(
        int parent
        );
#endif //NVM_EXT
    /** 
     * There is no such transaction. The value of parent is 
     * greater than or equal to the current transaction depth.
     */
#define NVM_TX_NONE       0 

    /**
     * The transaction is active. Its fate is not yet 
     * resolved.
     */
#define NVM_TX_ACTIVE     1 

    /**  
     * The transaction is still active but is currently being rolled back to 
     * a savepoint. Once the rollback completes the transaction will return
     *  to status active.
     */
#define NVM_TX_ROLLBACK   2 

    /** 
     * The transaction is in the middle of aborting. Undo application is not 
     * yet complete. The transaction will eventually become aborted when all 
     * undo is applied. 
     */
#define NVM_TX_ABORTING   3 

    /**
     * The transaction was successfully aborted, but execution is still within
     * the transaction code block. In other words nvm_txend() has not yet 
     * been called.
     */
#define NVM_TX_ABORTED    4 

    /**
     * The transaction is in the middle of committing. It may be releasing 
     * locks or calling oncommit operations as part of committing. The 
     * transaction will eventually become committed when all oncommit 
     * operations are complete.
     */
#define NVM_TX_COMMITTING 5 
    /**
     * The transaction was successfully committed, but execution is still 
     * within the transaction code block. In other words nvm_txend() has not 
     * yet been called.
     */
#define NVM_TX_COMMITTED  6 


    /**
     * This returns the region descriptor for the current transaction. It is
     * useful for on abort or on commit callbacks to know which region they
     * are in. It must be called from within a transaction.
     * 
     * @return region descriptor of current transaction.
     */
#ifdef NVM_EXT
    nvm_desc nvm_txdesc@();
#else
    nvm_desc nvm_txdesc();
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
    uint16_t nvm_txslot@();
    #else
    uint16_t nvm_txslot();
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
        size_t bytes
        );
#else
    void nvm_undo(   
        const void *data,    
        size_t bytes
        );
#endif //NVM_EXT

    /**
     * This macro takes an lval for an assignment statement and creates undo
     * for it. This is a convenient way of generating undo before an assignment
     * when not using the NVM C language extensions.
     */
#ifdef NVM_EXT
#define NVM_UNDO(lval) do { nvm_undo(%(lval), sizeof(lval)); } while (0)
#else
#define NVM_UNDO(lval) do { nvm_undo(&(lval), sizeof(lval)); } while (0)
#endif //NVM_EXT

    /**
     * This creates an undo record in the current transaction. The record 
     * contains an instance of the persistent struct described by the nvm_type.
     * A pointer to the argument struct is returned so that the caller can 
     * configure the arguments. Initially the struct will be zero except for 
     * the USID field. 
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
        );
#else
    void *nvm_onabort(   
        nvm_usid func    
        );
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
        );
#else
    void *nvm_oncommit(   
        nvm_usid func    
        );
#endif //NVM_EXT
    
    /**
     * This creates an undo record in the current transaction. The function
     * must be a persistent callback, meaning it has a pointer to a persistent
     * struct as its only argument. The record contains an instance of the
     * persistent struct. A pointer to the argument struct is returned so that
     * the caller can configure the arguments. The struct will be initialized
     * as if was allocated from an NVM heap. Note that if the calling thread
     * dies during this call, the callback function might be called with
     * the initialized argument.
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
     * are different than nvm_mutex locks. 
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
        );
#else
    void *nvm_onunlock(   
        nvm_usid func    
        );
#endif //NVM_EXT
    
    /**
     * This commits the current transaction. The current transaction is 
     * committed releasing all its locks and executing any oncommit operations.
     * The transaction remains the current transaction until its code block is 
     * exited. No more transactional operations are allowed in this transaction.
     * 
     * It is an error if there is no current transaction or it is already 
     * committed or aborted. If there are any errors then an assert is fired.
     */
#ifdef NVM_EXT
    void nvm_commit@();
#else
    void nvm_commit();    
#endif //NVM_EXT
    
    /**
     * This aborts the current transaction applying all undo and releasing 
     * locks as the undo is applied. The transaction remains the current 
     * transaction until its code block is exited. No more transactional 
     * operations are allowed in this transaction.
     * 
     * It is an error if there is no current transaction or it is already 
     * committed or aborted. If there are any errors then an assert is fired.
     */
#ifdef NVM_EXT
    void nvm_abort@();
#else
    void nvm_abort();    
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
        );
#else
    void_srp *nvm_savepoint(   
        void *name    
        );
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
        );
#else
    int nvm_rollback(
        void *name    
        );
#endif //NVM_EXT

    /**
     * Every region has a fixed number of transaction slots that can be used 
     * and a fixed number of undo blocks to be shared by all running 
     * transactions. To catch a runaway transaction, there is a limit to the 
     * number of undo blocks consumed by a single transaction. An assert fires 
     * if this limit is exceeded.  This struct describes the configuration for 
     * region. It is also used for changing the configuration.
     */
    struct nvm_txconfig
    {
        /**
         * This is the maximum number of simultaneous transactions in the 
         * region.
         */
        uint32_t txn_slots;

        /**
         * This is the number of 4K undo blocks available in region for all
         * transactions to share.
         */
        uint32_t undo_blocks;

        /**
         * This is the maximum number of undo blocks that a single transaction,
         * including nested transactions, can consume. Exceeding this limit 
         * will fire an assert on the presumption that the transaction is in
         * an infinite loop generating excessive undo.
         * 
         * This value must always be 4 or greater and no more than half the
         * total number of undo blocks.
         */
        uint32_t undo_limit; // max undo blocks in a single transaction
    };
    typedef struct nvm_txconfig nvm_txconfig;

    /**
     * This returns the current  transaction configuration for a region.
     * 
     * @param[in] region
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
        nvm_desc region,
        nvm_txconfig *cfg
        );
#else
    int nvm_get_txconfig(
        nvm_desc region,
        nvm_txconfig *cfg
        );
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
        nvm_desc region,
        nvm_txconfig *cfg
        );
#else
    int nvm_set_txconfig(
        nvm_desc region,
        nvm_txconfig *cfg
        );
#endif //NVM_EXT
    
#ifdef	__cplusplus
}
#endif

#endif	/* NVM_TRANSACTION_H */

