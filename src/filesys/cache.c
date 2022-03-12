#include "cache.h"
#include <string.h>
#include "threads/synch.h"
#include "inode.h"
#include "debug.h"
#include "filesys.h"

/* Lock to ensure only one instance of the clock algorithm runs*/
static struct lock clock_lock; 
/* Cache table*/
static struct cache_block cache[CACHE_NUM_SECTORS];
static int clock_hand;

/* Private helper functions declarations and definitions.*/
struct cache_block *get_sector (block_sector_t sector_idx, bool is_metadata);
struct cache_block *block_lookup (block_sector_t sector_idx);
struct cache_block* cache_sector (block_sector_t sector_idx, bool is_metadata);
struct cache_block* pick_and_evict (void);
void write_to_disk (struct cache_block *block);
void read_from_disk (block_sector_t sector_idx, struct cache_block *block, 
                      bool is_metadata);

/* This function finds the next eligible block to evict and returns it, claiming
 * it's lock first
 *
 * NOTE: The caller of this function is responsible for releasing the lock of 
 * the block returned when it is done using it. */
struct cache_block*
pick_and_evict ()
{
  /* Critical section so thread A doesn't evict the block thread B wants
   * to evict before thread B is able to set said block's state to evicted. */
  lock_acquire (&clock_lock);
  int clock_start = (++clock_hand) % CACHE_NUM_SECTORS;
  struct cache_block *cand = &cache[clock_start];

  while (cand->state != CACHE_READY)
    {
      clock_start = (++clock_hand) % CACHE_NUM_SECTORS;
      if (clock_hand == clock_start)
        PANIC("No READY block found to evict"); //TODO(kenny): reevaluate
      cand = &cache[clock_start];
    }

  do
    {
      if (cand->state == CACHE_READY)
        {
          if (cand->dirty_bit & ACCESSED)
            cand->dirty_bit &= ~ACCESSED;
          else if (cand->dirty_bit & META)
            cand->dirty_bit &= ~META;
          else
            break;
        }
      cand = &cache[++clock_hand % CACHE_NUM_SECTORS];
    }
  while (clock_hand != clock_start);

  ASSERT (cand->state == CACHE_READY);
  lock_acquire (&cand->lock);
  cand->state = CACHE_EVICTED;
  while (cand->num_accessors > 0)
    cond_wait(&cand->being_accessed, &cand->lock);
  lock_release (&clock_lock);
  return cand;
}

/* This buffer writes to disk the contents of the buffer of cache block BLOCK
 *
 * NOTE: the caller of this function must hold the lock to block
 */
void
write_to_disk (struct cache_block *block)
{
  /* Writing a block to disk is a critical section, no other thread should 
   * access this block while it is being written to disk. */
  ASSERT (lock_held_by_current_thread(&block->lock));
  // TODO(kenny): unsure of this for now, is it possible for write_to_disk
  // to be called on a disk that's being read from disk.
  ASSERT (block->state != CACHE_BEING_READ);
  if (!(block->dirty_bit & DIRTY))
    return;
  if (block->state == CACHE_READY || block->state == CACHE_EVICTED)
    {
      enum cache_state og_state = block->state;
      /* We don't want to write to disk until accessors have finished making
       * their changes. */
      // TODO(kenny) : still unsure if we need pending write
      block->state = CACHE_PENDING_WRITE;
      while (block->num_accessors > 0)
        cond_wait(&block->being_accessed, &block->lock);

      block->state = CACHE_BEING_WRITTEN;
      // TODO(kenny) maybe release lock here 
      ASSERT (block->sector_idx != INODE_INVALID_SECTOR);
      block_write (fs_device, block->sector_idx, block->buffer);
      
      // maybe reclaim lock here
      block->dirty_bit &= ~DIRTY;
      block->state = og_state;
      cond_broadcast (&block->being_written, &block->lock);
    }
  else if (block->state == CACHE_BEING_WRITTEN ||
      block->state == CACHE_PENDING_WRITE)
    {
      /*If block is already being written to disk, it's not our problem,
       * wait til it's finished then return */
      while (block->state == CACHE_BEING_WRITTEN ||
          block->state == CACHE_PENDING_WRITE)
        cond_wait (&block->being_written, &block->lock);
    }
}

/* This function reads the content of the sector at SECTOR_IDX into the 
 * buffer of BLOCK and updates BLOCK to reflect that it holds said sector
 *
 * NOTE: The caller of this function must be holding the lock to BLOCK
 */
void
read_from_disk (block_sector_t sector_idx, struct cache_block *block,
    bool is_metadata)
{
  ASSERT (block->state != CACHE_READY)
  /* We don't want to ovewrite what accessors are reading */
  while (block->num_accessors > 0)
    cond_wait (&block->being_accessed, &block->lock);

  ASSERT (block->num_accessors == 0);

  block->state = CACHE_BEING_READ;
  block->sector_idx = sector_idx;
  block->dirty_bit = CLEAN;
  block->is_metadata = is_metadata;

  // TODO(kenny) maybe release lock here 
  ASSERT (block->sector_idx != INODE_INVALID_SECTOR);
  block_read (fs_device, sector_idx, block->buffer);
  // TODO(kenny) maybe reclaim lock here 
  
  block->state = CACHE_READY;
  cond_broadcast (&block->being_read, &block->lock);
}

/* This function caches the sector at SECTOR_IDX, evicting a block if necessary
 *
 * NOTE: The caller of this function is responsible for decrementing the
 * NUM_ACCESSORS of the block returned when it is done using it. */
struct cache_block*
cache_sector (block_sector_t sector_idx,  bool is_metadata)
{
  struct cache_block *block = pick_and_evict();
  write_to_disk (block);

  // read from disk
  read_from_disk (sector_idx, block, is_metadata);
  block->num_accessors++;
  lock_release (&block->lock);

  return block;
}

/* This function checks if there exists a ready cache block associated with
 * the sector at SECTOR_IDX, returns it if it exists, NULL otherwise. 
 * 
 * NOTE: The caller of this function is responsible for decrementing the
 * NUM_ACCESSORS of the block returned when it is done using it. */
struct cache_block*
block_lookup (block_sector_t sector_idx)
{
  for (int i = 0; i < CACHE_NUM_SECTORS; ++i)
    {
      struct cache_block *cand = &cache[i];
      if (cand->sector_idx == sector_idx)
        {
          /* A block that is not ready or actively being read has been evicted*/
          if (cand->state != CACHE_READY && cand->state != CACHE_BEING_READ)
            continue;
          /* Critical point so this block isn't evicted before we declare 
           * we're accessing it*/
          lock_acquire (&cand->lock);
          //if (cand->state == CACHE_BEING_READ)
          //  cond_wait (&cand->being_read, &cand->lock);
          if (cand->sector_idx == sector_idx)
            {
              ASSERT (cand->state == CACHE_READY);
              cand->num_accessors++;
              lock_release (&cand->lock);
              return cand;
            }
          else
            {
              /* This block was replaced between our finding it and the
               * acquirance of its lock*/
              lock_release (&cand->lock);
              return NULL;
            }

        }
    }
    return NULL;
}

/* This function returns a cache block that holds disk sector at sector_idx
 * If such a block does not exist, it caches it. */
struct cache_block*
get_sector (block_sector_t sector_idx, bool is_metadata)
{
  struct cache_block *block = block_lookup (sector_idx);
  if (block == NULL)
    block = cache_sector (sector_idx, is_metadata);

  lock_acquire (&block->lock);
  block->dirty_bit |= ACCESSED;
  if (is_metadata)
    block->dirty_bit |= META;
  lock_release (&block->lock);
  return block;
}

/* Performs IO of SIZE bytes between cache block for dist sector at SECTOR_IDX 
 * and buffer BUFFER. caches the disk sector if it is not already cached 
 * returns the number of bytes it successfully IOs */
void
cache_io_at (block_sector_t sector_idx, void *buffer, 
    bool is_metadata, off_t offset, off_t size, bool is_write)
{
  struct cache_block *block = get_sector (sector_idx, is_metadata);

  ASSERT (offset + size <= BLOCK_SECTOR_SIZE);
  ASSERT (block->state == CACHE_READY);
  ASSERT (block->sector_idx == sector_idx);
  ASSERT (block->num_accessors > 0);

  if (!is_write)
    memcpy (buffer, block->buffer + offset, size);
  else
    {
      block->dirty_bit |= DIRTY;
      memcpy (block->buffer + offset, buffer, size);
    }

  lock_acquire (&block->lock);
  block->num_accessors--;
  if (block->num_accessors == 0)
    cond_broadcast(&block->being_accessed, &block->lock);
  lock_release (&block->lock);
  return;
}

/* Writes all dirty blocks to disk */
void
cache_write_all (void)
{
  for (int i = 0; i < CACHE_NUM_SECTORS; ++i)
    {
      lock_acquire (&cache[i].lock);
      write_to_disk (&cache[i]);
      lock_release (&cache[i].lock);
    }
}

void
cache_init (void)
{
  lock_init (&clock_lock);
  clock_hand = CACHE_NUM_SECTORS - 1;
  for (int i = 0; i < CACHE_NUM_SECTORS; ++i)
    {
      cache[i].num_accessors = 0;
      cache[i].sector_idx = INODE_INVALID_SECTOR;
      cache[i].is_metadata = false;
      cache[i].dirty_bit = CLEAN;
      cache[i].state = CACHE_READY;
      lock_init(&cache[i].lock);
      cond_init (&cache[i].being_accessed);
      cond_init (&cache[i].being_read);
      cond_init (&cache[i].being_written);
    }
}
