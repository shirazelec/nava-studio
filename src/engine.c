/* engine.c — native FFmpeg playback engine for Nava Studio.
 * libavformat/libavcodec/libswresample decode audio and video directly;
 * audio goes through the in-memory DSP and the platform audio backend. */
#include "engine.h"
#include "dsp.h"
#include "audio_out.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define OUT_RATE 48000
#define OUT_CH 2
#define FRAME_W 960
#define FRAME_H 540
#define QMAX 4
#define MAX_QUEUED_FRAMES 24

typedef struct Frame {
    uint8_t *data;
    int w, h;
    double pts;
    struct Frame *next;
} Frame;

struct Engine {
    pthread_mutex_t lock;
    pthread_t thread;
    int thread_running;
    int thread_done;
    pthread_t vthread;
    int vthread_running;
    volatile int v_done;
    volatile int vseek_pending;
    double v_seek_target;
    AVRational v_tb;
    double video_wall0, video_pts0;
    volatile int stop;
    volatile int playing;
    volatile int eof;

    char path[4096];
    AVFormatContext *fmt;
    int audio_index, video_index;
    AVCodecContext *adec, *vdec;
    SwrContext *swr;
    struct SwsContext *sws;
    int has_video, has_audio;
    int width, height;
    double duration;

    NavaAudio *audio;
    int audio_ok;
    void *dsp;
    int dsp_rate, dsp_ch;

    double seek_req;
    double seek_target;    /* drop decoded frames older than this after a seek */
    int seek_pending;
    double clock;
    double last_audio_pts;
    double lat_avg;
    int lat_n;
    double clock_anchor;
    double anchor_wall;
    double audio_wall0;   /* wall clock at the first written audio of this run */
    double audio_pts0;    /* stream pts corresponding to audio_wall0 */
    int audio_paced;
    double video_clock;

    Frame *head, *tail;
    int qcount;
    uint8_t *frame_buf;
    int frame_buf_w, frame_buf_h;

    uint8_t *cover;
    int cover_w, cover_h;

    float volume;
    int muted;
    float eq_gains[10];
    int eq_enabled;
    float echo_i, echo_fb, echo_ms;
    float rev_amt, fl_amt;
    float comp;
    float fx_brightness, fx_contrast, fx_saturation, fx_gamma, fx_hue;
    float rate;
    float rate_applied;
    int rotation;          /* 0 normal, 1:90, 2:180, 3:270, 4 mirror H, 5 mirror V */
    int video_paced;

    /* Click-free fades and track crossfades.  All of these run on the audio
     * path so they stay sample accurate regardless of UI timing. */
    int cf_enabled;
    double cf_ms, fade_ms;
    double stop_at, pause_at, seek_at;
    double seek_delay_req;
    int fade_after_seek;
    int16_t *pend_buf;
    int pend_cap, pend_n, pend_off;
    int xf_active;
    int xf_pos, xf_len_frames;
    int xf_swap_ready;
    int16_t *xf_buf;
    AVFormatContext *pend_fmt;
    AVCodecContext *pend_adec;
    SwrContext *pend_swr;
    int pend_ai;
    char pend_path[4096];
    double pend_dur;
};

static double now_sec(void) {
    return (double)av_gettime_relative() / 1000000.0;
}

static void queue_clear(Engine *e) {
    Frame *f = e->head;
    while (f) {
        Frame *n = f->next;
        free(f->data);
        free(f);
        f = n;
    }
    e->head = e->tail = NULL;
    e->qcount = 0;
}

static void queue_push(Engine *e, uint8_t *data, int w, int h, double pts) {
    Frame *f = calloc(1, sizeof(*f));
    if (!f) { free(data); return; }
    f->data = data;
    f->w = w; f->h = h; f->pts = pts;
    if (e->tail) e->tail->next = f; else e->head = f;
    e->tail = f;
    e->qcount++;
    while (e->qcount > MAX_QUEUED_FRAMES && e->head) {
        Frame *old = e->head;
        e->head = old->next;
        if (!e->head) e->tail = NULL;
        free(old->data);
        free(old);
        e->qcount--;
    }
}

static int open_decoder(AVFormatContext *fmt, int index, AVCodecContext **out) {
    if (index < 0) return -1;
    AVStream *st = fmt->streams[index];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) return -1;
    AVCodecContext *c = avcodec_alloc_context3(codec);
    if (!c) return -1;
    if (avcodec_parameters_to_context(c, st->codecpar) < 0 ||
        avcodec_open2(c, codec, NULL) < 0) {
        avcodec_free_context(&c);
        return -1;
    }
    c->pkt_timebase = st->time_base;
    *out = c;
    return 0;
}

/* Drops any in-flight crossfade and the preloaded track.  Callers must own
 * the decode threads (or hold e->lock) before calling this. */
static void cancel_crossfade(Engine *e) {
    e->xf_active = 0;
    e->xf_swap_ready = 0;
    e->xf_pos = e->xf_len_frames = 0;
    av_free(e->xf_buf); e->xf_buf = NULL;
    if (e->pend_swr) swr_free(&e->pend_swr);
    if (e->pend_adec) avcodec_free_context(&e->pend_adec);
    if (e->pend_fmt) avformat_close_input(&e->pend_fmt);
    av_free(e->pend_buf); e->pend_buf = NULL;
    e->pend_cap = e->pend_n = e->pend_off = 0;
    e->pend_ai = -1;
}

static void close_media(Engine *e) {
    cancel_crossfade(e);
    e->stop_at = e->pause_at = e->seek_at = 0.0;
    e->seek_delay_req = 0.0;
    e->fade_after_seek = 0;
    if (e->audio) { nava_audio_close(e->audio); e->audio = NULL; }
    e->audio_ok = 0;
    if (e->dsp) { nava_dsp_stop(e->dsp); nava_dsp_free(e->dsp); e->dsp = NULL; }
    if (e->swr) swr_free(&e->swr);
    if (e->sws) sws_freeContext(e->sws), e->sws = NULL;
    if (e->adec) avcodec_free_context(&e->adec);
    if (e->vdec) avcodec_free_context(&e->vdec);
    if (e->fmt) avformat_close_input(&e->fmt);
    queue_clear(e);
    free(e->frame_buf); e->frame_buf = NULL;
    free(e->cover); e->cover = NULL;
    e->cover_w = e->cover_h = 0;
    e->audio_index = e->video_index = -1;
    e->has_audio = e->has_video = 0;
    e->width = e->height = 0;
    e->duration = 0;
    e->clock = 0;
    e->last_audio_pts = 0;
    e->video_clock = 0;
    e->eof = 0;
    e->seek_pending = 0;
    e->seek_target = -1.0;
    e->v_seek_target = -1.0;
    e->vseek_pending = 0;
    e->v_done = 0;
    e->lat_avg = 0.0;
    e->clock_anchor = 0.0;
    e->anchor_wall = 0.0;
    e->audio_wall0 = 0.0;
    e->audio_pts0 = 0.0;
    e->audio_paced = 0;
}

static inline void set_playing(Engine *e, int v) {
    __atomic_store_n(&e->playing, v, __ATOMIC_SEQ_CST);
}
static inline int get_playing(Engine *e) {
    return __atomic_load_n(&e->playing, __ATOMIC_SEQ_CST);
}
static inline int get_stop(Engine *e) {
    return __atomic_load_n(&e->stop, __ATOMIC_SEQ_CST);
}

Engine *engine_new(void) {
    Engine *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    pthread_mutex_init(&e->lock, NULL);
    e->audio_index = e->video_index = -1;
    e->volume = 1.0f;
    e->eq_enabled = 1;
    e->echo_ms = 120.0f;
    e->echo_fb = 0.35f;
    e->fx_contrast = 1.0f;
    e->fx_saturation = 1.0f;
    e->fx_gamma = 1.0f;
    e->rate = 1.0f;
    e->rate_applied = 1.0f;
    e->seek_target = -1.0;
    e->v_seek_target = -1.0;
    e->v_tb = (AVRational){1, 1000};
    e->cf_enabled = 1;
    e->cf_ms = 2000.0;
    e->fade_ms = 120.0;
    e->pend_ai = -1;
    return e;
}

static int rebuild_swr(Engine *e, float rate);

static void decode_cover(Engine *e, int index) {
    AVStream *st = e->fmt->streams[index];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) return;
    AVCodecContext *c = avcodec_alloc_context3(codec);
    if (!c) return;
    if (avcodec_parameters_to_context(c, st->codecpar) < 0 ||
        avcodec_open2(c, codec, NULL) < 0) {
        avcodec_free_context(&c);
        return;
    }
    AVFrame *frm = av_frame_alloc();
    if (!frm) { avcodec_free_context(&c); return; }

    int got_frame = 0;
    if (st->attached_pic.size > 0) {
        if (avcodec_send_packet(c, &st->attached_pic) >= 0 &&
            avcodec_receive_frame(c, frm) >= 0) {
            got_frame = 1;
        }
    } else {
        AVPacket *pkt = av_packet_alloc();
        if (pkt) {
            int tries = 0;
            while (tries++ < 64 && av_read_frame(e->fmt, pkt) >= 0) {
                if (pkt->stream_index == index) {
                    if (avcodec_send_packet(c, pkt) >= 0 &&
                        avcodec_receive_frame(c, frm) >= 0) {
                        got_frame = 1;
                    }
                    av_packet_unref(pkt);
                    break;
                }
                av_packet_unref(pkt);
            }
            av_packet_free(&pkt);
            av_seek_frame(e->fmt, -1, 0, AVSEEK_FLAG_BACKWARD);
        }
    }

    if (got_frame) {
        struct SwsContext *sws = sws_getContext(frm->width, frm->height, frm->format,
                                                 frm->width, frm->height, AV_PIX_FMT_RGBA,
                                                 SWS_BILINEAR, NULL, NULL, NULL);
        if (sws) {
            int stride = frm->width * 4;
            uint8_t *buf = malloc((size_t)stride * frm->height);
            if (buf) {
                uint8_t *dst[4] = { buf, NULL, NULL, NULL };
                int dst_stride[4] = { stride, 0, 0, 0 };
                sws_scale(sws, (const uint8_t * const *)frm->data, frm->linesize, 0, frm->height, dst, dst_stride);
                e->cover = buf;
                e->cover_w = frm->width;
                e->cover_h = frm->height;
            }
            sws_freeContext(sws);
        }
    }
    av_frame_free(&frm);
    avcodec_free_context(&c);
}

int engine_open(Engine *e, const char *path, char *err, int errlen) {
    engine_stop(e);
    pthread_mutex_lock(&e->lock);
    close_media(e);
    pthread_mutex_unlock(&e->lock);

    AVFormatContext *fmt = NULL;
    int rc = avformat_open_input(&fmt, path, NULL, NULL);
    if (rc < 0) {
        av_strerror(rc, err, errlen);
        return rc;
    }
    if (avformat_find_stream_info(fmt, NULL) < 0) {
        avformat_close_input(&fmt);
        snprintf(err, errlen, "cannot read stream info");
        return -1;
    }

    pthread_mutex_lock(&e->lock);
    e->fmt = fmt;
    snprintf(e->path, sizeof(e->path), "%s", path);
    e->duration = fmt->duration > 0 ? (double)fmt->duration / AV_TIME_BASE : 0.0;
    e->audio_index = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    e->video_index = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);

    int cover_index = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        AVStream *st = fmt->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
            (st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            cover_index = (int)i;
            if ((int)i == e->video_index) e->video_index = -1;
        }
    }
    if (cover_index >= 0) decode_cover(e, cover_index);

    if (e->video_index >= 0) {
        if (open_decoder(fmt, e->video_index, &e->vdec) < 0) e->video_index = -1;
        else { e->has_video = 1; e->width = e->vdec->width; e->height = e->vdec->height; }
    }
    if (e->audio_index >= 0) {
        if (open_decoder(fmt, e->audio_index, &e->adec) < 0) e->audio_index = -1;
        else e->has_audio = 1;
    }

    if (e->has_audio) {
        if (rebuild_swr(e, 1.0f) < 0) {
            e->has_audio = 0;
        } else {
            e->dsp = nava_dsp_new(OUT_RATE, OUT_CH);
            if (e->dsp) {
                nava_dsp_set_volume(e->dsp, e->volume, e->muted);
                nava_dsp_set_eq(e->dsp, e->eq_gains, 10, e->eq_enabled);
                nava_dsp_set_echo(e->dsp, e->echo_i, e->echo_fb, e->echo_ms);
                nava_dsp_set_compressor(e->dsp, e->comp);
                nava_dsp_set_reverb(e->dsp, e->rev_amt);
                nava_dsp_set_flanger(e->dsp, e->fl_amt);
            }
            e->audio = nava_audio_open(OUT_RATE, OUT_CH);
            e->audio_ok = e->audio != NULL;
        }
    }
    e->stop_at = e->pause_at = e->seek_at = 0.0;
    e->fade_after_seek = 0;
    if (e->dsp) {
        nava_dsp_set_fade(e->dsp, 0.0f);
        nava_dsp_fade_to(e->dsp, 1.0f, e->fade_ms / 1000.0);
    }
    set_playing(e, 1);
    e->eof = 0;
    __atomic_store_n(&e->stop, 0, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&e->lock);

    if (!e->has_audio && !e->has_video) {
        snprintf(err, errlen, "no playable audio or video stream");
        pthread_mutex_lock(&e->lock);
        close_media(e);
        pthread_mutex_unlock(&e->lock);
        return -1;
    }
    return 0;
}

static int rebuild_swr(Engine *e, float rate) {
    if (!e->adec) return -1;
    if (rate < 0.5f) rate = 0.5f;
    if (rate > 2.0f) rate = 2.0f;
    int in_rate = (int)((e->adec->sample_rate ? e->adec->sample_rate : OUT_RATE) * rate + 0.5);
    if (in_rate < 4000) in_rate = 4000;
    if (in_rate > 384000) in_rate = 384000;
    swr_free(&e->swr);
    AVChannelLayout in_layout = {0};
    if (e->adec->ch_layout.nb_channels > 0)
        av_channel_layout_copy(&in_layout, &e->adec->ch_layout);
    else
        av_channel_layout_default(&in_layout, 2);
    AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
    int rc = swr_alloc_set_opts2(&e->swr, &out_layout, AV_SAMPLE_FMT_S16, OUT_RATE,
                                 &in_layout, e->adec->sample_fmt, in_rate, 0, NULL);
    av_channel_layout_uninit(&in_layout);
    if (rc < 0 || !e->swr || swr_init(e->swr) < 0) {
        swr_free(&e->swr);
        return -1;
    }
    e->rate_applied = rate;
    return 0;
}

static void swap_crossfade(Engine *e);

/* Build a resampler for the preloaded (crossfade-in) track. */
static int pend_rebuild_swr(Engine *e) {
    if (!e->pend_adec) return -1;
    swr_free(&e->pend_swr);
    AVChannelLayout in_layout = {0};
    if (e->pend_adec->ch_layout.nb_channels > 0)
        av_channel_layout_copy(&in_layout, &e->pend_adec->ch_layout);
    else
        av_channel_layout_default(&in_layout, 2);
    AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
    int rc = swr_alloc_set_opts2(&e->pend_swr, &out_layout, AV_SAMPLE_FMT_S16, OUT_RATE,
                                 &in_layout, e->pend_adec->sample_fmt,
                                 e->pend_adec->sample_rate ? e->pend_adec->sample_rate : OUT_RATE,
                                 0, NULL);
    av_channel_layout_uninit(&in_layout);
    if (rc < 0 || !e->pend_swr || swr_init(e->pend_swr) < 0) {
        swr_free(&e->pend_swr);
        return -1;
    }
    return 0;
}

/* Decode up to `frames` interleaved stereo frames from the preloaded track.
 * Resampled output is buffered so no samples are dropped between chunks. */
static int pull_pending(Engine *e, int16_t *dst, int frames) {
    int done = 0;
    if (!e->pend_fmt || !e->pend_adec || !e->pend_swr) return 0;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frm = av_frame_alloc();
    if (!pkt || !frm) { av_packet_free(&pkt); av_frame_free(&frm); return 0; }
    while (done < frames) {
        if (e->pend_off < e->pend_n) {
            int avail = e->pend_n - e->pend_off;
            int take = avail < frames - done ? avail : frames - done;
            memcpy(dst + (size_t)done * OUT_CH,
                   e->pend_buf + (size_t)e->pend_off * OUT_CH,
                   (size_t)take * OUT_CH * sizeof(int16_t));
            e->pend_off += take;
            done += take;
            continue;
        }
        int rc = av_read_frame(e->pend_fmt, pkt);
        if (rc < 0) {
            if (avcodec_send_packet(e->pend_adec, NULL) < 0) break;
        } else if (pkt->stream_index != e->pend_ai) {
            av_packet_unref(pkt);
            continue;
        } else if (avcodec_send_packet(e->pend_adec, pkt) < 0) {
            av_packet_unref(pkt);
            continue;
        }
        av_packet_unref(pkt);
        int produced = 0;
        while (avcodec_receive_frame(e->pend_adec, frm) >= 0) {
            int in_rate = e->pend_adec->sample_rate ? e->pend_adec->sample_rate : OUT_RATE;
            int need = (int)av_rescale_rnd(swr_get_delay(e->pend_swr, in_rate) +
                                           frm->nb_samples, OUT_RATE, in_rate, AV_ROUND_UP);
            if (need < 1) need = 1;
            if (need > e->pend_cap) {
                int16_t *nb = av_realloc_array(e->pend_buf, (size_t)need * OUT_CH,
                                               sizeof(int16_t));
                if (!nb) { av_frame_unref(frm); break; }
                e->pend_buf = nb;
                e->pend_cap = need;
            }
            uint8_t *o[1] = { (uint8_t *)e->pend_buf };
            int n = swr_convert(e->pend_swr, o, e->pend_cap,
                                (const uint8_t **)frm->extended_data, frm->nb_samples);
            av_frame_unref(frm);
            if (n > 0) {
                e->pend_n = n;
                e->pend_off = 0;
                produced = 1;
                break;
            }
        }
        if (!produced) break;
    }
    av_packet_free(&pkt);
    av_frame_free(&frm);
    return done;
}

/* Equal-power blend of the outgoing and incoming tracks. */
static void mix_crossfade(Engine *e, int16_t *buf, int frames) {
    if (!e->xf_active || !e->xf_buf || e->xf_len_frames <= 0) return;
    int n = frames;
    if (e->xf_pos + n > e->xf_len_frames) n = e->xf_len_frames - e->xf_pos;
    if (n <= 0) return;
    memset(e->xf_buf, 0, (size_t)n * OUT_CH * sizeof(int16_t));
    pull_pending(e, e->xf_buf, n);
    for (int i = 0; i < n; i++) {
        double t = (double)(e->xf_pos + i) / (double)e->xf_len_frames;
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
        double out_g = cos(t * 1.5707963267948966);
        double in_g = sin(t * 1.5707963267948966);
        for (int c = 0; c < OUT_CH; c++) {
            double v = buf[i * OUT_CH + c] * out_g + e->xf_buf[i * OUT_CH + c] * in_g;
            if (v > 32767.0) v = 32767.0;
            if (v < -32768.0) v = -32768.0;
            buf[i * OUT_CH + c] = (int16_t)lrint(v);
        }
    }
    e->xf_pos += n;
    if (e->xf_pos >= e->xf_len_frames) e->xf_swap_ready = 1;
}

/* Installs the preloaded track as the active one.  Runs on play_thread, so
 * the demuxer/decoder swap is invisible to the rest of the engine. */
static void swap_crossfade(Engine *e) {
    pthread_mutex_lock(&e->lock);
    if (!e->pend_fmt || !e->pend_adec || !e->pend_swr) {
        e->xf_active = 0;
        e->xf_swap_ready = 0;
        e->xf_pos = e->xf_len_frames = 0;
        av_free(e->xf_buf); e->xf_buf = NULL;
        pthread_mutex_unlock(&e->lock);
        return;
    }
    if (e->swr) swr_free(&e->swr);
    if (e->adec) avcodec_free_context(&e->adec);
    if (e->vdec) avcodec_free_context(&e->vdec);
    if (e->sws) sws_freeContext(e->sws), e->sws = NULL;
    if (e->fmt) avformat_close_input(&e->fmt);
    queue_clear(e);
    free(e->frame_buf); e->frame_buf = NULL;
    free(e->cover); e->cover = NULL;
    e->cover_w = e->cover_h = 0;

    e->fmt = e->pend_fmt; e->pend_fmt = NULL;
    e->adec = e->pend_adec; e->pend_adec = NULL;
    e->swr = e->pend_swr; e->pend_swr = NULL;
    e->audio_index = e->pend_ai;
    e->video_index = -1;
    e->has_audio = 1;
    e->has_video = 0;
    e->width = e->height = 0;
    snprintf(e->path, sizeof(e->path), "%s", e->pend_path);
    e->duration = e->pend_dur;
    e->rate_applied = 1.0f;
    e->seek_target = -1.0;
    e->seek_pending = 0;
    e->vseek_pending = 0;
    e->v_seek_target = -1.0;
    e->clock = 0.0;
    e->last_audio_pts = 0.0;
    e->clock_anchor = 0.0;
    e->anchor_wall = now_sec();
    e->audio_paced = 0;
    e->lat_avg = 0.0;
    e->eof = 0;
    e->xf_active = 0;
    e->xf_swap_ready = 0;
    e->xf_pos = e->xf_len_frames = 0;
    av_free(e->xf_buf); e->xf_buf = NULL;
    if (e->dsp) nava_dsp_set_fade(e->dsp, 1.0f);
    pthread_mutex_unlock(&e->lock);
}

static void drain_audio(Engine *e, AVFrame *frm) {
    if (!e->dsp || !e->adec) return;
    float rate = e->rate > 0.1f ? e->rate : 1.0f;
    if (rate < 0.5f) rate = 0.5f;
    if (rate > 2.0f) rate = 2.0f;
    if (!e->swr || fabsf(rate - e->rate_applied) > 0.001f) {
        if (rebuild_swr(e, rate) < 0) return;
    }
    int in_rate = (int)((e->adec->sample_rate ? e->adec->sample_rate : OUT_RATE) * rate + 0.5);
    int base = (int)av_rescale_rnd(swr_get_delay(e->swr, in_rate) +
                                   frm->nb_samples, OUT_RATE, in_rate, AV_ROUND_UP);
    if (base <= 0) return;
    int cap = base + 4096;
    if (cap < 1024) cap = 1024;
    int16_t *buf = av_malloc_array((size_t)cap * OUT_CH, sizeof(int16_t));
    if (!buf) return;
    uint8_t *out[1] = { (uint8_t *)buf };
    int got = swr_convert(e->swr, out, cap, (const uint8_t **)frm->extended_data, frm->nb_samples);
    double apts = frm->pts != AV_NOPTS_VALUE
        ? (double)frm->pts * av_q2d(e->fmt->streams[e->audio_index]->time_base) : -1.0;
    double adur = (double)frm->nb_samples / (e->adec->sample_rate ? e->adec->sample_rate : OUT_RATE);
    if (e->seek_target >= 0.0) {
        if (apts >= 0.0 && apts + adur < e->seek_target) { av_free(buf); return; }
        if (!e->has_video) {
            e->seek_target = -1.0;
            e->video_paced = 0;
        }
    }
    if (got > 0) {
        int off = 0;
        while (off < got) {
            int chunk = got - off > 512 ? 512 : got - off;
            mix_crossfade(e, buf + (size_t)off * OUT_CH, chunk);
            nava_dsp_process(e->dsp, buf + (size_t)off * OUT_CH, chunk);
            if (e->audio_ok && e->audio) {
                nava_audio_write(e->audio, buf + (size_t)off * OUT_CH, chunk);
            } else {
                usleep((useconds_t)((double)chunk / OUT_RATE * 1000000.0));
            }
            off += chunk;
            if (get_stop(e)) break;
        }
        if (e->xf_swap_ready) swap_crossfade(e);
        double dur = (double)frm->nb_samples / (e->adec->sample_rate ? e->adec->sample_rate : OUT_RATE);
        double pts = frm->pts != AV_NOPTS_VALUE
            ? (double)frm->pts * av_q2d(e->fmt->streams[e->audio_index]->time_base) : e->last_audio_pts;

        /* Pace audio against an absolute wall deadline.  Video conversion
         * runs on this same thread, so without this the decoder would drift
         * ~10% behind real time (and video would stutter with it). */
        double rr0 = e->rate > 0.1f ? e->rate : 1.0f;
        if (e->audio_ok && e->audio) {
            /* The sink already paces us in real time; an extra deadline
             * here would make playback run slightly slow. */
            e->audio_paced = 0;
        } else if (!e->audio_paced) {
            e->audio_wall0 = now_sec();
            e->audio_pts0 = pts;
            e->audio_paced = 1;
        } else {
            double deadline = e->audio_wall0 + (pts + dur - e->audio_pts0) / rr0;
            double wait = deadline - now_sec();
            if (wait > 0.0005) {
                if (wait > 0.25) wait = 0.25;
                av_usleep((int64_t)(wait * 1000000.0));
            }
        }
        /* What we have written is not what is audible yet. */
        double lat = e->lat_avg;
        if (e->audio_ok && e->audio && (++e->lat_n % 8) == 1) {
            uint64_t us = nava_audio_latency_us(e->audio);
            if (us > 0) lat = (double)us / 1000000.0;
        }
        if (lat > 1.0) lat = 1.0;
        if (e->lat_avg <= 0.0) e->lat_avg = lat;
        else e->lat_avg += (lat - e->lat_avg) * 0.08;
        double audible = pts + dur - e->lat_avg;
        if (audible < 0.0) audible = 0.0;
        pthread_mutex_lock(&e->lock);
        e->last_audio_pts = pts;
        if (e->seek_target >= 0.0 && audible < e->seek_req) audible = e->seek_req;
        double tnow = now_sec(), rr = e->rate > 0.1f ? e->rate : 1.0f;
        if (e->anchor_wall <= 0.0 || fabs(audible - e->clock_anchor) > 0.5) e->clock_anchor = audible;
        else {
            double dt = tnow - e->anchor_wall;
            if (dt > 0.25) dt = 0.25;
            double err = audible - (e->clock_anchor + dt * rr);
            if (err > 0.06) err = 0.06;
            if (err < -0.06) err = -0.06;
            e->clock_anchor += dt * rr + err * 0.3;
        }
        e->anchor_wall = tnow;
        e->clock = e->clock_anchor;
        pthread_mutex_unlock(&e->lock);
    }
    av_free(buf);
}

static void apply_video_fx(Engine *e, uint8_t *px, int n) {
    float br = e->fx_brightness, co = e->fx_contrast;
    float sa = e->fx_saturation, ga = e->fx_gamma, hue = e->fx_hue;
    int active = fabsf(br) > 0.001f || fabsf(co - 1.0f) > 0.001f ||
                 fabsf(sa - 1.0f) > 0.001f || fabsf(ga - 1.0f) > 0.001f ||
                 fabsf(hue) > 0.5f;
    if (!active) return;
    double rad = (double)hue * 3.141592653589793 / 180.0;
    double cs = cos(rad), sn = sin(rad);

    uint8_t gamma_lut[256];
    int use_gamma = fabsf(ga - 1.0f) > 0.001f && ga > 0.01f;
    if (use_gamma) {
        double inv_ga = 1.0 / (double)ga;
        for (int k = 0; k < 256; k++) {
            double v = pow((double)k / 255.0, inv_ga) * 255.0 + 0.5;
            if (v < 0.0) v = 0.0;
            else if (v > 255.0) v = 255.0;
            gamma_lut[k] = (uint8_t)v;
        }
    }

    for (int i = 0; i < n; i++, px += 4) {
        double r = px[0] / 255.0, g = px[1] / 255.0, b = px[2] / 255.0;
        r = (r - 0.5) * co + 0.5 + br * 0.5;
        g = (g - 0.5) * co + 0.5 + br * 0.5;
        b = (b - 0.5) * co + 0.5 + br * 0.5;
        if (r < 0.0) r = 0.0; else if (r > 1.0) r = 1.0;
        if (g < 0.0) g = 0.0; else if (g > 1.0) g = 1.0;
        if (b < 0.0) b = 0.0; else if (b > 1.0) b = 1.0;

        if (use_gamma) {
            int ir = (int)(r * 255.0 + 0.5);
            int ig = (int)(g * 255.0 + 0.5);
            int ib = (int)(b * 255.0 + 0.5);
            r = gamma_lut[ir < 0 ? 0 : (ir > 255 ? 255 : ir)] / 255.0;
            g = gamma_lut[ig < 0 ? 0 : (ig > 255 ? 255 : ig)] / 255.0;
            b = gamma_lut[ib < 0 ? 0 : (ib > 255 ? 255 : ib)] / 255.0;
        }
        double luma = 0.299 * r + 0.587 * g + 0.114 * b;
        r = luma + (r - luma) * sa;
        g = luma + (g - luma) * sa;
        b = luma + (b - luma) * sa;
        if (fabsf(hue) > 0.5f) {
            double Y = 0.299 * r + 0.587 * g + 0.114 * b;
            double I = 0.596 * r - 0.274 * g - 0.322 * b;
            double Q = 0.211 * r - 0.523 * g + 0.312 * b;
            double I2 = I * cs - Q * sn, Q2 = I * sn + Q * cs;
            r = Y + 0.956 * I2 + 0.621 * Q2;
            g = Y - 0.272 * I2 - 0.647 * Q2;
            b = Y - 1.106 * I2 + 1.703 * Q2;
        }
        if (r < 0.0) r = 0.0; else if (r > 1.0) r = 1.0;
        if (g < 0.0) g = 0.0; else if (g > 1.0) g = 1.0;
        if (b < 0.0) b = 0.0; else if (b > 1.0) b = 1.0;
        px[0] = (uint8_t)(r * 255.0 + 0.5);
        px[1] = (uint8_t)(g * 255.0 + 0.5);
        px[2] = (uint8_t)(b * 255.0 + 0.5);
    }
}


/* Rotate / mirror an RGBA frame.  rot: 1=90, 2=180, 3=270, 4=mirror H, 5=mirror V */
static void rotate_rgba(const uint8_t *src, int w, int h, int rot,
                        uint8_t **out, int *ow, int *oh) {
    int swap = (rot == 1 || rot == 3);
    int nw = swap ? h : w, nh = swap ? w : h;
    uint8_t *dst = malloc((size_t)nw * nh * 4);
    if (!dst) return;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int dx = x, dy = y;
            switch (rot) {
            case 1: dx = h - 1 - y; dy = x; break;
            case 2: dx = w - 1 - x; dy = h - 1 - y; break;
            case 3: dx = y; dy = w - 1 - x; break;
            case 4: dx = w - 1 - x; dy = y; break;
            case 5: dx = x; dy = h - 1 - y; break;
            default: break;
            }
            memcpy(dst + ((size_t)dy * nw + dx) * 4,
                   src + ((size_t)y * w + x) * 4, 4);
        }
    }
    *out = dst;
    *ow = nw;
    *oh = nh;
}

static void drain_video(Engine *e, AVFrame *frm) {
    if (!frm || frm->width <= 0 || frm->height <= 0) return;
    int target_w = frm->width;
    int target_h = frm->height;
    if (target_w > FRAME_W || target_h > FRAME_H) {
        double scale_w = (double)FRAME_W / target_w;
        double scale_h = (double)FRAME_H / target_h;
        double scale = scale_w < scale_h ? scale_w : scale_h;
        target_w = (int)(target_w * scale);
        target_h = (int)(target_h * scale);
    }
    target_w = (target_w / 2) * 2;
    target_h = (target_h / 2) * 2;
    if (target_w < 16) target_w = 16;
    if (target_h < 16) target_h = 16;

    if (!e->sws || e->frame_buf_w != target_w || e->frame_buf_h != target_h) {
        if (e->sws) sws_freeContext(e->sws);
        e->sws = sws_getContext(frm->width, frm->height, frm->format, target_w, target_h,
                                AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
        if (!e->sws) return;
        free(e->frame_buf);
        e->frame_buf = malloc((size_t)target_w * target_h * 4);
        e->frame_buf_w = target_w;
        e->frame_buf_h = target_h;
    }
    if (!e->frame_buf) return;
    uint8_t *dst[4] = { e->frame_buf, NULL, NULL, NULL };
    int dst_stride[4] = { target_w * 4, 0, 0, 0 };
    sws_scale(e->sws, (const uint8_t * const *)frm->data, frm->linesize, 0, frm->height, dst, dst_stride);
    int out_w = target_w, out_h = target_h;
    double pts = 0;
    if (frm->pts != AV_NOPTS_VALUE)
        pts = (double)frm->pts * av_q2d(e->v_tb);

    /* A backward seek decodes from the previous keyframe.  Those pictures
     * belong to the old position and must not be shown, otherwise the UI
     * receives a burst of frames at once (visible as a stutter). */
    if (e->v_seek_target >= 0.0) {
        if (frm->pts != AV_NOPTS_VALUE && pts + 0.001 < e->v_seek_target) return;
        e->v_seek_target = -1.0;
        e->video_pts0 = pts;
        e->video_wall0 = now_sec();
        e->video_paced = 0;
    }

    uint8_t *copy = malloc((size_t)target_w * target_h * 4);
    if (!copy) return;
    memcpy(copy, e->frame_buf, (size_t)target_w * target_h * 4);
    apply_video_fx(e, copy, target_w * target_h);
    if (e->rotation) {
        uint8_t *rot = NULL;
        rotate_rgba(copy, target_w, target_h, e->rotation, &rot, &out_w, &out_h);
        if (rot) { free(copy); copy = rot; }
    }

    /* Video-only files have no audio sink to pace them; sleep against pts. */
    if (!e->has_audio && pts >= 0.0) {
        if (!e->video_paced) {
            e->video_pts0 = pts;
            e->video_wall0 = now_sec();
            e->video_paced = 1;
        } else {
            double r = e->rate > 0.1f ? e->rate : 1.0;
            double target = e->video_wall0 + (pts - e->video_pts0) / r;
            double wait = target - now_sec();
            if (wait > 0.001) av_usleep((int64_t)(wait * 1000000.0));
        }
    }
    /* On A/V files the picture thread now runs independently, so it must
     * pace itself against the audio clock; otherwise it decodes the whole
     * file in a few seconds and the bounded queue keeps only the tail. */
    if (e->has_audio && pts >= 0.0) {
        for (int spin = 0; spin < 400 && !get_stop(e); spin++) {
            double clk = engine_position(e);
            if (!get_playing(e) || pts - clk < 0.080) break;
            av_usleep(5000);
        }
    }
    pthread_mutex_lock(&e->lock);
    queue_push(e, copy, out_w, out_h, pts);
    e->video_clock = pts;
    if (!e->has_audio) {
        e->clock = pts;
        e->clock_anchor = pts;
        e->anchor_wall = now_sec();
    }
    pthread_mutex_unlock(&e->lock);
}

static void decode_packet(Engine *e, AVPacket *pkt) {
    AVFrame *frm = av_frame_alloc();
    if (!frm) return;
    if (pkt->stream_index == e->audio_index && e->adec) {
        if (avcodec_send_packet(e->adec, pkt) >= 0) {
            while (avcodec_receive_frame(e->adec, frm) >= 0) {
                drain_audio(e, frm);
                av_frame_unref(frm);
                if (get_stop(e)) break;
            }
        }
    }
    av_frame_free(&frm);
}

static void do_seek(Engine *e) {
    if (!e->fmt) return;
    int64_t ts = (int64_t)(e->seek_req * AV_TIME_BASE);
    if (avformat_seek_file(e->fmt, -1, INT64_MIN, ts, INT64_MAX, 0) < 0 &&
        av_seek_frame(e->fmt, -1, ts, AVSEEK_FLAG_BACKWARD) < 0)
        return;
    if (e->adec) avcodec_flush_buffers(e->adec);
    double target = e->seek_req;
    pthread_mutex_lock(&e->lock);
    queue_clear(e);
    e->clock = target;
    e->last_audio_pts = target;
    e->video_clock = target;
    e->seek_target = target;
    e->lat_avg = 0.0;
    e->clock_anchor = target;
    e->anchor_wall = now_sec();
    e->audio_paced = 0;
    e->video_pts0 = 0.0;
    e->video_wall0 = now_sec();
    e->eof = 0;
    pthread_mutex_unlock(&e->lock);
    /* Throw away audio queued at the old position. */
    if (e->audio_ok && e->audio) nava_audio_flush(e->audio);
    if (e->fade_after_seek) {
        e->fade_after_seek = 0;
        /* If the user seeked while paused, stay silent: engine_play() will
         * ramp the gain back up on resume. */
        if (e->dsp) {
            if (get_playing(e))
                nava_dsp_fade_to(e->dsp, 1.0f, e->fade_ms / 1000.0);
            else
                nava_dsp_set_fade(e->dsp, 0.0f);
        }
    }
}

/* ------------------------------------------------------- video thread -- */
/* Video gets its own demuxer + decoder so colour conversion never shares a
 * thread with the PulseAudio writer (that coupling made the whole clock run
 * ~10% slow, which showed up as periodic video stutter). */
static void *video_thread(void *arg) {
    Engine *e = arg;
    AVFormatContext *fmt = NULL;
    AVCodecContext *vdec = NULL;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frm = av_frame_alloc();
    if (!pkt || !frm) goto done;
    if (avformat_open_input(&fmt, e->path, NULL, NULL) < 0) goto done;
    if (avformat_find_stream_info(fmt, NULL) < 0) goto done;
    int vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vi < 0) goto done;
    {
        AVStream *st = fmt->streams[vi];
        if (st->disposition & AV_DISPOSITION_ATTACHED_PIC) goto done;
        const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!codec) goto done;
        vdec = avcodec_alloc_context3(codec);
        if (!vdec) goto done;
        if (avcodec_parameters_to_context(vdec, st->codecpar) < 0) goto done;
        if (avcodec_open2(vdec, codec, NULL) < 0) goto done;
        e->v_tb = st->time_base;
        e->v_done = 0;
    }

    while (!get_stop(e)) {
        if (__atomic_exchange_n(&e->vseek_pending, 0, __ATOMIC_SEQ_CST)) {
            pthread_mutex_lock(&e->lock);
            double tgt = e->v_seek_target;
            queue_clear(e);
            pthread_mutex_unlock(&e->lock);

            int64_t ts = (int64_t)(tgt / av_q2d(e->v_tb));
            if (avformat_seek_file(fmt, vi, INT64_MIN, ts, INT64_MAX, 0) < 0 &&
                av_seek_frame(fmt, vi, ts, AVSEEK_FLAG_BACKWARD) < 0) {
                /* keep going; the old position is still valid */
            }
            avcodec_flush_buffers(vdec);

            if (!get_playing(e)) {
                while (!get_stop(e) && !get_playing(e)) {
                    if (__atomic_load_n(&e->vseek_pending, __ATOMIC_SEQ_CST)) break;
                    int rc = av_read_frame(fmt, pkt);
                    if (rc < 0) break;
                    if (pkt->stream_index == vi) {
                        if (avcodec_send_packet(vdec, pkt) >= 0) {
                            int got = 0;
                            while (avcodec_receive_frame(vdec, frm) >= 0) {
                                drain_video(e, frm);
                                av_frame_unref(frm);
                                got = 1;
                            }
                            if (got) {
                                av_packet_unref(pkt);
                                break;
                            }
                        }
                    }
                    av_packet_unref(pkt);
                }
            }
        }
        if (!get_playing(e)) { usleep(15000); continue; }

        int rc = av_read_frame(fmt, pkt);
        if (rc < 0) {
            avcodec_send_packet(vdec, NULL);
            while (avcodec_receive_frame(vdec, frm) >= 0) {
                drain_video(e, frm);
                av_frame_unref(frm);
            }
            if (e->has_audio) e->v_done = 0;   /* audio owns EOF when present */
            else e->v_done = 1;
            break;
        }
        if (pkt->stream_index == vi) {
            if (avcodec_send_packet(vdec, pkt) >= 0) {
                while (avcodec_receive_frame(vdec, frm) >= 0) {
                    drain_video(e, frm);
                    av_frame_unref(frm);
                }
            }
        }
        av_packet_unref(pkt);
    }

done:
    av_packet_free(&pkt);
    av_frame_free(&frm);
    if (vdec) avcodec_free_context(&vdec);
    if (fmt) avformat_close_input(&fmt);
    return NULL;
}

static void *play_thread(void *arg) {
    Engine *e = arg;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frm = av_frame_alloc();
    if (!pkt || !frm) goto done;

    while (!get_stop(e)) {
        double tnow = now_sec();
        if (e->stop_at > 0.0 && tnow >= e->stop_at) { e->stop_at = 0.0; break; }
        if (e->pause_at > 0.0 && tnow >= e->pause_at) {
            e->pause_at = 0.0;
            if (e->dsp) nava_dsp_set_fade(e->dsp, 0.0f);
            set_playing(e, 0);
        }
        if (e->seek_at > 0.0 && tnow >= e->seek_at) {
            e->seek_at = 0.0;
            pthread_mutex_lock(&e->lock);
            e->seek_req = e->seek_delay_req;
            e->v_seek_target = e->seek_delay_req;
            pthread_mutex_unlock(&e->lock);
            __atomic_store_n(&e->seek_pending, 1, __ATOMIC_SEQ_CST);
            __atomic_store_n(&e->vseek_pending, 1, __ATOMIC_SEQ_CST);
        }
        if (__atomic_exchange_n(&e->seek_pending, 0, __ATOMIC_SEQ_CST)) do_seek(e);
        /* A pending pause keeps decoding so the outgoing audio is actually
         * heard fading out instead of being cut mid-ramp. */
        int fading_out = e->pause_at > 0.0 || e->stop_at > 0.0;
        if (!get_playing(e) && !fading_out) { usleep(15000); continue; }
        if (!e->has_audio && e->has_video) {
            /* Video-only: the picture thread owns pacing and end-of-file. */
            usleep(20000);
            if (e->v_done) { e->eof = 1; set_playing(e, 0); break; }
            continue;
        }

        int rc = av_read_frame(e->fmt, pkt);
        if (rc < 0) {
            if (e->adec) {
                avcodec_send_packet(e->adec, NULL);
                while (avcodec_receive_frame(e->adec, frm) >= 0) { drain_audio(e, frm); av_frame_unref(frm); }
            }
            /* Outgoing track ran dry while an equal-power blend is still in
             * flight.  Feed silence on the outgoing side so mix_crossfade can
             * finish the ramp; swap_crossfade then installs the next track and
             * this same thread keeps running on it. */
            if (e->xf_active || e->xf_swap_ready) {
                while (e->xf_active && !e->xf_swap_ready && !get_stop(e)) {
                    int16_t sil[OUT_CH * 512];
                    memset(sil, 0, sizeof(sil));
                    mix_crossfade(e, sil, 512);
                    if (e->dsp) nava_dsp_process(e->dsp, sil, 512);
                    if (e->audio_ok && e->audio) nava_audio_write(e->audio, sil, 512);
                    else usleep((useconds_t)(512.0 / OUT_RATE * 1000000.0));
                    if (e->xf_swap_ready) swap_crossfade(e);
                }
                if (e->xf_active && !e->xf_swap_ready) {
                    /* Blend could not finish (pending stream vanished). */
                    cancel_crossfade(e);
                    e->eof = 1;
                    set_playing(e, 0);
                    break;
                }
                if (get_stop(e)) break;
                continue;
            }
            e->eof = 1;
            set_playing(e, 0);
            break;
        }
        decode_packet(e, pkt);
        av_packet_unref(pkt);
    }
done:
    av_packet_free(&pkt);
    av_frame_free(&frm);
    e->thread_done = 1;
    set_playing(e, 0);
    return NULL;
}

void engine_play(Engine *e) {
    pthread_mutex_lock(&e->lock);
    if (!e->fmt) { pthread_mutex_unlock(&e->lock); return; }
    if (e->thread_running && e->thread_done) {
        pthread_mutex_unlock(&e->lock);
        pthread_join(e->thread, NULL);
        pthread_mutex_lock(&e->lock);
        e->thread_running = 0;
    }
    if (e->vthread_running && (e->v_done || e->thread_done)) {
        pthread_mutex_unlock(&e->lock);
        pthread_join(e->vthread, NULL);
        pthread_mutex_lock(&e->lock);
        e->vthread_running = 0;
    }
    if (e->thread_running) {
        /* Pause keeps the decode thread parked; just unpark it. */
        e->pause_at = e->stop_at = 0.0;
        if (e->dsp) nava_dsp_fade_to(e->dsp, 1.0f, e->fade_ms / 1000.0);
        __atomic_store_n(&e->stop, 0, __ATOMIC_SEQ_CST);
        set_playing(e, 1);
        e->eof = 0;
        if (e->has_video && !e->vthread_running) {
            e->v_done = 0;
            if (pthread_create(&e->vthread, NULL, video_thread, e) == 0) e->vthread_running = 1;
        }
        pthread_mutex_unlock(&e->lock);
        return;
    }
    /* If playback already ran to the end (decode threads exited at EOF),
     * rewind to the start so pressing Play again restarts the media. */
    if (e->duration > 0.0 && e->clock >= e->duration - 0.05) {
        e->seek_req = 0.0;
        e->v_seek_target = 0.0;
        e->clock = 0.0;
        e->seek_target = 0.0;
        e->last_audio_pts = 0.0;
        __atomic_store_n(&e->seek_pending, 1, __ATOMIC_SEQ_CST);
        __atomic_store_n(&e->vseek_pending, 1, __ATOMIC_SEQ_CST);
    }
    e->pause_at = e->stop_at = 0.0;
    if (e->dsp) nava_dsp_fade_to(e->dsp, 1.0f, e->fade_ms / 1000.0);
    __atomic_store_n(&e->stop, 0, __ATOMIC_SEQ_CST);
    set_playing(e, 1);
    e->eof = 0;
    e->thread_done = 0;
    if (pthread_create(&e->thread, NULL, play_thread, e) == 0) e->thread_running = 1;
    if (e->has_video) {
        e->v_done = 0;
        if (pthread_create(&e->vthread, NULL, video_thread, e) == 0) e->vthread_running = 1;
    }
    pthread_mutex_unlock(&e->lock);
}

void engine_pause(Engine *e) { set_playing(e, 0); }

void engine_toggle(Engine *e) {
    if (get_playing(e)) { set_playing(e, 0); return; }
    if (e->fmt && e->eof) { engine_seek(e, 0); e->eof = 0; }
    engine_play(e);
}

int engine_playing(Engine *e) { return get_playing(e); }

void engine_seek(Engine *e, double seconds) {
    if (seconds < 0) seconds = 0;
    pthread_mutex_lock(&e->lock);
    if (e->duration > 0 && seconds > e->duration) seconds = e->duration;
    e->seek_req = seconds;
    e->v_seek_target = seconds;
    __atomic_store_n(&e->vseek_pending, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&e->seek_pending, 1, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&e->lock);
}

double engine_position(Engine *e) {
    pthread_mutex_lock(&e->lock);
    double p = e->clock;
    if (e->anchor_wall > 0.0 && get_playing(e)) {
        double dt = now_sec() - e->anchor_wall;
        double r = e->rate > 0.1f ? e->rate : 1.0f;
        if (dt > 0.25) dt = 0.25;
        if (dt > 0.0 && e->clock_anchor + dt * r > p) p = e->clock_anchor + dt * r;
    }
    pthread_mutex_unlock(&e->lock);
    return p;
}

double engine_duration(Engine *e) { return e->duration; }
int engine_eof(Engine *e) { return e->eof; }
void engine_clear_eof(Engine *e) { e->eof = 0; }
int engine_has_video(Engine *e) { return e->has_video; }
int engine_has_audio(Engine *e) { return e->has_audio; }
int engine_width(Engine *e) { return e->width; }
int engine_height(Engine *e) { return e->height; }
int engine_active(Engine *e) { return e->fmt != NULL; }

int engine_seek_pending(Engine *e) {
    if (!e) return 0;
    if (__atomic_load_n(&e->seek_pending, __ATOMIC_SEQ_CST)) return 1;
    pthread_mutex_lock(&e->lock);
    int pending = e->seek_at > 0.0;
    pthread_mutex_unlock(&e->lock);
    return pending;
}

void engine_set_volume(Engine *e, float volume, int muted) {
    e->volume = volume; e->muted = muted;
    if (e->dsp) nava_dsp_set_volume(e->dsp, volume, muted);
}

void engine_set_eq(Engine *e, const float *gains, int n, int enabled) {
    if (n > 10) n = 10;
    for (int i = 0; i < n; i++) e->eq_gains[i] = gains[i];
    e->eq_enabled = enabled;
    if (e->dsp) nava_dsp_set_eq(e->dsp, gains, n, enabled);
}

void engine_set_echo(Engine *e, float intensity, float feedback, float delay_ms) {
    e->echo_i = intensity; e->echo_fb = feedback; e->echo_ms = delay_ms;
    if (e->dsp) nava_dsp_set_echo(e->dsp, intensity, feedback, delay_ms);
}

void engine_set_compressor(Engine *e, float amount) {
    e->comp = amount;
    if (e->dsp) nava_dsp_set_compressor(e->dsp, amount);
}

void engine_set_reverb(Engine *e, float amount) {
    e->rev_amt = amount;
    if (e->dsp) nava_dsp_set_reverb(e->dsp, amount);
}

void engine_set_flanger(Engine *e, float amount) {
    e->fl_amt = amount;
    if (e->dsp) nava_dsp_set_flanger(e->dsp, amount);
}

void engine_set_rate(Engine *e, float rate) {
    if (rate < 0.5f) rate = 0.5f;
    if (rate > 2.0f) rate = 2.0f;
    pthread_mutex_lock(&e->lock);
    e->rate = rate;
    pthread_mutex_unlock(&e->lock);
}

void engine_set_rotation(Engine *e, int rotation) {
    if (rotation < 0) rotation = 0;
    if (rotation > 5) rotation = 5;
    e->rotation = rotation;
}

void engine_set_video_effects(Engine *e, float brightness, float contrast,
                              float saturation, float gamma, float hue) {
    e->fx_brightness = brightness;
    e->fx_contrast = contrast;
    e->fx_saturation = saturation;
    e->fx_gamma = gamma;
    e->fx_hue = hue;
}

void engine_set_fades(Engine *e, int crossfade_enabled,
                      double crossfade_ms, double fade_ms) {
    if (crossfade_ms < 100.0) crossfade_ms = 100.0;
    if (crossfade_ms > 10000.0) crossfade_ms = 10000.0;
    if (fade_ms < 10.0) fade_ms = 10.0;
    if (fade_ms > 1000.0) fade_ms = 1000.0;
    e->cf_enabled = crossfade_enabled ? 1 : 0;
    e->cf_ms = crossfade_ms;
    e->fade_ms = fade_ms;
}

void engine_fade_in(Engine *e) {
    if (e->dsp) nava_dsp_fade_to(e->dsp, 1.0f, e->fade_ms / 1000.0);
}

void engine_fade_out(Engine *e) {
    if (e->dsp) nava_dsp_fade_to(e->dsp, 0.0f, e->fade_ms / 1000.0);
}

void engine_seek_faded(Engine *e, double seconds) {
    if (seconds < 0) seconds = 0;
    pthread_mutex_lock(&e->lock);
    if (e->duration > 0 && seconds > e->duration) seconds = e->duration;
    e->seek_delay_req = seconds;
    e->fade_after_seek = 1;
    e->seek_at = now_sec() + e->fade_ms / 1000.0;
    pthread_mutex_unlock(&e->lock);
    engine_fade_out(e);
}

void engine_pause_faded(Engine *e) {
    if (!e->fmt) return;
    pthread_mutex_lock(&e->lock);
    e->pause_at = now_sec() + e->fade_ms / 1000.0;
    pthread_mutex_unlock(&e->lock);
    /* Publish the paused state now; play_thread only drains the fade. */
    set_playing(e, 0);
    engine_fade_out(e);
}

void engine_resume_faded(Engine *e) {
    /* engine_play() unparks the decode thread and ramps the gain from its
     * current value to 1.0 over fade_ms, so resume fades in click-free. */
    engine_play(e);
}

void engine_stop_faded(Engine *e) {
    if (!e->fmt || !e->dsp) { engine_stop(e); return; }
    pthread_mutex_lock(&e->lock);
    e->stop_at = now_sec() + e->fade_ms / 1000.0;
    pthread_mutex_unlock(&e->lock);
    engine_fade_out(e);
}

double engine_crossfade_duration(Engine *e) {
    return e->xf_active ? (double)e->xf_len_frames / OUT_RATE : -1.0;
}

int engine_crossfade_to(Engine *e, const char *path, char *err, int errlen) {
    if (!e || !path || !e->fmt || !e->dsp) return -1;
    if (!e->cf_enabled || e->cf_ms < 100.0) return -1;
    if (e->has_video) return -1;

    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, NULL) < 0) return -1;
    if (avformat_find_stream_info(fmt, NULL) < 0) {
        avformat_close_input(&fmt);
        return -1;
    }
    int ai = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (ai < 0) { avformat_close_input(&fmt); return -1; }
    AVCodecContext *adec = NULL;
    if (open_decoder(fmt, ai, &adec) < 0) {
        avformat_close_input(&fmt);
        return -1;
    }
    double dur = fmt->duration > 0 ? (double)fmt->duration / AV_TIME_BASE : 0.0;

    pthread_mutex_lock(&e->lock);
    if (e->pend_swr) swr_free(&e->pend_swr);
    if (e->pend_adec) avcodec_free_context(&e->pend_adec);
    if (e->pend_fmt) avformat_close_input(&e->pend_fmt);
    av_free(e->pend_buf);
    e->pend_buf = NULL;
    e->pend_cap = e->pend_n = e->pend_off = 0;
    e->pend_fmt = fmt;
    e->pend_adec = adec;
    e->pend_ai = ai;
    snprintf(e->pend_path, sizeof(e->pend_path), "%s", path);
    e->pend_dur = dur;
    int swr_ok = pend_rebuild_swr(e) == 0;
    double cf_ms = e->cf_ms;
    if (e->duration > 0.0) {
        double left_ms = (e->duration - e->clock) * 1000.0;
        /* Leave a small tail so the outgoing audio is never cut abruptly. */
        if (left_ms < 0.0) left_ms = 0.0;
        if (cf_ms > left_ms) cf_ms = left_ms;
    }
    if (cf_ms < 100.0) swr_ok = 0;
    int frames = swr_ok ? (int)(cf_ms * OUT_RATE / 1000.0) : 0;
    if (frames > 0) {
        int16_t *buf = av_malloc_array((size_t)frames * OUT_CH, sizeof(int16_t));
        if (!buf) frames = 0;
        else e->xf_buf = buf;
    }
    if (frames > 0) {
        e->xf_len_frames = frames;
        e->xf_pos = 0;
        e->xf_swap_ready = 0;
        e->xf_active = 1;
    } else {
        if (e->pend_swr) swr_free(&e->pend_swr);
        if (e->pend_adec) avcodec_free_context(&e->pend_adec);
        if (e->pend_fmt) avformat_close_input(&e->pend_fmt);
        e->pend_ai = -1;
        e->xf_active = 0;
        e->xf_len_frames = 0;
    }
    pthread_mutex_unlock(&e->lock);

    if (frames <= 0) {
        if (err && errlen > 0) snprintf(err, errlen, "crossfade unavailable");
        return -1;
    }
    (void)err; (void)errlen;
    return 0;
}

double engine_next_frame_pts(Engine *e) {
    pthread_mutex_lock(&e->lock);
    double pts = e->head ? e->head->pts : -1.0;
    pthread_mutex_unlock(&e->lock);
    return pts;
}

int engine_frame(Engine *e, uint8_t **rgba, int *w, int *h) {
    pthread_mutex_lock(&e->lock);
    Frame *f = e->head;
    if (!f) { pthread_mutex_unlock(&e->lock); return 0; }
    e->head = f->next;
    if (!e->head) e->tail = NULL;
    e->qcount--;
    pthread_mutex_unlock(&e->lock);
    *rgba = f->data; *w = f->w; *h = f->h;
    free(f);
    return 1;
}

int engine_cover(Engine *e, uint8_t **rgba, int *w, int *h) {
    if (!e->cover) return 0;
    *rgba = malloc((size_t)e->cover_w * e->cover_h * 4);
    if (!*rgba) return 0;
    memcpy(*rgba, e->cover, (size_t)e->cover_w * e->cover_h * 4);
    *w = e->cover_w; *h = e->cover_h;
    return 1;
}

int engine_get_spectrum(Engine *e, float *out, int n) {
    return e->dsp ? nava_dsp_spectrum(e->dsp, out, n) : 0;
}
int engine_get_waveform(Engine *e, float *out, int n) {
    return e->dsp ? nava_dsp_waveform(e->dsp, out, n) : 0;
}
void engine_get_levels(Engine *e, float *peak, float *rms) {
    if (e->dsp) nava_dsp_levels(e->dsp, peak, rms);
    else { if (peak) *peak = 0; if (rms) *rms = 0; }
}

void engine_stop(Engine *e) {
    __atomic_store_n(&e->stop, 1, __ATOMIC_SEQ_CST);
    set_playing(e, 0);
    if (e->thread_running) {
        pthread_join(e->thread, NULL);
        e->thread_running = 0;
        e->thread_done = 0;
    }
    if (e->vthread_running) {
        pthread_join(e->vthread, NULL);
        e->vthread_running = 0;
    }
    e->v_done = 0;
    cancel_crossfade(e);
    e->pause_at = e->stop_at = e->seek_at = 0.0;
}

void engine_free(Engine *e) {
    if (!e) return;
    engine_stop(e);
    close_media(e);
    pthread_mutex_destroy(&e->lock);
    free(e);
}
