#include "procrun.h"
#include <string.h>

#ifdef _WIN32

int proc_start(char *const argv[], proc_t *p) {
    char cmd[1024] = "";
    for (int i = 0; argv[i]; i++) {
        if (i) strcat(cmd, " ");
        strcat(cmd, "\"");
        strcat(cmd, argv[i]);
        strcat(cmd, "\"");
    }
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        return -1;
    CloseHandle(pi.hThread);
    *p = pi.hProcess;
    return 0;
}

int proc_wait(proc_t p, int *exit_code) {
    DWORD code;
    WaitForSingleObject(p, INFINITE);
    if (!GetExitCodeProcess(p, &code)) return -1;
    CloseHandle(p);
    *exit_code = (int)code;
    return 0;
}

#else

#include <unistd.h>
#include <sys/wait.h>

int proc_start(char *const argv[], proc_t *p) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);              /* exec не удался */
    }
    *p = pid;
    return 0;
}

int proc_wait(proc_t p, int *exit_code) {
    int status;
    if (waitpid(p, &status, 0) < 0) return -1;
    *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return 0;
}

#endif