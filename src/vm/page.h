
#include <stdbool.h>
#include <inttypes.h>

uint8_t *page_alloc (uint8_t *uaddr);
void page_free (uint8_t *uaddr);
void page_set_writable (uint8_t *uaddr, bool writable);

