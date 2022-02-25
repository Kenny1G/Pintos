#include "vm/frame.h"
#include <debug.h>
#include <stdio.h>
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/synch.h"

/* Frame table keeping track of all frames in the system. */
static struct frame_table ft;
/* Lock guarding operations on ft. */
static struct lock frame_table_lock;

void 
frame_free (struct frame *frame)
{
  return;
}

/* Initializes the frame table FT by calling palloc_get_page on all
   user pages and storing them as free frames for future use. */
void
frame_init (void)
{
  void *upage;
  struct frame *frame;

  lock_init (&frame_table_lock);
  lock_acquire (&frame_table_lock);
  list_init (&ft.free_frames);
  list_init (&ft.allocated_frames);
  /* Query palloc_get_page until user pool is exhausted. */
  while ((upage = palloc_get_page (PAL_USER)))
    {
      frame = malloc (sizeof (struct frame));
      ASSERT (frame != NULL); /* Otherwise fails to build frame table. */
      frame->kaddr = upage;
      frame->page = NULL;
      frame->pinned = false;
      list_push_back (&ft.free_frames, &frame->elem);
    }
  lock_release (&frame_table_lock);
}

/* Allocates a free frame for THREAD's PAGE and updates THREAD's
   pagedir to reflect the new physical address. Useful when resolving
   a pagefault. The returned frame is pinned by default.

   TODO: resolve other cases where PAGE is in swap etc. 
   TODO: resolve eviction when no free frames are available. */
struct frame *
frame_alloc (void)
{
  struct frame *frame;

  ASSERT (!list_empty (&ft.free_frames));
  
  lock_acquire (&frame_table_lock);
  frame = list_entry (list_pop_front (&ft.free_frames), struct frame, elem);
  list_push_back (&ft.allocated_frames, &frame->elem);
  frame->pinned = true;
  lock_release (&frame_table_lock);
  return frame;
}

/* Evicts FRAME by calling page_evict on its page if it exists. 
   Returns false if FRAME is pinned or page_evict fails and
   true otherwise. */
bool
frame_evict (struct frame *frame)
{
  printf ("\n>> Evicting Frame %p\n", frame->kaddr);
  /* Will not evict a pinned frame. */
  if (frame->pinned)
    return false;
  /* Evict page if it exists. */
  if (frame->page != NULL && !page_evict (frame->page))
    return false;
  frame->page = NULL;
  /* Reflect the change on the frame table. */
  lock_acquire (&frame_table_lock);
  list_remove (&frame->elem);
  list_push_back (&ft.free_frames, &frame->elem);
  lock_release (&frame_table_lock);
  printf ("\n>> Eviction complete.\n");
  return true;
}

