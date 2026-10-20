#ifndef LINKR_DEBUGGER_SERIAL_RUNTIME_STUB_SOCKET_H_
#define LINKR_DEBUGGER_SERIAL_RUNTIME_STUB_SOCKET_H_

#include <netinet/in.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/types.h>

#define ZSOCK_POLLIN 0x01
#define ZSOCK_POLLOUT 0x04
#define ZSOCK_POLLERR 0x08
#define ZSOCK_POLLHUP 0x10
#define ZSOCK_POLLNVAL 0x20
#define ZSOCK_MSG_DONTWAIT MSG_DONTWAIT
#define ZSOCK_SHUT_RDWR SHUT_RDWR
#define ZVFS_F_SETFL 1
#define ZVFS_O_NONBLOCK 1

struct zsock_pollfd {
	int fd;
	short events;
	short revents;
};

int zsock_socket(int family, int type, int protocol);
int zsock_setsockopt(int fd, int level, int option, const void *value,
		     socklen_t length);
int zsock_fcntl(int fd, int command, int flags);
int zsock_bind(int fd, const struct sockaddr *address, socklen_t length);
int zsock_listen(int fd, int backlog);
int zsock_accept(int fd, struct sockaddr *address, socklen_t *length);
int zsock_poll(struct zsock_pollfd *fds, int count, int timeout);
ssize_t zsock_recv(int fd, void *buffer, size_t length, int flags);
ssize_t zsock_send(int fd, const void *buffer, size_t length, int flags);
int zsock_shutdown(int fd, int how);
int zsock_close(int fd);

#endif
