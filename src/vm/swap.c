#include "vm/swap.h"

#define SECTORS_PER_PAGE (PGSIZE / BLOCK_SECTOR_SIZE)

static struct swap_table st;

/* Initializes the swap table st by loading its block device 
   and creating its allocation bitmap. */
void
swap_init (void)
{
  int slot_count;

  st.block_device = block_get_role (BLOCK_SWAP);
  if (st.block_device == NULL)
    PANIC ("Swap block device does not exist!");
  slot_count = block_size (st.block_device) / SECTORS_PER_PAGE;
  st.allocated_slots = bitmap_create (slot_count);
  if (st.allocated_slots == NULL)
    PANIC ("OOM when allocating swap table structures!");
}

/* Stores the page in FRAME from memory into the first available 
   swap slot in the swap block device and returns its index. Returns
   SWAP_ERROR when no swap slots are available. */
swap_slot
swap_out (struct frame *frame)
{
  size_t slot_idx;
  block_sector_t sector_begin, sector_offs;

  /* Scan for the first free swap slot. */
  slot_idx = bitmap_scan_and_flip (st.allocated_slots, 0, 1, false);
  if (slot_idx == BITMAP_ERROR)
    return SWAP_ERROR;
  /* Loop over the frame and write it sector by sector to swap slot. */
  sector_begin = slot_idx * SECTORS_PER_PAGE;
  for (sector_offs = 0; sector_offs < SECTORS_PER_PAGE; sector_offs++)
    block_write (st.block_device, sector_begin + sector_offs, 
                 ((uint8_t *) frame->kaddr) + sector_offs * BLOCK_SECTOR_SIZE);
  return slot_idx;
}

/* Loads swap slot at SLOT_IDX into memory page mapped by FRAME.
   Returns true on success or false if SLOT_IDX is invalid. */
bool
swap_in (struct frame *frame, swap_slot slot_idx)
{
  block_sector_t sector_begin, sector_offs;

  /* Verify that the swap slot is actually occupied. */
  if (!bitmap_test (st.allocated_slots, slot_idx))
    return false;
  /* Read the swap slot sector by sector to the frame. */
  sector_begin = slot_idx * SECTORS_PER_PAGE;
  for (sector_offs = 0; sector_offs < SECTORS_PER_PAGE; sector_offs++)
    block_read (st.block_device, sector_begin + sector_offs, 
                ((uint8_t *) frame->kaddr) + sector_offs * BLOCK_SECTOR_SIZE);
  /* Free up the swap slot. */
  bitmap_reset (st.allocated_slots, slot_idx);
  return true;
}

