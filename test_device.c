#define _POSIX_C_SOURCE 200809L
#include "device.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int doubles_close(double a, double b)
{
    return fabs(a - b) < 1e-9;
}

static int test_default_initialization(void)
{
    struct aquacare_device dev;

    if (device_init_default(&dev, DEVICE_TYPE_ENV_SENSOR, NULL, NULL, 0) < 0 ||
        dev.type != DEVICE_TYPE_ENV_SENSOR ||
        strncmp(dev.id, "SEN-", 4) != 0 ||
        strcmp(dev.password, DEVICE_DEFAULT_PASSWORD) != 0 ||
        !dev.is_on ||
        dev.tcp_port != DEVICE_DEFAULT_TCP_PORT ||
        dev.discovery_port != DEVICE_DEFAULT_DISCOVERY_PORT ||
        !doubles_close(dev.params.env_sensor.salinity_ppt, 20.0) ||
        !doubles_close(dev.params.env_sensor.oxygen_mg_l, 6.5) ||
        !doubles_close(dev.params.env_sensor.ph, 7.5) ||
        device_validate(&dev) < 0) {
        fprintf(stderr, "ENV_SENSOR default init failed\n");
        return -1;
    }

    if (device_init_default(&dev, DEVICE_TYPE_WATER_PUMP, NULL, NULL, 0) < 0 ||
        dev.type != DEVICE_TYPE_WATER_PUMP ||
        strncmp(dev.id, "PMP-", 4) != 0 ||
        strcmp(dev.password, DEVICE_DEFAULT_PASSWORD) != 0 ||
        dev.is_on ||
        dev.tcp_port != DEVICE_DEFAULT_TCP_PORT ||
        dev.discovery_port != DEVICE_DEFAULT_DISCOVERY_PORT ||
        !doubles_close(dev.params.water_pump.flow_rate_m3_per_h, 10.0) ||
        !doubles_close(dev.params.water_pump.duration_hours, 1.0) ||
        device_validate(&dev) < 0) {
        fprintf(stderr, "WATER_PUMP default init failed\n");
        return -1;
    }

    if (device_init_default(&dev, DEVICE_TYPE_AERATOR, NULL, NULL, 0) < 0 ||
        dev.type != DEVICE_TYPE_AERATOR ||
        strncmp(dev.id, "AER-", 4) != 0 ||
        strcmp(dev.password, DEVICE_DEFAULT_PASSWORD) != 0 ||
        dev.is_on ||
        !doubles_close(dev.params.aerator.speed_rpm, 120.0) ||
        dev.params.aerator.schedule_count != 2U ||
        dev.params.aerator.schedules[0].start.hour != 5U ||
        dev.params.aerator.schedules[0].end.hour != 7U ||
        dev.params.aerator.schedules[1].start.hour != 17U ||
        dev.params.aerator.schedules[1].end.hour != 19U ||
        device_validate(&dev) < 0) {
        fprintf(stderr, "AERATOR default init failed\n");
        return -1;
    }

    if (device_init_default(&dev, DEVICE_TYPE_PH_BALANCER, NULL, NULL, 0) < 0 ||
        dev.type != DEVICE_TYPE_PH_BALANCER ||
        strncmp(dev.id, "PHB-", 4) != 0 ||
        strcmp(dev.password, DEVICE_DEFAULT_PASSWORD) != 0 ||
        dev.is_on ||
        !doubles_close(dev.params.ph_balancer.lime_kg, 2.5) ||
        !doubles_close(dev.params.ph_balancer.ph_min, 7.0) ||
        !doubles_close(dev.params.ph_balancer.current_ph, 7.5) ||
        device_validate(&dev) < 0) {
        fprintf(stderr, "PH_BALANCER default init failed\n");
        return -1;
    }

    if (device_init_default(&dev, DEVICE_TYPE_AUTO_FEEDER, NULL, NULL, 0) < 0 ||
        dev.type != DEVICE_TYPE_AUTO_FEEDER ||
        strncmp(dev.id, "FDR-", 4) != 0 ||
        strcmp(dev.password, DEVICE_DEFAULT_PASSWORD) != 0 ||
        dev.is_on ||
        !doubles_close(dev.params.auto_feeder.feed_kg, 1.5) ||
        dev.params.auto_feeder.feed_time_count != 2U ||
        dev.params.auto_feeder.feed_times[0].hour != 7U ||
        dev.params.auto_feeder.feed_times[1].hour != 17U ||
        device_validate(&dev) < 0) {
        fprintf(stderr, "AUTO_FEEDER default init failed\n");
        return -1;
    }

    return 0;
}

static int test_unique_id_generation(void)
{
    enum device_type types[5] = {
        DEVICE_TYPE_ENV_SENSOR,
        DEVICE_TYPE_WATER_PUMP,
        DEVICE_TYPE_AERATOR,
        DEVICE_TYPE_PH_BALANCER,
        DEVICE_TYPE_AUTO_FEEDER
    };
    char ids[100][DEVICE_ID_MAX_LEN];

    for (size_t i = 0; i < 100; ++i) {
        enum device_type t = types[i % 5U];
        const char *prefix = device_type_id_prefix(t);

        if (device_generate_id(t, ids[i], sizeof(ids[i])) < 0 ||
            !device_id_is_valid(ids[i]) ||
            strncmp(ids[i], prefix, strlen(prefix)) != 0 ||
            ids[i][strlen(prefix)] != '-') {
            fprintf(stderr, "Failed generating valid ID at index %zu\n", i);
            return -1;
        }
        for (size_t j = 0; j < i; ++j) {
            if (strcmp(ids[i], ids[j]) == 0) {
                fprintf(stderr, "Duplicate generated ID: %s\n", ids[i]);
                return -1;
            }
        }
    }

    char tiny[8];
    if (device_generate_id(DEVICE_TYPE_ENV_SENSOR, tiny, sizeof(tiny)) == 0 ||
        errno != ENOSPC) {
        fprintf(stderr, "Small buffer should fail with ENOSPC\n");
        return -1;
    }
    if (device_generate_id(DEVICE_TYPE_UNKNOWN, ids[0], sizeof(ids[0])) == 0) {
        fprintf(stderr, "Unknown type ID generation should fail\n");
        return -1;
    }
    return 0;
}

static int test_custom_init_and_negatives(void)
{
    struct aquacare_device dev;

    if (device_init_default(&dev, DEVICE_TYPE_WATER_PUMP, "PMP-CUSTOM_01",
                            "secret#99", 9012U) < 0 ||
        strcmp(dev.id, "PMP-CUSTOM_01") != 0 ||
        strcmp(dev.password, "secret#99") != 0 ||
        dev.tcp_port != 9012U ||
        device_set_name(&dev, "Pond A Main Pump") < 0 ||
        strcmp(dev.name, "Pond A Main Pump") != 0 ||
        device_validate(&dev) < 0) {
        fprintf(stderr, "Custom initialization failed\n");
        return -1;
    }

    if (device_init_default(NULL, DEVICE_TYPE_WATER_PUMP, NULL, NULL, 0) == 0 ||
        device_init_default(&dev, DEVICE_TYPE_UNKNOWN, NULL, NULL, 0) == 0 ||
        device_init_default(&dev, DEVICE_TYPE_WATER_PUMP, "", NULL, 0) == 0 ||
        device_init_default(&dev, DEVICE_TYPE_WATER_PUMP, "BAD ID", NULL, 0) == 0 ||
        device_init_default(&dev, DEVICE_TYPE_WATER_PUMP, "BAD|ID", NULL, 0) == 0 ||
        device_init_default(&dev, DEVICE_TYPE_WATER_PUMP, NULL, "", 0) == 0 ||
        device_init_default(&dev, DEVICE_TYPE_WATER_PUMP, NULL, "bad pass", 0) == 0 ||
        device_init_default(&dev, DEVICE_TYPE_WATER_PUMP, NULL, "bad|pass", 0) == 0 ||
        device_init_default(&dev, DEVICE_TYPE_WATER_PUMP, NULL, "bad\npass", 0) == 0 ||
        device_set_name(&dev, "") == 0 ||
        device_set_name(&dev, "bad|name") == 0 ||
        device_set_name(NULL, "ok") == 0) {
        fprintf(stderr, "Negative validation accepted invalid input\n");
        return -1;
    }

    return 0;
}

static int test_time_parsing_and_setters(void)
{
    struct device_time_point tp;
    struct device_time_window tw;
    struct aquacare_device dev;
    double nan_val = NAN;
    double inf_val = INFINITY;

    if (device_parse_time_point("00:00", &tp) < 0 || tp.hour != 0 || tp.minute != 0 ||
        device_parse_time_point("23:59", &tp) < 0 || tp.hour != 23 || tp.minute != 59 ||
        device_parse_time_point("24:00", &tp) == 0 ||
        device_parse_time_point("12:60", &tp) == 0 ||
        device_parse_time_point("7:00", &tp) == 0 ||
        device_parse_time_point("12-00", &tp) == 0 ||
        device_parse_time_point(NULL, &tp) == 0 ||
        device_parse_time_point("12:00", NULL) == 0) {
        fprintf(stderr, "device_parse_time_point test failed\n");
        return -1;
    }

    if (device_parse_time_window("06:30-08:45", &tw) < 0 ||
        tw.start.hour != 6 || tw.start.minute != 30 ||
        tw.end.hour != 8 || tw.end.minute != 45 ||
        device_parse_time_window("10:00-10:00", &tw) == 0 ||
        device_parse_time_window("15:00-14:00", &tw) == 0 ||
        device_parse_time_window("10:00/12:00", &tw) == 0 ||
        device_parse_time_window("10:00-12:000", &tw) == 0 ||
        device_parse_time_window(NULL, &tw) == 0) {
        fprintf(stderr, "device_parse_time_window test failed\n");
        return -1;
    }

    /* ENV_SENSOR */
    if (device_init_default(&dev, DEVICE_TYPE_ENV_SENSOR, NULL, NULL, 9001U) < 0 ||
        device_set_env_sensor_params(&dev, 32.5, 7.8, 8.1) < 0 ||
        device_set_water_pump_params(&dev, 5.0, 1.0) == 0 ||
        device_set_env_sensor_params(&dev, -1.0, 7.0, 7.0) == 0 ||
        device_set_env_sensor_params(&dev, 20.0, 25.0, 7.0) == 0 ||
        device_set_env_sensor_params(&dev, 20.0, 7.0, 15.0) == 0 ||
        device_set_env_sensor_params(&dev, nan_val, 7.0, 7.0) == 0 ||
        device_set_env_sensor_params(&dev, 20.0, inf_val, 7.0) == 0 ||
        device_validate(&dev) < 0) {
        fprintf(stderr, "ENV_SENSOR setter validation failed\n");
        return -1;
    }

    /* WATER_PUMP */
    if (device_init_default(&dev, DEVICE_TYPE_WATER_PUMP, NULL, NULL, 9002U) < 0 ||
        device_set_water_pump_params(&dev, 25.0, 3.5) < 0 ||
        device_set_env_sensor_params(&dev, 20.0, 6.5, 7.5) == 0 ||
        device_set_water_pump_params(&dev, 0.0, 1.0) == 0 ||
        device_set_water_pump_params(&dev, 10.0, 0.0) == 0 ||
        device_set_water_pump_params(&dev, 10001.0, 1.0) == 0 ||
        device_set_water_pump_params(&dev, 10.0, 169.0) == 0 ||
        device_set_water_pump_params(&dev, inf_val, 1.0) == 0 ||
        device_validate(&dev) < 0) {
        fprintf(stderr, "WATER_PUMP setter validation failed\n");
        return -1;
    }

    /* AERATOR */
    struct device_time_window bad_win = { { 18U, 0U }, { 16U, 0U } };
    struct device_time_window good_wins[2] = {
        { { 4U, 30U }, { 6U, 30U } },
        { { 18U, 0U }, { 21U, 0U } }
    };
    if (device_init_default(&dev, DEVICE_TYPE_AERATOR, NULL, NULL, 9003U) < 0 ||
        device_set_aerator_params(&dev, 150.0, good_wins, 2U) < 0 ||
        device_set_auto_feeder_params(&dev, 1.0, NULL, 1U) == 0 ||
        device_set_aerator_params(&dev, 0.0, good_wins, 1U) == 0 ||
        device_set_aerator_params(&dev, nan_val, good_wins, 1U) == 0 ||
        device_set_aerator_params(&dev, 100.0, good_wins, 0U) == 0 ||
        device_set_aerator_params(&dev, 100.0, good_wins, DEVICE_MAX_SCHEDULES + 1U) == 0 ||
        device_set_aerator_params(&dev, 100.0, &bad_win, 1U) == 0 ||
        device_validate(&dev) < 0) {
        fprintf(stderr, "AERATOR setter validation failed\n");
        return -1;
    }

    /* PH_BALANCER */
    if (device_init_default(&dev, DEVICE_TYPE_PH_BALANCER, NULL, NULL, 9004U) < 0 ||
        device_set_ph_balancer_params(&dev, 3.2, 6.8, 7.2) < 0 ||
        device_set_aerator_params(&dev, 120.0, good_wins, 1U) == 0 ||
        device_set_ph_balancer_params(&dev, 0.0, 6.8, 7.2) == 0 ||
        device_set_ph_balancer_params(&dev, 3.2, 0.0, 7.2) == 0 ||
        device_set_ph_balancer_params(&dev, 3.2, 14.5, 7.2) == 0 ||
        device_set_ph_balancer_params(&dev, 3.2, 7.0, -0.5) == 0 ||
        device_set_ph_balancer_params(&dev, nan_val, 7.0, 7.2) == 0 ||
        device_validate(&dev) < 0) {
        fprintf(stderr, "PH_BALANCER setter validation failed\n");
        return -1;
    }

    /* AUTO_FEEDER */
    struct device_time_point bad_tp = { 24U, 0U };
    struct device_time_point good_tps[3] = {
        { 6U, 0U }, { 12U, 0U }, { 18U, 30U }
    };
    if (device_init_default(&dev, DEVICE_TYPE_AUTO_FEEDER, NULL, NULL, 9005U) < 0 ||
        device_set_auto_feeder_params(&dev, 2.25, good_tps, 3U) < 0 ||
        device_set_ph_balancer_params(&dev, 2.0, 7.0, 7.5) == 0 ||
        device_set_auto_feeder_params(&dev, 0.0, good_tps, 1U) == 0 ||
        device_set_auto_feeder_params(&dev, inf_val, good_tps, 1U) == 0 ||
        device_set_auto_feeder_params(&dev, 2.0, good_tps, 0U) == 0 ||
        device_set_auto_feeder_params(&dev, 2.0, good_tps, DEVICE_MAX_FEED_TIMES + 1U) == 0 ||
        device_set_auto_feeder_params(&dev, 2.0, &bad_tp, 1U) == 0 ||
        device_validate(&dev) < 0) {
        fprintf(stderr, "AUTO_FEEDER setter validation failed\n");
        return -1;
    }

    return 0;
}

static int test_summary_and_type_parsing(void)
{
    enum device_type types[5] = {
        DEVICE_TYPE_ENV_SENSOR,
        DEVICE_TYPE_WATER_PUMP,
        DEVICE_TYPE_AERATOR,
        DEVICE_TYPE_PH_BALANCER,
        DEVICE_TYPE_AUTO_FEEDER
    };
    const char *expected_tokens[5] = {
        "Salinity",
        "Flow Rate (V)",
        "Speed (C)",
        "Lime (Wca)",
        "Feed (Wf)"
    };
    char summary[1024];
    char small_buf[16];
    struct aquacare_device dev;

    for (size_t i = 0; i < 5; ++i) {
        if (device_init_default(&dev, types[i], NULL, "TopSecret99!", (uint16_t)(9001U + i)) < 0 ||
            device_format_summary(&dev, summary, sizeof(summary)) < 0 ||
            strstr(summary, device_type_to_string(types[i])) == NULL ||
            strstr(summary, dev.id) == NULL ||
            strstr(summary, expected_tokens[i]) == NULL ||
            strstr(summary, "TopSecret99!") != NULL) {
            fprintf(stderr, "Summary test failed for type %zu\n", i);
            return -1;
        }
    }

    if (device_format_summary(&dev, small_buf, sizeof(small_buf)) == 0 ||
        errno != ENOSPC) {
        fprintf(stderr, "Small summary buffer should fail with ENOSPC\n");
        return -1;
    }

    enum device_type parsed = DEVICE_TYPE_UNKNOWN;
    if (device_type_from_string("env_sensor", &parsed) < 0 || parsed != DEVICE_TYPE_ENV_SENSOR ||
        device_type_from_string("SENSOR", &parsed) < 0 || parsed != DEVICE_TYPE_ENV_SENSOR ||
        device_type_from_string("water_pump", &parsed) < 0 || parsed != DEVICE_TYPE_WATER_PUMP ||
        device_type_from_string("PuMp", &parsed) < 0 || parsed != DEVICE_TYPE_WATER_PUMP ||
        device_type_from_string("Aerator", &parsed) < 0 || parsed != DEVICE_TYPE_AERATOR ||
        device_type_from_string("ph_balancer", &parsed) < 0 || parsed != DEVICE_TYPE_PH_BALANCER ||
        device_type_from_string("pH", &parsed) < 0 || parsed != DEVICE_TYPE_PH_BALANCER ||
        device_type_from_string("auto_feeder", &parsed) < 0 || parsed != DEVICE_TYPE_AUTO_FEEDER ||
        device_type_from_string("feeder", &parsed) < 0 || parsed != DEVICE_TYPE_AUTO_FEEDER ||
        device_type_from_string("", &parsed) == 0 ||
        device_type_from_string("robot", &parsed) == 0) {
        fprintf(stderr, "device_type_from_string test failed\n");
        return -1;
    }

    return 0;
}

int main(void)
{
    if (test_default_initialization() < 0 ||
        test_unique_id_generation() < 0 ||
        test_custom_init_and_negatives() < 0 ||
        test_time_parsing_and_setters() < 0 ||
        test_summary_and_type_parsing() < 0) {
        return EXIT_FAILURE;
    }

    puts("All device initialization tests passed.");
    return EXIT_SUCCESS;
}
