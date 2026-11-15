/*
 * What the board's speaker plays: through this computer's speakers (in the
 * window) and/or into a WAV file (--wav, for tests). The board calls
 * sim_audio_play with what reaches its speaker.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Before the firmware starts. `wav`: record to this file (NULL: no);
 *  `append`: go on with the file a boot before this one wrote. */
void sim_audio_open(bool speakers, const char *wav, bool append);
/** `n` frames of 16-bit audio, `channels` interleaved, at `rate`; NULL
 *  frames: that long of silence (the amplifier is off). */
void sim_audio_play(const int16_t *frames, size_t n, int channels, uint32_t rate);
