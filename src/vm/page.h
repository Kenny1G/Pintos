#ifndef VM_PAGE_H
#define VM_PAGE_H
#include <stdbool.h>
#include <hash.h>
#include "threads/thread.h"

enum page_location
  {
    NEW,
    FRAME,
    SWAP,
    FILE
  };

struct page
  {
    struct hash_elem hash_elem;
    void *uaddr;
    enum page_location location;
    bool writable;
  };

bool page_table_init (struct thread *t);
void *page_alloc (void *uaddr);
void page_free (void *uaddr);
void page_set_writable (void *uaddr, bool writable);
struct page *page_lookup (struct thread *t, void *address);


#endif /* vm/page.h */
