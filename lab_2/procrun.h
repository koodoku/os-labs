#ifndef PROCRUN_H
#define PROCRUN_H

#ifdef _WIN32
#include <windows.h>
typedef HANDLE proc_t;
#else
#include <sys/types.h>
typedef pid_t proc_t;
#endif

int proc_start(char *const argv[], proc_t *p);
int proc_wait(proc_t p, int *exit_code);

#endif