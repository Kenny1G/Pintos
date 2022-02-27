#ifndef VM_PAGE_H
#define VM_PAGE_H
#include <stdbool.h>
#include <stddef.h>
#include <hash.h>
#include "threads/thread.h"
#include "threads/synch.h"
#include "vm/frame.h"
#include "vm/swap.h"

/* Where to load the page from. */
enum page_location
  {
    NEW,         /* Unintialized page. */
    FRAME,       /* Page already in a frame. */
    SWAP,        /* Page has been swapped out. */
    FILE,        /* Page is in mapped file. */
    CORRUPTED,   /* Page lost. */
  };

/* A page in a threads page_table. */
struct page
  {
    struct hash_elem hash_elem;   /* In thread's page_table hash table. */ 
    struct lock lock;
    struct thread *thread;
    void *uaddr;                  /* User virtual address, page_table key. */
    enum page_location location;  /* Where to load the page from. */
    bool writable;                /* RW vs RO. */
    struct frame *frame;
    bool pinned;
    size_t swap_slot;
    enum page_location evict_to;  /* Where to evict the frame (e.g. FILE). */
    size_t file_zero_bytes;       /* For MMAP: Number of bytes that stick out*/
  };

struct page_mmap 
  {
    mapid_t id;                 /* ID of mmap*/
    struct list_elem list_elem; /* List element to place mmap in list */
    struct file *file;          /* File backing mmap */
    size_t file_size;           /* Size of above */
    struct list mmap_pages;     /* List of pages mapped to this mmap */
  };

/* Wrapper struct for a page in an mmap*/
struct page_mmap_wrapper 
  {
    struct list_elem list_elem; /* List element to put page in mmap_pages */
    void* page_addr;            /* Virtual Address of page */
  };

bool page_table_init (void);
void page_table_destroy (void);
void *page_alloc (void *uaddr);
void page_free (void *uaddr);
bool page_evict (struct page *page);
void page_pin (void *uaddr);
void page_unpin (void *uaddr);
void page_set_writable (void *uaddr, bool writable);
bool page_resolve_fault (void *fault_addr);
bool page_add_to_mmap (struct page_mmap *mmap, void* uaddr, 
                       unsigned offset);
void page_delete_mmap (struct page_mmap *mmap);
struct page_mmap *page_get_mmap (struct thread *t, mapid_t id);

#endif /* vm/page.h */
