/*
 * Lua `imu` and `light` modules: the sensors on the board itself. CoreS3 and
 * Lite have them; on the SE every call returns nil and why.
 *
 *   imu.accel() -> x, y, z            acceleration in g (flat on a table: z about 1)
 *   imu.gyro()  -> x, y, z            rotation in degrees a second
 *   light.lux() -> lux                ambient light, roughly
 *   light.proximity() -> 0..2047      how near something is to the screen's top edge: more is nearer
 *
 * A sensor is switched on by the first call that needs it, which takes a
 * moment (a third of a second for imu, a tenth for light), and off again
 * when the script ends.
 */
#include "lua_sensors.h"

#include "hw.h"
#include "lauxlib.h"
#include "script.h"

#define ACCEL_G    (8.0 / 32768.0)             /* a raw step, at +-8 g */
#define GYRO_DPS   (2000.0 / 32768.0)          /* ... at +-2000 degrees a second */

void lua_sensors_release(void)
{
    hw_imu_stop();
    hw_light_stop();
}

static int fail(lua_State *L, esp_err_t err, const char *what)
{
    lua_pushnil(L);
    if (err == ESP_ERR_NOT_FOUND) lua_pushfstring(L, "no %s sensor on this board", what);
    else lua_pushfstring(L, "the %s sensor doesn't answer", what);
    return 2;
}

/* Start a sensor (slow the first time: not the script's watchdog time). */
static esp_err_t start(lua_State *L, esp_err_t (*fn)(void))
{
    uint32_t t0 = hw_millis();
    esp_err_t err = fn();
    script_watchdog_credit(L, t0, 1000);
    return err;
}

static int imu_read(lua_State *L, bool gyro)
{
    int16_t a[3], g[3];
    esp_err_t err = start(L, hw_imu_start);
    if (err == ESP_OK) err = hw_imu_read(a, g);
    if (err != ESP_OK) return fail(L, err, "motion");
    for (int i = 0; i < 3; i++) lua_pushnumber(L, gyro ? g[i] * GYRO_DPS : a[i] * ACCEL_G);
    return 3;
}

static int l_accel(lua_State *L) { return imu_read(L, false); }
static int l_gyro(lua_State *L) { return imu_read(L, true); }

/* The LTR-553's lux formula (its data sheet's appendix), at gain 1 and
   100 ms: by how much of the light is infrared. */
static double lux_of(uint16_t ch0, uint16_t ch1)
{
    if (!ch0 && !ch1) return 0;
    double ratio = (double)ch1 / ((double)ch0 + ch1);
    if (ratio < 0.45) return 1.7743 * ch0 + 1.1059 * ch1;
    if (ratio < 0.64) return 4.2785 * ch0 - 1.9548 * ch1;
    if (ratio < 0.85) return 0.5926 * ch0 + 0.1185 * ch1;
    return 0;
}

static int light_read(lua_State *L, bool proximity)
{
    uint16_t ch0, ch1, ps;
    esp_err_t err = start(L, hw_light_start);
    if (err == ESP_OK) err = hw_light_read(&ch0, &ch1, &ps);
    if (err != ESP_OK) return fail(L, err, "light");
    if (proximity) lua_pushinteger(L, ps);
    else lua_pushnumber(L, lux_of(ch0, ch1));
    return 1;
}

static int l_lux(lua_State *L) { return light_read(L, false); }
static int l_proximity(lua_State *L) { return light_read(L, true); }

static const luaL_Reg imu_funcs[] = { { "accel", l_accel }, { "gyro", l_gyro }, { NULL, NULL } };
static const luaL_Reg light_funcs[] = { { "lux", l_lux }, { "proximity", l_proximity }, { NULL, NULL } };

void lua_sensors_register(lua_State *L)
{
    luaL_newlib(L, imu_funcs);
    lua_setglobal(L, "imu");
    luaL_newlib(L, light_funcs);
    lua_setglobal(L, "light");
}
