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
//todo(kenny) rename to num_sectors in ind block
#define INODE_NUM_IN_IND_BLOCK 128

static char ZEROARRAY[BLOCK_SECTOR_SIZE];

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    block_sector_t direct_blocks [INODE_NUM_DIRECT];
    block_sector_t indirect_block;
    block_sector_t dubindirect_block;

    bool is_dir;
    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */
  };

struct inode_indirect_sector
{
  block_sector_t data[INODE_NUM_IN_IND_BLOCK];
};

static block_sector_t get_index (const struct inode_disk *disk_inode,
                                 off_t abs_idx);
static bool inode_expand (struct inode_disk* disk_inode, off_t new_size);
static bool inode_clear (struct inode* inode);

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
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct inode_disk data;             /* Inode content. */
  };

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos) 
{
  ASSERT (inode != NULL);
  if (pos >= 0 && pos < inode->data.length)
    {
      off_t abs_idx =  pos / BLOCK_SECTOR_SIZE;
      return get_index (&inode->data, abs_idx);
    }
    //return inode->data.start + pos / BLOCK_SECTOR_SIZE;
  else
    return INODE_INVALID_SECTOR;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
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
      disk_inode->magic = INODE_MAGIC;
      disk_inode->is_dir = isdir;
      /* old way write inode at first sector at fill rest with 0*/
      //size_t sectors = bytes_to_sectors (length);
      //if (free_map_allocate (sectors, &disk_inode->start)) 
      //  {
      //    //block_write (fs_device, sector, disk_inode);
      //    cache_io_at (sector, disk_inode, true, 0, BLOCK_SECTOR_SIZE, true);
      //    if (sectors > 0) 
      //      {
      //        static char zeros[BLOCK_SECTOR_SIZE];
      //        size_t i;
      //        
      //        for (i = 0; i < sectors; i++) 
      //        {
      //          //block_write (fs_device, disk_inode->start + i, zeros);
      //          cache_io_at (disk_inode->start + i, zeros, true, 0, BLOCK_SECTOR_SIZE, true);
      //        }
      //      }
      //    success = true; 
      //  } 
      /* New way: set all blocks in inode to invalid and write inode to cache/disk... file fetched as needed */
      memset (&disk_inode->direct_blocks, INODE_INVALID_SECTOR,
              INODE_NUM_DIRECT * sizeof(block_sector_t));
      disk_inode->indirect_block = INODE_INVALID_SECTOR;
      disk_inode->dubindirect_block = INODE_INVALID_SECTOR;
      if (!inode_expand (disk_inode, length))
        success = false;
      else
        {
          cache_io_at (sector, disk_inode, true, 0, BLOCK_SECTOR_SIZE, true);
          success = true; 
        } 
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
          return inode_reopen (inode);
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
  cache_io_at (inode->sector, &inode->data, true, 0, sizeof(inode->data),
               false);
  //block_read (fs_device, inode->sector, &inode->data);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode == NULL || inode->removed)
    return NULL;
  inode->open_cnt++;
  return inode;
}

/* Returns the number of open instances of INODE. */
int 
inode_open_count (struct inode *inode)
{
  return inode->open_cnt;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
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

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
 
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
          free_map_release (inode->sector, 1);
          inode_clear (inode);
        }

      free (inode); 
    }
}

/* Returns true if INODE represents a directory not a file. */
bool
inode_isdir (const struct inode *inode)
{ 
  return inode->data.is_dir;
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t *bounce = NULL;

  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
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

      block_sector_t sector_next = byte_to_sector (inode, offset + chunk_size);
      if (sector_next == sector_idx) sector_next = INODE_INVALID_SECTOR;
      cache_io_at_ (sector_idx, buffer + bytes_read, false, sector_ofs,
                   chunk_size, false,
                   sector_next);
      //if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
      //  {
      //    /* Read full sector directly into caller's buffer. */
      //    block_read (fs_device, sector_idx, buffer + bytes_read);
      //  }
      //else 
      //  {
      //    /* Read sector into bounce buffer, then partially copy
      //       into caller's buffer. */
      //    if (bounce == NULL) 
      //      {
      //        bounce = malloc (BLOCK_SECTOR_SIZE);
      //        if (bounce == NULL)
      //          break;
      //      }
      //    block_read (fs_device, sector_idx, bounce);
      //    memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
      //  }
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  free (bounce);

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
  uint8_t *bounce = NULL;

  if (inode->deny_write_cnt)
    return 0;

  // Expand if write will go past end of file
  if (byte_to_sector (inode, offset + size - 1) == INODE_INVALID_SECTOR)
    {
      if (!inode_expand (&inode->data, offset + size))
        return 0;

      // Update inode to reflect new size
      inode->data.length = offset + size;
      cache_io_at (inode->sector, &inode->data, true, 0, BLOCK_SECTOR_SIZE,
                   true);
    }

  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      block_sector_t sector_next = byte_to_sector (inode, offset + chunk_size);
      if (sector_next == sector_idx) sector_next = INODE_INVALID_SECTOR;
      cache_io_at_ (sector_idx, (void*) buffer + bytes_written, false,
                    sector_ofs, chunk_size, true,
                    sector_next);
//      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
//        {
//          /* Write full sector directly to disk. */
//          block_write (fs_device, sector_idx, buffer + bytes_written);
//        }
//      else 
//        {
//          /* We need a bounce buffer. */
//          if (bounce == NULL) 
//            {
//              bounce = malloc (BLOCK_SECTOR_SIZE);
//              if (bounce == NULL)
//                break;
//            }
//
//          /* If the sector contains data before or after the chunk
//             we're writing, then we need to read in the sector
//             first.  Otherwise we start with a sector of all zeros. */
//          if (sector_ofs > 0 || chunk_size < sector_left) 
//            block_read (fs_device, sector_idx, bounce);
//          else
//            memset (bounce, 0, BLOCK_SECTOR_SIZE);
//          memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
//          block_write (fs_device, sector_idx, bounce);
//        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  free (bounce);

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
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
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  return inode->data.length;
}

static block_sector_t
get_index (const struct inode_disk *disk_inode, off_t abs_idx)
{
  struct inode_indirect_sector *sect;
  block_sector_t idx = INODE_INVALID_SECTOR;

  if (abs_idx < INODE_NUM_DIRECT)
    {
        idx = disk_inode->direct_blocks[abs_idx];
    }
  else if (abs_idx < (INODE_NUM_DIRECT + INODE_NUM_IN_IND_BLOCK))
    {
      sect = calloc (1, sizeof (struct inode_indirect_sector));
      if (sect != NULL)
        {
          cache_io_at (disk_inode->indirect_block, sect, true, 0,
                       BLOCK_SECTOR_SIZE, false);
          idx = sect->data [abs_idx - INODE_NUM_DIRECT];
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
          cache_io_at (disk_inode->dubindirect_block, sect, true, 0,
                       BLOCK_SECTOR_SIZE, false);
          cache_io_at (sect->data[outer_idx], sect, true, 0, BLOCK_SECTOR_SIZE,
                       false);
          idx = sect->data[inner_idx];
          free (sect);
        }
    }

  return idx;
}

static bool
inode_expand_helper (block_sector_t *idx, size_t num_sectors_left, int level)
{
  static block_sector_t filler[BLOCK_SECTOR_SIZE] = {INODE_INVALID_SECTOR};

  if (level == 0) {
    if (*idx == 0)
      {
        if (!free_map_allocate (1, idx)) return false;
        cache_io_at (*idx, ZEROARRAY, false, 0, BLOCK_SECTOR_SIZE, true);
      }
    return true;
  }

  struct inode_indirect_sector indirect_block;
  if(*idx == INODE_INVALID_SECTOR || *idx == 0) 
  {
    if (!free_map_allocate (1, idx)) return false;
    cache_io_at (*idx, ZEROARRAY, true, 0, BLOCK_SECTOR_SIZE, true);
  } 
  cache_io_at (*idx, &indirect_block, true, 0, BLOCK_SECTOR_SIZE, false);

  size_t unit = (level == 1 ? 1 : INODE_NUM_IN_IND_BLOCK);
  size_t i, l = DIV_ROUND_UP (num_sectors_left, unit);

  for (i = 0; i < l; ++ i) {
    size_t subsize = (num_sectors_left <  unit) ? num_sectors_left : unit;
    bool bRet = inode_expand_helper (&indirect_block.data[i], subsize, level - 1);
    if (!bRet) return false;
    num_sectors_left -= subsize;
  }

  ASSERT (num_sectors_left == 0);
  cache_io_at (*idx, &indirect_block, true, 0, BLOCK_SECTOR_SIZE, true);
  return true;
}

/* Expand inode so it has enough sectors to hold a file of size NEW_SIZE.
   Returns true on success and false on error. */
static bool
inode_expand (struct inode_disk* disk_inode, off_t new_size)
{
  if (new_size < 0) return false;

  int num_sectors_left = bytes_to_sectors (new_size);

  // Fill in direct blocks
  int num_direct = (num_sectors_left < INODE_NUM_DIRECT) ?
                    num_sectors_left : INODE_NUM_DIRECT;
  for (int i = 0; i < num_direct; ++i)
    {
      block_sector_t *cand =  &disk_inode->direct_blocks[i];
      if (*cand  == INODE_INVALID_SECTOR)
        {
          if (!free_map_allocate(1, cand)) return false;
          cache_io_at (*cand, ZEROARRAY, false, 0, BLOCK_SECTOR_SIZE, true);
        }
    }
  num_sectors_left -= num_direct;
  if (num_sectors_left == 0) return true;

  // Fill in indirect blocks
  int num_indirect = (num_sectors_left <  INODE_NUM_IN_IND_BLOCK) ?
    num_sectors_left : INODE_NUM_IN_IND_BLOCK;
  bool bRet = inode_expand_helper (&disk_inode->indirect_block, num_indirect, 1);
  if (!bRet) return false;
  num_sectors_left -= num_indirect;
  if(num_sectors_left == 0) return true;

  // Fill in doubly indirect blocks
  off_t oRet = INODE_NUM_IN_IND_BLOCK * INODE_NUM_IN_IND_BLOCK;
  num_indirect = (num_sectors_left <  oRet) ?  num_sectors_left : oRet;
  bRet = inode_expand_helper (&disk_inode->dubindirect_block, num_indirect, 2);
  if (!bRet) return false;
  num_sectors_left -= num_indirect;
  if(num_sectors_left == 0) return true;
  return false;
}

static void
inode_clear_helper (block_sector_t idx, size_t num_sectors, int level)
{
  if (level == 0) {
    free_map_release (idx, 1);
    return;
  }

  struct inode_indirect_sector indirect_block; 
  cache_io_at (idx, &indirect_block, true, 0, BLOCK_SECTOR_SIZE, false);

  size_t unit = (level == 1 ? 1 : INODE_NUM_IN_IND_BLOCK);
  size_t i, l = DIV_ROUND_UP (num_sectors, unit);

  for (i = 0; i < l; ++ i) {
    size_t subsize = num_sectors < unit? num_sectors : unit;
    inode_clear_helper (indirect_block.data[i], subsize, level - 1);
    num_sectors -= subsize;
  }

  ASSERT (num_sectors == 0);
  free_map_release (idx, 1);
}

/* Marks free all sectors related to this inode. */
static bool
inode_clear (struct inode* inode)
{
  if (inode->data.length < 0) return false;

  int num_sectors_left = bytes_to_sectors (inode->data.length);

  // Clear direct blocks
  int num_direct = (num_sectors_left < INODE_NUM_DIRECT) ?
                    num_sectors_left : INODE_NUM_DIRECT;
  for (int i = 0; i < num_direct; ++i)
    {
      free_map_release (inode->data.direct_blocks[i], 1);
    }
  num_sectors_left -= num_direct;

  // Free indirect blocks
  int num_indirect = (num_sectors_left <  INODE_NUM_IN_IND_BLOCK) ?
    num_sectors_left : INODE_NUM_IN_IND_BLOCK;
  if (num_indirect > 0)
  {
    inode_clear_helper (inode->data.indirect_block, num_indirect, 1);
    num_sectors_left -= num_indirect;
  }

  // Free doubly indirect blocks
  off_t oRet = INODE_NUM_IN_IND_BLOCK * INODE_NUM_IN_IND_BLOCK;
  num_indirect = (num_sectors_left <  oRet) ?  num_sectors_left : oRet;
  if (num_indirect > 0)
    {
      inode_clear_helper (inode->data.dubindirect_block, num_indirect, 1);
      num_sectors_left -= num_indirect;
    }

  ASSERT (num_sectors_left == 0);
  return true;
}
