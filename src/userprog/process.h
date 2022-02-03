#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include "threads/synch.h"

/* Keeps track of the status of a child in the list of children
   of a parent thread. */
struct process_child {
  struct thread *thread;
  struct list_elem elem;
  int32_t exit_code;
  struct semaphore exited;
};

tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);

#endif /* userprog/process.h */
