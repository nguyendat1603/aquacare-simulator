#define _POSIX_C_SOURCE 200809L
#include "tcp_stream.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
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

int tcp_encode_message(const void *data, uint32_t length, void **frame,
                       size_t *frame_length)
{
    uint32_t network_length;
    unsigned char *encoded;

    if (frame == NULL || frame_length == NULL) {
        errno = EINVAL;
        return -1;
    }
    *frame = NULL;
    *frame_length = 0;
    if (length > TCP_STREAM_MAX_MESSAGE_SIZE) {
        errno = EMSGSIZE;
        return -1;
    }
    if (length > 0 && data == NULL) {
        errno = EINVAL;
        return -1;
    }

    encoded = malloc(sizeof(network_length) + (size_t)length);
    if (encoded == NULL) {
        return -1;
    }
    network_length = htonl(length);
    memcpy(encoded, &network_length, sizeof(network_length));
    if (length > 0) {
        memcpy(encoded + sizeof(network_length), data, length);
    }
    *frame = encoded;
    *frame_length = sizeof(network_length) + (size_t)length;
    return 0;
}

int tcp_message_parser_feed(struct tcp_message_parser *parser,
                            const void *bytes, size_t byte_count,
                            size_t *consumed, void **data, uint32_t *length)
{
    const unsigned char *input = bytes;
    size_t offset = 0;

    if (parser == NULL || consumed == NULL || data == NULL || length == NULL ||
        (byte_count > 0 && bytes == NULL)) {
        errno = EINVAL;
        return -1;
    }
    *consumed = 0;
    *data = NULL;
    *length = 0;

    while (offset < byte_count) {
        if (parser->header_received < sizeof(parser->header)) {
            size_t remaining = sizeof(parser->header) -
                              parser->header_received;
            size_t available = byte_count - offset;
            size_t count = remaining < available ? remaining : available;

            memcpy(parser->header + parser->header_received, input + offset,
                   count);
            parser->header_received += count;
            offset += count;
            if (parser->header_received < sizeof(parser->header)) {
                break;
            }

            memcpy(&parser->payload_length, parser->header,
                   sizeof(parser->payload_length));
            parser->payload_length = ntohl(parser->payload_length);
            if (parser->payload_length > TCP_STREAM_MAX_MESSAGE_SIZE) {
                errno = EMSGSIZE;
                *consumed = offset;
                return -1;
            }
            if (parser->payload_length == 0) {
                parser->header_received = 0;
                *data = NULL;
                *length = 0;
                *consumed = offset;
                return 1;
            }
            parser->payload = malloc(parser->payload_length);
            if (parser->payload == NULL) {
                *consumed = offset;
                return -1;
            }
        }

        if (parser->payload_received < parser->payload_length) {
            size_t remaining = parser->payload_length -
                               parser->payload_received;
            size_t available = byte_count - offset;
            size_t count = remaining < available ? remaining : available;

            memcpy(parser->payload + parser->payload_received, input + offset,
                   count);
            parser->payload_received += count;
            offset += count;
        }
        if (parser->payload_received == parser->payload_length) {
            *data = parser->payload;
            *length = parser->payload_length;
            parser->payload = NULL;
            parser->payload_length = 0;
            parser->payload_received = 0;
            parser->header_received = 0;
            *consumed = offset;
            return 1;
        }
    }

    *consumed = offset;
    return 0;
}

void tcp_message_parser_destroy(struct tcp_message_parser *parser)
{
    if (parser != NULL) {
        free(parser->payload);
        memset(parser, 0, sizeof(*parser));
    }
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
