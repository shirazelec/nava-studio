#pragma once
#include <stdint.h>

typedef struct Engine Engine;

Engine *engine_new(void);
void    engine_free(Engine *e);

/* Stops any current media, probes the file and starts playback. Returns 0
 * on success, negative on failure and fills err. */
int  engine_open(Engine *e, const char *path, char *err, int errlen);
void engine_stop(Engine *e);
int  engine_active(Engine *e);
/* Non-zero while a seek (including a fade-out seek) is still in flight. */
int  engine_seek_pending(Engine *e);

void   engine_play(Engine *e);
void   engine_pause(Engine *e);
void   engine_toggle(Engine *e);
int    engine_playing(Engine *e);
void   engine_seek(Engine *e, double seconds);
double engine_position(Engine *e);
double engine_duration(Engine *e);
int    engine_eof(Engine *e);
void   engine_clear_eof(Engine *e);

int  engine_has_video(Engine *e);
int  engine_has_audio(Engine *e);
int  engine_width(Engine *e);
int  engine_height(Engine *e);

void engine_set_volume(Engine *e, float volume, int muted);
void engine_set_eq(Engine *e, const float *gains, int n, int enabled);
void engine_set_echo(Engine *e, float intensity, float feedback, float delay_ms);
void engine_set_compressor(Engine *e, float amount);
void engine_set_reverb(Engine *e, float amount);
void engine_set_flanger(Engine *e, float amount);
void engine_set_rate(Engine *e, float rate);
void engine_set_rotation(Engine *e, int rotation);
void engine_set_video_effects(Engine *e, float brightness, float contrast,
                              float saturation, float gamma, float hue);

/* Click-free gain envelopes and crossfade between audio tracks. */
void   engine_set_fades(Engine *e, int crossfade_enabled,
                        double crossfade_ms, double fade_ms);
void   engine_fade_in(Engine *e);
void   engine_fade_out(Engine *e);
void   engine_seek_faded(Engine *e, double seconds);
void   engine_pause_faded(Engine *e);
void   engine_resume_faded(Engine *e);
void   engine_stop_faded(Engine *e);
/* Starts an equal-power crossfade into path.  Returns 0 when the transition
 * is running and -1 when the caller must fall back to engine_open(). */
int    engine_crossfade_to(Engine *e, const char *path, char *err, int errlen);
/* Duration of the track being crossfaded in, or -1 when idle. */
double engine_crossfade_duration(Engine *e);

/* Pops the next video frame (RGBA, malloc'd, owned by caller). */
int  engine_frame(Engine *e, uint8_t **rgba, int *w, int *h);
/* PTS (seconds) of the next queued video frame, or -1 when empty. */
double engine_next_frame_pts(Engine *e);
/* Attached cover art decoded to RGBA, or 0. */
int  engine_cover(Engine *e, uint8_t **rgba, int *w, int *h);
int  engine_get_spectrum(Engine *e, float *out, int n);
int  engine_get_waveform(Engine *e, float *out, int n);
void engine_get_levels(Engine *e, float *peak, float *rms);
