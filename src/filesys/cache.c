#include "cache.h"
#include <string.h>
#include "inode.h"
#include "debug.h"
#include "filesys.h"
#include "devices/timer.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "threads/malloc.h"

#define TIME_BETWEEN_FLUSH 30000
/*
 * Wrapper struct to add next sectors to list of sectors to read in background. 
 */
struct async_sector_wrapper 
  {
    block_sector_t sector_idx;
    struct list_elem elem;
  };

/* Cache table*/
static struct cache_sector cache[CACHE_NUM_SECTORS];
static struct lock clock_lock; /* Lock to ensure only one instance of the clock
                                  algorithm runs*/
static struct lock async_read_lock;
static struct condition async_list_populated;
static struct list async_read_list; /* List of sectors to be cached in back*/
static int clock_hand;


/* Private helper functions declarations and definitions.*/
static thread_func read_asynchronously;
static thread_func async_flush;
struct cache_sector *get_sector (block_sector_t sector_idx, bool is_metadata);
struct cache_sector *sector_lookup (block_sector_t sector_idx);
struct cache_sector* cache_sector_at (block_sector_t sector_idx, bool is_metadata);
struct cache_sector* pick_and_evict (void);
void write_to_disk (struct cache_sector *sect, bool wait);
void read_from_disk (block_sector_t sector_idx, struct cache_sector *sector, 
                     bool is_metadata);
static void read_ahead (block_sector_t sector_idx);

/* A thread function that asynchronously reads from  disk sectors added to 
 * async_read_list. */
static void
read_asynchronously (void *aux UNUSED)
{
  struct list side_piece;
  struct async_sector_wrapper *a;
  list_init (&side_piece);

  for (;;)
    {
      lock_acquire (&async_read_lock);
      /* Wait for list to be populated */
      while (list_empty (&async_read_list))
        cond_wait (&async_list_populated, &async_read_lock);

      while (!list_empty (&async_read_list))
        list_push_front (&side_piece, list_pop_back (&async_read_list));

      lock_release (&async_read_lock);

      while (!list_empty (&side_piece))
        {
          struct list_elem *e = list_pop_front (&side_piece);
          a = list_entry (e, struct async_sector_wrapper, elem);
          struct cache_sector *s = get_sector (a->sector_idx, false);

          lock_acquire (&s->lock);
          s->num_accessors--;
          if (s->num_accessors == 0)
            cond_broadcast(&s->being_accessed, &s->lock);
          lock_release (&s->lock);
          free (a);
        }
    }
}

static void
async_flush (void *aux UNUSED)
{
  for (;;)
    {
      timer_msleep (TIME_BETWEEN_FLUSH);
      for (int i = 0; i < CACHE_NUM_SECTORS; ++i)
        {
          lock_acquire (&cache[i].lock);
          write_to_disk (&cache[i], false);
          lock_release (&cache[i].lock);
        }
    }
}

/* This function finds the next eligible cache sector to evict and returns it,
 * claiming it's lock first
 *
 * NOTE: The caller of this function is responsible for releasing the lock of 
 * the cache sector returned when it is done using it. */
struct cache_sector*
pick_and_evict ()
{
  /* Critical section so thread A doesn't evict the cache sector thread B wants
   * to evict before thread B is able to set said cache sector's state to evicted. */
  lock_acquire (&clock_lock);
  int clock_start = (++clock_hand) % CACHE_NUM_SECTORS;
  struct cache_sector *cand = &cache[clock_start];

  while (cand->state != CACHE_READY)
    {
      clock_start = (++clock_hand) % CACHE_NUM_SECTORS;
      if (clock_hand == clock_start)
        PANIC("No READY cache sector found to evict"); //TODO(kenny): reevaluate
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

/* This buffer writes to disk the contents of the buffer of cache sector SECT 
 *
 * NOTE: the caller of this function must hold the lock to SECT 
 */
void
write_to_disk (struct cache_sector *sect, bool wait)
{
  /* Writing a sector to disk is a critical section, no other thread should 
   * access this sector while it is being written to disk. */
  ASSERT (lock_held_by_current_thread(&sect->lock));
  // TODO(kenny): unsure of this for now, is it possible for write_to_disk
  // to be called on a disk that's being read from disk.
  ASSERT (sect->state != CACHE_BEING_READ);
  if (!(sect->dirty_bit & DIRTY))
    return;
  if (sect->state == CACHE_READY || sect->state == CACHE_EVICTED)
    {
      enum cache_state og_state = sect->state;
      /* We don't want to write to disk until accessors have finished making
       * their changes. */
      // TODO(kenny) : still unsure if we need pending write
      sect->state = CACHE_PENDING_WRITE;
      while (sect->num_accessors > 0)
        cond_wait(&sect->being_accessed, &sect->lock);

      sect->state = CACHE_BEING_WRITTEN;
      // TODO(kenny) maybe release lock here 
      ASSERT (sect->sector_idx != INODE_INVALID_SECTOR);
      block_write (fs_device, sect->sector_idx, sect->buffer);
      
      // maybe reclaim lock here
      sect->dirty_bit &= ~DIRTY;
      sect->state = og_state;
      cond_broadcast (&sect->being_written, &sect->lock);
    }
  else if (wait)
    {
      /*If this cache sector is already being written to disk, it's not our problem,
       * wait til it's finished then return */
      while (sect->state == CACHE_BEING_WRITTEN ||
          sect->state == CACHE_PENDING_WRITE)
        cond_wait (&sect->being_written, &sect->lock);
    }
}

/* This function reads the content of the sector at SECTOR_IDX into the 
 * buffer of SECT and updates SECT to reflect that it holds said sector
 *
 * NOTE: The caller of this function must be holding the lock to SECT 
 */
void
read_from_disk (block_sector_t sector_idx, struct cache_sector *sect,
    bool is_metadata)
{
  ASSERT (sect->state != CACHE_READY)
  /* We don't want to ovewrite what accessors are reading */
  while (sect->num_accessors > 0)
    cond_wait (&sect->being_accessed, &sect->lock);

  ASSERT (sect->num_accessors == 0);

  sect->state = CACHE_BEING_READ;
  sect->sector_idx = sector_idx;
  sect->dirty_bit = CLEAN;
  sect->is_metadata = is_metadata;

  // TODO(kenny) maybe release lock here 
  ASSERT (sect->sector_idx != INODE_INVALID_SECTOR);
  block_read (fs_device, sector_idx, sect->buffer);
  // TODO(kenny) maybe reclaim lock here 
  
  sect->state = CACHE_READY;
  cond_broadcast (&sect->being_read, &sect->lock);
}

/* This function caches the sector at SECTOR_IDX, evicting a cache sector if 
 * necessary
 *
 * NOTE: The caller of this function is responsible for decrementing the
 * NUM_ACCESSORS of the cache sector returned when it is done using it. */
struct cache_sector*
cache_sector_at (block_sector_t sector_idx,  bool is_metadata)
{
  struct cache_sector *sect = pick_and_evict();
  write_to_disk (sect, true);

  // read from disk
  read_from_disk (sector_idx, sect, is_metadata);
  sect->num_accessors++;
  lock_release (&sect->lock);

  return sect;
}

/* This function checks if there exists a ready cache sect associated with
 * the sector at SECTOR_IDX, returns it if it exists, NULL otherwise. 
 * 
 * NOTE: The caller of this function is responsible for decrementing the
 * NUM_ACCESSORS of the cache sector returned when it is done using it. */
struct cache_sector*
sector_lookup (block_sector_t sector_idx)
{
  for (int i = 0; i < CACHE_NUM_SECTORS; ++i)
    {
      struct cache_sector *cand = &cache[i];
      if (cand->sector_idx == sector_idx)
        {
          /* A cache sector that is not ready or actively being read has been evicted*/
          if (cand->state != CACHE_READY && cand->state != CACHE_BEING_READ)
            continue;
          /* Critical point so this cahce sector's lock isn't 
           * evicted before we declare we're accessing it*/
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
              /* This cache sector was replaced between our finding it and the
               * acquirance of its lock*/
              lock_release (&cand->lock);
              return NULL;
            }

        }
    }
    return NULL;
}

/* This function returns a cache sector that holds disk sector at sector_idx
 * If such a sector does not exist, it caches it. */
struct cache_sector*
get_sector (block_sector_t sector_idx, bool is_metadata)
{
  struct cache_sector *sect = sector_lookup (sector_idx);
  if (sect == NULL)
    sect = cache_sector_at (sector_idx, is_metadata);

  lock_acquire (&sect->lock);
  sect->dirty_bit |= ACCESSED;
  if (is_metadata)
    sect->dirty_bit |= META;
  lock_release (&sect->lock);
  return sect;
}

/* This function adds sector_idx to the list of sectors to be read in
 * the background. */
static void
read_ahead (block_sector_t sector_idx)
{
  if (sector_idx == INODE_INVALID_SECTOR) return;

  lock_acquire (&async_read_lock);
  struct async_sector_wrapper *a = malloc (sizeof (struct async_sector_wrapper));
  if (a != NULL)
    {
      a->sector_idx = sector_idx;
      list_push_back (&async_read_list, &a->elem);
      cond_broadcast (&async_list_populated, &async_read_lock);
    }
  lock_release (&async_read_lock);

}

/* Performs IO of SIZE bytes between cache sector for dist sector at SECTOR_IDX 
 * and buffer BUFFER. caches the disk sector if it is not already cached 
 * returns the number of bytes it successfully IOs */
void
cache_io_at (block_sector_t sector_idx, void *buffer, 
    bool is_metadata, off_t offset, off_t size, bool is_write)
{
  struct cache_sector *sect = get_sector (sector_idx, is_metadata);

  ASSERT (offset + size <= BLOCK_SECTOR_SIZE);
  ASSERT (sect->state == CACHE_READY);
  ASSERT (sect->sector_idx == sector_idx);
  ASSERT (sect->num_accessors > 0);

  if (!is_write)
    memcpy (buffer, sect->buffer + offset, size);
  else
    {
      sect->dirty_bit |= DIRTY;
      memcpy (sect->buffer + offset, buffer, size);
    }

  lock_acquire (&sect->lock);
  sect->num_accessors--;
  if (sect->num_accessors == 0)
    cond_broadcast(&sect->being_accessed, &sect->lock);
  lock_release (&sect->lock);
  return;
}

/* This function solely serves to ensure backward compatibility with existing code*/
void
cache_io_at_ (block_sector_t sector_idx, void *buffer, bool is_metadata,
              off_t offset, off_t size, bool is_write,
              block_sector_t sector_next)
{
  cache_io_at (sector_idx, buffer, is_metadata, offset, size, is_write);
  read_ahead (sector_next);
}
/* Writes all dirty cache sectors to disk */
void
cache_write_all (void)
{
  for (int i = 0; i < CACHE_NUM_SECTORS; ++i)
    {
      lock_acquire (&cache[i].lock);
      write_to_disk (&cache[i], true);
      lock_release (&cache[i].lock);
    }
}

bool
cache_init (void)
{
  clock_hand = CACHE_NUM_SECTORS - 1;
  lock_init (&clock_lock);
  lock_init (&async_read_lock);
  cond_init (&async_list_populated);
  list_init (&async_read_list);

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

  if (thread_create ("cache_async_read", PRI_DEFAULT, read_asynchronously, NULL)
      == TID_ERROR) return false;

  if (thread_create ("cache_async_write", PRI_DEFAULT, async_flush, NULL)
      == TID_ERROR) return false;

  return true;
}
