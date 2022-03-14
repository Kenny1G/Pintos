#include "userprog/syscall.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include <stdio.h>
#include <syscall-nr.h>
#include <stddef.h>
#include <hash.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "devices/shutdown.h"
#include "devices/input.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "vm/page.h"

/* Array of syscall handler functions to dispatch on interrupt. */
#define SYSCALL_CNT SYS_INUMBER + 1
typedef void syscall_handler_func (struct intr_frame *);
static syscall_handler_func *syscall_handlers[SYSCALL_CNT];

/* Syscall handlers prototypes. */
static void syscall_handler (struct intr_frame *);
static void syscall_halt (struct intr_frame *);
static void syscall_exit (struct intr_frame *);
static void syscall_exec (struct intr_frame *);
static void syscall_wait (struct intr_frame *);
static void syscall_chdir (struct intr_frame *f);
static void syscall_mkdir (struct intr_frame *f);
static void syscall_readdir (struct intr_frame *f);
static void syscall_isdir (struct intr_frame *f);
static void syscall_inumber (struct intr_frame *f);
static void syscall_create (struct intr_frame *);
static void syscall_remove (struct intr_frame *);
static void syscall_open (struct intr_frame *);
static void syscall_filesize (struct intr_frame *);
static void syscall_read (struct intr_frame *);
static void syscall_write (struct intr_frame *);
static void syscall_seek (struct intr_frame *);
static void syscall_tell (struct intr_frame *);
static void syscall_close (struct intr_frame *);
static void syscall_mmap (struct intr_frame *);
static void syscall_munmap (struct intr_frame *);

/* User memory access infrastructure. */
static uint32_t syscall_get_arg (struct intr_frame *f, size_t idx);
static void syscall_validate_user_memory (const void *uaddr, size_t, bool);
static void syscall_validate_user_string (const char *uaddr, size_t max_size);
static void syscall_terminate_process (void);

/* Represents a file descriptor in the current process fd_table. */
struct fd_entry
  {
    struct hash_elem hash_elem; /* Element in fd_table. */
    int fd;                     /* File descriptor. */
    void *filesys_ptr;          /* struct file * or struct dir *. */
    bool isdir;                 /* True if filesys_ptr is struct dir *. */
  };
/* File descriptors 0, 1, and 2 are reserved for std i/o/e. */
#define SYSCALL_FIRST_FD 3
static hash_less_func fd_entry_less;
static hash_hash_func fd_entry_hash;
static hash_action_func fd_entry_destroy;
static int fd_allocate (void);
static struct fd_entry *fd_lookup (int);

/* Initialize syscalls by registering dispatch functions for supported
   syscall numbers and then registering the syscall interrupt handler. */
void
syscall_init (void)
{
  syscall_handlers[SYS_HALT] = syscall_halt;
  syscall_handlers[SYS_EXIT] = syscall_exit;
  syscall_handlers[SYS_EXEC] = syscall_exec;
  syscall_handlers[SYS_WAIT] = syscall_wait;
  syscall_handlers[SYS_CREATE] = syscall_create;
  syscall_handlers[SYS_REMOVE] = syscall_remove;
  syscall_handlers[SYS_OPEN] = syscall_open;
  syscall_handlers[SYS_FILESIZE] = syscall_filesize;
  syscall_handlers[SYS_READ] = syscall_read;
  syscall_handlers[SYS_WRITE] = syscall_write;
  syscall_handlers[SYS_SEEK] = syscall_seek;
  syscall_handlers[SYS_TELL] = syscall_tell;
  syscall_handlers[SYS_CLOSE] = syscall_close;
  syscall_handlers[SYS_MMAP] = syscall_mmap;
  syscall_handlers[SYS_MUNMAP] = syscall_munmap;
  syscall_handlers[SYS_CHDIR] = syscall_chdir;
  syscall_handlers[SYS_MKDIR] = syscall_mkdir;
  syscall_handlers[SYS_READDIR] = syscall_readdir;
  syscall_handlers[SYS_ISDIR] = syscall_isdir;
  syscall_handlers[SYS_INUMBER] = syscall_inumber;
  
  barrier ();  /* Write all handlers before starting syscalls. */
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

/* Initializes the file descriptor infrastructure for the current thread. */
bool 
syscall_process_init (void)
{
  struct thread *t = thread_current ();

  t->fd_next = SYSCALL_FIRST_FD;
  t->fd_table_ready = hash_init (&t->fd_table, fd_entry_hash, 
                                 fd_entry_less, NULL);
  return t->fd_table_ready;
}


/* Frees up the resources allocated for system calls if any. */
void 
syscall_process_done (void)
{
  struct thread *t = thread_current ();

  /* Check if nothing was allocated in the first place and abort. */
  if (!t->fd_table_ready)
    return;
  /* Destroy the open file table by closing its files. */
  hash_destroy (&t->fd_table, fd_entry_destroy);
  t->fd_table_ready = false;
}

/* Dispatches the correct syscall function to handle a syscall
   interrupt. */
static void
syscall_handler (struct intr_frame *f)
{
  int syscall_number;
  syscall_handler_func *handler_func;

  ASSERT (f != NULL);

  syscall_number = syscall_get_arg(f, 0);
  if (syscall_number < 0 || syscall_number >= SYSCALL_CNT
      || syscall_handlers[syscall_number] == NULL)
    /* Unsupported syscall. */
    syscall_terminate_process ();
  else
    {
      handler_func = syscall_handlers[syscall_number];
      handler_func (f);
    }
}

/* Shuts down the machine by calling shutdown_power_off.
   Never returns. */
static void
syscall_halt (struct intr_frame *f UNUSED)
{
  shutdown_power_off ();
}

/* Terminates the current user program, returning STATUS to the parent.
   Never returns. */
static void
syscall_exit (struct intr_frame *f)
{
  int32_t status = syscall_get_arg (f, 1);
  thread_current ()->process_exit_code = status;
  thread_exit ();
}

/* Runs the executable whose name is given in CMD_LINE, 
  passing any given arguments, and returns the new process's 
  program id (TID). Returns TID_ERROR on error. */
static void
syscall_exec (struct intr_frame *f)
{
  const char *cmd_line = (const char* ) syscall_get_arg (f, 1);
  syscall_validate_user_string (cmd_line, PGSIZE);
  tid_t tid = process_execute (cmd_line);
  f->eax = tid;
}

/* Waits for a child process TID and retrieves the child's exit status. */
static void
syscall_wait (struct intr_frame *f)
{
  tid_t tid = syscall_get_arg (f, 1);
  int32_t exit_code = process_wait (tid);
  f->eax = exit_code;
}

/* Changes the current working directory of the process to DIR_PATH,
   which may be relative or absolute. Returns true if successful,
   false on failure. */
static void
syscall_chdir (struct intr_frame *f)
{
  const char *dir_path = (const char *) syscall_get_arg (f, 1);
  void *dir_filesys_ptr;
  bool isdir = false;
  syscall_validate_user_string (dir_path, PGSIZE);

  f->eax = false;
  dir_filesys_ptr = filesys_open (dir_path, &isdir);
  if (dir_filesys_ptr != NULL && isdir)
    {
      /* Close the old directory and setup the new one. */
      filesys_closedir (thread_current ()->cwd);
      thread_current ()->cwd = dir_filesys_ptr;
      f->eax = true;
    }
  else if (dir_filesys_ptr != NULL)
    /* Not a directory, close it and fail. */
    filesys_close (dir_filesys_ptr);
}

/* Creates the directory named DIR_PATH, which may be relative or absolute.
   Returns true if successful, false on failure. Fails if DIR_PATH already 
   exists or if any directory name in DIR_PATH, besides the last, does not 
   already exist. That is, syscall_mkdir("/a/b/c") succeeds only if 
   /a/b already exists and /a/b/c does not. */
static void
syscall_mkdir (struct intr_frame *f)
{
  const char *dir_path = (const char* ) syscall_get_arg (f, 1);
  syscall_validate_user_string (dir_path, PGSIZE);
  
  f->eax = filesys_mkdir (dir_path);
}

/* Reads a directory entry from file descriptor FD, which must represent a
   directory. If successful, stores the null-terminated file name in NAME,
   which must have room for READDIR_MAX_LEN + 1 bytes, and returns true. 
   If no entries are left in the directory, returns false. */
static void
syscall_readdir (struct intr_frame *f)
{
  int32_t fd = syscall_get_arg (f, 1);
  char *name = (char *) syscall_get_arg (f, 2);
  struct fd_entry *fd_entry;
  syscall_validate_user_memory (name, FILESYS_NAME_MAX + 1, true);

  fd_entry = fd_lookup (fd);
  if (fd_entry == NULL || !fd_entry->isdir)
    /* FD invalid or not a directory, fail. */
    f->eax = false;
  else
    /* Found valid dir, perform the readdir operation on it. */
    f->eax = filesys_readdir (fd_entry->filesys_ptr, name);
}

/* Returns true if FD represents a directory, 
   false if it represents an ordinary file or is invalid. */
static void
syscall_isdir (struct intr_frame *f)
{
  int32_t fd = syscall_get_arg (f, 1);
  struct fd_entry *fd_entry;

  fd_entry = fd_lookup (fd);
  f->eax = fd_entry != NULL && fd_entry->isdir;
}

/* Returns the inode number of the inode associated with FD, 
   which may represent an ordinary file or a directory. */
static void
syscall_inumber (struct intr_frame *f)
{
  int32_t fd = syscall_get_arg (f, 1);
  struct fd_entry *fd_entry;

  fd_entry = fd_lookup (fd);
  if (fd_entry == NULL)
    /* FD is invalid, fail. */
    f->eax = SYSCALL_ERROR;
  else
    /* Query the filesystem for inumber. */
    f->eax = fd_entry->isdir ? filesys_dir_inumber (fd_entry->filesys_ptr) 
                             : filesys_file_inumber (fd_entry->filesys_ptr);
}

/* Creates a new file at PATH initially INITIAL_SIZE bytes in size. 
   Returns true if successful, false otherwise. */
static void
syscall_create (struct intr_frame *f)
{
  const char *path = (const char* ) syscall_get_arg (f, 1);
  uint32_t initial_size = syscall_get_arg (f, 2);
  syscall_validate_user_string(path, PGSIZE);

  f->eax = filesys_create (path, initial_size);
}

/* Deletes the file/dir at PATH. Returns true if successful, false otherwise.
   A file may be removed regardless of whether it is open or closed, 
   and removing an open file does not close it. */
static void
syscall_remove (struct intr_frame *f)
{
  const char *path = (const char *) syscall_get_arg (f, 1);
  syscall_validate_user_string (path, PGSIZE);

  f->eax = filesys_remove (path);
}

/* Opens the file/dir at PATH. Returns a nonnegative integer handle called 
   a "file descriptor" (fd), or -1 if PATH could not be opened. */
static void
syscall_open (struct intr_frame *f)
{
  const char *path = (const char *) syscall_get_arg (f, 1);
  struct fd_entry *fd_entry = NULL;
  syscall_validate_user_string(path, PGSIZE);

  fd_entry = malloc (sizeof *fd_entry);
  if (fd_entry == NULL)
    goto fail;
  /* Attempt to open the file and update the fd_entry accordingly. */
  fd_entry->filesys_ptr = filesys_open (path, &fd_entry->isdir);
  if (fd_entry->filesys_ptr == NULL)
    goto fail;
  fd_entry->fd = fd_allocate ();
  /* Add the file descriptor to the thread. */
  hash_insert (&thread_current ()->fd_table, &fd_entry->hash_elem);
  /* Return the file descriptor. */
  f->eax = fd_entry->fd;
  return;
fail:
  free (fd_entry);
  f->eax = -1;
  return;
}

/* Returns the size, in bytes, of the file open as FD.
   Returns SYSCALL_ERROR if FD is not a valid file. */
static void
syscall_filesize (struct intr_frame *f)
{
  int32_t fd = syscall_get_arg (f, 1);
  struct fd_entry *fd_entry;

  fd_entry = fd_lookup (fd);
  if (fd_entry == NULL || fd_entry->isdir)
    /* FD is invalid, fail. */
    f->eax = SYSCALL_ERROR;
  else
    /* Query the filesystem for filesize. */
    f->eax = filesys_filesize (fd_entry->filesys_ptr);
}

/* Reads SIZE bytes from the file open as FD into BUFFER. 
   Returns the number of bytes actually read (0 at end of file), 
   or SYSCALL_ERROR if the file could not be read.
   Fd 0 reads from the keyboard. */
static void
syscall_read (struct intr_frame *f)
{
  int32_t fd = syscall_get_arg (f, 1);
  uint8_t *buffer = (uint8_t *) syscall_get_arg (f, 2);
  uint32_t size = syscall_get_arg (f, 3);
  struct fd_entry *fd_entry;
  /* Verify that the entire buffer is valid user memory that's writable. */
  syscall_validate_user_memory (buffer, size, true);

  if (fd == 0)
    {
      /* Read from stdin */
      size_t bytes_read = 0;
      while (bytes_read < size)
        {
          page_pin (buffer);
          buffer[bytes_read] = input_getc ();
          page_unpin (buffer);
          bytes_read++;
        }
      f->eax = bytes_read;
    }
  else
    {
      fd_entry = fd_lookup (fd);
      if (fd_entry == NULL || fd_entry->isdir)
        {
          /* FD is invalid, fail. */
          f->eax = SYSCALL_ERROR;
          return;
        }
      /* Pin buffer and read. */
      for (uint8_t *cpage = pg_round_down (buffer); cpage <= buffer + size; cpage += PGSIZE)
        page_pin (cpage);
      f->eax = filesys_read (fd_entry->filesys_ptr, buffer, size);
      for (uint8_t *cpage = pg_round_down (buffer); cpage <= buffer + size; cpage += PGSIZE)
        page_unpin (cpage);
    }
}

/* Writes SIZE bytes from BUFFER to the open file FD. 
  Returns the number of bytes actually written, which may be less 
  than SIZE. */
static void
syscall_write (struct intr_frame *f)
{
  int32_t fd = syscall_get_arg (f, 1);
  uint8_t *buffer = (uint8_t *) syscall_get_arg (f, 2);
  size_t stride, size = syscall_get_arg (f, 3);
  struct fd_entry *fd_entry;
  /* Verify that the entire buffer is valid user memory. */
  syscall_validate_user_memory (buffer, size, false);

  if (fd == 1)
    {
       /* For FD==1, print to the console strides of the buffer. */
      while (size > 0)
        {
          size -= stride = size > 256 ? 256 : size;
          page_pin (buffer);
          putbuf ((char *) buffer, stride);
          page_unpin (buffer);
          buffer += stride;
        }
    }
  else
    {
      /* Write for files */
      fd_entry = fd_lookup (fd);
      if (fd_entry == NULL || fd_entry->isdir)
        {
          /* FD is invalid, fail. */
          f->eax = SYSCALL_ERROR;
          return;
        }
      /* Pin buffer and write. */
      for (uint8_t *cpage = pg_round_down (buffer); cpage <= buffer + size; cpage += PGSIZE)
        page_pin (cpage);
      f->eax = filesys_write (fd_entry->filesys_ptr, buffer, size);
      for (uint8_t *cpage = pg_round_down (buffer); cpage <= buffer + size; cpage += PGSIZE)
        page_unpin (cpage);
    }
}

/* Changes the next byte to be read or written in open file FD
   to POSITION, expressed in bytes from the beginning of the file.
   (Thus, a POSITION of 0 is the file's start). */
static void
syscall_seek (struct intr_frame *f)
{
  int32_t fd = syscall_get_arg (f, 1);
  unsigned position = syscall_get_arg (f, 2);
  struct fd_entry *fd_entry;

  fd_entry = fd_lookup (fd);
  if (fd_entry == NULL || fd_entry->isdir)
    {
      /* FD is invalid, fail. */
      f->eax = SYSCALL_ERROR;
      return;
    }
  filesys_seek (fd_entry->filesys_ptr, position);
}

/* Returns the position of the next byte to be read or written in 
   open file FD, expressed in bytes from the beginning of the file. */
static void
syscall_tell (struct intr_frame *f)
{
  int32_t fd = syscall_get_arg (f, 1);
  struct fd_entry *fd_entry;

  fd_entry = fd_lookup (fd);
  if (fd_entry == NULL || fd_entry->isdir)
    {
      /* FD is invalid, fail. */
      f->eax = SYSCALL_ERROR;
      return;
    }
  f->eax = filesys_tell (fd_entry->filesys_ptr);
}

/* Closes file descriptor FD and frees its resources. */
static void
syscall_close (struct intr_frame *f)
{
  int32_t fd = syscall_get_arg (f, 1);
  struct fd_entry query;
  struct hash_elem *e;

  query.fd = fd;
  e = hash_delete (&thread_current ()->fd_table, &query.hash_elem);
  if (e == NULL)
    return; /* FD does not exist. */
  /* Close FD and free its resources. */
  fd_entry_destroy (e, NULL);
}

/* Maps the file open as fd into the process's virtual address space*/
static void
syscall_mmap (struct intr_frame *f)
{
  int32_t fd = syscall_get_arg (f, 1);
  void *addr = (void *) syscall_get_arg (f, 2);
  struct thread *t = thread_current ();  
  struct fd_entry *fd_entry;
  struct page_mmap *mmap;
  size_t filesize, offset = 0;

  f->eax = MAP_FAILED;
  /* Fail if addr is 0, addr is not page-aligned, or fd is 0 or 1. */
  if (addr == 0 || pg_round_down (addr) != addr || fd == 0 || fd == 1)
    return;
  /* Get file that will back mmap. */
  fd_entry = fd_lookup (fd);
  if (fd_entry == NULL || fd_entry->isdir)
    return;
  filesize = filesys_filesize (fd_entry->filesys_ptr);
  /* Fail if backing file has length 0. */
  if (filesize == 0)
      return;
  /* Create a new memory map. */
  mmap = page_mmap_new (fd_entry->filesys_ptr, filesize);
  /* Populate mmap's pages. */
  while (offset < mmap->file_size)
    {
      /* Get zero bytes of page. */
      size_t zero_bytes = 0;
      size_t stick_out = mmap->file_size - offset;
      if (stick_out < PGSIZE)
        zero_bytes = PGSIZE - stick_out;
      bool success = page_add_to_mmap (mmap, addr + offset, 
                                       offset, zero_bytes);
      if (!success)
        {
          page_delete_mmap (mmap);
          return;
        }
      offset += PGSIZE;
    }
  /* Associate mmap with thread. */
  mmap->id = t->mmap_next_id++;
  list_push_back (&t->mmap_list, &mmap->list_elem);
  f->eax = mmap->id;
  return;
}

/* Unmaps the mapping designated by mapping, which must be a mapping ID
   returned by a previous call to mmap by the same process that has not
   yet been unmapped. */
static void
syscall_munmap (struct intr_frame *f)
{
  mapid_t id = syscall_get_arg (f, 1);

  struct page_mmap *mmap = page_get_mmap (thread_current (), id);
  if (mmap != NULL)
    {
      list_remove (&mmap->list_elem);
      page_delete_mmap (mmap);
    }
}

/* Returns the next available file descriptor for the current thread. */
static int 
fd_allocate (void)
{
  return thread_current ()->fd_next++;
}

/* Closes the file/dir of the fd_entry owning E and frees its resources. */
static void 
fd_entry_destroy (struct hash_elem *e, void *aux UNUSED)
{
  struct fd_entry *fd_entry;

  fd_entry = hash_entry (e, struct fd_entry, hash_elem);
  if (fd_entry->isdir)
    filesys_closedir (fd_entry->filesys_ptr);
  else
    filesys_close (fd_entry->filesys_ptr);
  free (fd_entry);
}

/* Returns a pointer to `struct fd_entry` corresponding to file
   descriptor FD in the current thread's fd_table. Returns NULL
   if FD is not found. */
static struct fd_entry *
fd_lookup (int fd)
{
  struct thread *t = thread_current ();
  struct fd_entry query, *found;
  struct hash_elem *e;

  query.fd = fd;
  e = hash_find (&t->fd_table, &query.hash_elem);
  found = e != NULL ? hash_entry (e, struct fd_entry, hash_elem) : NULL;
  return found;
}


/* Hash function that hashes an fd_entry's fd into its fd_table hash table. */
static unsigned
fd_entry_hash (const struct hash_elem *e, void *aux UNUSED)
{
  const struct fd_entry *fd_entry = hash_entry (e, struct fd_entry, hash_elem);
  return hash_int (fd_entry->fd);
}

/* Hash comparison function for fd_entry's in `hash fd_table`. */
static bool
fd_entry_less (const struct hash_elem *a_,
               const struct hash_elem *b_,
               void *aux UNUSED)
{
  const struct fd_entry *a = hash_entry (a_, struct fd_entry, hash_elem);
  const struct fd_entry *b = hash_entry (b_, struct fd_entry, hash_elem);
  return a->fd < b->fd;
}


/* Terminates the current process with error code -1 freeing its
   resources. Called in various syscalls on error. Never returns. */
static void
syscall_terminate_process (void)
{
  thread_current()->process_exit_code = -1;
  thread_exit();
}

/* Returns only if UADDR is a valid virtual address in the page directory
   of the curren thread along with the entire block of SIZE bytes following
   UADDR. Otherwise, calls syscall_terminate_process and never returns.
   SIZE must be a positive value. */
static void
syscall_validate_user_memory (const void *uaddr, size_t size, bool writable)
{
  const void *current_page;
  struct page *p;

  ASSERT (thread_current ()->pagedir != NULL);

  if (uaddr == NULL)
      syscall_terminate_process ();
  /* Loop over every page in the queried block and check its validity. */
  for (current_page = pg_round_down (uaddr);
       current_page <= pg_round_down ((const uint8_t *)uaddr + size);
       current_page += PGSIZE)
    {
      if (!is_user_vaddr (current_page)
          || (p = page_lookup ((void *) current_page)) == NULL
          || (writable && !page_is_writable (p)))
          syscall_terminate_process ();
    }
}

/* Returns only if UADDR points to a valid null-terminated string in the
   current thread's page directory or if no null-terminator is found
   in the MAX_SIZE valid chars following UADDR. Otherwise, calls
   syscall_terminate_process and never returns.
   MAX_SIZE must be positive. */
static void
syscall_validate_user_string (const char *uaddr, size_t max_size)
{
  const char *caddr = uaddr;

  ASSERT (thread_current ()->pagedir != NULL);

  for (; caddr != uaddr + max_size + 1; ++caddr)
    {
      syscall_validate_user_memory (caddr, sizeof (char), false);
      if (*caddr == '\0')
        break;
    }
}

/* Returns argument number IDX passed to the system call threough
   interrupt frame F after passing it to syscall_validate_user_memory.
   Remember that all arguments are of size 32-bit.
   Remember that IDX=0 is the syscall number. */
static uint32_t
syscall_get_arg (struct intr_frame *f, size_t idx)
{
  uint32_t *arg = (uint32_t *)(f->esp) + idx;
  syscall_validate_user_memory (arg, sizeof (uint32_t), false);
  return *arg;
}
