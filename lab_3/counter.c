#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#else
#define _WIN32_WINNT 0x0600
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <time.h>
#include <stdatomic.h>

#ifdef _WIN32
#include <windows.h>
#include <wchar.h>
#include <share.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#endif

enum {
    TICK_MS = 300, LOG_MS = 1000,
    SPAWN_MS = 3000, CHILD_DELAY_MS = 2000
};

enum { DATA_LOCK, LEADER_LOCK, CHILD1_LOCK, CHILD2_LOCK };
enum { RELEASE = -1, TRY_LOCK, WAIT_LOCK };

typedef struct {
    uint32_t pid;
    uint32_t started;
} ChildSlot;

typedef struct {
    int64_t counter;
    ChildSlot children[2];
} Shared;

typedef struct {
    Shared *shared;
    FILE *log;
    uint32_t pid;
    atomic_int stop;

#ifdef _WIN32
    HANDLE file, mapping, children[2];
    CRITICAL_SECTION mutex;
    wchar_t executable[MAX_PATH];
#else
    int file;
    pid_t children[2];
    pthread_mutex_t mutex;
    char executable[PATH_MAX];
#endif
} App;

static void system_error(const char *operation)
{
#ifdef _WIN32
    fprintf(stderr, "%s: Windows error %lu\n",
            operation, (unsigned long)GetLastError());
#else
    perror(operation);
#endif
    exit(EXIT_FAILURE);
}

static void library_error(const char *operation)
{
    perror(operation);
    exit(EXIT_FAILURE);
}

#ifndef _WIN32
static void thread_check(int error)
{
    if (error) {
        fprintf(stderr, "pthread: %s\n", strerror(error));
        exit(EXIT_FAILURE);
    }
}
#endif

/* Каждый байт файла используется как отдельная блокировка. */
static int file_lock(App *app, int byte, int operation)
{
#ifdef _WIN32
    OVERLAPPED position = {0};
    position.Offset = (DWORD)byte;

    if (operation == RELEASE) {
        if (!UnlockFileEx(app->file, 0, 1, 0, &position))
            system_error("UnlockFileEx");
        return 1;
    }

    DWORD flags = LOCKFILE_EXCLUSIVE_LOCK;
    if (operation == TRY_LOCK)
        flags |= LOCKFILE_FAIL_IMMEDIATELY;

    if (LockFileEx(app->file, flags, 0, 1, 0, &position))
        return 1;
    if (operation == TRY_LOCK &&
        GetLastError() == ERROR_LOCK_VIOLATION)
        return 0;

    system_error("LockFileEx");
#else
    struct flock lock = {0};
    lock.l_type = operation == RELEASE ? F_UNLCK : F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = byte;
    lock.l_len = 1;

    int command = operation == WAIT_LOCK ? F_SETLKW : F_SETLK;
    while (fcntl(app->file, command, &lock) == -1) {
        if (errno == EINTR)
            continue;
        if (operation == TRY_LOCK &&
            (errno == EACCES || errno == EAGAIN))
            return 0;
        system_error("fcntl");
    }
    return 1;
#endif
    return 0;
}

/* Мьютекс защищает потоки, файловая блокировка — процессы. */
static void lock_data(App *app)
{
#ifdef _WIN32
    EnterCriticalSection(&app->mutex);
#else
    thread_check(pthread_mutex_lock(&app->mutex));
#endif
    file_lock(app, DATA_LOCK, WAIT_LOCK);
}

static void unlock_data(App *app)
{
    file_lock(app, DATA_LOCK, RELEASE);
#ifdef _WIN32
    LeaveCriticalSection(&app->mutex);
#else
    thread_check(pthread_mutex_unlock(&app->mutex));
#endif
}

static uint64_t milliseconds(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1)
        system_error("clock_gettime");
    return (uint64_t)now.tv_sec * 1000 +
           (uint64_t)now.tv_nsec / 1000000;
#endif
}

static void sleep_ms(unsigned duration)
{
#ifdef _WIN32
    Sleep(duration);
#else
    struct timespec delay = {
        duration / 1000,
        (long)(duration % 1000) * 1000000L
    };
    while (nanosleep(&delay, &delay) == -1) {
        if (errno != EINTR)
            system_error("nanosleep");
    }
#endif
}

/* Вызывается под блокировкой данных; fflush также защищён. */
static void write_log(App *app, const char *event)
{
    int year, month, day, hour, minute, second, ms;

#ifdef _WIN32
    SYSTEMTIME now;
    GetLocalTime(&now);
    year = now.wYear;
    month = now.wMonth;
    day = now.wDay;
    hour = now.wHour;
    minute = now.wMinute;
    second = now.wSecond;
    ms = now.wMilliseconds;
#else
    struct timespec now;
    struct tm local;
    if (clock_gettime(CLOCK_REALTIME, &now) == -1)
        system_error("clock_gettime");
    if (!localtime_r(&now.tv_sec, &local))
        system_error("localtime_r");
    year = local.tm_year + 1900;
    month = local.tm_mon + 1;
    day = local.tm_mday;
    hour = local.tm_hour;
    minute = local.tm_min;
    second = local.tm_sec;
    ms = (int)(now.tv_nsec / 1000000);
#endif

    if (fprintf(app->log,
                "%04d-%02d-%02d %02d:%02d:%02d.%03d "
                "pid=%" PRIu32 " event=%s counter=%" PRId64 "\n",
                year, month, day, hour, minute, second, ms,
                app->pid, event, app->shared->counter) < 0 ||
        fflush(app->log) == EOF)
        library_error("write log");
}

static void open_app(App *app, const char *program)
{
    atomic_init(&app->stop, 0);

#ifdef _WIN32
    (void)program;
    InitializeCriticalSection(&app->mutex);
    app->pid = GetCurrentProcessId();

    DWORD length = GetModuleFileNameW(NULL, app->executable, MAX_PATH);
    if (!length || length >= MAX_PATH)
        system_error("GetModuleFileNameW");

    app->file = CreateFileW(
        L"counter.state", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (app->file == INVALID_HANDLE_VALUE)
        system_error("CreateFileW");

    lock_data(app);
    LARGE_INTEGER size;
    if (!GetFileSizeEx(app->file, &size))
        system_error("GetFileSizeEx");

    if (size.QuadPart == 0) {
        size.QuadPart = sizeof(Shared);
        if (!SetFilePointerEx(app->file, size, NULL, FILE_BEGIN) ||
            !SetEndOfFile(app->file))
            system_error("resize state");
    } else if (size.QuadPart != (LONGLONG)sizeof(Shared)) {
        fputs("Invalid counter.state size\n", stderr);
        exit(EXIT_FAILURE);
    }

    app->mapping = CreateFileMappingW(
        app->file, NULL, PAGE_READWRITE, 0, 0, NULL);
    if (!app->mapping)
        system_error("CreateFileMappingW");

    app->shared = MapViewOfFile(
        app->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared));
    if (!app->shared)
        system_error("MapViewOfFile");

    app->log = _fsopen("counter.log", "a", _SH_DENYNO);
#else
    thread_check(pthread_mutex_init(&app->mutex, NULL));
    app->pid = (uint32_t)getpid();
    if (!realpath(program, app->executable))
        system_error("realpath");

    app->file = open("counter.state", O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (app->file == -1)
        system_error("open state");

    lock_data(app);
    struct stat info;
    if (fstat(app->file, &info) == -1)
        system_error("fstat");

    if (info.st_size == 0) {
        if (ftruncate(app->file, sizeof(Shared)) == -1)
            system_error("ftruncate");
    } else if (info.st_size != (off_t)sizeof(Shared)) {
        fputs("Invalid counter.state size\n", stderr);
        exit(EXIT_FAILURE);
    }

    app->shared = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE,
                       MAP_SHARED, app->file, 0);
    if (app->shared == MAP_FAILED)
        system_error("mmap");

    app->log = fopen("counter.log", "a");
#endif

    if (!app->log)
        library_error("open log");
#ifndef _WIN32
    if (fcntl(fileno(app->log), F_SETFD, FD_CLOEXEC) == -1)
        system_error("fcntl log");
#endif
    unlock_data(app);
}

static void close_app(App *app)
{
    if (fclose(app->log) == EOF)
        library_error("close log");
#ifdef _WIN32
    if (!UnmapViewOfFile(app->shared))
        system_error("UnmapViewOfFile");
    if (!CloseHandle(app->mapping))
        system_error("CloseHandle mapping");
    if (!CloseHandle(app->file))
        system_error("CloseHandle state");
    DeleteCriticalSection(&app->mutex);
#else
    if (munmap(app->shared, sizeof(Shared)) == -1)
        system_error("munmap");
    if (close(app->file) == -1)
        system_error("close state");
    thread_check(pthread_mutex_destroy(&app->mutex));
#endif
}

/* Проверяем созданную копию, ещё не захватившую свою блокировку. */
static int process_exists(uint32_t pid)
{
#ifdef _WIN32
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) {
        DWORD error = GetLastError();
        if (error == ERROR_INVALID_PARAMETER)
            return 0;
        if (error == ERROR_ACCESS_DENIED)
            return 1;
        system_error("OpenProcess");
    }

    DWORD result = WaitForSingleObject(process, 0);
    if (result == WAIT_FAILED)
        system_error("WaitForSingleObject");
    if (!CloseHandle(process))
        system_error("CloseHandle process");
    return result == WAIT_TIMEOUT;
#else
    if (kill((pid_t)pid, 0) == 0 || errno == EPERM)
        return 1;
    if (errno == ESRCH)
        return 0;
    system_error("kill");
    return 0;
#endif
}

/* Получаем статус ребёнка; в POSIX это также предотвращает зомби. */
static int collect_child(App *app, int index, int wait)
{
    if (!app->children[index])
        return 1;

    int success;
#ifdef _WIN32
    DWORD result = WaitForSingleObject(
        app->children[index], wait ? INFINITE : 0);
    if (result == WAIT_TIMEOUT)
        return 0;
    if (result != WAIT_OBJECT_0)
        system_error("WaitForSingleObject child");

    DWORD code;
    if (!GetExitCodeProcess(app->children[index], &code))
        system_error("GetExitCodeProcess");
    success = code == 0;
    if (!success)
        fprintf(stderr, "Child %d: exit code %lu\n",
                index + 1, (unsigned long)code);
    if (!CloseHandle(app->children[index]))
        system_error("CloseHandle child");
#else
    int status;
    pid_t result;
    do {
        result = waitpid(app->children[index], &status, wait ? 0 : WNOHANG);
    } while (result == -1 && errno == EINTR);
    if (result == -1)
        system_error("waitpid");
    if (result == 0)
        return 0;

    success = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!success) {
        if (WIFEXITED(status))
            fprintf(stderr, "Child %d: exit code %d\n",
                    index + 1, WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            fprintf(stderr, "Child %d: signal %d\n",
                    index + 1, WTERMSIG(status));
    }
#endif
    app->children[index] = 0;
    if (!success) {
        lock_data(app);
        write_log(app, index == 0 ? "CHILD1_FAILED" : "CHILD2_FAILED");
        unlock_data(app);
    }
    return 1;
}

static void spawn_child(App *app, int index)
{
    int finished = collect_child(app, index, 0);
    lock_data(app);

    ChildSlot *slot = &app->shared->children[index];
    int free_slot = file_lock(app, CHILD1_LOCK + index, TRY_LOCK);
    if (free_slot)
        file_lock(app, CHILD1_LOCK + index, RELEASE);

    int pending = slot->pid && !slot->started && process_exists(slot->pid);
    if (!finished || !free_slot || pending) {
        write_log(app, index == 0 ?
                  "SKIP_CHILD1_RUNNING" : "SKIP_CHILD2_RUNNING");
        unlock_data(app);
        return;
    }

    /* Ребёнок прочитает свой PID после освобождения DATA_LOCK. */
#ifdef _WIN32
    wchar_t command[32];
    int length = swprintf(command, 32, L"counter --child %d", index + 1);
    if (length < 0 || length >= 32) {
        fputs("Cannot format child command\n", stderr);
        exit(EXIT_FAILURE);
    }

    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process = {0};
    startup.cb = sizeof(startup);

    if (!CreateProcessW(app->executable, command, NULL, NULL,
                        FALSE, 0, NULL, NULL, &startup, &process)) {
        DWORD error = GetLastError();
        write_log(app, "SPAWN_FAILED");
        fprintf(stderr, "CreateProcessW: Windows error %lu\n",
                (unsigned long)error);
        unlock_data(app);
        return;
    }
    app->children[index] = process.hProcess;
    slot->pid = process.dwProcessId;
    slot->started = 0;
    if (!CloseHandle(process.hThread))
        system_error("CloseHandle thread");
#else
    pid_t pid = fork();
    if (pid == -1) {
        int error = errno;
        write_log(app, "SPAWN_FAILED");
        fprintf(stderr, "fork: %s\n", strerror(error));
        unlock_data(app);
        return;
    }
    if (pid == 0) {
        /* После fork многопоточного процесса сразу выполняем exec. */
        execl(app->executable, app->executable,
              "--child", index == 0 ? "1" : "2", (char *)NULL);
        _exit(127);
    }
    app->children[index] = pid;
    slot->pid = (uint32_t)pid;
    slot->started = 0;
#endif
    // write_log(app, index == 0 ? "SPAWN_CHILD1" : "SPAWN_CHILD2");
    unlock_data(app);
}

static int run_child(App *app, int index)
{
    lock_data(app);
    write_log(app, index == 0 ? "START_CHILD1" : "START_CHILD2");

    ChildSlot *slot = &app->shared->children[index];
    if (slot->pid != app->pid || slot->started ||
        !file_lock(app, CHILD1_LOCK + index, TRY_LOCK)) {
        write_log(app, "CHILD_NOT_REGISTERED");
        unlock_data(app);
        return EXIT_FAILURE;
    }
    slot->started = 1;

    int64_t value = app->shared->counter;
    int overflow = index == 0 ? value > INT64_MAX - 10 :
                   value > INT64_MAX / 2 || value < INT64_MIN / 2;

    if (overflow) {
        write_log(app, "CHILD_OVERFLOW");
        fputs("Counter operation would overflow int64_t\n", stderr);
    } else {
        app->shared->counter = index == 0 ? value + 10 : value * 2;
        // write_log(app, index == 0 ? "ADD10" : "MULTIPLY2");
    }
    unlock_data(app);

    if (index == 1 && !overflow) {
        /* Пока копия спит, другие процессы могут менять счётчик. */
        sleep_ms(CHILD_DELAY_MS);
        lock_data(app);
        app->shared->counter /= 2;
        // write_log(app, "DIVIDE2");
        unlock_data(app);
    }

    lock_data(app);
    write_log(app, index == 0 ? "EXIT_CHILD1" : "EXIT_CHILD2");
    unlock_data(app);
    return overflow ? EXIT_FAILURE : EXIT_SUCCESS;
}

#ifdef _WIN32
static DWORD WINAPI timer_thread(LPVOID argument)
#else
static void *timer_thread(void *argument)
#endif
{
    App *app = argument;
    int leader = 0;
    uint64_t next_tick = milliseconds() + TICK_MS;
    uint64_t next_log = 0, next_spawn = 0;

    while (!atomic_load(&app->stop)) {
        uint64_t now = milliseconds();

        if (!leader && file_lock(app, LEADER_LOCK, TRY_LOCK)) {
            leader = 1;
            next_log = now + LOG_MS;
            next_spawn = now + SPAWN_MS;
            // lock_data(app);
            // write_log(app, "LEADER");
            // unlock_data(app);
        }

        if (now >= next_tick) {
            /* Учитываем задержки планировщика ОС. */
            uint64_t ticks = (now - next_tick) / TICK_MS + 1;
            next_tick += ticks * TICK_MS;
            lock_data(app);
            if (ticks > INT64_MAX ||
                app->shared->counter > INT64_MAX - (int64_t)ticks) {
                write_log(app, "TICK_OVERFLOW");
                fputs("Counter increment would overflow int64_t\n", stderr);
            } else {
                app->shared->counter += (int64_t)ticks;
            }
            unlock_data(app);
        }

        if (leader) {
            for (int i = 0; i < 2; ++i)
                collect_child(app, i, 0);

            if (now >= next_log) {
                next_log += ((now - next_log) / LOG_MS + 1) * LOG_MS;
                lock_data(app);
                write_log(app, "COUNTER");
                unlock_data(app);
            }
            if (now >= next_spawn) {
                next_spawn += ((now - next_spawn) / SPAWN_MS + 1) * SPAWN_MS;
                for (int i = 0; i < 2; ++i)
                    spawn_child(app, i);
            }
        }

        uint64_t deadline = next_tick;
        if (leader && next_log < deadline)
            deadline = next_log;
        if (leader && next_spawn < deadline)
            deadline = next_spawn;

        now = milliseconds();
        if (deadline > now)
            sleep_ms((unsigned)(deadline - now));
    }

    /* До окончания своих копий ведущий сохраняет блокировку. */
    for (int i = 0; i < 2; ++i)
        collect_child(app, i, 1);

    return 0;
}

static int run_main(App *app)
{
    lock_data(app);
    write_log(app, "START_MAIN");
    unlock_data(app);

#ifdef _WIN32
    HANDLE thread = CreateThread(NULL, 0, timer_thread, app, 0, NULL);
    if (!thread)
        system_error("CreateThread");
#else
    pthread_t thread;
    thread_check(pthread_create(&thread, NULL, timer_thread, app));
#endif

    printf("PID=%" PRIu32 "\nEnter an integer or quit.\n", app->pid);
    if (fflush(stdout) == EOF)
        library_error("flush stdout");

    char line[128];
    while (fgets(line, sizeof(line), stdin)) {
        if (!strchr(line, '\n') && !feof(stdin)) {
            int ch;
            while ((ch = getchar()) != '\n' && ch != EOF) {}
            fputs("Input is too long\n", stderr);
            continue;
        }
        line[strcspn(line, "\r\n")] = '\0';
        if (strcmp(line, "quit") == 0)
            break;

        char *end;
        errno = 0;
        intmax_t value = strtoimax(line, &end, 10);
        int no_digits = end == line;
        while (isspace((unsigned char)*end))
            ++end;

        if (errno == ERANGE || no_digits || *end ||
            value < INT64_MIN || value > INT64_MAX) {
            fputs("Enter an integer in the int64_t range\n", stderr);
            continue;
        }
        lock_data(app);
        app->shared->counter = (int64_t)value;
        // write_log(app, "SET");
        unlock_data(app);
    }

    int input_error = ferror(stdin);
    if (input_error)
        perror("read stdin");
    atomic_store(&app->stop, 1);

#ifdef _WIN32
    if (WaitForSingleObject(thread, INFINITE) != WAIT_OBJECT_0)
        system_error("WaitForSingleObject thread");
    if (!CloseHandle(thread))
        system_error("CloseHandle thread");
#else
    thread_check(pthread_join(thread, NULL));
#endif

    lock_data(app);
    write_log(app, "EXIT_MAIN");
    unlock_data(app);
    return input_error ? EXIT_FAILURE : EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    int child = 0;
    if (argc == 3 && strcmp(argv[1], "--child") == 0 &&
        (strcmp(argv[2], "1") == 0 || strcmp(argv[2], "2") == 0)) {
        child = argv[2][0] - '0';
    } else if (argc != 1) {
        fputs("Usage: counter\n", stderr);
        return EXIT_FAILURE;
    }

    App app = {0};
    open_app(&app, argv[0]);
    int result = child ? run_child(&app, child - 1) : run_main(&app);
    close_app(&app);
    return result;
}