#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "atd.h"

int
sendcode(int fd, int code)
{
    int ret, left = 2;
    char buf[2];
    char *ptr = buf;
    buf[0] = code;
    buf[1] = '\0';

    while (left > 0) {
        ret = write(fd, ptr, left);
        if (ret == -1)
            return -1;

        ptr += ret;
        left -= ret;
    }

    return 0;
}

int
dial(int fd, char *num)
{
    char dialcode = CMD_DIAL;
    int ret, left = strlen(num);
    do {
        ret = write(fd, &dialcode, 1);
        if (ret == -1)
            return -1;
        if (ret == 1)
            break;
    } while (ret);

    do {
        ret = write(fd, num, left);
        if (ret == -1)
            return -1;
        num += ret;
        left -= ret;
    } while (left);

    do {
        ret = write(fd, &"\0", 1);
        if (ret == -1)
            return -1;
        if (ret == 1)
            break;
    } while (ret);
    

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

    sleep(1);

    close(con);
    close(sock);

    return 0;
}
