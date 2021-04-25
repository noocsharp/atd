#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "atd.h"
#include "util.h"

#define AT_MAX 256
#define ATD_SOCKET "/tmp/atd-socket"

#define QUEUE_MAX 100
#define STDOUT 0
#define STDIN 1
#define STDERR 2
#define LISTENER 3
#define BACKEND 5
#define SIGNALINT 6
#define RSRVD_FDS 6
#define MAX_FDS 16

#define BUFSIZE 256

char *argv0;

struct {
    char *ptrs[QUEUE_MAX];
    char **start;
    char **next;
    size_t count;
} atq;

struct fdbuf {
    ssize_t len;
    char buf[BUFSIZE];
    char *ptr;
};

int at_enqueue(char *cmd)
{
    if (atq.count != 0 && atq.start == atq.next) {
        fprintf(stderr, "command queue full");
        free(cmd);
        return 1;
    }

    *atq.next = cmd;
    atq.next = (char **)((atq.next + 1 - atq.ptrs) % QUEUE_MAX);
    atq.count++;
    return 0;
}

int cmd_call(const char *buf)
{
    char num[PHONE_NUMBER_MAX_LEN];
    char *command;
    size_t numlen = 0;

    buf += 1;
    while (buf) {
        num[numlen++] = *buf;
    }

    /* TODO don't use malloc here */
    command = malloc(numlen + 5);
    if (!command)
        return 1;

    /* ATD is used to initiate a phone call */
    if (snprintf(command, AT_MAX, "ATD%s;", num) > AT_MAX) {
        free(command);
        return 1;
    }

    return at_enqueue(command);
}

int cmd_answer() {
    char *command = malloc(4);
    if (!command)
        return 1;

    strcpy(command, "ATA");

    return at_enqueue(command);
}

int cmd_hangup() {
    char *command = malloc(4);
    if (!command)
        return 1;

    strcpy(command, "ATH");

    return at_enqueue(command);
}

int dispatch(const char *buf)
{
    switch (buf[0]) {
        case CMD_DIAL: return cmd_call(buf);
        case CMD_ANSWER: return cmd_answer(buf);
        case CMD_HANGUP: return cmd_hangup(buf);
    }
}

/* caller is responsible for freeing */
char *at_sendnext(int fd)
{
    char *cmd = *atq.start;
    size_t left = strlen(cmd);
    ssize_t ret;
    while (left) {
        ret = write(fd, cmd, left);
        if (ret == -1) {
            fprintf(stderr, "failed write when sending command %s", cmd);
            return NULL;
        }
        cmd += ret;
        left -= ret;
    }

    cmd = *atq.start;
    atq.start = (char **)((atq.start + 1 - atq.ptrs) % QUEUE_MAX);
    atq.count--;
    return cmd;
}

int main(int argc, char *argv[])
{
    argv0 = argv[0];

    struct sockaddr_un sockaddr = {
        .sun_family = AF_UNIX,
        .sun_path = ATD_SOCKET ,
    };

    struct sockaddr_un backaddr = {
        .sun_family = AF_UNIX,
        .sun_path = "/tmp/atsim",
    };

    ssize_t len = 0, written = 0;
    struct pollfd fds[MAX_FDS];
    sigset_t mask;

    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);

    if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1)
        die("failed to block SIGINT:");

    int sigintfd = signalfd(-1, &mask, 0);
    if (sigintfd == -1)
        die("failed to create signalfd:");

    /* this is used to store read data from fds, and length */
    struct fdbuf fdbufs[MAX_FDS] = {0};

    for (int i = 0; i < MAX_FDS; i++)
        fds[i].fd = -1;

    int backsock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (backsock == -1)
        die("failed to create backend socket:");

    int back = connect(backsock, (struct sockaddr *) &backaddr, sizeof(struct sockaddr_un));
    if (back == -1) {
        die("failed to connect to backend:");
    }

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == -1) {
        die("failed to create socket:");
    }

    /* TODO replace these dies with warns and gotos so the socket file gets removed properly */
    if (bind(sock, (struct sockaddr *) &sockaddr, sizeof(struct sockaddr_un)) == -1) {
        die("failed to bind to socket:");
    }

    if (listen(sock, 50) == -1) {
        die("failed to set socket to listening:");
    }

    fds[STDIN].fd = STDIN;
    fds[STDIN].events = POLLIN;
    fds[STDOUT].fd = STDOUT;
    fds[STDOUT].events = 0;
    fds[LISTENER].fd = sock;
    fds[LISTENER].events = POLLIN;
    fds[BACKEND].fd = back;
    fds[BACKEND].events = 0;
    fds[SIGNALINT].fd = sigintfd;
    fds[SIGNALINT].events = POLLIN;

    while (true) {
        if (poll(fds, sizeof(fds) / sizeof(fds[0]), -1) == -1) {
            warn("poll failed");
            break;
        }

        /* handle interrupt */
        if ((fds[SIGNALINT].revents & POLLIN) || (fds[LISTENER].revents & SIGHUP)) {
            warn("time to die");
            break;
        }

        for (int i = RSRVD_FDS; i < MAX_FDS; i++) {
            if (fds[i].revents & POLLHUP) {
                close(fds[i].fd);
                fds[i].fd = -1;
            } else if (fds[i].revents & POLLIN) {
                /* TODO not this */
                close(fds[i].fd);
                fds[i].fd = -1;
            }
        }

        /* TODO hook stdin up to command input? */
        if (fds[STDIN].revents & POLLIN) {
            len = read(fds[STDIN].fd, &fdbufs[STDIN].buf, BUFSIZE);
            if (len == -1) {
                warn("failed to read from stdin");
                break;
            }

            fdbufs[STDIN].len = len;
            fdbufs[STDIN].ptr = fdbufs[STDIN].buf;
            fds[STDIN].events &= ~POLLIN;
            fds[STDOUT].events |= POLLOUT;
        }

        /* TODO write to stdout when a command is received */
        if (fds[STDOUT].revents & POLLOUT) {
            int wr = write(fds[STDOUT].fd, &fdbufs[STDIN].buf, fdbufs[STDIN].len);
            if (wr == -1) {
                warn("failed to write to stdout");
                break;
            }

            fdbufs[STDIN].len -= wr;

            fdbufs[BACKEND].len -= wr;
            fdbufs[STDIN].ptr += wr;
            if (fdbufs[STDIN].len == 0) {
                fds[STDOUT].events &= ~POLLOUT;
                fds[STDIN].events |= POLLIN;
            }
        }

        if (fds[LISTENER].revents & POLLIN) {
            for (int i = RSRVD_FDS; i < MAX_FDS; i++) {
                if (fds[i].fd != -1)
                    continue;
                
                fds[i].fd = accept(fds[LISTENER].fd, NULL, NULL);
                if (fds[i].fd == -1) {
                    warn("failed to accept connection");
                    break;
                }
                fds[i].events = POLLIN;
                warn("accepted connection!");
                break;
            }
        }
    }

    for (int i = 0; i < MAX_FDS; i++) {
        if (fds[i].fd > STDERR)
            close(fds[i].fd);
    }
    unlink(ATD_SOCKET);
}
