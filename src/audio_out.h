/* audio_out.h — cross-platform audio output abstraction for Nava Studio.
 *
 * Supported backends (auto-selected at compile time):
 *   NAVA_AUDIO_PULSE   — PulseAudio  (Linux default)
 *   NAVA_AUDIO_COREAUDIO — CoreAudio (macOS default)
 *   NAVA_AUDIO_WASAPI  — WASAPI      (Windows default)
 *
 * Usage:
 *   NavaAudio *out = nava_audio_open(rate, channels);  // NULL on failure
 *   nava_audio_write(out, pcm_s16, frames);
 *   uint64_t lat_us = nava_audio_latency_us(out);
 *   nava_audio_flush(out);
 *   nava_audio_close(out);
 */
#pragma once
#include <stdint.h>

typedef struct NavaAudio NavaAudio;

NavaAudio *nava_audio_open(int rate, int channels);
void       nava_audio_write(NavaAudio *a, const int16_t *pcm, int frames);
uint64_t   nava_audio_latency_us(NavaAudio *a);
void       nava_audio_flush(NavaAudio *a);
void       nava_audio_close(NavaAudio *a);
