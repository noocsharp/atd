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
#include <termios.h>
#include <unistd.h>

#include "atd.h"
#include "encdec.h"
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
#define BACKEND 4
#define SIGNALINT 5
#define RSRVD_FDS 6
#define MAX_FDS 16

#define BUFSIZE 256

char *startup[] = { "AT+CLIP=1\r", "AT+COLP=1\r" };

struct command_args cmddata[] = {
    [CMD_DIAL] = { ATD, { TYPE_STRING, TYPE_NONE} },
    [CMD_ANSWER] = { ATA, { TYPE_NONE} },
    [CMD_HANGUP] = { ATH, { TYPE_NONE} },
};

char *atcmds[] = {
    [ATD] = "ATD%s;\r",
    [ATA] = "ATA\r",
    [ATH] = "ATH\r",
    [CLCC] = "AT+CLCC\r",
};

char *argv0;

struct fdbuf {
    ssize_t inlen;
    ssize_t outlen;
    char in[BUFSIZE]; /* stuff that needs to go *into* the fd */
    char out[BUFSIZE]; /* stuff that came out *from* the fd */
    char *inptr; /* where to write new data to */
    char *outptr;
};

struct command currentcmd;
int cmd_progress;
bool active_command = false;
enum atcmd currentatcmd;
int calld = -1;

struct fdbuf fdbufs[MAX_FDS] = {0};
struct pollfd fds[MAX_FDS];

struct call calls[MAX_CALLS];

/* add one command to queue, returns the number of bytes intepreted if the
 * command was validated and added successfully, -1 if the queue is full, -2 if
 * the command is invalid but terminated */
ssize_t cmdadd(int index) {
    struct command cmd = {index, CMD_NONE, NULL};
    char *ptr = fdbufs[index].out;
    size_t count = 0;

    if (cmdq.count == QUEUE_SIZE)
        return -1;

    cmd.op = *(ptr++);
    switch (cmd.op) {
    case CMD_DIAL:
        count = dec_str(ptr, &cmd.data.dial.num);
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
    case CMD_CALL_EVENTS:
        fprintf(stderr, "received request call events\n");
        if (calld == -1)
            calld = index;
        goto end;
        break;
    default:
        fprintf(stderr, "got code: %d\n", cmd.op);
        return -2;
    }

    /* we already checked that the queue has enough capacity */
    if (cmd.op)
        command_enqueue(cmd);

end:
    return count + 1;
}

int
send_status(int fd, enum status status)
{
    fprintf(stderr, "send_status\n");
    char st = status;
    ssize_t ret;
    do {
        ret = write(fd, &st, 1);
        if (ret == -1)
            return -1;
    } while (ret != 1);
    return 0;
}

/* [0] = STATUS_CALL
   [1] = # of update entries
   list of update entries follows
   update entry is a callstatus, followed by a string containing the phone number */
int
send_call_status(enum callstatus status, char *num)
{
    if (calld != -1) {
        fprintf(stderr, "update call status\n");
        return atd_status_call(fds[calld].fd, status, num);
    }

    return 0;
}

int
send_clip(char *start, size_t len)
{
    char number[PHONE_NUMBER_MAX_LEN + 1];
    int ret = sscanf(start, "+CLIP: \"%[+1234567890ABCD]\"", number);
    if (ret != 1)
        return -1;

    return send_call_status(CALL_INCOMING, number);
}

int
send_colp(char *start, size_t len)
{
    char number[PHONE_NUMBER_MAX_LEN + 1];
    int ret = sscanf(start, "+COLP: \"%[+1234567890ABCD]\"", number);
    if (ret != 1)
        return -1;

    return send_call_status(CALL_ANSWERED, number);
}

int
lprint(char *buf, size_t len)
{
    for (int i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            puts("<LF>");
        } else if (buf[i] == '\r') {
            puts("<CR>");
        } else {
            putchar(buf[i]);
        }
    }
}

size_t
handle_resp(int fd, int idx)
{
    fprintf(stderr, "%s\n", __func__);
    char *start = fdbufs[idx].out, *ptr;
    enum status status = 0;

    /* ignore lines without content */
    if (memcmp(start, "\n", 1) == 0)
        return 1;
    else if (memcmp(start, "\r\n", 2) == 0)
        return 2;

    ptr = memchr(fdbufs[idx].out, '\n', fdbufs[idx].outlen);
    if (ptr == NULL)
        return 0;

    lprint(start, ptr - start);

    size_t length = 1 + ptr - fdbufs[idx].out;

    if (strncmp(start, "OK", sizeof("OK") - 1) == 0) {
        status = STATUS_OK;
        active_command = false;
        if (currentatcmd == ATD) {
            if (send_call_status(CALL_DIALING, currentcmd.data.dial.num) < 0)
                fprintf(stderr, "failed to send call status\n");
        }

        currentcmd.op = CMD_NONE;
        currentatcmd = ATNONE;
        fprintf(stderr, "got OK\n");
    } else if (strncmp(start, "ERROR", sizeof("ERROR") - 1) == 0) {
        status = STATUS_ERROR;
        active_command = false;
        fprintf(stderr, "got ERROR\n");
    } else if (strncmp(start, "NO CARRIER", sizeof("NO CARRIER") - 1) == 0) {
        if (currentcmd.op == CMD_ANSWER || currentcmd.op == CMD_DIAL) {
            active_command = false;
            status = STATUS_ERROR;
            currentcmd.op = CMD_NONE;
            currentatcmd = ATNONE;
        }

        if (send_call_status(CALL_INACTIVE, "") < 0) {
            fprintf(stderr, "failed to send call status\n");
        }
    } else if (strncmp(start, "RING", sizeof("RING") - 1) == 0) {
        fprintf(stderr, "got RING\n");
    } else if (strncmp(start, "CONNECT", sizeof("CONNECT") - 1) == 0) {
        fprintf(stderr, "got CONNECT\n");
    } else if (strncmp(start, "BUSY", sizeof("BUSY") - 1) == 0) {
        fprintf(stderr, "got BUSY\n");
    } else if (strncmp(start, "+CLIP", sizeof("+CLIP") - 1) == 0) {
        fprintf(stderr, "got +CLIP\n");

        send_clip(start, ptr - start);
    } else if (strncmp(start, "+COLP", sizeof("+COLP") - 1) == 0) {
        fprintf(stderr, "got +COLP\n");

        send_colp(start, ptr - start);
    }


    if (status && fd > 0)
        send_status(fd, status);

    fprintf(stderr, "%s: %d\n", __func__, length);
    return length;
}

ssize_t
fdbuf_write(int idx)
{
    int wr = write(fds[idx].fd, &fdbufs[idx].in, fdbufs[idx].inlen);
    if (wr == -1)
        return -1;

    fdbufs[idx].inlen -= wr;
    fdbufs[idx].inptr -= wr;
    memmove(fdbufs[idx].in, fdbufs[idx].in + wr, BUFSIZE - wr);

    return wr;
}

ssize_t
fdbuf_read(int idx)
{
    int r = read(fds[idx].fd, fdbufs[idx].outptr, BUFSIZE - fdbufs[idx].outlen);
    if (r == -1)
        return -1;

    fdbufs[idx].outlen += r;
    fdbufs[idx].outptr += r;

    return r;
}

bool
send_command(int idx, enum atcmd atcmd, union atdata atdata)
{
    ssize_t ret;
    fprintf(stderr, "send command: %d\n", atcmd);
    if (atcmd == ATD) {
        ret = snprintf(fdbufs[idx].in, BUFSIZE, atcmds[atcmd], atdata.dial.num);
    } else {
        ret = snprintf(fdbufs[idx].in, BUFSIZE, atcmds[atcmd]);
    }
    if (ret >= BUFSIZE) {
        warn("AT command too long!");
        return false;
    }
    fdbufs[idx].inptr = fdbufs[idx].in;
    fdbufs[idx].inlen = ret;

    ret = fdbuf_write(idx);
    if (ret == -1) {
        warn("failed to write to backend!");
        return false;
    }
    active_command = true;
    currentatcmd = atcmd;
    return true;
}

static int
setup_modem_tty(int fd)
{
    struct termios config;

    tcgetattr(fd, &config);
    config.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | INPCK | ICRNL | INLCR | ISTRIP | IXON);
    config.c_oflag = 0;
    config.c_lflag &= ~(ECHO | ECHONL | ICANON | IEXTEN | ISIG);
    config.c_cflag &= ~(CSIZE | PARENB);
    config.c_cflag |= CS8;
    config.c_cc[VMIN] = 1;
    config.c_cc[VTIME] = 0;

    cfsetispeed(&config, B115200);
    cfsetospeed(&config, B115200);

    tcsetattr(fd, TCSANOW, &config);
    return 1;
}

int main(int argc, char *argv[])
{
    argv0 = argv[0];

    if (argc != 2)
        die("exactly 2 arguments required\n");

    struct sockaddr_un sockaddr = {
        .sun_family = AF_UNIX,
        .sun_path = ATD_SOCKET ,
    };

    struct sockaddr_un backaddr = {
        .sun_family = AF_UNIX,
        .sun_path = "/tmp/atsim",
    };
    struct command cmd;
    ssize_t ret = 0;
    sigset_t mask;
    char *next;

    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);

    if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1)
        die("failed to block SIGINT:");

    int sigintfd = signalfd(-1, &mask, 0);
    if (sigintfd == -1)
        die("failed to create signalfd:");


    for (int i = 0; i < MAX_FDS; i++)
        fds[i].fd = -1;

    int backsock;

#ifdef DEBUG
    backsock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (backsock == -1) {
        warn("failed to create backend socket:");
        goto error;
    }

    const int val = 1;
    if (setsockopt(backsock, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) == -1) {
        warn("failed to set SO_REUSEADDR:");
    }

    if (connect(backsock, (struct sockaddr *) &backaddr, sizeof(backaddr)) == -1) {
        warn("failed to connect to backend:");
        goto error;
    }
#else
    backsock = open(argv[1], O_RDWR | O_NOCTTY);
    if (backsock == -1) {
        warn("failed to connect to tty:");
        goto error;
    }
#endif

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

    char startupresp[256];

    fprintf(stderr, "%d startup commands\n", sizeof(startup));
    for (int i = 0; i < sizeof(startup) / sizeof(startup[0]); i++) {
        fprintf(stderr, "startup: %s, %i\n", startup[i], i);
        ret = xwrite(fds[BACKEND].fd, startup[i], strlen(startup[i]));
        if (ret == -1)
            goto error;

        ret = read(fds[BACKEND].fd, startupresp, sizeof(startupresp));
        if (ret == -1)
            goto error;

        fprintf(stderr, startupresp);
        memset(startupresp, 0, sizeof(startupresp));
    }

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

        /* handle clients */
        for (int i = RSRVD_FDS; i < MAX_FDS; i++) {
            if (fds[i].fd == -1)
                continue;

            if (fds[i].revents & POLLHUP) {
                /* TODO check if out buffer is empty */
                warn("closed connection!");
                close(fds[i].fd);
                fds[i].fd = -1;
                if (i == calld)
                    calld = -1;
            } else if (fds[i].revents & POLLIN) {
                if (fdbuf_read(i) == -1) {
                    warn("failed to read from fd %d:", i);
                    break;
                }
                // parsecmd should parse as much as it can, letting us know how
                // much was left unparsed so we can move it to the beginning of
                // the buffer.
                ret = cmdadd(i);

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
            } else if (fds[i].revents & POLLOUT) {
                if (fdbuf_write(i) == -1) {
                    warn("failed to write to fd %d:", i);
                    break;
                }

                if (fdbufs[i].inlen == 0)
                    POLLDROP(fds[i], POLLOUT);
            }
        }

        if (fds[BACKEND].revents & POLLIN) {
            ret = fdbuf_read(BACKEND);
            if (ret == -1) {
                warn("failed to read from backend:");
                break;
            }
        }

        while (fdbufs[BACKEND].outlen) {
            ret = handle_resp(fds[cmd.index].fd, BACKEND);
            if (ret == 0)
                break;

            memmove(fdbufs[BACKEND].out, fdbufs[BACKEND].out + ret, BUFSIZE - ret);
            fdbufs[BACKEND].outlen -= ret;
            fdbufs[BACKEND].outptr -= ret;
        }

        /* send next command to modem */
        if (fds[BACKEND].revents & POLLOUT) {
            fprintf(stderr, "have a command!\n");

            cmd = command_dequeue();
            assert(cmd.op != CMD_NONE);

            if (!send_command(BACKEND, cmddata[cmd.op].atcmd, cmd.data))
                break;

            currentcmd = cmd;

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
                fdbufs[i].inptr = fdbufs[i].in;
                warn("accepted connection!", fds[i].fd);
                break;
            }
        }

        /* note that this doesn't take effect until the next poll cycle...
         * maybe this can be replaced with something more integrated? */
        if (cmdq.count && !active_command)
            POLLADD(fds[BACKEND], POLLOUT);
        else
            POLLDROP(fds[BACKEND], POLLOUT);
    }

error:
    for (int i = STDERR+1; i < MAX_FDS; i++) {
        if (fds[i].fd > 0)
            close(fds[i].fd);
    }
    unlink(ATD_SOCKET);
}
