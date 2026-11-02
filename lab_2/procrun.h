#ifndef PROCRUN_H
#define PROCRUN_H

#ifdef _WIN32
#include <windows.h>
typedef HANDLE proc_t;
#else
#include <sys/types.h>
typedef pid_t proc_t;
#endif

/* Запускает программу в фоне. argv: имя программы, аргументы, NULL.
   Возвращает 0 при успехе, -1 при ошибке. */
int proc_start(char *const argv[], proc_t *p);

/* Ждёт завершения программы и записывает её код возврата. 0 / -1. */
int proc_wait(proc_t p, int *exit_code);

#endif