#define LOG_TAG "server"

#include <ctype.h>
#include <cutils/log.h>
#include <cutils/properties.h>
#include <cutils/sockets.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/rtc.h>
#include <mtd/mtd-user.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define REF_DEBUG

#ifdef REF_DEBUG
#define REF_LOGD(x...) ALOGD( x )
#define REF_LOGE(x...) ALOGE( x )
#else
#define REF_LOGD(x...)
#define REF_LOGE(x...)
#endif


#define TIME_SERVER_SOCK_NAME "cp_time_sync_server"
#define TIME_SYNC_CLIENT_NUM 2

struct refnotify_cmd {
	int cmd_type;
	uint32_t length;
};

struct time_sync {
	uint32_t sys_cnt;
	uint32_t uptime;
}__attribute__ ((__packed__));

static int time_sync_srv = -1;
static int socks[2] = {-1 , -1};

static pthread_t g_timetid;
static bool g_thread_started = false;

static void usage(void)
{
	fprintf(stderr,
	"\n"
	"Usage: refnotify [-t type] [-h]\n"
	"receive and do the notify from modem side \n"
	"  -t type     td type:0, wcdma type: other \n"
	"  -h           : show this help message\n\n");
}

static int set_nonblock(int fd)
{
	long flags = fcntl(fd, F_GETFL);
	flags |= O_NONBLOCK;
	return fcntl(fd, F_SETFL, flags);
}

static void* timesync_task(void *arg)
{
	struct pollfd fds[TIME_SYNC_CLIENT_NUM + 2];
	struct time_sync time_info = *(struct time_sync*)arg;
	struct time_sync time_info_rev;
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
			fds[i].fd = time_sync_srv;
			fds[i].events = POLLIN;
		} else if (1 == i) {
			fds[i].fd = socks[1];
			fds[i].events = POLLIN;
		} else {
			fds[i].fd = -1;
			fds[i].events = POLLRDHUP;
		}
	}

	REF_LOGD("Enter timesync_task.");

	while (1) {
		ret = poll(fds, fd_num, -1);

		if (0 >= ret) {
			REF_LOGE("poll failed, errno = %d.", errno);
			continue;
		}

		for (i = 2; i < fd_num; ++i) {
			if (POLLRDHUP & fds[i].revents) {
				close(fds[i].fd);
				fds[i].fd = -1;
				++num_closed;
				REF_LOGD("A time sync peer client is closed.\n");
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
				REF_LOGE("read error n = %d, errno: %d.", n, errno);
			} else {
				time_info = time_info_rev;
				for (i = 2; i < fd_num; ++i) {
					n = write(fds[i].fd, &time_info, sizeof(struct time_sync));
					if (n != sizeof(struct time_sync)) {
						REF_LOGE("write error n = %d, errno: %d.", n, errno);
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
						REF_LOGE("Set sync client O_NONBLOCK fail.");
						close(sock_client);
					} else {
						++num_client;
						++fd_num;

						fds[num_client + 1].fd = sock_client;
						REF_LOGD("A time sync client is connected.");

						n = write(sock_client, &time_info, sizeof(struct time_sync));
						if (n != sizeof(struct time_sync)) {
							REF_LOGE("write error n = %d, errno: %d.", n, errno);
						}
					}
				} else {
					close(sock_client);
					REF_LOGE("Too many time sync clients.");
				}
			} else {
				REF_LOGE("time sync client accept fail, errno: %d.", errno);
			}
		}
	}

	close(socks[0]);
	for (i = 0; i < fd_num; ++i) {
		close(fds[i].fd);
	}

	return NULL;
}

static void RefNotify_DoTimesync(struct refnotify_cmd *pcmd)
{
	struct sysinfo info;
	static struct time_sync s_initial_sync;
	struct time_sync time_info;
	pthread_attr_t attr;
	ssize_t n;
	int ret;

	if (sysinfo(&info)) {
		REF_LOGE("get sysinfo failed.");
	}

	time_info.sys_cnt = *(uint32_t*)(pcmd+1);
	time_info.uptime = (uint32_t)info.uptime;

	REF_LOGD("AP up time sys_cnt %u", time_info.sys_cnt);

	if (time_sync_srv < 0) {
		// Initialize time server socket
		time_sync_srv = socket_local_server(TIME_SERVER_SOCK_NAME,
						    ANDROID_SOCKET_NAMESPACE_ABSTRACT,
						    SOCK_STREAM);
		if (-1 == time_sync_srv) {
			REF_LOGE("create time server socket error: %d.", errno);
			return;
		}

		if (set_nonblock(time_sync_srv) < 0) {
			close(time_sync_srv);
			time_sync_srv = -1;
			REF_LOGE("time_sync_srv set_nonblock error: %d.", errno);
			return;
		}
	}

        if ((-1 == socks[0]) || (-1 == socks[1])) {
		if (-1 == socketpair(AF_LOCAL, SOCK_STREAM, 0, socks)) {
			REF_LOGE("create socket pair error: %d.", errno);
			return;
		}

		// Put the sockets to non-blocking mode
		if ((set_nonblock(socks[0]) < 0) || (set_nonblock(socks[1]) < 0)) {
			close(socks[0]);
			close(socks[1]);
			close(time_sync_srv);
			socks[0] = -1;
			socks[1] = -1;
			time_sync_srv = -1;
			REF_LOGE("set socks nonblock error: %d.", errno);
			return;
		}
	}

	// inform time sync task to update cp sync time
	if (!g_thread_started) {
		s_initial_sync = time_info;

		ret = pthread_attr_init(&attr);
		if (ret) {
			REF_LOGE("Thread attribute init fail, return errno: %d.", ret);
			return;
		}

		ret = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		if (ret) {
			REF_LOGE("Set thread detach failed, errno: %d.", ret);
		} else if (ret = pthread_create(&g_timetid, &attr, timesync_task,
						&s_initial_sync)) {
			REF_LOGE("timesync_task creation failed, return errno: %d.", ret);
		} else {
			g_thread_started = true;
		}

		ret = pthread_attr_destroy(&attr);
		if (ret) {
			REF_LOGE("Thread attribute destroy fail, return errno: %d.", ret);
		}
	} else {
		n = write(socks[0], &time_info, sizeof(struct time_sync));
		if (n != sizeof(struct time_sync)) {
			REF_LOGE("write error n = %d, errno: %d.", n, errno);
		}
	}
}

int main(int argc, char *argv[]) {
  RefNotify_DoTimesync(struct refnotify_cmd *pcmd);
}

