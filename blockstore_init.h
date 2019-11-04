#pragma once

class blockstore_init_meta
{
    blockstore *bs;
    uint8_t *metadata_buffer = NULL;
    uint64_t metadata_read = 0;
    int prev = 0, prev_done = 0, done_len = 0, submitted = 0, done_cnt = 0;
    void handle_entries(struct clean_disk_entry* entries, int count);
public:
    blockstore_init_meta(blockstore *bs);
    void handle_event(ring_data_t *data);
    int loop();
};

class blockstore_init_journal
{
    blockstore *bs;
    uint8_t *journal_buffer = NULL;
    int step = 0;
    uint32_t crc32_last = 0;
    uint64_t done_pos = 0, journal_pos = 0;
    uint64_t cur_skip = 0;
    bool wrapped = false;
    int submitted = 0, done_buf = 0, done_len = 0;
    int handle_journal_part(void *buf, uint64_t len);
public:
    blockstore_init_journal(blockstore* bs);
    void handle_event(ring_data_t *data);
    int loop();
};
