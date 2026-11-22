#ifndef TCP_STREAM_H
#define TCP_STREAM_H

#include <stddef.h>
#include <stdint.h>

#define TCP_STREAM_MAX_MESSAGE_SIZE (1024U * 1024U)

struct tcp_message_parser {
    unsigned char header[4];
    size_t header_received;
    unsigned char *payload;
    uint32_t payload_length;
    size_t payload_received;
};

/*
 * Sends one message as a 4-byte network-order length followed by its payload.
 * Returns 0 on success and -1 on error (errno is set).
 */
int tcp_send_message(int socket_fd, const void *data, uint32_t length);

/*
 * Encodes one message into an allocated frame. The caller owns *frame.
 */
int tcp_encode_message(const void *data, uint32_t length, void **frame,
                       size_t *frame_length);

/*
 * Incrementally decodes bytes without waiting for more socket data.
 * Returns 1 for a complete message, 0 when more bytes are needed, or -1 on
 * error. *consumed reports how many bytes were used. A returned message is
 * owned by the caller and must be freed. Initialize the parser to zero and
 * call tcp_message_parser_destroy() when finished.
 */
int tcp_message_parser_feed(struct tcp_message_parser *parser,
                            const void *bytes, size_t byte_count,
                            size_t *consumed, void **data, uint32_t *length);
void tcp_message_parser_destroy(struct tcp_message_parser *parser);

/*
 * Receives one complete message.
 * Returns 1 on success, 0 if the peer closed cleanly between messages, or
 * -1 on error (errno is set). The caller owns *data and must free it.
 * For an empty message, *data is NULL and *length is 0.
 */
int tcp_receive_message(int socket_fd, void **data, uint32_t *length);

#endif
