#ifndef __LIB_FIXED_POINT_H
#define __LIB_FIXED_POINT_H
#include <stdint.h>

#define F (1 << 14)

/* Helper functions for fixed-point arithmetic
 * Note: helpers were not defined for arithmetic operations
 * that can be done without fixed-point const F.
 */

#include "fixed-point.h"
#include <stdint.h>

typedef int32_t fp_t;
static inline fp_t fp (int n) { return n * F; }
static inline int fp_to_int (fp_t x) { return x / F; }
static inline int fp_to_nearest_int (fp_t x)
{
  return (x >= 0) ? (x + F/2) / F : (x - F/2) / F;
}
static inline int fp_add (fp_t n, fp_t x) { return x + n * F; }
static inline int fp_sub (fp_t x, fp_t n) { return x - n * F; }
static inline int fp_mult (fp_t x, fp_t y) 
{
  return ((int64_t) x) * y / F; 
}
static inline int fp_div (fp_t x, fp_t y) 
{
  return ((int64_t) x) * F / y; 
}

#endif /* lib/fixed-point.h */
