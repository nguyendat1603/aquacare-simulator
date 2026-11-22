//.
#define _POSIX_C_SOURCE 200809L
#include "tcp_stream.h"

#include <errno.h>
#include <inttypes.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int parse_port(const char *text, uint16_t *port)
{
    char *end = NULL;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value == 0 || value > 65535) {
        return -1;
    }
    *port = (uint16_t)value;
    return 0;
}

int main(int argc, char **argv)
{
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct addrinfo *address;
    char service[6];
    uint16_t port;
    int socket_fd = -1;
    int result;
    void *response = NULL;
    uint32_t response_length = 0;
    size_t message_length;

    if (argc != 4 || parse_port(argv[2], &port) < 0) {
        fprintf(stderr, "Usage: %s <host> <port> <message>\n", argv[0]);
        return EXIT_FAILURE;
    }
    message_length = strlen(argv[3]);
    if (message_length > TCP_STREAM_MAX_MESSAGE_SIZE) {
        fprintf(stderr, "Message exceeds the 1 MiB limit\n");
        return EXIT_FAILURE;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(service, sizeof(service), "%u", (unsigned int)port);
    result = getaddrinfo(argv[1], service, &hints, &addresses);
    if (result != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(result));
        return EXIT_FAILURE;
    }

    for (address = addresses; address != NULL; address = address->ai_next) {
        socket_fd = socket(address->ai_family, address->ai_socktype,
                           address->ai_protocol);
        if (socket_fd < 0) {
            continue;
        }
        if (connect(socket_fd, address->ai_addr, address->ai_addrlen) == 0) {
            break;
        }
        close(socket_fd);
        socket_fd = -1;
    }
    freeaddrinfo(addresses);
    if (socket_fd < 0) {
        perror("connect");
        return EXIT_FAILURE;
    }

    if (tcp_send_message(socket_fd, argv[3], (uint32_t)message_length) < 0) {
        perror("tcp_send_message");
        close(socket_fd);
        return EXIT_FAILURE;
    }
    result = tcp_receive_message(socket_fd, &response, &response_length);
    if (result != 1) {
        if (result == 0) {
            fprintf(stderr, "Server closed the connection without a reply\n");
        } else {
            perror("tcp_receive_message");
        }
        close(socket_fd);
        return EXIT_FAILURE;
    }

    printf("Echo (%" PRIu32 " bytes): ", response_length);
    if (response_length > 0 &&
        fwrite(response, 1, response_length, stdout) != response_length) {
        perror("fwrite");
        free(response);
        close(socket_fd);
        return EXIT_FAILURE;
    }
    putchar('\n');
    free(response);
    close(socket_fd);
    return EXIT_SUCCESS;
}
