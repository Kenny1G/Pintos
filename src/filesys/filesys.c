#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/cache.h"
#include "threads/thread.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  inode_init ();
  free_map_init ();

  cache_init ();

  if (format) 
    do_format ();

  free_map_open ();
  thread_current ()->cwd = dir_open_root ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  free_map_close ();
}

/* Creates a file at PATH with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if PATH contain non-existent dirs,
   or if internal memory allocation fails. */
bool
filesys_create (const char *path, off_t initial_size) 
{
  block_sector_t inode_sector = 0;
  struct dir *dir = dir_open_dirs (path);
  const char *name = dir_parse_filename (path);
  bool success = (dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size)
                  && dir_add (dir, name, inode_sector));
  if (!success && inode_sector != 0) 
    free_map_release (inode_sector, 1);
  dir_close (dir);

  return success;
}

/* Creates the directory named PATH, which may be relative or absolute. 
   Returns true if successful, false on failure. Fails if PATH already exists 
   or if any directory name in PATH, besides the last, does not already exist. 
   That is, filesys_mkdir("/a/b/c") succeeds only if /a/b already exists and 
   /a/b/c does not. */
bool
filesys_mkdir (const char *path) 
{
  struct dir *parent_dir = NULL, *dir = NULL;
  struct inode *inode = NULL;
  const char *dir_name;
  block_sector_t inode_sector = 0;

  ASSERT (path != NULL);

  parent_dir = dir_open_dirs (path);
  dir_name = dir_parse_filename (path);
  if (parent_dir == NULL
      || !free_map_allocate (1, &inode_sector)
      || !dir_create (inode_sector, 16))
    goto fail;
  /* dir inode created successfully. Now link the dirs. */
  inode = inode_open (inode_sector);
  if (inode == NULL)
    goto fail;
  dir = dir_open (inode);
  if (dir == NULL
      || !dir_add (dir, "..", inode_get_inumber (dir_get_inode (parent_dir)))
      || !dir_add (dir, ".", inode_sector)
      || !dir_add (parent_dir, dir_name, inode_sector))
    goto fail;
  dir_close (dir);
  dir_close (parent_dir);
  return true;

fail:
  if (dir != NULL)
    dir_close (dir);
  if (inode != NULL)
    inode_remove (inode);
  else if (inode_sector != 0)
    free_map_release (inode_sector, 1);
  if (parent_dir != NULL)
    dir_close (parent_dir);
  return false;
}

/* Reads a directory entry from DIR. If successful, 
   stores the null-terminated file name in NAME,
   which must have room for READDIR_MAX_LEN + 1 bytes, and returns true. 
   If no entries are left in the directory, returns false. */
bool
filesys_readdir (struct dir *dir, char *name) 
{
  ASSERT (name != NULL && dir != NULL);
  return dir_readdir (dir, name);
}

/* Opens the file/dir with the given PATH. 
   Sets *ISDIR according to what is found at PATH if ISDIR is not NULL.
   Returns the new file/dir if successful or a null pointer otherwise.
   Fails if PATH doesn't exist, or if an internal memory allocation fails. */
void *
filesys_open (const char *path, bool *isdir)
{
  struct dir *dir;
  const char *name;
  struct inode *inode = NULL;

  ASSERT (path != NULL);

  dir = dir_open_dirs (path);
  name = dir_parse_filename (path);
  if (dir != NULL)
    dir_lookup (dir, name, &inode);
  dir_close (dir);
  if (isdir != NULL)
    *isdir = inode_isdir (inode);
  return inode_isdir (inode) ? (void *) dir_open (inode) 
                             : (void *) file_open (inode);
}

/* Returns the inumber of directory DIR. */
int 
filesys_dir_inumber (struct dir *dir)
{
  return inode_get_inumber (dir_get_inode (dir));
}

/* Returns the inumber of file FILE. */
int 
filesys_file_inumber (struct file *file)
{
  return inode_get_inumber (file_get_inode (file));
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) 
{
  struct dir *dir = dir_open_dirs (name);
  name = dir_parse_filename (name);
  bool success = dir != NULL && dir_remove (dir, name);
  dir_close (dir); 

  return success;
}

/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}

/* Wrapper for file_read. */
off_t 
filesys_read (struct file *file, void *buffer, off_t size)
{
  return file_read (file, buffer, size);
}

/* Wrapper for file_write. */
off_t 
filesys_write (struct file *file, const void *buffer, off_t size)
{
  return file_write (file, buffer, size);
}

/* Wrapper for file_seek. */
void 
filesys_seek (struct file *file, off_t position)
{
  file_seek (file, position);
}

/* Wrapper for file_tell. */
off_t 
filesys_tell (struct file *file)
{
  return file_tell (file);
}

/* Wrapper for file_deny_write. */
void 
filesys_deny_write (struct file *file)
{
  file_deny_write (file);
}

/* Wrapper for file_allow_write. */
void 
filesys_allow_write (struct file *file)
{
  file_allow_write (file);
}

/* Wrapper for dir_close. */
void 
filesys_closedir (struct dir *dir)
{
  dir_close (dir);
}

/* Wrapper for file_close. */
void 
filesys_close (struct file *file)
{
  file_close (file);
}

/* Wrapper for file_length. */
int 
filesys_filesize (struct file *file)
{
  return file_length (file);
}

/* Wrapper for file_read_at. */
off_t 
filesys_read_at (struct file *file, void *buffer, off_t size, off_t start)
{
  return file_read_at (file, buffer, size, start);
}

/* Wrapper for file_write_at. */
off_t filesys_write_at (struct file *file, const void *buffer, 
                        off_t size, off_t start)
{
  return file_write_at (file, buffer, size, start);
}
