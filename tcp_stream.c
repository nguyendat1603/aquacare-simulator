#define _POSIX_C_SOURCE 200809L

#include "tcp_stream.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/socket.h>

static int send_all(int socket_fd, const void *data, size_t length)
{
    const unsigned char *bytes = data;
    size_t sent = 0;
    int flags = 0;

#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif

    while (sent < length) {
        ssize_t result = send(socket_fd, bytes + sent, length - sent, flags);

        if (result > 0) {
            sent += (size_t)result;
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else {
            if (result == 0) {
                errno = EPIPE;
            }
            return -1;
        }
    }

    return 0;
}

/*
 * Reads exactly length bytes. A return value of 0 is only possible when
 * allow_clean_eof is set and the peer closes before sending any bytes.
 */
static int receive_all(int socket_fd, void *data, size_t length,
                       int allow_clean_eof)
{
    unsigned char *bytes = data;
    size_t received = 0;

    while (received < length) {
        ssize_t result = recv(socket_fd, bytes + received,
                              length - received, 0);

        if (result > 0) {
            received += (size_t)result;
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else if (result == 0) {
            if (allow_clean_eof && received == 0) {
                return 0;
            }
            errno = ECONNRESET;
            return -1;
        } else {
            return -1;
        }
    }

    return 1;
}

int tcp_send_message(int socket_fd, const void *data, uint32_t length)
{
    uint32_t network_length;

    if (length > TCP_STREAM_MAX_MESSAGE_SIZE) {
        errno = EMSGSIZE;
        return -1;
    }
    if (length > 0 && data == NULL) {
        errno = EINVAL;
        return -1;
    }

    network_length = htonl(length);
    if (send_all(socket_fd, &network_length, sizeof(network_length)) < 0) {
        return -1;
    }
    if (length > 0 && send_all(socket_fd, data, length) < 0) {
        return -1;
    }

    return 0;
}

int tcp_receive_message(int socket_fd, void **data, uint32_t *length)
{
    uint32_t network_length;
    uint32_t message_length;
    void *message = NULL;
    int result;

    if (data == NULL || length == NULL) {
        errno = EINVAL;
        return -1;
    }

    *data = NULL;
    *length = 0;

    result = receive_all(socket_fd, &network_length, sizeof(network_length), 1);
    if (result <= 0) {
        return result;
    }

    message_length = ntohl(network_length);
    if (message_length > TCP_STREAM_MAX_MESSAGE_SIZE) {
        errno = EMSGSIZE;
        return -1;
    }

    if (message_length > 0) {
        message = malloc(message_length);
        if (message == NULL) {
            return -1;
        }

        if (receive_all(socket_fd, message, message_length, 0) < 0) {
            free(message);
            return -1;
        }
    }

    *data = message;
    *length = message_length;
    return 1;
}
