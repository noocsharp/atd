#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "atd.h"

int
sendcode(int fd, int code)
{
    char c = code;
    /* TODO make more robust */
    int ret = write(fd, &c, 1);
    if (ret == -1)
        return -1;
}

int
dial(int fd, char *num)
{
    int ret, len = strlen(num), left;
    char buf[PHONE_NUMBER_MAX_LEN + 3];
    char *ptr;

    if (len > PHONE_NUMBER_MAX_LEN)
        return -1;

    buf[0] = CMD_DIAL;
    buf[1] = len & 0xFF;
    buf[2] = (len >> 8) & 0xFF;

    memcpy(buf + 3, num, len);

    left = len + 3;
    ptr = buf;

    do {
        ret = write(fd, ptr, left);
        if (ret == -1)
            return -1;
        ptr += ret;
        left -= ret;
    } while (left);

    return 0;
}

int
main(int argc, char *argv[])
{
    enum ops cmd;
    struct sockaddr_un addr = {
        .sun_family = AF_UNIX,
        .sun_path = "/tmp/atd-socket"
    };
    if (argc < 2)
        return 1;

    if (strcmp(argv[1], "dial") == 0) {
        if (argc < 3)
            return 1;
        cmd = CMD_DIAL;
    } else if (strcmp(argv[1], "answer") == 0) {
        cmd = CMD_ANSWER;
    } else if (strcmp(argv[1], "hangup") == 0) {
        cmd = CMD_HANGUP;
    }

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == -1) {
        fprintf(stderr, "failed to open socket\n");
        return 1;
    }

    int con = connect(sock, (struct sockaddr *) &addr, sizeof(struct sockaddr_un));
    if (con == -1) {
        fprintf(stderr, "failed to connect\n");
        close(sock);
        return 1;
    }

    fprintf(stderr, "confd: %d\n", sock);

    switch (cmd) {
    case CMD_DIAL:
        dial(sock, argv[2]);
        break;
    default:
        sendcode(sock, cmd);
        break;
    }

	char buf;
	read(sock, &buf, 1);

	if (buf == STATUS_OK)
		fprintf(stderr, "OK\n");

    sleep(1);

    close(con);
    close(sock);

    return 0;
}
