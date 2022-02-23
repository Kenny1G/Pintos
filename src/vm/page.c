#include <stdio.h>
#include "vm/page.h"
#include "vm/frame.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "userprog/pagedir.h"


static hash_less_func page_less;
static hash_hash_func page_hash;


static unsigned 
page_hash (const struct hash_elem *e, void *aux UNUSED)
{
  const struct page *p = hash_entry (e, struct page, hash_elem);
  return hash_bytes (&p->uaddr, sizeof p->uaddr);
}

static bool 
page_less (const struct hash_elem *a_,
           const struct hash_elem *b_,
           void *aux UNUSED)
{
  const struct page *a = hash_entry (a_, struct page, hash_elem);
  const struct page *b = hash_entry (b_, struct page, hash_elem);
  return a->uaddr < b->uaddr;
}

struct page *
page_lookup (struct thread *t, void *address)
{
  struct page p;
  struct hash_elem *e;

  p.uaddr = address;
  e = hash_find (&t->page_table, &p.hash_elem);
  return e != NULL ? hash_entry (e, struct page, hash_elem) : NULL;
}

bool
page_table_init (struct thread *t)
{
  return hash_init (&t->page_table, page_hash, page_less, NULL);
}

void *
page_alloc (void *uaddr)
{
  struct thread *t = thread_current ();
  struct page *p;

  ASSERT (is_user_vaddr (uaddr));

  /* Verify that there's not already a page at that virtual
     address, then create a new page. */
  p = page_lookup (t, uaddr);
  if (p != NULL)  /* A page already exists with this address. */
    return NULL;
  p = malloc (sizeof (struct page));
  if (p == NULL)  /* Failed to allocate a page entry. */
    return NULL;
  p->uaddr = uaddr;
  p->location = NEW;
  p->writable = true;
  pagedir_set_present (t->pagedir, uaddr, false);
  hash_insert (&t->page_table, &p->hash_elem);
  return uaddr;
}

void
page_set_writable (void *uaddr, bool writable)
{
  struct thread *t = thread_current ();
  struct page *p;
  void *kpage, *upage = pg_round_down (uaddr);

  ASSERT (is_user_vaddr (uaddr));

  p = page_lookup (t, uaddr);
  if (p != NULL)
    {
      p->writable = writable;
      /* Update the pagedir if the page is present. */
      kpage = pagedir_get_page (t->pagedir, upage);
      if (kpage != NULL)
        pagedir_set_page (t->pagedir, upage, kpage, writable);
    }

}

void
page_free (void *uaddr)
{
  // TODO - remove from tables
}

bool 
page_resolve_fault (void *fault_addr)
{
  struct thread *t = thread_current ();
  struct page *page;

  ASSERT (is_user_vaddr (fault_addr));

  page = page_lookup (t, pg_round_down (fault_addr));
  if (page == NULL)  /* Fault address is not mapped. */
    return false;
  return frame_alloc (t, page);
}

