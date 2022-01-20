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

typedef int fixed_point_t;
static inline fixed_point_t conv_to_fp(int n) { return n * F; }
static inline int conv_to_int(fixed_point_t x) { return x / F; }
static inline int conv_to_nearest_int(fixed_point_t x)
{
  return (x >= 0) ? (x + F/2) / F : (x - F/2) / F;
}
static inline int add_fp(fixed_point_t n, fixed_point_t x) 
{
  return x + n * F; 
}
static inline int sub_fp(fixed_point_t x, fixed_point_t n) 
{ 
  return x - n * F; 
}
static inline int mult_fp(fixed_point_t x, fixed_point_t y) 
{
  return ((int64_t) x) * y / F; 
}
static inline int div_fp(fixed_point_t x, fixed_point_t y) {
  return ((int64_t) x) * F / y; 
}

#endif /* lib/fixed-point.h */
