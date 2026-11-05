#include <stdio.h>
#include <stdlib.h>
#include "procrun.h"

int main(int argc, char *argv[]) {

    // Если программа запущена как дочерний процесс (есть хотя бы один аргумент)
    if (argc > 1)
    {
        return atoi(argv[1]);//возвращаем первый аргумент как код завершения
    }
    proc_t p;
    int code;

    // argv[0] - путь к самой себе, дальше любое количество аргументов, в конце NULL
    char *child_argv[] = { argv[0], "555", "hiiiiii", "args", NULL };

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

    //проверка результата
    if (code == 555) {
        printf("TEST PASSED\n");
    } else {
        printf("TEST FAILED\n");
        return 1;
    }

    return 0;
}