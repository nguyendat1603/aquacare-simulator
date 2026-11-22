#define _POSIX_C_SOURCE 200809L
#include "device.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int ascii_strcasecmp(const char *lhs, const char *rhs)
{
    while (*lhs != '\0' && *rhs != '\0') {
        unsigned char a = (unsigned char)tolower((unsigned char)*lhs);
        unsigned char b = (unsigned char)tolower((unsigned char)*rhs);

        if (a != b) {
            return (int)a - (int)b;
        }
        ++lhs;
        ++rhs;
    }
    return (int)(unsigned char)tolower((unsigned char)*lhs) -
           (int)(unsigned char)tolower((unsigned char)*rhs);
}

static int is_known_device_type(enum device_type type)
{
    return type >= DEVICE_TYPE_ENV_SENSOR && type <= DEVICE_TYPE_AUTO_FEEDER;
}

static int is_finite_closed_range(double value, double min_val, double max_val)
{
    return isfinite(value) && value >= min_val && value <= max_val;
}

static int is_finite_open_low_range(double value, double min_exclusive,
                                    double max_inclusive)
{
    return isfinite(value) && value > min_exclusive && value <= max_inclusive;
}

static int copy_bounded_string(char *dst, size_t dst_size, const char *src)
{
    size_t len;

    if (dst == NULL || src == NULL || dst_size == 0) {
        return -1;
    }
    len = strlen(src);
    if (len >= dst_size) {
        return -1;
    }
    memcpy(dst, src, len + 1U);
    return 0;
}

static int append_fmt(char *buffer, size_t buffer_size, size_t *offset,
                      const char *format, ...)
{
    va_list args;
    int written;

    if (*offset >= buffer_size) {
        errno = ENOSPC;
        return -1;
    }
    va_start(args, format);
    written = vsnprintf(buffer + *offset, buffer_size - *offset, format, args);
    va_end(args);

    if (written < 0) {
        errno = EINVAL;
        return -1;
    }
    if ((size_t)written >= buffer_size - *offset) {
        errno = ENOSPC;
        return -1;
    }
    *offset += (size_t)written;
    return 0;
}

const char *device_type_to_string(enum device_type type)
{
    switch (type) {
    case DEVICE_TYPE_ENV_SENSOR:
        return "ENV_SENSOR";
    case DEVICE_TYPE_WATER_PUMP:
        return "WATER_PUMP";
    case DEVICE_TYPE_AERATOR:
        return "AERATOR";
    case DEVICE_TYPE_PH_BALANCER:
        return "PH_BALANCER";
    case DEVICE_TYPE_AUTO_FEEDER:
        return "AUTO_FEEDER";
    default:
        return "UNKNOWN";
    }
}

const char *device_type_display_name(enum device_type type)
{
    switch (type) {
    case DEVICE_TYPE_ENV_SENSOR:
        return "Environment Sensor";
    case DEVICE_TYPE_WATER_PUMP:
        return "Water Pump";
    case DEVICE_TYPE_AERATOR:
        return "Surface Aerator";
    case DEVICE_TYPE_PH_BALANCER:
        return "pH Balancer";
    case DEVICE_TYPE_AUTO_FEEDER:
        return "Auto Feeder";
    default:
        return "Unknown Device";
    }
}

const char *device_type_id_prefix(enum device_type type)
{
    switch (type) {
    case DEVICE_TYPE_ENV_SENSOR:
        return "SEN";
    case DEVICE_TYPE_WATER_PUMP:
        return "PMP";
    case DEVICE_TYPE_AERATOR:
        return "AER";
    case DEVICE_TYPE_PH_BALANCER:
        return "PHB";
    case DEVICE_TYPE_AUTO_FEEDER:
        return "FDR";
    default:
        return NULL;
    }
}

int device_type_from_string(const char *text, enum device_type *out_type)
{
    if (text == NULL || out_type == NULL || text[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    if (ascii_strcasecmp(text, "ENV_SENSOR") == 0 ||
        ascii_strcasecmp(text, "sensor") == 0) {
        *out_type = DEVICE_TYPE_ENV_SENSOR;
        return 0;
    }
    if (ascii_strcasecmp(text, "WATER_PUMP") == 0 ||
        ascii_strcasecmp(text, "pump") == 0) {
        *out_type = DEVICE_TYPE_WATER_PUMP;
        return 0;
    }
    if (ascii_strcasecmp(text, "AERATOR") == 0) {
        *out_type = DEVICE_TYPE_AERATOR;
        return 0;
    }
    if (ascii_strcasecmp(text, "PH_BALANCER") == 0 ||
        ascii_strcasecmp(text, "ph") == 0) {
        *out_type = DEVICE_TYPE_PH_BALANCER;
        return 0;
    }
    if (ascii_strcasecmp(text, "AUTO_FEEDER") == 0 ||
        ascii_strcasecmp(text, "feeder") == 0) {
        *out_type = DEVICE_TYPE_AUTO_FEEDER;
        return 0;
    }

    errno = EINVAL;
    return -1;
}

int device_id_is_valid(const char *id)
{
    size_t len;

    if (id == NULL) {
        return 0;
    }
    len = strlen(id);
    if (len == 0 || len >= DEVICE_ID_MAX_LEN) {
        return 0;
    }
    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)id[i];

        if (!(isalnum(ch) || ch == '_' || ch == '-')) {
            return 0;
        }
    }
    return 1;
}

int device_password_is_valid(const char *password)
{
    size_t len;

    if (password == NULL) {
        return 0;
    }
    len = strlen(password);
    if (len == 0 || len >= DEVICE_PASSWORD_MAX_LEN) {
        return 0;
    }
    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)password[i];

        if (ch < 0x21U || ch > 0x7EU || ch == '|') {
            return 0;
        }
    }
    return 1;
}

int device_name_is_valid(const char *name)
{
    size_t len;

    if (name == NULL) {
        return 0;
    }
    len = strlen(name);
    if (len == 0 || len >= DEVICE_NAME_MAX_LEN) {
        return 0;
    }
    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)name[i];

        if (ch < 0x20U || ch > 0x7EU || ch == '|') {
            return 0;
        }
    }
    return 1;
}

int device_time_point_is_valid(struct device_time_point point)
{
    return point.hour <= 23U && point.minute <= 59U;
}

int device_time_window_is_valid(struct device_time_window window)
{
    unsigned int start_minutes;
    unsigned int end_minutes;

    if (!device_time_point_is_valid(window.start) ||
        !device_time_point_is_valid(window.end)) {
        return 0;
    }
    start_minutes = (unsigned int)window.start.hour * 60U +
                    (unsigned int)window.start.minute;
    end_minutes = (unsigned int)window.end.hour * 60U +
                  (unsigned int)window.end.minute;
    return start_minutes < end_minutes;
}

int device_parse_time_point(const char *text, struct device_time_point *out_point)
{
    unsigned int hour;
    unsigned int minute;

    if (text == NULL || out_point == NULL || strlen(text) != 5U) {
        errno = EINVAL;
        return -1;
    }
    if (!isdigit((unsigned char)text[0]) || !isdigit((unsigned char)text[1]) ||
        text[2] != ':' ||
        !isdigit((unsigned char)text[3]) || !isdigit((unsigned char)text[4])) {
        errno = EINVAL;
        return -1;
    }

    hour = (unsigned int)(text[0] - '0') * 10U + (unsigned int)(text[1] - '0');
    minute = (unsigned int)(text[3] - '0') * 10U + (unsigned int)(text[4] - '0');
    if (hour > 23U || minute > 59U) {
        errno = EINVAL;
        return -1;
    }

    out_point->hour = (uint8_t)hour;
    out_point->minute = (uint8_t)minute;
    return 0;
}

int device_parse_time_window(const char *text, struct device_time_window *out_window)
{
    char start_buf[6];
    struct device_time_window window;

    if (text == NULL || out_window == NULL || strlen(text) != 11U ||
        text[5] != '-') {
        errno = EINVAL;
        return -1;
    }

    memcpy(start_buf, text, 5U);
    start_buf[5] = '\0';
    if (device_parse_time_point(start_buf, &window.start) < 0 ||
        device_parse_time_point(text + 6, &window.end) < 0 ||
        !device_time_window_is_valid(window)) {
        errno = EINVAL;
        return -1;
    }

    *out_window = window;
    return 0;
}

static uint32_t read_random_u32(void)
{
    static uint32_t fallback_counter = 0;
    uint32_t value = 0;
    int fd = open("/dev/urandom", O_RDONLY);

    if (fd >= 0) {
        size_t total = 0;
        unsigned char *buf = (unsigned char *)&value;

        while (total < sizeof(value)) {
            ssize_t n = read(fd, buf + total, sizeof(value) - total);

            if (n > 0) {
                total += (size_t)n;
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else {
                break;
            }
        }
        close(fd);
        if (total == sizeof(value)) {
            return value;
        }
    }

    ++fallback_counter;
    uint32_t mixed = (uint32_t)time(NULL) ^
                     ((uint32_t)getpid() * 0x9E3779B1U) ^
                     (fallback_counter * 0x85EBCA6BU);
    mixed ^= mixed >> 16;
    mixed *= 0x7FEB352DU;
    mixed ^= mixed >> 15;
    return mixed;
}

int device_generate_id(enum device_type type, char *out_id, size_t out_size)
{
    static uint32_t sequence = 0;
    const char *prefix = device_type_id_prefix(type);
    uint32_t rand_bits;
    int written;

    if (prefix == NULL || out_id == NULL || out_size == 0) {
        errno = EINVAL;
        return -1;
    }

    sequence = (sequence + 1U) & 0xFFFFU;
    rand_bits = read_random_u32();
    written = snprintf(out_id, out_size, "%s-%08X-%04X",
                       prefix, rand_bits, (unsigned int)sequence);
    if (written < 0 || (size_t)written >= out_size) {
        errno = ENOSPC;
        return -1;
    }
    return 0;
}

static void set_type_defaults(struct aquacare_device *device)
{
    switch (device->type) {
    case DEVICE_TYPE_ENV_SENSOR:
        device->is_on = true;
        device->params.env_sensor.salinity_ppt = 20.0;
        device->params.env_sensor.oxygen_mg_l = 6.5;
        device->params.env_sensor.ph = 7.5;
        break;
    case DEVICE_TYPE_WATER_PUMP:
        device->is_on = false;
        device->params.water_pump.flow_rate_m3_per_h = 10.0;
        device->params.water_pump.duration_hours = 1.0;
        break;
    case DEVICE_TYPE_AERATOR:
        device->is_on = false;
        device->params.aerator.speed_rpm = 120.0;
        device->params.aerator.schedule_count = 2U;
        device->params.aerator.schedules[0] = (struct device_time_window){
            { 5U, 0U }, { 7U, 0U }
        };
        device->params.aerator.schedules[1] = (struct device_time_window){
            { 17U, 0U }, { 19U, 0U }
        };
        break;
    case DEVICE_TYPE_PH_BALANCER:
        device->is_on = false;
        device->params.ph_balancer.lime_kg = 2.5;
        device->params.ph_balancer.ph_min = 7.0;
        device->params.ph_balancer.current_ph = 7.5;
        break;
    case DEVICE_TYPE_AUTO_FEEDER:
        device->is_on = false;
        device->params.auto_feeder.feed_kg = 1.5;
        device->params.auto_feeder.feed_time_count = 2U;
        device->params.auto_feeder.feed_times[0] = (struct device_time_point){
            7U, 0U
        };
        device->params.auto_feeder.feed_times[1] = (struct device_time_point){
            17U, 0U
        };
        break;
    default:
        break;
    }
}

int device_init_default(struct aquacare_device *device,
                        enum device_type type,
                        const char *custom_id,
                        const char *custom_password,
                        uint16_t tcp_port)
{
    if (device == NULL || !is_known_device_type(type)) {
        errno = EINVAL;
        return -1;
    }

    if (custom_id != NULL && !device_id_is_valid(custom_id)) {
        errno = EINVAL;
        return -1;
    }
    if (custom_password != NULL && !device_password_is_valid(custom_password)) {
        errno = EINVAL;
        return -1;
    }

    memset(device, 0, sizeof(*device));
    device->type = type;
    device->tcp_port = (tcp_port == 0) ? DEVICE_DEFAULT_TCP_PORT : tcp_port;
    device->discovery_port = DEVICE_DEFAULT_DISCOVERY_PORT;

    if (custom_id != NULL) {
        copy_bounded_string(device->id, sizeof(device->id), custom_id);
    } else if (device_generate_id(type, device->id, sizeof(device->id)) < 0) {
        return -1;
    }

    copy_bounded_string(device->name, sizeof(device->name),
                        device_type_display_name(type));
    copy_bounded_string(device->password, sizeof(device->password),
                        custom_password != NULL ? custom_password :
                                                  DEVICE_DEFAULT_PASSWORD);
    set_type_defaults(device);
    return 0;
}

int device_set_name(struct aquacare_device *device, const char *name)
{
    if (device == NULL || !device_name_is_valid(name)) {
        errno = EINVAL;
        return -1;
    }
    return copy_bounded_string(device->name, sizeof(device->name), name);
}

int device_set_env_sensor_params(struct aquacare_device *device,
                                 double salinity_ppt,
                                 double oxygen_mg_l,
                                 double ph)
{
    if (device == NULL || device->type != DEVICE_TYPE_ENV_SENSOR ||
        !is_finite_closed_range(salinity_ppt, 0.0, 50.0) ||
        !is_finite_closed_range(oxygen_mg_l, 0.0, 20.0) ||
        !is_finite_closed_range(ph, 0.0, 14.0)) {
        errno = EINVAL;
        return -1;
    }
    device->params.env_sensor.salinity_ppt = salinity_ppt;
    device->params.env_sensor.oxygen_mg_l = oxygen_mg_l;
    device->params.env_sensor.ph = ph;
    return 0;
}

int device_set_water_pump_params(struct aquacare_device *device,
                                 double flow_rate_m3_per_h,
                                 double duration_hours)
{
    if (device == NULL || device->type != DEVICE_TYPE_WATER_PUMP ||
        !is_finite_open_low_range(flow_rate_m3_per_h, 0.0, 10000.0) ||
        !is_finite_open_low_range(duration_hours, 0.0, 168.0)) {
        errno = EINVAL;
        return -1;
    }
    device->params.water_pump.flow_rate_m3_per_h = flow_rate_m3_per_h;
    device->params.water_pump.duration_hours = duration_hours;
    return 0;
}

int device_set_aerator_params(struct aquacare_device *device,
                              double speed_rpm,
                              const struct device_time_window *windows,
                              size_t window_count)
{
    if (device == NULL || device->type != DEVICE_TYPE_AERATOR ||
        !is_finite_open_low_range(speed_rpm, 0.0, 10000.0) ||
        windows == NULL || window_count == 0U ||
        window_count > DEVICE_MAX_SCHEDULES) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < window_count; ++i) {
        if (!device_time_window_is_valid(windows[i])) {
            errno = EINVAL;
            return -1;
        }
    }

    device->params.aerator.speed_rpm = speed_rpm;
    device->params.aerator.schedule_count = window_count;
    memset(device->params.aerator.schedules, 0,
           sizeof(device->params.aerator.schedules));
    memcpy(device->params.aerator.schedules, windows,
           window_count * sizeof(windows[0]));
    return 0;
}

int device_set_ph_balancer_params(struct aquacare_device *device,
                                  double lime_kg,
                                  double ph_min,
                                  double current_ph)
{
    if (device == NULL || device->type != DEVICE_TYPE_PH_BALANCER ||
        !is_finite_open_low_range(lime_kg, 0.0, 1000.0) ||
        !is_finite_open_low_range(ph_min, 0.0, 14.0) ||
        !is_finite_closed_range(current_ph, 0.0, 14.0)) {
        errno = EINVAL;
        return -1;
    }
    device->params.ph_balancer.lime_kg = lime_kg;
    device->params.ph_balancer.ph_min = ph_min;
    device->params.ph_balancer.current_ph = current_ph;
    return 0;
}

int device_set_auto_feeder_params(struct aquacare_device *device,
                                  double feed_kg,
                                  const struct device_time_point *times,
                                  size_t time_count)
{
    if (device == NULL || device->type != DEVICE_TYPE_AUTO_FEEDER ||
        !is_finite_open_low_range(feed_kg, 0.0, 1000.0) ||
        times == NULL || time_count == 0U ||
        time_count > DEVICE_MAX_FEED_TIMES) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < time_count; ++i) {
        if (!device_time_point_is_valid(times[i])) {
            errno = EINVAL;
            return -1;
        }
    }

    device->params.auto_feeder.feed_kg = feed_kg;
    device->params.auto_feeder.feed_time_count = time_count;
    memset(device->params.auto_feeder.feed_times, 0,
           sizeof(device->params.auto_feeder.feed_times));
    memcpy(device->params.auto_feeder.feed_times, times,
           time_count * sizeof(times[0]));
    return 0;
}

int device_validate(const struct aquacare_device *device)
{
    if (device == NULL || !is_known_device_type(device->type) ||
        !device_id_is_valid(device->id) ||
        !device_name_is_valid(device->name) ||
        !device_password_is_valid(device->password) ||
        device->tcp_port == 0U || device->discovery_port == 0U) {
        errno = EINVAL;
        return -1;
    }

    switch (device->type) {
    case DEVICE_TYPE_ENV_SENSOR:
        if (!is_finite_closed_range(device->params.env_sensor.salinity_ppt,
                                    0.0, 50.0) ||
            !is_finite_closed_range(device->params.env_sensor.oxygen_mg_l,
                                    0.0, 20.0) ||
            !is_finite_closed_range(device->params.env_sensor.ph, 0.0, 14.0)) {
            errno = EINVAL;
            return -1;
        }
        return 0;

    case DEVICE_TYPE_WATER_PUMP:
        if (!is_finite_open_low_range(
                device->params.water_pump.flow_rate_m3_per_h, 0.0, 10000.0) ||
            !is_finite_open_low_range(
                device->params.water_pump.duration_hours, 0.0, 168.0)) {
            errno = EINVAL;
            return -1;
        }
        return 0;

    case DEVICE_TYPE_AERATOR:
        if (!is_finite_open_low_range(device->params.aerator.speed_rpm,
                                      0.0, 10000.0) ||
            device->params.aerator.schedule_count == 0U ||
            device->params.aerator.schedule_count > DEVICE_MAX_SCHEDULES) {
            errno = EINVAL;
            return -1;
        }
        for (size_t i = 0; i < device->params.aerator.schedule_count; ++i) {
            if (!device_time_window_is_valid(
                    device->params.aerator.schedules[i])) {
                errno = EINVAL;
                return -1;
            }
        }
        return 0;

    case DEVICE_TYPE_PH_BALANCER:
        if (!is_finite_open_low_range(device->params.ph_balancer.lime_kg,
                                      0.0, 1000.0) ||
            !is_finite_open_low_range(device->params.ph_balancer.ph_min,
                                      0.0, 14.0) ||
            !is_finite_closed_range(device->params.ph_balancer.current_ph,
                                    0.0, 14.0)) {
            errno = EINVAL;
            return -1;
        }
        return 0;

    case DEVICE_TYPE_AUTO_FEEDER:
        if (!is_finite_open_low_range(device->params.auto_feeder.feed_kg,
                                      0.0, 1000.0) ||
            device->params.auto_feeder.feed_time_count == 0U ||
            device->params.auto_feeder.feed_time_count > DEVICE_MAX_FEED_TIMES) {
            errno = EINVAL;
            return -1;
        }
        for (size_t i = 0; i < device->params.auto_feeder.feed_time_count; ++i) {
            if (!device_time_point_is_valid(
                    device->params.auto_feeder.feed_times[i])) {
                errno = EINVAL;
                return -1;
            }
        }
        return 0;

    default:
        errno = EINVAL;
        return -1;
    }
}

int device_format_summary(const struct aquacare_device *device,
                          char *buffer,
                          size_t buffer_size)
{
    size_t offset = 0;

    if (device == NULL || buffer == NULL || buffer_size == 0 ||
        device_validate(device) < 0) {
        errno = EINVAL;
        return -1;
    }

    if (append_fmt(buffer, buffer_size, &offset,
                   "Device: %s (%s)\n"
                   "  ID             : %s\n"
                   "  Name           : %s\n"
                   "  State          : %s\n"
                   "  TCP Port       : %u\n"
                   "  Discovery Port : %u\n",
                   device_type_to_string(device->type),
                   device_type_display_name(device->type),
                   device->id,
                   device->name,
                   device->is_on ? "ON" : "OFF",
                   (unsigned int)device->tcp_port,
                   (unsigned int)device->discovery_port) < 0) {
        return -1;
    }

    switch (device->type) {
    case DEVICE_TYPE_ENV_SENSOR:
        return append_fmt(buffer, buffer_size, &offset,
                          "  Salinity       : %.2f ppt\n"
                          "  Oxygen         : %.2f mg/L\n"
                          "  pH             : %.2f\n",
                          device->params.env_sensor.salinity_ppt,
                          device->params.env_sensor.oxygen_mg_l,
                          device->params.env_sensor.ph);

    case DEVICE_TYPE_WATER_PUMP:
        return append_fmt(buffer, buffer_size, &offset,
                          "  Flow Rate (V)  : %.2f m3/h\n"
                          "  Duration (T)   : %.2f h\n",
                          device->params.water_pump.flow_rate_m3_per_h,
                          device->params.water_pump.duration_hours);

    case DEVICE_TYPE_AERATOR:
        if (append_fmt(buffer, buffer_size, &offset,
                       "  Speed (C)      : %.2f rpm\n"
                       "  Schedules      : ",
                       device->params.aerator.speed_rpm) < 0) {
            return -1;
        }
        for (size_t i = 0; i < device->params.aerator.schedule_count; ++i) {
            const struct device_time_window *w =
                &device->params.aerator.schedules[i];

            if (append_fmt(buffer, buffer_size, &offset,
                           "%s%02u:%02u-%02u:%02u",
                           i > 0 ? ", " : "",
                           (unsigned int)w->start.hour,
                           (unsigned int)w->start.minute,
                           (unsigned int)w->end.hour,
                           (unsigned int)w->end.minute) < 0) {
                return -1;
            }
        }
        return append_fmt(buffer, buffer_size, &offset, "\n");

    case DEVICE_TYPE_PH_BALANCER:
        return append_fmt(buffer, buffer_size, &offset,
                          "  Lime (Wca)     : %.2f kg\n"
                          "  Min pH (pHmin) : %.2f\n"
                          "  Current pH     : %.2f\n",
                          device->params.ph_balancer.lime_kg,
                          device->params.ph_balancer.ph_min,
                          device->params.ph_balancer.current_ph);

    case DEVICE_TYPE_AUTO_FEEDER:
        if (append_fmt(buffer, buffer_size, &offset,
                       "  Feed (Wf)      : %.2f kg\n"
                       "  Feed Times     : ",
                       device->params.auto_feeder.feed_kg) < 0) {
            return -1;
        }
        for (size_t i = 0; i < device->params.auto_feeder.feed_time_count; ++i) {
            const struct device_time_point *t =
                &device->params.auto_feeder.feed_times[i];

            if (append_fmt(buffer, buffer_size, &offset,
                           "%s%02u:%02u",
                           i > 0 ? ", " : "",
                           (unsigned int)t->hour,
                           (unsigned int)t->minute) < 0) {
                return -1;
            }
        }
        return append_fmt(buffer, buffer_size, &offset, "\n");

    default:
        errno = EINVAL;
        return -1;
    }
}

void device_print_info(const struct aquacare_device *device, FILE *stream)
{
    char buffer[1024];

    if (stream == NULL) {
        stream = stdout;
    }
    if (device_format_summary(device, buffer, sizeof(buffer)) == 0) {
        fputs(buffer, stream);
    } else {
        fprintf(stream, "Invalid device\n");
    }
}
