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
#include "pdu.h"
#include "util.h"
#include "queue.h"

#define AT_MAX 256
#define ATD_SOCKET "/tmp/atd-socket"

#define POLLADD(fd, arg) fd.events |= (arg)
#define POLLDROP(fd, arg) fd.events &= ~(arg)

#define LENGTH(x) (sizeof(x) / sizeof(x[0]))

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

int nextline();

int linelen;

char *startup[] = { "AT+CLIP=1\r", "AT+COLP=1\r", "AT+CNMI=2,2,0,1,0\r", NULL };
char **curstartup = startup;

struct command_args cmddata[] = {
    [CMD_DIAL] = { ATD, { TYPE_STRING, TYPE_NONE } },
    [CMD_ANSWER] = { ATA, { TYPE_NONE } },
    [CMD_HANGUP] = { ATH, { TYPE_NONE } },
    [CMD_SUBMIT] = { ATCMGS, { TYPE_NONE } },
};

char *atcmds[] = {
    [ATD] = "ATD%s;\r",
    [ATA] = "ATA\r",
    [ATH] = "ATH\r",
    [CLCC] = "AT+CLCC\r",
    [ATCMGS] = "AT+CMGS=%d\r", // requires extra PDU data to be sent
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

struct command cmd;
int cmd_progress;
bool active_command = false;
enum atcmd currentatcmd;
int calld = -1, smsd = -1;

struct fdbuf fdbufs[MAX_FDS];
struct pollfd fds[MAX_FDS];

struct call calls[MAX_CALLS];

/* add one command to queue, returns the number of bytes intepreted if the
 * command was validated and added successfully, -1 if the queue is full or we
 * run out of memory, and -2 if the command is invalid but terminated */
ssize_t cmdadd(int index) {
    struct command cmd = {index, CMD_NONE};
    char *ptr = fdbufs[index].out;
    size_t count = 0;
    char *num, *msg, *raw;

    if (cmdq.count == QUEUE_SIZE)
        return -1;

    cmd.op = *(ptr++);
    switch (cmd.op) {
    case CMD_DIAL:
        count = dec_str(ptr, &cmd.data.dial.num);
        if (count == -1)
            return -1;

        fprintf(stderr, "received dial with number %s\n", cmd.data.dial.num);
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
    case CMD_SMS_EVENTS:
        fprintf(stderr, "received request sms events\n");
        if (smsd == -1)
            smsd = index;
        goto end;
        break;
    case CMD_SUBMIT:
        count = dec_str(ptr, &num);
        if (count == -1)
            return -1;

        ptr += count;
        count = dec_str(ptr, &msg);
        if (count == -1)
            return -1;

        count = encode_pdu(NULL, num, msg);
        raw = malloc(count);
        if (!raw)
            return -1;

        encode_pdu(raw, num, msg);
        free(num);
        free(msg);

        cmd.data.submit.len = count;
        cmd.data.submit.pdu = malloc(2*count + 1);
        if (!cmd.data.submit.pdu)
            return -1;

        for (int i = 0; i < count; i++) {
            htoa(&cmd.data.submit.pdu[2*i], raw[i]);
        }
        cmd.data.submit.pdu[2*count] = 0;
        free(raw);

        fprintf(stderr, "received submit to number %s: %s\n", num, cmd.data.submit.pdu);
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

void
lprint(char *buf, size_t len)
{
    for (int i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            fputs("<LF>", stderr);
        } else if (buf[i] == '\r') {
            fputs("<CR>", stderr);
        } else {
            putc(buf[i], stderr);
        }
    }
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
process_cmt(char *start, size_t len)
{
    char pdubuf[1024];
    struct pdu_msg pdu_msg;
    unsigned int pdulen;
    int ret = sscanf(start, "+CMT: ,%u", &pdulen);
    if (ret != 1)
        return -1;

    ret = nextline();
    if (ret == -1)
        return -1;
    fprintf(stderr, "nextline len: %d\n", linelen - 2);

    memcpy(pdubuf, fdbufs[BACKEND].out, linelen - 2);
    pdubuf[linelen - 2] = 0;
    decode_pdu(&pdu_msg, pdubuf);

	if (smsd > 0)
	    return atd_status_delivered(fds[smsd].fd, pdu_msg.d.d.sender.number, pdu_msg.d.d.msg.data);
}

static int
memcspn(const char *mem, const char *invalid, int n)
{
    for (int i = 0; i < n; i++) {
        if (strchr(invalid, mem[i]))
            return i;
    }

    return -1;
}

static int
memspn(const char *mem, const char *valid, int n)
{
    for (int i = 0; i < n; i++) {
        if (!strchr(valid, mem[i]))
            return i;
    }

    return n;
}

int
atcmgs2()
{
    char *loc;
    int ret;
    size_t before, after;

    /* this shouldn't be possible */
    if (!(loc = memchr(fdbufs[BACKEND].out, '>', fdbufs[BACKEND].outlen))) {
        fprintf(stderr, "%s: prompt not found\n", __func__);
        // TODO send \x1a
        return -1;
    }

    before = loc - fdbufs[BACKEND].out;
    after = fdbufs[BACKEND].out + fdbufs[BACKEND].outlen - loc;

    ret = snprintf(fdbufs[BACKEND].in, BUFSIZE, "%s\x1a", cmd.data.submit.pdu);

    fdbufs[BACKEND].inptr = fdbufs[BACKEND].in;
    if (ret > BUFSIZE) {
       fdbufs[BACKEND].in[0] = '\x1a'; // \x1a will terminate read for a PDU
       fdbufs[BACKEND].inlen = 1;
       fprintf(stderr, "%s: PDU too long!\n", __func__);
    } else {
        fdbufs[BACKEND].inlen = ret;
    }

    ret = fdbuf_write(BACKEND);

    // the prompt will be "> ", so remove the prompt from the buffer
    memmove(fdbufs[BACKEND].out + before, loc + 2, after);
    fdbufs[BACKEND].outlen -= 2;
    fdbufs[BACKEND].outptr -= 2;

    free(cmd.data.submit.pdu);
    cmd.data.submit.pdu = NULL;

    return ret;
}

int
nextline()
{
    fprintf(stderr, "%s start: linelen = %d\n", __func__, linelen);
    char *start = fdbufs[BACKEND].out;
    int total = 0;

    do {
        total += linelen;
        memmove(start, fdbufs[BACKEND].out + linelen, fdbufs[BACKEND].outlen - linelen);
        fdbufs[BACKEND].outlen -= linelen;
        fdbufs[BACKEND].outptr -= linelen;
        linelen = memcspn(start, "\r\n", fdbufs[BACKEND].outlen);
        if (linelen == -1) {
            linelen = 0;
            return -1; // we didn't find a newline, so there must not be a line to process
        }
        linelen += memspn(start+linelen, "\r\n", fdbufs[BACKEND].outlen - linelen);
    } while (linelen <= 2); // while the line is blank

    return total;
}

size_t
handle_resp(int fd)
{
    fprintf(stderr, "%s start\n", __func__);
    char *start = fdbufs[BACKEND].out;
    enum status status = 0;

    // this must be put before nextline, because a prompt doesn't end
    // in a newline, so nextline won't interpret it as a line
    if (currentatcmd == ATCMGS && cmd.data.submit.pdu) {
        if (atcmgs2() < 0)
            return -1;

        return 2;
    }

    if (nextline() < 0)
        return 0;

    if (strncmp(start, "OK", sizeof("OK") - 1) == 0) {
        status = STATUS_OK;
        active_command = false;
        if (*curstartup)
            curstartup++;

        if (currentatcmd == ATD) {
            if (send_call_status(CALL_DIALING, cmd.data.dial.num) < 0)
                fprintf(stderr, "failed to send call status\n");
        }

        cmd.op = CMD_NONE;
        currentatcmd = ATNONE;
        fprintf(stderr, "got OK\n");
    } else if (strncmp(start, "ERROR", sizeof("ERROR") - 1) == 0) {
        status = STATUS_ERROR;
        active_command = false;
        fprintf(stderr, "got ERROR\n");
    } else if (strncmp(start, "NO CARRIER", sizeof("NO CARRIER") - 1) == 0) {
        if (cmd.op == CMD_ANSWER || cmd.op == CMD_DIAL) {
            active_command = false;
            status = STATUS_ERROR;
            cmd.op = CMD_NONE;
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

        send_clip(start, linelen);
    } else if (strncmp(start, "+COLP", sizeof("+COLP") - 1) == 0) {
        fprintf(stderr, "got +COLP\n");

        send_colp(start, linelen);
    } else if (strncmp(start, "+CMT", sizeof("+CMT") - 1) == 0) {
        fprintf(stderr, "got +CMT\n");

        process_cmt(start, linelen);
    }

    if (status && fd > 0)
        send_status(fd, status);

    fprintf(stderr, "%s: %.*s\n", __func__, linelen, start);
    return linelen;
}

bool
send_startup()
{
    int ret;
    ret = snprintf(fdbufs[BACKEND].in, BUFSIZE, *curstartup);
    if (ret >= BUFSIZE) {
        warn("AT command too long!");
        return false;
    }

    fprintf(stderr, "send startup: %.*s\n", ret, fdbufs[BACKEND].in);
    fdbufs[BACKEND].inptr = fdbufs[BACKEND].in;
    fdbufs[BACKEND].inlen = ret;

    ret = fdbuf_write(BACKEND);
    if (ret == -1) {
        warn("failed to write to backend!");
        return false;
    }

    active_command = true;
    return true;
}

bool
send_command(int idx, enum atcmd atcmd, union atdata atdata)
{
    int ret;
    fprintf(stderr, "send command: %d\n", atcmd);
    if (atcmd == ATD) {
        ret = snprintf(fdbufs[idx].in, BUFSIZE, atcmds[atcmd], atdata.dial.num);
        free(atdata.dial.num);
    } else if (atcmd == ATCMGS) {
        ret = snprintf(fdbufs[idx].in, BUFSIZE, atcmds[atcmd], atdata.submit.len);
    } else {
        ret = snprintf(fdbufs[idx].in, BUFSIZE, atcmds[atcmd]);
    }
    if (ret >= BUFSIZE) {
        warn("AT command too long!");
        return false;
    }
    fprintf(stderr, "send command: %.*s\n", ret, fdbufs[idx].in);
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
    ssize_t ret = 0;
    sigset_t mask;

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
    struct sockaddr_un backaddr = {
        .sun_family = AF_UNIX,
        .sun_path = "/tmp/atsim",
    };

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

    if (!setup_modem_tty(backsock)) {
        warn("failed to configure tty:");
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
    fds[BACKEND].events = POLLIN | POLLOUT;
    fdbufs[BACKEND].outptr = fdbufs[BACKEND].out;
    fds[SIGNALINT].fd = sigintfd;
    fds[SIGNALINT].events = POLLIN;

    while (true) {
        if (poll(fds, LENGTH(fds), -1) == -1) {
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
                if (i == smsd)
                    smsd = -1;
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

        while ((ret = handle_resp(fds[cmd.index].fd)) != 0) {
            if (ret < 0) {
                fprintf(stderr, "atd: failure in atcmgs\n");
                goto error;
            }
        }

        /* send next command to modem */
        if (fds[BACKEND].revents & POLLOUT) {
            if (*curstartup) {
                if (!send_startup()) {
                    fprintf(stderr, "failed to send startup command!\n");
                    break;
                }
            } else {
                fprintf(stderr, "have a command!\n");

                cmd = command_dequeue();
                assert(cmd.op != CMD_NONE);

                if (!send_command(BACKEND, cmddata[cmd.op].atcmd, cmd.data))
                    break;

                /* don't write any more until we hear back */
                if (fdbufs[BACKEND].inlen == 0) {
                    POLLDROP(fds[BACKEND], POLLOUT);
                }
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
        if ((cmdq.count || *curstartup) && !active_command)
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
