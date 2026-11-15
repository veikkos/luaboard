/*
 * The speaker: a queue of tones and clips, played in order by a thread of
 * its own, so a script never waits for a sound. The amplifier is on while
 * something plays and for SOUND_IDLE_OFF_MS after. Loudness is the Volume
 * setting times each sound's own volume.
 *
 * Call everything except sound_playing() from the main task.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/** Queue a tone (a square wave) of `hz` (0: a rest, silence) for `ms`, at
 *  `volume` % of the Volume setting. NULL if queued, else why not. */
const char *sound_tone(int hz, int ms, int volume);

/** Queue a WAV file's audio (PCM, 8 or 16 bit, mono or stereo), copied.
 *  NULL if queued, else why not ("not a WAV file", ...). */
const char *sound_wav(const void *data, size_t len, int volume);

/** A short beep at `volume` % itself, not a share of the Volume setting:
 *  what a volume being chosen sounds like. Drops what was playing. */
const char *sound_preview(int volume);

/** Drop everything queued and end what's playing. */
void sound_stop(void);

/** Is a sound playing or waiting? */
bool sound_playing(void);

/** From the main loop: the amplifier off once it's been idle long enough. */
void sound_poll(void);
