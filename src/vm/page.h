#ifndef VM_PAGE_H
#define VM_PAGE_H
#include <stdbool.h>
#include <hash.h>
#include "threads/thread.h"

/* Where to load the page from. */
enum page_location
  {
    NEW,      /* Unintialized page. */
    FRAME,    /* Page already in a frame. */
    SWAP,     /* Page has been swapped out. */
    FILE      /* Page is in mapped file. */
  };

/* A page in a threads page_table. */
struct page
  {
    struct hash_elem hash_elem;   /* In thread's page_table hash table. */ 
    void *uaddr;                  /* User virtual address, page_table key. */
    enum page_location location;  /* Where to load the page from. */
    bool writable;                /* RW vs RO. */
  };

bool page_table_init (struct thread *t);
void *page_alloc (void *uaddr);
void page_free (void *uaddr);
void page_set_writable (void *uaddr, bool writable);
bool page_resolve_fault (void *fault_addr);
struct page *page_lookup (struct thread *t, void *address);


#endif /* vm/page.h */
