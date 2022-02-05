#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include "threads/synch.h"

/* Struct and functions for process fd table*/
struct process_fd
 {
   int id;                     /* ID of file descriptor*/
   struct list_elem list_elem; /* List element to place fd in table*/ 
   struct file *file;          /* File associated with fd*/
   char* file_name;            /* Name of file*/
 };

int process_new_fd(struct thread *t, struct file *file, char* file_name);
void process_remove_fd(struct thread *t, int id);
struct process_fd *process_get_fd(struct thread *t, int id);

/* Keeps track of the status of a child in the list of children
   of a parent thread. */
struct process_child 
  {
    tid_t tid;
    struct thread *thread;
    struct list_elem elem;
    int32_t exit_code;
    struct semaphore exited;
  };

void process_init (void);
tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);

#endif /* userprog/process.h */
