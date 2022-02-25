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

/* FOR TESTING. */
static void
frame_evict_all (void)
{
  struct frame *frame = NULL;

  lock_acquire (&frame_table_lock);
  if (!list_empty (&ft.allocated_frames))
    frame = list_entry (list_front (&ft.allocated_frames), struct frame, elem);
  lock_release (&frame_table_lock);
  if (frame != NULL)
    {
      frame_evict (frame);
      frame_evict_all ();
    }
}

/* Helper for frame_alloc. Chooses which frame to evict and evicts it.
   Assumes that frame_table_lock is acquired by the current thread.

   TODO - implement a better eviction strategy. */
static struct frame *
frame_pick_and_evict
{
  ASSERT (lock_held_by_current_thread (&frame_table_lock));

  /* try palloc_get_page and free_frames and evict allocated_frames. */
  /* implement pinning and unpinning and use them. */
}

/* Allocates a free frame for THREAD's PAGE and updates THREAD's
   pagedir to reflect the new physical address. Useful when resolving
   a pagefault. The returned frame is pinned by default. */
struct frame *
frame_alloc (void)
{
  struct frame *frame;

  lock_acquire (&frame_table_lock);
  if (!list_empty (&ft.free_frames))
    frame = list_entry (list_pop_front (&ft.free_frames), struct frame, elem);
  else
    frame = frame_pick_and_evict ();
  frame->pinned = true;
  list_push_back (&ft.allocated_frames, &frame->elem);
  lock_release (&frame_table_lock);
  return frame;
}

/* Evicts FRAME by calling page_evict on its page if it exists. 
   Returns false if FRAME is pinned or page_evict fails and
   true otherwise. Assumes frame_table_lock is acquired. */
bool
frame_evict (struct frame *frame)
{
  ASSERT (lock_held_by_current_thread (&frame_table_lock));
  
  printf ("\n>> Evicting Frame %p\n", frame->kaddr);
  /* Will not evict a pinned frame. */
  if (frame->pinned)
    return false;
  /* Evict page if it exists. */
  if (frame->page != NULL && !page_evict (frame->page))
    return false;
  frame->page = NULL;
  /* Remove the frame from list of allocated frames. */
  list_remove (&frame->elem);
  printf ("\n>> Eviction complete.\n");
  return true;
}

