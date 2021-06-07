#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "atd.h"
#include "encdec.h"

static unsigned short
dec_short(char *in)
{
    return in[0] + (in[1] << 8);
}

static unsigned short
enc_short(char *buf, unsigned short num)
{
    buf[0] = num;
    buf[1] = num >> 8;
}

ssize_t
dec_str(char *in, char **out)
{
    unsigned short len;
    char *ptr = in;
    len = dec_short(in);
    ptr += 2;

    *out = malloc(len);
    if (!(*out))
        return -1;

    memcpy(*out, ptr, len);
    return len + 2;
}

static ssize_t
enc_str(char *buf, char *str)
{
    size_t len = strlen(str);
    enc_short(buf, len);
    buf += 2;
    strcpy(buf, str);

    return len + 2;
}

int
xwrite(int fd, char *buf, size_t len)
{
    char *ptr;
    ssize_t ret;
    while (len) {
        ret = write(fd, buf, len);
        if (ret == -1)
            return -1;

        len -= ret;
        ptr += ret;
    }
}

int
atd_cmd_dial(int fd, char *num)
{
    size_t len = strlen(num) + 3; // 3 = op + length
    char buf[len];
    buf[0] = CMD_DIAL;
    enc_str(buf + 1, num);

    return xwrite(fd, buf, len);
}

int
atd_cmd_hangup(int fd)
{
    char buf = CMD_HANGUP;
    return xwrite(fd, &buf, 1);
}

int
atd_cmd_answer(int fd)
{
    char buf = CMD_ANSWER;
    return xwrite(fd, &buf, 1);
}

int
atd_cmd_call_events(int fd)
{
    char buf = CMD_CALL_EVENTS;
    return xwrite(fd, &buf, 1);
}

int
atd_status_call(int fd, struct call *calls, size_t len)
{
    char buf[(PHONE_NUMBER_MAX_LEN + 2) * MAX_CALLS + 2];
    char *ptr;
    ssize_t ret;

    buf[0] = STATUS_CALL;
    buf[1] = 0;

    ptr = buf + 2;

    for (int i = 0; i < len; i++) {
        if (!(calls[i].present))
            continue;

        ptr += enc_str(ptr, calls[i].num);

        buf[1]++;
    }

    return xwrite(fd, buf, ptr - buf);
}
