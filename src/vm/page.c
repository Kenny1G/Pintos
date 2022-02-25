#include "vm/page.h"
#include <stdio.h>
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "userprog/pagedir.h"

static struct page *page_lookup (void *uaddr);
static void page_page_free (struct page *p);
static hash_less_func page_less;
static hash_hash_func page_hash;
static hash_action_func hash_page_free;

/* Initializes the page_table for the current thread.
   Called in process.c/load before any memory allocation. */
bool
page_table_init (void)
{
  struct thread *t = thread_current ();
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

/* Destroys the page_table for the current thread and frees all pages.
   Called in process.c/process_exit for frame reclamation. */
void
page_table_destroy (void)
{
  struct thread *t = thread_current ();

  lock_acquire (t->page_table_lock);
  /* Destroy the page table hash table to free its memory.
     hash_page_free will call page_page_free on each entry. */
  hash_destroy (&t->page_table, hash_page_free);
  /* Release and destroy the page table lock. */
  lock_release (t->page_table_lock);
  free (t->page_table_lock);
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
  p = page_lookup (paddr);
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
  p->evict_to = SWAP; /* TODO - modify to FILE for mmap. */
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
  void *upage = pg_round_down (uaddr);
  struct page *p;

  ASSERT (is_user_vaddr (uaddr));

  lock_acquire (t->page_table_lock);
  p = page_lookup (uaddr);
  if (p != NULL)
    {
      lock_acquire (&p->lock);
      p->writable = writable;
      /* Update the pagedir if the page is present. */
      if (p->location == FRAME)
        pagedir_set_writable (t->pagedir, upage, writable);
      lock_release (&p->lock);
    }
  lock_release (t->page_table_lock);
}

/* Removes the page P from the current thread's page_table and
   frees its frame evicting its data when appropritate. */
static void
page_page_free (struct page *p)
{
  struct thread *t = thread_current ();
  
  lock_acquire (&p->lock);
  if (p != NULL)
    {
      /* Free up the resources for this page's data. */
      switch (p->location)
        {
          case SWAP:
            /* Pages in swap should be discarded. */
            swap_free (p->swap_slot);
            break;
          case FRAME:
            /* Evict the page data to the appropriate location (e.g. FILE). */
            if (p->evict_to != SWAP)
              page_evict (p);
            frame_free (p->frame);
            break;
          /* TODO - implement other page locations. */
          default:
            break;
        }
    }
  /* Remove page from the page table hash table. */
  hash_delete (&t->page_table, &p->hash_elem);
  pagedir_clear_page (t->pagedir, p->uaddr);
  lock_release (&p->lock);
  /* Free up the memory for this page table entry. */
  free (p);
}

/* Removes the page at UADDR from the current thread's page_table and
   frees its frame evicting its data when appropritate. */
void
page_free (void *uaddr)
{
  struct thread *t = thread_current ();
  struct page *p;

  lock_acquire (t->page_table_lock);
  p = page_lookup (uaddr);
  page_page_free (p);
  lock_release (t->page_table_lock);
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

  page = page_lookup (pg_round_down (fault_addr));
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
        printf ("\n>>> Paging in a NEW page.\n");
        break;
      case SWAP:
        /* Swap-in the page data. */
        printf ("\n>>> Paging in a SWAP page.\n");
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

/* Finds page with uaddr UADDR in the curren thread's page_table returning 
   the address of its struct page or NULL if not found. */
static struct page *
page_lookup (void *uaddr)
{
  struct thread *t = thread_current ();
  struct page p;
  struct hash_elem *e;
  struct page *result;

  p.uaddr = pg_round_down (uaddr);
  e = hash_find (&t->page_table, &p.hash_elem);
  result = e != NULL ? hash_entry (e, struct page, hash_elem) : NULL;
  return result;
}

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

/* Helper function that calls page_page_free on a hash 
   element. Useful when destroying the hash table. */
static void
hash_page_free (struct hash_elem *e, void *aux UNUSED)
{
  struct page *page;

  page = hash_entry (e, struct page, hash_elem);
  page_page_free (page);
}
