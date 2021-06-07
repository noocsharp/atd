#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "atd.h"
#include "encdec.h"

struct call calls[MAX_CALLS];

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
    } else if (strcmp(argv[1], "callevents") == 0) {
        cmd = CMD_CALL_EVENTS;
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
        atd_cmd_dial(sock, argv[2]);
        break;
    case CMD_HANGUP:
        atd_cmd_hangup(sock);
        break;
    case CMD_ANSWER:
        atd_cmd_answer(sock);
        break;
    case CMD_CALL_EVENTS:
        atd_cmd_call_events(sock);
        break;
    }

    char op;

    read(sock, &op, 1);
    if (op == STATUS_CALL) {
        dec_call_status(sock, calls);
    }

    if (op == STATUS_OK)
        fprintf(stderr, "OK\n");
    else if (op == STATUS_OK)
        fprintf(stderr, "ERROR\n");
    else if (op == STATUS_CALL) {
        for (int i = 0; i < MAX_CALLS; i++) {
            if (!calls[i].present)
                continue;

            fprintf(stderr, "status: %s, %d", calls[i].num, calls[i].status);
        }
    }

    sleep(1);

    close(con);
    close(sock);

    return 0;
}
