#ifndef THREADS_FIXED_POINT_H
#define THREADS_FIXED_POINT_H

#include <stdint.h>

#define F (1 << 14)

/* Helper functions for fixed-point arithmetic
 * Note: helpers were not defined for arithmetic operations
 * that can be done without fixed-point const F.
 */

static inline int convert_to_fixed_point(int n)
{
  return n * F;
}

static inline int convert_to_int_roud_zero(int x)
{
  return x / F;
}

static inline int convert_to_int_round_nearest(int x)
{
  return (x >= 0) ? (x + F/2) / F : (x - F/2) / F;
}

static inline int add_int_to_fixed_point(int n, int x)
{
  return x + n * F;
}

static inline int sub_int_from_fixed_point(int x, int n)
{
  return x - n * F;
}

static inline  int mult_fixed_point(int x, int y) 
{
  return ((int64_t) x) * y / F;
}

static inline int div_fixed_point(int x, int y)
{
  return ((int64_t) x) * F / y;
}

#endif /* threads/fixed-point.h */
