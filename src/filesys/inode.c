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
#define INODE_NUM_DIRECT 122
#define INODE_NUM_INDIRECT 1
#define INODE_NUM_DUBINDIRECT 2
// Size of Blocks
#define INODE_DIRECT_SIZE (INODE_NUM_DIRECT * BLOCK_SECTOR_SIZE) // 62,464
#define INODE_INDIRECT_SIZE INODE_DIRECT_SIZE
#define INODE_DUBINDIRECT_SIZE (INODE_NUM_DIRECT * INODE_INDIRECT_SIZE)
        // 7,620,608
// Index of Blocks
#define INODE_INDIRECT_INDEX INODE_NUM_DIRECT
#define INODE_DUBINDIRECT_INDEX (INODE_NUM_DIRECT + INODE_NUM_INDIRECT)
// Byte offset of blocks
#define INODE_INDIRECT_OFFSET INODE_DIRECT_SIZE
#define INODE_DUBINDIRECT_OFFSET (INODE_DIRECT_SIZE + INODE_NUM_INDIRECT * INODE_INDIRECT_SIZE)

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    uint8_t unused[3];                  /* Padding. */
    bool is_dir;                        /* Directory status */
    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */
    uint32_t blocks[125];               /* Not used. */
  };

/* In-memory inode. */
struct inode
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    bool is_dir;                        /* Directory status */
    off_t length;                       /* Length of file */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok; >0: deny writes. */
    struct lock inode_lock;             /* Lock since inode is shared struct */
  };

/* Returns the byte offset in the file given the block index.
   Fascilitates index math when writing or reading.  */
static off_t
index_to_offset (int idx)
{
  return offsetof(struct inode_disk, blocks) + idx * sizeof (block_sector_t);
}

/* Allocates a new sector and returns the sector number */
static block_sector_t
alloc_sector (block_sector_t root_sector, int idx, bool is_meta)
{
  block_sector_t sector;
  if (!free_map_allocate (1, &sector))
    return INODE_INVALID_SECTOR;

  // Write sector number into root
  cache_io_at (root_sector, &sector, true, index_to_offset(idx),
               sizeof (block_sector_t), true);

  // Initialize buffer (may have to malloc)
  uint32_t fill = is_meta ? INODE_INVALID_SECTOR : 0;
  uint32_t buff[BLOCK_SECTOR_SIZE];
  for (uint32_t i = 0; i < BLOCK_SECTOR_SIZE / sizeof (uint32_t); i++)
    buff[i] = fill;
  // Write new sector into cache
  cache_io_at (sector, &buff, is_meta, 0, BLOCK_SECTOR_SIZE, true);

  return sector;
}

/* Reads the block sector from the block sector.
   Returns INODE_INVALID_SECTOR if not allocated */
static block_sector_t
read_block_from_sector (block_sector_t root_sector, int idx)
{
  if (root_sector == INODE_INVALID_SECTOR)
    return INODE_INVALID_SECTOR;

  block_sector_t sector;
  cache_io_at (root_sector, &sector, true, index_to_offset(idx),
               sizeof (block_sector_t), false);

  return sector;
}

/* Retrieves the block sector from the root sector.
   Creates a new sector if writing. */
static block_sector_t
retrieve_sector (block_sector_t root_sector, int idx, bool write, bool is_meta)
{
  if (root_sector == INODE_INVALID_SECTOR)
    return INODE_INVALID_SECTOR;

  block_sector_t sector = read_block_from_sector (root_sector, idx);

  if (sector == INODE_INVALID_SECTOR && write)
    sector = alloc_sector (root_sector, idx, is_meta);

  return sector;
}

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (struct inode *inode, off_t pos, bool write)
{
  ASSERT (inode != NULL);

  bool lock_held = lock_held_by_current_thread (&inode->inode_lock);
  if (!lock_held)
    lock_acquire (&inode->inode_lock);

  block_sector_t sector = INODE_INVALID_SECTOR;

  bool in_dubindirect = false;
  bool in_indirect = false;
  block_sector_t dubindirect_sector = INODE_INVALID_SECTOR;
  block_sector_t indirect_sector = INODE_INVALID_SECTOR;

  off_t rel_pos = pos;
  /* Check if in doubly indirect section */
  if (rel_pos >= INODE_DUBINDIRECT_OFFSET)
    {
      in_dubindirect = true;
      // Change rel_pos to now be relative to start of dubindirect blocks.
      rel_pos -= INODE_DUBINDIRECT_OFFSET;
      // Determine which doubly indirect block it is in
      int dubindirect_idx = INODE_DUBINDIRECT_INDEX
                            + rel_pos / INODE_DUBINDIRECT_SIZE; // 1 or 2
      dubindirect_sector = retrieve_sector (inode->sector, dubindirect_idx,
                                            write, true);
      // Use remainder to determine which indirect block in the dubindirect.
      rel_pos %= INODE_DUBINDIRECT_SIZE;
    }
  /* Check if in singly indirect section */
  if (in_dubindirect || rel_pos >= INODE_INDIRECT_OFFSET)
    {
      in_indirect = true;
      int indirect_idx = rel_pos / INODE_INDIRECT_SIZE; // 1 - 122
      if (!in_dubindirect)
        {
          // Change rel_pos to now be relative to start of indirect block.
          rel_pos -= INODE_INDIRECT_OFFSET;
          indirect_idx += INODE_INDIRECT_INDEX;
        }
      block_sector_t indirect_root = in_dubindirect ? dubindirect_sector :
                                     inode->sector;
      indirect_sector = retrieve_sector (indirect_root, indirect_idx, write,
                                        true);
      // Use remainder to determine which direct block in the indirect.
      rel_pos %= INODE_INDIRECT_SIZE;
    }
  /* Verify now in direct */
  if (rel_pos < INODE_DIRECT_SIZE)
    {
      int direct_idx = rel_pos / BLOCK_SECTOR_SIZE;
      block_sector_t direct_root = in_indirect ? indirect_sector :
                                   inode->sector;
      sector = retrieve_sector (direct_root, direct_idx, write, false);
    }

  if (!lock_held)
    lock_release (&inode->inode_lock);

  return sector;
}

/* Initializes the inode module. */
void
inode_init (void)
{
  list_init (&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector in the cache.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length, bool is_dir)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      disk_inode->length = length;
      disk_inode->is_dir = is_dir;
      disk_inode->magic = INODE_MAGIC;

      // Initialize all sectors to unallocated (@KENNY)
      memset(&disk_inode->blocks, INODE_INVALID_SECTOR,
                        INODE_NUM_BLOCKS * sizeof(block_sector_t));
      // Write new inode to cache instead of directly to disk
      cache_io_at (sector, disk_inode, true, 0, BLOCK_SECTOR_SIZE, true);
      success = true;

      free (disk_inode);
    }
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
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e))
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector)
        {
          inode_reopen (inode);
          return inode;
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  // Read in length and is_dir
  cache_io_at (inode->sector, &inode->is_dir, true, 0,
               sizeof(bool) + sizeof(off_t), false);
  lock_init (&inode->inode_lock);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    {
      lock_acquire (&inode->inode_lock);
      inode->open_cnt++;
      lock_release (&inode->inode_lock);
    }
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

static void
inode_close_direct_helper (block_sector_t root_sector, off_t *processed,
                           off_t length)
{
  if (root_sector != INODE_INVALID_SECTOR)
    {
      int idx = 0;
      block_sector_t sector;
      while (*processed < length && idx < INODE_NUM_DIRECT)
        {
          sector = read_block_from_sector (root_sector, idx);
          if (sector != INODE_INVALID_SECTOR)
            free_map_release (sector, 1);

          idx++;
          *processed += BLOCK_SECTOR_SIZE;
        }
    }
}

static void
inode_close_indirect_helper (block_sector_t root_sector, int idx,
                             off_t *processed, off_t length)
{
  block_sector_t indirect_sector = read_block_from_sector
                                              (root_sector, idx);
  inode_close_direct_helper (indirect_sector, processed, length);
  if (indirect_sector != INODE_INVALID_SECTOR)
    free_map_release (indirect_sector, 1);
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode)
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  lock_acquire (&inode->inode_lock);
  inode->open_cnt--;
  lock_release (&inode->inode_lock);

  /* Release resources if this was the last opener. */
  if (inode->open_cnt == 0)
    {
      /* Remove from inode list */
      list_remove (&inode->elem);

      /* Deallocate blocks if removed. */
      if (inode->removed)
        {
          ASSERT (inode->sector != INODE_INVALID_SECTOR);

          int processed = 0;
          /* Free all direct blocks */
          inode_close_direct_helper (inode->sector, &processed, inode->length);
          /* Free all indirect blocks */
          for (int i = 0; i < INODE_NUM_INDIRECT; i++)
            {
              int idx = INODE_INDIRECT_INDEX + i;
              inode_close_indirect_helper (inode->sector, idx, &processed,
                                           inode->length);
            }
          /* Free all doubly indirect blocks */
          for (int i = 0; i < INODE_NUM_DUBINDIRECT; i++)
            {
              int dub_idx = INODE_DUBINDIRECT_INDEX + i;
              block_sector_t dubindirect_sector = read_block_from_sector
                                                 (inode->sector, dub_idx);
              int idx = 0;
              while (processed < inode->length && idx < INODE_NUM_DIRECT)
                {
                  inode_close_indirect_helper (dubindirect_sector, idx,
                                               &processed, inode->length);
                  idx++;
                }
              if (dubindirect_sector != INODE_INVALID_SECTOR)
                free_map_release (dubindirect_sector, 1);
            }
          free_map_release (inode->sector, 1);
        }
      // Write in length
      cache_io_at (inode->sector, &inode->length, true, 0,
                   sizeof(inode->length), true);
      free (inode);
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
bool
inode_remove (struct inode *inode)
{
  ASSERT (inode != NULL);
  if (inode->open_cnt > 0 && inode->is_dir && !inode->removed)
    return false;
  lock_acquire (&inode->inode_lock);
  inode->removed = true;
  lock_release (&inode->inode_lock);
  return true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset)
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;

  while (size > 0)
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset, false);
      if (sector_idx == INODE_INVALID_SECTOR) break;
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      cache_io_at (sector_idx, (char *) buffer + bytes_read, false, sector_ofs,
                     chunk_size, false);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }

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

  if (inode->deny_write_cnt)
    return 0;

  while (size > 0)
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset, true);
      if (sector_idx == INODE_INVALID_SECTOR) break;
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < sector_left ? size : sector_left;
      if (chunk_size <= 0)
        break;

      cache_io_at (sector_idx, (char *) buffer + bytes_written, false,
          sector_ofs, chunk_size, true);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }

  /* Extend length of file if needed */
  bool lock_held = lock_held_by_current_thread (&inode->inode_lock);
  if (!lock_held)
	  lock_acquire (&inode->inode_lock);
  if (offset > inode->length) inode->length = offset;
  if (!lock_held)
	  lock_release (&inode->inode_lock);

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode)
{
  lock_acquire (&inode->inode_lock);
  inode->deny_write_cnt++;
  lock_release (&inode->inode_lock);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode)
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  lock_acquire (&inode->inode_lock);
  inode->deny_write_cnt--;
  lock_release (&inode->inode_lock);
}

/* Returns if INODE is a directory. */
bool
inode_is_dir (const struct inode *inode)
  {
    return inode->is_dir;
  }

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  return inode->length;
}