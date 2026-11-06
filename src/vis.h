#pragma once
#include <cairo.h>

#define NAVA_VIS_COUNT 14

typedef struct {
    int mode;                 /* index into nava_vis_names() */
    float sensitivity;        /* 0.2 .. 3.0 */
    float bands[64];          /* live, smoothed, 0..1 */
    float wave[128];
    float energy;
    double phase;
} NavaVis;

const char *nava_vis_name(int index);
const char *nava_vis_key(int index);
int nava_vis_index_for_key(const char *key);
int nava_vis_is_off(int index);

void nava_vis_draw(cairo_t *cr, int w, int h, const NavaVis *v);
