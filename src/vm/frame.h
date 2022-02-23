#ifndef VM_FRAME_H
#define VM_FRAME_H
#include <list.h>
#include <stdbool.h>
#include "vm/page.h"
#include "threads/thread.h"

/* Keeps track of free and allocated frames in the system. */
struct frame_table 
  {
    struct list free_frames;      /* Available for allocation. */
    struct list allocated_frames; /* Candidates for eviction. */
  };

/* An entry in either lists of frame_table reflecting one frame. */
struct frame
  {
    struct list_elem elem;        /* Elem in either frame_table lists. */
    void *kaddr;                  /* Physical address = kernel address. */
    struct thread *thread;        /* Thread of pagedir containing PAGE. */
    struct page *page;            /* The page mapped to this frame. */
  };

void frame_init (void);
bool frame_alloc (struct thread *, struct page *);


#endif /* vm/frame.h */
