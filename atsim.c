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
    char fromin[BUFSIZE];
    char *fromoff = fromsock;
    char *tooff = tosock;

    ssize_t tmpcount;
    ssize_t tocount = 0;
    ssize_t fromcount = 0;

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

            write(STDOUT, fromsock, ret);
        }

        int ret;
        if (fds[SOCKFD].revents & POLLOUT) {
            fprintf(stderr, "in SOCKFD POLLOUT: %d\n", tocount);
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
            int ret = read(fds[STDIN].fd, fromin, BUFSIZE);
            if (ret == -1) {
                fprintf(stderr, "failed to read from stdin\n");
                break;
            }

			// remove ending newline
			fromin[ret-1] = '\0';
			ret -= 1;

			// convert escapes and special characters
			int idx = 0;
			for (int i = 0; i < ret; i++) {
				if (fromin[i] == '\\') {
					i++;
					switch (fromin[i]) {
					case 'n':
						tosock[idx] = '\n';
						break;
					case 'r':
						tosock[idx] = '\r';
						break;
					case 'z':
						tosock[idx] = '\x1a';
						break;
					default:
						tosock[idx] = '\\';
						break;
					}
				} else {
					tosock[tocount+idx] = fromin[i];
				}

				idx++;
			}

			tocount += idx;

            for (int i = 0; i < tocount; i++) {
                fprintf(stderr, "%x ", tosock[i]);
            }

            tooff = tosock;
            fds[SOCKFD].events |= POLLOUT;
        }
    }

    close(fds[SOCKFD].fd);
err:
    close(sock);
    unlink(sockaddr.sun_path);
    return 1;
}
