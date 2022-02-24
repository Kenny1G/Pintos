#ifndef VM_SWAP_H
#define VM_SWAP_H
#include <bitmap.h>
#include <stdbool.h>
#include <stddef.h>
#include <debug.h>
#include "threads/vaddr.h"
#include "devices/block.h"
#include "vm/frame.h"

/* Swap Table keeps track of allocated swap slots on a block
   device. */
struct swap_table
  {
    struct block *block_device;     /* Block device where slots are stored. */
    struct bitmap *allocated_slots; /* Bitmap of allocated/free swap slots. */
  };

/* Identifies one swap slot useful for paging. */
typedef size_t swap_slot;
#define SWAP_ERROR SIZE_MAX

/* Swap Table paging functions. */
void swap_init (void);
swap_slot swap_out (struct frame *frame);
bool swap_in (struct frame *frame, swap_slot slot_idx);

#endif /* vm/swap.h */
