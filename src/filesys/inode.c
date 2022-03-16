#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "filesys/cache.h"
#include "threads/malloc.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* Indexed Inodes Constants */
// Number of Blocks
#define INODE_NUM_BLOCKS 125
#define INODE_NUM_DIRECT 123
#define INODE_IND_IDX INODE_NUM_DIRECT
#define INODE_DUB_IND_IDX INODE_NUM_BLOCKS - 1
#define INODE_NUM_IN_IND_BLOCK 128

static char ZEROARRAY[BLOCK_SECTOR_SIZE];

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct lock open_inodes_lock;
static struct list open_inodes;

/* On-disk inode for indirect sector
 * Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_indirect_sector
  {
    block_sector_t block_idxs[INODE_NUM_IN_IND_BLOCK];
  };

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    block_sector_t block_idxs [INODE_NUM_BLOCKS];
    bool is_dir;
    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */
  };

static block_sector_t get_index (const struct inode_disk*, off_t);
static struct inode_disk *get_data_at (block_sector_t);
static bool inode_expand (struct inode_disk*, off_t);
static bool inode_expand_helper (block_sector_t*, off_t, int);
static bool inode_clear (struct inode*);
static void inode_clear_helper (block_sector_t, off_t, int);

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode 
  {
    struct lock lock;                   /* Guards the inode. */
    struct lock eof_lock;               /* Lock to make read past EOF atomic*/
    struct condition data_loaded_cond;  /* Wait to load data on open. */
    bool data_loaded;                   /* If the inode is usable. */
    struct lock dir_lock;               /* Lock for directory synch. */
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    bool is_dir;                        /* Whether this inode is dir or not*/
    off_t length;                       /* File size in bytes. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
  };

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. 
   LENGTH is the size of INODE's data. This could be different from
   the length stored on disk while expanding the inode. */
static block_sector_t
byte_to_sector (const struct inode_disk *disk_inode, off_t pos, off_t length) 
{
  ASSERT (disk_inode != NULL);
  if (pos >= 0 && pos < length)
    {
      off_t abs_idx =  pos / BLOCK_SECTOR_SIZE;
      return get_index (disk_inode, abs_idx);
    }
  else
    return INODE_INVALID_SECTOR;
}

/* Initializes the inode module. */
void
inode_init (void) 
{
  lock_init (&open_inodes_lock);
  list_init (&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length, bool isdir)
{
  struct inode_disk *t_disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *t_disk_inode == BLOCK_SECTOR_SIZE);

  t_disk_inode = calloc (1, sizeof(struct inode_disk));
  if (t_disk_inode != NULL)
    {
      t_disk_inode->length = length;
      t_disk_inode->magic = INODE_MAGIC;
      t_disk_inode->is_dir = isdir;
      memset (&t_disk_inode->block_idxs, INODE_INVALID_SECTOR,
              INODE_NUM_BLOCKS * sizeof(block_sector_t));
      if (!inode_expand (t_disk_inode, length))
        success = false;
      else
        {
          cache_io_at (sector, t_disk_inode, true, 0, BLOCK_SECTOR_SIZE, true);
          success = true; 
        } 
    }
  free (t_disk_inode);
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  lock_acquire (&open_inodes_lock);
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          lock_release (&open_inodes_lock);
          return inode_reopen (inode);
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    {
      lock_release (&open_inodes_lock);
      return NULL;
    }

  /* Initialize. */
  lock_init (&inode->lock);
  lock_init (&inode->eof_lock);
  cond_init (&inode->data_loaded_cond);
  lock_init (&inode->dir_lock);
  lock_acquire (&inode->lock);
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  inode->data_loaded = false;
  lock_release (&inode->lock);
  lock_release (&open_inodes_lock);

  /* Lazily load needed inode data from disk. */
  lock_acquire (&inode->lock);
  struct inode_disk *disk_inode = get_data_at (inode->sector);
  cache_io_at (inode->sector, disk_inode, true, 0, sizeof(struct inode_disk),
               false);
  inode->is_dir = disk_inode->is_dir;
  inode->length = disk_inode->length;
  /* Broadcast the fact that the inode has been fully loaded. */
  inode->data_loaded = true;
  cond_broadcast (&inode->data_loaded_cond, &inode->lock);
  lock_release (&inode->lock);
  free (disk_inode);
  return inode;
}

/* Reopens and returns INODE after acquiring and releasing its lock. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode == NULL)
    return NULL;
  /* We know that INODE will not be free'd because we have it open. */
  lock_acquire (&inode->lock);
  if (inode->removed)
    {
      lock_release (&inode->lock);
      return NULL;
    }
  inode->open_cnt++;
  /* Wait until the data loads. */
  while (!inode->data_loaded)
    cond_wait (&inode->data_loaded_cond, &inode->lock);
  lock_release (&inode->lock);
  return inode;
}

/* Returns the number of open instances of INODE. */
int 
inode_open_count (struct inode *inode)
{
  int count;
  lock_acquire (&inode->lock);
  count = inode->open_cnt;
  lock_release (&inode->lock);
  return count;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Returns a pointer to the directory lock. */
struct lock *
inode_dir_lock (struct inode *inode)
{
  return &inode->dir_lock;
}

/* Marks free all sectors related to this inode. */
static bool
inode_clear (struct inode* inode)
{
  struct inode_disk *disk_inode = get_data_at (inode->sector);
  if (inode->length < 0) return false;

  int num_sectors_left = bytes_to_sectors (inode->length);

  // Clear direct blocks
  int num_direct = (num_sectors_left < INODE_NUM_DIRECT) ?
                    num_sectors_left : INODE_NUM_DIRECT;
  for (int i = 0; i < num_direct; ++i)
    {
      free_map_release (disk_inode->block_idxs[i], 1);
    }
  num_sectors_left -= num_direct;

  // Free indirect blocks
  int num_indirect = (num_sectors_left <  INODE_NUM_IN_IND_BLOCK) ?
    num_sectors_left : INODE_NUM_IN_IND_BLOCK;
  if (num_indirect > 0)
  {
    inode_clear_helper (disk_inode->block_idxs[INODE_IND_IDX], num_indirect, 1);
    num_sectors_left -= num_indirect;
  }

  // Free doubly indirect blocks
  off_t oRet = INODE_NUM_IN_IND_BLOCK * INODE_NUM_IN_IND_BLOCK;
  num_indirect = (num_sectors_left <  oRet) ?  num_sectors_left : oRet;
  if (num_indirect > 0)
    {
      inode_clear_helper (disk_inode->block_idxs[INODE_DUB_IND_IDX], num_indirect, 1);
      num_sectors_left -= num_indirect;
    }

  ASSERT (num_sectors_left == 0);
  free (disk_inode);
  return true;
}

static void
inode_clear_helper (block_sector_t idx, off_t num_sectors, int level)
{
  if (level != 0) 
    {
      struct inode_indirect_sector indirect_block; 
      cache_io_at (idx, &indirect_block, true, 0, BLOCK_SECTOR_SIZE, false);

      off_t base = (level == 1 ? 1 : INODE_NUM_IN_IND_BLOCK);
      off_t n = DIV_ROUND_UP (num_sectors, base);
      for (off_t i = 0; i < n; ++i) 
        {
          off_t num_in_level = num_sectors < base? num_sectors : base;
          inode_clear_helper (indirect_block.block_idxs[i], num_in_level, level - 1);
          num_sectors -= num_in_level;
        }
    }
  free_map_release (idx, 1);
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  bool last_instance;

  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Decrement the open count and find out if this is the last instance. */
  lock_acquire (&open_inodes_lock);
  lock_acquire (&inode->lock);
  last_instance = --inode->open_cnt == 0;
  if (last_instance)
    {
      list_remove (&inode->elem);
      if (inode->removed)
        {
          free_map_release (inode->sector, 1);
          inode_clear (inode);
        }
    }
  lock_release (&inode->lock);
  lock_release (&open_inodes_lock);

  /* If this is the last instance, then we own it and can free it. */
  if (last_instance)
    free (inode); 
}

/* Returns true if INODE represents a directory not a file. */
bool
inode_isdir (const struct inode *inode)
{ 
  /* No need to acquire the lock because this member is read-only on init. */
  return inode->is_dir;
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  lock_acquire (&inode->lock);
  inode->removed = true;
  lock_release (&inode->lock);
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  off_t inode_len = inode_length (inode);

  struct inode_disk *disk_inode = get_data_at (inode->sector);
  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (disk_inode, offset, inode_len);
      if (sector_idx == INODE_INVALID_SECTOR) break;
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_len - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      block_sector_t sector_next = byte_to_sector (disk_inode, offset + chunk_size, 
                                                   inode_len);
      if (sector_next == sector_idx) sector_next = INODE_INVALID_SECTOR;
      cache_io_at_ (sector_idx, buffer + bytes_read, false, sector_ofs,
                   chunk_size, false, sector_next);
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }

  free (disk_inode);
  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  off_t length_after_write;
  bool expand_write = false;

  if (inode->deny_write_cnt)
    return 0;

  struct inode_disk *disk_inode = get_data_at (inode->sector);
  length_after_write = inode_length (inode);

  /* Expand if write will go past end of file. */
  // Writes past EOF are atomic, claim lock if it is a write past EOF
  expand_write = (offset + size) > length_after_write;
  if (expand_write)
    lock_acquire (&inode->eof_lock);

  /*  We reach here when we aren't writing past file or if we are, we
   *  are currently the only thread writing past EOF 
   *  (we claimed the lock successfully)
   *  So check to see noone else expanded while we were waiting
   */
  if (byte_to_sector (disk_inode, offset + size - 1, length_after_write) ==
      INODE_INVALID_SECTOR)
    {
      if (!inode_expand (disk_inode, offset + size))
        {
          lock_release (&inode->eof_lock);
          free (disk_inode);
          return 0;  /* Failed to expand the inode. */
        }
    }
  else if (expand_write)
    {
      //While we where waiting for lock someone else expanded the file
      expand_write = false;
      lock_release (&inode->eof_lock);
    }
  /* Use the new size while writing.*/
  if (expand_write)
    length_after_write = offset + size;  
  
  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (disk_inode, offset,
                                                  length_after_write);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = length_after_write - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      block_sector_t sector_next = byte_to_sector (disk_inode, offset + chunk_size,
                                                   length_after_write);
      if (sector_next == sector_idx) sector_next = INODE_INVALID_SECTOR;
      cache_io_at_ (sector_idx, (void*) buffer + bytes_written, false,
                    sector_ofs, chunk_size, true, sector_next);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  /* Update inode to reflect new size only after writing everything. */
  /* Only update the size if it increases. Another process could have
     increased the size further already so don't overwrite it. */
  if (expand_write)
    {
      inode->length = length_after_write;
      disk_inode->length = length_after_write;
      lock_release (&inode->eof_lock);
      /* Flush the changes to cache. */
      cache_io_at (inode->sector, disk_inode, true, 0, BLOCK_SECTOR_SIZE, true);
    }
  free (disk_inode);
  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  lock_acquire (&inode->lock);
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  lock_release (&inode->lock);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  lock_acquire (&inode->lock);
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
  lock_release (&inode->lock);
}

/* Returns the length, in bytes, of INODE's data.
   INODE could potentially expand in length later and that's okay. */
off_t
inode_length (struct inode *inode)
{
  off_t length;
  lock_acquire (&inode->lock);
  length = inode->length;
  lock_release (&inode->lock);
  return length;
}

static block_sector_t
get_index (const struct inode_disk *disk_inode, off_t abs_idx)
{
  struct inode_indirect_sector *sect;
  block_sector_t idx = INODE_INVALID_SECTOR;
  if (abs_idx < INODE_NUM_DIRECT)
    {
        idx = disk_inode->block_idxs[abs_idx];
    }
  else if (abs_idx < (INODE_NUM_DIRECT + INODE_NUM_IN_IND_BLOCK))
    {
      sect = calloc (1, sizeof (struct inode_indirect_sector));
      if (sect != NULL)
        {
          cache_io_at (disk_inode->block_idxs[INODE_IND_IDX], sect, true, 0,
                       BLOCK_SECTOR_SIZE, false);
          idx = sect->block_idxs [abs_idx - INODE_NUM_DIRECT];
          free (sect);
        }
    }
  else if (abs_idx < (INODE_NUM_DIRECT + INODE_NUM_IN_IND_BLOCK) + 
      INODE_NUM_IN_IND_BLOCK * INODE_NUM_IN_IND_BLOCK)
    {
      off_t start = abs_idx - (INODE_NUM_DIRECT + INODE_NUM_IN_IND_BLOCK);
      off_t outer_idx = start / INODE_NUM_IN_IND_BLOCK;
      off_t inner_idx = start % INODE_NUM_IN_IND_BLOCK;

      sect = calloc (1, sizeof (struct inode_indirect_sector));
      if (sect != NULL)
        {
          cache_io_at (disk_inode->block_idxs[INODE_DUB_IND_IDX], sect, true, 0,
                       BLOCK_SECTOR_SIZE, false);
          cache_io_at (sect->block_idxs[outer_idx], sect, true, 0, BLOCK_SECTOR_SIZE,
                       false);
          idx = sect->block_idxs[inner_idx];
          free (sect);
        }
    }

  return idx;
}

/* Reads the sector data for inode from disk and returns it.
 *
 * NOTE: user is responsible for freeing the returned bufer after using it.
 */
static struct inode_disk *
get_data_at (block_sector_t sector_idx)
{
  struct inode_disk *ret_disk_inode = calloc (1, sizeof (struct inode_disk)); 
  cache_io_at (sector_idx, ret_disk_inode, true, 0, BLOCK_SECTOR_SIZE, false);
  return ret_disk_inode;
}

/* Expand inode so it has enough sectors to hold a file of size NEW_SIZE.
   Returns true on success and false on error. */
static bool
inode_expand (struct inode_disk *disk_inode, off_t new_size)
{
  if (new_size < 0) return false;

  int num_sectors_left = bytes_to_sectors (new_size);

  // Fill in direct blocks
  int num_direct = (num_sectors_left < INODE_NUM_DIRECT) ?
                    num_sectors_left : INODE_NUM_DIRECT;
  for (int i = 0; i < num_direct; ++i)
    {
      block_sector_t *cand =  &disk_inode->block_idxs[i];
      if (*cand == INODE_INVALID_SECTOR)
        {
          if (!free_map_allocate (1, cand))
            {
              return false;
            }
          cache_io_at (*cand, ZEROARRAY, false, 0, BLOCK_SECTOR_SIZE, true);
        }
    }
  num_sectors_left -= num_direct;
  if (num_sectors_left == 0) return true;

  // Fill in indirect blocks
  int num_indirect = (num_sectors_left <  INODE_NUM_IN_IND_BLOCK) ?
    num_sectors_left : INODE_NUM_IN_IND_BLOCK;
  bool bRet = inode_expand_helper (&disk_inode->block_idxs[INODE_IND_IDX],
                                   num_indirect, 1);
  if (!bRet) return false;
  num_sectors_left -= num_indirect;
  if(num_sectors_left == 0) return true;

  // Fill in doubly indirect blocks
  off_t oRet = INODE_NUM_IN_IND_BLOCK * INODE_NUM_IN_IND_BLOCK;
  num_indirect = (num_sectors_left <  oRet) ?  num_sectors_left : oRet;
  bRet = inode_expand_helper (&disk_inode->block_idxs[INODE_DUB_IND_IDX], 
                              num_indirect, 2);
  if (!bRet) return false;
  num_sectors_left -= num_indirect;
  if(num_sectors_left == 0) return true;
  return false;
}

static bool
inode_expand_helper (block_sector_t *idx, off_t num_sectors_left, int level)
{
  if (level == 0) {
    if (*idx == 0)
      {
        if (!free_map_allocate (1, idx)) 
          {
            return false;
          }
        cache_io_at (*idx, ZEROARRAY, false, 0, BLOCK_SECTOR_SIZE, true);
      }
    return true;
  }

  struct inode_indirect_sector indirect_block;
  if(*idx == INODE_INVALID_SECTOR || *idx == 0) 
  {
    if (!free_map_allocate (1, idx)) 
      {
        return false;
      }
    cache_io_at (*idx, ZEROARRAY, true, 0, BLOCK_SECTOR_SIZE, true);
  } 
  cache_io_at (*idx, &indirect_block, true, 0, BLOCK_SECTOR_SIZE, false);

  off_t base = (level == 1 ? 1 : INODE_NUM_IN_IND_BLOCK);
  off_t n = DIV_ROUND_UP (num_sectors_left, base);
  for (off_t i = 0; i < n; ++i) 
    {
      off_t num_in_level = (num_sectors_left <  base) ? num_sectors_left : base;
      bool bRet = inode_expand_helper (&indirect_block.block_idxs[i], 
                                       num_in_level, level - 1);
      if (!bRet) return false;
      num_sectors_left -= num_in_level;
    }

  cache_io_at (*idx, &indirect_block, true, 0, BLOCK_SECTOR_SIZE, true);
  return true;
}
