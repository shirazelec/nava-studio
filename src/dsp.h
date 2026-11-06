#pragma once
#include <stdint.h>

void *nava_dsp_new(int rate, int channels);
void nava_dsp_free(void *h);
void nava_dsp_stop(void *h);
void nava_dsp_set_eq(void *h, const float *gains, int n, int enabled);
void nava_dsp_set_volume(void *h, float volume, int muted);
void nava_dsp_set_fade(void *h, float gain);
void nava_dsp_fade_to(void *h, float target, double seconds);
void nava_dsp_set_echo(void *h, float intensity, float feedback, float delay_ms);
void nava_dsp_set_compressor(void *h, float amount);
void nava_dsp_set_reverb(void *h, float amount);
void nava_dsp_set_flanger(void *h, float amount);
int  nava_dsp_spectrum(void *h, float *out, int n);
int  nava_dsp_waveform(void *h, float *out, int n);
void nava_dsp_levels(void *h, float *peak, float *rms);
double nava_dsp_frames(void *h);
int  nava_dsp_run(void *h, int in_fd, int out_fd);
/* Process an interleaved s16 stereo block in place. frames <= 512. */
void nava_dsp_process(void *h, int16_t *buf, int frames);
