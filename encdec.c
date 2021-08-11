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

    *out = malloc(len+1);
    if (!(*out))
        return -1;

    memcpy(*out, ptr, len);
    out[len] = 0;
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
    char *ptr = buf;
    ssize_t ret;
    while (len) {
        ret = write(fd, ptr, len);
        if (ret == -1)
            return -1;

        len -= ret;
        ptr += ret;
    }
}

int
xread(int fd, char *buf, size_t len)
{
    char *ptr = buf;
    ssize_t ret;
    while (len) {
        ret = read(fd, ptr, len);
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
atd_cmd_sms_events(int fd)
{
    char buf = CMD_SMS_EVENTS;
    return xwrite(fd, &buf, 1);
}

int
atd_cmd_submit(int fd, char *num, char *msg)
{
    size_t len = strlen(num) + strlen(msg) + 5; // 5 = op + length + length
    char buf[len];
    buf[0] = CMD_SUBMIT;
    enc_str(buf + 1, num);
    enc_str(buf + 3 + strlen(num), msg);

    return xwrite(fd, buf, len);
}

int
atd_status_delivered(int fd, char *num, char *msg)
{
	if (strlen(num) > PHONE_NUMBER_MAX_LEN)
		return -1;

    size_t len = strlen(num) + strlen(msg) + 5; // 5 = op + length + length
    char buf[len];
    buf[0] = STATUS_DELIVERED;

    enc_str(buf + 1, num);
    enc_str(buf + 3 + strlen(num), msg);

    return xwrite(fd, buf, len);
}

int
atd_status_call(int fd, enum callstatus status, char *num)
{
    char buf[4 + strlen(num)];
    char *ptr;
    ssize_t ret;

    buf[0] = STATUS_CALL;
    buf[1] = status;

    ptr = buf + 2;
    ptr += enc_str(ptr, num);

    return xwrite(fd, buf, ptr - buf);
}

/* calls should be MAX_CALLS long */
int
dec_call_status(int fd, struct call *call)
{
    char status = 0;
    unsigned short len = 0;
    ssize_t ret;
    char buf[PHONE_NUMBER_MAX_LEN];

    ret = xread(fd, (char *) &call->status, 1);
    if (ret == -1 || status >= CALL_LAST)
        return -1;

    ret = xread(fd, buf, 2);
    if (ret == -1)
        return -1;

    len = dec_short(buf);
    if (len > PHONE_NUMBER_MAX_LEN)
        return -1;

    if (len == 0)
    	return 0;

    ret = xread(fd, call->num, len);
    if (ret == -1)
        return -1;

    return 0;
}

int
dec_sms_status(int fd, struct sms *sms)
{
    char status = 0;
    unsigned short len = 0;
    ssize_t ret;
    char buf[PHONE_NUMBER_MAX_LEN];

    ret = xread(fd, buf, 2);
    if (ret == -1)
        return -1;

    len = dec_short(buf);
    if (len > PHONE_NUMBER_MAX_LEN || len == 0)
        return -1;

    ret = xread(fd, sms->num, len);
    if (ret == -1)
        return -1;

    ret = xread(fd, buf, 2);
    if (ret == -1)
        return -1;

    len = dec_short(buf);
    if (len == 0)
        return 0; // XXX should we accept empty messages?

    sms->msg = calloc(1, len);
    if (sms->msg == NULL)
    	return -1;

    ret = xread(fd, sms->msg, len);
    if (ret == -1)
        return -1;

    return 0;
}
