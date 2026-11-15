/*
 * Generic register-file I2C device: the first written byte sets the register
 * pointer, following bytes are written to consecutive registers; reads
 * continue from the pointer. Most simple I2C chips behave like this.
 * Hooks let a model react to writes or produce live values on reads.
 */
#pragma once

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

typedef struct i2c_regs i2c_regs_t;
struct i2c_regs {
    pthread_mutex_t lock;
    uint8_t reg[256];
    uint8_t ptr;
    void (*on_write)(i2c_regs_t *r, uint8_t addr, uint8_t value); /* after store */
    void (*on_read)(i2c_regs_t *r, uint8_t addr);                 /* before load */
    void *user;
};

void i2c_regs_init(i2c_regs_t *r);
void i2c_regs_write(void *ctx, const uint8_t *data, size_t len); /* sim_i2c_write_fn */
void i2c_regs_read(void *ctx, uint8_t *buf, size_t len);         /* sim_i2c_read_fn */
uint8_t i2c_regs_get(i2c_regs_t *r, uint8_t addr);
void i2c_regs_set(i2c_regs_t *r, uint8_t addr, uint8_t value);
