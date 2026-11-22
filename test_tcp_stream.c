//Test tcp_stream.c
#define _POSIX_C_SOURCE 200809L

#include "tcp_stream.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct raw_sender_args {
    int socket_fd;
    const unsigned char *bytes;
    size_t length;
    int result;
};

struct message_sender_args {
    int socket_fd;
    const void *data;
    uint32_t length;
    int result;
    int error_number;
};

static int send_raw_all(int socket_fd, const void *data, size_t length)
{
    const unsigned char *bytes = data;
    size_t sent = 0;

    while (sent < length) {
        ssize_t result = send(socket_fd, bytes + sent, length - sent, 0);

        if (result > 0) {
            sent += (size_t)result;
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else {
            return -1;
        }
    }

    return 0;
}

static void *send_fragmented_frames(void *argument)
{
    struct raw_sender_args *args = argument;

    args->result = -1;
    if (send_raw_all(args->socket_fd, args->bytes, 1) < 0 ||
        send_raw_all(args->socket_fd, args->bytes + 1, 2) < 0 ||
        send_raw_all(args->socket_fd, args->bytes + 3,
                     args->length - 3) < 0) {
        return NULL;
    }

    args->result = 0;
    return NULL;
}

static void *send_large_message(void *argument)
{
    struct message_sender_args *args = argument;

    args->result = tcp_send_message(args->socket_fd, args->data, args->length);
    args->error_number = errno;
    return NULL;
}

static int test_fragmented_and_coalesced_messages(void)
{
    static const unsigned char first[] = "sensor=7";
    static const unsigned char second[] = "pump=on";
    unsigned char stream[4 + sizeof(first) - 1 + 4 + sizeof(second) - 1];
    uint32_t network_length;
    int sockets[2];
    pthread_t sender_thread;
    struct raw_sender_args sender_args;
    void *received = NULL;
    uint32_t received_length = 0;
    int success = 0;
    size_t offset = 0;

    network_length = htonl((uint32_t)(sizeof(first) - 1));
    memcpy(stream + offset, &network_length, sizeof(network_length));
    offset += sizeof(network_length);
    memcpy(stream + offset, first, sizeof(first) - 1);
    offset += sizeof(first) - 1;

    network_length = htonl((uint32_t)(sizeof(second) - 1));
    memcpy(stream + offset, &network_length, sizeof(network_length));
    offset += sizeof(network_length);
    memcpy(stream + offset, second, sizeof(second) - 1);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0) {
        perror("socketpair");
        return -1;
    }

    sender_args.socket_fd = sockets[0];
    sender_args.bytes = stream;
    sender_args.length = sizeof(stream);
    sender_args.result = -1;
    if (pthread_create(&sender_thread, NULL, send_fragmented_frames,
                       &sender_args) != 0) {
        fprintf(stderr, "pthread_create failed\n");
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }

    if (tcp_receive_message(sockets[1], &received, &received_length) != 1 ||
        received_length != sizeof(first) - 1 ||
        memcmp(received, first, received_length) != 0) {
        fprintf(stderr, "first fragmented message did not match\n");
        goto cleanup;
    }
    free(received);
    received = NULL;

    if (tcp_receive_message(sockets[1], &received, &received_length) != 1 ||
        received_length != sizeof(second) - 1 ||
        memcmp(received, second, received_length) != 0) {
        fprintf(stderr, "second coalesced message did not match\n");
        goto cleanup;
    }

    success = 1;

cleanup:
    free(received);
    pthread_join(sender_thread, NULL);
    close(sockets[0]);
    close(sockets[1]);
    if (sender_args.result != 0) {
        fprintf(stderr, "raw sender failed\n");
        success = 0;
    }

    return success ? 0 : -1;
}

static int test_send_and_receive_message(void)
{
    static const unsigned char expected[] = {
        0x41, 0x00, 0x42, 0xff, 0x43
    };
    int sockets[2];
    void *received = NULL;
    uint32_t received_length = 0;
    int result = -1;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0) {
        perror("socketpair");
        return -1;
    }

    if (tcp_send_message(sockets[0], expected, sizeof(expected)) < 0) {
        perror("tcp_send_message");
        goto cleanup;
    }

    if (tcp_receive_message(sockets[1], &received, &received_length) != 1 ||
        received_length != sizeof(expected) ||
        memcmp(received, expected, sizeof(expected)) != 0) {
        fprintf(stderr, "binary round-trip did not match\n");
        goto cleanup;
    }

    free(received);
    received = NULL;
    if (tcp_send_message(sockets[0], NULL, 0) < 0 ||
        tcp_receive_message(sockets[1], &received, &received_length) != 1 ||
        received != NULL || received_length != 0) {
        fprintf(stderr, "empty message did not match\n");
        goto cleanup;
    }

    result = 0;

cleanup:
    free(received);
    close(sockets[0]);
    close(sockets[1]);
    return result;
}

static int test_large_message(void)
{
    const uint32_t message_length = 256U * 1024U;
    unsigned char *expected = malloc(message_length);
    int sockets[2];
    pthread_t sender_thread;
    struct message_sender_args sender_args;
    void *received = NULL;
    uint32_t received_length = 0;
    int result = -1;

    if (expected == NULL) {
        perror("malloc");
        return -1;
    }
    for (uint32_t i = 0; i < message_length; ++i) {
        expected[i] = (unsigned char)(i % 251U);
    }

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0) {
        perror("socketpair");
        free(expected);
        return -1;
    }

    sender_args.socket_fd = sockets[0];
    sender_args.data = expected;
    sender_args.length = message_length;
    sender_args.result = -1;
    sender_args.error_number = 0;
    if (pthread_create(&sender_thread, NULL, send_large_message,
                       &sender_args) != 0) {
        fprintf(stderr, "pthread_create failed\n");
        close(sockets[0]);
        close(sockets[1]);
        free(expected);
        return -1;
    }

    if (tcp_receive_message(sockets[1], &received, &received_length) != 1 ||
        received_length != message_length ||
        memcmp(received, expected, message_length) != 0) {
        fprintf(stderr, "large message did not match\n");
        goto cleanup;
    }
    result = 0;

cleanup:
    free(received);
    if (result != 0) {
        shutdown(sockets[1], SHUT_RDWR);
    }
    pthread_join(sender_thread, NULL);
    close(sockets[0]);
    close(sockets[1]);
    if (sender_args.result != 0) {
        errno = sender_args.error_number;
        perror("tcp_send_message");
        result = -1;
    }
    free(expected);
    return result;
}

static int test_connection_and_size_errors(void)
{
    uint32_t oversized_length = htonl(TCP_STREAM_MAX_MESSAGE_SIZE + 1U);
    unsigned char partial_header[2] = { 0x00, 0x00 };
    int sockets[2];
    void *received = NULL;
    uint32_t received_length = 0;
    int result = -1;

    if (tcp_send_message(-1, NULL, TCP_STREAM_MAX_MESSAGE_SIZE + 1U) != -1 ||
        errno != EMSGSIZE) {
        fprintf(stderr, "oversized outgoing message was not rejected\n");
        return -1;
    }

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0) {
        perror("socketpair");
        return -1;
    }
    if (send_raw_all(sockets[0], &oversized_length,
                     sizeof(oversized_length)) < 0) {
        perror("send");
        goto oversized_cleanup;
    }
    if (tcp_receive_message(sockets[1], &received, &received_length) != -1 ||
        errno != EMSGSIZE) {
        fprintf(stderr, "oversized incoming message was not rejected\n");
        goto oversized_cleanup;
    }
    result = 0;

oversized_cleanup:
    close(sockets[0]);
    close(sockets[1]);
    if (result != 0) {
        return -1;
    }

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0) {
        perror("socketpair");
        return -1;
    }
    if (send_raw_all(sockets[0], partial_header, sizeof(partial_header)) < 0) {
        perror("send");
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }
    shutdown(sockets[0], SHUT_WR);

    result = -1;
    if (tcp_receive_message(sockets[1], &received, &received_length) != -1 ||
        errno != ECONNRESET) {
        fprintf(stderr, "truncated message did not report connection loss\n");
    } else {
        result = 0;
    }
    close(sockets[0]);
    close(sockets[1]);
    if (result != 0) {
        return -1;
    }

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0) {
        perror("socketpair");
        return -1;
    }
    close(sockets[0]);
    result = -1;
    if (tcp_receive_message(sockets[1], &received, &received_length) != 0 ||
        received != NULL || received_length != 0) {
        fprintf(stderr, "clean end-of-stream was not reported\n");
    } else {
        result = 0;
    }
    close(sockets[1]);
    return result;
}

static int test_incremental_parser(void)
{
    static const unsigned char first_message[] = "first";
    static const unsigned char second_message[] = "second";
    struct tcp_message_parser first_parser = { 0 };
    struct tcp_message_parser second_parser = { 0 };
    void *first_frame = NULL;
    void *second_frame = NULL;
    void *empty_frame = NULL;
    size_t first_frame_length = 0;
    size_t second_frame_length = 0;
    size_t empty_frame_length = 0;
    unsigned char *combined = NULL;
    size_t combined_length;
    size_t consumed = 0;
    uint32_t message_length = 0;
    void *message = NULL;
    int result = -1;

    if (tcp_encode_message(first_message, sizeof(first_message) - 1,
                           &first_frame, &first_frame_length) < 0 ||
        tcp_encode_message(second_message, sizeof(second_message) - 1,
                           &second_frame, &second_frame_length) < 0 ||
        tcp_encode_message(NULL, 0, &empty_frame,
                           &empty_frame_length) < 0) {
        perror("tcp_encode_message");
        goto cleanup;
    }

    combined_length = first_frame_length + second_frame_length;
    combined = malloc(combined_length);
    if (combined == NULL) {
        perror("malloc");
        goto cleanup;
    }
    memcpy(combined, first_frame, first_frame_length);
    memcpy(combined + first_frame_length, second_frame, second_frame_length);

    if (tcp_message_parser_feed(&first_parser, first_frame, 6, &consumed,
                                &message, &message_length) != 0 ||
        consumed != 6 || message != NULL) {
        fprintf(stderr, "parser did not retain a fragmented frame\n");
        goto cleanup;
    }
    if (tcp_message_parser_feed(&second_parser, second_frame,
                                second_frame_length, &consumed, &message,
                                &message_length) != 1 ||
        consumed != second_frame_length ||
        message_length != sizeof(second_message) - 1 ||
        memcmp(message, second_message, message_length) != 0) {
        fprintf(stderr, "independent parser state did not decode message\n");
        goto cleanup;
    }
    free(message);
    message = NULL;

    if (tcp_message_parser_feed(&first_parser, combined + 6,
                                combined_length - 6, &consumed, &message,
                                &message_length) != 1 ||
        consumed != first_frame_length - 6 ||
        message_length != sizeof(first_message) - 1 ||
        memcmp(message, first_message, message_length) != 0) {
        fprintf(stderr, "fragmented frame did not decode correctly\n");
        goto cleanup;
    }
    free(message);
    message = NULL;

    if (tcp_message_parser_feed(&first_parser,
                                combined + first_frame_length,
                                second_frame_length, &consumed, &message,
                                &message_length) != 1 ||
        consumed != second_frame_length ||
        message_length != sizeof(second_message) - 1 ||
        memcmp(message, second_message, message_length) != 0) {
        fprintf(stderr, "coalesced frame did not decode correctly\n");
        goto cleanup;
    }
    free(message);
    message = NULL;

    if (tcp_message_parser_feed(&second_parser, empty_frame,
                                empty_frame_length, &consumed, &message,
                                &message_length) != 1 ||
        consumed != empty_frame_length || message != NULL ||
        message_length != 0) {
        fprintf(stderr, "incremental parser did not decode empty message\n");
        goto cleanup;
    }
    result = 0;

cleanup:
    free(message);
    free(combined);
    free(first_frame);
    free(second_frame);
    free(empty_frame);
    tcp_message_parser_destroy(&first_parser);
    tcp_message_parser_destroy(&second_parser);
    return result;
}

int main(void)
{
    if (test_fragmented_and_coalesced_messages() < 0 ||
        test_send_and_receive_message() < 0 ||
        test_large_message() < 0 ||
        test_connection_and_size_errors() < 0 ||
        test_incremental_parser() < 0) {
        return EXIT_FAILURE;
    }

    puts("All TCP stream tests passed.");
    return EXIT_SUCCESS;
}
