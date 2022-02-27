#include "vm/page.h"
#include <stdio.h>
#include <string.h>
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "userprog/pagedir.h"
#include "filesys/file.h"

static struct page *page_lookup (void *uaddr);
static bool page_in (struct page *page);
static bool page_file_in (struct page *page);
static void page_page_pin (struct page *page);
static void page_page_unpin (struct page *page);
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
  p->pinned = false;
  p->mmap = NULL;
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
  if (p == NULL)
    PANIC ("Freeing page at invalid memory!");
  page_page_free (p);
  lock_release (t->page_table_lock);
}

/* Alias for page_page_pin (page_lookup (UADDR)). Thread-safe. */
void 
page_pin (void *uaddr)
{  
  struct page *page = page_lookup (uaddr);

  if (page == NULL)
    PANIC ("Pinning invalid page!");

  lock_acquire (&page->lock);
  page_page_pin (page);
  lock_release (&page->lock);
}

/* Alias for page_page_unpin (page_lookup (UADDR)). Thread-safe. */
void 
page_unpin (void *uaddr)
{
  struct page *page = page_lookup (uaddr);

  if (page == NULL)
    PANIC ("Unpinning invalid page!");

  lock_acquire (&page->lock);
  page_page_unpin (page);
  lock_release (&page->lock);
}

/* PAGE will never pagefault after this function call until
   page_page_unpin (PAGE) is called.
   Assumes PAGE->lock is acquired. */
static void
page_page_pin (struct page *page)
{
  ASSERT (lock_held_by_current_thread (&page->lock));
  
  page->pinned = true;
  /* Make sure the page has a frame. */
  if (page->location != FRAME && !page_in (page))
    PANIC ("Failed to Page-in page before pinning it!");
  /* Pin the frame if not already pinned. */
  frame_pin (page->frame);
}

/* Cancels the effects of calling page_page_pin and makes PAGE
   a candidate for eviction.
   Assumes PAGE->lock is acquired. */
static void
page_page_unpin (struct page *page)
{
  ASSERT (lock_held_by_current_thread (&page->lock));
  
  page->pinned = false;
  /* Unpin the frame associated with the page if it exists. */
  if (page->location == FRAME)
    frame_unpin (page->frame);
}

/* Returns true after placing PAGE in a frame and false on failure.
   Assumes that PAGE->lock is acquired by the current thread to prevent
   page eviction race conditions while loading the page. Page is pinned
   by default. */
static bool
page_in (struct page *page)
{
  struct thread *t = thread_current ();
  struct frame *frame;

  ASSERT (lock_held_by_current_thread (&page->lock));

  /* Return true if already paged in. */
  if (page->location == FRAME)
    return true;
  /* Allocate a pinned frame to resolve the pagefault into. */
  frame = frame_alloc ();
  page->pinned = true;
  frame->page = page;
  page->frame = frame;  
  /* Set the page to be writable until the data is fully loaded. */
  if (!pagedir_set_page (t->pagedir, page->uaddr, frame->kaddr, 
                         true))
    {
      frame_free (frame);
      page->pinned = false;
      return false;
    }
  /* Populate the frame with the appopriate data. */
  switch (page->location)
    {
      case NEW:
        /* NEW pages are populated with 0xcc just for debugging. */
        memset (frame->kaddr, 0xcc, PGSIZE);
        break;
      case SWAP:
        /* Swap-in the page data. */
        if (!swap_in (frame, page->swap_slot))
          {
            /* Failing to swap-in a page means that we lost it forever. */
            page->location = CORRUPTED;
            frame_free (frame);
            page->pinned = false;
            return false;
          }
        break;
      case FILE:
        if (!page_file_in (page))
          {
            /*TODO (kenny): definitely need to do some more cleaning up*/
            frame_free (frame);
            page->pinned = false;
            return false;
          }
        break;
      /* TODO - add other cases (e.g. mmaped files). */
      default:
        PANIC ("Failed to page-in at address %p!", page->uaddr);
    }
  page->location = FRAME;
  /* Reset the original value of the writable bit. */
  pagedir_set_writable (t->pagedir, page->uaddr, page->writable);
  return true;
}

/* Reads data into mmapped PAGE from file backing it */
static bool
page_file_in (struct page *page)
{
  struct page_mmap *mmap = page->mmap;
  if (mmap == NULL)
    return false;

  /*get file lock here*/
  // Read data from mmap file
  off_t old_cur = file_tell(mmap->file);
  file_seek (mmap->file, page->start_byte);
  off_t bytes_to_read = PGSIZE - page->file_zero_bytes;
  off_t bytes_read = file_read (mmap->file, page->frame->kaddr, bytes_to_read);
  file_seek (mmap->file, old_cur);

  if (bytes_read != bytes_to_read) 
    return false;

  //Zero out zero bytes
  memset(page->frame->kaddr + bytes_read, 0, page->file_zero_bytes);
  return true;
}

/* Attempts to resolve a pagefault on FAULT_ADDR by calling
   page_in on its page. Returns true on success and false if
   FAULT_ADDR is not a valid address in the first place. */
bool 
page_resolve_fault (void *fault_addr)
{
  bool success;
  struct page *page;

  ASSERT (is_user_vaddr (fault_addr));

  page = page_lookup (fault_addr);
  /* Fault address is not mapped. */
  if (page == NULL) 
    return false;
  /* Page is already in a frame (can't be paged-in) or is corrupted. */
  if (page->location == FRAME || page->location == CORRUPTED)
    return false;
  /* Page-in to a frame. */
  lock_acquire (&page->lock);
  success = page_in (page);
  /* Unpin the page by default. */
  page_page_unpin (page);
  lock_release (&page->lock);
  return success;
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

  lock_acquire (&page->lock);  
  /* The page could be already evicted. */
  if (page->location != FRAME)
    {
      success = true;
      goto done;
    }
  /* Can't evict pinned pages. */
  if (page->pinned)
    {
      success = false;
      goto done;
    }
  /* Evict the page to swap. */
  if (page->location == FRAME)
    {
      page->swap_slot = swap_out (page->frame);
      if (page->swap_slot != SWAP_ERROR)
        {
          pagedir_clear_page (page->thread->pagedir, page->uaddr);
          page->location = SWAP;
          success = true;
        }
      else
        success = false;
    }
done:
  lock_release (&page->lock);
  return success;
}

/* Allocate a page in user address UADDR and associate it with file backed
 * memory map MMAP */
bool page_add_to_mmap(struct page_mmap *mmap, void* uaddr, 
                            unsigned offset)
{
  struct thread *t = thread_current ();

  // Check that address isn't already mapped to a page
  struct page *pRet = page_lookup(uaddr);
  if (pRet != NULL || pagedir_get_page(t->pagedir, uaddr))
    return false;
  // Get zero bytes of page
  size_t zero_bytes = 0;
  size_t stick_out = mmap->file_size - offset;

  if (stick_out < PGSIZE)
    zero_bytes = PGSIZE - stick_out;

  struct page_mmap_wrapper *page_wrapper = malloc (sizeof (struct page_mmap_wrapper));
  if (page_wrapper == NULL)
    return false;
  page_wrapper->page_addr = uaddr;

  // Add page to page table
  void *addr = page_alloc(uaddr);
  pRet = page_lookup(addr);
  if (pRet == NULL) 
  {
    free (page_wrapper);
    return false;
  }

  pRet->location = FILE;
  pRet->file_zero_bytes = zero_bytes;
  pRet->start_byte = offset;
  pRet->mmap = mmap;

  // add page to mmap list of pages
  list_push_back(&mmap->mmap_pages, &page_wrapper->list_elem);
  return true;
}

/* Delete mmapp and free all resources associated with it*/
void page_delete_mmap (struct page_mmap *mmap)
{
  struct list_elem *curr_elem = NULL;
  struct list_elem *next_elem = NULL; 

  for (curr_elem = list_begin (&mmap->mmap_pages); 
       curr_elem != list_end (&mmap->mmap_pages);
       curr_elem = next_elem)
    {
      next_elem = list_next(curr_elem);
      
      struct page_mmap_wrapper *page = list_entry(curr_elem,
                                                  struct page_mmap_wrapper,
                                                  list_elem);

      struct page *pRet = page_lookup(page->page_addr);
      if (pRet == NULL)
        PANIC ("Error in mmap, missing page entry");
      page_free(page->page_addr);
      free(page);
    }
  free (mmap);
}

static bool
page_mmap_equal (struct list_elem *elem, void *aux)
{
  struct page_mmap *mmap = list_entry(elem, 
                                        struct page_mmap, list_elem);
  return mmap->id == *(mapid_t *)aux;
}

struct page_mmap *page_get_mmap(struct thread *t, mapid_t id)
{
  struct list_elem *e;
  if (!list_empty(&t->mmap_list))
    {
      e = list_find(&t->mmap_list, page_mmap_equal,
                    &id);
      return list_entry(e, struct page_mmap, list_elem);
    }
  return NULL;
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
