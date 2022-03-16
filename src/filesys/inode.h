#ifndef FILESYS_INODE_H
#define FILESYS_INODE_H

#include <stdbool.h>
#include "filesys/off_t.h"
#include "devices/block.h"
#include "threads/synch.h"

#define INODE_INVALID_SECTOR (block_sector_t) -1
struct bitmap;

void inode_init (void);
bool inode_create (block_sector_t, off_t, bool isdir);
struct inode *inode_open (block_sector_t);
struct inode *inode_reopen (struct inode *);
int inode_open_count (struct inode *);
block_sector_t inode_get_inumber (const struct inode *);
struct lock *inode_dir_lock (struct inode *inode);
void inode_close (struct inode *);
void inode_remove (struct inode *);
off_t inode_read_at (struct inode *, void *, off_t size, off_t offset);
off_t inode_write_at (struct inode *, const void *, off_t size, off_t offset);
void inode_deny_write (struct inode *);
void inode_allow_write (struct inode *);
off_t inode_length (struct inode *);
bool inode_isdir (const struct inode *);

#endif /* filesys/inode.h */
