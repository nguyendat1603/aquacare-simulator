#ifndef TCP_STREAM_H
#define TCP_STREAM_H

#include <stdint.h>

#define TCP_STREAM_MAX_MESSAGE_SIZE (1024U * 1024U)

/*
 * Sends one message as a 4-byte network-order length followed by its payload.
 * Returns 0 on success and -1 on error (errno is set).
 */
int tcp_send_message(int socket_fd, const void *data, uint32_t length);

/*
 * Receives one complete message.
 * Returns 1 on success, 0 if the peer closed cleanly between messages, or
 * -1 on error (errno is set). The caller owns *data and must free it.
 * For an empty message, *data is NULL and *length is 0.
 */
int tcp_receive_message(int socket_fd, void **data, uint32_t *length);

#endif
