#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "procrun.h"

int main(int argc, char *argv[]) {
    /* child process mode */
    if (argc > 2 && strcmp(argv[1], "--child") == 0)
        return atoi(argv[2]);

    char *child[] = { argv[0], "--child", "42", NULL };
    char *const *cmd = (argc > 1) ? argv + 1 : child;

    proc_t p;
    int code;

    if (proc_start(cmd, &p) != 0) {
        printf("Start error\n");
        return 1;
    }
    printf("Process started in background, main program keeps running\n");

    if (proc_wait(p, &code) != 0) {
        printf("Wait error\n");
        return 1;
    }
    printf("Process exited with code %d\n", code);

    if (argc == 1 && code != 42) {
        printf("TEST FAILED\n");
        return 1;
    }
    if (argc == 1) printf("TEST PASSED\n");
    return 0;
}