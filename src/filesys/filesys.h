#ifndef FILESYS_FILESYS_H
#define FILESYS_FILESYS_H

#include <stdbool.h>
#include "filesys/off_t.h"
#include "filesys/directory.h"
#include "filesys/file.h"

/* Maximum length of a file name component. */
#define FILESYS_NAME_MAX NAME_MAX

/* Sectors of system file inodes. */
#define FREE_MAP_SECTOR 0       /* Free map file inode sector. */
#define ROOT_DIR_SECTOR 1       /* Root directory file inode sector. */

/* Block device that contains the file system. */
extern struct block *fs_device;

/* Filesystem house keeping. */
void filesys_init (bool format);
void filesys_done (void);

/* File operations. */
bool filesys_create (const char *path, off_t initial_size);
void filesys_close (struct file *);
off_t filesys_read (struct file *, void *buffer, off_t size);
off_t filesys_write (struct file *, const void *buffer, off_t size);
void filesys_seek (struct file *, off_t position);
off_t filesys_tell (struct file *);
int filesys_file_inumber (struct file *);
int filesys_filesize (struct file *);
void filesys_deny_write (struct file *);
void filesys_allow_write (struct file *);

/* Directory operations. */
bool filesys_mkdir (const char *path);
void filesys_closedir (struct dir *);
bool filesys_readdir (struct dir *, char *name);
int filesys_dir_inumber (struct dir *);

/* File and directory operations. */
bool filesys_remove (const char *path);
void *filesys_open (const char *path, bool *isdir);


#endif /* filesys/filesys.h */
