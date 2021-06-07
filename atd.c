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
#define BACKEND 5
#define SIGNALINT 6
#define RSRVD_FDS 6
#define MAX_FDS 16

#define BUFSIZE 256

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
bool check_call_status = false;
bool handling_urc = false;
enum atcmd currentatcmd;
int calld = -1;

struct fdbuf fdbufs[MAX_FDS] = {0};

struct call calls[MAX_CALLS];
struct call calls2[MAX_CALLS];

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

struct call
parseclcc(char *start, size_t len)
{
    unsigned char idx;
    unsigned char dir;
    unsigned char stat;
    unsigned char mode;
    unsigned char mpty;
    char number[PHONE_NUMBER_MAX_LEN];
    unsigned char type;
    unsigned char alpha;

    struct call call = { .present = true };

    int ret = sscanf(start, "+CLCC: %hhu,%hhu,%hhu,%hhu,%hhu,\"%[+1234567890ABCD]\",%hhu,%hhu", &idx, &dir, &call.status, &mode, &mpty, call.num, &type, &alpha);

    fprintf(stderr, "clcc: got %d successful matches\n", ret);

    calls2[idx] = call;
}

/* [0] = STATUS_CALL
   [1] = # of update entries
   list of update entries follows
   update entry is a callstatus, followed by a string containing the phone number */
void
update_call_status()
{
    fprintf(stderr, "update call status\n");
    char buf[(PHONE_NUMBER_MAX_LEN + 2) * MAX_CALLS + 2];
    char *ptr;
    size_t len;
    ssize_t ret;

    if (calld < 0)
        return;

    buf[0] = STATUS_CALL;
    buf[1] = 0;

    ptr = buf + 2;

    for (int i = 1; i < MAX_CALLS; i++) {
        if (!(calls[i].present || calls2[i].present))
            continue;

        if ((calls[i].status == calls2[i].status) && strcmp(calls[i].num, calls2[i].num) == 0)
            continue;
        fprintf(stderr, "update POINT 0: %d, %s\n", i, calls2[i].num);

        len = strlen(calls2[i].num);
        *ptr = len;
        strcpy(ptr + 1, calls2[i].num);
        ptr += len + 1;

        buf[1]++;
    }

    fprintf(stderr, "update POINT 1: %d\n", ptr - buf);
    memcpy(fdbufs[calld].inptr, buf, ptr - buf);
    fdbufs[calld].inlen += ptr - buf;

    fprintf(stderr, "update POINT 2\n");
    memcpy(calls, calls2, MAX_CALLS*sizeof(calls[0]));
    memset(calls2, 0, MAX_CALLS*sizeof(calls[0]));
    fprintf(stderr, "update POINT 3\n");
}

size_t
handle_resp(int fd, int idx)
{
    fprintf(stderr, "handle_resp start\n");
    char *start = fdbufs[idx].out, *ptr = memmem(fdbufs[idx].out, fdbufs[idx].outlen, "\r\n", 2);
    enum status status = 0;

    if (ptr == NULL)
        return 0;

    // find next line with content
    while (start == ptr) {
        ptr += sizeof("\r\n") - 1;
        start = ptr;
        ptr = memmem(start, fdbufs[idx].outlen - (ptr - fdbufs[idx].out), "\r\n", 2);
    }

    if (strncmp(start, "OK", sizeof("OK") - 1) == 0) {
        status = STATUS_OK;
        active_command = false;
        if (handling_urc)
            handling_urc = false;
        if (currentatcmd == CLCC) {
            update_call_status();
            check_call_status = false;
        }
        fprintf(stderr, "got OK\n");
    } else if (strncmp(start, "ERROR", sizeof("ERROR") - 1) == 0) {
        status = STATUS_ERROR;
        active_command = false;
        fprintf(stderr, "got ERROR\n");
    } else if (strncmp(start, "NO CARRIER", sizeof("NO CARRIER") - 1) == 0) {
        check_call_status = true;
        fprintf(stderr, "got NO CARRIER\n");
    } else if (strncmp(start, "RING", sizeof("RING") - 1) == 0) {
        check_call_status = true;
        fprintf(stderr, "got RING\n");
    } else if (strncmp(start, "CONNECT", sizeof("CONNECT") - 1) == 0) {
        check_call_status = true;
        fprintf(stderr, "got CONNECT\n");
    } else if (strncmp(start, "BUSY", sizeof("BUSY") - 1) == 0) {
        check_call_status = true;
        fprintf(stderr, "got BUSY\n");
    } else if (strncmp(start, "+CLCC", sizeof("+CLCC") - 1) == 0) {
        fprintf(stderr, "got +CLCC\n");
        assert(check_call_status);

        struct call call = parseclcc(start, ptr - start);
    }

    if (status && fd > 0)
        send_status(fd, status);

    ptr += 2;

    fprintf(stderr, "handle_resp: %d\n", ptr - fdbufs[idx].out);
    return ptr - fdbufs[idx].out;
}

ssize_t
fdbuf_write(int fd, int idx)
{
    int wr = write(fd, &fdbufs[idx].in, fdbufs[idx].inlen);
    if (wr == -1)
        return -1;

    fdbufs[idx].inlen -= wr;
    fdbufs[idx].inptr -= wr;
    memmove(fdbufs[idx].in, fdbufs[idx].in + wr, BUFSIZE - wr);

    return wr;
}

ssize_t
fdbuf_read(int fd, int idx)
{
    int r = read(fd, fdbufs[idx].outptr, BUFSIZE - fdbufs[idx].outlen);
    if (r == -1)
        return -1;

    fdbufs[idx].outlen += r;
    fdbufs[idx].outptr += r;

    return r;
}

bool
send_command(int fd, int idx, enum atcmd atcmd, union atdata atdata)
{
    ssize_t ret;
    fprintf(stderr, "send command\n");
    if (atcmd == ATD) {
        ret = snprintf(fdbufs[idx].in, BUFSIZE, atcmds[atcmd], atdata.dial.num);
    } else {
        ret = snprintf(fdbufs[idx].in, BUFSIZE, atcmds[atcmd]);
    }
    fprintf(stderr, "after data\n");
    if (ret >= BUFSIZE) {
        warn("AT command too long!");
        return false;
    }
    fdbufs[idx].inptr = fdbufs[idx].in;
    fdbufs[idx].inlen = ret;

    ret = fdbuf_write(fd, idx);
    if (ret == -1) {
        warn("failed to write to backend!");
        return false;
    }
    fprintf(stderr, "done writing: %d\n", fdbufs[idx].inlen);
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


    for (int i = 0; i < MAX_FDS; i++)
        fds[i].fd = -1;

    int backsock;

#ifdef DEBUG
    backsock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (backsock == -1) {
        warn("failed to create backend socket:");
        goto error;
    }

    if (connect(backsock, (struct sockaddr *) &backaddr, sizeof(backaddr)) == -1) {
        warn("failed to connect to backend:");
        goto error;
    }
#else
    backsock = open("/dev/ttyUSB2", O_RDWR | O_NOCTTY);
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
                if (i == calld)
                    calld = -1;
            } else if (fds[i].revents & POLLIN) {
                if (fdbuf_read(fds[i].fd, i) == -1) {
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
                if (fdbuf_write(fds[i].fd, i) == -1) {
                    warn("failed to write to fd %d:", i);
                    break;
                }

                if (fdbufs[i].inlen == 0)
                    POLLDROP(fds[i], POLLOUT);
            }
        }

        if (fds[BACKEND].revents & POLLIN) {
            fprintf(stderr, "len: %d\n", fdbufs[BACKEND].outlen);
            ret = fdbuf_read(fds[BACKEND].fd, BACKEND);
            if (ret == -1) {
                warn("failed to read from backend:");
                break;
            }

            /* TODO should this be here? */
            if (check_call_status)
                POLLADD(fds[calld], POLLOUT);

            ret = handle_resp(fds[cmd.index].fd, BACKEND);
            memmove(fdbufs[BACKEND].out, fdbufs[BACKEND].out + ret, BUFSIZE - ret);
            fdbufs[BACKEND].outlen -= ret;
            fdbufs[BACKEND].outptr -= ret;
        }

        /* note that this doesn't take effect until the next poll cycle...
         * maybe this can be replaced with something more integrated? */
        if ((cmdq.count || check_call_status) && !active_command)
            POLLADD(fds[BACKEND], POLLOUT);
        else
            POLLDROP(fds[BACKEND], POLLOUT);

        /* send next command to modem */
        if (fds[BACKEND].revents & POLLOUT) {
            fprintf(stderr, "have a command!\n");

            if (check_call_status) {
                if (!send_command(fds[BACKEND].fd, BACKEND, CLCC, (union atdata){0}))
                    break;
                handling_urc = true;
            } else {
                cmd = command_dequeue();
                fprintf(stderr, "op: %d\n", cmd.op);
                assert(cmd.op != CMD_NONE);

                if (!send_command(fds[BACKEND].fd, BACKEND, cmddata[cmd.op].atcmd, cmd.data))
                    break;
            }

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
    }

error:
    for (int i = STDERR+1; i < MAX_FDS; i++) {
        if (fds[i].fd > 0)
            close(fds[i].fd);
    }
    unlink(ATD_SOCKET);
}
