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

struct lock syscall_file_lock;  /*Lock to synchronize filesystem access*/

/* Structs and functions to create a system wide table of open files.*/
struct hash syscall_file_table; /*System wide table of open files*/
/* Struct wrapper to put files in syscall_file_table*/
struct syscall_file
  {
    struct hash_elem hash_elem; /* Hash table element. */
    char* file_name;            /* Key in hash table. */
    int count;                  /* Number of open instances of file. */
    struct file* file;          /* File associated with this wrapper. */            
    bool marked_del;            /* If this file is to be removed. */
  };
static unsigned
syscall_file_hash(const struct hash_elem *p_, void *aux UNUSED)
{
  const struct syscall_file *f = hash_entry(p_, struct syscall_file, hash_elem);
  return hash_string(f->file_name);
}

static bool syscall_file_less(const struct hash_elem *a_,
                              const struct hash_elem *b_,
                              void *aux UNUSED)
{
  const struct syscall_file *a = hash_entry (a_, struct syscall_file, hash_elem);
  const struct syscall_file *b = hash_entry (b_, struct syscall_file, hash_elem);
  return strcmp(a->file_name, b->file_name) > 0;
}

static struct syscall_file *
syscall_file_lookup (const char *file_name)
{
  struct syscall_file f;
  struct hash_elem *e;

  f.file_name = (char *) file_name;
  e = hash_find (&syscall_file_table, &f.hash_elem);

  return e != NULL ? hash_entry(e, struct syscall_file, hash_elem) : NULL;
}

/* This function Removes key FILE_NAME from syscall_file_table and 
 * returns removed element.
 * returns NULL if file_name was not found in syscall_file_table. */
static struct hash_elem *
syscall_file_remove (const char *file_name)
{
  struct syscall_file f;
  struct hash_elem *e;

  f.file_name = (char *) file_name;
  e = hash_find (&syscall_file_table, &f.hash_elem);
  if (e == NULL)
    return NULL;

  return hash_delete (&syscall_file_table, &f.hash_elem);
}


static void syscall_handler (struct intr_frame *);
static uint32_t syscall_get_arg (struct intr_frame *f, size_t idx);
static void syscall_validate_user_memory (const void *uaddr, size_t size);
static void syscall_validate_user_string (const char *uaddr, size_t max_size);
static void syscall_terminate_process (void);

/* Array of syscall handler functions to dispatch on interrupt. */
#define SYSCALL_CNT SYS_INUMBER + 1
typedef void syscall_handler_func (struct intr_frame *);
static syscall_handler_func *syscall_handlers[SYSCALL_CNT];

static void syscall_halt (struct intr_frame *);
static void syscall_exit (struct intr_frame *);
static void syscall_exec (struct intr_frame *);
static void syscall_wait (struct intr_frame *);
static void syscall_create (struct intr_frame *);
static void syscall_remove (struct intr_frame *);
static void syscall_open (struct intr_frame *);
static void syscall_filesize (struct intr_frame *);
static void syscall_read (struct intr_frame *);
static void syscall_write (struct intr_frame *);
static void syscall_seek (struct intr_frame *);
static void syscall_tell (struct intr_frame *);
static void syscall_close (struct intr_frame *);

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
  barrier ();  /* Write all handlers before starting syscalls. */ 
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");

  lock_init (&syscall_file_lock);
  hash_init (&syscall_file_table, syscall_file_hash, syscall_file_less, NULL);
}

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


static void 
syscall_terminate_process (void)
{
   /* TODO - free up all resources. */
  thread_current()->process_exit_code = -1;
  thread_exit();
}


/* Returns if UADDR is a valid virtual address in the page directory
   of the curren thread along with the entire block of SIZE bytes following 
   UADDR. Otherwise, calls syscall_terminate_process and never returns.
   SIZE must be a positive value. */
static void 
syscall_validate_user_memory (const void *uaddr, size_t size)
{
  const void *current_page;

  ASSERT (thread_current ()->pagedir != NULL);

  if (uaddr == NULL)
      syscall_terminate_process ();
  /* Loop over every page in the queried block and check its validity. */
  for (current_page = pg_round_down (uaddr); 
       current_page <= pg_round_down ((const uint8_t *)uaddr + size); 
       current_page += PGSIZE)
    {
      if (!is_user_vaddr (current_page) 
          || pagedir_get_page (thread_current ()->pagedir, 
                               current_page) == NULL)
          syscall_terminate_process ();
    } 
}

/* Returns if UADDR points to a valid null-terminated string in the
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
      syscall_validate_user_memory (caddr, sizeof (char));
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
  syscall_validate_user_memory (arg, sizeof (uint32_t));
  return *arg;
}

/* Shuts down the machine by calling shutdown_power_off.
   Never returns. */
static void 
syscall_halt (struct intr_frame *f)
{
  shutdown_power_off ();
}

static void 
syscall_exit (struct intr_frame *f)
{
  int32_t status = syscall_get_arg (f, 1);
  thread_current ()->process_exit_code = status;
  thread_exit ();
}

static void 
syscall_exec (struct intr_frame *f)
{
  const char *cmd_line = (const char* ) syscall_get_arg (f, 1);
  syscall_validate_user_string (cmd_line, PGSIZE);
  tid_t tid = process_execute (cmd_line);
  f->eax = tid;
}

static void 
syscall_wait (struct intr_frame *f)
{
  tid_t tid = syscall_get_arg (f, 1);
  int32_t exit_code = process_wait (tid);
  f->eax = exit_code;
}

static void 
syscall_create (struct intr_frame *f)
{
  const char *file_name = (const char* ) syscall_get_arg (f, 1);
  uint32_t initial_size = syscall_get_arg (f, 2);
  syscall_validate_user_string(file_name, PGSIZE);

  lock_acquire (&syscall_file_lock);
  f->eax = filesys_create (file_name, initial_size);
  lock_release (&syscall_file_lock);
}

static void 
syscall_remove (struct intr_frame *f)
{
  const char *file_name = (const char *) syscall_get_arg (f, 1);
  syscall_validate_user_string(file_name, PGSIZE);

  lock_acquire (&syscall_file_lock);
  struct syscall_file *file_wrapper = syscall_file_lookup(file_name);
  bool bRet = false;
  if (file_wrapper == NULL)
    {
      /*File is not opened*/
      bRet = filesys_remove(file_name);
    }
  else
    {
      /*File is opened by a process*/
      file_wrapper->marked_del = true;
      bRet = true;
    }
  lock_release(&syscall_file_lock);
  f->eax = bRet;
}

static void 
syscall_open (struct intr_frame *f)
{
  const char *file_name = (const char *) syscall_get_arg (f, 1);
  syscall_validate_user_string(file_name, PGSIZE);
  struct syscall_file *file_wrapper = syscall_file_lookup(file_name);

  if (file_wrapper != NULL)
    {
      /* Ensure file_name hasn't been marked for deletion */
      if (file_wrapper->marked_del)
          goto  fail;
    }
  else
    {
      /* file_name is opening for the first time and needs to be added
       * to system wide file table */

      /* Freed when it's last fd closes */
      file_wrapper = calloc (1, sizeof(struct syscall_file)); 
      if (file_wrapper == NULL) 
        goto fail;

      /* Set file_wrapper members */
      /* Allocate file name on heap */
      size_t len = strlen(file_name) + 1;
      file_wrapper->file_name = malloc(len);
      strlcpy (file_wrapper->file_name, file_name, len);

      file_wrapper->count = 0;
      file_wrapper->marked_del = false;
      file_wrapper->file = NULL;

      /* Set file_wrapper's file and add it to system wide file table */
      lock_acquire (&syscall_file_lock);
      struct file *file = filesys_open (file_wrapper->file_name);
      if (file == NULL)
        {
          free(file_wrapper);
          lock_release (&syscall_file_lock);
          goto  fail;
        }
      file_wrapper->file = file;
      hash_insert (&syscall_file_table, &file_wrapper->hash_elem);
      lock_release (&syscall_file_lock);
    }

  file_wrapper->count++;
  f->eax = process_new_fd (thread_current(), file_wrapper->file,
                           file_wrapper->file_name);
  return;
fail:
  f->eax = -1;
  return;
}

static void 
syscall_filesize (struct intr_frame *f)
{
  int32_t fd = syscall_get_arg (f, 1);
  
  struct process_fd *process_fd = process_get_fd (thread_current (), fd);
  if (process_fd == NULL)
    {
      f->eax = -1;
      return;
    }

  lock_acquire(&syscall_file_lock);
  f->eax = file_length(process_fd->file);
  lock_release(&syscall_file_lock);
}

static void 
syscall_read (struct intr_frame *f)
{
  int32_t fd = syscall_get_arg (f, 1);
  char *buffer = (char *) syscall_get_arg (f, 2);
  uint32_t size = syscall_get_arg (f, 3);
  /* Verify that the entire buffer is valid user memory. */
  syscall_validate_user_memory (buffer, size);
  
  if (fd == 0)
    {
      /* Read from stdin */
      size_t bytes_read = 0;
      while (bytes_read < size) 
        {
          buffer[bytes_read] = input_getc ();
          bytes_read++;
        }
      f->eax = bytes_read;
    }
  else 
    {
      /* Read for files */
      struct process_fd *process_fd = process_get_fd (thread_current (), fd);
      if (process_fd == NULL)
        {
          f->eax = -1;
          return;
        }
      
      lock_acquire(&syscall_file_lock);
      f->eax = file_read(process_fd->file, buffer, size);
      lock_release(&syscall_file_lock);
    }
}

static void 
syscall_write (struct intr_frame *f)
{
  int32_t fd = syscall_get_arg (f, 1);
  const char *buffer = (const char* ) syscall_get_arg (f, 2);
  size_t stride, size = syscall_get_arg (f, 3);
  /* Verify that the entire buffer is valid user memory. */
  syscall_validate_user_string (buffer, size);
 
  if (fd == 1)
    {
       /* For FD==1, print to the console strides of the buffer. */
      while (size > 0)
        {
          size -= stride = size > 256 ? 256 : size;
          putbuf(buffer, stride);

          buffer += stride;
        }
    }
  else 
    {
      /* Write for files */
      struct process_fd *process_fd = process_get_fd (thread_current (), fd);
      if (process_fd == NULL)
        {
          f->eax = -1;
          return;
        }
      
      lock_acquire(&syscall_file_lock);
      f->eax = file_write(process_fd->file, buffer, size);
      lock_release(&syscall_file_lock);
    }
}

static void 
syscall_seek (struct intr_frame *f)
{
  int32_t fd = syscall_get_arg (f, 1);
  unsigned position = syscall_get_arg (f, 2);
  
  struct process_fd *process_fd = process_get_fd (thread_current (), fd);
  if (process_fd == NULL)
    {
      f->eax = -1;
      return;
    }

  lock_acquire(&syscall_file_lock);
  file_seek(process_fd->file, position);
  lock_release(&syscall_file_lock);
}

static void 
syscall_tell (struct intr_frame *f)
{
  int32_t fd = syscall_get_arg (f, 1);
  
  struct process_fd *process_fd = process_get_fd (thread_current (), fd);
  if (process_fd == NULL)
    {
      f->eax = -1;
      return;
    }
  
  lock_acquire(&syscall_file_lock);
  f->eax = file_tell(process_fd->file);
  lock_release(&syscall_file_lock);
}

static void 
syscall_close (struct intr_frame *f)
{
  int32_t fd = syscall_get_arg (f, 1);
  syscall_close_helper (fd);
}

/* Helper function to close files so other files can update the system wide
 * file descriptor table. */
void syscall_close_helper (int fd)
{
  struct process_fd *process_fd = process_get_fd (thread_current (), fd); 
  if (process_fd == NULL) 
    return;
  
  lock_acquire(&syscall_file_lock);
  struct syscall_file *file_wrapper = syscall_file_lookup (process_fd->file_name);


  /* Update system wide file table */
  if (--file_wrapper->count == 0)
    {
      file_close (process_fd->file);
      if (file_wrapper->marked_del) 
        filesys_remove(process_fd->file_name);
      syscall_file_remove (process_fd->file_name);
      if (file_wrapper->file_name) free(file_wrapper->file_name);
      free(file_wrapper);
    }

  /* Update process' file descriptor table */
  process_remove_fd (thread_current(), fd);

  lock_release(&syscall_file_lock);
  return;
}
