#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include <stdbool.h>
#include "devices/block.h"
#include "threads/synch.h"
#include "off_t.h"

#define CACHE_NUM_SECTORS 64
enum cache_state
  {
    CACHE_READY,
    CACHE_PENDING_WRITE,
    CACHE_BEING_WRITTEN,
    CACHE_BEING_READ,
    CACHE_EVICTED
  };

/*TODO(kenny): reevaluate*/
enum cache_info_bit
  {
    CLEAN = 0x0,
    ACCESSED = 0x01,
    DIRTY = 0x02,
    META = 0x04
  };

struct cache_block 
  {
    uint8_t buffer[BLOCK_SECTOR_SIZE];
    int num_accessors;
    block_sector_t sector_idx;
    bool is_metadata;
    struct lock lock;
    enum cache_info_bit dirty_bit;
    enum cache_state state;
    struct condition being_accessed;
    struct condition being_read;
    struct condition being_written;
  };

void cache_init (void);
void cache_io_at (block_sector_t sector_idx, void *buffer,
                  bool is_metadata, off_t offset, off_t size, bool is_write);
#endif /* filesys/cache.h */
