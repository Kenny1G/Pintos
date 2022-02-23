#include <list.h>
#include <stdbool.h>
#include "vm/page.h"
#include "threads/thread.h"

struct frame_table 
  {
    struct list free_frames;
    struct list allocated_frames;
  };

struct frame
  {
    struct list_elem elem;
    void *kaddr;
    struct thread *thread;
    struct page *page;
  };

void frame_init (void);
bool frame_alloc (struct thread *, struct page *);
