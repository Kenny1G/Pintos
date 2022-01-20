#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "devices/timer.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* List of processes put to sleep by a call to timer_sleep */
static struct list slept_list;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame 
  {
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
  };

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use priority scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;
#define MLFQS_THREAD_PRIORITY_FREQ 4
#define MLFQS_NICE_DEFAULT 0
#define MLFQS_RECENT_CPU_DEFAULT 0
fp_t mlfqs_load_average;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *t, const char *name, int priority, 
                         int mlfqs_nice, int mlfqs_recent_cpu);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);
static void thread_mlfqs_update_tick (void);
static void thread_mlfqs_update_recent_cpu (struct thread *, void * UNUSED);
static void thread_mlfqs_update_priority (struct thread *, void * UNUSED);


/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&slept_list);
  list_init (&all_list);
  mlfqs_load_average = fp (0);

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT, MLFQS_NICE_DEFAULT, 
               MLFQS_RECENT_CPU_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) 
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) 
{
  struct thread *t = thread_current ();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();  

  /* Update MLFQS statistics. */
  if (thread_mlfqs)
    thread_mlfqs_update_tick ();
}

/* Called every tick by the timer to update the MLFQS scheduler
   load_average and recent_cpu statistics. */
static void
thread_mlfqs_update_tick (void)
{
  enum intr_level old_level;
  size_t count_ready_threads;

  ASSERT (thread_mlfqs);
  
  old_level = intr_disable ();
  if (timer_ticks () % TIMER_FREQ == 0)
    {
      /* Update load_average every second. */
      count_ready_threads = list_size (&ready_list) + (thread_current () != idle_thread) ? 1 : 0;
      mlfqs_load_average = fp_add (fp_mult (fp_div (fp (59), fp (60)), 
                                            mlfqs_load_average),
                                   fp_mult (fp_div (fp (1), fp (60)), 
                                           fp (count_ready_threads)));
      /* Update all threads' recent_cpu every second. */
      thread_foreach (thread_mlfqs_update_recent_cpu, NULL);
    }
  else if (thread_ticks % MLFQS_THREAD_PRIORITY_FREQ == 0)
    {
      /* Update all threads' priori every MLFQS_THREAD_PRIORITY_FREQ tick. */
      thread_foreach (thread_mlfqs_update_priority, NULL);
    }
  else
    {
      /* Update this thread's recent_cpu every tick. */
      if (thread_current () != idle_thread)
        thread_current ()->mlfqs_recent_cpu = fp_add (fp (1),
                                        thread_current ()->mlfqs_recent_cpu);
    }
  intr_set_level (old_level);
}

/* Updates the priority of T if T->mlfqs_cache_priority is false
   assuming interrupts are off to prevent a race condition. */
static void
thread_mlfqs_update_priority (struct thread *t, void *aux UNUSED)
{
  enum intr_level old_level;

  ASSERT (thread_mlfqs);

  old_level = intr_disable ();
  if (t->mlfqs_cache_priority) return;
  t->priority = fp_to_int (fp_sub (fp_sub (fp (PRI_MAX), 
                                           fp_div (t->mlfqs_recent_cpu, 
                                                   fp (4))), 
                                   fp (2 * t->mlfqs_nice)));
  t->priority = t->priority < PRI_MIN ? PRI_MIN : t->priority;
  t->priority = t->priority > PRI_MAX ? PRI_MAX : t->priority;
  t->mlfqs_cache_priority = true;
  intr_set_level (old_level);
}

/* Update the recent_cpu member for T assuming interrupts are
   off to prevent a writing race condition. */
static void
thread_mlfqs_update_recent_cpu (struct thread *t, void *aux UNUSED)
{
  enum intr_level old_level;
  fp_t decay_factor;
  fp_t old_recent_cpu;

  ASSERT (thread_mlfqs);

  old_level = intr_disable ();
  old_recent_cpu = t->mlfqs_recent_cpu;
  decay_factor = fp_div (fp_mult (fp (2), mlfqs_load_average),
                         fp_add (fp_mult (fp (2), mlfqs_load_average), 
                                 fp (1)));
  t->mlfqs_recent_cpu = fp_add (fp_mult (decay_factor, t->mlfqs_recent_cpu),
                                fp (t->mlfqs_nice));
  t->mlfqs_cache_priority = old_recent_cpu == t->mlfqs_recent_cpu;
  intr_set_level (old_level);
}

/* Prints thread statistics. */
void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   Sets the new thread's `priority' member to PRIORITY and yields
   the CPU if another ready thread has higher priority. */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{
  enum intr_level old_level;
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;

  ASSERT (function != NULL);

  /* Allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread (t, name, priority, thread_current ()->mlfqs_nice,
               thread_current ()->mlfqs_recent_cpu);
  tid = t->tid = allocate_tid ();

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  /* Add to run queue. */
  old_level = intr_disable ();
  thread_unblock (t);
  thread_yield_for_priority();
  intr_set_level (old_level);

  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  list_push_back (&ready_list, &t->elem);
  t->status = THREAD_READY;
  intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  
  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) 
{
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  intr_disable ();
  list_remove (&thread_current()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur != idle_thread) 
    list_push_back (&ready_list, &cur->elem);
  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

/* Yields the CPU if the current thread's priority is smaller than some
   other ready thread's priority by calling thread_yield(). */
void
thread_yield_for_priority (void) 
{
  enum intr_level old_level;

  old_level = intr_disable ();
  if (!list_empty (&ready_list) 
      && thread_current ()->priority < list_entry (list_min (&ready_list, 
        thread_higher_priority, NULL), struct thread, elem)->priority)
    {
      if (intr_context ())
        intr_yield_on_return ();
      else
        thread_yield();
    }
  intr_set_level (old_level);
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

/* Adds current thread to the list of slept threads*/
void
thread_add_to_slept (void)
{
  struct thread *t = thread_current ();
  list_push_back(&slept_list, &(t->slept_elem));
}

/* This function wakes sleeping threads that have been asleep
 * for longer than they were set to be */
void
thread_wake_eligible_slept (void)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  e = list_begin(&slept_list);
  while (e != list_end (&slept_list))
    {
      struct thread *t = list_entry(e, struct thread, slept_elem);
      if (--t->sleep_ticks <= 0) {
        e = list_remove(e);
        sema_up(t->sleep_sema);
      }
      else {
        e = list_next (e);
      }
    }
}

/* Sets the current thread's priority to NEW_PRIORITY. 
   If any other ready thread has higher priority than NEW_PRIORITY, the
   current thread yields the CPU, so don't call in an interrupt context. */
void
thread_set_priority (int new_priority) 
{  
  enum intr_level old_level;

  old_level = intr_disable ();
  thread_current ()->base_priority = new_priority;
  thread_recalculate_priority (thread_current (), 0);
  thread_yield_for_priority ();
  intr_set_level (old_level);
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) 
{
  return thread_current ()->priority;
}

/* Looks at all threads awaiting locks held by the thread T
   to determine its maximum donated priority and store it. */
void
thread_recalculate_priority (struct thread* t, size_t nested_depth)
{
  int max_priority;
  struct list_elem *iterator, *max_lock_donation_elem;
  struct lock *lock;
  enum intr_level old_level;

  ASSERT (is_thread (t));
  
  if (nested_depth > MAX_PRIORITY_DONATION_NESTED_DEPTH)
    return;
  old_level = intr_disable ();
  max_priority = t->base_priority;
  for (iterator = list_begin(&t->locks_held);
       iterator != list_end (&t->locks_held);
       iterator = list_next(iterator))
    {
      lock = list_entry(iterator, struct lock, elem);
      if (lock->max_priority_donation > max_priority)
        max_priority = lock->max_priority_donation;
    }
  t->priority = max_priority;
  if (t->blocking_lock != NULL)
    {
      max_lock_donation_elem = list_min (&t->blocking_lock->waiters, thread_higher_priority, NULL);
      t->blocking_lock->max_priority_donation = list_entry (max_lock_donation_elem, struct thread,
                                                lock_elem)->priority;
      thread_recalculate_priority (t->blocking_lock->holder, nested_depth + 1);
    }
  intr_set_level (old_level);
}

/* Returns true if A has higher priority than B or false otherwise. */
bool
thread_higher_priority (const struct list_elem *a, const struct list_elem *b, 
                        void *aux UNUSED)
{
  return list_entry (a, struct thread, elem)->priority 
         > list_entry (b, struct thread, elem)->priority;
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice) 
{
  thread_current ()->mlfqs_nice = nice;
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) 
{
  return thread_current ()->mlfqs_nice;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) 
{
  return fp_to_int (fp_mult (mlfqs_load_average, fp (100)));
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) 
{
  return fp_to_int (fp_mult (thread_current ()->mlfqs_recent_cpu, fp (100)));
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      /* Let someone else run. */
      intr_disable ();
      thread_block ();

      /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);

  intr_enable ();       /* The scheduler runs with interrupts off. */
  function (aux);       /* Execute the thread function. */
  thread_exit ();       /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority, int mlfqs_nice,
             int mlfqs_recent_cpu)
{
  enum intr_level old_level;

  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  t->base_priority = priority;
  t->priority = priority;
  t->mlfqs_nice = mlfqs_nice;
  t->mlfqs_recent_cpu = mlfqs_recent_cpu;
  list_init(&t->locks_held);
  t->magic = THREAD_MAGIC;
  if (thread_mlfqs) 
    thread_mlfqs_update_priority (t, NULL);
  
  old_level = intr_disable ();
  list_push_back (&all_list, &t->allelem);
  intr_set_level (old_level);
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) 
{
  struct list_elem *next_thread_elem;

  if (list_empty (&ready_list))
    return idle_thread;
  else
    {
      next_thread_elem = list_pop_min (&ready_list, thread_higher_priority, 
                                       NULL);
      return list_entry (next_thread_elem, struct thread, elem);
    }
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
static void
schedule (void) 
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);
