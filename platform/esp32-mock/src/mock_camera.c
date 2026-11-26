/*
 * ESP32 mock platform - esp32-camera.
 * A camera is there while its chip answers on the I2C bus (the simulated
 * board attaches it with its supplies on and its reset released), and its
 * pictures are what the board draws.
 */
#include "esp_camera.h"
#include "esp_rom_sys.h"
#include "sim_hw.h"

#define W 320
#define H 240

static sim_camera_fn s_picture;
static void *s_ctx;
static uint8_t s_addr;
static int s_port;
static bool s_on;
static uint8_t s_px[W * H * 2];
static camera_fb_t s_fb;
static bool s_mirror;

static int set_hmirror(sensor_t *sensor, int enable)
{
    (void)sensor;
    s_mirror = enable;
    return 0;
}

static sensor_t s_sensor = { .set_hmirror = set_hmirror };

sensor_t *esp_camera_sensor_get(void) { return s_on ? &s_sensor : NULL; }

void sim_camera_attach(uint8_t addr7, sim_camera_fn picture, void *ctx)
{
    s_addr = addr7;
    s_picture = picture;
    s_ctx = ctx;
}

bool sim_camera_running(void) { return s_on && sim_i2c_present(s_port, s_addr); }

esp_err_t esp_camera_init(const camera_config_t *c)
{
    if (!c || s_on) return ESP_ERR_INVALID_STATE;
    if (c->pixel_format != PIXFORMAT_RGB565 || c->frame_size != FRAMESIZE_QVGA) return ESP_ERR_NOT_SUPPORTED;
    if (!s_picture || !sim_i2c_present(c->sccb_i2c_port, s_addr)) return ESP_ERR_NOT_SUPPORTED;   /* the probe finds no chip (the real one's answer to that) */
    s_port = c->sccb_i2c_port;
    s_on = true;
    s_mirror = false;                                /* the chip was reset */
    /* The real driver warns of a picture that came short or one it had no
       room for from its interrupt, with the ROM's printf. */
    esp_rom_printf("cam_hal: EV-VSYNC-OVF\r\n");
    return ESP_OK;
}

esp_err_t esp_camera_deinit(void)
{
    if (!s_on) return ESP_ERR_INVALID_STATE;
    s_on = false;
    return ESP_OK;
}

camera_fb_t *esp_camera_fb_get(void)
{
    if (!sim_camera_running()) return NULL;          /* (the real one waits four seconds first) */
    s_picture(s_ctx, s_px, W, H);
    if (s_mirror)
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W / 2; x++) {
                uint8_t *a = s_px + (y * W + x) * 2, *b = s_px + (y * W + W - 1 - x) * 2;
                uint8_t t0 = a[0], t1 = a[1];
                a[0] = b[0], a[1] = b[1], b[0] = t0, b[1] = t1;
            }
    s_fb = (camera_fb_t){ .buf = s_px, .len = sizeof s_px, .width = W, .height = H, .format = PIXFORMAT_RGB565 };
    return &s_fb;
}

void esp_camera_fb_return(camera_fb_t *fb) { (void)fb; }
