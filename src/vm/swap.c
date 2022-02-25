#include "vm/swap.h"
#include <stdio.h>
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "threads/synch.h"

#define SECTORS_PER_PAGE (PGSIZE / BLOCK_SECTOR_SIZE)

/* Swap table keeping track of free and allocated swap slots
   and the block deivce containing them. */
static struct swap_table st;
/* Lock guarding the swap table allocations and frees. */
static struct lock swap_table_lock;

/* Initializes the swap table st by loading its block device 
   and creating its allocation bitmap. */
void
swap_init (void)
{
  size_t slot_count;

  lock_init (&swap_table_lock);
  st.block_device = block_get_role (BLOCK_SWAP);
  if (st.block_device == NULL)
    PANIC ("Swap block device does not exist!");
  slot_count = block_size (st.block_device) / SECTORS_PER_PAGE;
  lock_acquire (&swap_table_lock);
  st.allocated_slots = bitmap_create (slot_count);
  if (st.allocated_slots == NULL)
    PANIC ("OOM when allocating swap table structures!");
  lock_release (&swap_table_lock);
}

/* Stores the page in FRAME from memory into the first available 
   swap slot in the swap block device and returns its index. Returns
   SWAP_ERROR when no swap slots are available. */
size_t
swap_out (void *frame_)
{
  struct frame *frame = frame_;
  size_t swap_slot;
  block_sector_t sector_begin, sector_offs;

  /* Scan for the first free swap slot. */
  lock_acquire (&swap_table_lock);
  swap_slot = bitmap_scan_and_flip (st.allocated_slots, 0, 1, false);
  lock_release (&swap_table_lock);
  if (swap_slot == BITMAP_ERROR)
    return SWAP_ERROR;
  /* Loop over the frame and write it sector by sector to swap slot. */
  sector_begin = swap_slot * SECTORS_PER_PAGE;
  for (sector_offs = 0; sector_offs < 1; sector_offs++)
    block_write (st.block_device, sector_begin + sector_offs, 
                 ((uint8_t *) frame->kaddr) + sector_offs * BLOCK_SECTOR_SIZE);
  return swap_slot;
}

/* Loads swap slot at SWAP_SLOT into memory page mapped by FRAME.
   Returns true on success or false if SWAP_SLOT is invalid. */
bool
swap_in (void *frame_, size_t swap_slot)
{
  struct frame *frame = frame_;
  block_sector_t sector_begin, sector_offs;

  /* Verify that the swap slot is actually occupied. */
  if (!bitmap_test (st.allocated_slots, swap_slot))
    return false;
  /* Read the swap slot sector by sector to the frame. */
  sector_begin = swap_slot * SECTORS_PER_PAGE;
  for (sector_offs = 0; sector_offs < SECTORS_PER_PAGE; sector_offs++)
    block_read (st.block_device, sector_begin + sector_offs, 
                ((uint8_t *) frame->kaddr) + sector_offs * BLOCK_SECTOR_SIZE);
  /* Free up the swap slot. */
  bitmap_reset (st.allocated_slots, swap_slot);
  return true;
}

/* Frees up a swap slot in the swap table. */
void
swap_free (size_t swap_slot)
{
  bitmap_reset (st.allocated_slots, swap_slot);
}

