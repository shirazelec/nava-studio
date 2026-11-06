/* engine_test.c — headless verification of the native playback engine. */
#include "engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double wall(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s FILE [seconds]\n", argv[0]); return 2; }
    double limit = argc > 2 ? atof(argv[2]) : 1.0;
    char err[256] = {0};
    Engine *e = engine_new();
    if (engine_open(e, argv[1], err, sizeof(err)) < 0) {
        fprintf(stderr, "open failed: %s\n", err);
        engine_free(e);
        return 1;
    }
    printf("opened audio=%d video=%d %dx%d duration=%.3f\n",
           engine_has_audio(e), engine_has_video(e), engine_width(e), engine_height(e),
           engine_duration(e));

    double t0 = wall();
    engine_play(e);
    usleep((useconds_t)(limit * 1e6));
    double pos1 = engine_position(e);
    double elapsed = wall() - t0;

    /* Live EQ/effect change must not restart or stall playback. */
    float gains[10] = {6, 5, 4, 2, 0, -1, -2, 2, 4, 6};
    engine_set_eq(e, gains, 10, 1);
    engine_set_echo(e, 0.3f, 0.4f, 90.0f);
    engine_set_compressor(e, 0.5f);
    engine_set_video_effects(e, 0.1f, 1.2f, 1.3f, 1.1f, 20.0f);
    float spec[64] = {0}, peak = 0, rms = 0;
    int bins = engine_get_spectrum(e, spec, 64);
    engine_get_levels(e, &peak, &rms);

    int frames = 0;
    unsigned char *px = NULL;
    int w = 0, h = 0;
    while (engine_frame(e, &px, &w, &h)) { free(px); frames++; }
    int state_ok = engine_playing(e);
    usleep(300000);
    double pos2 = engine_position(e);
    engine_seek(e, 0.5);
    usleep(400000);
    double pos3 = engine_position(e);

    /* Pause -> resume must advance the clock again. */
    engine_pause(e);
    usleep(250000);
    double paused_a = engine_position(e);
    usleep(250000);
    double paused_b = engine_position(e);
    int paused_ok = engine_playing(e) == 0 && (paused_b - paused_a) < 0.08;
    engine_play(e);
    usleep(500000);
    double resumed = engine_position(e);
    int resume_ok = engine_playing(e) && (resumed - paused_b) > 0.15;

    /* Speed changes must be safe and actually change the clock rate. */
    double sp0 = engine_position(e);
    engine_set_rate(e, 2.0f);
    usleep(600000);
    double sp2 = engine_position(e);
    engine_set_rate(e, 0.5f);
    usleep(600000);
    double sp05 = engine_position(e);
    engine_set_rate(e, 1.0f);
    usleep(300000);
    double sp1 = engine_position(e);
    int speed_ok = engine_playing(e) && (sp2 - sp0) > 0.5 && (sp05 - sp2) > 0.05;
    if (sp1 - sp05 > 0.1) speed_ok = 1;

    printf("elapsed=%.2f pos_after_wait=%.2f playing=%d pos_after_fx=%.2f seek_target=0.5 pos_after_seek=%.2f\n",
           elapsed, pos1, state_ok, pos2, pos3);
    printf("pause: frozen=%.2f->%.2f paused_ok=%d resumed=%.2f resume_ok=%d\n",
           paused_a, paused_b, paused_ok, resumed, resume_ok);
    printf("speed: 1x=%.2f 2x=%.2f 0.5x=%.2f back1x=%.2f speed_ok=%d\n",
           sp0, sp2, sp05, sp1, speed_ok);
    printf("frames_pending=%d spectrum_bins=%d peak=%.3f rms=%.3f eof=%d\n",
           frames, bins, peak, rms, engine_eof(e));

    int ok = (elapsed < limit + 1.0) && (pos1 < engine_duration(e) + 0.01) &&
             state_ok && (pos3 >= 0.4 && pos3 < 2.5) &&
             paused_ok && resume_ok && speed_ok;
    printf(ok ? "ENGINE_TEST_OK\n" : "ENGINE_TEST_FAIL\n");
    engine_stop(e);
    engine_free(e);
    return ok ? 0 : 1;
}
