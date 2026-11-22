#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "discovery.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DISCOVERY_REQ_MAGIC "AQUACARE_SCAN"
#define DISCOVERY_RESP_MAGIC "AQUACARE_HERE"

static int nonce_is_valid(const char *nonce)
{
    size_t len;

    if (nonce == NULL) {
        return 0;
    }
    len = strlen(nonce);
    if (len == 0U || len >= DISCOVERY_NONCE_MAX_LEN) {
        return 0;
    }
    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)nonce[i];

        if (!(isalnum(ch) || ch == '_' || ch == '-')) {
            return 0;
        }
    }
    return 1;
}

static int parse_u16_port(const char *text, uint16_t *out_port)
{
    char *end = NULL;
    unsigned long value;

    if (text == NULL || text[0] == '\0' || out_port == NULL) {
        return -1;
    }
    for (size_t i = 0; text[i] != '\0'; ++i) {
        if (!isdigit((unsigned char)text[i])) {
            return -1;
        }
    }
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value == 0UL || value > 65535UL) {
        return -1;
    }
    *out_port = (uint16_t)value;
    return 0;
}

static int split_fields(char *text, char **fields, size_t max_fields,
                        size_t *out_count)
{
    size_t count = 0;
    char *cursor = text;

    for (;;) {
        char *sep;

        if (count >= max_fields) {
            return -1;
        }
        fields[count++] = cursor;
        sep = strchr(cursor, '|');
        if (sep == NULL) {
            break;
        }
        *sep = '\0';
        cursor = sep + 1;
    }

    *out_count = count;
    return 0;
}

static int prepare_packet_copy(const char *packet, size_t packet_length,
                               char *out_copy, size_t copy_size)
{
    if (packet == NULL || packet_length == 0U ||
        packet_length > DISCOVERY_MAX_PACKET_SIZE ||
        packet_length >= copy_size) {
        errno = EINVAL;
        return -1;
    }
    if (memchr(packet, '\0', packet_length) != NULL) {
        errno = EINVAL;
        return -1;
    }

    memcpy(out_copy, packet, packet_length);
    out_copy[packet_length] = '\0';
    while (packet_length > 0U &&
           (out_copy[packet_length - 1U] == '\n' ||
            out_copy[packet_length - 1U] == '\r')) {
        out_copy[--packet_length] = '\0';
    }
    if (packet_length == 0U) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int set_nonblocking_fd(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }
    return 0;
}

static void generate_random_nonce(char *out_nonce, size_t out_size)
{
    static uint32_t counter = 0;
    uint32_t value = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    int have_random = 0;

    if (fd >= 0) {
        size_t total = 0;
        unsigned char *bytes = (unsigned char *)&value;

        while (total < sizeof(value)) {
            ssize_t n = read(fd, bytes + total, sizeof(value) - total);

            if (n > 0) {
                total += (size_t)n;
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else {
                break;
            }
        }
        close(fd);
        have_random = (total == sizeof(value));
    }
    if (!have_random) {
        ++counter;
        value = (uint32_t)time(NULL) ^
                ((uint32_t)getpid() * 0x9E3779B1U) ^
                (counter * 0x85EBCA6BU);
    }

    snprintf(out_nonce, out_size, "%08x", value);
}

static int64_t monotonic_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
        return -1;
    }
    return (int64_t)ts.tv_sec * 1000LL + (int64_t)(ts.tv_nsec / 1000000L);
}

int discovery_build_scan_request(const char *nonce,
                                 char *buffer,
                                 size_t buffer_size,
                                 size_t *out_length)
{
    int written;

    if (!nonce_is_valid(nonce) || buffer == NULL || buffer_size == 0U) {
        errno = EINVAL;
        return -1;
    }

    written = snprintf(buffer, buffer_size, "%s|%d|%s",
                       DISCOVERY_REQ_MAGIC,
                       DISCOVERY_PROTOCOL_VERSION,
                       nonce);
    if (written < 0 || (size_t)written >= buffer_size ||
        (size_t)written > DISCOVERY_MAX_PACKET_SIZE) {
        errno = ENOSPC;
        return -1;
    }
    if (out_length != NULL) {
        *out_length = (size_t)written;
    }
    return 0;
}

int discovery_parse_scan_request(const char *packet,
                                 size_t packet_length,
                                 char *out_nonce,
                                 size_t nonce_size)
{
    char copy[DISCOVERY_MAX_PACKET_SIZE + 1];
    char *fields[4];
    size_t field_count = 0;
    char version_str[16];
    const char *parsed_nonce;
    size_t nonce_len;

    if (out_nonce == NULL || nonce_size == 0U) {
        errno = EINVAL;
        return -1;
    }
    if (prepare_packet_copy(packet, packet_length, copy, sizeof(copy)) < 0) {
        return -1;
    }
    if (split_fields(copy, fields, 4U, &field_count) < 0 ||
        (field_count != 2U && field_count != 3U)) {
        errno = EINVAL;
        return -1;
    }

    snprintf(version_str, sizeof(version_str), "%d",
             DISCOVERY_PROTOCOL_VERSION);
    if (strcmp(fields[0], DISCOVERY_REQ_MAGIC) != 0 ||
        strcmp(fields[1], version_str) != 0) {
        errno = EINVAL;
        return -1;
    }

    parsed_nonce = (field_count == 3U) ? fields[2] : "0";
    if (!nonce_is_valid(parsed_nonce)) {
        errno = EINVAL;
        return -1;
    }
    nonce_len = strlen(parsed_nonce);
    if (nonce_len >= nonce_size) {
        errno = ENOSPC;
        return -1;
    }
    memcpy(out_nonce, parsed_nonce, nonce_len + 1U);
    return 0;
}

int discovery_build_scan_response(const struct aquacare_device *device,
                                  const char *nonce,
                                  char *buffer,
                                  size_t buffer_size,
                                  size_t *out_length)
{
    int written;

    if (device == NULL || device_validate(device) < 0 ||
        !nonce_is_valid(nonce) || buffer == NULL || buffer_size == 0U) {
        errno = EINVAL;
        return -1;
    }

    written = snprintf(buffer, buffer_size, "%s|%d|%s|%s|%s|%u|%d|%s",
                       DISCOVERY_RESP_MAGIC,
                       DISCOVERY_PROTOCOL_VERSION,
                       nonce,
                       device_type_to_string(device->type),
                       device->id,
                       (unsigned int)device->tcp_port,
                       device->is_on ? 1 : 0,
                       device->name);
    if (written < 0 || (size_t)written >= buffer_size ||
        (size_t)written > DISCOVERY_MAX_PACKET_SIZE) {
        errno = ENOSPC;
        return -1;
    }
    if (out_length != NULL) {
        *out_length = (size_t)written;
    }
    return 0;
}

int discovery_parse_scan_response(const char *packet,
                                  size_t packet_length,
                                  const char *expected_nonce,
                                  struct discovered_device *out_device)
{
    char copy[DISCOVERY_MAX_PACKET_SIZE + 1];
    char *fields[9];
    size_t field_count = 0;
    char version_str[16];
    struct discovered_device dev;

    if (out_device == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (prepare_packet_copy(packet, packet_length, copy, sizeof(copy)) < 0) {
        return -1;
    }
    if (split_fields(copy, fields, 9U, &field_count) < 0 ||
        field_count != 8U) {
        errno = EINVAL;
        return -1;
    }

    snprintf(version_str, sizeof(version_str), "%d",
             DISCOVERY_PROTOCOL_VERSION);
    if (strcmp(fields[0], DISCOVERY_RESP_MAGIC) != 0 ||
        strcmp(fields[1], version_str) != 0 ||
        !nonce_is_valid(fields[2])) {
        errno = EINVAL;
        return -1;
    }
    if (expected_nonce != NULL && strcmp(fields[2], expected_nonce) != 0) {
        errno = EINVAL;
        return -1;
    }

    memset(&dev, 0, sizeof(dev));
    if (device_type_from_string(fields[3], &dev.type) < 0 ||
        !device_id_is_valid(fields[4]) ||
        parse_u16_port(fields[5], &dev.tcp_port) < 0 ||
        !device_name_is_valid(fields[7])) {
        errno = EINVAL;
        return -1;
    }
    if (strcmp(fields[6], "1") == 0) {
        dev.is_on = true;
    } else if (strcmp(fields[6], "0") == 0) {
        dev.is_on = false;
    } else {
        errno = EINVAL;
        return -1;
    }

    memcpy(dev.id, fields[4], strlen(fields[4]) + 1U);
    memcpy(dev.name, fields[7], strlen(fields[7]) + 1U);
    *out_device = dev;
    return 0;
}

int discovery_listener_open(uint16_t discovery_port)
{
    int udp_fd;
    int reuse = 1;
    struct sockaddr_in addr;
    struct ip_mreq mreq;

    if (discovery_port == 0U) {
        errno = EINVAL;
        return -1;
    }

    udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) {
        return -1;
    }

    if (setsockopt(udp_fd, SOL_SOCKET, SO_REUSEADDR, &reuse,
                   sizeof(reuse)) < 0) {
        int saved_errno = errno;
        close(udp_fd);
        errno = saved_errno;
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(discovery_port);
    if (bind(udp_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int saved_errno = errno;
        close(udp_fd);
        errno = saved_errno;
        return -1;
    }

    memset(&mreq, 0, sizeof(mreq));
    if (inet_pton(AF_INET, DISCOVERY_MULTICAST_ADDR, &mreq.imr_multiaddr) == 1) {
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        (void)setsockopt(udp_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq,
                         sizeof(mreq));
    }

    if (set_nonblocking_fd(udp_fd) < 0) {
        int saved_errno = errno;
        close(udp_fd);
        errno = saved_errno;
        return -1;
    }

    return udp_fd;
}

int discovery_listener_handle_readable(int udp_fd,
                                       const struct aquacare_device *device)
{
    int replies_sent = 0;

    if (udp_fd < 0 || device == NULL || device_validate(device) < 0) {
        errno = EINVAL;
        return -1;
    }

    for (int iter = 0; iter < 64; ++iter) {
        char packet[DISCOVERY_MAX_PACKET_SIZE + 1];
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        ssize_t received = recvfrom(udp_fd, packet, DISCOVERY_MAX_PACKET_SIZE,
                                    0, (struct sockaddr *)&peer, &peer_len);
        char nonce[DISCOVERY_NONCE_MAX_LEN];
        char reply[DISCOVERY_MAX_PACKET_SIZE + 1];
        size_t reply_len = 0;

        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            return -1;
        }

        if (discovery_parse_scan_request(packet, (size_t)received,
                                         nonce, sizeof(nonce)) == 0 &&
            discovery_build_scan_response(device, nonce, reply, sizeof(reply),
                                          &reply_len) == 0) {
            for (;;) {
                ssize_t sent = sendto(udp_fd, reply, reply_len, 0,
                                      (struct sockaddr *)&peer, peer_len);
                if (sent >= 0) {
                    ++replies_sent;
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
        }
    }

    return replies_sent;
}

static int send_probe_to_addr(int udp_fd, const char *ip_str, uint16_t port,
                              const char *packet, size_t packet_len)
{
    struct sockaddr_in dest;

    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    if (inet_pton(AF_INET, ip_str, &dest.sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }
    for (;;) {
        ssize_t sent = sendto(udp_fd, packet, packet_len, 0,
                              (struct sockaddr *)&dest, sizeof(dest));
        if (sent >= 0) {
            return 0;
        }
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }
}

static int send_scan_probes(int udp_fd, uint16_t discovery_port,
                            const char *target_ip,
                            const char *packet, size_t packet_len)
{
    static const char *const auto_targets[] = {
        DISCOVERY_MULTICAST_ADDR,
        "255.255.255.255",
        "127.255.255.255"
    };
    int successes = 0;

    if (target_ip != NULL && target_ip[0] != '\0') {
        return send_probe_to_addr(udp_fd, target_ip, discovery_port,
                                  packet, packet_len);
    }

    for (size_t i = 0; i < sizeof(auto_targets) / sizeof(auto_targets[0]); ++i) {
        if (send_probe_to_addr(udp_fd, auto_targets[i], discovery_port,
                               packet, packet_len) == 0) {
            ++successes;
        }
    }
    return successes > 0 ? 0 : -1;
}

static int results_contains_id(const struct discovered_device *results,
                               size_t count, const char *id)
{
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(results[i].id, id) == 0) {
            return 1;
        }
    }
    return 0;
}

int discovery_scan(uint16_t discovery_port,
                   int timeout_ms,
                   const char *target_ip,
                   struct discovered_device *results,
                   size_t max_results)
{
    int udp_fd = -1;
    int enable_broadcast = 1;
    int mcast_loop = 1;
    unsigned char mcast_ttl = 1;
    struct sockaddr_in local_addr;
    char nonce[DISCOVERY_NONCE_MAX_LEN];
    char request[DISCOVERY_MAX_PACKET_SIZE + 1];
    size_t request_len = 0;
    int64_t start_ms;
    int64_t retry_at_ms;
    int64_t deadline_ms;
    int retry_sent = 0;
    size_t found_count = 0;
    int saved_errno = 0;

    if (discovery_port == 0U || timeout_ms <= 0 ||
        (results == NULL && max_results > 0U)) {
        errno = EINVAL;
        return -1;
    }

    generate_random_nonce(nonce, sizeof(nonce));
    if (discovery_build_scan_request(nonce, request, sizeof(request),
                                     &request_len) < 0) {
        return -1;
    }

    udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) {
        return -1;
    }

    (void)setsockopt(udp_fd, SOL_SOCKET, SO_BROADCAST,
                     &enable_broadcast, sizeof(enable_broadcast));
    (void)setsockopt(udp_fd, IPPROTO_IP, IP_MULTICAST_LOOP,
                     &mcast_loop, sizeof(mcast_loop));
    (void)setsockopt(udp_fd, IPPROTO_IP, IP_MULTICAST_TTL,
                     &mcast_ttl, sizeof(mcast_ttl));

    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    local_addr.sin_port = htons(0);
    if (bind(udp_fd, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0 ||
        set_nonblocking_fd(udp_fd) < 0) {
        saved_errno = errno;
        close(udp_fd);
        errno = saved_errno;
        return -1;
    }

    if (send_scan_probes(udp_fd, discovery_port, target_ip,
                         request, request_len) < 0) {
        saved_errno = errno;
        close(udp_fd);
        errno = saved_errno;
        return -1;
    }

    start_ms = monotonic_ms();
    if (start_ms < 0) {
        saved_errno = errno;
        close(udp_fd);
        errno = saved_errno;
        return -1;
    }
    deadline_ms = start_ms + (int64_t)timeout_ms;
    retry_at_ms = start_ms + (int64_t)(timeout_ms / 2);
    if (timeout_ms < 200) {
        retry_sent = 1;
    }

    for (;;) {
        int64_t now_ms = monotonic_ms();
        int64_t wait_until_ms;
        int64_t remaining_ms;
        fd_set read_set;
        struct timeval tv;
        int ready;

        if (now_ms < 0) {
            saved_errno = errno;
            close(udp_fd);
            errno = saved_errno;
            return -1;
        }
        if (now_ms >= deadline_ms) {
            break;
        }
        if (!retry_sent && now_ms >= retry_at_ms) {
            (void)send_scan_probes(udp_fd, discovery_port, target_ip,
                                   request, request_len);
            retry_sent = 1;
        }

        wait_until_ms = (!retry_sent && retry_at_ms < deadline_ms) ?
                            retry_at_ms : deadline_ms;
        remaining_ms = wait_until_ms - now_ms;
        if (remaining_ms < 0) {
            remaining_ms = 0;
        }

        FD_ZERO(&read_set);
        FD_SET(udp_fd, &read_set);
        tv.tv_sec = (long)(remaining_ms / 1000LL);
        tv.tv_usec = (long)((remaining_ms % 1000LL) * 1000LL);

        ready = select(udp_fd + 1, &read_set, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            saved_errno = errno;
            close(udp_fd);
            errno = saved_errno;
            return -1;
        }
        if (ready > 0 && FD_ISSET(udp_fd, &read_set)) {
            for (;;) {
                char packet[DISCOVERY_MAX_PACKET_SIZE + 1];
                struct sockaddr_in peer;
                socklen_t peer_len = sizeof(peer);
                ssize_t n = recvfrom(udp_fd, packet, DISCOVERY_MAX_PACKET_SIZE,
                                     0, (struct sockaddr *)&peer, &peer_len);
                struct discovered_device item;

                if (n < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    saved_errno = errno;
                    close(udp_fd);
                    errno = saved_errno;
                    return -1;
                }

                if (discovery_parse_scan_response(packet, (size_t)n,
                                                  nonce, &item) == 0) {
                    if (inet_ntop(AF_INET, &peer.sin_addr, item.ip_address,
                                  sizeof(item.ip_address)) == NULL) {
                        memcpy(item.ip_address, "0.0.0.0", 8U);
                    }
                    if (!results_contains_id(results, found_count, item.id) &&
                        found_count < max_results) {
                        results[found_count++] = item;
                    }
                }
            }
        }
    }

    close(udp_fd);
    return (int)found_count;
}
