#include "hw.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_config.h"
#include "board.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "hal/usb_serial_jtag_ll.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "hw";

static void light_sleep_init(void);
static void free_pins_init(void);

/* USB TX ring buffer, and the most handed to the driver in one write. */
#define USB_TX_BUF   4096
#define USB_TX_CHUNK 2048

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_pmic, s_ioexp, s_touch, s_rtc;
static bool s_imu;                /* BMI270 found: CoreS3 or Lite, not SE */
static i2c_master_dev_handle_t s_imu_dev, s_light_dev;   /* the sensors (hw_imu_*, hw_light_*) */
static adc_oneshot_unit_handle_t s_adc;
static uint16_t s_adc_configured; /* bitmask of configured channels */
/* The sensors' supplies, in the PMIC's 0x90 (sensor_power): BLDO1 feeds the
   light sensor and the camera, ALDO3 and BLDO2 the camera alone. */
#define PMIC_ALDO3   0x04
#define PMIC_BLDO1   0x10
#define PMIC_BLDO2   0x20
#define PMIC_SENSORS (PMIC_ALDO3 | PMIC_BLDO1 | PMIC_BLDO2)
static uint8_t s_ioexp_out[2];    /* shadow of AW9523 output registers */

/* ------------------------------------------------------------- I2C helpers */

static esp_err_t reg_write(i2c_master_dev_handle_t d, uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(d, b, 2, 50);
}

static esp_err_t reg_read(i2c_master_dev_handle_t d, uint8_t reg, uint8_t *out, size_t n)
{
    return i2c_master_transmit_receive(d, &reg, 1, out, n, 50);
}

static esp_err_t add_dev(uint8_t addr, i2c_master_dev_handle_t *out)
{
    i2c_device_config_t c = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = BOARD_I2C_HZ,
    };
    return i2c_master_bus_add_device(s_i2c, &c, out);
}

static void ioexp_set(int port, int bit, int level)
{
    if (level) s_ioexp_out[port] |= (uint8_t)(1u << bit);
    else s_ioexp_out[port] &= (uint8_t)~(1u << bit);
    reg_write(s_ioexp, (uint8_t)(0x02 + port), s_ioexp_out[port]);
}

/* ------------------------------------------------------------------- init */

esp_err_t hw_init(void)
{
    /* PC link: the ESP32-S3's built-in USB-Serial/JTAG. The IDF console is
       moved to UART0 in sdkconfig so this port carries only our protocol. */
    usb_serial_jtag_driver_config_t ucfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ucfg.rx_buffer_size = 128;          /* unused, see hw_serial_read (the driver wants more than a packet) */
    ucfg.tx_buffer_size = USB_TX_BUF;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&ucfg));
    usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
    /* The ROM's printf, which code that can't use the log calls prints with
       (the camera driver's interrupt, of a picture that came short), writes
       to this port as well as to UART0: its lines would land in the middle
       of what the link is sending. g_usb_print is the ROM's own switch for
       that (ESP-IDF sets it at start-up; taking away the ROM's output
       channel 2 doesn't stop it, as tried on a Lite). */
    extern bool g_usb_print;
    g_usb_print = false;

    /* Internal I2C bus: PMIC, IO expander, touch. */
    i2c_master_bus_config_t bcfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = 1,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bcfg, &s_i2c));
    ESP_ERROR_CHECK(add_dev(BOARD_PMIC_ADDR, &s_pmic));
    ESP_ERROR_CHECK(add_dev(BOARD_IOEXP_ADDR, &s_ioexp));
    ESP_ERROR_CHECK(add_dev(BOARD_TOUCH_ADDR, &s_touch));
    if (add_dev(BOARD_RTC_ADDR, &s_rtc) != ESP_OK) s_rtc = NULL;   /* optional */

    /* AW9523B: P0_0 (touch reset) and P1_1 (LCD reset) as outputs, held high.
       Config registers: 1 = input. */
    /* Also P0_1 and P1_7: 5V out to the Grove ports, off until an app uses
       a port (hw_port_power): its converter draws from the battery. */
    /* And P0_5 an output held low: USB power comes in. As an input the board
       pulls it high, which sends the board's 5V out to the USB socket and
       lets none in: it ran from the battery without charging it, and a board
       with no battery would lose power here. */
    s_ioexp_out[0] = (uint8_t)(1u << BOARD_IOEXP_TOUCH_RST);
    s_ioexp_out[1] = (uint8_t)(1u << BOARD_IOEXP_LCD_RST);
    reg_write(s_ioexp, 0x02, s_ioexp_out[0]);
    reg_write(s_ioexp, 0x03, s_ioexp_out[1]);
    /* P0_2 too: the speaker amplifier's reset, low (off) until a sound plays. */
    reg_write(s_ioexp, 0x04,
              (uint8_t)~(1u << BOARD_IOEXP_TOUCH_RST | 1u << BOARD_IOEXP_BUS_EN | 1u << BOARD_IOEXP_AMP_RST |
                         1u << BOARD_IOEXP_USB_OUT_EN));
    /* Port 0 push-pull (GCR 0x11 bit 4), as M5Unified sets it: after reset
       it's open-drain, so its "high" outputs would float. VERIFY on hardware. */
    reg_write(s_ioexp, 0x11, 0x10);
    /* P1_0 too: the camera's reset, low (held in reset) until a script uses it. */
    reg_write(s_ioexp, 0x05,
              (uint8_t)~(1u << BOARD_IOEXP_LCD_RST | 1u << BOARD_IOEXP_BOOST_EN | 1u << BOARD_IOEXP_CAM_RST));

    /* The charger, as M5Unified sets it, since chips differ in what they
       start with: 4.2 V (0x64 = 3: one has been seen with a reserved value),
       and the TS pin a plain input (0x50 bit 4). It has no battery
       thermistor on it but the 5V bus, which a chip measuring temperature
       there takes for a cold battery, and never charges. */
    reg_write(s_pmic, 0x64, 0x03);
    uint8_t ts = 0;
    if (reg_read(s_pmic, 0x50, &ts, 1) == ESP_OK) reg_write(s_pmic, 0x50, ts | 0x10);

    /* The sensors' supply is off until a script uses them. The PMIC keeps
       its registers over a reset, so one in the middle of such a script
       would leave it on. */
    uint8_t ldo = 0;
    if (reg_read(s_pmic, 0x90, &ldo, 1) == ESP_OK && (ldo & PMIC_SENSORS)) reg_write(s_pmic, 0x90, ldo & (uint8_t)~PMIC_SENSORS);

    /* BMI270 chip ID: CoreS3 and Lite have one, the SE doesn't. */
    uint8_t id = 0;
    if (add_dev(BOARD_IMU_ADDR, &s_imu_dev) == ESP_OK) {
        s_imu = reg_read(s_imu_dev, 0x00, &id, 1) == ESP_OK && id == 0x24;
        if (!s_imu) {
            i2c_master_bus_rm_device(s_imu_dev);
            s_imu_dev = NULL;
        }
    }

    /* Power key: enable the PWRON short/long press IRQs (0x41 bits 3:2) and
       clear stale ones (0x49, write 1 to clear). VERIFY on hardware. */
    uint8_t irq_en = 0;
    reg_read(s_pmic, 0x41, &irq_en, 1);
    reg_write(s_pmic, 0x41, irq_en | 0x0C);
    reg_write(s_pmic, 0x49, 0x0C);

    light_sleep_init();
    free_pins_init();

    /* Backlight on (AXP2101 DLDO1); main applies the saved setting once NVS is up. */
    hw_backlight(DISPLAY_DEFAULT_BRIGHTNESS);

    /* ADC1 unit; channels are configured on demand. */
    adc_oneshot_unit_init_cfg_t acfg = { .unit_id = BOARD_ADC_UNIT };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&acfg, &s_adc));

    ESP_LOGI(TAG, "board %s (%s) up", BOARD_NAME, hw_model_name());
    return ESP_OK;
}

const char *hw_model(void) { return s_imu ? "cores3" : "cores3se"; }
const char *hw_model_name(void) { return s_imu ? "CoreS3" : "CoreS3 SE"; }
bool hw_has_buttons(void) { return !s_imu; }

/* As M5Unified switches it. On: the converter, then the bus charged with
   short pulses, each a little longer, then the switch: connected at once,
   the empty bus's inrush can pull the PMIC's converters under their limit
   on a weak supply, and the board goes off. Off: the converter, and the
   switch only once the bus has run down (200 ms): opened sooner, USB's 5V
   keeps coming through it. */
void hw_port_power(bool on)
{
    if (on == hw_port_power_on()) return;
    if (on) {
        ioexp_set(1, BOARD_IOEXP_BOOST_EN, 1);
        for (int i = 0; i < 8; i++) {
            ioexp_set(0, BOARD_IOEXP_BUS_EN, 1);
            esp_rom_delay_us(200 + (uint32_t)i * 16);
            ioexp_set(0, BOARD_IOEXP_BUS_EN, 0);
            esp_rom_delay_us(1000);
        }
        ioexp_set(0, BOARD_IOEXP_BUS_EN, 1);
    } else {
        ioexp_set(1, BOARD_IOEXP_BOOST_EN, 0);
        vTaskDelay(pdMS_TO_TICKS(200));
        ioexp_set(0, BOARD_IOEXP_BUS_EN, 0);
    }
}

bool hw_port_power_on(void) { return s_ioexp_out[0] & (1u << BOARD_IOEXP_BUS_EN); }

/* --------------------------------------------------------------- speaker */

static i2s_chan_handle_t s_spk;
static i2c_master_dev_handle_t s_amp;
static bool s_audio_on;

/* AW88298 registers are 16-bit, sent high byte first. */
static esp_err_t amp_write(uint8_t reg, uint16_t v)
{
    uint8_t b[3] = { reg, (uint8_t)(v >> 8), (uint8_t)v };
    return i2c_master_transmit(s_amp, b, 3, 50);
}

/* The AW88298's I2S sample rate field (I2SCTRL 0x06 bits 3:0). */
static uint16_t amp_rate_code(uint32_t hz)
{
    static const uint32_t rates[] = { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 96000 };
    size_t i = 0;
    while (i + 1 < sizeof rates / sizeof *rates && rates[i] < hz) i++;
    return (uint16_t)i;
}

esp_err_t hw_audio_start(void)
{
    if (s_audio_on) return ESP_OK;
    if (!s_amp && add_dev(BOARD_AMP_ADDR, &s_amp) != ESP_OK) {
        s_amp = NULL;
        return ESP_FAIL;
    }
    if (!s_spk) {
        i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(BOARD_SPK_I2S_PORT, I2S_ROLE_MASTER);
        cc.dma_desc_num = 4;
        cc.dma_frame_num = 256;                       /* 4 x 256 frames: 23 ms buffered */
        cc.auto_clear_after_cb = true;                /* silence, not old buffers, if the player falls behind */
        i2s_std_config_t sc = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SOUND_RATE),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
            .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = BOARD_SPK_BCLK, .ws = BOARD_SPK_WS,
                          .dout = BOARD_SPK_DOUT, .din = I2S_GPIO_UNUSED },
        };
        esp_err_t err = i2s_new_channel(&cc, &s_spk, NULL);
        if (err == ESP_OK) err = i2s_channel_init_std_mode(s_spk, &sc);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "speaker I2S: %s", esp_err_to_name(err));
            if (s_spk) i2s_del_channel(s_spk);
            s_spk = NULL;
            return err;
        }
    }
    esp_err_t err = i2s_channel_enable(s_spk);
    if (err != ESP_OK) return err;
    /* Out of reset, then M5Unified's set-up: boost off, I2S in and the
       amplifier on, no mute or AGC, the rate, full volume (scaled in
       software instead). VERIFY on hardware. */
    ioexp_set(0, BOARD_IOEXP_AMP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    err = amp_write(0x61, 0x0673);
    if (err == ESP_OK) err = amp_write(0x04, 0x4040);
    if (err == ESP_OK) err = amp_write(0x05, 0x0008);
    if (err == ESP_OK) err = amp_write(0x06, 0x14C0 | amp_rate_code(SOUND_RATE));
    if (err == ESP_OK) err = amp_write(0x0C, 0x0064);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "speaker amplifier: %s", esp_err_to_name(err));
        ioexp_set(0, BOARD_IOEXP_AMP_RST, 0);
        i2s_channel_disable(s_spk);
        return err;
    }
    s_audio_on = true;
    return ESP_OK;
}

esp_err_t hw_audio_write(const int16_t *frames, size_t n)
{
    if (!s_audio_on) return ESP_ERR_INVALID_STATE;
    size_t done = 0;
    return i2s_channel_write(s_spk, frames, n * 2 * sizeof *frames, &done, 1000);
}

void hw_audio_stop(void)
{
    if (!s_audio_on) return;
    s_audio_on = false;
    ioexp_set(0, BOARD_IOEXP_AMP_RST, 0);             /* in reset: off, and set up again next time */
    i2s_channel_disable(s_spk);
}

void hw_lcd_reset(void)
{
    ioexp_set(1, BOARD_IOEXP_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    ioexp_set(1, BOARD_IOEXP_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

/* The LCD can't be asked over its write-only bus, so this goes by the touch
   controller glued to it, as M5GFX 0.2.27 and the board's factory firmware
   do: M5Stack's touch firmware (vendor 0x11 in 0xA8) has the id 0x10 (in
   0xA6) on a C panel and 0x12 on an E. The touch chip takes a moment after
   power-on, so a few tries. No answer, or another id: the C, with a warning. */
char hw_lcd_panel(void)
{
    uint8_t id = 0, vendor = 0;
    for (int i = 0; i < 5; i++) {
        if (reg_read(s_touch, 0xA6, &id, 1) == ESP_OK && reg_read(s_touch, 0xA8, &vendor, 1) == ESP_OK &&
            vendor == 0x11 && (id == 0x10 || id == 0x12))
            return id == 0x12 ? 'E' : 'C';
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGW(TAG, "touch firmware %02x of vendor %02x says no LCD type: taken as ILI9342C", id, vendor);
    return 'C';
}

/* ---------------------------------------------------------------- serial */

/* Bytes from the PC are read straight from the endpoint's FIFO, which holds
   one packet (64 bytes): until it is read the chip takes no other, and the PC
   waits. The driver would do it otherwise: its interrupt (turned off in
   hw_init) moves each packet to a ring buffer at once and drops the ones the
   ring has no room for, so a firmware image, which comes faster than it is
   written to flash, arrived with holes ("upload timeout").

   A full packet is rarely the last: the next follows within a USB frame
   (1 ms) of this one being read. So after a full one, wait that long before
   saying there is no more, and a stream is read in one go. */
#define USB_PACKET       64
#define USB_NEXT_WAIT_US 1500

/* Over a restart of the chip the computer can give up on the port: it
   stays listed there, but nothing more comes or goes, not even the frames
   a connected port sees every millisecond, and so not the reset a flasher
   asks for either. Seen on a Lite after two of some forty updates over
   USB, and a restart didn't end it: the chip's USB keeps its connection
   over a restart, while the clock it runs from is set up anew. Dropping
   D+'s pull-up for a moment did end it: the computer takes that for the
   plug pulled and put back, and starts on the device anew. So if USB
   power is there but no computer is heard a few seconds after the start,
   that is done, twice at most (a charger never answers). */
#define USB_HOST_WAIT_MS  3000           /* after the start, and between the tries */
#define USB_REPLUG_MS     300
#define USB_REPLUG_TRIES  2

static void usb_host_poll(void)
{
    static const usb_serial_jtag_pull_override_vals_t off = { 0 }, on = { .dp_pu = true };
    static uint32_t at;                  /* the start, then the last try */
    static int tries;
    static bool pulled, done;
    if (done) return;
    uint32_t now = hw_millis();
    if (pulled) {
        if (now - at < USB_REPLUG_MS) return;
        usb_serial_jtag_ll_phy_enable_pull_override(&on);
        usb_serial_jtag_ll_phy_disable_pull_override();
        pulled = false;
        at = now;
        return;
    }
    if (now - at < USB_HOST_WAIT_MS) return;
    if (usb_serial_jtag_is_connected() || tries == USB_REPLUG_TRIES || !hw_usb_powered()) {
        done = true;
        return;
    }
    ESP_LOGW(TAG, "USB power but no computer heard: asking it to start on the port again");
    usb_serial_jtag_ll_phy_enable_pull_override(&off);
    pulled = true;
    tries++;
    at = now;
}

int hw_serial_read(uint8_t *buf, size_t cap)
{
    usb_host_poll();
    static size_t s_pkt;        /* bytes read of the packet in the FIFO */
    static bool s_more;         /* the last packet was a full one */
    static int64_t s_at;        /* when it had been read */
    size_t n = 0;
    while (n < cap) {
        size_t want = cap - n < USB_PACKET ? cap - n : USB_PACKET;
        size_t got = usb_serial_jtag_ll_read_rxfifo(buf + n, (uint32_t)want);
        n += got;
        s_pkt += got;
        if (got == want) continue;          /* (the packet may go on) */
        /* The FIFO is empty: the packet is read. */
        if (s_pkt) {
            s_more = s_pkt % USB_PACKET == 0;
            s_pkt = 0;
            s_at = esp_timer_get_time();
        }
        if (!s_more || esp_timer_get_time() - s_at > USB_NEXT_WAIT_US) {
            s_more = false;
            break;
        }
    }
    return (int)n;
}

bool hw_serial_write(const void *data, size_t len)
{
    /* The driver queues a write into its TX ring buffer in one piece, and a
       write bigger than the buffer fails outright, so send in chunks. Wait up
       to 50 ms per chunk for the host to drain. If it doesn't (a port nobody
       has open), drop the rest, and drop further writes at once until one
       gets through again, so a chatty script doesn't stall the main loop.
       No host at all (battery, or a charger): drop without waiting. */
    static bool s_stalled;
    if (!usb_serial_jtag_is_connected()) return false;
    const uint8_t *p = data;
    while (len > 0) {
        size_t n = len < USB_TX_CHUNK ? len : USB_TX_CHUNK;
        if (usb_serial_jtag_write_bytes(p, n, s_stalled ? 0 : pdMS_TO_TICKS(50)) != (int)n) {
            s_stalled = true;
            return false;
        }
        s_stalled = false;
        p += n;
        len -= n;
    }
    return true;
}

void hw_serial_puts(const char *s) { hw_serial_write(s, strlen(s)); }

/* ------------------------------------------------------------------ ADC */

#define ADC_SETTLE_MS 10

esp_err_t hw_adc_read(int channel, int *raw)
{
    if (channel < 0 || channel > 9) return ESP_ERR_INVALID_ARG; /* ADC1: CH0..CH9 on the S3 */
    if (!(s_adc_configured & (1u << channel))) {
        adc_oneshot_chan_cfg_t c = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
        esp_err_t err = adc_oneshot_config_channel(s_adc, (adc_channel_t)channel, &c);
        if (err != ESP_OK) return err;
        s_adc_configured |= (uint16_t)(1u << channel);
        /* That switched off the pull-up a free pin has (hw_gpio_release),
           which had lifted the voltage on the pin: give it time to come
           down to what the source gives. Enough for a 10 k source with a
           100 nF capacitor; a weaker source takes longer (docs/LUA_API.md). */
        vTaskDelay(pdMS_TO_TICKS(ADC_SETTLE_MS));
    }
    return adc_oneshot_read(s_adc, (adc_channel_t)channel, raw);
}

int hw_adc_raw_to_mv(int raw)
{
    /* Uncalibrated linear approximation for 12 dB attenuation (~0..3100 mV).
       On hardware, prefer esp_adc/adc_cali for per-chip eFuse calibration. */
    return (raw * 3100) / 4095;
}

void hw_gpio_release(int pin)
{
    /* ESP-IDF's reset state: not driven, the input off, the weak pull-up on
       (so the pin doesn't float). hw_init() puts every pin a script can
       take in this state too, so a pin no app uses is the same at boot and
       after an app let go of it. */
    gpio_reset_pin((gpio_num_t)pin);
    /* That makes an analog pin digital again: configure the channel anew
       on the next read. ADC1 channel n is GPIO n+1. */
    if (pin >= 1 && pin <= 10) s_adc_configured &= (uint16_t)~(1u << (pin - 1));
}

/* The pins a script can take (those the chip has, less the reserved ones)
   start as an app leaves them. */
static void free_pins_init(void)
{
    for (int pin = 0; pin < GPIO_NUM_MAX; pin++)
        if (GPIO_IS_VALID_GPIO(pin) && !hw_gpio_is_reserved(pin)) hw_gpio_release(pin);
}

bool hw_gpio_is_reserved(int pin)
{
    static const int reserved[] = BOARD_RESERVED_PINS;
    for (size_t i = 0; i < sizeof reserved / sizeof reserved[0]; i++)
        if (reserved[i] == pin) return true;
    return false;
}

/* ---------------------------------------------------------------- touch */

bool hw_touch_read(int *x, int *y)
{
    uint8_t d[5];
    if (reg_read(s_touch, 0x02, d, sizeof d) != ESP_OK) return false;
    if ((d[0] & 0x0F) == 0) return false;
    *x = ((d[1] & 0x0F) << 8) | d[2];
    *y = ((d[3] & 0x0F) << 8) | d[4];
    return *x < BOARD_TOUCH_W && *y < BOARD_TOUCH_H;
}

/* -------------------------------------------------------------- sensors */

#include "bmi270_config.h"
#include "esp_camera.h"

#define IMU_CHUNK 256                    /* of the configuration file per I2C write */

static bool s_imu_on, s_light_on;

static esp_err_t reg_write_n(i2c_master_dev_handle_t d, uint8_t reg, const uint8_t *data, size_t n)
{
    uint8_t b[1 + IMU_CHUNK];
    if (n > IMU_CHUNK) return ESP_ERR_INVALID_SIZE;
    b[0] = reg;
    memcpy(b + 1, data, n);
    return i2c_master_transmit(d, b, n + 1, 100);
}

/* The compass (BMM150) isn't on our I2C bus but on the BMI270's auxiliary
   one, as M5Unified reaches it: 0x6B bit 5 switches that bus on, 0x4B is
   the compass's address shifted left once. By hand (0x4C bit 7), 0x4F and
   then 0x4E write one of its registers, and 0x4D reads eight of them (0x4C
   bits 3:2 = 3; bits 1:0 are the count when the chip reads by itself) into
   0x04 on; 0x03 bit 2 says the bus is still busy.
   Left to itself (0x4C = 0x4F, 0x7D bit 0), the BMI270 keeps 0x04..0x0B
   filled with the compass's eight data registers from 0x42.
   The compass: 0x4B bit 0 out of suspend (3 ms), 0x40 chip ID 0x32,
   0x51/0x52 its repetitions (4 and 7: Bosch's "regular"), 0x4C = 0x38
   measuring 30 times a second, 0x5D..0x71 the trim values hw_mag_read()
   corrects the readings with. */
static bool s_mag;                       /* it started, and has readings */
static bool s_mag_awake;                 /* it was taken out of suspend */
static struct {
    int8_t x1, y1, x2, y2, xy2;
    uint8_t xy1;
    uint16_t z1, xyz1;
    int16_t z2, z3, z4;
} s_trim;

static esp_err_t aux_wait(void)
{
    vTaskDelay(1);
    for (int i = 0; i < 10; i++) {
        uint8_t busy = 0;
        esp_err_t err = reg_read(s_imu_dev, 0x03, &busy, 1);
        if (err != ESP_OK) return err;
        if (!(busy & 0x04)) return ESP_OK;
        vTaskDelay(1);
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t aux_write(uint8_t reg, uint8_t val)
{
    esp_err_t err = reg_write(s_imu_dev, 0x4F, val);
    if (err == ESP_OK) err = reg_write(s_imu_dev, 0x4E, reg);
    return err == ESP_OK ? aux_wait() : err;
}

static esp_err_t aux_read(uint8_t reg, uint8_t out[8])
{
    esp_err_t err = reg_write(s_imu_dev, 0x4D, reg);
    if (err == ESP_OK) err = aux_wait();
    return err == ESP_OK ? reg_read(s_imu_dev, 0x04, out, 8) : err;
}

static void mag_start(void)
{
    uint8_t t[24] = { 0 }, id[8] = { 0 };            /* t: the compass's 0x5D..0x74 */
    esp_err_t err = reg_write(s_imu_dev, 0x6B, 0x20);
    if (err == ESP_OK) err = reg_write(s_imu_dev, 0x4C, 0x8F);
    if (err == ESP_OK) err = reg_write(s_imu_dev, 0x4B, BOARD_MAG_ADDR << 1);
    if (err == ESP_OK) err = aux_write(0x4B, 0x01);
    if (err == ESP_OK) s_mag_awake = true;
    vTaskDelay(2);                                   /* 3 ms; one tick can be next to nothing */
    if (err == ESP_OK) err = aux_read(0x40, id);
    if (err == ESP_OK && id[0] != 0x32) err = ESP_ERR_NOT_FOUND;
    for (int i = 0; err == ESP_OK && i < 3; i++) err = aux_read((uint8_t)(0x5D + 8 * i), t + 8 * i);
    if (err == ESP_OK) err = aux_write(0x51, 4);
    if (err == ESP_OK) err = aux_write(0x52, 7);
    if (err == ESP_OK) err = aux_write(0x4C, 0x38);
    if (err == ESP_OK) err = reg_write(s_imu_dev, 0x4C, 0x4F);
    if (err == ESP_OK) err = reg_write(s_imu_dev, 0x4D, 0x42);
    if (err == ESP_OK) err = reg_write(s_imu_dev, 0x7D, 0x0F);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "compass: %s", esp_err_to_name(err));
        return;
    }
#define T16(at) ((uint16_t)(t[(at) - 0x5D] | t[(at) - 0x5C] << 8))
    s_trim.x1 = (int8_t)t[0x5D - 0x5D];
    s_trim.y1 = (int8_t)t[0x5E - 0x5D];
    s_trim.z4 = (int16_t)T16(0x62);
    s_trim.x2 = (int8_t)t[0x64 - 0x5D];
    s_trim.y2 = (int8_t)t[0x65 - 0x5D];
    s_trim.z2 = (int16_t)T16(0x68);
    s_trim.z1 = T16(0x6A);
    s_trim.xyz1 = T16(0x6C) & 0x7FFF;
    s_trim.z3 = (int16_t)T16(0x6E);
    s_trim.xy2 = (int8_t)t[0x70 - 0x5D];
    s_trim.xy1 = t[0x71 - 0x5D];
#undef T16
    s_mag = true;
}

/* The compass back in suspend: a reset of the BMI270 alone would leave it
   measuring. */
static void mag_stop(void)
{
    if (!s_mag_awake) return;                        /* (also one that woke and then failed to start) */
    s_mag = s_mag_awake = false;
    reg_write(s_imu_dev, 0x7D, 0x0E);
    reg_write(s_imu_dev, 0x4C, 0x8F);
    aux_write(0x4B, 0x00);
}

/* The field from the raw readings, by the correction of Bosch's BMM150
   driver (bmm150.c, its floating-point version): X and Y are 13 bits, Z
   15, and the Hall resistance (14 bits) says how warm the chip is. A raw
   -4096 (X, Y) or -16384 (Z) is a field beyond the range (1300 microtesla
   on X and Y, 2500 on Z): a magnet next to the chip, which a Lite has in
   its back cover. */
esp_err_t hw_mag_read(float ut[3])
{
    if (!s_imu_on) return ESP_ERR_INVALID_STATE;
    if (!s_mag) return ESP_ERR_NOT_FOUND;
    uint8_t d[8];
    esp_err_t err = reg_read(s_imu_dev, 0x04, d, sizeof d);
    if (err != ESP_OK) return err;
    int16_t x = (int16_t)((int8_t)d[1] * 32 | d[0] >> 3), y = (int16_t)((int8_t)d[3] * 32 | d[2] >> 3);
    int16_t z = (int16_t)((int8_t)d[5] * 128 | d[4] >> 1);
    uint16_t rhall = (uint16_t)(d[7] << 6 | d[6] >> 2);
    if (!rhall || !s_trim.xyz1 || !s_trim.z1 || !s_trim.z2) return ESP_ERR_TIMEOUT;      /* no measurement (yet) */
    if (x == -4096 || y == -4096 || z == -16384) return ESP_ERR_INVALID_RESPONSE;
    float k = (float)s_trim.xyz1 * 16384.0f / (float)rhall - 16384.0f;
    float xy = (float)s_trim.xy2 * (k * k / 268435456.0f) + k * (float)s_trim.xy1 / 16384.0f + 256.0f;
    ut[0] = ((float)x * xy * ((float)s_trim.x2 + 160.0f) / 8192.0f + (float)s_trim.x1 * 8.0f) / 16.0f;
    ut[1] = ((float)y * xy * ((float)s_trim.y2 + 160.0f) / 8192.0f + (float)s_trim.y1 * 8.0f) / 16.0f;
    float zn = ((float)z - (float)s_trim.z4) * 131072.0f - (float)s_trim.z3 * ((float)rhall - (float)s_trim.xyz1);
    ut[2] = zn / (((float)s_trim.z2 + (float)s_trim.z1 * (float)rhall / 32768.0f) * 4.0f) / 16.0f;
    return ESP_OK;
}

/* BMI270, as its data sheet's quick start and M5Unified do it: soft reset
   (0x7E = 0xB6), advanced power save off (0x7C), the 8 KB configuration
   file to 0x5E in bursts, each after the file position in 16-bit words
   split over 0x5B (low 4 bits) and 0x5C, then 0x59 = 1 and 0x21 says 1
   when the chip took it. 0x7D bits 3..1: temperature, accelerometer,
   gyroscope on. 0x40..0x43 are the rates and ranges, written as their
   power-up values (100 and 200 Hz, +-8 g, +-2000 degrees a second) so the
   scale doesn't depend on them. VERIFY on hardware. */
esp_err_t hw_imu_start(void)
{
    if (!s_imu) return ESP_ERR_NOT_FOUND;
    if (s_imu_on) return ESP_OK;
    esp_err_t err = reg_write(s_imu_dev, 0x7E, 0xB6);
    vTaskDelay(pdMS_TO_TICKS(20));
    if (err == ESP_OK) err = reg_write(s_imu_dev, 0x7C, 0x00);
    vTaskDelay(2);                                   /* at least 450 us; one tick can be next to nothing */
    if (err == ESP_OK) err = reg_write(s_imu_dev, 0x59, 0x00);
    for (size_t at = 0; err == ESP_OK && at < sizeof bmi270_config_file; at += IMU_CHUNK) {
        uint8_t word[2] = { (uint8_t)((at / 2) & 0x0F), (uint8_t)((at / 2) >> 4) };
        err = reg_write_n(s_imu_dev, 0x5B, word, 2);
        if (err == ESP_OK) err = reg_write_n(s_imu_dev, 0x5E, bmi270_config_file + at, IMU_CHUNK);
    }
    if (err == ESP_OK) err = reg_write(s_imu_dev, 0x59, 0x01);
    uint8_t status = 0;
    for (int i = 0; err == ESP_OK && i < 20 && !(status & 0x0F); i++) {
        vTaskDelay(1);
        reg_read(s_imu_dev, 0x21, &status, 1);
    }
    if (err == ESP_OK && (status & 0x0F) != 0x01) err = ESP_FAIL;      /* the file wasn't taken */
    static const uint8_t conf[4] = { 0xA8, 0x02, 0xA9, 0x00 };          /* 0x40..0x43 */
    if (err == ESP_OK) err = reg_write_n(s_imu_dev, 0x40, conf, sizeof conf);
    if (err == ESP_OK) err = reg_write(s_imu_dev, 0x7D, 0x0E);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "motion sensor: %s", esp_err_to_name(err));
        reg_write(s_imu_dev, 0x7E, 0xB6);                               /* back to its idle state */
        return err;
    }
    mag_start();                                                        /* (without a compass it's still a motion sensor) */
    vTaskDelay(pdMS_TO_TICKS(s_mag ? 100 : 50));                        /* its first readings, the compass's through it */
    s_imu_on = true;
    return ESP_OK;
}

void hw_imu_stop(void)
{
    if (!s_imu_on) return;
    mag_stop();
    reg_write(s_imu_dev, 0x7E, 0xB6);        /* a reset leaves it suspended: a few microamps */
    s_imu_on = false;
}

esp_err_t hw_imu_read(int16_t accel[3], int16_t gyro[3])
{
    if (!s_imu_on) return ESP_ERR_INVALID_STATE;
    uint8_t d[12];
    esp_err_t err = reg_read(s_imu_dev, 0x0C, d, sizeof d);
    if (err != ESP_OK) return err;
    for (int i = 0; i < 3; i++) {
        accel[i] = (int16_t)(d[2 * i] | d[2 * i + 1] << 8);
        gyro[i] = (int16_t)(d[6 + 2 * i] | d[7 + 2 * i] << 8);
    }
    return ESP_OK;
}

/* The sensors' supplies: the PMIC starts with them off, and without its own
   a chip isn't on the bus at all (seen on a Lite). BLDO1 (2.8 V) feeds the
   light sensor and the camera, ALDO3 (3.3 V) and BLDO2 (1.5 V) the camera
   alone: the voltages a Lite's PMIC came with (0x94, 0x96, 0x97: 0.5 V +
   0.1 V * n), and ALDO3's is M5Unified's. On only while a script uses the
   sensor. */
enum { POWER_LIGHT = 1, POWER_CAMERA = 2 };
static uint8_t s_powered;                /* who has them on */

static esp_err_t sensor_power(uint8_t who, bool on)
{
    uint8_t users = on ? s_powered | who : s_powered & (uint8_t)~who;
    uint8_t want = (uint8_t)((users ? PMIC_BLDO1 : 0) | ((users & POWER_CAMERA) ? PMIC_ALDO3 | PMIC_BLDO2 : 0));
    uint8_t en;
    s_powered = users;                                      /* (also when the read fails: one that stopped doesn't keep them on) */
    esp_err_t err = reg_read(s_pmic, 0x90, &en, 1);         /* (the other LDOs' bits: never from a failed read) */
    if (err != ESP_OK) return err;
    if ((en & PMIC_SENSORS) == want) return ESP_OK;
    bool more = want & (uint8_t)~en;
    if (more) {
        reg_write(s_pmic, 0x94, 28);
        reg_write(s_pmic, 0x96, 23);
        reg_write(s_pmic, 0x97, 10);
    }
    err = reg_write(s_pmic, 0x90, (uint8_t)((en & (uint8_t)~PMIC_SENSORS) | want));
    if (err == ESP_OK && more) vTaskDelay(pdMS_TO_TICKS(100));   /* a chip's start-up time from power */
    return err;
}

/* LTR-553ALS: 0x80 ALS control (1 = active, gain 1), 0x81 PS control (3 =
   active), 0x86 part ID (0x9x), 0x88..0x8B the light channels 1 and 0
   (channel 1 first, as the data sheet asks), 0x8D/0x8E proximity (11
   bits). The measurement rates stay at their power-up values (100 ms
   integration). VERIFY on hardware. */
esp_err_t hw_light_start(void)
{
    if (!s_imu) return ESP_ERR_NOT_FOUND;                   /* the SE: no sensors */
    if (s_light_on) return ESP_OK;
    if (!s_light_dev && add_dev(BOARD_LIGHT_ADDR, &s_light_dev) != ESP_OK) { s_light_dev = NULL; return ESP_FAIL; }
    esp_err_t err = sensor_power(POWER_LIGHT, true);
    uint8_t id = 0;
    if (err == ESP_OK) err = reg_read(s_light_dev, 0x86, &id, 1);
    if (err == ESP_OK && (id >> 4) != 0x9) err = ESP_ERR_NOT_FOUND;
    if (err == ESP_OK) err = reg_write(s_light_dev, 0x80, 0x01);
    if (err == ESP_OK) err = reg_write(s_light_dev, 0x81, 0x03);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "light sensor: %s", esp_err_to_name(err));
        sensor_power(POWER_LIGHT, false);
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(150));                         /* wake-up and the first measurement (100 ms) */
    s_light_on = true;
    return ESP_OK;
}

void hw_light_stop(void)
{
    if (!s_light_on) return;
    reg_write(s_light_dev, 0x80, 0x00);                     /* both parts to standby: the camera may keep the supply on */
    reg_write(s_light_dev, 0x81, 0x00);
    sensor_power(POWER_LIGHT, false);
    s_light_on = false;
}

/* The camera, through Espressif's esp32-camera: it sets the GC0308 up over
   our I2C bus and takes the pictures into two buffers in PSRAM, handing
   out the newer one. */
static bool s_cam_on, s_cam_mirror;
static camera_fb_t *s_cam_fb;

/* The chip mirrors the picture itself. The pictures it had taken or begun
   by then are still on their way, one in each of the driver's buffers:
   they are let go, so the next one handed out is as asked. */
static void camera_mirror_set(void)
{
    sensor_t *s = esp_camera_sensor_get();
    if (!s || !s->set_hmirror) return;
    s->set_hmirror(s, s_cam_mirror);
    for (int i = 0; i < 2; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) esp_camera_fb_return(fb);
    }
}

void hw_camera_mirror(bool on)
{
    if (on == s_cam_mirror) return;
    s_cam_mirror = on;
    if (!s_cam_on) return;
    hw_camera_done();
    camera_mirror_set();
}

esp_err_t hw_camera_start(void)
{
    if (!s_imu) return ESP_ERR_NOT_FOUND;                   /* the SE: no camera */
    if (s_cam_on) return ESP_OK;
    static const int data[8] = BOARD_CAM_DATA;
    camera_config_t cfg = {
        .pin_pwdn = -1,
        .pin_reset = -1,
        .pin_xclk = -1,
        .pin_sccb_sda = -1,                                 /* the bus that is there already */
        .pin_sccb_scl = -1,
        .sccb_i2c_port = I2C_NUM_0,
        .pin_d0 = data[0],
        .pin_d1 = data[1],
        .pin_d2 = data[2],
        .pin_d3 = data[3],
        .pin_d4 = data[4],
        .pin_d5 = data[5],
        .pin_d6 = data[6],
        .pin_d7 = data[7],
        .pin_vsync = BOARD_CAM_VSYNC,
        .pin_href = BOARD_CAM_HREF,
        .pin_pclk = BOARD_CAM_PCLK,
        .xclk_freq_hz = 20000000,
        .pixel_format = PIXFORMAT_RGB565,
        .frame_size = FRAMESIZE_QVGA,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };
    esp_err_t err = sensor_power(POWER_CAMERA, true);
    ioexp_set(1, BOARD_IOEXP_CAM_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    if (err == ESP_OK) err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "camera: %s", esp_err_to_name(err));
        ioexp_set(1, BOARD_IOEXP_CAM_RST, 0);
        sensor_power(POWER_CAMERA, false);
        return err;
    }
    s_cam_on = true;
    if (s_cam_mirror) camera_mirror_set();
    return ESP_OK;
}

void hw_camera_stop(void)
{
    if (!s_cam_on) return;
    hw_camera_done();
    esp_camera_deinit();
    ioexp_set(1, BOARD_IOEXP_CAM_RST, 0);
    sensor_power(POWER_CAMERA, false);
    s_cam_on = false;
}

bool hw_camera_on(void) { return s_cam_on; }

const uint8_t *hw_camera_frame(void)
{
    if (!s_cam_on) return NULL;
    hw_camera_done();
    s_cam_fb = esp_camera_fb_get();
    if (s_cam_fb && s_cam_fb->len < (size_t)BOARD_CAM_W * BOARD_CAM_H * 2) hw_camera_done();
    return s_cam_fb ? s_cam_fb->buf : NULL;
}

void hw_camera_done(void)
{
    if (s_cam_fb) esp_camera_fb_return(s_cam_fb);
    s_cam_fb = NULL;
}

esp_err_t hw_light_read(uint16_t *ch0, uint16_t *ch1, uint16_t *proximity)
{
    if (!s_light_on) return ESP_ERR_INVALID_STATE;
    uint8_t d[4], p[2];
    esp_err_t err = reg_read(s_light_dev, 0x88, d, sizeof d);
    if (err == ESP_OK) err = reg_read(s_light_dev, 0x8D, p, sizeof p);
    if (err != ESP_OK) return err;
    *ch1 = (uint16_t)(d[0] | d[1] << 8);
    *ch0 = (uint16_t)(d[2] | d[3] << 8);
    *proximity = (uint16_t)(p[0] | (p[1] & 0x07) << 8);
    return ESP_OK;
}

/* ---------------------------------------------------------------- power */

static int s_backlight = DISPLAY_DEFAULT_BRIGHTNESS;   /* the level asked for */
static bool s_dark;                                     /* hw_screen_off: kept off meanwhile */

static esp_err_t backlight_set(int percent);

esp_err_t hw_backlight(int percent)
{
    s_backlight = percent;
    return s_dark ? ESP_OK : backlight_set(percent);
}

esp_err_t hw_screen_off(bool off)
{
    s_dark = off;
    return backlight_set(off ? 0 : s_backlight);
}

bool hw_screen_is_off(void) { return s_dark; }

/* Automatic light sleep (esp_pm, tickless idle) is configured at boot and
   held off with this lock; releasing it is all it takes to allow it.
   VERIFY on hardware. */
static esp_pm_lock_handle_t s_awake_lock;
static bool s_light;

static void light_sleep_init(void)
{
    esp_pm_config_t cfg = { .max_freq_mhz = 240, .min_freq_mhz = 240, .light_sleep_enable = true };
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "awake", &s_awake_lock) != ESP_OK) { s_awake_lock = NULL; return; }
    esp_pm_lock_acquire(s_awake_lock);
    if (esp_pm_configure(&cfg) != ESP_OK) ESP_LOGW(TAG, "power management not configured: no light sleep");
}

void hw_light_sleep_allow(bool on)
{
    if (!s_awake_lock || on == s_light) return;
    s_light = on;
    if (on) esp_pm_lock_release(s_awake_lock);
    else esp_pm_lock_acquire(s_awake_lock);
}

static esp_err_t backlight_set(int percent)
{
    /* 0x90 also enables the other LDOs (audio, camera, SD): never write it
       back from a failed read. */
    uint8_t en;
    esp_err_t rerr = reg_read(s_pmic, 0x90, &en, 1);
    if (rerr != ESP_OK) return rerr;
    if (percent <= 0) return reg_write(s_pmic, 0x90, en & (uint8_t)~0x80);
    if (percent > 100) percent = 100;
    /* DLDO1 = 0.5 V + 0.1 V * n. Usable range ~2.5 V (dim) .. 3.3 V (full). */
    uint8_t n = (uint8_t)(20 + (percent * 8 + 50) / 100);
    esp_err_t err = reg_write(s_pmic, 0x99, n);
    if (err == ESP_OK) err = reg_write(s_pmic, 0x90, en | 0x80);
    return err;
}

/* CoreS3 and Lite have a battery; the SE runs from USB-C (or the M-Bus 5 V)
   unless a battery module is added. AXP2101 status1 bit3 says which. */
bool hw_battery_present(void)
{
    uint8_t v = 0;
    return reg_read(s_pmic, 0x00, &v, 1) == ESP_OK && (v & 0x08);
}

int hw_battery_percent(void)
{
    uint8_t v;
    if (!hw_battery_present()) return -1;
    return reg_read(s_pmic, 0xA4, &v, 1) == ESP_OK && v <= 100 ? v : -1;
}

bool hw_usb_powered(void)
{
    uint8_t v = 0;
    reg_read(s_pmic, 0x00, &v, 1);
    return (v & 0x20) != 0;
}

/* ------------------------------------------------------------------ RTC */
/* BM8563 (PCF8563 layout): 0x00 control 1 (bit5 STOP), 0x02 seconds (bit7 VL:
   lost power, time not reliable), 0x03 minutes, 0x04 hours, 0x05 day,
   0x06 weekday, 0x07 month (bit7 century), 0x08 year 00..99. BCD. Kept in UTC. */

static uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static int unbcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }

bool hw_rtc_read(int64_t *utc)
{
    uint8_t r[7];
    if (!s_rtc || reg_read(s_rtc, 0x02, r, sizeof r) != ESP_OK) return false;
    if (r[0] & 0x80) return false;                         /* VL: lost power */
    int sec = unbcd(r[0] & 0x7F), min = unbcd(r[1] & 0x7F), hour = unbcd(r[2] & 0x3F);
    int day = unbcd(r[3] & 0x3F), mon = unbcd(r[5] & 0x1F), year = 2000 + unbcd(r[6]);
    if (sec > 59 || min > 59 || hour > 23 || day < 1 || day > 31 || mon < 1 || mon > 12) return false;
    struct tm tm = { .tm_year = year - 1900, .tm_mon = mon - 1, .tm_mday = day,
                     .tm_hour = hour, .tm_min = min, .tm_sec = sec };
    *utc = timegm(&tm);
    return true;
}

esp_err_t hw_rtc_write(int64_t utc)
{
    if (!s_rtc) return ESP_ERR_NOT_FOUND;
    time_t t = (time_t)utc;
    struct tm tm;
    if (!gmtime_r(&t, &tm) || tm.tm_year < 100 || tm.tm_year > 199) return ESP_ERR_INVALID_ARG;
    /* One transaction from register 0: control 1 (clock running), control 2,
       then the time; writing the seconds clears VL. */
    uint8_t buf[10] = { 0x00, 0x00, 0x00,
                        bcd(tm.tm_sec), bcd(tm.tm_min), bcd(tm.tm_hour), bcd(tm.tm_mday),
                        (uint8_t)tm.tm_wday, bcd(tm.tm_mon + 1), bcd(tm.tm_year - 100) };
    return i2c_master_transmit(s_rtc, buf, sizeof buf, 50);
}

bool hw_charging(void)
{
    uint8_t v = 0;
    if (!hw_battery_present()) return false;
    reg_read(s_pmic, 0x01, &v, 1);
    return ((v >> 5) & 3) == 1;
}

/* The RTC as an alarm clock for the board. BM8563 (PCF8563 layout): 0x01
   control 2 (bit3 AF, bit2 TF: written 0 to clear; bit1 AIE, bit0 TIE),
   0x09..0x0C minute, hour, day and weekday alarm (bit7: not compared),
   0x0E timer control (bit7 enable; source 2 = 1 Hz, 3 = 1/60 Hz), 0x0F the
   timer's count. Its interrupt output shares the PMIC's IRQ line, which
   turns the PMIC on. A count of n runs out after n-1 to n periods, so never
   late. VERIFY on hardware. */
void hw_rtc_wake_disarm(void)
{
    if (!s_rtc) return;
    reg_write(s_rtc, 0x0E, 0x03);                    /* timer off (and its slowest source: least current) */
    reg_write(s_rtc, 0x01, 0x00);                    /* no interrupts, flags cleared */
    for (uint8_t r = 0x09; r <= 0x0C; r++) reg_write(s_rtc, r, 0x80);
}

bool hw_rtc_wake_fired(void)
{
    uint8_t v = 0;
    return s_rtc && reg_read(s_rtc, 0x01, &v, 1) == ESP_OK && (v & 0x0C);
}

esp_err_t hw_rtc_wake_arm(const wake_plan_t *p)
{
    if (!s_rtc) return ESP_ERR_NOT_FOUND;
    hw_rtc_wake_disarm();
    esp_err_t err;
    if (p->kind == WAKE_ALARM) {
        time_t t = (time_t)p->alarm_utc;
        struct tm tm;
        gmtime_r(&t, &tm);
        err = reg_write(s_rtc, 0x09, bcd(tm.tm_min));
        if (err == ESP_OK) err = reg_write(s_rtc, 0x0A, bcd(tm.tm_hour));
        if (err == ESP_OK) err = reg_write(s_rtc, 0x0B, bcd(tm.tm_mday));
        if (err == ESP_OK) err = reg_write(s_rtc, 0x01, 0x02);              /* AIE */
    } else {
        err = reg_write(s_rtc, 0x0F, p->count);
        if (err == ESP_OK) err = reg_write(s_rtc, 0x0E, p->kind == WAKE_TIMER_MIN ? 0x83 : 0x82);
        if (err == ESP_OK) err = reg_write(s_rtc, 0x01, 0x01);              /* TIE */
    }
    return err;
}

void hw_power_off(const wake_plan_t *wake)
{
    bool armed = true;
    if (wake) {
        armed = hw_rtc_wake_arm(wake) == ESP_OK;
        esp_sleep_enable_timer_wakeup((uint64_t)wake->after_s * 1000000ULL);   /* a second source, as M5Unified has */
    }
    if (armed) {
        /* AXP2101 0x10 bit 0: power off. VERIFY on hardware. */
        uint8_t v = 0;
        if (reg_read(s_pmic, 0x10, &v, 1) == ESP_OK) reg_write(s_pmic, 0x10, v | 0x01);
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_LOGW(TAG, "the PMIC didn't power off: deep sleep");
    } else {
        /* With the power cut and no RTC alarm, nothing would turn the board
           on again: it sleeps instead, where the chip's own timer runs. */
        ESP_LOGW(TAG, "the RTC isn't armed: deep sleep, the chip's own timer wakes it");
    }
    /* Deep sleep, which the chip's timer ends, or a touch (the touch
       interrupt reaches this pin through the IO expander, whose interrupt
       reading its inputs clears). The board stays powered: what's lit or
       fed from it goes off first. */
    hw_screen_off(true);
    hw_port_power(false);
    uint8_t in[2];
    reg_read(s_ioexp, 0x00, in, 2);
    esp_sleep_enable_ext1_wakeup_io(1ULL << BOARD_PIN_I2C_INT, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();
}

char hw_boot_kind(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return 'p';
    case ESP_RST_DEEPSLEEP: return esp_sleep_get_wakeup_causes() & (1u << ESP_SLEEP_WAKEUP_TIMER) ? 't' : 'p';
    default: return 'r';
    }
}

int hw_power_key(void)
{
    uint8_t v = 0;
    if (reg_read(s_pmic, 0x49, &v, 1) != ESP_OK || !(v & 0x0C)) return 0;
    reg_write(s_pmic, 0x49, v & 0x0C);
    return (v & 0x08) ? 1 : 2;               /* bit3 short, bit2 long */
}

uint32_t hw_millis(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
bool hw_reset_was_crash(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_PANIC: case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT:
    case ESP_RST_BROWNOUT: case ESP_RST_PWR_GLITCH: case ESP_RST_CPU_LOCKUP:
        return true;
    default:
        return false;
    }
}

const char *hw_reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_SW: return "sw";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int-wdt";
    case ESP_RST_TASK_WDT: return "task-wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_PWR_GLITCH: return "power-glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu-lockup";
    case ESP_RST_EXT: return "ext";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    default: return "unknown";
    }
}

bool hw_reset_was_fw_crash(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_PANIC: case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: case ESP_RST_CPU_LOCKUP:
        return true;
    default:
        return false;
    }
}

uint64_t hw_millis64(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

void *hw_big_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

void hw_mem_stats(size_t *free_now, size_t *low)
{
    *free_now = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    *low = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
}

/* ------------------------------------------------------------- watchdog */
/* The task watchdog (CONFIG_ESP_TASK_WDT_PANIC, sdkconfig.defaults) resets
   the chip if the main loop stops coming round, e.g. a C call that never
   returns. The crash-loop guard in main.c then skips a bad autostart app. */

void hw_watchdog_start(void) { esp_task_wdt_add(NULL); }
void hw_watchdog_feed(void) { esp_task_wdt_reset(); }
void hw_watchdog_stretch(uint32_t ms)
{
#ifdef CONFIG_ESP_TASK_WDT_TIMEOUT_S
    uint32_t idle = 0;
#ifdef CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
    idle |= 1;
#endif
#ifdef CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
    idle |= 2;
#endif
    esp_task_wdt_config_t c = {
        .timeout_ms = ms ? ms : CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000,
        .idle_core_mask = idle,
        .trigger_panic = true,
    };
    esp_err_t e = esp_task_wdt_reconfigure(&c);
    if (e != ESP_OK) ESP_LOGW(TAG, "cannot set the watchdog's timeout: %s", esp_err_to_name(e));
#else
    (void)ms;                     /* (no watchdog here) */
#endif
}
