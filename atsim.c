#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>
#include <stdbool.h>

#define STDOUT 0
#define STDIN  1
#define SOCKFD 2
#define FDCOUNT 3

int main() {
    struct sockaddr_un sockaddr = {
        .sun_family = AF_UNIX,
        .sun_path = "/tmp/atsim",
    };
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    struct pollfd fds[FDCOUNT];

    char tosock[1024];
    char fromsock[1024];
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

    while (true) {
        if (poll(fds, 2, -1) == -1) {
            fprintf(stderr, "poll failed");
        }

        /* can read from socket */
        if (fds[SOCKFD].revents & POLLIN) {
            int ret = read(fds[SOCKFD].fd, fromsock, 1024);
            if (ret == -1) {
                fprintf(stderr, "failed to read from socket\n");
                return 1;
            }
            fromcount = ret;
            fromoff = fromsock;
            fds[SOCKFD].events &= ~POLLIN;
            fds[STDOUT].events |= POLLOUT;
        }

        if (fds[STDOUT].events & POLLOUT) {
            int ret = write(fds[STDOUT].fd, fromoff, fromcount);
            if (ret == -1) {
                fprintf(stderr, "failed to write to stdout\n");
                return 1;
            }
            fromcount -= ret;
            fromoff += ret;

            /* we are done writing to stdout, so we can resume to reading */
            if (fromcount == 0) {
                fds[STDOUT].events &= ~POLLOUT;
                fds[SOCKFD].events |= POLLIN;
            }
        }

        if (fds[SOCKFD].events & POLLOUT) {
            int ret = write(fds[SOCKFD].fd, tooff, tocount);
            if (ret == -1) {
                fprintf(stderr, "failed to write to stdout\n");
                return 1;
            }
            tocount -= ret;
            tooff += ret;

            /* we are done writing to stdout, so we can resume reading */
            if (fromcount == 0) {
                fds[SOCKFD].events &= ~POLLOUT;
                fds[STDIN].events |= POLLIN;
            }
        }

        if (fds[STDIN].revents & POLLIN) {
            int ret = read(fds[STDIN].fd, tosock, 1024);
            if (ret == -1) {
                fprintf(stderr, "failed to read from stdin\n");
                return 1;
            }
            tocount = ret;
            tooff = fromsock;
            fds[STDIN].events &= ~POLLIN;
            fds[SOCKFD].events |= POLLOUT;
        }
    }

    close(fds[SOCKFD].fd);
err:
    close(sock);
    return 1;
}
