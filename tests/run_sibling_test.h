#ifndef FPGAGPU_RUN_SIBLING_TEST_H
#define FPGAGPU_RUN_SIBLING_TEST_H
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Keep the current HIL shell's test names and run isolation in a fresh
 * executable, not a fork of an initialized OpenCL runtime. */
static inline int run_sibling_test(const char *name)
{
    char path[4096];
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len < 0 || (size_t)len >= sizeof(path) - 1) {
        perror("readlink /proc/self/exe"); return 1;
    }
    path[len] = '\0';
    char *slash = strrchr(path, '/');
    if (!slash || strlen(name) >= sizeof(path) - (size_t)(slash + 1 - path)) {
        fprintf(stderr, "Cannot locate sibling test %s\n", name); return 1;
    }
    strcpy(slash + 1, name);
    printf("Running %s\n", name);
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) { perror("fork sibling test"); return 1; }
    if (!pid) {
        execl(path, name, (char *)NULL);
        perror(path);
        _exit(127);
    }
    int status;
    pid_t w;
    do { w = waitpid(pid, &status, 0); } while (w < 0 && errno == EINTR);
    if (w != pid || !WIFEXITED(status) || WEXITSTATUS(status)) {
        fprintf(stderr, "FAIL sibling test %s\n", name);
        return 1;
    }
    return 0;
}
#endif
