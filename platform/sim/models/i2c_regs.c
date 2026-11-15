#include "i2c_regs.h"

#include <string.h>

void i2c_regs_init(i2c_regs_t *r)
{
    memset(r, 0, sizeof *r);
    pthread_mutex_init(&r->lock, NULL);
}

void i2c_regs_write(void *ctx, const uint8_t *d, size_t n)
{
    i2c_regs_t *r = ctx;
    if (!n) return;
    pthread_mutex_lock(&r->lock);
    r->ptr = d[0];
    for (size_t i = 1; i < n; i++) {
        uint8_t a = r->ptr++;
        r->reg[a] = d[i];
        if (r->on_write) r->on_write(r, a, d[i]);
    }
    pthread_mutex_unlock(&r->lock);
}

void i2c_regs_read(void *ctx, uint8_t *buf, size_t n)
{
    i2c_regs_t *r = ctx;
    pthread_mutex_lock(&r->lock);
    for (size_t i = 0; i < n; i++) {
        uint8_t a = r->ptr++;
        if (r->on_read) r->on_read(r, a);
        buf[i] = r->reg[a];
    }
    pthread_mutex_unlock(&r->lock);
}

uint8_t i2c_regs_get(i2c_regs_t *r, uint8_t a)
{
    pthread_mutex_lock(&r->lock);
    uint8_t v = r->reg[a];
    pthread_mutex_unlock(&r->lock);
    return v;
}

void i2c_regs_set(i2c_regs_t *r, uint8_t a, uint8_t v)
{
    pthread_mutex_lock(&r->lock);
    r->reg[a] = v;
    pthread_mutex_unlock(&r->lock);
}
