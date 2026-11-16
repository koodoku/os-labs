#include <stdio.h>
#include <stdlib.h>
#include "procrun.h"

int main(int argc, char *argv[]) {
    if (argc > 1)
    {
        return atoi(argv[1]);
    }
    proc_t p;
    int code;

    char *child_argv[] = { argv[0], "42", "hiiiiii", "args", NULL };

    //запуск дочернего
    if (proc_start(child_argv, &p) != 0) {
        printf("Start error\n");
        return 1;
    }

    printf("Process started in background\n");

    //ожидание завершения
    if (proc_wait(p, &code) != 0) {
        printf("Wait error\n");
        return 1;
    }

    printf("Process exited with code %d\n", code);

    if (code == 42) {
        printf("TEST PASSED\n");
    } else {
        printf("TEST FAILED\n");
        return 1;
    }

    return 0;
}