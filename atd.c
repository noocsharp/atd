#include <assert.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

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
    ssize_t inlen;
    ssize_t outlen;
    char in[BUFSIZE]; /* stuff that needs to go *into* the fd */
    char out[BUFSIZE]; /* stuff that came out *from* the fd */
    char *inptr;
    char *outptr;
};

/* add one command to queue, returns 0 if command was validated and added successfully, -1 if the command is invalid but terminated, and -2 if the command is invalid and unterminated. Then sets next to point after the command */
/* TODO what if queue is full? */
ssize_t cmdadd(struct fdbuf fdbuf) {
    struct command cmd;
    char *end = memchr(fdbuf.out, '\0', fdbuf.outlen);
    char *ptr = fdbuf.out;
    size_t count = 0;

    /* given that we have a max length for a command, if the buffer does not contain a terminated command, we can assume that the command is incomplete and read mode data from the fd until the buffer is full. If it still doesn't contain a terminated command, then we know that we are dealing with garbage, and we throw it out, and inform the client. In case of garbage, two '\0' in a row flush the buffer */
    if (end == NULL)
        return -2;

    switch (*(ptr++)) {
    case CMD_DIAL:
        cmd.op = CMD_DIAL;
        /* TODO replace this malloc */
        cmd.data = malloc(sizeof(struct data_dial));
        if (cmd.data == NULL)
            goto bad;
        while (*ptr != '\0') {
            if (count > PHONE_NUMBER_MAX_LEN || strchr(DIALING_DIGITS, *ptr) == NULL)
                goto bad;

            ((struct data_dial *)cmd.data)->num[count] = *(ptr++);
            count++;
        }
        ((struct data_dial *)cmd.data)->count = count;
        
        fprintf(stderr, "received dial with number ");
        for (int i = 0; i < count; i++)
            fputc(((struct data_dial *)cmd.data)->num[i], stderr);
        fputc('\n', stderr);
        break;
    case CMD_ANSWER:
        ptr++;
        if (*ptr != '\0')
            goto bad;
        fprintf(stderr, "received answer\n");
    case CMD_HANGUP:
        ptr++;
        if (*ptr != '\0')
            goto bad;
        fprintf(stderr, "received answer\n");
    }

    return ptr - fdbuf.out;

bad:
    fdbuf.outptr = end + 1;
    return -1;
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

    ssize_t len = 0, written = 0, ret = 0;
    struct pollfd fds[MAX_FDS];
    sigset_t mask;
    char *next;

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
        if ((fds[SIGNALINT].revents & POLLIN) || fds[LISTENER].revents & POLLHUP) {
            warn("time to die");
            break;
        }

        for (int i = RSRVD_FDS; i < MAX_FDS; i++) {
            if (fds[i].fd == -1)
                continue;

            if (fds[i].revents & POLLHUP) {
                /* TODO check if out buffer is empty */
                warn("closed connection!");
                close(fds[i].fd);
                fds[i].fd = -1;
            } else if (fds[i].revents & POLLIN) {
                len = read(fds[i].fd, fdbufs[i].outptr, BUFSIZE - fdbufs[i].outlen);
                if (len == -1) {
                    warn("failed to read from fd %d:", i);
                    break;
                }

                fdbufs[i].outlen += len;
                fdbufs[i].outptr += len;
                // parsecmd should parse as much as it can, letting us know how much was left unparsed so we can move it to the beginning of the buffer.
                len = cmdadd(fdbufs[i]);
                if (len == -2) {
                    continue;
                } else if (len == -1) {
                    // TODO mark paused
                } else {
                    assert(len <= BUFSIZE);
                    fdbufs[i].outlen -= len;
                    memmove(fdbufs[i].out, fdbufs[i].out + len, fdbufs[i].outlen);
                    assert(fdbufs[i].outlen >= 0);
                    fdbufs[i].outptr = fdbufs[i].out + fdbufs[i].outlen;
                }
            }
        }

        /* TODO hook stdin up to command input? */
        if (fds[STDIN].revents & POLLIN) {
            len = read(fds[STDIN].fd, &fdbufs[STDIN].out, BUFSIZE);
            if (len == -1) {
                warn("failed to read from stdin");
                break;
            }

            fdbufs[STDIN].outlen = len;
            fdbufs[STDIN].outptr = fdbufs[STDIN].out;
            fds[STDIN].events &= ~POLLIN;
            fds[STDOUT].events |= POLLOUT;
        }

        /* TODO write to stdout when a command is received */
        if (fds[STDOUT].revents & POLLOUT) {
            int wr = write(fds[STDOUT].fd, &fdbufs[STDIN].out, fdbufs[STDIN].outlen);
            if (wr == -1) {
                warn("failed to write to stdout");
                break;
            }

            fdbufs[STDIN].outlen -= wr;
            fdbufs[STDIN].outptr += wr;
            if (fdbufs[STDIN].outlen == 0) {
                fds[STDOUT].events &= ~POLLOUT;
                fds[STDIN].events |= POLLIN;
            }
        }

        if (fds[LISTENER].revents & POLLIN) {
            /* TODO come up with a better way of assigning indices? */
            for (int i = RSRVD_FDS; i < MAX_FDS; i++) {
                if (fds[i].fd != -1)
                    continue;
                
                fds[i].fd = accept(fds[LISTENER].fd, NULL, NULL);
                if (fds[i].fd == -1) {
                    warn("failed to accept connection");
                    break;
                }
                fds[i].events = POLLIN;
                fdbufs[i].outptr = fdbufs[i].out;
                warn("accepted connection!", fds[i].fd);
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
