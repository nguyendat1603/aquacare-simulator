#ifndef AQUACARE_DISCOVERY_H
#define AQUACARE_DISCOVERY_H

#include "device.h"

#include <arpa/inet.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DISCOVERY_DEFAULT_PORT DEVICE_DEFAULT_DISCOVERY_PORT
#define DISCOVERY_MULTICAST_ADDR "239.255.42.99"
#define DISCOVERY_PROTOCOL_VERSION 1
#define DISCOVERY_MAX_PACKET_SIZE 512
#define DISCOVERY_NONCE_MAX_LEN 17
#define DISCOVERY_DEFAULT_TIMEOUT_MS 1500
#define DISCOVERY_MAX_RESULTS 64

struct discovered_device {
    enum device_type type;
    char id[DEVICE_ID_MAX_LEN];
    char name[DEVICE_NAME_MAX_LEN];
    char ip_address[INET_ADDRSTRLEN];
    uint16_t tcp_port;
    bool is_on;
};

int discovery_build_scan_request(const char *nonce,
                                 char *buffer,
                                 size_t buffer_size,
                                 size_t *out_length);

int discovery_parse_scan_request(const char *packet,
                                 size_t packet_length,
                                 char *out_nonce,
                                 size_t nonce_size);

int discovery_build_scan_response(const struct aquacare_device *device,
                                  const char *nonce,
                                  char *buffer,
                                  size_t buffer_size,
                                  size_t *out_length);

int discovery_parse_scan_response(const char *packet,
                                  size_t packet_length,
                                  const char *expected_nonce,
                                  struct discovered_device *out_device);

int discovery_listener_open(uint16_t discovery_port);

int discovery_listener_handle_readable(int udp_fd,
                                       const struct aquacare_device *device);

int discovery_scan(uint16_t discovery_port,
                   int timeout_ms,
                   const char *target_ip,
                   struct discovered_device *results,
                   size_t max_results);

#endif
