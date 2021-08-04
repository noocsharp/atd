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

    bool anotherline = false;
    char tmp[BUFSIZE];
    char tosock[BUFSIZE];
    char fromsock[BUFSIZE];
    char *fromoff = fromsock;
    char *tooff = tosock;

    ssize_t tmpcount;
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
                break;
            }

            if (fromsock[ret-1] == '\x1a') {
                fromsock[ret-1] = '\n';
                char *place = memchr(fromsock, '\r', ret-2);
                if (*place)
	                *place = '\n';
            } 

            write(STDOUT, fromsock, ret);
        }

        int ret;
        if (fds[SOCKFD].revents & POLLOUT) {
            fprintf(stderr, "in SOCKFD POLLOUT: %d\n", tocount);
            if (tmpcount) {
            	fprintf(stderr, "tmpcount!\n");
	            for (int i = 0; i < tocount; i++) {
	                fprintf(stderr, "%x ", tmp[i]);
	            }
                ret = write(fds[SOCKFD].fd, tmp, tmpcount);
	            if (ret == -1) {
	                fprintf(stderr, "failed to write to socket\n");
	                break;
	            }
	            tmpcount -= ret;
            }

            if (tmpcount)
                continue;

            ret = write(fds[SOCKFD].fd, tosock, tocount);
            if (ret == -1) {
                fprintf(stderr, "failed to write to socket\n");
                break;
            }
            tocount -= ret;
            memmove(tosock, tosock + ret, BUFSIZE - ret);
            fprintf(stderr, "tocount: %d\n", tocount);
            if (tocount == 0) {
                fds[SOCKFD].events &= ~POLLOUT;
                fds[SOCKFD].events |= POLLIN;
            }
        }

        if (fds[STDIN].revents & POLLIN) {
            fprintf(stderr, "in STDIN POLLIN\n");
            int ret = read(fds[STDIN].fd, tosock, BUFSIZE);
            if (ret == -1) {
                fprintf(stderr, "failed to read from stdin\n");
                break;
            }

            if (tosock[ret-1] == '\n') {
                tosock[ret-1] = '\r';
                tosock[ret] = '\n';
                tosock[ret+1] = '\0';
                tocount = ret + 1;
            } 

            if (memcmp(tosock, "+CMT:", sizeof("+CMT:") - 1) == 0) {
                memcpy(tmp, tosock, ret);
                tmpcount = ret;
                anotherline = 1;
                continue;
            }

            if (anotherline)
                anotherline = 0;

            for (int i = 0; i < tocount; i++) {
                fprintf(stderr, "%x ", tosock[i]);
            }
            tooff = fromsock;
            fds[SOCKFD].events |= POLLOUT;
        }
    }

    close(fds[SOCKFD].fd);
err:
    close(sock);
    unlink(sockaddr.sun_path);
    return 1;
}
