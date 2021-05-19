#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>
#include <stdbool.h>

#define STDOUT 0
#define STDIN  1
#define SOCKFD 2
#define FDCOUNT 3

#define BUFSIZE 1024

int main() {
    struct sockaddr_un sockaddr = {
        .sun_family = AF_UNIX,
        .sun_path = "/tmp/atsim",
    };
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    struct pollfd fds[FDCOUNT];

    char tosock[BUFSIZE];
    char fromsock[BUFSIZE];
    char *fromoff = fromsock;
    char *tooff = tosock;

    ssize_t tocount;
    ssize_t fromcount;

    if (sock == -1) {
        fprintf(stderr, "failed to create socket\n");
    }

    if (bind(sock, (struct sockaddr *) &sockaddr, sizeof(struct sockaddr_un)) == -1) {
        fprintf(stderr, "failed to bind to socket\n");
        goto err;
    }

    if (listen(sock, 1)  != 0) {
        fprintf(stderr, "failed to listen on socket\n");
        goto err;
    }

    fds[SOCKFD].fd = accept(sock, NULL, NULL);
    if (fds[SOCKFD].fd == -1) {
        fprintf(stderr, "failed to accept connection\n");
        goto err;
    }

    fds[SOCKFD].events = POLLIN;
    fds[STDIN].fd = 1;
    fds[STDIN].events = POLLIN;
    fds[STDOUT].fd = 0;
    fds[STDOUT].events = 0;

    fprintf(stderr, "at loop\n");
    while (true) {
        if (poll(fds, FDCOUNT, -1) == -1) {
            fprintf(stderr, "poll failed");
        }

        if (fds[SOCKFD].revents & POLLHUP)
            break;

        if (fds[SOCKFD].revents & POLLIN) {
            int ret = read(fds[SOCKFD].fd, fromsock, BUFSIZE);
            if (ret == -1) {
                fprintf(stderr, "failed to read from socket\n");
                return 1;
            }
            write(STDOUT, fromsock, ret);
            fds[STDIN].events |= POLLIN;
        }

        if (fds[SOCKFD].revents & POLLOUT) {
            fprintf(stderr, "in SOCKFD POLLOUT: %d\n", tocount);
            int ret = write(fds[SOCKFD].fd, tosock, tocount);
            if (ret == -1) {
                fprintf(stderr, "failed to read from socket\n");
                return 1;
            }
            tocount -= ret;
            memmove(tosock, tosock + ret, BUFSIZE - ret);
            fprintf(stderr, "tocount: %d\n", tocount);
            if (tocount == 0) {
                fds[SOCKFD].events &= ~POLLOUT;
                fds[SOCKFD].events &= POLLIN;
            }
        }

        if (fds[STDIN].revents & POLLIN) {
            fprintf(stderr, "in STDIN POLLIN\n");
            int ret = read(fds[STDIN].fd, tosock, BUFSIZE);
            if (ret == -1) {
                fprintf(stderr, "failed to read from stdin\n");
                return 1;
            }
            tocount = ret;
            tooff = fromsock;
            fds[SOCKFD].events |= POLLOUT;
        }
    }

    close(fds[SOCKFD].fd);
err:
    close(sock);
    return 1;
}
