/* Executes various tests, optionally repeatedly, checking each time whether
   memory allocated before and after the wait(exec()) provides the same
   memory address. NOTE: This test makes the assumption that the malloc
   allocator returns constistent addresses regardless of the ordering of
   malloc/free. I don't actually know if that's true, but this tool was capable
   of precisely finding my own leak.

   Written by: David Kvasnica Feb 1, 2022


   !!! This test requires the following syscall to be piped !!!

   // lib/user/syscall
   unsigned
   curmem ()
   {
     return syscall0 (SYS_CURMEM);
   }

   // userprog/syscall
   case SYS_CURMEM:
     {
       void *m = malloc (1);
       free (m);
       f->eax = (unsigned) m;
     }
     break;
*/

#include <syscall.h>
#include "tests/lib.h"
#include "tests/main.h"

#define NUM_ITERATIONS 1

/* Careful! Some tests can't loop and run correctly, since they leave things in
   an inconsistent state. Ex. write-normal: since it creates a file but doesn't
   remove it. Therefore you'll see errors printed for subsequent runs of that
   test, but it still shouldn't leak. */
static char *testnames[] = {
  "args-multiple"
};
//  "open-normal",
//  "close-normal",
//  "syn-remove",
//  "multi-recurse 2",
//  "sc-boundary",
//  "sc-boundary-2",
//  "sc-boundary-3",
//
//  /* Known to show errors when looped: */
//  "write-normal"
//};

void
test_main (void) 
{
  static unsigned first_m = 0;
  first_m = curmem ();

  const size_t num_tests = sizeof(testnames) / sizeof(testnames[0]);
  for(size_t i_test = 0; i_test < num_tests; i_test++)
    {
      for (size_t i = 0; i < NUM_ITERATIONS; i++)
        {
          char *testname = testnames[i_test];
          wait (exec (testname));
          unsigned m = curmem();

          if (first_m != m)
            {
              PANIC ("Something is leaking during: %s!\n", testname);
              exit (-1);
            }
        }
    }

  msg ("No leaks detected\n");
}

