
/* TODO -
 * - init table by loading all available frames
 * - provide a free frame when needed
 */
#include <debug.h>
#include <stdio.h>
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "userprog/pagedir.h"
#include "vm/frame.h"

struct frame_table ft;

/* Initializes the frame table FT by calling palloc_get_page on all
   user pages and storing them as free frames for future use. */
void
frame_init (void)
{
  void *upage;
  struct frame *frame;

  list_init (&ft.free_frames);
  list_init (&ft.allocated_frames);
  /* Query palloc_get_page until user pool is exhausted. */
  while ((upage = palloc_get_page (PAL_USER)))
    {
      frame = malloc (sizeof (struct frame));
      ASSERT (frame != NULL); /* Otherwise fails to build frame table. */
      frame->kaddr = upage;
      list_push_back (&ft.free_frames, &frame->elem);
    }
}

/* Allocates a free frame for THREAD's PAGE and updates THREAD's
   pagedir to reflect the new physical address. Useful when resolving
   a pagefault.

   TODO: resolve other cases where PAGE is in swap etc. 
   TODO: resolve eviction when no free frames are available. */
bool
frame_alloc (struct thread *thread, struct page *page)
{
  struct frame *frame;

  ASSERT (!list_empty (&ft.free_frames));

  frame = list_entry (list_pop_front (&ft.free_frames), struct frame, elem);
  list_push_back (&ft.allocated_frames, &frame->elem);
  frame->page = page;
  frame->thread = thread;
  return pagedir_set_page (thread->pagedir, page->uaddr, frame->kaddr, 
                           page->writable);
}

