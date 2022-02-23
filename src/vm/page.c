#include "vm/page.h"
#include "vm/frame.h"
#include "threads/thread.h"
#include "userprog/pagedir.h"



uint8_t *
page_alloc (uint8_t *uaddr)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  if (pagedir_get_page (t->pagedir, uaddr) == NULL
      && pagedir_set_page (t->pagedir, uaddr, NULL, true))
    {
      pagedir_set_present (t->pagedir, uaddr, false);
      return uaddr;
    }
  return NULL;
}

void
page_set_writable (uint8_t *uaddr, bool writable)
{
  // TODO - and remember to call it
}

void
page_free (uint8_t *uaddr)
{
  // TODO - remove from tables
}

