#ifndef FPGAGPU_MULTIPROCESS_TEST_H
#define FPGAGPU_MULTIPROCESS_TEST_H

/* Parent-controlled, serial commands to two independent forked workers.
 * No device FD or OpenCL context is created in the parent. */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

struct mp_worker {
    pid_t pid;
    int fd;
};

typedef int (*mp_entry)(unsigned, int);

static inline int mp_send(int fd, char value)
{
    ssize_t n;
    do { n = send(fd, &value, 1, MSG_NOSIGNAL); } while (n < 0 && errno == EINTR);
    return n == 1 ? 0 : -1;
}

static inline int mp_recv(int fd, char *value)
{
    ssize_t n;
    do { n = recv(fd, value, 1, 0); } while (n < 0 && errno == EINTR);
    return n == 1 ? 0 : -1;
}

static inline int mp_step(struct mp_worker *w, char command, const char *label)
{
    struct pollfd p = { .fd = w->fd, .events = POLLIN };
    char response;
    printf("%s\n", label);
    fflush(stdout);
    if (mp_send(w->fd, command) || poll(&p, 1, 30000) <= 0 ||
        !(p.revents & POLLIN) || mp_recv(w->fd, &response) || response != 'Y') {
        fprintf(stderr, "FAIL: %s (worker error, disconnect or timeout)\n", label);
        return -1;
    }
    return 0;
}

static inline int mp_start(struct mp_worker workers[2], mp_entry entry)
{
    fflush(NULL);
    for (unsigned i = 0; i < 2; ++i) {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) {
            perror("socketpair"); return -1;
        }
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork"); close(sv[0]); close(sv[1]); return -1;
        }
        if (!pid) {
            close(sv[0]);
            for (unsigned k = 0; k < i; ++k) close(workers[k].fd);
            int rc = entry(i, sv[1]);
            fflush(NULL);
            close(sv[1]);
            _exit(rc ? 1 : 0);
        }
        close(sv[1]);
        workers[i].pid = pid;
        workers[i].fd = sv[0];
    }
    return 0;
}

static inline int mp_finish(struct mp_worker workers[2], int failed)
{
    for (unsigned i = 0; i < 2; ++i) {
        struct mp_worker *w = &workers[i];
        if (w->pid <= 0) continue;
        if (!failed && mp_step(w, 'Q', "Close worker")) failed = 1;
        close(w->fd);
        w->fd = -1;
        if (failed) {
            /* Best effort; a kernel hang still needs external HIL recovery. */
            kill(w->pid, SIGKILL);
            waitpid(w->pid, NULL, WNOHANG);
        } else {
            int status;
            pid_t result;
            do { result = waitpid(w->pid, &status, 0); }
            while (result < 0 && errno == EINTR);
            if (result != w->pid || !WIFEXITED(status) || WEXITSTATUS(status))
                failed = 1;
        }
        w->pid = -1;
    }
    return failed ? 1 : 0;
}

#endif
