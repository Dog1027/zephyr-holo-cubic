#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <errno.h>
#include "net_server.h"

int net_server_listen(uint16_t port)
{
    struct net_sockaddr_in addr = {
        .sin_family = NET_AF_INET,
        .sin_port = net_htons(port),
        .sin_addr = {.s_addr = NET_INADDR_ANY},
    };
    int one = 1;
    int fd = zsock_socket(NET_AF_INET, NET_SOCK_STREAM, NET_IPPROTO_TCP);

    if (fd < 0) {
        return -errno;
    }
    // 關掉 app 再打開時，舊連線可能還在 TIME_WAIT
    zsock_setsockopt(fd, ZSOCK_SOL_SOCKET, ZSOCK_SO_REUSEADDR, &one, sizeof(one));
    if (zsock_bind(fd, (struct net_sockaddr *)&addr, sizeof(addr)) < 0 || zsock_listen(fd, 1) < 0) {
        int ret = -errno;
        zsock_close(fd);
        return ret;
    }
    return fd;
}

static int wait_readable(int fd, int timeout_ms)
{
    struct zsock_pollfd pfd = {.fd = fd, .events = ZSOCK_POLLIN};
    int ret = zsock_poll(&pfd, 1, timeout_ms);

    if (ret < 0) {
        return -errno;
    }
    return ret == 0 ? -EAGAIN : 0;
}

int net_server_accept(int listen_fd)
{
    int ret = wait_readable(listen_fd, 0);

    if (ret < 0) {
        return ret;
    }
    ret = zsock_accept(listen_fd, NULL, NULL);
    return ret < 0 ? -errno : ret;
}

ssize_t net_server_recv(int fd, void *buf, size_t len, int timeout_ms)
{
    int ret = wait_readable(fd, timeout_ms);

    if (ret < 0) {
        return ret;
    }
    ssize_t n = zsock_recv(fd, buf, len, ZSOCK_MSG_DONTWAIT);
    if (n < 0) {
        return errno == EAGAIN ? -EAGAIN : -errno;
    }
    return n;
}

ssize_t net_server_send(int fd, const void *buf, size_t len)
{
    ssize_t n = zsock_send(fd, buf, len, ZSOCK_MSG_DONTWAIT);

    if (n < 0) {
        return errno == EAGAIN ? 0 : -errno;
    }
    return n;
}
