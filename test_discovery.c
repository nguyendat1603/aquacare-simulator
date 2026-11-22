#define _POSIX_C_SOURCE 200809L
#include "device.h"
#include "discovery.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_TEST_LISTENERS 5

struct listener_worker {
    int fds[MAX_TEST_LISTENERS];
    struct aquacare_device devices[MAX_TEST_LISTENERS];
    size_t count;
    int stop_pipe[2];
    int error;
};

static void *listener_thread_main(void *arg)
{
    struct listener_worker *worker = arg;

    for (;;) {
        fd_set read_set;
        int max_fd = worker->stop_pipe[0];
        int ready;

        FD_ZERO(&read_set);
        FD_SET(worker->stop_pipe[0], &read_set);
        for (size_t i = 0; i < worker->count; ++i) {
            FD_SET(worker->fds[i], &read_set);
            if (worker->fds[i] > max_fd) {
                max_fd = worker->fds[i];
            }
        }

        ready = select(max_fd + 1, &read_set, NULL, NULL, NULL);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            worker->error = 1;
            return NULL;
        }
        if (FD_ISSET(worker->stop_pipe[0], &read_set)) {
            break;
        }
        for (size_t i = 0; i < worker->count; ++i) {
            if (FD_ISSET(worker->fds[i], &read_set)) {
                if (discovery_listener_handle_readable(
                        worker->fds[i], &worker->devices[i]) < 0) {
                    worker->error = 1;
                    return NULL;
                }
            }
        }
    }

    return NULL;
}

static int start_worker(struct listener_worker *worker,
                        const enum device_type *types,
                        size_t count,
                        uint16_t discovery_port,
                        uint16_t base_tcp_port)
{
    memset(worker, 0, sizeof(*worker));
    worker->stop_pipe[0] = -1;
    worker->stop_pipe[1] = -1;
    for (size_t i = 0; i < MAX_TEST_LISTENERS; ++i) {
        worker->fds[i] = -1;
    }

    if (pipe(worker->stop_pipe) < 0) {
        perror("pipe");
        return -1;
    }

    for (size_t i = 0; i < count; ++i) {
        if (device_init_default(&worker->devices[i], types[i], NULL, NULL,
                                (uint16_t)(base_tcp_port + i)) < 0) {
            fprintf(stderr, "device_init_default failed in worker\n");
            goto fail;
        }
        worker->devices[i].discovery_port = discovery_port;
        worker->fds[i] = discovery_listener_open(discovery_port);
        if (worker->fds[i] < 0) {
            perror("discovery_listener_open");
            goto fail;
        }
        worker->count++;
    }
    return 0;

fail:
    for (size_t i = 0; i < worker->count; ++i) {
        close(worker->fds[i]);
    }
    close(worker->stop_pipe[0]);
    close(worker->stop_pipe[1]);
    return -1;
}

static void abort_worker(struct listener_worker *worker)
{
    for (size_t i = 0; i < worker->count; ++i) {
        close(worker->fds[i]);
    }
    close(worker->stop_pipe[0]);
    close(worker->stop_pipe[1]);
}

static int stop_worker(struct listener_worker *worker, pthread_t thread)
{
    char byte = 'x';
    ssize_t ignored = write(worker->stop_pipe[1], &byte, 1);

    (void)ignored;
    pthread_join(thread, NULL);
    abort_worker(worker);
    return worker->error ? -1 : 0;
}

static int test_packet_codec(void)
{
    char req[DISCOVERY_MAX_PACKET_SIZE + 1];
    size_t req_len = 0;
    char nonce[DISCOVERY_NONCE_MAX_LEN];
    struct aquacare_device pump;
    char resp[DISCOVERY_MAX_PACKET_SIZE + 1];
    size_t resp_len = 0;
    struct discovered_device parsed;
    char small_buf[12];
    char oversized[DISCOVERY_MAX_PACKET_SIZE + 10];
    static const char bare_req[] = "AQUACARE_SCAN|1\r\n";
    static const char crlf_resp[] =
        "AQUACARE_HERE|1|abc123|WATER_PUMP|PMP-1234|9010|1|Main Pump\r\n";
    static const char with_nul[] = "AQUACARE_SCAN|1|ab\0cd";

    if (discovery_build_scan_request("n1a2b3c4", req, sizeof(req), &req_len) < 0 ||
        strcmp(req, "AQUACARE_SCAN|1|n1a2b3c4") != 0 ||
        discovery_parse_scan_request(req, req_len, nonce, sizeof(nonce)) < 0 ||
        strcmp(nonce, "n1a2b3c4") != 0) {
        fprintf(stderr, "Scan request round-trip failed\n");
        return -1;
    }

    if (discovery_parse_scan_request(bare_req, sizeof(bare_req) - 1U,
                                     nonce, sizeof(nonce)) < 0 ||
        strcmp(nonce, "0") != 0) {
        fprintf(stderr, "Bare scan request with CRLF failed\n");
        return -1;
    }

    if (device_init_default(&pump, DEVICE_TYPE_WATER_PUMP, "PMP-TEST-01",
                            NULL, 9011U) < 0 ||
        discovery_build_scan_response(&pump, "n1a2b3c4",
                                      resp, sizeof(resp), &resp_len) < 0 ||
        discovery_parse_scan_response(resp, resp_len, "n1a2b3c4", &parsed) < 0 ||
        parsed.type != DEVICE_TYPE_WATER_PUMP ||
        strcmp(parsed.id, "PMP-TEST-01") != 0 ||
        parsed.tcp_port != 9011U ||
        parsed.is_on != false ||
        strcmp(parsed.name, "Water Pump") != 0) {
        fprintf(stderr, "Scan response round-trip failed\n");
        return -1;
    }

    if (discovery_parse_scan_response(crlf_resp, sizeof(crlf_resp) - 1U,
                                      "abc123", &parsed) < 0 ||
        parsed.is_on != true ||
        strcmp(parsed.name, "Main Pump") != 0) {
        fprintf(stderr, "CRLF scan response parsing failed\n");
        return -1;
    }

    /* Negative packet codec cases */
    memset(oversized, 'A', sizeof(oversized));
    static const char *const bad_requests[] = {
        "OTHER|1|abc",
        "AQUACARE_SCAN|2|abc",
        "AQUACARE_SCAN|1|"
    };
    if (discovery_build_scan_request("", req, sizeof(req), &req_len) == 0 ||
        discovery_build_scan_request("bad nonce", req, sizeof(req), &req_len) == 0 ||
        discovery_build_scan_request("abc", small_buf, sizeof(small_buf), &req_len) == 0 ||
        discovery_parse_scan_request("", 0U, nonce, sizeof(nonce)) == 0 ||
        discovery_parse_scan_request(with_nul, sizeof(with_nul) - 1U,
                                     nonce, sizeof(nonce)) == 0 ||
        discovery_parse_scan_request(oversized, sizeof(oversized),
                                     nonce, sizeof(nonce)) == 0) {
        fprintf(stderr, "Invalid scan request was wrongly accepted\n");
        return -1;
    }
    for (size_t i = 0; i < sizeof(bad_requests) / sizeof(bad_requests[0]); ++i) {
        if (discovery_parse_scan_request(bad_requests[i], strlen(bad_requests[i]),
                                         nonce, sizeof(nonce)) == 0) {
            fprintf(stderr, "Invalid scan request %zu was wrongly accepted\n", i);
            return -1;
        }
    }

    static const char *const bad_responses[] = {
        "AQUACARE_HERE|2|abc|WATER_PUMP|PMP-1|9000|0|Pump",
        "AQUACARE_HERE|1|abc|WATER_PUMP|PMP-1|70000|0|Pump",
        "AQUACARE_HERE|1|abc|WATER_PUMP|PMP-1|0|0|Pump",
        "AQUACARE_HERE|1|abc|WATER_PUMP|PMP-1|9000|2|Pump",
        "AQUACARE_HERE|1|abc|UNKNOWN|PMP-1|9000|0|Pump",
        "AQUACARE_HERE|1|abc|WATER_PUMP|PMP-1|9000|0|Pump|extra"
    };
    if (discovery_parse_scan_response(resp, resp_len, "wrong_nonce", &parsed) == 0) {
        fprintf(stderr, "Mismatched nonce response was wrongly accepted\n");
        return -1;
    }
    for (size_t i = 0; i < sizeof(bad_responses) / sizeof(bad_responses[0]); ++i) {
        if (discovery_parse_scan_response(bad_responses[i], strlen(bad_responses[i]),
                                          "abc", &parsed) == 0) {
            fprintf(stderr, "Invalid scan response %zu was wrongly accepted\n", i);
            return -1;
        }
    }

    return 0;
}

static int test_live_single_device_scan(void)
{
    static const enum device_type types[1] = { DEVICE_TYPE_ENV_SENSOR };
    struct listener_worker worker;
    pthread_t thread;
    struct discovered_device results[8];
    int found;

    if (start_worker(&worker, types, 1U, 19101U, 19010U) < 0) {
        return -1;
    }
    if (pthread_create(&thread, NULL, listener_thread_main, &worker) != 0) {
        fprintf(stderr, "pthread_create failed\n");
        abort_worker(&worker);
        return -1;
    }

    found = discovery_scan(19101U, 600, "127.0.0.1", results, 8U);
    if (stop_worker(&worker, thread) < 0 ||
        found != 1 ||
        results[0].type != DEVICE_TYPE_ENV_SENSOR ||
        strcmp(results[0].id, worker.devices[0].id) != 0 ||
        results[0].tcp_port != 19010U ||
        results[0].is_on != true ||
        strcmp(results[0].name, "Environment Sensor") != 0 ||
        strcmp(results[0].ip_address, "127.0.0.1") != 0) {
        fprintf(stderr, "Live single-device scan failed (found=%d)\n", found);
        return -1;
    }

    return 0;
}

static int test_live_five_devices_same_port_scan(void)
{
    static const enum device_type types[5] = {
        DEVICE_TYPE_ENV_SENSOR,
        DEVICE_TYPE_WATER_PUMP,
        DEVICE_TYPE_AERATOR,
        DEVICE_TYPE_PH_BALANCER,
        DEVICE_TYPE_AUTO_FEEDER
    };
    struct listener_worker worker;
    pthread_t thread;
    struct discovered_device results[16];
    int found;

    if (start_worker(&worker, types, 5U, 19102U, 9001U) < 0) {
        return -1;
    }
    if (pthread_create(&thread, NULL, listener_thread_main, &worker) != 0) {
        fprintf(stderr, "pthread_create failed\n");
        abort_worker(&worker);
        return -1;
    }

    found = discovery_scan(19102U, 800, NULL, results, 16U);
    if (stop_worker(&worker, thread) < 0 || found != 5) {
        fprintf(stderr, "5-device same-port scan failed (found=%d)\n", found);
        return -1;
    }

    for (size_t expected = 0; expected < 5U; ++expected) {
        int matched = 0;

        for (int r = 0; r < found; ++r) {
            if (strcmp(results[r].id, worker.devices[expected].id) == 0) {
                if (results[r].type == worker.devices[expected].type &&
                    results[r].tcp_port == worker.devices[expected].tcp_port &&
                    results[r].is_on == worker.devices[expected].is_on &&
                    strcmp(results[r].name, worker.devices[expected].name) == 0 &&
                    results[r].ip_address[0] != '\0') {
                    matched = 1;
                }
                break;
            }
        }
        if (!matched) {
            fprintf(stderr, "Missing or mismatched device %zu (%s)\n",
                    expected, worker.devices[expected].id);
            return -1;
        }
    }

    return 0;
}

static int test_malformed_packet_resilience_and_empty_timeout(void)
{
    static const enum device_type types[1] = { DEVICE_TYPE_AUTO_FEEDER };
    struct listener_worker worker;
    pthread_t thread;
    int raw_sock;
    struct sockaddr_in dest;
    static const char garbage[] = "NOT_AN_AQUACARE_PACKET|xyz";
    struct discovered_device results[4];
    int found;

    if (start_worker(&worker, types, 1U, 19103U, 19030U) < 0) {
        return -1;
    }
    if (pthread_create(&thread, NULL, listener_thread_main, &worker) != 0) {
        fprintf(stderr, "pthread_create failed\n");
        abort_worker(&worker);
        return -1;
    }

    raw_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (raw_sock < 0) {
        perror("socket");
        stop_worker(&worker, thread);
        return -1;
    }
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(19103U);
    inet_pton(AF_INET, "127.0.0.1", &dest.sin_addr);
    (void)sendto(raw_sock, garbage, sizeof(garbage) - 1U, 0,
                 (struct sockaddr *)&dest, sizeof(dest));
    close(raw_sock);

    found = discovery_scan(19103U, 500, "127.0.0.1", results, 4U);
    if (stop_worker(&worker, thread) < 0 ||
        found != 1 ||
        strcmp(results[0].id, worker.devices[0].id) != 0) {
        fprintf(stderr, "Malformed packet resilience test failed (found=%d)\n",
                found);
        return -1;
    }

    found = discovery_scan(19199U, 200, "127.0.0.1", results, 4U);
    if (found != 0) {
        fprintf(stderr, "Empty network timeout expected 0, got %d\n", found);
        return -1;
    }

    return 0;
}

int main(void)
{
    if (test_packet_codec() < 0 ||
        test_live_single_device_scan() < 0 ||
        test_live_five_devices_same_port_scan() < 0 ||
        test_malformed_packet_resilience_and_empty_timeout() < 0) {
        return EXIT_FAILURE;
    }

    puts("All device discovery tests passed.");
    return EXIT_SUCCESS;
}
