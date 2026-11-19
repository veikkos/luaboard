/* Firmware-wide configuration. */
#pragma once

#define FW_NAME              "luaboard"
/* The version is the build's `git describe` (fw_version(), ota.h). */

/* The Lua API apps are written against (docs/LUA_API.md). APP_LUA_API goes
   up with every change to it, additions too; APP_LUA_API_MIN moves up to
   APP_LUA_API only when a change breaks existing apps. An app declares the
   version it was written for as "lua_api" in app.json (none = 1) and runs
   if APP_LUA_API_MIN <= lua_api <= APP_LUA_API. Version 1 is frozen with
   the first release, 0.9.0: the next change to the API makes it 2. (Lua itself owns
   LUA_API.) */
#define APP_LUA_API          1
#define APP_LUA_API_MIN      1

/* app.json's text (and the marketplace index's copy of it): the most the
   board keeps, in bytes. Longer text is cut, ending in "…". The
   marketplace's checks (its tools/board.json) keep its apps within these,
   and a description within what the screens show of it: APP_DESC_LINES
   lines of APP_DESC_COLS characters in the app's options, and on the
   Marketplace page of an app that needs setup, APP_DESC_QR_LINES of
   APP_DESC_QR_COLS beside the QR code (menu.c). */
#define APP_DESC_LINES       3
#define APP_DESC_COLS        48
#define APP_DESC_QR_LINES    4
#define APP_DESC_QR_COLS     26
#define APP_TITLE_MAX        31
#define APP_DESC_MAX         (APP_DESC_LINES * APP_DESC_COLS * 3)   /* what fits, in up to 3 bytes a character (€) */
#define APP_AUTHOR_MAX       31
#define APP_LICENSE_MAX      23

/* Apps: files are "<app>/<file>". Installed apps live in a LittleFS
   partition (APPS_PARTITION, mounted at APPS_MOUNT); uploads that aren't
   installed yet are kept in RAM (VFS_RAM_*), shadowing the flash copy. */
#define VFS_APP_NAME_MAX     24          /* incl. NUL: app folder names */
#define VFS_FILE_NAME_MAX    32          /* incl. NUL: file names inside an app */
#define VFS_MAX_NAME         (VFS_APP_NAME_MAX + VFS_FILE_NAME_MAX) /* "app/file" incl. NUL */
#define VFS_MAX_APPS         64
#define VFS_RAM_FILES        64          /* uploaded, not installed */
#define VFS_RAM_CAPACITY     (512 * 1024)
#define VFS_FILE_MAX         (256 * 1024) /* one file */
#define APPS_PARTITION       "apps"
#ifndef APPS_MOUNT                        /* the simulator's mock maps it to a host folder */
#define APPS_MOUNT           "/apps"
#endif

/* Home menu and touch buttons. */
#define BUTTON_LONG_MS       800         /* a long press (hold B this long = back) */
#define BUTTON_REPEAT_EVERY  150         /* hold A/C: after the long press, repeat every this many ms */
#define POWER_KEY_POLL_MS    50          /* how often to ask the PMIC about the power key */
#define TOUCH_READ_MS        10          /* how often the touch controller is read */
#define TOUCH_UP_MS          20          /* no finger this long: let go (shorter is a glitch) */
#define TOUCH_SLOP_PX        16          /* a finger further than this from where it went down drags */
#define REMOTE_TOUCH_MS      1000        /* a remote finger (TOUCH command) not repeated this long is let go */
#define BRIGHTNESS_MIN       10          /* settings can't make the menu invisible */

/* Lua runtime limits. */
#define LUA_HEAP_LIMIT       (4 * 1024 * 1024) /* hard cap for the Lua allocator (PSRAM on the S3,
                                            8 MB, one app at a time);
                                            an http body (max 256 KiB) needs twice its size */
#define LUA_WATCHDOG_MS      1000         /* max time a single call may run */
#define LUA_WATCHDOG_HARD_MS 10000        /* ... with slow C calls (sys.delay, disp.jpeg, TLS) */
#define LUA_DEFAULT_RATE_HZ  30           /* loop() call frequency */
#define LUA_MAX_RATE_HZ      100
#define LUA_MAX_DELAY_MS     1000         /* cap for sys.delay() */
#define DISPLAY_DEFAULT_BRIGHTNESS 80     /* % - until the Brightness setting is changed */

/* Sound (sound.c): the speaker's output and what scripts may queue. */
#define SOUND_RATE           44100        /* Hz, the speaker's I2S output */
#define SOUND_QUEUE_MAX      64           /* tones and clips waiting */
#define SOUND_QUEUE_BYTES    (1024 * 1024) /* their audio data together (PSRAM) */
#define SOUND_TONE_MAX_MS    10000
#define SOUND_IDLE_OFF_MS    2000         /* the amplifier goes off this long after the last sound */
#define SOUND_DEFAULT_VOLUME 50           /* % - until the Volume setting is changed */

/* Serial protocol. */
#define PROTO_LINE_MAX       2048         /* long enough for OAuth URLs and codes */
#define PROTO_MSG_MAX        1536         /* one reply / event line */
#define PROTO_UPLOAD_TIMEOUT_MS 3000
#define PROTO_DRAIN_QUIET_MS   500    /* after a timed-out upload: input dropped until this quiet */

/* Firmware updates (ota.c): a new firmware that crashes this many boots in
   a row goes back to the old one (the crash-loop guard skips autostart at
   BOOT_FAIL_LIMIT, before); one that runs this long is proven. The device
   reboots this long after an update is in, so the reply gets out first. */
#define OTA_ROLLBACK_BOOTS   5
#define OTA_STABLE_MS        BOOT_STABLE_MS
#define OTA_REBOOT_DELAY_MS  500
/* The watchdog's timeout while the apps partition is formatted or erased
   (hw_watchdog_stretch): long, but a hang there still resets the device. */
#define WATCHDOG_FORMAT_MS   (5 * 60 * 1000)

/* The update check (update.c): the newest release's manifest, fetched a
   minute after Wi-Fi first comes up, then once a day. A check that failed
   comes again after a part of that day: its 144th (10 minutes), then its
   24th (an hour), then daily. The manifest is the newest release's
   manifest-<board>.json, here (update.c adds the name): each board has its
   own, so the name never has to change. */
#define UPDATE_LATEST_URL     "https://github.com/veikkos/luaboard/releases/latest/download/"
#define UPDATE_HELP_URL       "github.com/veikkos/luaboard/releases"   /* shown when an update can't be pulled */
#define UPDATE_FIRST_CHECK_MS (60 * 1000)
#define UPDATE_CHECK_EVERY_MS (24 * 3600 * 1000)
#define UPDATE_RETRY_1_PART   144
#define UPDATE_RETRY_2_PART   24
#define UPDATE_MANIFEST_MAX   4096
/* The app marketplace (app_update.c): its index at APPS_URL/main/index.json,
   an app version's file at APPS_URL/<commit>/<app>/<file>, on the same
   schedule as the update check. A bigger index fails the check. */
#define APPS_URL              "https://raw.githubusercontent.com/veikkos/luaboard-marketplace"
#define APPS_README_URL       "https://github.com/veikkos/luaboard-marketplace/tree/main/"
#define APPS_INDEX_MAX        (64 * 1024)
/* An install's download still going after this long is given up: a server
   that keeps sending, slowly, is never idle (HTTPC_IDLE_MS). */
#define UPDATE_INSTALL_MAX_MS (10 * 60 * 1000)
/* The HTTP client (httpc.c): a request with no progress this long fails;
   redirects followed at most. */
#define HTTPC_IDLE_MS         15000
#define HTTPC_REDIRECTS_MAX   5

/* Wi-Fi link: TCP server speaking the same protocol as USB, announced via mDNS. */
#define NET_TCP_PORT         3333
#define NET_HOSTNAME         "luaboard"           /* luaboard.local */
#define NET_MDNS_SERVICE     "_luaboard"          /* _luaboard._tcp */
#define NET_SEND_STALL_MS    2000                 /* drop the client if it stops reading */
#define NET_MAX_CLIENTS      3                    /* one logged in + others still logging in */
#define NET_LOGIN_TIMEOUT_MS 60000                /* drop a client that hasn't logged in by then */

/* Crash-loop guard: this many boots in a row that crash while the autostart
   app runs -> skip it (safe mode). One that runs STABLE_MS is forgiven: far
   past the 30 s task watchdog, so a hang anywhere before it still counts. */
#define BOOT_FAIL_LIMIT      3
#define RESTART_NOTICE_MS    10000  /* the "Restarted" screen after a crash, at boot (a tap ends it) */
#define BOOT_STABLE_MS       600000

/* NVS layout. */
#define NVS_NAMESPACE        "luafw"
#define NVS_KEY_AUTOSTART    "autostart"          /* app to start at boot, "" = menu */
#define NVS_KEY_TIMEZONE     "tz"
#define NVS_KEY_BRIGHTNESS   "bright"
#define NVS_KEY_SCREEN_OFF   "scrnoff"            /* minutes without a touch before the screen goes off, 0 never */
#define NVS_KEY_TURN_OFF     "turnoff"            /* minutes asleep before the device turns itself off, 0 never */
#define NVS_KEY_VOLUME       "volume"             /* speaker volume %, 0 off */
#define NVS_KEY_UPDATE_CHECK "upd_check"          /* look for new releases: 1 (default) or 0 */
#define NVS_KEY_WIFI_SSID    "wifi_ssid"
#define NVS_KEY_WIFI_PASS    "wifi_pass"
#define NVS_KEY_BOOT_FAILS   "bootfails"
#define NVS_KEY_RESUME       "resume"             /* the app that ran when the device turned itself off: started at the next boot */
#define NVS_KEY_OTA          "ota"                /* an update until it's proven (ota.c) */
#define NVS_KEY_OTA_FAILED   "ota_failed"         /* the version an update rolled back from */
#define NVS_KEY_WIFI_KEY     "wifikey"            /* pairing key for the Wi-Fi link, see auth.c */
#define NVS_SECRETS_NS       "secrets"            /* secrets.set(): one key per secret */
#define SECRET_MAX_LEN       2048
#define SECRET_MAX_COUNT     32
/* Byte budgets: NVS (64 KB) also holds the pairing key, Wi-Fi, settings, the
   crash-loop counter and Bluetooth bonds, so secrets must never fill it. */
#define SECRETS_APP_BYTES    (8 * 1024)           /* one app's secrets together */
#define SECRETS_TOTAL_BYTES  (16 * 1024)          /* all apps' */
#define STORE_MAX_BYTES      (64 * 1024)          /* store.set(): all of one app's values */
#define STORE_KEY_MAX        32

/* Clock: the time zone is a POSIX TZ string (default: Finland). */
#define NTP_SERVER           "pool.ntp.org"
#define TZ_DEFAULT           "EET-2EEST,M3.5.0/3,M10.5.0/4"
#define TZ_MAX               48

/* Saving power while asleep (sleep.c). POWER_LIGHT_SLEEP 0 keeps the chip
   out of light sleep altogether: for ruling it in or out on a board that
   misbehaves. While light sleep is allowed the main loop waits for the
   next script tick, at most SLEEP_POLL_MS (a touch or the power key is
   seen that late at worst), where it otherwise waits one tick. */
#define POWER_LIGHT_SLEEP    1
#define SLEEP_POLL_MS        50

/* The microSD card (sdcard.c): where its FAT file system is mounted, and how
   long an empty slot is left alone before it's looked at again (a mount
   attempt without a card takes a while). */
#ifndef SD_MOUNT                          /* the simulator's mock maps it to a host folder */
#define SD_MOUNT             "/sd"
#endif
#define SD_RETRY_MS          2000

