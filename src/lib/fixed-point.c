#include "fixed-point.h"
#include <stdint.h>

static int convert_to_fixed_point(int n)
{
  return n * F;
}

static int convert_to_int_roud_zero(int x)
{
  return x / F;
}

static int convert_to_int_round_nearest(int x)
{
  return (x >= 0) ? (x + F/2) / F : (x - F/2) / F;
}

static int add_int_to_fixed_point(int n, int x)
{
  return x + n * F;
}

static int sub_int_from_fixed_point(int x, int n)
{
  return x - n * F;
}

static  int mult_fixed_point(int x, int y)
{
  return ((int64_t) x) * y / F;
}

static int div_fixed_point(int x, int y)
{
  return ((int64_t) x) * F / y;
}
