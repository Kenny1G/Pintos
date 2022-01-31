#include "userprog/syscall.h"
#include "userprog/pagedir.h"
#include <stdio.h>
#include <syscall-nr.h>
#include <stddef.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "devices/shutdown.h"

static void syscall_handler (struct intr_frame *);
static uint32_t syscall_get_arg (struct intr_frame *f, size_t idx);
static bool syscall_validate_user_memory (const void *uaddr, size_t size);
static bool syscall_validate_user_string (const char *uaddr, size_t max_size);
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
   /* TODO - exit thread and free up all resources. */
}


/* Returns true if UADDR is a valid virtual address in the page directory
   of the curren thread along with the entire block of SIZE bytes following 
   UADDR. Otherwise, calls syscall_terminate_process and returns false.
   SIZE must be a positive value. */
static bool 
syscall_validate_user_memory (const void *uaddr, size_t size)
{
  const void *current_page;

  ASSERT (thread_current ()->pagedir != NULL);
  ASSERT (size > 0);

  if (uaddr == NULL)
    {
      syscall_terminate_process ();
      return false;
    }
  /* Loop over every page in the queried block and check its validity. */
  for (current_page = pg_round_down (uaddr); 
       current_page <= pg_round_down ((const uint8_t *)uaddr + size); 
       current_page += PGSIZE)
    {
      if (!is_user_vaddr (current_page) 
          || pagedir_get_page (thread_current ()->pagedir, 
                               current_page) == NULL)
        {
          syscall_terminate_process ();
          return false;
        }
    } 
  return true;
}

/* Returns true if UADDR points to a valid null-terminated string in the
   current thread's page directory or if no null-terminator is found
   in the MAX_SIZE valid chars following UADDR. Otherwise, calls
   syscall_terminate_process and returns false.
   MAX_SIZE must be positive. */
static bool 
syscall_validate_user_string (const char *uaddr, size_t max_size)
{
  const char *caddr = uaddr;

  ASSERT (thread_current ()->pagedir != NULL);
  ASSERT (max_size > 0);

  for (; caddr != uaddr + max_size + 1; ++caddr)
    {
      if (!syscall_validate_user_memory (caddr, sizeof (char)))
        return false;
      if (*caddr == '\0')
        return true;
    }
  return true;
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
  NOT_REACHED ();
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
  const char *cmd_line = syscall_get_arg (f, 1);
  /* TODO */
  tid_t tid = process_execute (cmd_line);
  f->eax = tid;
}

static void 
syscall_wait (struct intr_frame *f)
{
  tid_t pid = syscall_get_arg (f, 1);
  NOT_REACHED ();
}

static void 
syscall_create (struct intr_frame *f)
{
  const char *file = syscall_get_arg (f, 1);
  uint32_t initial_size = syscall_get_arg (f, 2);
  NOT_REACHED ();
}

static void 
syscall_remove (struct intr_frame *f)
{
  const char *file = syscall_get_arg (f, 1);
  NOT_REACHED ();
}

static void 
syscall_open (struct intr_frame *f)
{
  const char *file = syscall_get_arg (f, 1);
  NOT_REACHED ();
}

static void 
syscall_filesize (struct intr_frame *f)
{
  int32_t fd = syscall_get_arg (f, 1);
  NOT_REACHED ();
}

static void 
syscall_read (struct intr_frame *f)
{
  int32_t fd = syscall_get_arg (f, 1);
  void *buffer = syscall_get_arg (f, 2);
  uint32_t size = syscall_get_arg (f, 3);
  NOT_REACHED ();
}

static void 
syscall_write (struct intr_frame *f)
{
  int32_t fd = syscall_get_arg (f, 1);
  const char *buffer = syscall_get_arg (f, 2);
  uint32_t stride, size = syscall_get_arg (f, 3);
  /* Verify that the entire buffer is valid user memory. */
  if (!pagedir_is_user_accessible (thread_current ()->pagedir, buffer, size))
    {
      /* TODO - Terminate process. */
      ASSERT (false);
    }
  /* For FD==1, print to the console strides of the buffer. */
  while (fd == 1 && size > 0)
    {
      size -= stride = size > 256 ? 256 : size;
      putbuf(buffer, stride);
      buffer += stride;
    }
  /* TODO - implement write for regular files. */
}

static void 
syscall_seek (struct intr_frame *f)
{
  int32_t fd = syscall_get_arg (f, 1);
  uint32_t position = syscall_get_arg (f, 2);
  NOT_REACHED ();
}

static void 
syscall_tell (struct intr_frame *f)
{
  int32_t fd = syscall_get_arg (f, 1);
  NOT_REACHED ();
}

static void 
syscall_close (struct intr_frame *f)
{
  int32_t fd = syscall_get_arg (f, 1);
  NOT_REACHED ();
}
