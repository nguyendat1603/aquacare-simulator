#define _POSIX_C_SOURCE 200809L
#include "device.h"
#include "discovery.h"
#include "tcp_stream.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

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

struct cli_overrides {
    enum device_type type;
    uint16_t tcp_port;
    uint16_t discovery_port;
    const char *custom_id;
    const char *custom_name;
    const char *custom_password;

    int has_salinity;
    double salinity_ppt;
    int has_oxygen;
    double oxygen_mg_l;
    int has_ph;
    double ph;

    int has_flow;
    double flow_rate_m3_per_h;
    int has_duration;
    double duration_hours;

    int has_rpm;
    double speed_rpm;
    int has_schedules;
    struct device_time_window schedules[DEVICE_MAX_SCHEDULES];
    size_t schedule_count;

    int has_lime;
    double lime_kg;
    int has_ph_min;
    double ph_min;

    int has_feed;
    double feed_kg;
    int has_feed_times;
    struct device_time_point feed_times[DEVICE_MAX_FEED_TIMES];
    size_t feed_time_count;
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

    if (text == NULL || text[0] == '\0') {
        return -1;
    }
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value == 0UL || value > 65535UL) {
        return -1;
    }
    *port = (uint16_t)value;
    return 0;
}

static int parse_double_arg(const char *text, double *out_value)
{
    char *end = NULL;
    double value;

    if (text == NULL || text[0] == '\0') {
        return -1;
    }
    errno = 0;
    value = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !isfinite(value)) {
        return -1;
    }
    *out_value = value;
    return 0;
}

static int parse_schedule_list(const char *text,
                               struct device_time_window *windows,
                               size_t *out_count)
{
    size_t count = 0;
    const char *cursor = text;

    if (text == NULL || text[0] == '\0') {
        return -1;
    }
    for (;;) {
        const char *comma = strchr(cursor, ',');
        size_t len = (comma != NULL) ? (size_t)(comma - cursor) : strlen(cursor);
        char item[32];

        if (len == 0U || len >= sizeof(item) || count >= DEVICE_MAX_SCHEDULES) {
            return -1;
        }
        memcpy(item, cursor, len);
        item[len] = '\0';
        if (device_parse_time_window(item, &windows[count]) < 0) {
            return -1;
        }
        ++count;
        if (comma == NULL) {
            break;
        }
        cursor = comma + 1;
    }

    *out_count = count;
    return 0;
}

static int parse_feed_time_list(const char *text,
                                struct device_time_point *times,
                                size_t *out_count)
{
    size_t count = 0;
    const char *cursor = text;

    if (text == NULL || text[0] == '\0') {
        return -1;
    }
    for (;;) {
        const char *comma = strchr(cursor, ',');
        size_t len = (comma != NULL) ? (size_t)(comma - cursor) : strlen(cursor);
        char item[16];

        if (len == 0U || len >= sizeof(item) || count >= DEVICE_MAX_FEED_TIMES) {
            return -1;
        }
        memcpy(item, cursor, len);
        item[len] = '\0';
        if (device_parse_time_point(item, &times[count]) < 0) {
            return -1;
        }
        ++count;
        if (comma == NULL) {
            break;
        }
        cursor = comma + 1;
    }

    *out_count = count;
    return 0;
}

static void print_usage(const char *program_name)
{
    fprintf(stderr,
            "Usage: %s --type <env_sensor|water_pump|aerator|ph_balancer|auto_feeder> [options]\n"
            "Options:\n"
            "  --port <tcp_port>               TCP port (default: %u)\n"
            "  --discovery-port <udp_port>     UDP discovery port (default: %u)\n"
            "  --id <custom_id>                Custom device ID\n"
            "  --name <custom_name>            Custom device name\n"
            "  --password <password>           Device password (default: %s)\n"
            "  --salinity <ppt>                ENV_SENSOR initial salinity\n"
            "  --oxygen <mg/L>                 ENV_SENSOR initial oxygen\n"
            "  --ph <value>                    ENV_SENSOR initial pH\n"
            "  --flow <m3/h>                   WATER_PUMP flow rate (V)\n"
            "  --duration <hours>              WATER_PUMP duration (T)\n"
            "  --rpm <speed>                   AERATOR speed (C)\n"
            "  --schedule <HH:MM-HH:MM,...>    AERATOR time windows\n"
            "  --lime <kg>                     PH_BALANCER lime mass (Wca)\n"
            "  --ph-min <value>                PH_BALANCER min pH threshold (pHmin)\n"
            "  --feed <kg>                     AUTO_FEEDER feed mass (Wf)\n"
            "  --feed-time <HH:MM,...>         AUTO_FEEDER feed times\n",
            program_name,
            (unsigned int)DEVICE_DEFAULT_TCP_PORT,
            (unsigned int)DEVICE_DEFAULT_DISCOVERY_PORT,
            DEVICE_DEFAULT_PASSWORD);
}

static const char *require_arg_value(int argc, char **argv, int *index,
                                     const char *flag)
{
    if (*index + 1 >= argc) {
        fprintf(stderr, "Missing value for %s\n", flag);
        return NULL;
    }
    ++(*index);
    return argv[*index];
}

static int parse_cli(int argc, char **argv, struct cli_overrides *opts)
{
    memset(opts, 0, sizeof(*opts));
    opts->tcp_port = DEVICE_DEFAULT_TCP_PORT;
    opts->discovery_port = DEVICE_DEFAULT_DISCOVERY_PORT;

    for (int i = 1; i < argc; ++i) {
        const char *flag = argv[i];
        const char *val = NULL;

        if (strcmp(flag, "--help") == 0 || strcmp(flag, "-h") == 0) {
            print_usage(argv[0]);
            return 1;
        } else if (strcmp(flag, "--type") == 0) {
            val = require_arg_value(argc, argv, &i, flag);
            if (val == NULL || device_type_from_string(val, &opts->type) < 0) {
                fprintf(stderr, "Invalid device type: %s\n",
                        val != NULL ? val : "");
                return -1;
            }
        } else if (strcmp(flag, "--port") == 0) {
            val = require_arg_value(argc, argv, &i, flag);
            if (val == NULL || parse_port(val, &opts->tcp_port) < 0) {
                fprintf(stderr, "Invalid TCP port: %s\n",
                        val != NULL ? val : "");
                return -1;
            }
        } else if (strcmp(flag, "--discovery-port") == 0) {
            val = require_arg_value(argc, argv, &i, flag);
            if (val == NULL || parse_port(val, &opts->discovery_port) < 0) {
                fprintf(stderr, "Invalid discovery port: %s\n",
                        val != NULL ? val : "");
                return -1;
            }
        } else if (strcmp(flag, "--id") == 0) {
            opts->custom_id = require_arg_value(argc, argv, &i, flag);
            if (opts->custom_id == NULL) {
                return -1;
            }
        } else if (strcmp(flag, "--name") == 0) {
            opts->custom_name = require_arg_value(argc, argv, &i, flag);
            if (opts->custom_name == NULL) {
                return -1;
            }
        } else if (strcmp(flag, "--password") == 0) {
            opts->custom_password = require_arg_value(argc, argv, &i, flag);
            if (opts->custom_password == NULL) {
                return -1;
            }
        } else if (strcmp(flag, "--salinity") == 0) {
            val = require_arg_value(argc, argv, &i, flag);
            if (val == NULL || parse_double_arg(val, &opts->salinity_ppt) < 0) {
                fprintf(stderr, "Invalid salinity value\n");
                return -1;
            }
            opts->has_salinity = 1;
        } else if (strcmp(flag, "--oxygen") == 0) {
            val = require_arg_value(argc, argv, &i, flag);
            if (val == NULL || parse_double_arg(val, &opts->oxygen_mg_l) < 0) {
                fprintf(stderr, "Invalid oxygen value\n");
                return -1;
            }
            opts->has_oxygen = 1;
        } else if (strcmp(flag, "--ph") == 0) {
            val = require_arg_value(argc, argv, &i, flag);
            if (val == NULL || parse_double_arg(val, &opts->ph) < 0) {
                fprintf(stderr, "Invalid pH value\n");
                return -1;
            }
            opts->has_ph = 1;
        } else if (strcmp(flag, "--flow") == 0) {
            val = require_arg_value(argc, argv, &i, flag);
            if (val == NULL ||
                parse_double_arg(val, &opts->flow_rate_m3_per_h) < 0) {
                fprintf(stderr, "Invalid flow value\n");
                return -1;
            }
            opts->has_flow = 1;
        } else if (strcmp(flag, "--duration") == 0) {
            val = require_arg_value(argc, argv, &i, flag);
            if (val == NULL ||
                parse_double_arg(val, &opts->duration_hours) < 0) {
                fprintf(stderr, "Invalid duration value\n");
                return -1;
            }
            opts->has_duration = 1;
        } else if (strcmp(flag, "--rpm") == 0) {
            val = require_arg_value(argc, argv, &i, flag);
            if (val == NULL || parse_double_arg(val, &opts->speed_rpm) < 0) {
                fprintf(stderr, "Invalid rpm value\n");
                return -1;
            }
            opts->has_rpm = 1;
        } else if (strcmp(flag, "--schedule") == 0) {
            val = require_arg_value(argc, argv, &i, flag);
            if (val == NULL ||
                parse_schedule_list(val, opts->schedules,
                                    &opts->schedule_count) < 0) {
                fprintf(stderr, "Invalid schedule format (expected HH:MM-HH:MM,...)\n");
                return -1;
            }
            opts->has_schedules = 1;
        } else if (strcmp(flag, "--lime") == 0) {
            val = require_arg_value(argc, argv, &i, flag);
            if (val == NULL || parse_double_arg(val, &opts->lime_kg) < 0) {
                fprintf(stderr, "Invalid lime value\n");
                return -1;
            }
            opts->has_lime = 1;
        } else if (strcmp(flag, "--ph-min") == 0) {
            val = require_arg_value(argc, argv, &i, flag);
            if (val == NULL || parse_double_arg(val, &opts->ph_min) < 0) {
                fprintf(stderr, "Invalid pH min value\n");
                return -1;
            }
            opts->has_ph_min = 1;
        } else if (strcmp(flag, "--feed") == 0) {
            val = require_arg_value(argc, argv, &i, flag);
            if (val == NULL || parse_double_arg(val, &opts->feed_kg) < 0) {
                fprintf(stderr, "Invalid feed value\n");
                return -1;
            }
            opts->has_feed = 1;
        } else if (strcmp(flag, "--feed-time") == 0) {
            val = require_arg_value(argc, argv, &i, flag);
            if (val == NULL ||
                parse_feed_time_list(val, opts->feed_times,
                                     &opts->feed_time_count) < 0) {
                fprintf(stderr, "Invalid feed-time format (expected HH:MM,...)\n");
                return -1;
            }
            opts->has_feed_times = 1;
        } else {
            fprintf(stderr, "Unknown option: %s\n", flag);
            return -1;
        }
    }

    if (opts->type == DEVICE_TYPE_UNKNOWN) {
        fprintf(stderr, "Option --type is required\n");
        print_usage(argv[0]);
        return -1;
    }

    return 0;
}

static int apply_cli_overrides(struct aquacare_device *device,
                               const struct cli_overrides *opts)
{
    if (device_init_default(device, opts->type, opts->custom_id,
                            opts->custom_password, opts->tcp_port) < 0) {
        fprintf(stderr, "Failed to initialize device (invalid id or password)\n");
        return -1;
    }
    device->discovery_port = opts->discovery_port;

    if (opts->custom_name != NULL &&
        device_set_name(device, opts->custom_name) < 0) {
        fprintf(stderr, "Invalid device name\n");
        return -1;
    }

    if ((opts->has_salinity || opts->has_oxygen || opts->has_ph) &&
        device_set_env_sensor_params(
            device,
            opts->has_salinity ? opts->salinity_ppt :
                                 device->params.env_sensor.salinity_ppt,
            opts->has_oxygen ? opts->oxygen_mg_l :
                               device->params.env_sensor.oxygen_mg_l,
            opts->has_ph ? opts->ph : device->params.env_sensor.ph) < 0) {
        fprintf(stderr, "Invalid ENV_SENSOR parameters\n");
        return -1;
    }

    if ((opts->has_flow || opts->has_duration) &&
        device_set_water_pump_params(
            device,
            opts->has_flow ? opts->flow_rate_m3_per_h :
                             device->params.water_pump.flow_rate_m3_per_h,
            opts->has_duration ? opts->duration_hours :
                                 device->params.water_pump.duration_hours) < 0) {
        fprintf(stderr, "Invalid WATER_PUMP parameters\n");
        return -1;
    }

    if ((opts->has_rpm || opts->has_schedules) &&
        device_set_aerator_params(
            device,
            opts->has_rpm ? opts->speed_rpm : device->params.aerator.speed_rpm,
            opts->has_schedules ? opts->schedules :
                                  device->params.aerator.schedules,
            opts->has_schedules ? opts->schedule_count :
                                  device->params.aerator.schedule_count) < 0) {
        fprintf(stderr, "Invalid AERATOR parameters\n");
        return -1;
    }

    if ((opts->has_lime || opts->has_ph_min) &&
        device_set_ph_balancer_params(
            device,
            opts->has_lime ? opts->lime_kg :
                             device->params.ph_balancer.lime_kg,
            opts->has_ph_min ? opts->ph_min :
                               device->params.ph_balancer.ph_min,
            device->params.ph_balancer.current_ph) < 0) {
        fprintf(stderr, "Invalid PH_BALANCER parameters\n");
        return -1;
    }

    if ((opts->has_feed || opts->has_feed_times) &&
        device_set_auto_feeder_params(
            device,
            opts->has_feed ? opts->feed_kg :
                             device->params.auto_feeder.feed_kg,
            opts->has_feed_times ? opts->feed_times :
                                   device->params.auto_feeder.feed_times,
            opts->has_feed_times ? opts->feed_time_count :
                                   device->params.auto_feeder.feed_time_count) < 0) {
        fprintf(stderr, "Invalid AUTO_FEEDER parameters\n");
        return -1;
    }

    if (device_validate(device) < 0) {
        fprintf(stderr, "Device configuration validation failed\n");
        return -1;
    }

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
    printf("TCP client disconnected (socket %d)\n", socket_fd);
    fflush(stdout);
}

static int queue_reply(struct client_state *client, const void *data,
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

static int handle_tcp_command(struct client_state *client,
                              const struct aquacare_device *device,
                              const void *message,
                              uint32_t length)
{
    static const char pong[] = "PONG";
    static const char unsupported[] = "ERR|UNSUPPORTED_COMMAND";
    char summary[1024];

    if (length == 4U && memcmp(message, "PING", 4U) == 0) {
        return queue_reply(client, pong, (uint32_t)(sizeof(pong) - 1U));
    }
    if (length == 4U && memcmp(message, "INFO", 4U) == 0 &&
        device_format_summary(device, summary, sizeof(summary)) == 0) {
        return queue_reply(client, summary, (uint32_t)strlen(summary));
    }
    return queue_reply(client, unsupported,
                       (uint32_t)(sizeof(unsupported) - 1U));
}

static int read_client(int socket_fd, struct client_state *client,
                       const struct aquacare_device *device)
{
    unsigned char buffer[4096];
    ssize_t received = recv(socket_fd, buffer, sizeof(buffer), 0);
    size_t offset = 0;

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
        if (consumed == 0U) {
            free(message);
            errno = EPROTO;
            perror("tcp_message_parser_feed");
            return 0;
        }
        offset += consumed;
        if (result == 1) {
            int ok = handle_tcp_command(client, device, message, message_length);
            free(message);
            if (ok < 0) {
                perror("queue_reply");
                return 0;
            }
        }
    }
    return 1;
}

static int write_client(int socket_fd, struct client_state *client)
{
    size_t budget = CLIENT_WRITE_BUDGET;

    while (client->output_head != NULL && budget > 0U) {
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
        printf("TCP client connected (socket %d)\n", socket_fd);
        fflush(stdout);
    }
}

static int open_tcp_listener(uint16_t port)
{
    int listener;
    int reuse_address = 1;
    struct sockaddr_in server_address;

    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        perror("socket");
        return -1;
    }
    if (listener >= FD_SETSIZE) {
        fprintf(stderr, "TCP listening socket exceeds FD_SETSIZE\n");
        close(listener);
        return -1;
    }
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse_address,
                   sizeof(reuse_address)) < 0 ||
        set_nonblocking(listener) < 0) {
        perror("tcp listener setup");
        close(listener);
        return -1;
    }

    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);
    server_address.sin_port = htons(port);
    if (bind(listener, (struct sockaddr *)&server_address,
             sizeof(server_address)) < 0 ||
        listen(listener, SOMAXCONN) < 0) {
        perror("tcp bind/listen");
        close(listener);
        return -1;
    }
    return listener;
}

int main(int argc, char **argv)
{
    struct cli_overrides opts;
    struct aquacare_device device;
    struct client_state clients[FD_SETSIZE] = { 0 };
    struct sigaction action;
    int cli_status;
    int tcp_listener = -1;
    int udp_discovery_fd = -1;
    int exit_status = EXIT_SUCCESS;

    cli_status = parse_cli(argc, argv, &opts);
    if (cli_status > 0) {
        return EXIT_SUCCESS;
    }
    if (cli_status < 0 || apply_cli_overrides(&device, &opts) < 0) {
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

    tcp_listener = open_tcp_listener(device.tcp_port);
    if (tcp_listener < 0) {
        return EXIT_FAILURE;
    }
    udp_discovery_fd = discovery_listener_open(device.discovery_port);
    if (udp_discovery_fd < 0 || udp_discovery_fd >= FD_SETSIZE) {
        perror("discovery_listener_open");
        if (udp_discovery_fd >= 0) {
            close(udp_discovery_fd);
        }
        close(tcp_listener);
        return EXIT_FAILURE;
    }

    device_print_info(&device, stdout);
    printf("Listening: TCP port %u, UDP discovery port %u\n",
           (unsigned int)device.tcp_port,
           (unsigned int)device.discovery_port);
    fflush(stdout);

    while (!stop_requested) {
        fd_set read_set;
        fd_set write_set;
        int max_fd = tcp_listener > udp_discovery_fd ?
                         tcp_listener : udp_discovery_fd;
        int ready;

        FD_ZERO(&read_set);
        FD_ZERO(&write_set);
        FD_SET(tcp_listener, &read_set);
        FD_SET(udp_discovery_fd, &read_set);
        for (int fd = 0; fd < FD_SETSIZE; ++fd) {
            if (!clients[fd].active) {
                continue;
            }
            FD_SET(fd, &read_set);
            if (clients[fd].output_head != NULL) {
                FD_SET(fd, &write_set);
            }
            if (fd > max_fd) {
                max_fd = fd;
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

        if (FD_ISSET(udp_discovery_fd, &read_set)) {
            int replied = discovery_listener_handle_readable(udp_discovery_fd,
                                                             &device);
            if (replied < 0) {
                perror("discovery_listener_handle_readable");
                exit_status = EXIT_FAILURE;
                break;
            }
            if (replied > 0) {
                printf("Discovery: replied to %d scan probe(s) as %s (%s)\n",
                       replied,
                       device_type_to_string(device.type),
                       device.id);
                fflush(stdout);
            }
        }

        if (FD_ISSET(tcp_listener, &read_set)) {
            accept_clients(tcp_listener, clients);
        }

        for (int fd = 0; fd < FD_SETSIZE && !stop_requested; ++fd) {
            struct client_state *client = &clients[fd];

            if (!client->active) {
                continue;
            }
            if (FD_ISSET(fd, &read_set) && !read_client(fd, client, &device)) {
                clear_client(client, fd);
                continue;
            }
            if (client->active && FD_ISSET(fd, &write_set) &&
                !write_client(fd, client)) {
                clear_client(client, fd);
            }
        }
    }

    for (int fd = 0; fd < FD_SETSIZE; ++fd) {
        if (clients[fd].active) {
            clear_client(&clients[fd], fd);
        }
    }
    close(udp_discovery_fd);
    close(tcp_listener);
    return exit_status;
}
