/*
 * ESP32 mock platform - esp32-camera (the part the firmware uses).
 * The pictures come from the simulated board (sim_camera_attach), while its
 * camera chip answers on the I2C bus.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum { PIXFORMAT_RGB565, PIXFORMAT_JPEG } pixformat_t;
typedef enum { FRAMESIZE_QVGA, FRAMESIZE_VGA } framesize_t;
typedef enum { CAMERA_FB_IN_PSRAM, CAMERA_FB_IN_DRAM } camera_fb_location_t;
typedef enum { CAMERA_GRAB_WHEN_EMPTY, CAMERA_GRAB_LATEST } camera_grab_mode_t;

typedef struct {
    int pin_pwdn, pin_reset, pin_xclk, pin_sccb_sda, pin_sccb_scl;
    int pin_d7, pin_d6, pin_d5, pin_d4, pin_d3, pin_d2, pin_d1, pin_d0;
    int pin_vsync, pin_href, pin_pclk;
    int xclk_freq_hz;
    int ledc_timer, ledc_channel;
    pixformat_t pixel_format;
    framesize_t frame_size;
    int jpeg_quality;
    size_t fb_count;
    camera_fb_location_t fb_location;
    camera_grab_mode_t grab_mode;
    int sccb_i2c_port;
} camera_config_t;

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t width, height;
    pixformat_t format;
} camera_fb_t;

/* The camera chip's settings: here the one the firmware uses. */
typedef struct sensor sensor_t;
struct sensor {
    int (*set_hmirror)(sensor_t *sensor, int enable);
};

esp_err_t esp_camera_init(const camera_config_t *config);
/** The chip's settings, NULL while the camera is off. They start from the
 *  chip's own at every init. */
sensor_t *esp_camera_sensor_get(void);
esp_err_t esp_camera_deinit(void);
camera_fb_t *esp_camera_fb_get(void);
void esp_camera_fb_return(camera_fb_t *fb);
