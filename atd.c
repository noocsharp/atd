#define _GNU_SOURCE // memmem

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
#include "queue.h"

#define AT_MAX 256
#define ATD_SOCKET "/tmp/atd-socket"

#define POLLADD(fd, arg) fd.events |= (arg)
#define POLLDROP(fd, arg) fd.events &= ~(arg)

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

struct fdbuf {
    ssize_t inlen;
    ssize_t outlen;
    char in[BUFSIZE]; /* stuff that needs to go *into* the fd */
    char out[BUFSIZE]; /* stuff that came out *from* the fd */
    char *inptr; /* where to write new data to */
    char *outptr;
};

ssize_t
parse_str(char *in, char **out)
{
    short len;
    char *ptr = in;
    len = ptr[0] + (ptr[1] << 8);
    assert(len < BUFSIZE);
    ptr += 2;

    *out = malloc(len);
    if (!(*out))
        return -1;

    memcpy(*out, ptr, len);
    return len + 2;
}

/* add one command to queue, returns the number of bytes intepreted if the
 * command was validated and added successfully, -1 if the queue is full, -2 if
 * the command is invalid but terminated */
ssize_t cmdadd(struct fdbuf fdbuf) {
    struct command cmd = {0, CMD_NONE, NULL};
    char *ptr = fdbuf.out;
    size_t count = 0;

    if (cmdq.count == QUEUE_SIZE)
        return -1;

    cmd.op = *(ptr++);
    switch (cmd.op) {
    case CMD_DIAL:
        count = parse_str(ptr, &cmd.data.dial.num);
        if (count == -1)
            return -1;

        fprintf(stderr, "received dial with number %*.*s\n", count, count, cmd.data.dial.num);
        break;
    case CMD_ANSWER:
        fprintf(stderr, "received answer\n");
        break;
    case CMD_HANGUP:
        fprintf(stderr, "received hangup\n");
        break;
    default:
        fprintf(stderr, "got code: %d\n", cmd.op);
    }

    /* we already checked that the queue has enough capacity */
    command_enqueue(cmd);

    return count + 1;
}

size_t
handle_resp(struct fdbuf *fdbuf)
{
    fprintf(stderr, "handle_resp start\n");
    char *start = fdbuf->out, *ptr = memmem(fdbuf->out, fdbuf->outlen, "\r\n", 2);

    for (int i = 0; i < fdbuf->outlen; i++) {
        fprintf(stderr, "%x ", fdbuf->out[i]);
    }
    fprintf(stderr, "\n");

    if (ptr == NULL)
        return 0;

    // find next line with content
    while (start == ptr) {
        ptr += sizeof("\r\n");
        start = ptr;
        ptr = memmem(start, BUFSIZE - (ptr - fdbuf->out), "\r\n", 2);
    }

    if (strncmp(start, "NO CARRIER", sizeof("NO CARRIER") - 1) == 0) {
        fprintf(stderr, "got NO CARRIER\n");
    } else if (strncmp(start, "OK", sizeof("OK") - 1) == 0) {
        fprintf(stderr, "got OK\n");
    } else if (strncmp(start, "RING", sizeof("RING") - 1) == 0) {
        fprintf(stderr, "got RING\n");
    }

    ptr += 2;

    fprintf(stderr, "handle_resp: %d\n", ptr - fdbuf->out);
    return ptr - fdbuf->out;
}

ssize_t
fdbuf_write(int fd, struct fdbuf *fdbuf)
{
    int wr = write(fd, &fdbuf->in, fdbuf->inlen);
    if (wr == -1)
        return -1;

    fdbuf->inlen -= wr;
    fdbuf->inptr -= wr;
    memmove(fdbuf->in, fdbuf->in + wr, BUFSIZE - wr);

    return wr;
}

ssize_t
fdbuf_read(int fd, struct fdbuf *fdbuf)
{
    int r = read(fd, fdbuf->outptr, BUFSIZE - fdbuf->outlen);
    if (r == -1)
        return -1;

    fdbuf->outlen += r;
    fdbuf->outptr += r;

    return r;
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

    ssize_t ret = 0;
    struct pollfd fds[MAX_FDS];
    sigset_t mask;
    char *next;
    bool active_command = false;

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
    if (backsock == -1) {
        warn("failed to create backend socket:");
        goto error;
    }

    if (connect(backsock, (struct sockaddr *) &backaddr, sizeof(backaddr)) == -1) {
        warn("failed to connect to backend:");
        goto error;
    }

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == -1) {
        warn("failed to create socket:");
        goto error;
    }

    if (bind(sock, (struct sockaddr *) &sockaddr, sizeof(sockaddr)) == -1) {
        warn("failed to bind to socket:");
        goto error;
    }

    if (listen(sock, 50) == -1) {
        warn("failed to set socket to listening:");
        goto error;
    }

    fds[STDIN].fd = STDIN;
    fds[STDIN].events = POLLIN;
    fds[STDOUT].fd = STDOUT;
    fds[STDOUT].events = 0;
    fds[LISTENER].fd = sock;
    fds[LISTENER].events = POLLIN;
    fds[BACKEND].fd = backsock;
    fds[BACKEND].events = POLLIN;
    fdbufs[BACKEND].outptr = fdbufs[BACKEND].out;
    fds[SIGNALINT].fd = sigintfd;
    fds[SIGNALINT].events = POLLIN;

    while (true) {
        if (poll(fds, sizeof(fds) / sizeof(fds[0]), -1) == -1) {
            warn("poll failed");
            break;
        }

        if ((fds[SIGNALINT].revents & POLLIN) ||
            (fds[LISTENER].revents | fds[BACKEND].revents) & POLLHUP) {
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
                if (fdbuf_read(fds[i].fd, &fdbufs[i]) == -1) {
                    warn("failed to read from fd %d:", i);
                    break;
                }
                // parsecmd should parse as much as it can, letting us know how
                // much was left unparsed so we can move it to the beginning of
                // the buffer.
                ret = cmdadd(fdbufs[i]);
                if (ret != -1) {
                    assert(ret <= BUFSIZE);
                    fdbufs[i].outlen -= ret;
                    memmove(fdbufs[i].out, fdbufs[i].out + ret, fdbufs[i].outlen);
                    assert(fdbufs[i].outlen >= 0);
                    fdbufs[i].outptr = fdbufs[i].out + fdbufs[i].outlen;
                } else {
                    warn("failed to parse command\n");
                    break;
                }
            }
        }

        if (fds[BACKEND].revents & POLLIN) {
            fprintf(stderr, "len: %d\n", fdbufs[BACKEND].outlen);
            ret = fdbuf_read(fds[BACKEND].fd, &fdbufs[BACKEND]);
            if (ret == -1) {
                warn("failed to read from backend:");
                break;
            }

            ret = handle_resp(&fdbufs[BACKEND]);
            memmove(fdbufs[BACKEND].out, fdbufs[BACKEND].out + ret, BUFSIZE - ret);
            fdbufs[BACKEND].outlen -= ret;
            fdbufs[BACKEND].outptr -= ret;
            /* TODO: temporary, only change this if it is a response to the command */
            active_command = false;
        }

        if (cmdq.count && !active_command)
            POLLADD(fds[BACKEND], POLLOUT);
        else
            POLLDROP(fds[BACKEND], POLLOUT);

        /* send next command to modem */
        if (fds[BACKEND].revents & POLLOUT) {
            fprintf(stderr, "have a command!\n");
            struct command cmd = command_dequeue();
            fprintf(stderr, "op: %d\n", cmd.op);

            if (!cmd.op)
                continue;

            if (cmd.op == CMD_DIAL) {
                ret = snprintf(fdbufs[BACKEND].in, BUFSIZE, cmddata[cmd.op].atcmd, cmd.data.dial.num);
            } else {
                ret = snprintf(fdbufs[BACKEND].in, BUFSIZE, cmddata[cmd.op].atcmd);
            }
            fprintf(stderr, "after data\n");
            if (ret >= BUFSIZE) {
                warn("AT command too long!");
                break;
            }
            fdbufs[BACKEND].inptr = fdbufs[BACKEND].in;
            fdbufs[BACKEND].inlen = ret;

            int wr = fdbuf_write(fds[BACKEND].fd, &fdbufs[BACKEND]);
            if (wr == -1) {
                warn("failed to write to backend!");
                break;
            }
            fprintf(stderr, "done writing: %d\n", fdbufs[BACKEND].inlen);
            active_command = true;

            /* don't write any more until we hear back */
            if (fdbufs[BACKEND].inlen == 0) {
                POLLDROP(fds[BACKEND], POLLOUT);
                POLLADD(fds[BACKEND], POLLIN);
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

error:
    for (int i = STDERR+1; i < MAX_FDS; i++) {
        if (fds[i].fd > 0)
            close(fds[i].fd);
    }
    unlink(ATD_SOCKET);
}
