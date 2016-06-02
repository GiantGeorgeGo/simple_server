#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define SERVER_SOCK_NAME "maison_intelligent_service"
#define MEMBER_NUMBER 2

static int sever_fd = -1;
static int socks[2] = {-1 , -1};

static pthread_t g_timetid;
static bool g_thread_started = false;

static int set_nonblock(int fd) {
	long flags = fcntl(fd, F_GETFL);
	flags |= O_NONBLOCK;
	return fcntl(fd, F_SETFL, flags);
}

static void* work_load(void *arg) {
	struct pollfd fds[TIME_SYNC_CLIENT_NUM + 2];
	int request = *(int*)arg;
	int sock_client = -1;
	int ret = -1;
	int flag = 0;
	int num_client = 0;
	int num_closed = 0;
	int i = 0;
	int j = 0;
	ssize_t n = 0;
	nfds_t fd_num = num_client + 2;

	for (i = 0; i < TIME_SYNC_CLIENT_NUM + 2; ++i) {
		if (0 == i) {
			fds[i].fd = server_fd;
			fds[i].events = POLLIN;
		} else if (1 == i) {
			fds[i].fd = socks[1];
			fds[i].events = POLLIN;
		} else {
			fds[i].fd = -1;
			fds[i].events = POLLRDHUP;
		}
	}

	while (1) {
		ret = poll(fds, fd_num, -1);

		if (0 >= ret) {
			continue;
		}

		for (i = 2; i < fd_num; ++i) {
			if (POLLRDHUP & fds[i].revents) {
				close(fds[i].fd);
				fds[i].fd = -1;
				++num_closed;
			}
		}

		if (num_closed) {
			// Search for the first closed descriptor
			for (i = 2; i < fd_num; ++i) {
				if (fds[i].fd < 0) {
					// This is a closed descriptor
					break;
				}
			}

			for (j = i, ++i; i < fd_num; ++i) {
				if (fds[i].fd >= 0) {
					fds[j].fd = fds[i].fd;
					++j;
				}
			}

			fd_num -= num_closed;
			num_client -= num_closed;
			num_closed = 0;
		}

		if (POLLIN & fds[1].revents) {
			n = read(fds[1].fd, &time_info_rev, sizeof(struct time_sync));
			if (n != sizeof(struct time_sync)) {
				printf("read error n = %d, errno: %d.", n, errno);
			} else {
				time_info = time_info_rev;
				for (i = 2; i < fd_num; ++i) {
					n = write(fds[i].fd, &time_info, sizeof(struct time_sync));
					if (n != sizeof(struct time_sync)) {
						printf("write error n = %d, errno: %d.", n, errno);
					}
				}
			}
		}

		if (POLLIN & fds[0].revents) {
			sock_client = accept(time_sync_srv, 0, 0);
			if (sock_client >= 0) {
				if (num_client < TIME_SYNC_CLIENT_NUM) {
					flag = fcntl(sock_client, F_GETFL);
					flag |= O_NONBLOCK;
					ret = fcntl(sock_client, F_SETFL, flag);
					if (-1 == ret) {
						printf("Set sync client O_NONBLOCK fail.");
						close(sock_client);
					} else {
						++num_client;
						++fd_num;

						fds[num_client + 1].fd = sock_client;
						printf("A time sync client is connected.");

						n = write(sock_client, &time_info, sizeof(struct time_sync));
						if (n != sizeof(struct time_sync)) {
							printf("write error n = %d, errno: %d.", n, errno);
						}
					}
				} else {
					close(sock_client);
					printf("Too many time sync clients.");
				}
			} else {
				printf("time sync client accept fail, errno: %d.", errno);
			}
		}
	}

	close(socks[0]);
	for (i = 0; i < fd_num; ++i) {
		close(fds[i].fd);
	}

	return NULL;
}

static void do_task(int request) {
	pthread_attr_t attr;
	ssize_t n;
	int ret;
	static int s_req_cmd = request;

	if (time_sync_srv < 0) {
		// Initialize time server socket
		server_fd = socket_local_server(TIME_SERVER_SOCK_NAME,
						    ANDROID_SOCKET_NAMESPACE_ABSTRACT,
						    SOCK_STREAM);
		if (-1 == server_fd) {
			return;
		}

		if (set_nonblock(server_fd) < 0) {
			close(server_fd);
			server_fd = -1;
			return;
		}
	}

        if ((-1 == socks[0]) || (-1 == socks[1])) {
		if (-1 == socketpair(AF_LOCAL, SOCK_STREAM, 0, socks)) {
			printf("create socket pair error: %d.", errno);
			return;
		}

		// Put the sockets to non-blocking mode
		if ((set_nonblock(socks[0]) < 0) || (set_nonblock(socks[1]) < 0)) {
			close(socks[0]);
			close(socks[1]);
			close(server_fd);
			socks[0] = -1;
			socks[1] = -1;
			server_fd = -1;
			return;
		}
	}

	// inform time sync task to update cp sync time
	if (!g_thread_started) {
		ret = pthread_attr_init(&attr);
		if (ret) {
			return;
		}

		ret = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		if (ret) {
			printf("Set thread detach failed, errno: %d.", ret);
		} else if (ret = pthread_create(&g_timetid, &attr, work_load,
						&s_req_cmd)) {
			printf("thread creation failed, return errno: %d.", ret);
		} else {
			g_thread_started = true;
		}

		ret = pthread_attr_destroy(&attr);
		if (ret) {
			printf("Thread attribute destroy fail, return errno: %d.", ret);
		}
	} else {
		n = write(socks[0], &s_req_cmd, sizeof(int));
		if (n != sizeof(int)) {
			printf("write error n = %d, errno: %d.", n, errno);
		}
	}
}

int main(int argc, char *argv[]) {
  do_task(0);
}

