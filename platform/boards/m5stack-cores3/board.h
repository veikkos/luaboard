/*
 * Board definition: M5Stack CoreS3 family (CoreS3, CoreS3 Lite, CoreS3 SE)
 *
 *   One firmware for all three; hw.c tells them apart at boot by the BMI270
 *   IMU, which only CoreS3 and Lite have. CoreS3 and Lite also add a battery
 *   and a light/proximity sensor (both used), a camera and a compass (not
 *   used yet).
 *
 *   ESP32-S3, 16 MB flash, 8 MB quad PSRAM, 54 x 54 x 15.5 mm
 *   USB-C -> ESP32-S3 built-in USB-Serial/JTAG (no external USB-UART chip)
 *   2.0" IPS 320x240, ILI9342C on SPI2 (write-only)
 *   FT6336U capacitive touch, 320x280 sensing area: the strip of 40 px
 *     below the LCD is used as three touch buttons A / B / C. The SE has
 *     icons printed there: "< <" at x 93/122, "o" at 161, "> >" at 201/229,
 *     y ~270 (from a product photo; touches measured on an SE at y 258..279).
 *     CoreS3 and Lite have no icons, and their strip is unverified.
 *   Power key (side): AXP2101 PWRON, read from its IRQ status as Home/Back
 *   Internal I2C (G12 SDA, G11 SCL):
 *     AXP2101 PMIC 0x34   - LCD backlight on DLDO1, battery gauge
 *     AW9523B  IO  0x58   - LCD reset (P1_1), touch reset (P0_0), touch INT (P1_2)
 *     FT6336U touch 0x38, BM8563 RTC 0x51, ES7210 mic 0x40, AW88298 amp 0x36
 *     (speaker, reset on P0_2, I2S on G34 BCLK, G33 WS, G13 data)
 *     CoreS3/Lite only: BMI270 IMU 0x69, BMM150 0x10, LTR-553 0x23, GC0308 0x21
 *   Grove ports: A = G2/G1 (I2C), B = G9/G8 (GPIO/ADC), C = G17/G18 (UART)
 *
 * Sources: docs.m5stack.com/en/core/CoreS3 and M5CoreS3 SE (pin map), M5GFX's CoreS3
 * setup (LCD D/C shares G35 with the SD card's MISO). VERIFY marks details
 * not yet checked on a real board.
 *
 * Shared by the firmware (app/) and the simulator (platform/sim/).
 */
#pragma once

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_oneshot.h"

#define BOARD_NAME            "m5stack-cores3"

/* ---- Link to the PC ---------------------------------------------------- */
#define BOARD_LINK_USB_SERIAL_JTAG 1     /* /dev/cu.usbmodem* on macOS */

/* ---- Display ------------------------------------------------------------ */
#define BOARD_LCD_W           320
#define BOARD_LCD_H           240
#define BOARD_LCD_SPI_HOST    SPI2_HOST
#define BOARD_LCD_SPI_HZ      40000000
#define BOARD_LCD_MOSI        GPIO_NUM_37
#define BOARD_LCD_SCLK        GPIO_NUM_36
#define BOARD_LCD_CS          GPIO_NUM_3
#define BOARD_LCD_DC          GPIO_NUM_35  /* shared with SD card MISO */
#define BOARD_LCD_INVERT      1            /* this IPS panel needs INVON for correct colors */
#define BOARD_SD_CS           GPIO_NUM_4   /* the microSD card's select: high unless the card is in use (sdcard.c) */
#define BOARD_SD_MISO         GPIO_NUM_35  /* its data out: the same pin as the LCD's D/C */

/* ---- Touch -------------------------------------------------------------- */
#define BOARD_TOUCH_ADDR      0x38
#define BOARD_TOUCH_W         320
#define BOARD_TOUCH_H         280          /* 240 LCD rows + 40 px button strip */
#define BOARD_BTN_B_X0        130          /* strip zones: A x < 130, B 130..199, C x >= 200 */
#define BOARD_BTN_C_X0        200

/* ---- Internal I2C bus and chips ------------------------------------------ */
#define BOARD_I2C_SDA         GPIO_NUM_12
#define BOARD_I2C_SCL         GPIO_NUM_11
#define BOARD_I2C_HZ          400000
#define BOARD_PIN_I2C_INT     21           /* the IO expander's interrupt (the touch controller's through it) */
#define BOARD_PMIC_ADDR       0x34         /* AXP2101 */
#define BOARD_IOEXP_ADDR      0x58         /* AW9523B */
#define BOARD_RTC_ADDR        0x51         /* BM8563 real-time clock */
#define BOARD_IMU_ADDR        0x69         /* BMI270, chip ID 0x24 at register 0: CoreS3/Lite only */
#define BOARD_LIGHT_ADDR      0x23         /* LTR-553ALS light and proximity sensor: CoreS3/Lite only */
#define BOARD_IOEXP_TOUCH_RST 0            /* P0_0 (bit in port 0) */
#define BOARD_IOEXP_LCD_RST   1            /* P1_1 (bit in port 1). VERIFY: one M5GFX report mentions P1_5 */
/* 5V out to the Grove ports, as M5Unified's setExtOutput() switches it.
   VERIFY on hardware: taken from M5Unified, not measured. */
#define BOARD_IOEXP_BUS_EN    1            /* P0_1: 5V bus out to the ports */
#define BOARD_IOEXP_BOOST_EN  7            /* P1_7: 5V boost converter (on battery) */

/* ---- Speaker --------------------------------------------------------------
   An AW88298 class-D amplifier fed over I2S, set up over the internal I2C
   bus; its reset line is the IO expander's P0_2 (high = running). As
   M5Unified drives it. VERIFY on hardware: all three models have it. */
#define BOARD_AMP_ADDR        0x36         /* AW88298, 16-bit registers, chip ID 0x1852 */
#define BOARD_IOEXP_AMP_RST   2            /* P0_2 */
#define BOARD_SPK_I2S_PORT    I2S_NUM_1
#define BOARD_SPK_BCLK        GPIO_NUM_34
#define BOARD_SPK_WS          GPIO_NUM_33
#define BOARD_SPK_DOUT        GPIO_NUM_13

/* ---- Grove ports ----------------------------------------------------------- */
#define BOARD_PORTA_SDA       GPIO_NUM_2
#define BOARD_PORTA_SCL       GPIO_NUM_1
#define BOARD_PORTB_IN        GPIO_NUM_9   /* yellow wire; ADC1_CH8 */
#define BOARD_PORTB_OUT       GPIO_NUM_8   /* white wire;  ADC1_CH7 */
#define BOARD_PORTC_RX        GPIO_NUM_18
#define BOARD_PORTC_TX        GPIO_NUM_17

#define BOARD_ADC_UNIT        ADC_UNIT_1
#define BOARD_ADC_PORTB_IN    ADC_CHANNEL_8  /* GPIO9 */
#define BOARD_ADC_PORTB_OUT   ADC_CHANNEL_7  /* GPIO8 */
#define BOARD_ADC_PORTA_SDA   ADC_CHANNEL_1  /* GPIO2 */
#define BOARD_ADC_PORTA_SCL   ADC_CHANNEL_0  /* GPIO1 */

/* Pins the firmware owns, or that are wired to chips on the board; Lua
   cannot touch them. LCD/SD (3, 4, 35-37), internal I2C (11, 12), USB (19,
   20), the IO expander's interrupt (21, an open-drain output, and what
   wakes the chip from deep sleep), flash and PSRAM (26-32), the speaker amp
   and mic's I2S (0 MCLK, also a boot strap; 13, 14, 33, 34), the camera's
   data lines (15, 16, 38-42, 45-48; the SE has no camera but the pins stay
   reserved) and ESP-IDF's log output (UART0 on 43, 44: a script on them
   would end the log until the next boot). VERIFY the I2S and camera pins
   against a real board. */
#define BOARD_RESERVED_PINS   { 0, 3, 4, 11, 12, 13, 14, 15, 16, 19, 20, 21, 26, 27, 28, 29, 30, 31, \
                                32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48 }
