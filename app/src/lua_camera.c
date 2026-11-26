/*
 * Lua API: camera, the board's own (CoreS3 and Lite; the SE has none).
 *
 * camera.draw([x, y, [scale]])  -> w, h      the newest picture onto the screen
 * camera.jpeg([quality])        -> string    ... as a JPEG
 * camera.mirror([on])                        the picture mirrored (true, the default) or not
 * camera.stop()                              off before the script ends
 *
 * The first call switches the camera on (about a second); it is off again
 * when the script ends. Failures are nil, err.
 */
#include "lua_camera.h"

#include <string.h>

#include "board.h"
#include "gfx.h"
#include "hw.h"
#include "jpeg_enc.h"
#include "lua_api.h"
#include "lauxlib.h"
#include "script.h"

#define W BOARD_CAM_W
#define H BOARD_CAM_H

void lua_camera_release(void)
{
    hw_camera_stop();
    hw_camera_mirror(false);
}

static int fail(lua_State *L, esp_err_t err)
{
    lua_pushnil(L);
    lua_pushstring(L, err == ESP_ERR_NOT_FOUND  ? "no camera on this board"
                      : err == ESP_ERR_TIMEOUT ? "the camera gave no picture"
                                               : "the camera doesn't answer");
    return 2;
}

/* The newest picture, the camera started for it (slow: not the script's
   watchdog time). hw_camera_done() when it has been used. */
static esp_err_t picture(lua_State *L, const uint8_t **px)
{
    uint32_t t0 = hw_millis();
    esp_err_t err = hw_camera_start();
    if (err == ESP_OK && !(*px = hw_camera_frame())) err = ESP_ERR_TIMEOUT;
    script_watchdog_credit(L, t0, 6000);
    return err;
}

static int l_draw(lua_State *L)
{
    int x = lua_isnoneornil(L, 1) ? 0 : lua_api_coord(L, 1), y = lua_isnoneornil(L, 2) ? 0 : lua_api_coord(L, 2);
    lua_Integer shrink = luaL_optinteger(L, 3, 1);
    luaL_argcheck(L, shrink == 1 || shrink == 2 || shrink == 4, 3, "scale must be 1, 2 or 4");
    int scale = (int)shrink;
    const uint8_t *px;
    esp_err_t err = picture(L, &px);
    if (err != ESP_OK) return fail(L, err);
    lua_api_drawing();
    int w = W / scale, h = H / scale;
    if (scale == 1) gfx_blit_be565(x, y, W, H, px);
    else {
        uint8_t row[W];                              /* a row of w pixels: every scale-th one */
        for (int r = 0; r < h; r++) {
            const uint8_t *from = px + (size_t)r * (size_t)scale * W * 2;
            for (int c = 0; c < w; c++) memcpy(row + c * 2, from + (size_t)c * (size_t)scale * 2, 2);
            gfx_blit_be565(x, y + r, w, 1, row);
        }
    }
    hw_camera_done();
    lua_pushinteger(L, w);
    lua_pushinteger(L, h);
    return 2;
}

static int l_jpeg(lua_State *L)
{
    lua_Integer quality = luaL_optinteger(L, 1, 80);
    luaL_argcheck(L, quality >= 1 && quality <= 100, 1, "quality must be 1..100");
    size_t cap = (size_t)W * H * 2;                  /* as much as the pixels take: more than a JPEG of them, bar noise at the top qualities */
    uint8_t *out = lua_newuserdatauv(L, cap, 0);     /* Lua's to free, whatever happens */
    const uint8_t *px;
    esp_err_t err = picture(L, &px);
    if (err != ESP_OK) return fail(L, err);
    uint32_t t0 = hw_millis();
    size_t n = jpeg_encode_be565(px, W, H, (int)quality, out, cap);
    hw_camera_done();
    script_watchdog_credit(L, t0, 2000);
    if (!n) {
        lua_pushnil(L);
        lua_pushliteral(L, "the picture is too big as a JPEG of that quality");
        return 2;
    }
    lua_pushlstring(L, (const char *)out, n);
    return 1;
}

static int l_mirror(lua_State *L)
{
    hw_camera_mirror(lua_api_optbool(L, 1, true));
    return 0;
}

static int l_stop(lua_State *L)
{
    (void)L;
    hw_camera_stop();
    return 0;
}

static const luaL_Reg camera_funcs[] = { { "draw", l_draw }, { "jpeg", l_jpeg }, { "mirror", l_mirror },
                                         { "stop", l_stop }, { NULL, NULL } };

void lua_camera_register(lua_State *L)
{
    luaL_newlib(L, camera_funcs);
    lua_setglobal(L, "camera");
}
