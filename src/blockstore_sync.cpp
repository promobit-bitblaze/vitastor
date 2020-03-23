// Copyright (c) Vitaliy Filippov, 2019+
// License: VNPL-1.1 (see README.md for details)

#include "blockstore_impl.h"

#define SYNC_HAS_SMALL 1
#define SYNC_HAS_BIG 2
#define SYNC_DATA_SYNC_SENT 3
#define SYNC_DATA_SYNC_DONE 4
#define SYNC_JOURNAL_WRITE_SENT 5
#define SYNC_JOURNAL_WRITE_DONE 6
#define SYNC_JOURNAL_SYNC_SENT 7
#define SYNC_DONE 8

int blockstore_impl_t::continue_sync(blockstore_op_t *op, bool queue_has_in_progress_sync)
{
    if (immediate_commit == IMMEDIATE_ALL)
    {
        // We can return immediately because sync is only dequeued after all previous writes
        op->retval = 0;
        FINISH_OP(op);
        return 2;
    }
    if (PRIV(op)->op_state == 0)
    {
        stop_sync_submitted = false;
        unsynced_big_write_count -= unsynced_big_writes.size();
        PRIV(op)->sync_big_writes.swap(unsynced_big_writes);
        PRIV(op)->sync_small_writes.swap(unsynced_small_writes);
        PRIV(op)->sync_small_checked = 0;
        PRIV(op)->sync_big_checked = 0;
        unsynced_big_writes.clear();
        unsynced_small_writes.clear();
        if (PRIV(op)->sync_big_writes.size() > 0)
            PRIV(op)->op_state = SYNC_HAS_BIG;
        else if (PRIV(op)->sync_small_writes.size() > 0)
            PRIV(op)->op_state = SYNC_HAS_SMALL;
        else
            PRIV(op)->op_state = SYNC_DONE;
    }
    if (PRIV(op)->op_state == SYNC_HAS_SMALL)
    {
        // No big writes, just fsync the journal
        if (journal.sector_info[journal.cur_sector].dirty)
        {
            // Write out the last journal sector if it happens to be dirty
            BS_SUBMIT_GET_ONLY_SQE(sqe);
            prepare_journal_sector_write(journal, journal.cur_sector, sqe, [this, op](ring_data_t *data) { handle_sync_event(data, op); });
            PRIV(op)->min_flushed_journal_sector = PRIV(op)->max_flushed_journal_sector = 1 + journal.cur_sector;
            PRIV(op)->pending_ops = 1;
            PRIV(op)->op_state = SYNC_JOURNAL_WRITE_SENT;
            return 1;
        }
        else
        {
            PRIV(op)->op_state = SYNC_JOURNAL_WRITE_DONE;
        }
    }
    if (PRIV(op)->op_state == SYNC_HAS_BIG)
    {
        // 1st step: fsync data
        if (!disable_data_fsync)
        {
            BS_SUBMIT_GET_SQE(sqe, data);
            my_uring_prep_fsync(sqe, data_fd, IORING_FSYNC_DATASYNC);
            data->iov = { 0 };
            data->callback = [this, op](ring_data_t *data) { handle_sync_event(data, op); };
            PRIV(op)->min_flushed_journal_sector = PRIV(op)->max_flushed_journal_sector = 0;
            PRIV(op)->pending_ops = 1;
            PRIV(op)->op_state = SYNC_DATA_SYNC_SENT;
            return 1;
        }
        else
        {
            PRIV(op)->op_state = SYNC_DATA_SYNC_DONE;
        }
    }
    if (PRIV(op)->op_state == SYNC_DATA_SYNC_DONE)
    {
        // 2nd step: Data device is synced, prepare & write journal entries
        // Check space in the journal and journal memory buffers
        blockstore_journal_check_t space_check(this);
        if (!space_check.check_available(op, PRIV(op)->sync_big_writes.size(), sizeof(journal_entry_big_write), JOURNAL_STABILIZE_RESERVATION))
        {
            return 0;
        }
        // Get SQEs. Don't bother about merging, submit each journal sector as a separate request
        struct io_uring_sqe *sqe[space_check.sectors_to_write];
        for (int i = 0; i < space_check.sectors_to_write; i++)
        {
            BS_SUBMIT_GET_SQE_DECL(sqe[i]);
        }
        // Prepare and submit journal entries
        auto it = PRIV(op)->sync_big_writes.begin();
        int s = 0, cur_sector = -1;
        while (it != PRIV(op)->sync_big_writes.end())
        {
            if (!journal.entry_fits(sizeof(journal_entry_big_write)) &&
                journal.sector_info[journal.cur_sector].dirty)
            {
                if (cur_sector == -1)
                    PRIV(op)->min_flushed_journal_sector = 1 + journal.cur_sector;
                prepare_journal_sector_write(journal, journal.cur_sector, sqe[s++], [this, op](ring_data_t *data) { handle_sync_event(data, op); });
                cur_sector = journal.cur_sector;
            }
            journal_entry_big_write *je = (journal_entry_big_write*)prefill_single_journal_entry(
                journal, (dirty_db[*it].state & BS_ST_INSTANT) ? JE_BIG_WRITE_INSTANT : JE_BIG_WRITE,
                sizeof(journal_entry_big_write)
            );
            dirty_db[*it].journal_sector = journal.sector_info[journal.cur_sector].offset;
            journal.used_sectors[journal.sector_info[journal.cur_sector].offset]++;
#ifdef BLOCKSTORE_DEBUG
            printf(
                "journal offset %08lx is used by %lx:%lx v%lu (%lu refs)\n",
                dirty_db[*it].journal_sector, it->oid.inode, it->oid.stripe, it->version,
                journal.used_sectors[journal.sector_info[journal.cur_sector].offset]
            );
#endif
            je->oid = it->oid;
            je->version = it->version;
            je->offset = dirty_db[*it].offset;
            je->len = dirty_db[*it].len;
            je->location = dirty_db[*it].location;
            je->crc32 = je_crc32((journal_entry*)je);
            journal.crc32_last = je->crc32;
            it++;
        }
        prepare_journal_sector_write(journal, journal.cur_sector, sqe[s++], [this, op](ring_data_t *data) { handle_sync_event(data, op); });
        assert(s == space_check.sectors_to_write);
        if (cur_sector == -1)
            PRIV(op)->min_flushed_journal_sector = 1 + journal.cur_sector;
        PRIV(op)->max_flushed_journal_sector = 1 + journal.cur_sector;
        PRIV(op)->pending_ops = s;
        PRIV(op)->op_state = SYNC_JOURNAL_WRITE_SENT;
        return 1;
    }
    if (PRIV(op)->op_state == SYNC_JOURNAL_WRITE_DONE)
    {
        if (!disable_journal_fsync)
        {
            BS_SUBMIT_GET_SQE(sqe, data);
            my_uring_prep_fsync(sqe, journal.fd, IORING_FSYNC_DATASYNC);
            data->iov = { 0 };
            data->callback = [this, op](ring_data_t *data) { handle_sync_event(data, op); };
            PRIV(op)->pending_ops = 1;
            PRIV(op)->op_state = SYNC_JOURNAL_SYNC_SENT;
            return 1;
        }
        else
        {
            PRIV(op)->op_state = SYNC_DONE;
        }
    }
    if (PRIV(op)->op_state == SYNC_DONE && !queue_has_in_progress_sync)
    {
        ack_sync(op);
        return 2;
    }
    return 1;
}

void blockstore_impl_t::handle_sync_event(ring_data_t *data, blockstore_op_t *op)
{
    live = true;
    if (data->res != data->iov.iov_len)
    {
        throw std::runtime_error(
            "write operation failed ("+std::to_string(data->res)+" != "+std::to_string(data->iov.iov_len)+
            "). in-memory state is corrupted. AAAAAAAaaaaaaaaa!!!111"
        );
    }
    PRIV(op)->pending_ops--;
    if (PRIV(op)->pending_ops == 0)
    {
        // Release used journal sectors
        release_journal_sectors(op);
        // Handle states
        if (PRIV(op)->op_state == SYNC_DATA_SYNC_SENT)
        {
            PRIV(op)->op_state = SYNC_DATA_SYNC_DONE;
        }
        else if (PRIV(op)->op_state == SYNC_JOURNAL_WRITE_SENT)
        {
            PRIV(op)->op_state = SYNC_JOURNAL_WRITE_DONE;
        }
        else if (PRIV(op)->op_state == SYNC_JOURNAL_SYNC_SENT)
        {
            PRIV(op)->op_state = SYNC_DONE;
        }
        else
        {
            throw std::runtime_error("BUG: unexpected sync op state");
        }
        ringloop->wakeup();
    }
}

void blockstore_impl_t::ack_sync(blockstore_op_t *op)
{
    // Handle states
    for (auto it = PRIV(op)->sync_big_writes.begin(); it != PRIV(op)->sync_big_writes.end(); it++)
    {
#ifdef BLOCKSTORE_DEBUG
        printf("Ack sync big %lx:%lx v%lu\n", it->oid.inode, it->oid.stripe, it->version);
#endif
        auto & unstab = unstable_writes[it->oid];
        unstab = unstab < it->version ? it->version : unstab;
        auto dirty_it = dirty_db.find(*it);
        dirty_it->second.state = ((dirty_it->second.state & ~BS_ST_WORKFLOW_MASK) | BS_ST_SYNCED);
        if (dirty_it->second.state & BS_ST_INSTANT)
        {
            mark_stable(dirty_it->first);
        }
        dirty_it++;
        while (dirty_it != dirty_db.end() && dirty_it->first.oid == it->oid)
        {
            if ((dirty_it->second.state & BS_ST_WORKFLOW_MASK) == BS_ST_WAIT_BIG)
            {
                dirty_it->second.state = (dirty_it->second.state & ~BS_ST_WORKFLOW_MASK) | BS_ST_IN_FLIGHT;
            }
            dirty_it++;
        }
    }
    for (auto it = PRIV(op)->sync_small_writes.begin(); it != PRIV(op)->sync_small_writes.end(); it++)
    {
#ifdef BLOCKSTORE_DEBUG
        printf("Ack sync small %lx:%lx v%lu\n", it->oid.inode, it->oid.stripe, it->version);
#endif
        auto & unstab = unstable_writes[it->oid];
        unstab = unstab < it->version ? it->version : unstab;
        if (dirty_db[*it].state == (BS_ST_DELETE | BS_ST_WRITTEN))
        {
            dirty_db[*it].state = (BS_ST_DELETE | BS_ST_SYNCED);
            // Deletions are treated as immediately stable
            mark_stable(*it);
        }
        else /* (BS_ST_INSTANT?) | BS_ST_SMALL_WRITE | BS_ST_WRITTEN */
        {
            dirty_db[*it].state = (dirty_db[*it].state & ~BS_ST_WORKFLOW_MASK) | BS_ST_SYNCED;
            if (dirty_db[*it].state & BS_ST_INSTANT)
            {
                mark_stable(*it);
            }
        }
    }
    op->retval = 0;
    FINISH_OP(op);
}
