#ifndef VM_FRAME_H
#define VM_FRAME_H
#include <list.h>
#include <stdbool.h>
#include "vm/page.h"

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
    struct page *page;            /* The page mapped to this frame. */
    bool pinned;                  /* Don't evict when pinned. */
  };

void frame_init (void);
struct frame *frame_alloc (void);
void frame_pin (struct frame *frame);
void frame_unpin (struct frame *frame);
void frame_free (struct frame *frame);
bool frame_evict (struct frame *frame);


#endif /* vm/frame.h */
