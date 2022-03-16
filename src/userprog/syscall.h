#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include <stdbool.h>

#define SYSCALL_ERROR -1

void syscall_init (void);
bool syscall_process_init (void);
void syscall_process_done (void);
void syscall_close_helper (int fd);

/* Map region identifier. */
typedef int mapid_t;
#define MAP_FAILED ((mapid_t) -1)

#endif /* userprog/syscall.h */
