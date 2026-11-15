/*
 * ESP32 mock platform - GPIO.
 *
 * Each pin has a mode, pull configuration, output latch and an optional
 * external driver (set by the simulator, e.g. a pressed button pulls to 0).
 * Pad set follows the ESP32-S3 (GPIO0..21, GPIO26..48).
 */
#include <pthread.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "sim_hw.h"

#define NPINS GPIO_NUM_MAX

typedef struct {
    gpio_mode_t mode;
    int pull_up, pull_down;
    int out;      /* output latch */
    int ext;      /* external drive: -1 none, 0/1 level */
} pin_t;

static pin_t s_pins[NPINS];
static int s_inited;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

static int valid_pin(int p) { return GPIO_IS_VALID_GPIO(p); }

static void ensure_init(void)
{
    if (s_inited) return;
    for (int i = 0; i < NPINS; i++) s_pins[i] = (pin_t){ GPIO_MODE_DISABLE, 0, 0, 0, -1 };
    s_inited = 1;
}

static int is_output(gpio_mode_t m) { return (m & GPIO_MODE_OUTPUT) != 0; }
static int is_input(gpio_mode_t m)  { return (m & GPIO_MODE_INPUT) != 0; }

esp_err_t gpio_set_direction(gpio_num_t pin, gpio_mode_t mode)
{
    if (!valid_pin(pin)) return ESP_ERR_INVALID_ARG;
    pthread_mutex_lock(&s_lock);
    ensure_init();
    s_pins[pin].mode = mode;
    pthread_mutex_unlock(&s_lock);
    return ESP_OK;
}

esp_err_t gpio_set_pull_mode(gpio_num_t pin, gpio_pull_mode_t pull)
{
    if (!valid_pin(pin)) return ESP_ERR_INVALID_ARG;
    pthread_mutex_lock(&s_lock);
    ensure_init();
    s_pins[pin].pull_up = (pull == GPIO_PULLUP_ONLY || pull == GPIO_PULLUP_PULLDOWN);
    s_pins[pin].pull_down = (pull == GPIO_PULLDOWN_ONLY || pull == GPIO_PULLUP_PULLDOWN);
    pthread_mutex_unlock(&s_lock);
    return ESP_OK;
}

esp_err_t gpio_config(const gpio_config_t *cfg)
{
    if (!cfg || cfg->pin_bit_mask == 0) return ESP_ERR_INVALID_ARG;
    for (int p = 0; p < NPINS; p++) {
        if (!(cfg->pin_bit_mask & (1ULL << p))) continue;
        esp_err_t err = gpio_set_direction((gpio_num_t)p, cfg->mode);
        if (err != ESP_OK) return err;
        gpio_pull_mode_t pull = GPIO_FLOATING;
        if (cfg->pull_up_en && cfg->pull_down_en) pull = GPIO_PULLUP_PULLDOWN;
        else if (cfg->pull_up_en) pull = GPIO_PULLUP_ONLY;
        else if (cfg->pull_down_en) pull = GPIO_PULLDOWN_ONLY;
        gpio_set_pull_mode((gpio_num_t)p, pull);
    }
    return ESP_OK;
}

esp_err_t gpio_reset_pin(gpio_num_t pin)
{
    if (!valid_pin(pin)) return ESP_ERR_INVALID_ARG;
    pthread_mutex_lock(&s_lock);
    ensure_init();
    s_pins[pin].mode = GPIO_MODE_DISABLE;
    s_pins[pin].pull_up = 1;                 /* as ESP-IDF's: a reset pin isn't left floating */
    s_pins[pin].pull_down = 0;
    s_pins[pin].out = 0;
    pthread_mutex_unlock(&s_lock);
    return ESP_OK;
}

/* Is the pin exactly as gpio_reset_pin leaves it: no output, no input, the
   pull-up alone? */
bool sim_gpio_idle(gpio_num_t pin)
{
    if (!valid_pin(pin)) return false;
    pthread_mutex_lock(&s_lock);
    ensure_init();
    pin_t *p = &s_pins[pin];
    bool idle = p->mode == GPIO_MODE_DISABLE && p->pull_up && !p->pull_down;
    pthread_mutex_unlock(&s_lock);
    return idle;
}

/* What the chip does to the pin: 'o' drives it, 'u' pulls it up, 'd' pulls
   it down, 'f' leaves it floating. */
char sim_gpio_drive_kind(gpio_num_t pin)
{
    if (!valid_pin(pin)) return 'f';
    pthread_mutex_lock(&s_lock);
    ensure_init();
    pin_t *p = &s_pins[pin];
    char k = is_output(p->mode) ? 'o' : p->pull_up ? 'u' : p->pull_down ? 'd' : 'f';
    pthread_mutex_unlock(&s_lock);
    return k;
}

esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    if (!valid_pin(pin)) return ESP_ERR_INVALID_ARG;
    pthread_mutex_lock(&s_lock);
    ensure_init();
    s_pins[pin].out = level ? 1 : 0;
    pthread_mutex_unlock(&s_lock);
    return ESP_OK;
}

int gpio_get_level(gpio_num_t pin)
{
    if (!valid_pin(pin)) return 0;
    pthread_mutex_lock(&s_lock);
    ensure_init();
    pin_t *p = &s_pins[pin];
    int v;
    if (!is_input(p->mode)) {
        v = 0;                                  /* input buffer disabled reads 0 */
    } else if (p->ext >= 0) {
        v = p->ext;                             /* externally driven */
    } else if (is_output(p->mode)) {
        v = p->out;                             /* loopback through input buffer */
    } else {
        v = p->pull_up ? 1 : 0;                 /* floating: pulls decide */
    }
    pthread_mutex_unlock(&s_lock);
    return v;
}

void sim_gpio_drive(gpio_num_t pin, int level)
{
    if (!valid_pin(pin)) return;
    pthread_mutex_lock(&s_lock);
    ensure_init();
    s_pins[pin].ext = level < 0 ? -1 : (level ? 1 : 0);
    pthread_mutex_unlock(&s_lock);
}

int sim_gpio_output_level(gpio_num_t pin)
{
    if (!valid_pin(pin)) return -1;
    pthread_mutex_lock(&s_lock);
    ensure_init();
    int v = is_output(s_pins[pin].mode) ? s_pins[pin].out : -1;
    pthread_mutex_unlock(&s_lock);
    return v;
}
