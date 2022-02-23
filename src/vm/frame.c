
/* TODO -
 * - init table by loading all available frames
 * - provide a free frame when needed
 */
#include <debug.h>
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "vm/frame.h"

struct frame_table ft;

void
frame_init (void)
{
  void *upage;
  struct frame_entry *entry;

  list_init (&ft.free_frames);
  list_init (&ft.allocated_frames);
  /* Query palloc_get_page until user pool is exhausted. */
  while ((upage = palloc_get_page (PAL_USER)))
    {
      entry = malloc (sizeof (struct frame_entry));
      ASSERT (entry != NULL); /* Otherwise fails to build frame table. */
      entry->frame_addr = upage;
      list_push_back (&ft.free_frames, &entry->elem);
    }
}

void *
frame_alloc (struct page_entry *page)
{
  struct frame_entry *entry;

  ASSERT (!list_empty (&ft.free_frames));

  entry = list_entry (list_pop_front (&ft.free_frames), 
                      struct frame_entry, elem);
  entry->page = page;
  list_push_back (&ft.allocated_frames, &entry->elem);
  return entry->frame_addr;
}

