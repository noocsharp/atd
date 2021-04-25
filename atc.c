#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "atd.h"

int
call(int fd, char *num)
{
    char callcode = CMD_DIAL;
    int ret, left = strlen(num);
    do {
        ret = write(fd, &callcode, 1);
        if (ret == -1)
            return -1;
    } while (ret);

    do {
        ret = write(fd, num, left);
        if (ret == -1)
            return -1;
        num += ret;
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

    if (strcmp(argv[1], "call") == 0) {
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

    close(con);
    close(sock);

    return 0;
}
