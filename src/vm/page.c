#include "vm/page.h"
#include <stdio.h>
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "userprog/pagedir.h"


static hash_less_func page_less;
static hash_hash_func page_hash;

/* Hash function that hashes a page's uaddr into its page_table hash table. */
static unsigned 
page_hash (const struct hash_elem *e, void *aux UNUSED)
{
  const struct page *p = hash_entry (e, struct page, hash_elem);
  return hash_bytes (&p->uaddr, sizeof p->uaddr);
}

/* Hash comparison function necessary to use page_table as a hash table. */
static bool 
page_less (const struct hash_elem *a_,
           const struct hash_elem *b_,
           void *aux UNUSED)
{
  const struct page *a = hash_entry (a_, struct page, hash_elem);
  const struct page *b = hash_entry (b_, struct page, hash_elem);
  return a->uaddr < b->uaddr;
}

/* Finds page with uaddr ADDRESS in T's page_table hash table returning 
   its address or NULL if not found. */
struct page *
page_lookup (struct thread *t, void *address)
{
  struct page p;
  struct hash_elem *e;
  struct page *result;

  p.uaddr = address;
  e = hash_find (&t->page_table, &p.hash_elem);
  result = e != NULL ? hash_entry (e, struct page, hash_elem) : NULL;
  return result;
}

/* Initializes the page_table hash table for a thread T.
   Called in process.c/load before any memory allocation. */
bool
page_table_init (struct thread *t)
{
  bool success;
  
  t->page_table_lock = malloc (sizeof (struct lock));
  if (t->page_table_lock == NULL)
    return false;
  lock_init (t->page_table_lock);
  lock_acquire (t->page_table_lock);
  success = hash_init (&t->page_table, page_hash, page_less, NULL);
  lock_release (t->page_table_lock);
  return success;
}

/* Allocates a page with user address UADDR in the current thread's
   page_table. Invalidates the PTE for that page such that a future
   pagefault would load the page lazily. Returns NULL if the page is 
   already mapped or UADDR on success. Thread-safe. */
void *
page_alloc (void *uaddr)
{
  struct thread *t = thread_current ();
  void *paddr = pg_round_down (uaddr);
  struct page *p;

  ASSERT (is_user_vaddr (uaddr));

  lock_acquire (t->page_table_lock);
  /* Verify that there's not already a page at that virtual
     address, then create a new page. */
  p = page_lookup (t, paddr);
  if (p != NULL)  /* A page already exists with this address. */
    {
      uaddr = NULL;
      goto done;
    }
  p = malloc (sizeof (struct page));
  if (p == NULL)  /* Failed to allocate a page entry. */
    {
      uaddr = NULL;
      goto done;
    }
  lock_init (&p->lock);
  p->uaddr = paddr;
  p->thread = t;
  p->location = NEW;
  p->frame = NULL;
  p->writable = true;
  pagedir_clear_page (t->pagedir, paddr);
  hash_insert (&t->page_table, &p->hash_elem);
done:
  lock_release (t->page_table_lock);
  return uaddr;
}

/* Sets the writable bit to WRITABLE for page at UADDR
   in the curren thread and updates its PTE accordingly. */
void
page_set_writable (void *uaddr, bool writable)
{
  struct thread *t = thread_current ();
  struct page *p;
  void *kpage, *upage = pg_round_down (uaddr);

  ASSERT (is_user_vaddr (uaddr));

  lock_acquire (t->page_table_lock);
  p = page_lookup (t, uaddr);
  if (p != NULL)
    {
      p->writable = writable;
      /* Update the pagedir if the page is present. */
      kpage = pagedir_get_page (t->pagedir, upage);
      if (kpage != NULL)
        pagedir_set_writable (t->pagedir, upage, writable);
    }
  lock_release (t->page_table_lock);
}

/* Removes a page from the current thread's page_table and
   frees its frame. */
void
page_free (void *uaddr)
{
  // TODO - remove from tables
}

/* Attempts to resolve a pagefault on FAULT_ADDR by allocating
   a frame for it. Returns true on success and false if
   FAULT_ADDR is not a valid address in the first place. */
bool 
page_resolve_fault (void *fault_addr)
{
  struct thread *t = thread_current ();
  struct page *page;
  struct frame *frame;

  ASSERT (is_user_vaddr (fault_addr));

  page = page_lookup (t, pg_round_down (fault_addr));
  /* Fault address is not mapped. */
  if (page == NULL) 
    return false;
  /* Page is already in a frame or is corrupted. */
  if (page->location == FRAME || page->location == CORRUPTED)
    return false;
  /* Allocate a pinned frame to resolve the pagefault into. */
  frame = frame_alloc ();
  frame->page = page;
  page->frame = frame;
  /* Populate the frame with the appopriate data. */
  switch (page->location)
    {
      case NEW:
        /* NEW pages don't need to be populated with any data. */
        printf ("\n>>> Paging in a NEW page\n.");
        break;
      case SWAP:
        /* Swap-in the page data. */
        printf ("\n>>> Paging in a SWAP page\n.");
        if (!swap_in (frame, page->swap_slot))
          {
            /* Failing to swap-in a page means that we lost it forever. */
            page->location = CORRUPTED;
            frame_free (frame);
            return false;
          }
        break;
      /* TODO - add other cases (e.g. mmaped files). */
      default:
        PANIC ("Failed to page-in at address %p!", page->uaddr);
    }
  if (!pagedir_set_page (t->pagedir, page->uaddr, frame->kaddr, 
                         page->writable))
    {
      frame_free (frame);
      return false;
    }
  else
    {
      /* Successfully paged-in and can be unpinned. */
      page->location = FRAME;
      frame->pinned = false;
      return true;
    }
}

/* Evicts PAGE by swapping its frame out. PAGE need not belong
   to the current thread and it's thread-safe. Returns true
   when the page is no longer on memory or false on failure.
   
   TODO: implement other forms of eviction depending on the 
         nature of PAGE (e.g. write to filesystem). */
bool
page_evict (struct page *page)
{
  bool success;

  /* The page could be already evicted. */
  if (page->location != FRAME)
    return true;
  /* Swap-out the page. */
  lock_acquire (&page->lock);
  page->swap_slot = swap_out (page->frame);
  if (page->swap_slot != SWAP_ERROR)
    {
      pagedir_clear_page (page->thread->pagedir, page->uaddr);
      page->location = SWAP;
      success = true;
    }
  else
    success = false;
  lock_release (&page->lock);
  return success;
}

