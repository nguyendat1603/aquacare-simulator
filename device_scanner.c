#define _POSIX_C_SOURCE 200809L
#include "discovery.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static int parse_port_arg(const char *text, uint16_t *out_port)
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
    *out_port = (uint16_t)value;
    return 0;
}

static int parse_timeout_arg(const char *text, int *out_timeout)
{
    char *end = NULL;
    long value;

    if (text == NULL || text[0] == '\0') {
        return -1;
    }
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value <= 0L || value > 60000L) {
        return -1;
    }
    *out_timeout = (int)value;
    return 0;
}

int main(int argc, char **argv)
{
    uint16_t discovery_port = DISCOVERY_DEFAULT_PORT;
    int timeout_ms = DISCOVERY_DEFAULT_TIMEOUT_MS;
    const char *target_ip = NULL;
    struct discovered_device results[DISCOVERY_MAX_RESULTS];
    int found;

    if (argc > 4) {
        fprintf(stderr,
                "Usage: %s [discovery_port] [timeout_ms] [target_ip]\n",
                argv[0]);
        return EXIT_FAILURE;
    }
    if (argc >= 2 && parse_port_arg(argv[1], &discovery_port) < 0) {
        fprintf(stderr, "Invalid discovery port: %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    if (argc >= 3 && parse_timeout_arg(argv[2], &timeout_ms) < 0) {
        fprintf(stderr, "Invalid timeout_ms (1..60000): %s\n", argv[2]);
        return EXIT_FAILURE;
    }
    if (argc == 4) {
        target_ip = argv[3];
    }

    printf("Scanning for AquaCare devices on UDP port %u (timeout %d ms, target %s)...\n",
           (unsigned int)discovery_port,
           timeout_ms,
           target_ip != NULL ? target_ip : "auto multicast+broadcast");

    found = discovery_scan(discovery_port, timeout_ms, target_ip,
                           results, DISCOVERY_MAX_RESULTS);
    if (found < 0) {
        perror("discovery_scan");
        return EXIT_FAILURE;
    }

    if (found == 0) {
        printf("No devices found.\n");
        return EXIT_SUCCESS;
    }

    printf("%-3s %-12s %-20s %-15s %-8s %-5s %s\n",
           "#", "TYPE", "DEVICE ID", "IP", "TCP PORT", "STATE", "NAME");
    for (int i = 0; i < found; ++i) {
        printf("%-3d %-12s %-20s %-15s %-8u %-5s %s\n",
               i + 1,
               device_type_to_string(results[i].type),
               results[i].id,
               results[i].ip_address,
               (unsigned int)results[i].tcp_port,
               results[i].is_on ? "ON" : "OFF",
               results[i].name);
    }
    printf("Total devices found: %d\n", found);
    return EXIT_SUCCESS;
}
