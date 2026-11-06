/* nava_dsp.c — live audio DSP for Nava Studio.
 *
 * The decoder writes raw s16le into one pipe; this module owns the loop that
 * reads it, applies EQ / echo / compressor / volume, computes a spectrum and
 * writes to the audio sink.  Parameters are updated under a mutex and are
 * picked up on the next block, so nothing is ever restarted for a knob tweak.
 */
#include <math.h>
#ifndef _WIN32
#include <poll.h>
#endif
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NB 10
#define FFT_N 1024
#define SPEC_N 64
#define WAVE_N 128
#define BLOCK 512
#define PI 3.14159265358979323846

typedef struct { double b0, b1, b2, a1, a2; double z1[2], z2[2]; } Biquad;

typedef struct Dsp {
    int rate, ch;
    pthread_mutex_t lock;
    volatile int stop;
    double frames;

    int eq_enabled;
    double eq_target[NB];
    double eq_cur[NB];

    double volume_target, volume_cur;
    int muted;

    double fade_gain, fade_target, fade_step;

    double echo_intensity, echo_fb;
    int echo_samples;
    float *delay;
    int delay_cap, delay_pos;

    double comp_amount, comp_env;

    /* Reverb: three feedback combs sharing one ring, with damping. */
    double rev_amount;
    float *rev_buf;
    int rev_cap, rev_pos;
    double rev_damp[2];

    /* Flanger: short modulated delay with feedback. */
    double fl_amount, fl_phase;
    float *fl_buf;
    int fl_cap, fl_pos;

    Biquad bands[NB];

    float spectrum[SPEC_N];
    float wave[WAVE_N];
    float level_peak, level_rms;
    double acc_peak, acc_sum;
    long acc_count;
    double re[FFT_N], im[FFT_N], win[FFT_N];
    float mono[FFT_N];
    int mono_pos, mono_fill;
    int lo[SPEC_N], hi[SPEC_N];
    int16_t block[BLOCK * 2];
} Dsp;

static const double EQ_FREQS[NB] = {29, 59, 119, 237, 474, 947, 1889, 3770, 7523, 15011};

static void biquad_peak(Biquad *q, double fs, double f, double gain_db, double Q) {
    double A = pow(10.0, gain_db / 40.0);
    double w0 = 2.0 * PI * f / fs;
    if (w0 > PI * 0.98) w0 = PI * 0.98;
    double cw = cos(w0), sw = sin(w0);
    double alpha = sw / (2.0 * Q);
    double b0 = 1.0 + alpha * A, b1 = -2.0 * cw, b2 = 1.0 - alpha * A;
    double a0 = 1.0 + alpha / A, a1 = -2.0 * cw, a2 = 1.0 - alpha / A;
    q->b0 = b0 / a0; q->b1 = b1 / a0; q->b2 = b2 / a0;
    q->a1 = a1 / a0; q->a2 = a2 / a0;
}

static inline double bq_run(Biquad *q, int c, double x) {
    double y = q->b0 * x + q->z1[c];
    q->z1[c] = q->b1 * x - q->a1 * y + q->z2[c];
    q->z2[c] = q->b2 * x - q->a2 * y;
    return y;
}

/* ---------------------------------------------------------------- FFT ---- */
static void fft(double *re, double *im, int n) {
    int i, j, k, len;
    for (i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (len = 2; len <= n; len <<= 1) {
        double ang = -2.0 * PI / len;
        double wr = cos(ang), wi = sin(ang);
        for (i = 0; i < n; i += len) {
            double cr = 1.0, ci = 0.0;
            for (k = 0; k < len / 2; k++) {
                int a = i + k, b = a + len / 2;
                double xr = re[b] * cr - im[b] * ci;
                double xi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr;        im[a] += xi;
                double nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr; cr = nr;
            }
        }
    }
}

static void spectrum_update(Dsp *d) {
    int i, b;
    double norm = 1.0 / FFT_N;
    double scale = 0.0;
    for (i = 0; i < FFT_N; i++) {
        d->re[i] = (double)d->mono[i] * d->win[i];
        d->im[i] = 0.0;
    }
    fft(d->re, d->im, FFT_N);
    for (b = 0; b < SPEC_N; b++) {
        int lo = d->lo[b], hi = d->hi[b];
        double peak = 0.0;
        if (hi <= lo) hi = lo + 1;
        if (hi > FFT_N / 2) hi = FFT_N / 2;
        for (i = lo; i < hi; i++) {
            double m = d->re[i] * d->re[i] + d->im[i] * d->im[i];
            if (m > peak) peak = m;
        }
        double mag = sqrt(peak) * norm * 4.0;
        double db = 20.0 * log10(mag + 1e-7);
        double unit = (db + 66.0) / 66.0;         /* -66 dB .. 0 dB */
        if (unit < 0.0) unit = 0.0;
        if (unit > 1.0) unit = 1.0;
        unit = pow(unit, 1.35);                    /* lift quiet detail */
        double prev = d->spectrum[b];
        double coef = unit > prev ? 0.62 : 0.16;   /* fast attack, slow decay */
        d->spectrum[b] = (float)(prev + (unit - prev) * coef);
        if (d->spectrum[b] > scale) scale = d->spectrum[b];
    }
    (void)scale;

    /* Down-sampled waveform for oscilloscopes and particle visualizers. */
    for (b = 0; b < WAVE_N; b++) {
        int s0 = b * FFT_N / WAVE_N;
        int s1 = (b + 1) * FFT_N / WAVE_N;
        double peak = 0.0;
        if (s1 <= s0) s1 = s0 + 1;
        for (i = s0; i < s1; i++) {
            double a = fabs(d->mono[i]);
            if (a > peak) peak = a;
        }
        d->wave[b] = (float)(peak > 1.0 ? 1.0 : peak);
    }

    d->mono_fill = 0;
    d->mono_pos = 0;
}

/* ------------------------------------------------------------ processing -- */
static void apply_band_gains(Dsp *d) {
    int k;
    for (k = 0; k < NB; k++) {
        double target = d->eq_enabled ? d->eq_target[k] : 0.0;
        double cur = d->eq_cur[k];
        if (fabs(target - cur) < 0.002) continue;
        cur += (target - cur) * 0.18;              /* ~30 ms glide */
        d->eq_cur[k] = cur;
        biquad_peak(&d->bands[k], (double)d->rate, EQ_FREQS[k], cur, 1.1);
    }
}

static void process_block(Dsp *d, int frames) {
    int i, k, c;
    double vol_target = d->muted ? 0.0 : d->volume_target;
    double step = 1.0 / (d->rate * 0.012);         /* 12 ms volume ramp */
    int echo_on = d->echo_intensity > 0.01 && d->delay && d->echo_samples > 0;
    double comp = d->comp_amount;
    int rev_on = d->rev_amount > 0.01 && d->rev_buf && d->rev_cap > 0;
    int fl_on = d->fl_amount > 0.01 && d->fl_buf && d->fl_cap > 0;
    int rev_tap[3] = {0, 0, 0};
    double rev_amt = d->rev_amount;
    int fl_min = (int)(d->rate * 0.001), fl_span = (int)(d->rate * 0.009);
    if (rev_on) {
        rev_tap[0] = (int)(d->rate * 0.0297);
        rev_tap[1] = (int)(d->rate * 0.0371);
        rev_tap[2] = (int)(d->rate * 0.0411);
        for (int t = 0; t < 3; t++)
            if (rev_tap[t] >= d->rev_cap - 2) rev_tap[t] = d->rev_cap - 3;
    }

    for (i = 0; i < frames; i++) {
        double s[2] = {0.0, 0.0};
        if (d->ch == 1) {
            s[0] = s[1] = d->block[i] / 32768.0;
        } else {
            for (c = 0; c < 2; c++) s[c] = d->block[i * 2 + c] / 32768.0;
        }

        for (k = 0; k < NB; k++) {
            Biquad *q = &d->bands[k];
            for (c = 0; c < 2; c++) s[c] = bq_run(q, c, s[c]);
        }

        if (echo_on) {
            int w = (d->delay_pos + i) % d->delay_cap;
            int r = (w - d->echo_samples + d->delay_cap * 4) % d->delay_cap;
            for (c = 0; c < 2; c++) {
                double wet = d->delay[r * 2 + c];
                double dry = s[c];
                d->delay[w * 2 + c] = (float)(dry + wet * d->echo_fb);
                s[c] = dry + wet * d->echo_intensity;
            }
        }

        if (rev_on) {
            double wet = 0.0;
            for (int t = 0; t < 3; t++) {
                int r = (d->rev_pos - rev_tap[t] + d->rev_cap * 4) % d->rev_cap;
                wet += d->rev_buf[r * 2];
            }
            wet /= 3.0;
            d->rev_damp[0] += (wet - d->rev_damp[0]) * 0.35;
            wet = d->rev_damp[0];
            for (c = 0; c < 2; c++) {
                double dry = s[c];
                if (c == 1) {
                    double wet2 = 0.0;
                    for (int t = 0; t < 3; t++) {
                        int r = (d->rev_pos - rev_tap[t] + d->rev_cap * 4) % d->rev_cap;
                        wet2 += d->rev_buf[r * 2 + 1];
                    }
                    wet2 /= 3.0;
                    d->rev_damp[1] += (wet2 - d->rev_damp[1]) * 0.35;
                    wet = d->rev_damp[1];
                }
                d->rev_buf[d->rev_pos * 2 + c] = (float)(dry + wet * 0.72);
                s[c] = dry * (1.0 - 0.25 * rev_amt) + wet * 0.9 * rev_amt;
            }
            d->rev_pos = (d->rev_pos + 1) % d->rev_cap;
        }

        if (fl_on) {
            d->fl_phase += 2.0 * PI * 0.25 / (double)d->rate;
            if (d->fl_phase > 2.0 * PI) d->fl_phase -= 2.0 * PI;
            for (c = 0; c < 2; c++) {
                double lfo = sin(d->fl_phase + c * PI * 0.5);
                int dly = fl_min + (int)((lfo * 0.5 + 0.5) * fl_span);
                if (dly >= d->fl_cap - 2) dly = d->fl_cap - 3;
                int r = (d->fl_pos - dly + d->fl_cap * 4) % d->fl_cap;
                double dry = s[c];
                double wet = d->fl_buf[r * 2 + c];
                d->fl_buf[d->fl_pos * 2 + c] = (float)(dry + wet * 0.45);
                s[c] = dry + wet * 0.8 * d->fl_amount;
            }
            d->fl_pos = (d->fl_pos + 1) % d->fl_cap;
        }

        if (comp > 0.01) {
            double mag = fabs(s[0]) > fabs(s[1]) ? fabs(s[0]) : fabs(s[1]);
            if (mag > d->comp_env) d->comp_env += (mag - d->comp_env) * 0.35;
            else d->comp_env += (mag - d->comp_env) * 0.02;
            double thr = 0.55 - 0.45 * comp;
            if (d->comp_env > thr && d->comp_env > 1e-6) {
                double ratio = 1.0 + 9.0 * comp;
                double gain = pow(thr / d->comp_env, 1.0 - 1.0 / ratio);
                s[0] *= gain; s[1] *= gain;
            }
        }

        if (d->volume_cur < vol_target) {
            d->volume_cur += step;
            if (d->volume_cur > vol_target) d->volume_cur = vol_target;
        } else if (d->volume_cur > vol_target) {
            d->volume_cur -= step;
            if (d->volume_cur < vol_target) d->volume_cur = vol_target;
        }
        if (d->fade_gain < d->fade_target) {
            d->fade_gain += d->fade_step;
            if (d->fade_gain > d->fade_target) d->fade_gain = d->fade_target;
        } else if (d->fade_gain > d->fade_target) {
            d->fade_gain -= d->fade_step;
            if (d->fade_gain < d->fade_target) d->fade_gain = d->fade_target;
        }
        if (d->ch == 1) {
            double v = (s[0] + s[1]) * 0.5 * d->volume_cur * d->fade_gain;
            if (v > 1.0) v = 1.0;
            if (v < -1.0) v = -1.0;
            d->block[i] = (int16_t)lrint(v * 32767.0);
        } else {
            for (c = 0; c < 2; c++) {
                double v = s[c] * d->volume_cur * d->fade_gain;
                if (v > 1.0) v = 1.0;
                if (v < -1.0) v = -1.0;
                d->block[i * 2 + c] = (int16_t)lrint(v * 32767.0);
            }
        }

        /* Level meters read the signal that actually reaches the speakers. */
        double out_l = d->block[i * d->ch] / 32768.0;
        double out_r = (d->ch > 1) ? (d->block[i * d->ch + 1] / 32768.0) : out_l;
        double amp = fabs(out_l) > fabs(out_r) ? fabs(out_l) : fabs(out_r);
        double frame_rms = (out_l * out_l + out_r * out_r) * 0.5;
        if (amp > d->acc_peak) d->acc_peak = amp;
        d->acc_sum += frame_rms;
        d->acc_count++;

        double mono = (out_l + out_r) * 0.5;
        d->mono[d->mono_pos++] = (float)mono;
        if (d->mono_pos >= FFT_N) {
            if (d->acc_count > 0) {
                double rms = sqrt(d->acc_sum / d->acc_count);
                d->level_peak = (float)(d->acc_peak > 1.0 ? 1.0 : d->acc_peak);
                d->level_rms = (float)(rms > 1.0 ? 1.0 : rms);
            }
            d->acc_peak = 0.0; d->acc_sum = 0.0; d->acc_count = 0;
            spectrum_update(d);
        }
    }
    if (d->delay) d->delay_pos = (d->delay_pos + frames) % d->delay_cap;
    d->frames += frames;
}

/* --------------------------------------------------------------- public -- */
void *nava_dsp_new(int rate, int channels) {
    Dsp *d = calloc(1, sizeof(Dsp));
    if (!d) return NULL;
    d->rate = rate > 8000 ? rate : 48000;
    d->ch = (channels == 1) ? 1 : 2;
    pthread_mutex_init(&d->lock, NULL);
    d->eq_enabled = 1;
    d->volume_target = d->volume_cur = 1.0;
    d->fade_gain = d->fade_target = 1.0;
    d->fade_step = 0.0;
    d->comp_amount = 0.0;
    d->comp_env = 0.0;
    d->delay_cap = d->rate * 2 * 3;          /* 3 s stereo */
    d->delay = calloc((size_t)d->delay_cap * 2, sizeof(float));
    d->rev_cap = d->rate / 10;               /* 100 ms stereo */
    d->rev_buf = calloc((size_t)d->rev_cap * 2, sizeof(float));
    d->fl_cap = d->rate / 50;                /* 20 ms stereo */
    d->fl_buf = calloc((size_t)d->fl_cap * 2, sizeof(float));
    d->echo_samples = (int)(0.12 * d->rate);
    for (int i = 0; i < FFT_N; i++)
        d->win[i] = 0.5 - 0.5 * cos(2.0 * PI * i / (FFT_N - 1));
    for (int b = 0; b < SPEC_N; b++) {
        double f0 = 30.0 * pow(16000.0 / 30.0, (double)b / SPEC_N);
        double f1 = 30.0 * pow(16000.0 / 30.0, (double)(b + 1) / SPEC_N);
        d->lo[b] = (int)(f0 * FFT_N / d->rate);
        d->hi[b] = (int)(f1 * FFT_N / d->rate) + 1;
        if (d->lo[b] < 1) d->lo[b] = 1;
    }
    for (int k = 0; k < NB; k++)
        biquad_peak(&d->bands[k], (double)d->rate, EQ_FREQS[k], 0.0, 1.1);
    return d;
}

void nava_dsp_free(void *h) {
    Dsp *d = (Dsp *)h;
    if (!d) return;
    free(d->delay);
    free(d->rev_buf);
    free(d->fl_buf);
    pthread_mutex_destroy(&d->lock);
    free(d);
}

void nava_dsp_stop(void *h) { ((Dsp *)h)->stop = 1; }

void nava_dsp_set_eq(void *h, const float *gains, int n, int enabled) {
    Dsp *d = (Dsp *)h;
    pthread_mutex_lock(&d->lock);
    d->eq_enabled = enabled ? 1 : 0;
    for (int k = 0; k < NB; k++) {
        double g = (k < n) ? gains[k] : 0.0;
        if (g < -24.0) g = -24.0;
        if (g > 12.0) g = 12.0;
        d->eq_target[k] = g;
    }
    pthread_mutex_unlock(&d->lock);
}

void nava_dsp_set_volume(void *h, float volume, int muted) {
    Dsp *d = (Dsp *)h;
    pthread_mutex_lock(&d->lock);
    d->volume_target = volume;
    d->muted = muted ? 1 : 0;
    pthread_mutex_unlock(&d->lock);
}

void nava_dsp_set_fade(void *h, float gain) {
    Dsp *d = (Dsp *)h;
    if (!d) return;
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 1.0f) gain = 1.0f;
    pthread_mutex_lock(&d->lock);
    d->fade_gain = d->fade_target = gain;
    d->fade_step = 0.0;
    pthread_mutex_unlock(&d->lock);
}

void nava_dsp_fade_to(void *h, float target, double seconds) {
    Dsp *d = (Dsp *)h;
    if (!d) return;
    if (target < 0.0f) target = 0.0f;
    if (target > 1.0f) target = 1.0f;
    pthread_mutex_lock(&d->lock);
    d->fade_target = target;
    if (seconds <= 0.001 || d->fade_gain == target) {
        d->fade_gain = target;
        d->fade_step = 0.0;
    } else {
        double n = seconds * (double)d->rate;
        if (n < 1.0) n = 1.0;
        d->fade_step = fabs(target - d->fade_gain) / n;
    }
    pthread_mutex_unlock(&d->lock);
}

void nava_dsp_set_echo(void *h, float intensity, float feedback, float delay_ms) {
    Dsp *d = (Dsp *)h;
    pthread_mutex_lock(&d->lock);
    d->echo_intensity = intensity;
    d->echo_fb = feedback;
    int samples = (int)(delay_ms * d->rate / 1000.0);
    if (samples < 1) samples = 1;
    if (samples > d->delay_cap - BLOCK) samples = d->delay_cap - BLOCK;
    d->echo_samples = samples;
    pthread_mutex_unlock(&d->lock);
}

void nava_dsp_set_reverb(void *h, float amount) {
    Dsp *d = (Dsp *)h;
    if (!d) return;
    if (amount < 0.0f) amount = 0.0f;
    if (amount > 1.0f) amount = 1.0f;
    pthread_mutex_lock(&d->lock);
    d->rev_amount = amount;
    pthread_mutex_unlock(&d->lock);
}

void nava_dsp_set_flanger(void *h, float amount) {
    Dsp *d = (Dsp *)h;
    if (!d) return;
    if (amount < 0.0f) amount = 0.0f;
    if (amount > 1.0f) amount = 1.0f;
    pthread_mutex_lock(&d->lock);
    d->fl_amount = amount;
    pthread_mutex_unlock(&d->lock);
}

void nava_dsp_set_compressor(void *h, float amount) {
    Dsp *d = (Dsp *)h;
    pthread_mutex_lock(&d->lock);
    d->comp_amount = amount;
    pthread_mutex_unlock(&d->lock);
}

int nava_dsp_spectrum(void *h, float *out, int n) {
    Dsp *d = (Dsp *)h;
    if (!d || !out) return 0;
    if (n > SPEC_N) n = SPEC_N;
    pthread_mutex_lock(&d->lock);
    for (int i = 0; i < n; i++) out[i] = d->spectrum[i];
    pthread_mutex_unlock(&d->lock);
    return n;
}

int nava_dsp_waveform(void *h, float *out, int n) {
    Dsp *d = (Dsp *)h;
    if (!d || !out) return 0;
    if (n > WAVE_N) n = WAVE_N;
    pthread_mutex_lock(&d->lock);
    for (int i = 0; i < n; i++) out[i] = d->wave[i];
    pthread_mutex_unlock(&d->lock);
    return n;
}

void nava_dsp_levels(void *h, float *peak, float *rms) {
    Dsp *d = (Dsp *)h;
    if (!d) return;
    pthread_mutex_lock(&d->lock);
    if (peak) *peak = d->level_peak;
    if (rms) *rms = d->level_rms;
    pthread_mutex_unlock(&d->lock);
}

double nava_dsp_frames(void *h) { return ((Dsp *)h)->frames; }

static int read_full(int fd, int16_t *buf, size_t bytes) {
    size_t got = 0;
    while (got < bytes) {
        ssize_t n = read(fd, (char *)buf + got, bytes - got);
        if (n < 0) {
#ifndef _WIN32
            struct pollfd p = {fd, POLLIN, 0};
            if (poll(&p, 1, 40) <= 0) return (got > 0) ? (int)(got / 2) : -2;
            continue;
#else
            return (got > 0) ? (int)(got / 2) : -2;
#endif
        }
        if (n == 0) return (int)(got / 2);
        got += (size_t)n;
    }
    return (int)(got / 2);
}

static int write_full(int fd, const void *data, size_t bytes) {
    const char *p = (const char *)data;
    size_t sent = 0;
    while (sent < bytes) {
        ssize_t n = write(fd, p + sent, bytes - sent);
        if (n < 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

int nava_dsp_run(void *h, int in_fd, int out_fd) {
    Dsp *d = (Dsp *)h;
    if (!d) return -1;
    while (!d->stop) {
        int samples = read_full(in_fd, d->block, sizeof(d->block));
        if (samples == 0) break;                 /* decoder finished */
        if (samples < 0) {
            if (samples == -2) continue;         /* timeout, re-check stop */
            break;
        }
        int frames = samples / d->ch;
        if (frames <= 0) continue;
        pthread_mutex_lock(&d->lock);
        apply_band_gains(d);
        process_block(d, frames);
        pthread_mutex_unlock(&d->lock);
        if (write_full(out_fd, d->block, (size_t)frames * d->ch * 2) != 0) break;
    }
    return 0;
}

/* ------------------------------------------------------------ in-memory -- */
void nava_dsp_process(void *h, int16_t *buf, int frames) {
    Dsp *d = (Dsp *)h;
    if (!d || !buf || frames <= 0) return;
    if (frames > BLOCK) frames = BLOCK;
    pthread_mutex_lock(&d->lock);
    memcpy(d->block, buf, (size_t)frames * d->ch * sizeof(int16_t));
    apply_band_gains(d);
    process_block(d, frames);
    memcpy(buf, d->block, (size_t)frames * d->ch * sizeof(int16_t));
    pthread_mutex_unlock(&d->lock);
}
