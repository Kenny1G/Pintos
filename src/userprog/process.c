#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "vm/page.h"
#include "vm/frame.h"
#include "syscall.h"

/* Processes acquire this lock when modifying their parent or children
 to prevent race conditions when multiple processes exit at the same time. */
static struct lock process_child_lock;

/* Used to pass info concerning the process' name and arguments from
   process_execute() to start_process() to load(). Also holds a
   semaphore that ensures the process does not start running until
   the args have been fully loaded into the stack. */
struct process_info {
  char *cmd_line;           /* Pointer to the cmd line of args on the heap. */
  char *program_name;       /* Pointer to the program name (first arg). */
  struct semaphore loaded;  /* Prevents the thread from running until process
                               info is loaded in the stack */
  struct process_child *inparent;
                          /* Pointer to record of current process in parent. */
  bool load_success;
};

static thread_func start_process NO_RETURN;
static bool load (struct process_info *p_info, void (**eip) (void), void **esp);
static bool pass_args_to_stack(struct process_info *p_info, void **esp);
static bool stack_push(void **esp, void *data, size_t size);

void
process_init (void)
{
  lock_init(&process_child_lock);
}

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name) 
{
  tid_t tid;

  struct process_info *p_info = calloc (1, sizeof(struct process_info));
  struct process_child *p_child = calloc (1, sizeof(struct process_child));
  if (p_info == NULL || p_child == NULL)
    {
      tid = TID_ERROR;
      goto done;
    }

  /* Initialize process semaphore. */
  sema_init (&p_info->loaded, 0);
  sema_init (&p_child->exited, 0);

  /* Add a pointer to child's record in parent to link them after creating the thread. */
  p_child->thread = NULL;
  p_info->inparent = p_child;
  lock_acquire (&process_child_lock);
  list_push_back (&thread_current ()->process_children, &p_child->elem);
  lock_release (&process_child_lock);

  /* Make a copy of FILE_NAME (the command line).
     Otherwise there's a race between the caller and load(). */
  p_info->cmd_line = palloc_get_page (0);
  if (p_info->cmd_line == NULL) 
    {
      tid = TID_ERROR;
      goto done;
    }
  strlcpy (p_info->cmd_line, file_name, PGSIZE);
  
  /* Parse out program name without modifying str */
  size_t len_prog_name = strcspn(p_info->cmd_line, " ");
  p_info->program_name = calloc (sizeof(char), len_prog_name + 1);
  if (p_info->program_name == NULL)
    {
      tid = TID_ERROR;
      goto done;
    }
  memcpy(p_info->program_name, p_info->cmd_line, len_prog_name);

  /* Create a new thread to execute FILE_NAME. */
  tid = thread_create (p_info->program_name, PRI_DEFAULT, start_process,
                       p_info);
  sema_down (&p_info->loaded);

done: /* Arrives here on success or error. */
  if (p_info->load_success == false)
    tid = TID_ERROR;
  if (tid == TID_ERROR)
    {
      if (p_child != NULL)
        list_remove (&p_child->elem);
      free (p_child);
    }
  else
    p_child->tid = tid;
  if (p_info != NULL)
    palloc_free_page (p_info->cmd_line);
  free (p_info);
  return tid;
}

static bool
process_elem_tid_equal (struct list_elem *elem, void *aux)
{
  struct process_child *child = list_entry (elem, struct process_child, elem);
  return child->tid == *(tid_t *)aux;
}

static bool
process_elem_fd_equal (struct list_elem *elem, void *aux)
{
  struct process_fd * fd = list_entry(elem, struct process_fd, list_elem);
  return fd->id == *(int *)aux;
}

struct process_fd *
process_get_fd(struct thread *t, int id)
{
  struct list_elem *e;
  if (!list_empty(&t->process_fd_table))
    {
      e = list_find(&t->process_fd_table, process_elem_fd_equal,
                    &id);
      return list_entry(e, struct process_fd, list_elem);
    }
  return NULL;
}

int
process_new_fd(struct thread *t, struct file *file, char* file_name)
{
  int id = t->process_fd_next++;
  struct process_fd *fd = malloc (sizeof(struct process_fd));
  if (fd == NULL) return -1;

  fd->id = id;
  list_push_back(&t->process_fd_table, &fd->list_elem);
  fd->file = file;
  fd->file_name = file_name;
  return id;
}

void
process_remove_fd(struct thread *t, int id)
{
  struct process_fd *fd = process_get_fd(t, id);
  if (fd == NULL) return;
  list_remove(&fd->list_elem);
  free(fd);
}

/* A thread function that loads a user process and starts it
   running. */
static void
start_process (void *process_info)
{
  struct process_info *p_info = (struct process_info *) process_info;
  struct thread *cur = thread_current ();
  struct intr_frame if_;
  bool success = false;

  /* Set up received member values. */
  cur->process_fn = p_info->program_name;
  lock_acquire (&process_child_lock);
  cur->inparent = p_info->inparent;
  if (cur->inparent != NULL)
      cur->inparent->thread = cur;
  lock_release (&process_child_lock);

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;

  /* Open the file and prevent writes */
  struct file* file = filesys_open (p_info->program_name);
  if (file != NULL) file_deny_write (file);

  success = load (p_info, &if_.eip, &if_.esp);
  sema_up (&p_info->loaded);

  /* If load failed, quit. */
  if (!success) 
    {
      if (file != NULL) file_allow_write (file);
      thread_current ()->process_exit_code = -1;
      thread_exit ();
    }
  else
    thread_current ()-> exec_file = file;

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting. */
int
process_wait (tid_t child_tid) 
{
  struct list_elem *child_elem;
  struct process_child *child;
  int exit_code;

  lock_acquire (&process_child_lock);
  child_elem = list_find(&thread_current ()->process_children, 
                         process_elem_tid_equal, &child_tid);
  lock_release (&process_child_lock);

  if (child_elem != NULL)
    {
      child = list_entry (child_elem, struct process_child, elem);
      sema_down (&child->exited);
      lock_acquire (&process_child_lock);
      exit_code = child->exit_code;
      list_remove (&child->elem);
      free (child);
      lock_release (&process_child_lock);
      return exit_code;
    }
  else
    return -1;
}

/* Free the current process's resources and print its exit code. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  struct list_elem *curr_child_elem;
  struct process_child *curr_child;
  uint32_t *pd;

  lock_acquire (&process_child_lock);
  /* Update the parent (if exists) that this child has exited. */
  if (cur->inparent != NULL)
    {
      cur->inparent->exit_code = cur->process_exit_code;
      cur->inparent->thread = NULL;
      sema_up (&cur->inparent->exited);
    }
  if (cur->process_fn != NULL)
    {
      printf ("%s: exit(%d)\n", cur->process_fn, cur->process_exit_code);
      free (cur->process_fn);
    }
  /* Orphan all child processes. */
  for (curr_child_elem = list_begin (&cur->process_children); 
       curr_child_elem != list_end (&cur->process_children);
       curr_child_elem = list_remove (curr_child_elem))
    {
      curr_child = list_entry (curr_child_elem, struct process_child, elem);
      if (curr_child->thread != NULL)
        curr_child->thread->inparent = NULL;
      free (curr_child);
    }
  lock_release (&process_child_lock);

  /* Allow writes to the exec file again */
  if (cur->exec_file != NULL)
  {
    file_allow_write (cur->exec_file);
    file_close (cur->exec_file);
  }

  /* Close open file descriptors */
  struct list* fd_table = &thread_current()->process_fd_table;
  struct process_fd *fd;
  size_t n = list_size (fd_table);
  struct list_elem *e = list_begin (fd_table);
  for (size_t i = 0; i < n; ++i) 
    {
      fd = list_entry (e, struct process_fd, list_elem);
      e = list_next (e);
      syscall_close_helper (fd->id); 
    }


  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL) 
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (struct process_info *p_info, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory and supplemntal page table. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL || !page_table_init (t)) 
    goto done;
  
  process_activate ();

  /* Open executable file. */
  file = filesys_open (p_info->program_name);
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", p_info->program_name);
      goto done; 
    }

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", p_info->program_name);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp))
    goto done;
  /* pass args: push onto the stack */
  if (!pass_args_to_stack(p_info, esp))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

// MAY NEED modification
 done:
  /* We arrive here whether the load is successful or not. */
  p_info->load_success = success;
  if (file != NULL) file_close (file);
  return success;
}

/* load() helpers. */

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  /* Test Swap: */
  printf ("\n >> Swap test begins.\n");
  uint8_t *swap_test = page_alloc (upage);
  printf ("\n before: %d", *swap_test);
  *swap_test = 7;
  printf ("\n before 2: %d", *swap_test);
  frame_evict (page_lookup (thread_current (), swap_test)->frame);
  printf ("\n after: %d", *swap_test);
  printf ("\n >> Swap test ended.\n");
  /* End test swap. */

  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) 
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      /* Get a page of memory. */
      uint8_t *page = page_alloc (upage);
      if (page == NULL)
        return false;

      /* Load this page. */
      if (file_read (file, page, page_read_bytes) != (int) page_read_bytes)
        {
          page_free (page);
          return false; 
        }
      memset (page + page_read_bytes, 0, page_zero_bytes);
      page_set_writable (page, writable);
      
      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp) 
{
  uint8_t *page;

  page = page_alloc (((uint8_t *) PHYS_BASE) - PGSIZE);
  if (page != NULL) 
    {
      memset (page, 0, PGSIZE);
      *esp = PHYS_BASE;
      return true;
    }
  return false;
}

/* Push data onto the stack if in bounds */
static bool
stack_push(void **esp, void *data, size_t size) 
{
  /* Verify only writing within the bounds of the stack */
  if ((intptr_t)esp - size > 0)
    {
      *esp -= size;
      memcpy(*esp, data, size);
      return true;
    }
  return false;
}

/* Pass arguments to a new process. Parse arguments string and 
   push arguments and corresponding info onto the stack. */
static bool
pass_args_to_stack(struct process_info *p_info, void **esp)
{
  bool success = true;

  /* Parse command line into separate words and count args. */
  int argc = 0;
  char *token, *save_ptr;
  for (token = strtok_r (p_info->cmd_line, " ", &save_ptr); token != NULL;
       token = strtok_r (NULL, " ", &save_ptr))
    argc++;

  /* Create argv vector. */
  char *argv[argc];

  /* Push arguments onto the top of the stack. */
  token = p_info->cmd_line;
  for (int i = 0; i < argc; i++) 
    {
      /* Push onto the stack. */
      int size_arg = strlen(token) + 1;
      success = stack_push(esp, token, size_arg);

      /* Search for next token. */
      token = strchr(token, '\0') + 1;
      if (token[0] == ' ')
        token++;

      /* Save the stack location */
      argv[i] = *esp;
    }
  
  /* Round stack ptr down to a multiple of 4 so you're word aligned. */
  *esp = (void*) (((intptr_t) *esp) & 0xfffffffc);

  /* Starting with the nullptr, push address of every string. 
     (char * to memory we just put on the stack) */
  int null_ptr = 0;
  success = stack_push(esp, &null_ptr, sizeof(void *));
  for (int i = argc - 1; i >= 0; i--)
    success = stack_push(esp, &argv[i], sizeof(char *));
  /* Push argv (address argv[0]). */
  void *argv_0 = *esp;
  success = stack_push(esp, &argv_0, sizeof(void *));
  /* Push argc. */
  success = stack_push(esp, &(argc), sizeof(argc));
  /* Push the return address. */
  success = stack_push(esp, &null_ptr, sizeof(void *));
  return success;
}

bool process_mmap_add_page (struct process_mmap_entry *mmap, void* uaddr, 
                            unsigned offset)
{
}

void process_mmap_remove_page (struct process_mmap_entry *mmap)
{}
