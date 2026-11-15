/* ILI9342C 320x240 TFT over SPI (write-only, 4-wire with D/C). */
#include "lcd.h"

#include <stdbool.h>

#include "board.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hw.h"

static const char *TAG = "lcd";

#define CHUNK_ROWS 16   /* rows per SPI transaction: 320*16*2 = 10 KB DMA buffer */

static spi_device_handle_t s_dev;
static uint16_t *s_dma;  /* internal, DMA-capable bounce buffer (framebuffer lives in PSRAM) */
static bool s_up;        /* lcd_init got all the way: without it, flushes are dropped (headless) */

static void tx(const void *data, size_t len, int dc)
{
    if (!len) return;
    gpio_set_level(BOARD_LCD_DC, (uint32_t)dc);
    spi_transaction_t t = { .length = len * 8, .tx_buffer = data };
    spi_device_polling_transmit(s_dev, &t);
}

static void cmd(uint8_t c, const uint8_t *params, size_t n)
{
    tx(&c, 1, 0);
    tx(params, n, 1);
}

esp_err_t lcd_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = BOARD_LCD_MOSI,
        /* The LCD is write-only and uses G35 as its D/C. The microSD card on
           the same bus answers on that pin: it's the bus's MISO, an input
           while the card is in use (sdcard.c) and our output otherwise. */
        .miso_io_num = BOARD_SD_MISO,
        .sclk_io_num = BOARD_LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_LCD_W * CHUNK_ROWS * 2,
    };
    esp_err_t err = spi_bus_initialize(BOARD_LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) return err;
    spi_device_interface_config_t dev = {
        .clock_speed_hz = BOARD_LCD_SPI_HZ,
        .mode = 0,
        .spics_io_num = BOARD_LCD_CS,
        .queue_size = 1,
    };
    err = spi_bus_add_device(BOARD_LCD_SPI_HOST, &dev, &s_dev);
    if (err != ESP_OK) return err;

    /* After the bus is set up, which routes G35 to the SPI unit as its MISO:
       this makes the pin a plain output again, the display's D/C. */
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << BOARD_LCD_DC) | (1ULL << BOARD_SD_CS),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_set_level(BOARD_SD_CS, 1);      /* keep the SD card off the shared bus */

    s_dma = heap_caps_malloc(BOARD_LCD_W * CHUNK_ROWS * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_dma) return ESP_ERR_NO_MEM;

    hw_lcd_reset();
    cmd(0x11, NULL, 0);                  /* SLPOUT */
    vTaskDelay(pdMS_TO_TICKS(120));
    cmd(0x3A, (const uint8_t[]){ 0x55 }, 1);   /* COLMOD: 16 bit/pixel */
    cmd(0x36, (const uint8_t[]){ 0x08 }, 1);   /* MADCTL: BGR panel, native 320x240 landscape */
    if (BOARD_LCD_INVERT) cmd(0x21, NULL, 0);  /* INVON */
    cmd(0x29, NULL, 0);                  /* DISPON */
    s_up = true;
    ESP_LOGI(TAG, "ILI9342C %dx%d up", BOARD_LCD_W, BOARD_LCD_H);
    return ESP_OK;
}

void lcd_flush(const uint16_t *fb, int stride, int x0, int y0, int x1, int y1)
{
    if (!s_up) return;                   /* the panel didn't start: the firmware runs on without it */
    uint8_t ca[4] = { x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF };
    uint8_t ra[4] = { y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF };
    cmd(0x2A, ca, 4);                    /* CASET */
    cmd(0x2B, ra, 4);                    /* RASET */
    cmd(0x2C, NULL, 0);                  /* RAMWR */
    int w = x1 - x0 + 1;
    int rows_per_chunk = (BOARD_LCD_W * CHUNK_ROWS) / w;
    for (int y = y0; y <= y1; ) {
        int rows = y1 - y + 1 < rows_per_chunk ? y1 - y + 1 : rows_per_chunk;
        uint16_t *d = s_dma;
        for (int r = 0; r < rows; r++) {
            const uint16_t *src = fb + (y + r) * stride + x0;
            for (int i = 0; i < w; i++) d[i] = (uint16_t)((src[i] >> 8) | (src[i] << 8)); /* big-endian on the wire */
            d += w;
        }
        tx(s_dma, (size_t)rows * w * 2, 1);
        y += rows;
    }
}
