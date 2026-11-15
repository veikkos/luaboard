#include "sim_audio.h"

#include <SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_QUEUED_S 1             /* the computer's speakers lag at most this far (a fast-forward drops the rest) */

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static bool s_speakers;
static SDL_AudioDeviceID s_dev;
static int s_dev_rate, s_dev_ch;
static FILE *s_wav;
static uint32_t s_wav_rate;        /* the file's format, from its first frames (0: none yet) */
static int s_wav_ch;
static uint32_t s_wav_bytes;       /* audio bytes in it */

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

/* The 44-byte header for the format and length so far. */
static void wav_header(void)
{
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + s_wav_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    put16(h + 20, 1);
    put16(h + 22, (uint16_t)s_wav_ch);
    put32(h + 24, s_wav_rate);
    put32(h + 28, s_wav_rate * (uint32_t)s_wav_ch * 2);
    put16(h + 32, (uint16_t)(s_wav_ch * 2));
    put16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    put32(h + 40, s_wav_bytes);
    fseek(s_wav, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, s_wav);
    fseek(s_wav, 0, SEEK_END);
}

void sim_audio_open(bool speakers, const char *wav, bool append)
{
    s_speakers = speakers;
    if (speakers && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "no sound from the computer's speakers: %s\n", SDL_GetError());
        s_speakers = false;
    }
    if (!wav) return;
    if (append && (s_wav = fopen(wav, "r+b"))) {               /* the last boot's recording goes on */
        uint8_t h[44];
        if (fread(h, 1, sizeof h, s_wav) == sizeof h && !memcmp(h, "RIFF", 4)) {
            s_wav_ch = h[22] | h[23] << 8;
            s_wav_rate = h[24] | h[25] << 8 | h[26] << 16 | (uint32_t)h[27] << 24;
            s_wav_bytes = h[40] | h[41] << 8 | h[42] << 16 | (uint32_t)h[43] << 24;
            fseek(s_wav, 0, SEEK_END);
            return;
        }
        fclose(s_wav);
    }
    s_wav = fopen(wav, "wb");
    if (!s_wav) fprintf(stderr, "cannot write %s\n", wav);
}

static void to_speakers(const int16_t *frames, size_t n, int ch, uint32_t rate)
{
    if (!s_dev || s_dev_rate != (int)rate || s_dev_ch != ch) {
        if (s_dev) SDL_CloseAudioDevice(s_dev);
        SDL_AudioSpec want = { .freq = (int)rate, .format = AUDIO_S16SYS, .channels = (Uint8)ch, .samples = 1024 };
        s_dev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
        if (!s_dev) {
            fprintf(stderr, "no sound from the computer's speakers: %s\n", SDL_GetError());
            s_speakers = false;
            return;
        }
        s_dev_rate = (int)rate;
        s_dev_ch = ch;
        SDL_PauseAudioDevice(s_dev, 0);
    }
    Uint32 bytes = (Uint32)(n * (size_t)ch * 2);
    if (SDL_GetQueuedAudioSize(s_dev) + bytes > rate * (Uint32)ch * 2 * MAX_QUEUED_S) return;
    SDL_QueueAudio(s_dev, frames, bytes);
}

void sim_audio_play(const int16_t *frames, size_t n, int ch, uint32_t rate)
{
    static int16_t zeros[1024 * 2];
    pthread_mutex_lock(&s_lock);
    if (s_wav && !s_wav_rate) {
        s_wav_rate = rate;
        s_wav_ch = ch;
        wav_header();
    }
    for (size_t done = 0; done < n;) {
        size_t k = n - done;
        const int16_t *p = frames ? frames + done * (size_t)ch : zeros;
        if (!frames && k > sizeof zeros / sizeof *zeros / (size_t)ch) k = sizeof zeros / sizeof *zeros / (size_t)ch;
        if (s_speakers) to_speakers(p, k, ch, rate);
        if (s_wav && rate == s_wav_rate && ch == s_wav_ch) {
            fwrite(p, (size_t)ch * 2, k, s_wav);              /* little-endian hosts */
            s_wav_bytes += (uint32_t)(k * (size_t)ch * 2);
        }
        done += k;
    }
    if (s_wav && s_wav_rate) {
        wav_header();
        fflush(s_wav);
    }
    pthread_mutex_unlock(&s_lock);
}
