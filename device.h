#ifndef AQUACARE_DEVICE_H
#define AQUACARE_DEVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define DEVICE_ID_MAX_LEN 32
#define DEVICE_NAME_MAX_LEN 64
#define DEVICE_PASSWORD_MAX_LEN 64
#define DEVICE_MAX_SCHEDULES 8
#define DEVICE_MAX_FEED_TIMES 8
#define DEVICE_DEFAULT_PASSWORD "aquacare123"
#define DEVICE_DEFAULT_TCP_PORT 9000
#define DEVICE_DEFAULT_DISCOVERY_PORT 9100

enum device_type {
    DEVICE_TYPE_UNKNOWN = 0,
    DEVICE_TYPE_ENV_SENSOR = 1,
    DEVICE_TYPE_WATER_PUMP = 2,
    DEVICE_TYPE_AERATOR = 3,
    DEVICE_TYPE_PH_BALANCER = 4,
    DEVICE_TYPE_AUTO_FEEDER = 5
};

struct device_time_point {
    uint8_t hour;
    uint8_t minute;
};

struct device_time_window {
    struct device_time_point start;
    struct device_time_point end;
};

struct env_sensor_params {
    double salinity_ppt;
    double oxygen_mg_l;
    double ph;
};

struct water_pump_params {
    double flow_rate_m3_per_h;
    double duration_hours;
};

struct aerator_params {
    double speed_rpm;
    struct device_time_window schedules[DEVICE_MAX_SCHEDULES];
    size_t schedule_count;
};

struct ph_balancer_params {
    double lime_kg;
    double ph_min;
    double current_ph;
};

struct auto_feeder_params {
    double feed_kg;
    struct device_time_point feed_times[DEVICE_MAX_FEED_TIMES];
    size_t feed_time_count;
};

union device_params {
    struct env_sensor_params env_sensor;
    struct water_pump_params water_pump;
    struct aerator_params aerator;
    struct ph_balancer_params ph_balancer;
    struct auto_feeder_params auto_feeder;
};

struct aquacare_device {
    enum device_type type;
    char id[DEVICE_ID_MAX_LEN];
    char name[DEVICE_NAME_MAX_LEN];
    char password[DEVICE_PASSWORD_MAX_LEN];
    bool is_on;
    uint16_t tcp_port;
    uint16_t discovery_port;
    union device_params params;
};

const char *device_type_to_string(enum device_type type);
const char *device_type_display_name(enum device_type type);
const char *device_type_id_prefix(enum device_type type);
int device_type_from_string(const char *text, enum device_type *out_type);

int device_id_is_valid(const char *id);
int device_password_is_valid(const char *password);
int device_name_is_valid(const char *name);
int device_time_point_is_valid(struct device_time_point point);
int device_time_window_is_valid(struct device_time_window window);
int device_parse_time_point(const char *text, struct device_time_point *out_point);
int device_parse_time_window(const char *text, struct device_time_window *out_window);

int device_generate_id(enum device_type type, char *out_id, size_t out_size);
int device_init_default(struct aquacare_device *device,
                        enum device_type type,
                        const char *custom_id,
                        const char *custom_password,
                        uint16_t tcp_port);

int device_set_name(struct aquacare_device *device, const char *name);
int device_set_env_sensor_params(struct aquacare_device *device,
                                 double salinity_ppt,
                                 double oxygen_mg_l,
                                 double ph);
int device_set_water_pump_params(struct aquacare_device *device,
                                 double flow_rate_m3_per_h,
                                 double duration_hours);
int device_set_aerator_params(struct aquacare_device *device,
                              double speed_rpm,
                              const struct device_time_window *windows,
                              size_t window_count);
int device_set_ph_balancer_params(struct aquacare_device *device,
                                  double lime_kg,
                                  double ph_min,
                                  double current_ph);
int device_set_auto_feeder_params(struct aquacare_device *device,
                                  double feed_kg,
                                  const struct device_time_point *times,
                                  size_t time_count);

int device_validate(const struct aquacare_device *device);
int device_format_summary(const struct aquacare_device *device,
                          char *buffer,
                          size_t buffer_size);
void device_print_info(const struct aquacare_device *device, FILE *stream);

#endif
