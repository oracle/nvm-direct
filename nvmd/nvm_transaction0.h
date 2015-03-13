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
      transaction0.h - private transaction management declarations

    DESCRIPTION\n
      This contains the declarations for transaction management that are 
      private to the NVM library
 
 */

#ifndef NVM_TRANSACTION0_H
#define	NVM_TRANSACTION0_H

#include "nvms_locking.h" 
#include "nvms_sleeping.h"


#ifdef	__cplusplus
extern "C"
{
#endif
    /*
     * A transaction can be in one of these states. The enum values are
     * explicitly chosen since the values are in non-volatile memory and
     * must not change when the code is recompiled.
     */
    enum nvm_trans_state
    {
        /**
         * This slot is available for another transaction. There is no 
         * undo linked to the transaction.
         */
        nvm_idle_state = 0,

        /**
         * The transaction is actively storing undo. If the system dies,
         * then recovery at next attach will roll back this transaction.
         */
        nvm_active_state = 1,

        /**
         * The transaction is still active but is currently being rolled back
         * to a savepoint. Once the rollback completes the transaction will
         * return to status active.
         */
        nvm_rollback_state = 2,

        /**
         * The transaction is in the middle of aborting. Undo application is
         * not yet complete. The transaction will eventually become aborted
         * when all undo is applied.
         */
        nvm_aborting_state = 3,

        /**
         * The transaction was successfully aborted, but execution is still
         * within the transaction code block. In other words nvm_txend() has
         * not yet been called.
         */
        nvm_aborted_state = 4,

        /**
         * The transaction is in the middle of committing. It may be releasing
         * locks or calling oncommit operations as part of committing. The
         * transaction will eventually become committed when all oncommit
         * operations are complete. If the system dies, then recovery at next
         * attach will complete the on commit operations and commit the
         * transaction.
         */
        nvm_committing_state = 5,

        /**
         * The transaction was successfully committed, but execution is still
         * within the transaction code block. In other words nvm_txend() has
         * not yet been called.
         */
        nvm_committed_state = 6,

        /**
         * This transaction slot is not available for use. The application set
         * a lower bound on the number of simultaneous transactions which is
         * less than the number of nvm_transaction structs that actually exist.
         * The extra slots are marked as reserved and are not put on the
         * freelist.
         */
        nvm_reserved_state = 7
    };
    typedef enum nvm_trans_state nvm_trans_state;

    /**
     * This struct is one slot in the transaction table. It points to all the 
     * undo in NVM for one transaction. 
     */
#ifdef NVM_EXT
    persistent struct
    USID("37ca 2aac e2f6 4639 18e6 f015 2d8c 2a6e")
    tag("One transaction slot for one transaction")
    version(0)
    alignas(16)    
    size(128)
    nvm_transaction
    {
        /**
         * The USID that maps to the nvm_type for nvm_transaction
         */
//        nvm_usid type_usid; //#

        /**
         * The transaction slot number of this transaction. This is used to
         * identify the NVM locks that this transaction owns. This is set
         * when the transaction struct is allocated, and never changed.
         */
        uint16_t slot;

        /**
         * The current state of this transaction. THis actually contains a
         * nvm_trans_state, but to keep all the non-transient data in one
         * cache line it is packed in with the slot number.
         */
        uint8_t state;

        /**
         * This is 1 when the tansaction is dead and included in the count
         * of dead transactions in the transaction table volatile data.
         */
        transient uint8_t dead;    

        /**
         * This is just for debugging. Every time a transaction is begun a
         * new transaction number is assigned to the transaction. This is 
         * stored in the undo blocks as well as here to help in debugging.
         * Note that the transaction number can wrap since it is only 4 bytes.
         */
        uint32_t txnum;

        /**
         * This is the linked list of undo blocks for this transaction.
         */
        nvm_undo_blk ^undo;

        /**
         * This is a pointer to the last on_commit operation added.
         * The on_commit operations form a linked list that can be quickly
         * scanned when the transaction is in state committing. This does not
         * need need to be atomically updated with adding the on_commit
         * operation since it is ignored if there is a failure requiring 
         * rollback. Note that cleanup at commit is done in the order added
         * which is the reverse of the order on the linked list.
         */
        nvm_on_commit ^commit_ops;

        /**
         * This is a pointer to the last NVM lock operation added to the 
         * transaction. The NVM lock operations form a linked list so that
         * all the locks held to protect update of an NVM object can be
         * released at commit. This list includes nvm_on_abort records.
         */
        nvm_lkrec ^held_locks;

        /**
         * This points to the last save point undo operation. The save 
         * point operations are linked to each other by pointers. This is
         * needed for verifying a save point name before rollback.
         */
        nvm_savepnt ^savept_last;

        /**
         * This points to the currently active nested transaction's undo
         * operation. It is null if there are none. The Nested operations
         * form a linked list. This makes it quick to apply or discard
         * the redo for a nested transaction.
         */
        nvm_nested ^nstd_last;
        
        /**
         * This is the maximum number of undo blocks that this transaction can
         * consume. If the transaction attempts to fill more blocks then it
         * will fire an assert killing its thread and aborting its transaction.
         * The goal is to prevent a runaway transaction from consuming all the
         * undo blocks.
         * 
         * This is set when the transaction is begun rather than checking on
         * every undo block add. This ensures that reducing the maximum does
         * not affect currently running transactions.
         */
        uint32_t max_undo_blocks;

        /*
         * The following data is not maintained persistently. It could be kept
         * in a separate volatile memory struct, but it is simpler to put it
         * here and reconstruct when needed.
         */
        
        /**
         * This is the number of undo blocks currently filled by this
         * transaction. If it exceeds the maximum then the thread is killed.
         * It is recalculated before recovery since it is not atomically
         * maintained with adding a new undo block.
         */
        transient uint32_t cur_undo_blocks;

        /**
         * This is the current descriptor for the region containing this 
         * nvm_transaction. It is set at every region attach as part of 
         * recovery. 
         */
        transient nvm_desc desc;

        /**
         * This is the number of current undo operations in the entire linked
         * list of undo blocks. Rollback to savepoint or commit/abort of a
         * nested transaction will decrease this number. The current undo
         * operation count is used to compare undo records in linked lists to
         * decern their relative creation order.
         * 
         * This is not maintained atomically with committing undo to exist.
         * Thus it must be recalculated if the thread owning this transaction
         * dies.
         */
        transient uint32_t undo_ops;

        /**
         * This is the number of bytes of operation data that can be stored in
         * the current undo block assuming another operation would be 
         * allocated at the front of the undo data area. Note this will
         * always be a multiple of 8 for alignment.
         * 
         * This is not maintained atomically with committing undo to exist.
         * Thus it must be recalculated if the thread owning this transaction
         * dies.
         */
        transient uint32_t undo_bytes;

        /**
         * This is the low order bits of the time in seconds when the last 
         * attempt was made to spawn a thread to recover this transaction. It
         * is only meaningful if the transaction is in the dead transaction
         * list, and the spawn_cnt is not zero. 16 bits is enough to measure
         * time differences of up to 9 hours. If it takes more than a few
         * seconds for the spawned thread to remove the transaction from the
         * dead list then we will try again.
         */
        transient uint16_t spawn_time;

        /**
         * This is the number of threads that have been spawned attempting to
         * take ownership of this dead transaction. After a few tries we give
         * up on the presumption that something is seriously wrong.
         */
        transient uint8_t spawn_cnt;

        /* padding for alignment */
        uint8_t _pad1[1];

        /**
         * This is a pointer to the parent transaction in another region if
         * this is an off region nested transaction. If this is a base
         * transaction then the parent is null.
         */
        transient nvm_transaction ^parent;

        /**
         * This is the address of the last operation data written. New
         * operation data must be written below this. It is always 8 byte
         * aligned.
         * 
         * This is not maintained atomically with committing undo to exist.
         * Thus it must be recalculated if the thread owning this transaction
         * dies.
         */
        transient uint8_t ^undo_data;

        /**
         * Idle transactions are kept on a freelist for reuse. This is the link
         * for that list.
         */
        transient nvm_transaction ^link;

        uint8_t _padding_to_128[128 - 112]; //#
    };
#else
    struct nvm_transaction
    {
        /**
         * The USID that maps to the nvm_type for nvm_transaction
         */
        nvm_usid type_usid;    

        /**
         * The transaction slot number of this transaction. This is used to
         * identify the NVM locks that this transaction owns. This is set
         * when the transaction struct is allocated, and never changed.
         */
        uint16_t slot;

        /**
         * The current state of this transaction. THis actually contains a
         * nvm_trans_state, but to keep all the non-transient data in one
         * cache line it is packed in with the slot number.
         */
        uint8_t state;

        /**
         * This is 1 when the tansaction is dead and included in the count
         * of dead transactions in the transaction table volatile data.
         */
        uint8_t dead;    

        /**
         * This is just for debugging. Every time a transaction is begun a
         * new transaction number is assigned to the transaction. This is 
         * stored in the undo blocks as well as here to help in debugging.
         * Note that the transaction number can wrap since it is only 4 bytes.
         */
        uint32_t txnum;

        /**
         * This is the linked list of undo blocks for this transaction.
         */
        nvm_undo_blk_srp undo;    

        /**
         * This is a pointer to the last on_commit operation added.
         * The on_commit operations form a linked list that can be quickly
         * scanned when the transaction is in state committing. This does not
         * need need to be atomically updated with adding the on_commit
         * operation since it is ignored if there is a failure requiring 
         * rollback. Note that cleanup at commit is done in the order added
         * which is the reverse of the order on the linked list.
         */
        nvm_on_commit_srp commit_ops;    

        /**
         * This is a pointer to the last NVM lock operation added to the 
         * transaction. The NVM lock operations form a linked list so that
         * all the locks held to protect update of an NVM object can be
         * released at commit. This list includes nvm_on_abort records.
         */
        nvm_lkrec_srp held_locks; // nvm_lkrec ^held_locks

        /**
         * This points to the last save point undo operation. The save 
         * point operations are linked to each other by pointers. This is
         * needed for verifying a save point name before rollback.
         */
        nvm_savepnt_srp savept_last; // nvm_savepnt ^savept_last

        /**
         * This points to the currently active nested transaction's undo
         * operation. It is null if there are none. The Nested operations
         * form a linked list. This makes it quick to apply or discard
         * the redo for a nested transaction.
         */
        nvm_nested_srp nstd_last; // nvm_nested ^nstd_last
        
        /**
         * This is the maximum number of undo blocks that this transaction can
         * consume. If the transaction attempts to fill more blocks then it
         * will fire an assert killing its thread and aborting its transaction.
         * The goal is to prevent a runaway transaction from consuming all the
         * undo blocks.
         * 
         * This is set when the transaction is begun rather than checking on
         * every undo block add. This ensures that reducing the maximum does
         * not affect currently running transactions.
         */
        uint32_t max_undo_blocks;

        /*
         * The following data is not maintained persistently. It could be kept
         * in a separate volatile memory struct, but it is simpler to put it
         * here and reconstruct when needed.
         */
        
        /**
         * This is the number of undo blocks currently filled by this
         * transaction. If it exceeds the maximum then the thread is killed.
         * It is recalculated before recovery since it is not atomically
         * maintained with adding a new undo block.
         */
        uint32_t cur_undo_blocks;

        /**
         * This is the current descriptor for the region containing this 
         * nvm_transaction. It is set at every region attach as part of 
         * recovery. 
         */
        nvm_desc desc;    

        /**
         * This is the number of current undo operations in the entire linked
         * list of undo blocks. Rollback to savepoint or commit/abort of a
         * nested transaction will decrease this number. The current undo
         * operation count is used to compare undo records in linked lists to
         * decern their relative creation order.
         * 
         * This is not maintained atomically with committing undo to exist.
         * Thus it must be recalculated if the thread owning this transaction
         * dies.
         */
        uint32_t undo_ops;    

        /**
         * This is the number of bytes of operation data that can be stored in
         * the current undo block assuming another operation would be 
         * allocated at the front of the undo data area. Note this will
         * always be a multiple of 8 for alignment.
         * 
         * This is not maintained atomically with committing undo to exist.
         * Thus it must be recalculated if the thread owning this transaction
         * dies.
         */
        uint32_t undo_bytes;    

        /**
         * This is the low order bits of the time in seconds when the last 
         * attempt was made to spawn a thread to recover this transaction. It
         * is only meaningful if the transaction is in the dead transaction
         * list, and the spawn_cnt is not zero. 16 bits is enough to measure
         * time differences of up to 9 hours. If it takes more than a few
         * seconds for the spawned thread to remove the transaction from the
         * dead list then we will try again.
         */
        uint16_t spawn_time;    

        /**
         * This is the number of threads that have been spawned attempting to
         * take ownership of this dead transaction. After a few tries we give
         * up on the presumption that something is seriously wrong.
         */
        uint8_t spawn_cnt;    

        /* padding for alignment */
        uint8_t _pad1[1];

        /**
         * This is a pointer to the parent transaction in another region if
         * this is an off region nested transaction. If this is a base
         * transaction then the parent is null.
         */
        nvm_transaction *parent;

        /**
         * This is the address of the last operation data written. New
         * operation data must be written below this. It is always 8 byte
         * aligned.
         * 
         * This is not maintained atomically with committing undo to exist.
         * Thus it must be recalculated if the thread owning this transaction
         * dies.
         */
        uint8_t *undo_data;    

        /**
         * Idle transactions are kept on a freelist for reuse. This is the link
         * for that list.
         */
        nvm_transaction *link;

        uint8_t _padding_to_128[128 - 112];
    };
#endif //NVM_EXT
    extern const nvm_type nvm_type_nvm_transaction;

    /**
     * This struct holds a page of undo for a single transaction. If a
     * transaction needs more undo then another page can link to this one.
     */
#ifdef NVM_EXT
    persistent
    struct 
    USID("fb7a d37c e443 f03b 0e55 9be8 bd5e a95e")
    tag("One block of undo for a transaction")
    version(0)
    size(4*1024)
    nvm_undo_blk
    {
        /**
         * The USID that maps to the nvm_type for nvm_undo_blk
         */
//        nvm_usid type_usid; //#

        /**
         * This is a link to another page of undo that filled up before this
         * page was added to the transaction. This will be null in the first
         * page allocated for a transaction since it is the end of the list.
         * 
         * This link is also used for maintaining a free list of undo blocks.
         * The free list is reconstructed after recovery at region attach.
         */
        nvm_undo_blk ^link;

        /**
         * This is the number of undo operations stored in this undo page.
         * Incrementing this count after constructing an operation atomically
         * makes the next undo operation exist. Decrementing this retires the
         * undo after it has been applied or the transaction committed.
         */
        uint16_t count;

        /**
         * This is only to help debugging. It tracks the highest valid undo to
         * see what was done by the last transaction to commit while using this
         * undo block. This is updated when a new undo operation is created,
         * but not changed when count is decremented. Allocating undo after a
         * roll back to save point can reduce this value.
         */
        uint16_t hi_count;

        /**
         * This is also for debugging. It is the transaction number of the
         * last transaction to use this undo block. The transaction number is
         * incremented every time a transaction is begun, and assigned to
         * the new transaction. Since undo blocks are moved to the tail of the
         * undo block free list, there is a tendency for recent undo to still 
         * be available for debugging after a crash.
         */
        uint32_t txnum;

        /*
         * Leave some padding for future growth. 
         */
        uint8_t pad[32];

        /*
         * This is the undo data. While it is declared as an array of uint8_t
         * it actually starts with an array of operation[]. Each operation
         * contains an opcode and the size of its data. The operation data
         * is stored at the end of the array growing toward the front. The
         * page is full when the operation array meets the operation data.
         * The data for an operation must be 8 byte aligned, but the size
         * of the data could be an odd number of bytes leaving some unused 
         * padding for alignment.
         */
        uint8_t data[4096 - 64];
    };
#else
    struct nvm_undo_blk
    {
        /**
         * The USID that maps to the nvm_type for nvm_undo_blk
         */
        nvm_usid type_usid;    

        /**
         * This is a link to another page of undo that filled up before this
         * page was added to the transaction. This will be null in the first
         * page allocated for a transaction since it is the end of the list.
         * 
         * This link is also used for maintaining a free list of undo blocks.
         * The free list is reconstructed after recovery at region attach.
         */
        nvm_undo_blk_srp link;    // nvm_undo_blk ^link

        /**
         * This is the number of undo operations stored in this undo page.
         * Incrementing this count after constructing an operation atomically
         * makes the next undo operation exist. Decrementing this retires the
         * undo after it has been applied or the transaction committed.
         */
        uint16_t count;

        /**
         * This is only to help debugging. It tracks the highest valid undo to
         * see what was done by the last transaction to commit while using this
         * undo block. This is updated when a new undo operation is created,
         * but not changed when count is decremented. Allocating undo after a
         * roll back to save point can reduce this value.
         */
        uint16_t hi_count;

        /**
         * This is also for debugging. It is the transaction number of the
         * last transaction to use this undo block. The transaction number is
         * incremented every time a transaction is begun, and assigned to
         * the new transaction. Since undo blocks are moved to the tail of the
         * undo block free list, there is a tendency for recent undo to still 
         * be available for debugging after a crash.
         */
        uint32_t txnum;

        /*
         * Leave some padding for future growth. 
         */
        uint8_t _pad1[32];

        /*
         * This is the undo data. While it is declared as an array of uint8_t
         * it actually starts with an array of operation[]. Each operation
         * contains an opcode and the size of its data. The operation data
         * is stored at the end of the array growing toward the front. The
         * page is full when the operation array meets the operation data.
         * The data for an operation must be 8 byte aligned, but the size
         * of the data could be an odd number of bytes leaving some unused 
         * padding for alignment.
         */
        uint8_t data[4096 - 64];
    };
#endif //NVM_EXT
    extern const nvm_type nvm_type_nvm_undo_blk;


    /**
     * This struct contains the non-volatile transaction data. This is the data
     * needed to do recovery if the process attached to the region terminates
     * with transactions that are not idle. If a region needs more 
     * concurrent transactions or more undo, then additional instances of
     * this struct can be created.
     */
#define TRANS_TABLE_SLOTS (63)
#define TRANS_TABLE_UNDO_BLKS (254)
#ifdef NVM_EXT
    persistent
    struct 
    USID("1c2b e750 6f5f f07a 3083 5fc7 b2be 8c09")
    tag("A group of transaction slots and undo blocks")
    version(0)
    size(1024*1024)
    nvm_trans_table
    {
        /**
         * The USID that maps to the nvm_type for nvm_trans_table
         */
//        nvm_usid type_usid; //#

        /**
         * To support additional concurrent transactions and/or more undo
         * blocks, more transaction tables can be linked to this one. There
         * will only be one nvm_trans_table_data for all the 
         * transaction tables in a region.
         */
        nvm_trans_table ^link;

        /**
         * padding for 128 byte alignment of nvm_transaction
         */
        uint8_t pad[128 - 24];

        /**
         * This is the array of transaction slots in this transaction table.
         */
        nvm_transaction transactions[TRANS_TABLE_SLOTS];

        /**
         * This is the array of undo blocks provided by this transaction
         * table.
         */
        nvm_undo_blk undo_blks[TRANS_TABLE_UNDO_BLKS];

    };
#else
    struct nvm_trans_table
    {
        /**
         * The USID that maps to the nvm_type for nvm_trans_table
         */
        nvm_usid type_usid; //#

        /**
         * To support additional concurrent transactions and/or more undo
         * blocks, more transaction tables can be linked to this one. There
         * will only be one nvm_trans_table_data for all the 
         * transaction tables in a region.
         */
        nvm_trans_table_srp link; // nvm_trans_table *link

        /**
         * padding for 128 byte alignment of nvm_transaction
         */
        uint8_t _pad1[128 - 24];    

        /**
         * This is the array of transaction slots in this transaction table.
         */
        nvm_transaction transactions[TRANS_TABLE_SLOTS];

        /**
         * This is the array of undo blocks provided by this transaction
         * table.
         */
        nvm_undo_blk undo_blks[TRANS_TABLE_UNDO_BLKS];

    };
#endif //NVM_EXT
    extern const nvm_type nvm_type_nvm_trans_table;

    /*************************************************************************/
    /* The following defines the structs and constants for representing undo */
    /*************************************************************************/
    /**
     * Every undo entry has an opcode that defines how it interprets the 
     * data associated with the entry. 
     */
    enum nvm_opcode
    {
        /**
         * Ignore this operation. The code never creates an undo record with 
         * this opcode. Recovery will ignore the undo record. This may be used
         * to prevent recovery from failing when there is a bad undo record.
         * Patching the record's opcode to be zero will avoid applying it/.
         */
        nvm_op_ignore = 0,
        /*
         * Restore the data in the data. The first 8 bytes of data is 
         * an address to restore to. The remaining data bytes are the data 
         * to copy. Note that the data is treated as bytes and thus might 
         * not be a multiple of 8 bytes.
         */
        nvm_op_restore = 1,

        /**
         * NVM lock. The record contains the address of the nvm_amutex
         * locked by the transaction, and the nvm_lock_state of the lock
         * aquisition. It also holds the highest lock level held by the
         * transaction for deadlock prevention. This is used
         * to release the lock during rollback or at commit.
         */
        nvm_op_lock = 2,

        /**
         * Savepoint for partial roll back. The data contains the undo
         * operation number of the next save point. A self relative pointer 
         * follows the operation number. It is used for a pointer to an 
         * NVM address which names the savepoint.
         */
        nvm_op_savepoint = 5,

        /**
         * Beginning of a nested transaction. A commit will discard
         * undo until this Operation is discarded. An abort of the
         * nested transaction will apply undo until this is applied.
         * The data points to the previous nested transaction record,
         * if any, and the current state of the parent transaction to
         * restore when the nested transaction commits or aborts.
         */
        nvm_op_nested = 6,

        /**
         * Call a function with a registered USID when this undo record
         * is applied. The data contains the USID of the function to call
         * and a context to pass to the function. Any
         * locks acquired by the transaction after generating this
         * record are released before the function is called. The function
         * is called in a nested transaction that is committed when it
         * returns.
         */
        nvm_op_on_abort = 7,

        /**
         * Call a function with a registered USID when this transaction
         * commits. The data contains the USID of the function to call
         * and a context to pass to the function. Any locks acquired by
         * the transaction  are released before the function is called.
         * The function is called in a nested transaction that is
         * committed when it returns.
         */
        nvm_op_on_commit = 8,

        /**
         * Call a function with a registered USID when this transaction
         * releases locks at either commit or abort. This is linked into
         * the list of locks so that it is applied as part of lock release.
         * The data also contains the USID of the function to call
         * and a context to pass to the function. Any locks acquired by
         * the transaction after this record is created are released before
         * the function is called. The function is called in a nested
         * transaction that is committed when it returns.
         */
        nvm_op_on_unlock = 9,

    };
    typedef enum nvm_opcode nvm_opcode;
    /**
     * The data area of an undo page starts with an array of nvm_operation's. 
     * Each operation has an opcode to describe what it does and the number
     * of bytes of data stored at the end of the block for it to operate 
     * on. Note that there are only 12 bits of data size so this imposes 
     * a limit of 4095 bytes of data for any operation. An operation may 
     * contain an odd number of data bytes, but the next operation data
     * will always start at an 8 byte boundary.
     */
#ifdef NVM_EXT
    struct nvm_operation
    {
        uint16_t opcode : 4; // the operation to perform see nvm_opcode
        uint16_t size : 12; // the count of data bytes
    };
#else
    struct nvm_operation
    {
        uint16_t opcode : 4; // the operation to perform see nvm_opcode
        uint16_t size : 12; // the count of data bytes
    };
#endif //NVM_EXT
    typedef struct nvm_operation nvm_operation;
    
    /**
     * This struct is the data for an nvm_op_restore operation. It is the
     * most common undo operation. It holds data that needs to be restored
     * to undo changes made by the transaction. It also has a pointer to 
     * the location to restore.
     */
#ifdef NVM_EXT
    persistent
    struct nvm_restore
    {
        /** address to restore to */
        void ^addr;

        /** data to restore */
        uint8_t data[];
    };
#else
    struct nvm_restore
    {
        /** address to restore to */
        void_srp addr;//  void ^addr

        /** data to restore */
        uint8_t data[];
    };
#endif //NVM_EXT

    /**
     * These are the states that a lock undo record can be in. This is used
     * for recovery to determine if a share lock was acquired on not.
     */
    enum nvm_lock_state
    {
        /** Acquiring shared */
        nvm_lock_acquire_s = 1,

        /** Held shared */
        nvm_lock_held_s = 2,

        /** Releasing share */
        nvm_lock_release_s = 3,

        /** Share lock was released or not acquired */
        nvm_lock_free_s = 4,

        /** Exclusive lock. See mutex->owner to see if held. */
        nvm_lock_x = 5,

        /** This state means this is really an nvm_on_unlock record. */
        nvm_lock_callback = 6,
    };
    typedef enum nvm_lock_state nvm_lock_state;

    /**
     * This struct is the data for an nvm_op_xlock or nvm_op_slock. These form
     * a linked list so that all the locks can be released at commit. It also
     * contains the count of previous undo operations to determine which
     * nested transaction acquired the lock.
     */
#ifdef NVM_EXT
    persistent
    struct nvm_lkrec
    {
        /** Previous nvm_lkrec in this transacation or a parent */
        nvm_lkrec ^prev;

        /** undo operations before this one */
        uint32_t undo_ops;

        /** Current state of this lock. This is nvm_lock_callback if this
         * is actually an nvm_on_unlock record. */
        uint8_t state;

        /* Maximum lock level of any held locks before this one is acquired. */
        uint8_t old_level;

        /* Lock level after this lock is acquired. This may be the old level
         * rather then this lock's level if a no wait get of a lower level lock
         * succeeds. */
        uint8_t new_level;

        /** Pad to pointer alignment. */
        uint8_t _pad1[1];

        /** pointer to the mutex being locked. */
        nvm_amutex ^mutex;
    };
#else
    struct nvm_lkrec
    {
        /** Previous nvm_lkrec in this transacation or a parent */
        nvm_lkrec_srp prev; // nvm_lkrec ^prev

        /** undo operations before this one */
        uint32_t undo_ops;

        /** Current state of this lock. This is nvm_lock_callback if this
         * is actually an nvm_on_unlock record. */
        uint8_t state;

        /* Maximum lock level of any held locks before this one is acquired. */
        uint8_t old_level;

        /* Lock level after this lock is acquired. This may be the old level
         * rather then this lock's level if a no wait get of a lower level lock
         * succeeds. */
        uint8_t new_level;

        /** Pad to pointer alignment. */
        uint8_t _pad1[1];

        /** pointer to the mutex being locked. */
        nvm_amutex_srp mutex; //nvm_amutex ^mutex
    };
#endif //NVM_EXT
    
    /**
     * This struct is the data for an nvm_op_on_unlock operation. It contains
     * the USID of the function to call and the context to pass to it when
     * this transaction unlocks. These form a linked list so they can be 
     * quickly found at commit time. It also contains the count of previous 
     * undo operations to determine which nested transaction it is associated
     * with.
     * 
     * These are linked into the same list as the nvm_lock undo records. To 
     * make this work it is necessary that the fields prev, undo_ops, and
     * state must be identical in both structs.
     */
#ifdef NVM_EXT
    persistent
    struct nvm_on_unlock
    {
        /** Previous nvm_lkrec or nvm_on_unlock operation or null if none */
        nvm_lkrec ^prev;

        /** undo operations before this one */
        uint32_t undo_ops;

        /** This is nvm_lock_callback to indicate this is actually an
         * nvm_on_unlock record. */
        uint8_t state;

        /** Pad to pointer alignment */
        uint8_t _pad1[3];

        /** This is the USID that identifies the function to call  */
        nvm_usid func;
//        void (|func@)(void^); TODO use correct syntax

        /** This is the data that the application wants passed to it */
        uint8_t data[];
    };
#else
    struct nvm_on_unlock
    {
        /** Previous nvm_lkrec or nvm_on_unlock operation or null if none */
        nvm_lkrec_srp prev; //nvm_lkrec ^prev

        /** undo operations before this one */
        uint32_t undo_ops;

        /** This is nvm_lock_callback to indicate this is actually an
         * nvm_on_unlock record. */
        uint8_t state;

        /** Pad to pointer alignment */
        uint8_t _pad1[3];

        /** This is the USID that identifies the function to call  */
        nvm_usid func;

        /** This is the data that the application wants passed to it */
        uint8_t data[];
    };
#endif //NVM_EXT
    
    /**
     * This struct is the data for an nvm_op_savepoint. These form
     * a linked list so that the savepoint can be found before rollback to
     * savepoint. It also contains the count of previous undo operations to
     * determine which nested transaction set the savepoint.
     */
#ifdef NVM_EXT
    persistent
    struct nvm_savepnt
    {
        /** Previous nvm_savepnt in this transacation or a parent */
        nvm_savepnt ^prev;

        /** undo operations before this one */
        uint32_t undo_ops;

        /** Pad to pointer alignment */
        uint8_t _pad1[4];

        /** Arbitrary pointer to identify the savepoint. */
        void ^name;
    };
#else
    struct nvm_savepnt
    {
        /** Previous nvm_savepnt in this transacation or a parent */
        nvm_savepnt_srp prev; //nvm_savepnt ^prev

        /** undo operations before this one */
        uint32_t undo_ops;

        /** Pad to pointer alignment */
        uint8_t _pad1[4];

        /** Arbitrary pointer to identify the savepoint. */
        void_srp name; //void ^name
    };
#endif //NVM_EXT
    

    /**
     * This struct is the data for an nvm_op_nested operation. These form
     * a linked list so that the current transaction can be found when a
     * nested transaction aborts or commits. It also contains the count of 
     * previous undo operations to identify other undo records that are in
     * parent transactions. It holds the transaction state of the nested
     * transaction.
     */
#ifdef NVM_EXT
    persistent
    struct nvm_nested
    {
        /** Previous nvm_nested operation or null if none */
        nvm_nested ^prev;

        /** undo operations before this one */
        uint32_t undo_ops;

        /** Transaction state of this transaction. */
        nvm_trans_state state;
    };
#else
    struct nvm_nested
    {
        /** Previous nvm_nested operation or null if none */
        nvm_nested_srp prev; // nvm_nested ^prev

        /** undo operations before this one */
        uint32_t undo_ops;

        /** Transaction state of this transaction. */
        nvm_trans_state state;
    };
#endif //NVM_EXT
    
    /**
     * This struct is the data for an nvm_op_on_abort operation. It contains
     * the USID of the function to call and the context to pass to it when
     * this undo record is applied.
     */
#ifdef NVM_EXT
    persistent
    struct nvm_on_abort
    {
        /** This is the USID that identifies the function to call  */
        nvm_usid func;
//        void (|func@)(void^); TODO use correct syntax

        /** This is the data that the application wants passed to it */
        uint8_t data[];
    };
#else
    struct nvm_on_abort
    {
        /** This is the USID that identifies the function to call  */
        nvm_usid func;

        /** This is the data that the application wants passed to it */
        uint8_t data[];
    };
#endif //NVM_EXT
    
    /**
     * This struct is the data for an nvm_op_on_commit operation. It contains
     * the USID of the function to call and the context to pass to it when
     * this transaction commits. These form a linked list so they can be 
     * quickly found at commit time. It also contains the count of previous 
     * undo operations to determine which nested transaction it is associated
     * with.
     */
#ifdef NVM_EXT
    persistent
    struct nvm_on_commit
    {
        /** Previous nvm_on_commit operation or null if none */
        nvm_on_commit ^prev;

        /** undo operations before this one */
        uint32_t undo_ops;

        /** Pad to pointer alignment */
        uint8_t _pad1[4];

        /** Transient forward link used at commit and ignored otherwise. */
        transient nvm_on_commit ^next;

        /** This is the USID that identifies the function to call  */
        nvm_usid func;
//        void (|func@)(void^); TODO use correct syntax

        /** This is the data that the application wants passed to it */
        uint8_t data[];
    };
#else
    struct nvm_on_commit
    {
        /** Previous nvm_on_commit operation or null if none */
        nvm_on_commit_srp prev; // nvm_on_commit ^prev

        /** undo operations before this one */
        uint32_t undo_ops;

        /** Pad to pointer alignment */
        uint8_t _pad1[4];

        /** Transient forward link used at commit and ignored otherwise. */
        nvm_on_commit *next;

        /** This is the USID that identifies the function to call  */
        nvm_usid func;

        /** This is the data that the application wants passed to it */
        uint8_t data[];
    };
#endif //NVM_EXT
    
    /**
     * This is the volatile memory for managing the transaction tables in one
     * NVM region. Exactly one nvm_trans_table_data is created when a region
     * is attached, even if the region has multiple nvm_trans_table structs
     * allocated in NVM.
     * 
     * This is created by nvm_recover at attach time.
     */
#ifdef NVM_EXT
    struct nvm_trans_table_data
    {
        /**
         * This points to the NVM region managed by this nvm_trans_table_data.
         */
        nvm_region ^region;
        
        /**
         * This is the mutex that is acquired when allocating or freeing a
         * transaction.
         */
        nvms_mutex trans_mutex;

        /**
         * This is a condition variable that a thread waits on if there are 
         * insufficient free nvm_ransaction structs.
         */
        nvms_cond trans_cond;

        /**
         * This counter is used to assign a new transaction number to every
         * transaction. It is only used for debugging. The transaction number
         * will appear in the nvm_transaction and every nvm_undo_blk allocated
         * to the transaction. This allows investigation of the most recent
         * activity in the NVM region.
         */
        uint32_t txnum;

        /**
         * This is the number of Transaction objects that are currently
         * active and not on the freelist or in recovery.
         */
        uint32_t atrans_cnt;

        /**
         * This is the number of dead transactions that are in recovery. This
         * includes transactions on the dead list as well as those being
         * worked on by a recovery thread. Since these should complete on
         * their own, detach will wait for the recovery to complete. 
         */
        uint32_t dtrans_cnt;

        /**
         * This is the total number of nvm_transaction structs that exist in
         * all of the nvm_trans_data structs in NVM. The application could
         * have limited the number of simultaneous transactions to a smaller
         * value. The extra nvm_transaction structs are in state reserved.
         */
        uint32_t ttrans_cnt;

        /**
         * This is the number of reserved nvm_transaction structs set by the
         * application via nvm_set_txconfig. The total transactions minus this
         * is the maximum possible concurrent transactions.
         */
        uint32_t rtrans_cnt;

        /**
         * This is true if the region is being detached and cannot support
         * beginning new transactions.
         */
        int detaching;

        /**
         * This is true if the region is being recovered and cannot be
         * detached.
         */
        int recovering;

        /**
         * This is the head of a linked list of nvm_transaction structs
         * that are in state idle. The list is not maintained consistently
         * in NVM so it must be reconstructed at recovery.
         */
        nvm_transaction ^free_trans;

        /**
         * This is a list of transactions that were owned by a thread that died.
         * The thread may have been part of a previous application instance
         * that died or detached the region without ending all transactions.
         * The thread could be a thread that was running in this application
         * instance but encountered a problem. A transaction only stays in this
         * list until a new thread takes on the responsibility of recovering
         * the transaction.
         */
        nvm_transaction ^dead_trans;

        /**
         * This is the mutex that is acquired when allocating or freeing undo
         * blocks.
         */
        nvms_mutex undo_mutex;

        /**
         * This is a condition variable that a thread waits on if there are 
         * insufficient free NVundo objects.
         */
        nvms_cond undo_cond;

        /**
         * This is the head of the linked list of free undo pages. Allocation
         * of undo takes pages from the head of the list. 
         * 
         * The free list is reconstructed to contain all undo blocks at
         * region attach. Thus no undo is needed for the undo list itself.
         */
        nvm_undo_blk ^free_undo;
    };
#else
    struct nvm_trans_table_data
    {
        /**
         * This points to the NVM region managed by this nvm_trans_table_data.
         */
        nvm_region *region;
        
        /**
         * This is the mutex that is acquired when allocating or freeing a
         * transaction.
         */
        nvms_mutex trans_mutex;

        /**
         * This is a condition variable that a thread waits on if there are 
         * insufficient free nvm_ransaction structs.
         */
        nvms_cond trans_cond;

        /**
         * This counter is used to assign a new transaction number to every
         * transaction. It is only used for debugging. The transaction number
         * will appear in the nvm_transaction and every nvm_undo_blk allocated
         * to the transaction. This allows investigation of the most recent
         * activity in the NVM region.
         */
        uint32_t txnum;

        /**
         * This is the number of Transaction objects that are currently
         * active and not on the freelist or in recovery.
         */
        uint32_t atrans_cnt;

        /**
         * This is the number of dead transactions that are in recovery. This
         * includes transactions on the dead list as well as those being
         * worked on by a recovery thread. Since these should complete on
         * their own, detach will wait for the recovery to complete. 
         */
        uint32_t dtrans_cnt;

        /**
         * This is the total number of nvm_transaction structs that exist in
         * all of the nvm_trans_data structs in NVM. The application could
         * have limited the number of simultaneous transactions to a smaller
         * value. The extra nvm_transaction structs are in state reserved.
         */
        uint32_t ttrans_cnt;

        /**
         * This is the number of reserved nvm_transaction structs set by the
         * application via nvm_set_txconfig. The total transactions minus this
         * is the maximum possible concurrent transactions.
         */
        uint32_t rtrans_cnt;

        /**
         * This is true if the region is being detached and cannot support
         * beginning new transactions.
         */
        int detaching;

        /**
         * This is true if the region is being recovered and cannot be
         * detached.
         */
        int recovering;

        /**
         * This is the head of a linked list of nvm_transaction structs
         * that are in state idle. The list is not maintained consistently
         * in NVM so it must be reconstructed at recovery.
         */
        nvm_transaction *free_trans;

        /**
         * This is a list of transactions that were owned by a thread that died.
         * The thread may have been part of a previous application instance
         * that died or detached the region without ending all transactions.
         * The thread could be a thread that was running in this application
         * instance but encountered a problem. A transaction only stays in this
         * list until a new thread takes on the responsibility of recovering
         * the transaction.
         */
        nvm_transaction *dead_trans;

        /**
         * This is the mutex that is acquired when allocating or freeing undo
         * blocks.
         */
        nvms_mutex undo_mutex;

        /**
         * This is a condition variable that a thread waits on if there are 
         * insufficient free NVundo objects.
         */
        nvms_cond undo_cond;

        /**
         * This is the head of the linked list of free undo pages. Allocation
         * of undo takes pages from the head of the list. 
         * 
         * The free list is reconstructed to contain all undo blocks at
         * region attach. Thus no undo is needed for the undo list itself.
         */
        nvm_undo_blk *free_undo;    
    };
#endif //NVM_EXT
    

    /**************************************************************************/
    /* The following defines the private functions for transaction management */
    /**************************************************************************/

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
     * The initial nvm_trans_table in NVM.
     */
#ifdef NVM_EXT
    void nvm_create_trans_table(
        nvm_region ^rg,
        nvm_heap ^rh
        );
#else
    void nvm_create_trans_table(//#
        nvm_region *rg,
        nvm_heap *rh
        );
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
    void nvm_recover(nvm_desc desc);
#else
    void nvm_recover(nvm_desc desc);
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
     * @param[in] st
     * The initial state for the lock. This will indicate if this is a share
     * or exclusive lock.
     * 
     * @return
     * Pointer to the new nvm_lkrec struct
     */
#ifdef NVM_EXT
    nvm_lkrec ^nvm_add_lock_op@(
        nvm_transaction ^tx,
        nvm_thread_data *td,
        nvm_amutex ^mx,
        nvm_lock_state st);
#else
    nvm_lkrec *nvm_add_lock_op(
        nvm_transaction *tx,
        nvm_thread_data *td,
        nvm_amutex *mx,
        nvm_lock_state st);
#endif //NVM_EXT
    
    /**
     * Apply the last undo record in the current transaction. This is generally 
     * used to throw away an unneeded undo record that is a no-op. For example
     * it is used to remove the nvm_lkrec record for a lock operation that timed
     * out.
     * 
     * @param[in] tx
     * The transaction to rollback one record
     * 
     * @param[in] td
     * The thread data for the thread that owns the transaction.
     */
#ifdef NVM_EXT
    void nvm_apply_one@(nvm_transaction ^tx, nvm_thread_data *td);
#else
    void nvm_apply_one(nvm_transaction *tx, nvm_thread_data *td);
#endif //NVM_EXT
    
    /**
     * This counts the number of share locks on an NVM mutex before beginning
     * recovery of the dead transactions at NVM region attach time.
     * 
     * @param[in] ttd
     * The volatile memory transaction table data. It has the linked list of
     * dead transactions to scan.
     * 
     * @param[in] mutex
     * The mutex to search for.
     * 
     * @return 
     */
#ifdef NVM_EXT
    int nvm_find_shares(nvm_trans_table_data *ttd, nvm_amutex ^mutex);
#else
    int nvm_find_shares(nvm_trans_table_data *ttd, nvm_amutex *mutex);
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
    int nvm_trans_detach(nvm_region_data *rd);

    /**
     * This frees up a nvm_trans_table_data allocated by nvm_recover. 
     * @param trans_table points to trans_table pointer in region data
     * @return 
     */
    int nvm_trans_free_table(nvm_trans_table_data **trans_table);

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
    void nvm_recover_dead(nvm_trans_table_data *ttd);
    
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
    void
#ifdef NVM_EXT
    nvm_txrecover(
        void *ctx
        );
#else
    nvm_txrecover(
        void *ctx
        );
#endif //NVM_EXT

#ifdef	__cplusplus
}
#endif

#endif	/* NVM_TRANSACTION0_H */

