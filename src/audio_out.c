/* audio_out.c — cross-platform audio output for Nava Studio.
 *
 * Backend selection (compile-time):
 *   Linux   → PulseAudio  (define NAVA_AUDIO_PULSE, default)
 *   macOS   → CoreAudio   (define NAVA_AUDIO_COREAUDIO)
 *   Windows → WASAPI      (define NAVA_AUDIO_WASAPI)
 *
 * If none is defined the platform is auto-detected via pre-defined macros.
 */

#include "audio_out.h"
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- detect -- */
#if !defined(NAVA_AUDIO_PULSE) && !defined(NAVA_AUDIO_COREAUDIO) && !defined(NAVA_AUDIO_WASAPI)
#  if defined(__APPLE__)
#    define NAVA_AUDIO_COREAUDIO 1
#  elif defined(_WIN32) || defined(_WIN64)
#    define NAVA_AUDIO_WASAPI 1
#  else
#    define NAVA_AUDIO_PULSE 1
#  endif
#endif

/* ================================================================ PULSE === */
#if defined(NAVA_AUDIO_PULSE)
#include <pulse/simple.h>
#include <pulse/error.h>

struct NavaAudio {
    pa_simple *pa;
    int        rate;
    int        ch;
};

NavaAudio *nava_audio_open(int rate, int channels) {
    NavaAudio *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    a->rate = rate;
    a->ch   = channels;
    pa_sample_spec ss = {
        .format   = PA_SAMPLE_S16LE,
        .rate     = (uint32_t)rate,
        .channels = (uint8_t)channels,
    };
    /* Keep sink buffer ≈90 ms so the audio clock stays tight. */
    pa_buffer_attr attr = {
        .maxlength = (uint32_t)-1,
        .tlength   = (uint32_t)(rate * channels * 2 * 90 / 1000),
        .prebuf    = (uint32_t)-1,
        .minreq    = (uint32_t)-1,
        .fragsize  = (uint32_t)-1,
    };
    int err = 0;
    a->pa = pa_simple_new(NULL, "Nava Studio", PA_STREAM_PLAYBACK, NULL,
                          "playback", &ss, NULL, &attr, &err);
    if (!a->pa) { free(a); return NULL; }
    return a;
}
void nava_audio_write(NavaAudio *a, const int16_t *pcm, int frames) {
    if (!a || !a->pa) return;
    int err = 0;
    pa_simple_write(a->pa, pcm, (size_t)frames * a->ch * 2, &err);
}
uint64_t nava_audio_latency_us(NavaAudio *a) {
    if (!a || !a->pa) return 0;
    int err = 0;
    pa_usec_t us = pa_simple_get_latency(a->pa, &err);
    return (err == 0 && us != (pa_usec_t)-1) ? (uint64_t)us : 0;
}
void nava_audio_flush(NavaAudio *a) {
    if (!a || !a->pa) return;
    int err = 0;
    pa_simple_flush(a->pa, &err);
}
void nava_audio_close(NavaAudio *a) {
    if (!a) return;
    if (a->pa) pa_simple_free(a->pa);
    free(a);
}

/* ============================================================= COREAUDIO == */
#elif defined(NAVA_AUDIO_COREAUDIO)
#include <AudioToolbox/AudioQueue.h>
#include <CoreAudio/CoreAudioTypes.h>
#include <pthread.h>
#include <unistd.h>

#define CA_NBUF  4
#define CA_BSIZE 4096   /* frames per queue buffer */

struct NavaAudio {
    AudioQueueRef        queue;
    AudioQueueBufferRef  bufs[CA_NBUF];
    pthread_mutex_t      mtx;
    pthread_cond_t       cond;
    int                  free_count;
    int                  rate, ch;
};

static void ca_callback(void *ud, AudioQueueRef q, AudioQueueBufferRef buf) {
    NavaAudio *a = (NavaAudio *)ud;
    pthread_mutex_lock(&a->mtx);
    if (buf) buf->mAudioDataByteSize = 0;
    a->free_count++;
    pthread_cond_signal(&a->cond);
    pthread_mutex_unlock(&a->mtx);
    (void)q;
}

NavaAudio *nava_audio_open(int rate, int channels) {
    NavaAudio *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    a->rate = rate; a->ch = channels;
    pthread_mutex_init(&a->mtx, NULL);
    pthread_cond_init(&a->cond, NULL);

    AudioStreamBasicDescription fmt = {0};
    fmt.mSampleRate       = rate;
    fmt.mFormatID         = kAudioFormatLinearPCM;
    fmt.mFormatFlags      = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    fmt.mBitsPerChannel   = 16;
    fmt.mChannelsPerFrame = (UInt32)channels;
    fmt.mBytesPerFrame    = (UInt32)(channels * 2);
    fmt.mFramesPerPacket  = 1;
    fmt.mBytesPerPacket   = fmt.mBytesPerFrame;

    if (AudioQueueNewOutput(&fmt, ca_callback, a, NULL, NULL, 0, &a->queue) != 0) {
        free(a); return NULL;
    }
    for (int i = 0; i < CA_NBUF; i++) {
        AudioQueueAllocateBuffer(a->queue, CA_BSIZE * (UInt32)channels * 2, &a->bufs[i]);
        a->bufs[i]->mAudioDataByteSize = 0;
    }
    a->free_count = CA_NBUF;
    AudioQueueStart(a->queue, NULL);
    return a;
}
void nava_audio_write(NavaAudio *a, const int16_t *pcm, int frames) {
    if (!a || !pcm || frames <= 0) return;
    int bytes = frames * a->ch * 2;
    /* Find a free buffer (block until one becomes available). */
    pthread_mutex_lock(&a->mtx);
    while (a->free_count == 0)
        pthread_cond_wait(&a->cond, &a->mtx);
    AudioQueueBufferRef buf = NULL;
    for (int i = 0; i < CA_NBUF; i++) {
        if (a->bufs[i]->mAudioDataByteSize == 0) { buf = a->bufs[i]; break; }
    }
    if (!buf) {
        pthread_mutex_unlock(&a->mtx);
        return;
    }
    int copy = bytes < (int)buf->mAudioDataBytesCapacity ? bytes : (int)buf->mAudioDataBytesCapacity;
    memcpy(buf->mAudioData, pcm, (size_t)copy);
    buf->mAudioDataByteSize = (UInt32)copy;
    a->free_count--;
    pthread_mutex_unlock(&a->mtx);
    AudioQueueEnqueueBuffer(a->queue, buf, 0, NULL);
}
uint64_t nava_audio_latency_us(NavaAudio *a) {
    /* Approximate: CA_NBUF * CA_BSIZE frames of queued audio */
    if (!a) return 0;
    return (uint64_t)CA_BSIZE * CA_NBUF * 1000000ULL / (uint64_t)a->rate;
}
void nava_audio_flush(NavaAudio *a) {
    if (!a || !a->queue) return;
    AudioQueueFlush(a->queue);
}
void nava_audio_close(NavaAudio *a) {
    if (!a) return;
    if (a->queue) {
        AudioQueueStop(a->queue, true);
        AudioQueueDispose(a->queue, true);
    }
    pthread_mutex_destroy(&a->mtx);
    pthread_cond_destroy(&a->cond);
    free(a);
}

/* =============================================================== WASAPI === */
#elif defined(NAVA_AUDIO_WASAPI)
/* Minimal WASAPI implementation using the low-latency shared-mode render API.
 * Requires Windows Vista+ and linking against ole32, mmdevapi, uuid. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>

struct NavaAudio {
    IMMDeviceEnumerator *enumerator;
    IMMDevice           *device;
    IAudioClient        *client;
    IAudioRenderClient  *render;
    WAVEFORMATEX         wfx;
    UINT32               buf_frames;
    int                  rate, ch;
};

NavaAudio *nava_audio_open(int rate, int channels) {
    NavaAudio *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    a->rate = rate; a->ch = channels;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    HRESULT hr;
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void **)&a->enumerator);
    if (FAILED(hr)) goto fail;
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(a->enumerator,
             eRender, eConsole, &a->device);
    if (FAILED(hr)) goto fail;
    hr = IMMDevice_Activate(a->device, &IID_IAudioClient, CLSCTX_ALL,
                            NULL, (void **)&a->client);
    if (FAILED(hr)) goto fail;

    WAVEFORMATEX *fmt = &a->wfx;
    fmt->wFormatTag      = WAVE_FORMAT_PCM;
    fmt->nChannels       = (WORD)channels;
    fmt->nSamplesPerSec  = (DWORD)rate;
    fmt->wBitsPerSample  = 16;
    fmt->nBlockAlign     = (WORD)(channels * 2);
    fmt->nAvgBytesPerSec = (DWORD)(rate * channels * 2);
    fmt->cbSize          = 0;

    REFERENCE_TIME hns = 10000000LL * 90 / 1000; /* 90 ms */
    hr = IAudioClient_Initialize(a->client, AUDCLNT_SHAREMODE_SHARED,
                                 0, hns, 0, fmt, NULL);
    if (FAILED(hr)) goto fail;
    IAudioClient_GetBufferSize(a->client, &a->buf_frames);
    hr = IAudioClient_GetService(a->client, &IID_IAudioRenderClient,
                                 (void **)&a->render);
    if (FAILED(hr)) goto fail;
    IAudioClient_Start(a->client);
    return a;

fail:
    nava_audio_close(a);
    return NULL;
}
void nava_audio_write(NavaAudio *a, const int16_t *pcm, int frames) {
    if (!a || !a->render) return;
    UINT32 padding = 0;
    IAudioClient_GetCurrentPadding(a->client, &padding);
    UINT32 avail = a->buf_frames - padding;
    if ((UINT32)frames > avail) frames = (int)avail;
    if (frames <= 0) return;
    BYTE *buf = NULL;
    if (FAILED(IAudioRenderClient_GetBuffer(a->render, (UINT32)frames, &buf))) return;
    memcpy(buf, pcm, (size_t)frames * a->ch * 2);
    IAudioRenderClient_ReleaseBuffer(a->render, (UINT32)frames, 0);
}
uint64_t nava_audio_latency_us(NavaAudio *a) {
    if (!a || !a->client) return 0;
    REFERENCE_TIME lat = 0;
    IAudioClient_GetStreamLatency(a->client, &lat);
    return (uint64_t)(lat / 10); /* 100-ns units → µs */
}
void nava_audio_flush(NavaAudio *a) {
    if (!a || !a->client) return;
    IAudioClient_Stop(a->client);
    IAudioClient_Reset(a->client);
    IAudioClient_Start(a->client);
}
void nava_audio_close(NavaAudio *a) {
    if (!a) return;
    if (a->client)      { IAudioClient_Stop(a->client); IAudioClient_Release(a->client); }
    if (a->render)      IAudioRenderClient_Release(a->render);
    if (a->device)      IMMDevice_Release(a->device);
    if (a->enumerator)  IMMDeviceEnumerator_Release(a->enumerator);
    free(a);
}

#endif /* backend */
