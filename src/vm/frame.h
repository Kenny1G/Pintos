#include <list.h>
#include "vm/page.h"

struct frame_table 
  {
    struct list free_frames;
    struct list allocated_frames;
  };

struct frame_entry
  {
    struct list_elem elem;
    void *frame_addr;
    struct page_entry *page;
  };

void frame_init (void);
void *frame_alloc (struct page_entry *);
