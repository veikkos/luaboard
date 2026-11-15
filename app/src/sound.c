#include "sound.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "hw.h"
#include "settings.h"

static const char *TAG = "sound";

#define CHUNK       256                          /* frames rendered and written at a time */
#define TAIL_FRAMES 1536                         /* silence after the last sound: pushes it out of the DMA buffers */
#define RAMP_MS     3                            /* a tone fades in and out this long (no clicks) */
#define TONE_LEVEL  0.30f                        /* a tone's amplitude at full volume, of full scale */

typedef struct {
    uint32_t gen;                                /* sound_stop() since: a stale one is skipped */
    int hz;                                      /* tone; 0 = rest */
    uint32_t frames;                             /* length at SOUND_RATE */
    int volume;                                  /* 0..100 */
    bool absolute;                               /* volume isn't a share of the setting (sound_preview) */
    int16_t *pcm;                                /* clip: mono samples (NULL for a tone) */
    size_t n;
    uint32_t rate;
} item_t;

static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cv = PTHREAD_COND_INITIALIZER;
static item_t s_q[SOUND_QUEUE_MAX];
static int s_qh, s_qn;                           /* first item, count */
static size_t s_qbytes;                          /* their clips' samples */
static enum { S_OFF, S_RUN, S_DRAINED } s_state; /* output off / playing / on but idle (since s_idle_at) */
static uint32_t s_idle_at;
static _Atomic uint32_t s_gen;
static bool s_thread;

/* Volume % -> gain: squared, as loudness goes (50 % sounds about half as loud). */
static float gain(int volume) { return (float)volume * (float)volume / 10000.0f; }

static int16_t clamp16(float v) { return v > 32767.f ? 32767 : v < -32768.f ? -32768 : (int16_t)v; }

/* Write one chunk of mono samples to both channels. */
static void out(const int16_t *mono, size_t n)
{
    int16_t st[CHUNK * 2];
    for (size_t i = 0; i < n; i++) st[2 * i] = st[2 * i + 1] = mono[i];
    hw_audio_write(st, n);
}

/* Render one item; false if sound_stop() came meanwhile. */
static bool render(const item_t *it)
{
    int16_t buf[CHUNK];
    float g = it->absolute ? gain(it->volume) : gain(settings_volume()) * gain(it->volume);
    uint32_t ramp = SOUND_RATE * RAMP_MS / 1000;
    uint32_t phase = 0, inc = (uint32_t)((uint64_t)it->hz * 4294967296ULL / SOUND_RATE);
    uint64_t pos = 0, step = it->pcm ? ((uint64_t)it->rate << 32) / SOUND_RATE : 0;   /* 32.32 in the clip */
    for (uint32_t f = 0; f < it->frames;) {
        if (atomic_load(&s_gen) != it->gen) return false;
        size_t n = it->frames - f < CHUNK ? it->frames - f : CHUNK;
        for (size_t i = 0; i < n; i++, f++) {
            if (it->pcm) {
                size_t k = (size_t)(pos >> 32);
                float a = it->pcm[k], b = k + 1 < it->n ? it->pcm[k + 1] : a;
                float frac = (float)(uint32_t)pos / 4294967296.0f;
                buf[i] = clamp16((a + (b - a) * frac) * g);
                pos += step;
            } else if (it->hz) {
                uint32_t left = it->frames - f, e = f < left ? f : left;
                float env = e < ramp ? (float)e / (float)ramp : 1.0f;
                buf[i] = clamp16((phase < 0x80000000u ? 1.f : -1.f) * TONE_LEVEL * 32767.f * g * env);
                phase += inc;
            } else {
                buf[i] = 0;
            }
        }
        out(buf, n);
    }
    return true;
}

static void *player(void *arg)
{
    (void)arg;
    static const int16_t zeros[CHUNK];
    for (;;) {
        pthread_mutex_lock(&s_mu);
        while (s_state != S_RUN) pthread_cond_wait(&s_cv, &s_mu);
        while (s_qn && s_q[s_qh].gen != atomic_load(&s_gen)) {   /* stopped ones */
            free(s_q[s_qh].pcm);
            s_qbytes -= s_q[s_qh].n * sizeof(int16_t);
            s_qh = (s_qh + 1) % SOUND_QUEUE_MAX;
            s_qn--;
        }
        if (!s_qn) {                                              /* the last one is out: then idle */
            pthread_mutex_unlock(&s_mu);
            for (int i = 0; i < TAIL_FRAMES / CHUNK; i++) out(zeros, CHUNK);
            pthread_mutex_lock(&s_mu);
            if (!s_qn && s_state == S_RUN) {
                s_state = S_DRAINED;
                s_idle_at = hw_millis();
            }
            pthread_mutex_unlock(&s_mu);
            continue;
        }
        item_t it = s_q[s_qh];
        pthread_mutex_unlock(&s_mu);
        render(&it);
        pthread_mutex_lock(&s_mu);
        free(it.pcm);
        s_qbytes -= it.n * sizeof(int16_t);
        s_qh = (s_qh + 1) % SOUND_QUEUE_MAX;
        s_qn--;
        pthread_mutex_unlock(&s_mu);
    }
    return NULL;
}

/* Room for one more and `bytes` of samples? With `start`, also starts the
   thread and the output as needed (right before pushing it). NULL if fine.
   Called with s_mu held. */
static const char *make_room(size_t bytes, bool start)
{
    if (s_qn >= SOUND_QUEUE_MAX || s_qbytes + bytes > SOUND_QUEUE_BYTES) return "sound queue full";
    if (!start) return NULL;
    if (!s_thread) {
        pthread_attr_t at;
        pthread_attr_init(&at);
#ifdef ESP_PLATFORM
        pthread_attr_setstacksize(&at, 6144);
#endif
        pthread_t t;
        s_thread = pthread_create(&t, &at, player, NULL) == 0;
        if (s_thread) pthread_detach(t);
        pthread_attr_destroy(&at);
        if (!s_thread) return "no sound thread";
    }
    if (s_state == S_OFF && hw_audio_start() != ESP_OK) return "the speaker didn't start";
    return NULL;
}

static void push(const item_t *it)
{
    s_q[(s_qh + s_qn) % SOUND_QUEUE_MAX] = *it;
    s_qn++;
    s_qbytes += it->n * sizeof(int16_t);
    s_state = S_RUN;
    pthread_cond_signal(&s_cv);
}

const char *sound_tone(int hz, int ms, int volume)
{
    item_t it = { .gen = atomic_load(&s_gen), .hz = hz, .volume = volume,
                  .frames = (uint32_t)((uint64_t)ms * SOUND_RATE / 1000) };
    pthread_mutex_lock(&s_mu);
    const char *err = make_room(0, true);
    if (!err) push(&it);
    pthread_mutex_unlock(&s_mu);
    return err;
}

const char *sound_preview(int volume)
{
    sound_stop();
    if (volume <= 0) return NULL;
    item_t it = { .gen = atomic_load(&s_gen), .hz = 880, .volume = volume, .absolute = true,
                  .frames = (uint32_t)((uint64_t)70 * SOUND_RATE / 1000) };
    pthread_mutex_lock(&s_mu);
    const char *err = make_room(0, true);
    if (!err) push(&it);
    pthread_mutex_unlock(&s_mu);
    return err;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

const char *sound_wav(const void *data, size_t len, int volume)
{
    const uint8_t *p = data, *end = p + len;
    if (len < 12 || memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVE", 4)) return "not a WAV file";
    int fmt = -1, ch = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t *pcm = NULL;
    size_t pcm_len = 0;
    for (const uint8_t *c = p + 12; end - c >= 8;) {
        uint32_t size = le32(c + 4);
        const uint8_t *body = c + 8;
        if (size > (size_t)(end - body)) size = (uint32_t)(end - body);   /* a cut-off file: what's there */
        if (!memcmp(c, "fmt ", 4) && size >= 16) {
            fmt = le16(body);
            ch = le16(body + 2);
            rate = le32(body + 4);
            bits = le16(body + 14);
        } else if (!memcmp(c, "data", 4)) {
            pcm = body;
            pcm_len = size;
        }
        c = body + size + (size & 1);                              /* chunks are padded to even sizes */
    }
    if (fmt != 1 || (ch != 1 && ch != 2) || (bits != 8 && bits != 16))
        return "WAV: only PCM, 8 or 16 bit, mono or stereo";
    if (rate < 4000 || rate > 48000) return "WAV: sample rate must be 4000..48000 Hz";
    size_t frame = (size_t)ch * (size_t)(bits / 8), n = pcm ? pcm_len / frame : 0;
    if (!n) return "WAV: no audio";
    size_t bytes = n * sizeof(int16_t);
    pthread_mutex_lock(&s_mu);
    const char *err = make_room(bytes, false);
    pthread_mutex_unlock(&s_mu);
    if (err) return err;
    int16_t *mono = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mono) return "not enough memory for the sound";
    for (size_t i = 0; i < n; i++) {                               /* to mono, 16-bit */
        const uint8_t *s = pcm + i * frame;
        int sum = 0;
        for (int k = 0; k < ch; k++)
            sum += bits == 16 ? (int16_t)le16(s + 2 * k) : ((int)s[k] - 128) * 256;
        mono[i] = (int16_t)(sum / ch);
    }
    item_t it = { .gen = atomic_load(&s_gen), .volume = volume, .pcm = mono, .n = n, .rate = rate,
                  .frames = (uint32_t)((uint64_t)n * SOUND_RATE / rate) };
    pthread_mutex_lock(&s_mu);
    err = make_room(bytes, true);                                  /* again: the lock was let go */
    if (!err) push(&it);
    pthread_mutex_unlock(&s_mu);
    if (err) free(mono);
    return err;
}

void sound_stop(void)
{
    pthread_mutex_lock(&s_mu);
    atomic_fetch_add(&s_gen, 1);
    pthread_cond_signal(&s_cv);
    pthread_mutex_unlock(&s_mu);
}

bool sound_playing(void)
{
    pthread_mutex_lock(&s_mu);
    uint32_t gen = atomic_load(&s_gen);
    bool any = false;
    for (int i = 0; i < s_qn && !any; i++) any = s_q[(s_qh + i) % SOUND_QUEUE_MAX].gen == gen;
    pthread_mutex_unlock(&s_mu);
    return any;
}

void sound_poll(void)
{
    pthread_mutex_lock(&s_mu);
    bool off = s_state == S_DRAINED && hw_millis() - s_idle_at >= SOUND_IDLE_OFF_MS;
    if (off) s_state = S_OFF;
    pthread_mutex_unlock(&s_mu);
    if (off) {
        hw_audio_stop();
        ESP_LOGD(TAG, "speaker off");
    }
}
