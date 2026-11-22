#define _POSIX_C_SOURCE 200809L
#include "tcp_stream.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define SERVER_DEFAULT_PORT 9000
#define CLIENT_OUTPUT_LIMIT (4U * (TCP_STREAM_MAX_MESSAGE_SIZE + 4U))
#define CLIENT_WRITE_BUDGET (64U * 1024U)

struct queued_frame {
    unsigned char *data;
    size_t length;
    size_t sent;
    struct queued_frame *next;
};

struct client_state {
    int active;
    struct tcp_message_parser parser;
    struct queued_frame *output_head;
    struct queued_frame *output_tail;
    size_t output_bytes;
};

static volatile sig_atomic_t stop_requested;

static void handle_stop_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static int set_nonblocking(int socket_fd)
{
    int flags = fcntl(socket_fd, F_GETFL, 0);

    if (flags < 0 || fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }
    return 0;
}

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

static void clear_client(struct client_state *client, int socket_fd)
{
    struct queued_frame *frame = client->output_head;

    while (frame != NULL) {
        struct queued_frame *next = frame->next;
        free(frame->data);
        free(frame);
        frame = next;
    }
    tcp_message_parser_destroy(&client->parser);
    memset(client, 0, sizeof(*client));
    close(socket_fd);
    printf("Client disconnected (socket %d)\n", socket_fd);
}

static int queue_echo(struct client_state *client, const void *data,
                      uint32_t length)
{
    struct queued_frame *frame;
    void *encoded = NULL;

    frame = calloc(1, sizeof(*frame));
    if (frame == NULL) {
        return -1;
    }
    if (tcp_encode_message(data, length, &encoded, &frame->length) < 0) {
        free(frame);
        return -1;
    }
    frame->data = encoded;
    if (frame->length > CLIENT_OUTPUT_LIMIT - client->output_bytes) {
        free(frame->data);
        free(frame);
        errno = ENOBUFS;
        return -1;
    }

    if (client->output_tail == NULL) {
        client->output_head = frame;
    } else {
        client->output_tail->next = frame;
    }
    client->output_tail = frame;
    client->output_bytes += frame->length;
    return 0;
}

static int read_client(int socket_fd, struct client_state *client)
{
    unsigned char buffer[4096];
    ssize_t received;

    received = recv(socket_fd, buffer, sizeof(buffer), 0);
    if (received == 0) {
        return 0;
    }
    if (received < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            return 1;
        }
        perror("recv");
        return 0;
    }

    size_t offset = 0;
    while (offset < (size_t)received) {
        size_t consumed = 0;
        void *message = NULL;
        uint32_t message_length = 0;
        int result = tcp_message_parser_feed(
            &client->parser, buffer + offset, (size_t)received - offset,
            &consumed, &message, &message_length);

        if (result < 0) {
            perror("tcp_message_parser_feed");
            free(message);
            return 0;
        }
        if (consumed == 0) {
            free(message);
            errno = EPROTO;
            perror("tcp_message_parser_feed");
            return 0;
        }
        offset += consumed;
        if (result == 1) {
            if (queue_echo(client, message, message_length) < 0) {
                perror("queue_echo");
                free(message);
                return 0;
            }
            free(message);
        }
    }
    return 1;
}

static int write_client(int socket_fd, struct client_state *client)
{
    size_t budget = CLIENT_WRITE_BUDGET;

    while (client->output_head != NULL && budget > 0) {
        struct queued_frame *frame = client->output_head;
        size_t remaining = frame->length - frame->sent;
        size_t amount = remaining < budget ? remaining : budget;
        ssize_t sent = send(socket_fd, frame->data + frame->sent, amount, 0);

        if (sent > 0) {
            frame->sent += (size_t)sent;
            client->output_bytes -= (size_t)sent;
            budget -= (size_t)sent;
            if (frame->sent == frame->length) {
                client->output_head = frame->next;
                if (client->output_head == NULL) {
                    client->output_tail = NULL;
                }
                free(frame->data);
                free(frame);
            }
        } else if (sent < 0 && errno == EINTR) {
            continue;
        } else if (sent < 0 &&
                   (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else {
            if (sent == 0) {
                errno = EPIPE;
            }
            perror("send");
            return 0;
        }
    }
    return 1;
}

static void accept_clients(int listener, struct client_state *clients)
{
    for (;;) {
        struct sockaddr_in address;
        socklen_t address_length = sizeof(address);
        int socket_fd = accept(listener, (struct sockaddr *)&address,
                               &address_length);

        if (socket_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("accept");
            }
            return;
        }
        if (socket_fd >= FD_SETSIZE) {
            fprintf(stderr, "Rejected client: socket exceeds FD_SETSIZE\n");
            close(socket_fd);
            continue;
        }
        if (set_nonblocking(socket_fd) < 0) {
            perror("fcntl");
            close(socket_fd);
            continue;
        }

        memset(&clients[socket_fd], 0, sizeof(clients[socket_fd]));
        clients[socket_fd].active = 1;
        printf("Client connected (socket %d)\n", socket_fd);
    }
}

int main(int argc, char **argv)
{
    struct client_state clients[FD_SETSIZE] = { 0 };
    struct sigaction action;
    struct sockaddr_in server_address;
    uint16_t port = SERVER_DEFAULT_PORT;
    int listener;
    int reuse_address = 1;
    int exit_status = EXIT_SUCCESS;

    if (argc > 2 || (argc == 2 && parse_port(argv[1], &port) < 0)) {
        fprintf(stderr, "Usage: %s [port]\n", argv[0]);
        return EXIT_FAILURE;
    }
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_stop_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0) {
        perror("sigaction");
        return EXIT_FAILURE;
    }
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal");
        return EXIT_FAILURE;
    }

    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }
    if (listener >= FD_SETSIZE) {
        fprintf(stderr, "Listening socket exceeds FD_SETSIZE\n");
        close(listener);
        return EXIT_FAILURE;
    }
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse_address,
                   sizeof(reuse_address)) < 0) {
        perror("setsockopt");
        close(listener);
        return EXIT_FAILURE;
    }
    if (set_nonblocking(listener) < 0) {
        perror("fcntl");
        close(listener);
        return EXIT_FAILURE;
    }

    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);
    server_address.sin_port = htons(port);
    if (bind(listener, (struct sockaddr *)&server_address,
             sizeof(server_address)) < 0) {
        perror("bind");
        close(listener);
        return EXIT_FAILURE;
    }
    if (listen(listener, SOMAXCONN) < 0) {
        perror("listen");
        close(listener);
        return EXIT_FAILURE;
    }

    printf("Echo server listening on port %u\n", (unsigned int)port);
    while (!stop_requested) {
        fd_set read_set;
        fd_set write_set;
        int max_fd = listener;
        int ready;

        FD_ZERO(&read_set);
        FD_ZERO(&write_set);
        FD_SET(listener, &read_set);
        for (int socket_fd = 0; socket_fd < FD_SETSIZE; ++socket_fd) {
            if (!clients[socket_fd].active) {
                continue;
            }
            FD_SET(socket_fd, &read_set);
            if (clients[socket_fd].output_head != NULL) {
                FD_SET(socket_fd, &write_set);
            }
            if (socket_fd > max_fd) {
                max_fd = socket_fd;
            }
        }

        ready = select(max_fd + 1, &read_set, &write_set, NULL, NULL);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select");
            exit_status = EXIT_FAILURE;
            break;
        }
        if (FD_ISSET(listener, &read_set)) {
            accept_clients(listener, clients);
        }
        for (int socket_fd = 0;
             socket_fd < FD_SETSIZE && !stop_requested; ++socket_fd) {
            struct client_state *client = &clients[socket_fd];

            if (!client->active) {
                continue;
            }
            if (FD_ISSET(socket_fd, &read_set) &&
                !read_client(socket_fd, client)) {
                clear_client(client, socket_fd);
                continue;
            }
            if (client->active && FD_ISSET(socket_fd, &write_set) &&
                !write_client(socket_fd, client)) {
                clear_client(client, socket_fd);
            }
        }
    }

    for (int socket_fd = 0; socket_fd < FD_SETSIZE; ++socket_fd) {
        if (clients[socket_fd].active) {
            clear_client(&clients[socket_fd], socket_fd);
        }
    }
    close(listener);
    return exit_status;
}
