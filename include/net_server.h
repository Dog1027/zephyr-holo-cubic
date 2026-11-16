#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/*
 * Small non-blocking TCP server helpers for apps that serve one client at a time from run()
 * (no extra thread: DRAM has no room for another stack). Every call returns right away, or
 * after at most timeout_ms. Close the sockets with zsock_close().
 */

// Listening socket on port (any address), -errno on failure
int net_server_listen(uint16_t port);

// A waiting client's socket, -EAGAIN when there is none
int net_server_accept(int listen_fd);

// Wait up to timeout_ms for data. Bytes read, 0 when the peer closed, -EAGAIN on timeout
ssize_t net_server_recv(int fd, void *buf, size_t len, int timeout_ms);

// Send what fits in the socket buffer now. Bytes sent (may be 0), -errno on failure
ssize_t net_server_send(int fd, const void *buf, size_t len);
