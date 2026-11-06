/* vis.c — cairo stage visualizers. */
#include "vis.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

#define TAU (M_PI * 2.0)
#define NBANDS 64
#define NWAVE 128

static const char *VIS_NAMES[NAVA_VIS_COUNT] = {
    "Cthugha (Fomalhaut)", "Atari Video Music", "CosmicBelt", "Kaleidoscope",
    "Plasma", "Torus", "Vortex", "Spiral out", "Particles", "Tunnel",
    "Spectrum bars", "Oscilloscope", "VU meter", "Off"
};
static const char *VIS_KEYS[NAVA_VIS_COUNT] = {
    "cthugha", "atari", "cosmic", "kaleido", "plasma", "torus", "vortex",
    "spiral", "particles", "tunnel", "bars", "wave", "vu", "off"
};

const char *nava_vis_name(int index) {
    if (index < 0 || index >= NAVA_VIS_COUNT) index = 2;
    return VIS_NAMES[index];
}
const char *nava_vis_key(int index) {
    if (index < 0 || index >= NAVA_VIS_COUNT) index = 2;
    return VIS_KEYS[index];
}
int nava_vis_index_for_key(const char *key) {
    if (!key) return 2;
    for (int i = 0; i < NAVA_VIS_COUNT; i++)
        if (strcmp(key, VIS_KEYS[i]) == 0) return i;
    return 2;
}
int nava_vis_is_off(int index) { return index == NAVA_VIS_COUNT - 1; }

/* --------------------------------------------------------------- helpers */
static double clampi(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static double sample(const float *b, int n, int index) {
    if (!b || n <= 0) return 0.0;
    return b[((index % n) + n) % n];
}
static void resample(const float *src, int n, float *dst, int count) {
    if (!src || n <= 0) { for (int i = 0; i < count; i++) dst[i] = 0; return; }
    for (int i = 0; i < count; i++) {
        int k = (int)((double)i * n / count);
        if (k >= n) k = n - 1;
        dst[i] = src[k];
    }
}

/* colorsys.hls_to_rgb equivalent */
static void hls_to_rgb(double h, double l, double s, double *r, double *g, double *b) {
    h = h - floor(h);
    l = clampi(l, 0, 1);
    s = clampi(s, 0, 1);
    if (s == 0.0) { *r = *g = *b = l; return; }
    double m2 = l <= 0.5 ? l * (1 + s) : l + s - l * s;
    double m1 = 2 * l - m2;
    double hue[3] = { h + 1.0 / 3.0, h, h - 1.0 / 3.0 };
    double out[3];
    for (int i = 0; i < 3; i++) {
        double x = hue[i];
        if (x < 0) x += 1.0;
        if (x > 1) x -= 1.0;
        if (x < 1.0 / 6.0) out[i] = m1 + (m2 - m1) * x * 6.0;
        else if (x < 0.5) out[i] = m2;
        else if (x < 2.0 / 3.0) out[i] = m1 + (m2 - m1) * (2.0 / 3.0 - x) * 6.0;
        else out[i] = m1;
    }
    *r = out[0]; *g = out[1]; *b = out[2];
}
static void set_hsl(cairo_t *cr, double h, double s, double l, double a) {
    double r, g, b;
    hls_to_rgb(h, l, s, &r, &g, &b);
    cairo_set_source_rgba(cr, r, g, b, a);
}

static double field(double x, double y, double t) {
    return (sin(x * 1.7 + t * 0.9) * cos(y * 1.3 - t * 0.6)
            + 0.55 * sin((x + y) * 2.3 + t * 1.4)
            + 0.30 * sin((x - y) * 3.9 - t * 1.9)) / 1.85;
}

static void rounded(cairo_t *cr, double x, double y, double w, double h, double r) {
    double lim = w / 2.0 < h / 2.0 ? w / 2.0 : h / 2.0;
    if (r > lim) r = lim;
    if (r < 0) r = 0;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

static void vignette(cairo_t *cr, double w, double h, double strength) {
    double cx = w * 0.5, cy = h * 0.5;
    double r0 = (w < h ? w : h) * 0.18;
    double r1 = (w > h ? w : h) * 0.72;
    cairo_pattern_t *g = cairo_pattern_create_radial(cx, cy, r0, cx, cy, r1);
    cairo_pattern_add_color_stop_rgba(g, 0.0, 0, 0, 0, 0);
    cairo_pattern_add_color_stop_rgba(g, 0.65, 0, 0, 0, 0.10 * strength);
    cairo_pattern_add_color_stop_rgba(g, 1.0, 0, 0, 0, 0.62 * strength);
    cairo_set_source(cr, g);
    cairo_paint(cr);
    cairo_pattern_destroy(g);
}

static void ribbon(cairo_t *cr, double length, double width, double hue,
                   double alpha, double twist) {
    double r, g, b;
    hls_to_rgb(hue, 0.56, 0.78, &r, &g, &b);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    for (int layer = 0; layer < 2; layer++) {
        double mul = layer == 0 ? 1.0 : 0.55;
        double a = layer == 0 ? alpha : alpha * 0.85;
        cairo_set_line_width(cr, width * mul > 0.8 ? width * mul : 0.8);
        cairo_set_source_rgba(cr, r, g, b, a);
        cairo_move_to(cr, 0, 0);
        cairo_curve_to(cr, length * 0.42, -length * (0.30 + twist),
                       length * 0.80, -length * (0.16 - twist), length, 0);
        cairo_curve_to(cr, length * 0.80, length * (0.16 - twist),
                       length * 0.42, length * (0.30 + twist), 0, 0);
        cairo_stroke(cr);
    }
}

static void background(cairo_t *cr, double w, double h, double energy,
                       const double ha[3], const double hb[3]) {
    double cx = w * 0.5, cy = h * 0.5;
    double r_max = (w > h ? w : h) * 0.78;
    cairo_pattern_t *g = cairo_pattern_create_radial(
        cx, h * 0.42, 10.0, cx, cy, r_max);
    double pulse = 0.10 + 0.16 * energy;
    cairo_pattern_add_color_stop_rgb(g, 0.0,
        clampi(ha[0] + pulse, 0, 1), clampi(ha[1] + pulse * 0.8, 0, 1),
        clampi(ha[2] + pulse, 0, 1));
    cairo_pattern_add_color_stop_rgb(g, 0.55, hb[0], hb[1], hb[2]);
    cairo_pattern_add_color_stop_rgb(g, 1.0, 0.012, 0.016, 0.032);
    cairo_set_source(cr, g);
    cairo_paint(cr);
    cairo_pattern_destroy(g);
}

static const double HA_DEFAULT[3] = {0.06, 0.10, 0.22};
static const double HB_DEFAULT[3] = {0.02, 0.03, 0.07};

/* ---------------------------------------------------------------- stars */
typedef struct { double x, y, size, seed; } Star;
static Star stars[190];
static int stars_ready;
static void init_stars(void) {
    if (stars_ready) return;
    unsigned s = 20240607u;
    for (int i = 0; i < 190; i++) {
        s = s * 1103515245u + 12345u;
        double a = ((s >> 8) & 0xFFFFFF) / 16777216.0;
        s = s * 1103515245u + 12345u;
        double b = ((s >> 8) & 0xFFFFFF) / 16777216.0;
        s = s * 1103515245u + 12345u;
        double c = ((s >> 8) & 0xFFFFFF) / 16777216.0;
        s = s * 1103515245u + 12345u;
        double d = ((s >> 8) & 0xFFFFFF) / 16777216.0;
        stars[i].x = a; stars[i].y = b;
        stars[i].size = 0.4 + 1.5 * c;
        stars[i].seed = d * TAU;
    }
    stars_ready = 1;
}
static void draw_stars(cairo_t *cr, double w, double h, double phase) {
    init_stars();
    for (int band = 0; band < 3; band++) {
        cairo_new_path(cr);
        for (int i = 0; i < 190; i++) {
            Star *s = &stars[i];
            double tw = 0.45 + 0.55 * (0.5 + 0.5 * sin(phase * 1.7 + s->seed * 3.1));
            int level = (int)(tw * 3.0);
            if (level > 2) level = 2;
            if (level != band) continue;
            cairo_new_sub_path(cr);
            cairo_arc(cr, s->x * w, s->y * h, s->size, 0, TAU);
        }
        cairo_set_source_rgba(cr, 0.75, 0.88, 1.0, 0.035 + 0.045 * band);
        cairo_fill(cr);
    }
}

/* ------------------------------------------------------------- drawers */
static void draw_cosmic(cairo_t *cr, double w, double h, const float *bands,
                        int nb, double phase, double energy) {
    background(cr, w, h, energy * 0.55, HA_DEFAULT, HB_DEFAULT);
    draw_stars(cr, w, h, phase);
    double cx = w * 0.5, cy = h * 0.5, base = w < h ? w : h;

    /* Multi-layer glowing celestial nebulas */
    for (int index = 0; index < 4; index++) {
        double low = sample(bands, nb, index * 5);
        double radius = base * (0.18 + 0.14 * index) * (1.0 + 0.28 * low);
        double ox = cx + cos(phase * 0.32 + index * 2.1) * base * 0.06;
        double oy = cy + sin(phase * 0.26 + index * 1.7) * base * 0.05;
        double rr = radius > 10 ? radius : 10;
        cairo_pattern_t *g = cairo_pattern_create_radial(ox, oy, 0, ox, oy, rr);
        if (index == 0) {
            cairo_pattern_add_color_stop_rgba(g, 0.0, 0.10, 0.75, 1.0, 0.22 + 0.20 * low);
            cairo_pattern_add_color_stop_rgba(g, 0.6, 0.30, 0.20, 0.9, 0.10 + 0.10 * low);
        } else if (index == 1) {
            cairo_pattern_add_color_stop_rgba(g, 0.0, 0.85, 0.20, 0.85, 0.18 + 0.18 * low);
            cairo_pattern_add_color_stop_rgba(g, 0.6, 0.10, 0.40, 0.95, 0.08 + 0.08 * low);
        } else if (index == 2) {
            cairo_pattern_add_color_stop_rgba(g, 0.0, 0.05, 0.85, 0.95, 0.15 + 0.15 * low);
            cairo_pattern_add_color_stop_rgba(g, 0.6, 0.60, 0.10, 0.80, 0.06 + 0.06 * low);
        } else {
            cairo_pattern_add_color_stop_rgba(g, 0.0, 0.95, 0.65, 0.15, 0.12 + 0.14 * low);
            cairo_pattern_add_color_stop_rgba(g, 0.6, 0.80, 0.20, 0.40, 0.05 + 0.05 * low);
        }
        cairo_pattern_add_color_stop_rgba(g, 1.0, 0, 0, 0, 0);
        cairo_set_source(cr, g);
        cairo_arc(cr, ox, oy, rr, 0, TAU);
        cairo_fill(cr);
        cairo_pattern_destroy(g);
    }

    /* Radiant stellar core with chromatic corona */
    double core = base * (0.12 + 0.05 * energy);
    cairo_pattern_t *g = cairo_pattern_create_radial(cx, cy, 0, cx, cy, core * 1.5);
    cairo_pattern_add_color_stop_rgba(g, 0.0, 1.0, 1.0, 1.0, 0.95);
    cairo_pattern_add_color_stop_rgba(g, 0.20, 0.45, 0.85, 1.0, 0.70 + 0.25 * energy);
    cairo_pattern_add_color_stop_rgba(g, 0.55, 0.65, 0.25, 0.95, 0.35 + 0.20 * energy);
    cairo_pattern_add_color_stop_rgba(g, 1.0, 0.10, 0.15, 0.70, 0.0);
    cairo_set_source(cr, g);
    cairo_arc(cr, cx, cy, core * 1.5, 0, TAU);
    cairo_fill(cr);
    cairo_pattern_destroy(g);

    /* 3D Cosmic Belt orbit particles */
    int count = 180;
    double tilt = 0.44 + 0.06 * sin(phase * 0.7);
    for (int front = 1; front >= 0; front--) {
        cairo_new_path(cr);
        for (int i = 0; i < count; i++) {
            double band = sample(bands, nb, (int)((long)i * nb / count));
            double angle = i * (TAU / count) + phase * 0.24;
            double radius = base * (0.25 + 0.19 * band);
            double x = cx + cos(angle) * radius;
            double y = cy + sin(angle) * radius * tilt;
            double depth = (sin(angle) + 1.0) * 0.5;
            if ((depth > 0.52) != (front != 0)) continue;
            double size = 1.2 + 3.2 * band * (0.4 + depth);
            cairo_new_sub_path(cr);
            cairo_arc(cr, x, y, size, 0, TAU);
        }
        if (front) {
            cairo_set_source_rgba(cr, 0.75, 0.95, 1.0, 0.75);
            cairo_fill(cr);
        } else {
            cairo_set_source_rgba(cr, 0.85, 0.55, 1.0, 0.45);
            cairo_fill(cr);
        }
    }

    /* Concentric glowing energy rings */
    double ring = base * (0.38 + 0.04 * energy);
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, 1.0, 0.44 + 0.05 * sin(phase * 0.6));
    g = cairo_pattern_create_radial(0, 0, ring * 0.82, 0, 0, ring * 1.08);
    cairo_pattern_add_color_stop_rgba(g, 0.0, 0.2, 0.7, 1.0, 0.0);
    cairo_pattern_add_color_stop_rgba(g, 0.50, 0.0, 0.9, 1.0, 0.45 + 0.45 * energy);
    cairo_pattern_add_color_stop_rgba(g, 0.75, 0.8, 0.3, 0.9, 0.30 + 0.30 * energy);
    cairo_pattern_add_color_stop_rgba(g, 1.0, 0.0, 0.0, 0.0, 0.0);
    cairo_set_source(cr, g);
    cairo_set_line_width(cr, 3.0 + 4.0 * energy);
    cairo_arc(cr, 0, 0, ring, 0, TAU);
    cairo_stroke(cr);
    cairo_pattern_destroy(g);
    cairo_restore(cr);
}

static void draw_bars(cairo_t *cr, double w, double h, const float *bands,
                      int nb, double phase, double energy) {
    (void)phase;
    background(cr, w, h, energy * 0.45, HA_DEFAULT, HB_DEFAULT);

    double baseline = h * 0.76;
    double max_h = h * 0.56;
    double margin = w * 0.05;
    int count = nb < 48 ? nb : 48;
    if (count < 16) count = 16;
    float data[64];
    resample(bands, nb, data, count);

    /* Ambient bass pulse glow */
    cairo_pattern_t *bg_glow = cairo_pattern_create_radial(w * 0.5, baseline, 10, w * 0.5, baseline, w * 0.50);
    cairo_pattern_add_color_stop_rgba(bg_glow, 0.0, 0.05, 0.65, 1.0, 0.16 + 0.20 * energy);
    cairo_pattern_add_color_stop_rgba(bg_glow, 0.5, 0.55, 0.10, 0.85, 0.06 + 0.12 * energy);
    cairo_pattern_add_color_stop_rgba(bg_glow, 1.0, 0.0, 0.0, 0.0, 0.0);
    cairo_set_source(cr, bg_glow);
    cairo_paint(cr);
    cairo_pattern_destroy(bg_glow);

    double slot = (w - margin * 2.0) / count;
    double bw = slot * 0.72;
    if (bw < 3.0) bw = 3.0;

    for (int index = 0; index < count; index++) {
        double val = data[index];
        if (val < 0.0) val = 0.0;
        if (val > 1.0) val = 1.0;
        double bar_h = max_h * (0.03 + 0.97 * pow(val, 0.85));
        if (bar_h < 4.0) bar_h = 4.0;
        double x = margin + index * slot + (slot - bw) * 0.5;
        double top = baseline - bar_h;
        double corner = bw * 0.45;
        if (corner > 8.0) corner = 8.0;

        /* Soft neon bloom behind energetic bars */
        if (val > 0.12) {
            cairo_pattern_t *bloom = cairo_pattern_create_linear(0, baseline, 0, top);
            cairo_pattern_add_color_stop_rgba(bloom, 0.0, 0.0, 0.80, 1.0, 0.0);
            cairo_pattern_add_color_stop_rgba(bloom, 0.6, 0.6, 0.20, 1.0, 0.12 * val);
            cairo_pattern_add_color_stop_rgba(bloom, 1.0, 1.0, 0.25, 0.7, 0.22 * val);
            cairo_set_source(cr, bloom);
            rounded(cr, x - 3, top - 3, bw + 6, bar_h + 6, corner + 2);
            cairo_fill(cr);
            cairo_pattern_destroy(bloom);
        }

        /* Main bar: electric cyan -> neon violet -> hot magenta -> gold */
        cairo_pattern_t *g = cairo_pattern_create_linear(0, baseline, 0, top);
        cairo_pattern_add_color_stop_rgb(g, 0.00, 0.00, 0.85, 1.00);
        cairo_pattern_add_color_stop_rgb(g, 0.45, 0.45, 0.25, 0.98);
        cairo_pattern_add_color_stop_rgb(g, 0.80, 0.98, 0.15, 0.65);
        cairo_pattern_add_color_stop_rgb(g, 1.00, 1.00, 0.92, 0.70);
        cairo_set_source(cr, g);
        rounded(cr, x, top, bw, bar_h, corner);
        cairo_fill(cr);
        cairo_pattern_destroy(g);

        /* Glass tube core highlight */
        if (bw >= 6.0 && bar_h > 12.0) {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.20 + 0.35 * val);
            rounded(cr, x + bw * 0.25, top + 2, bw * 0.5, bar_h * 0.45, corner * 0.5);
            cairo_fill(cr);
        }

        /* Floating peak cap */
        double cap_y = top - 4.5;
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.90);
        rounded(cr, x, cap_y, bw, 2.2, 1.0);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 0.4, 0.85, 1.0, 0.45);
        rounded(cr, x - 1.5, cap_y - 1.0, bw + 3.0, 4.2, 2.0);
        cairo_fill(cr);

        /* Wet floor glossy reflection */
        cairo_pattern_t *rg = cairo_pattern_create_linear(0, baseline, 0, baseline + bar_h * 0.40);
        cairo_pattern_add_color_stop_rgba(rg, 0.0, 0.0, 0.75, 1.0, 0.22);
        cairo_pattern_add_color_stop_rgba(rg, 0.5, 0.5, 0.20, 0.8, 0.08);
        cairo_pattern_add_color_stop_rgba(rg, 1.0, 0.0, 0.00, 0.0, 0.0);
        cairo_set_source(cr, rg);
        rounded(cr, x, baseline + 2.0, bw, bar_h * 0.40, corner);
        cairo_fill(cr);
        cairo_pattern_destroy(rg);
    }

    /* Baseline luminous line */
    cairo_pattern_t *bl = cairo_pattern_create_linear(margin, 0, w - margin, 0);
    cairo_pattern_add_color_stop_rgba(bl, 0.0, 0.0, 0.8, 1.0, 0.0);
    cairo_pattern_add_color_stop_rgba(bl, 0.5, 0.4, 0.85, 1.0, 0.65);
    cairo_pattern_add_color_stop_rgba(bl, 1.0, 0.0, 0.8, 1.0, 0.0);
    cairo_set_source(cr, bl);
    cairo_set_line_width(cr, 1.5);
    cairo_move_to(cr, margin, baseline);
    cairo_line_to(cr, w - margin, baseline);
    cairo_stroke(cr);
    cairo_pattern_destroy(bl);
}

static void draw_smooth_curve(cairo_t *cr, const double *px, const double *py, int n) {
    if (n < 2) return;
    cairo_move_to(cr, px[0], py[0]);
    for (int i = 0; i < n - 1; i++) {
        double p0_x = i > 0 ? px[i - 1] : px[i];
        double p0_y = i > 0 ? py[i - 1] : py[i];
        double p1_x = px[i];
        double p1_y = py[i];
        double p2_x = px[i + 1];
        double p2_y = py[i + 1];
        double p3_x = i + 2 < n ? px[i + 2] : p2_x;
        double p3_y = i + 2 < n ? py[i + 2] : p2_y;

        double cp1_x = p1_x + (p2_x - p0_x) / 6.0;
        double cp1_y = p1_y + (p2_y - p0_y) / 6.0;
        double cp2_x = p2_x - (p3_x - p1_x) / 6.0;
        double cp2_y = p2_y - (p3_y - p1_y) / 6.0;

        cairo_curve_to(cr, cp1_x, cp1_y, cp2_x, cp2_y, p2_x, p2_y);
    }
}

static void draw_wave(cairo_t *cr, double w, double h, const float *bands,
                      int nb, double phase, double energy,
                      const float *wave, int nw) {
    background(cr, w, h, energy * 0.45, HA_DEFAULT, HB_DEFAULT);

    double mid = h * 0.5;

    /* High-tech oscilloscope grid lines */
    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgba(cr, 0.2, 0.5, 0.8, 0.08);
    for (double gy = mid - h * 0.35; gy <= mid + h * 0.36; gy += h * 0.10) {
        cairo_move_to(cr, w * 0.04, gy);
        cairo_line_to(cr, w * 0.96, gy);
        cairo_stroke(cr);
    }

    int points = 128;
    const float *data = bands;
    int use_wave = 0;
    if (wave && nw > 0) {
        for (int i = 0; i < nw; i++) if (fabs(wave[i]) > 0.001f) { use_wave = 1; break; }
    }
    static float tmp[128];
    if (use_wave) {
        data = wave;
        points = nw > 128 ? 128 : nw;
    } else {
        resample(bands, nb, tmp, 128);
        data = tmp;
        points = 128;
    }

    double amp = h * 0.32;
    double px[128], py[128], py2[128];
    for (int i = 0; i < points; i++) {
        px[i] = w * 0.05 + (w * 0.90) * i / (points - 1);
        double wobble = sin(phase * 2.2 + i * 0.28) * 0.02 * h;
        double c_val = use_wave ? (data[i] - 0.5) * 2.0 : (data[i] - 0.30);
        py[i] = mid + c_val * amp + wobble;
        /* Counter harmonic wave */
        double wobble2 = cos(phase * 1.8 + i * 0.35) * 0.025 * h;
        py2[i] = mid - c_val * amp * 0.65 + wobble2;
    }

    /* Glow fill under primary wave */
    cairo_new_path(cr);
    cairo_move_to(cr, px[0], mid);
    draw_smooth_curve(cr, px, py, points);
    cairo_line_to(cr, px[points - 1], mid);
    cairo_close_path(cr);
    cairo_pattern_t *ug = cairo_pattern_create_linear(0, mid - amp, 0, mid + amp);
    cairo_pattern_add_color_stop_rgba(ug, 0.0, 0.0, 0.85, 1.0, 0.20 + 0.15 * energy);
    cairo_pattern_add_color_stop_rgba(ug, 0.5, 0.5, 0.10, 0.9, 0.08);
    cairo_pattern_add_color_stop_rgba(ug, 1.0, 0.0, 0.0, 0.0, 0.0);
    cairo_set_source(cr, ug);
    cairo_fill(cr);
    cairo_pattern_destroy(ug);

    /* Secondary counter wave (neon magenta) */
    cairo_set_line_width(cr, 6.0);
    cairo_set_source_rgba(cr, 1.0, 0.15, 0.65, 0.12);
    draw_smooth_curve(cr, px, py2, points);
    cairo_stroke(cr);
    cairo_set_line_width(cr, 1.8);
    cairo_set_source_rgba(cr, 1.0, 0.25, 0.75, 0.70);
    draw_smooth_curve(cr, px, py2, points);
    cairo_stroke(cr);

    /* Primary wave passes (electric cyan laser) */
    cairo_set_line_width(cr, 14.0);
    cairo_set_source_rgba(cr, 0.0, 0.80, 1.0, 0.08);
    draw_smooth_curve(cr, px, py, points);
    cairo_stroke(cr);
    cairo_set_line_width(cr, 5.0);
    cairo_set_source_rgba(cr, 0.10, 0.90, 1.0, 0.32);
    draw_smooth_curve(cr, px, py, points);
    cairo_stroke(cr);
    cairo_set_line_width(cr, 2.2);
    cairo_set_source_rgba(cr, 0.70, 0.98, 1.0, 0.90);
    draw_smooth_curve(cr, px, py, points);
    cairo_stroke(cr);
    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
    draw_smooth_curve(cr, px, py, points);
    cairo_stroke(cr);
}

static void draw_vu(cairo_t *cr, double w, double h, const float *bands,
                    int nb, double phase, double energy) {
    (void)bands; (void)nb; (void)phase;
    background(cr, w, h, energy * 0.45, HA_DEFAULT, HB_DEFAULT);
    const char *labels[2] = {"CH 1 · LEFT", "CH 2 · RIGHT"};
    for (int index = 0; index < 2; index++) {
        double value = index == 0 ? energy : (energy * 1.06 > 1 ? 1 : energy * 1.06);
        double cx = w * (0.30 + 0.40 * index);
        double cy = h * 0.65;
        double rw = w * 0.17, rh = h * 0.36;
        double radius = rw < rh ? rw : rh;
        double start = M_PI * 0.80, end = M_PI * 2.20;

        /* Ambient dial plate circle */
        cairo_pattern_t *dp = cairo_pattern_create_radial(cx, cy - radius * 0.2, 5, cx, cy, radius * 1.15);
        cairo_pattern_add_color_stop_rgba(dp, 0.0, 0.08, 0.12, 0.24, 0.85);
        cairo_pattern_add_color_stop_rgba(dp, 0.85, 0.03, 0.05, 0.12, 0.90);
        cairo_pattern_add_color_stop_rgba(dp, 1.0, 0.01, 0.02, 0.05, 0.0);
        cairo_set_source(cr, dp);
        cairo_arc(cr, cx, cy, radius * 1.15, 0, TAU);
        cairo_fill(cr);
        cairo_pattern_destroy(dp);

        /* Arc track background */
        cairo_set_line_width(cr, 8);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.07);
        cairo_arc(cr, cx, cy, radius, start, end);
        cairo_stroke(cr);

        /* Glowing active arc */
        cairo_pattern_t *g = cairo_pattern_create_linear(cx - radius, cy, cx + radius, cy);
        cairo_pattern_add_color_stop_rgb(g, 0.0, 0.00, 0.85, 1.00); /* cyan */
        cairo_pattern_add_color_stop_rgb(g, 0.6, 0.50, 0.20, 0.95); /* violet */
        cairo_pattern_add_color_stop_rgb(g, 0.82, 1.00, 0.75, 0.10); /* amber */
        cairo_pattern_add_color_stop_rgb(g, 1.0, 1.00, 0.18, 0.25); /* redline */
        cairo_set_source(cr, g);
        cairo_set_line_width(cr, 8);
        cairo_arc(cr, cx, cy, radius, start, start + (end - start) * value);
        cairo_stroke(cr);
        cairo_pattern_destroy(g);

        /* Ticks around the gauge */
        for (int t = 0; t <= 10; t++) {
            double ta = start + (end - start) * (t / 10.0);
            double r1 = radius + 6.0;
            double r2 = radius + (t % 5 == 0 ? 12.0 : 9.0);
            if (t >= 8) cairo_set_source_rgba(cr, 1.0, 0.25, 0.30, 0.75);
            else cairo_set_source_rgba(cr, 0.6, 0.85, 1.0, 0.40);
            cairo_set_line_width(cr, t % 5 == 0 ? 2.0 : 1.2);
            cairo_move_to(cr, cx + cos(ta) * r1, cy + sin(ta) * r1);
            cairo_line_to(cr, cx + cos(ta) * r2, cy + sin(ta) * r2);
            cairo_stroke(cr);
        }

        /* Meter needle */
        double angle = start + (end - start) * value;
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        /* Needle shadow */
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.45);
        cairo_set_line_width(cr, 2.5);
        cairo_move_to(cr, cx + 2, cy + 2);
        cairo_line_to(cr, cx + 2 + cos(angle) * radius * 0.88,
                      cy + 2 + sin(angle) * radius * 0.88);
        cairo_stroke(cr);
        /* Needle body */
        cairo_set_source_rgba(cr, 1.0, 0.95, 0.95, 0.95);
        cairo_set_line_width(cr, 2.0);
        cairo_move_to(cr, cx, cy);
        cairo_line_to(cr, cx + cos(angle) * radius * 0.88,
                      cy + sin(angle) * radius * 0.88);
        cairo_stroke(cr);

        /* Needle glowing pivot center */
        cairo_pattern_t *piv = cairo_pattern_create_radial(cx - 1, cy - 1, 1, cx, cy, 6);
        cairo_pattern_add_color_stop_rgb(piv, 0.0, 1.0, 0.35, 0.45);
        cairo_pattern_add_color_stop_rgb(piv, 1.0, 0.15, 0.18, 0.30);
        cairo_set_source(cr, piv);
        cairo_arc(cr, cx, cy, 5.0, 0, TAU);
        cairo_fill(cr);
        cairo_pattern_destroy(piv);

        /* Label */
        cairo_set_source_rgba(cr, 0.70, 0.85, 1.0, 0.75);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, radius * 0.12 > 10 ? radius * 0.12 : 10);
        cairo_move_to(cr, cx - radius * 0.40, cy + radius * 0.45);
        cairo_show_text(cr, labels[index]);
    }
}

static void draw_cthugha(cairo_t *cr, double w, double h, const float *bands,
                         int nb, double phase, double energy) {
    const double ha[3] = {0.05, 0.04, 0.13}, hb[3] = {0.010, 0.012, 0.026};
    background(cr, w, h, energy * 0.55, ha, hb);
    double cx = w * 0.5, cy = h * 0.5, base = (w < h ? w : h) * 1.02;
    int lobes = 6;
    for (int i = 0; i < lobes; i++) {
        int step = nb / lobes > 0 ? nb / lobes : 1;
        double band = sample(bands, nb, i * step);
        double hue = 0.55 + i * 0.13 + phase * 0.030;
        double spin = phase * (0.55 + 0.25 * energy) + i * TAU / lobes;
        double length = base * (0.17 + 0.20 * band) *
                        (0.86 + 0.14 * sin(phase * 0.6 + i));
        double twist = 0.28 * sin(phase * 0.45 + i * 1.7);
        for (int ghost = 2; ghost >= 1; ghost--) {
            cairo_save(cr);
            cairo_translate(cr, cx, cy);
            cairo_rotate(cr, spin - ghost * 0.055 * (0.6 + energy));
            ribbon(cr, length, base * 0.028, hue, 0.035 + 0.015 * (2 - ghost), twist);
            cairo_restore(cr);
        }
        cairo_save(cr);
        cairo_translate(cr, cx, cy);
        cairo_rotate(cr, spin);
        ribbon(cr, length, base * 0.030, hue, 0.60, twist);
        cairo_restore(cr);
    }

    double low = sample(bands, nb, 1);
    double core = base * (0.055 + 0.030 * low + 0.010 * sin(phase * 1.1));
    cairo_pattern_t *g = cairo_pattern_create_radial(cx, cy, 0, cx, cy, core * 3.2);
    cairo_pattern_add_color_stop_rgba(g, 0.0, 1.0, 0.96, 0.90, 0.80);
    cairo_pattern_add_color_stop_rgba(g, 0.25, 0.55, 0.80, 1.0, 0.38);
    cairo_pattern_add_color_stop_rgba(g, 1.0, 0.30, 0.20, 0.85, 0.0);
    cairo_set_source(cr, g);
    cairo_arc(cr, cx, cy, core * 3.2, 0, TAU);
    cairo_fill(cr);
    cairo_pattern_destroy(g);

    double ring = base * (0.155 + 0.045 * energy);
    cairo_save(cr);
    cairo_translate(cr, cx, cy + base * 0.02 * sin(phase * 0.5));
    cairo_set_line_width(cr, 1.4);
    for (int k = 0; k < 36; k++) {
        double a = k * TAU / 36 + phase * 0.35;
        double amp = sample(bands, nb, k);
        double rr = ring * (1.0 + 0.10 * amp);
        set_hsl(cr, 0.58 + k * 0.004 + phase * 0.01, 0.55, 0.62, 0.18 + 0.35 * amp);
        cairo_arc(cr, cos(a) * rr, sin(a) * rr, 1.1 + 2.0 * amp, 0, TAU);
        cairo_fill(cr);
    }
    cairo_restore(cr);
    vignette(cr, w, h, 0.5);
}

static void draw_atari(cairo_t *cr, double w, double h, const float *bands,
                       int nb, double phase, double energy) {
    (void)energy;
    cairo_set_source_rgb(cr, 0.008, 0.006, 0.014);
    cairo_paint(cr);
    double cx = w * 0.5, cy = h * 0.56, base = w < h ? w : h;
    static const double palette[6][3] = {
        {1.00, 0.22, 0.72}, {0.22, 0.92, 1.00}, {1.00, 0.86, 0.22},
        {0.38, 1.00, 0.46}, {1.00, 0.48, 0.16}, {0.62, 0.44, 1.00}
    };
    int count = 7;
    double step = base * 0.118;
    double floor_y = cy + base * 0.30;
    cairo_set_source_rgba(cr, 0.35, 0.55, 1.0, 0.18);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, w * 0.04, floor_y);
    cairo_line_to(cr, w * 0.96, floor_y);
    cairo_stroke(cr);

    for (int i = 0; i < count; i++) {
        int idx = i * (nb / count > 0 ? nb / count : 1);
        double band = sample(bands, nb, idx);
        double size = base * (0.022 + 0.085 * band);
        double offset = (i - (count - 1) / 2.0) * step;
        int ci = ((int)(phase * 0.12 + i)) % 6;
        if (ci < 0) ci += 6;
        const double *color = palette[ci];
        double tilt = fmod(phase * 0.45 + i * 0.35, TAU);
        for (int sg = 0; sg < 2; sg++) {
            int sign = sg == 0 ? 1 : -1;
            if (i == 3 && sign == -1) continue;
            double x = cx + offset * sign;
            cairo_save(cr);
            cairo_translate(cr, x, cy - size * 0.55);
            cairo_rotate(cr, tilt * sign * 0.35);
            cairo_set_source_rgba(cr, color[0], color[1], color[2],
                                  0.22 + 0.45 * band);
            cairo_set_line_width(cr, 2.0 + 2.0 * band);
            int kind = i % 3;
            if (kind == 0) {
                cairo_arc(cr, 0, 0, size, 0, TAU);
                cairo_fill_preserve(cr);
                cairo_stroke(cr);
            } else if (kind == 1) {
                cairo_rectangle(cr, -size * 0.8, -size * 0.8,
                                size * 1.6, size * 1.6);
                cairo_fill_preserve(cr);
                cairo_stroke(cr);
            } else {
                cairo_move_to(cr, 0, -size);
                cairo_line_to(cr, size * 0.92, size * 0.72);
                cairo_line_to(cr, -size * 0.92, size * 0.72);
                cairo_close_path(cr);
                cairo_fill_preserve(cr);
                cairo_stroke(cr);
            }
            cairo_restore(cr);

            cairo_save(cr);
            cairo_translate(cr, x, floor_y + (floor_y - (cy - size * 0.55)));
            cairo_scale(cr, 1.0, -0.55);
            cairo_set_source_rgba(cr, color[0], color[1], color[2],
                                  0.05 + 0.10 * band);
            cairo_arc(cr, 0, 0, size, 0, TAU);
            cairo_fill(cr);
            cairo_restore(cr);
        }
    }

    cairo_set_source_rgba(cr, 0, 0, 0, 0.16);
    for (int y = 0; y < (int)h; y += 3) cairo_rectangle(cr, 0, y, w, 1);
    cairo_fill(cr);
}

static void draw_kaleido(cairo_t *cr, double w, double h, const float *bands,
                         int nb, double phase, double energy) {
    const double ha[3] = {0.10, 0.05, 0.20}, hb[3] = {0.012, 0.010, 0.026};
    background(cr, w, h, energy * 0.5, ha, hb);
    double cx = w * 0.5, cy = h * 0.5, base = (w < h ? w : h) * 0.46;
    float data[28];
    resample(bands, nb, data, 28);
    int segments = 12;

    for (int k = 0; k < segments; k++) {
        cairo_save(cr);
        cairo_translate(cr, cx, cy);
        cairo_rotate(cr, phase * 0.10 + k * TAU / segments);
        double half = (TAU / segments) * 0.46;
        double px[28], py[28];
        for (int j = 0; j < 28; j++) {
            double t = j / 27.0;
            double ang = half * t;
            double rr = base * (0.08 + 0.92 * t) * (0.50 + 0.50 * data[j]);
            px[j] = cos(ang) * rr;
            py[j] = sin(ang) * rr;
        }
        cairo_move_to(cr, 0, 0);
        for (int j = 0; j < 28; j++) cairo_line_to(cr, px[j], py[j]);
        for (int j = 27; j >= 0; j--) cairo_line_to(cr, px[j], -py[j]);
        cairo_close_path(cr);
        double hue = 0.52 + k * 0.012 + phase * 0.02;
        cairo_pattern_t *g = cairo_pattern_create_radial(0, 0, base * 0.05, 0, 0, base);
        double r, gg, b;
        hls_to_rgb(hue, 0.66, 0.70, &r, &gg, &b);
        cairo_pattern_add_color_stop_rgba(g, 0.0, r, gg, b, 0.34);
        hls_to_rgb(hue + 0.08, 0.42, 0.75, &r, &gg, &b);
        cairo_pattern_add_color_stop_rgba(g, 0.65, r, gg, b, 0.20);
        hls_to_rgb(hue + 0.16, 0.30, 0.80, &r, &gg, &b);
        cairo_pattern_add_color_stop_rgba(g, 1.0, r, gg, b, 0.02);
        cairo_set_source(cr, g);
        cairo_fill_preserve(cr);
        cairo_pattern_destroy(g);
        cairo_set_line_width(cr, 1.0);
        set_hsl(cr, hue + 0.05, 0.45, 0.78, 0.20 + 0.25 * energy);
        cairo_stroke(cr);
        cairo_restore(cr);
    }
    for (int k = 0; k < 3; k++) {
        cairo_set_line_width(cr, 1.0);
        set_hsl(cr, 0.55 + k * 0.05, 0.30, 0.70, 0.10 + 0.10 * energy);
        cairo_arc(cr, cx, cy,
                  base * (0.18 + 0.14 * k) * (1.0 + 0.02 * sin(phase + k)),
                  0, TAU);
        cairo_stroke(cr);
    }
}

static void draw_plasma(cairo_t *cr, double w, double h, const float *bands,
                        int nb, double phase, double energy) {
    const double ha[3] = {0.03, 0.02, 0.09}, hb[3] = {0.006, 0.008, 0.020};
    background(cr, w, h, energy * 0.4, ha, hb);
    double base = w < h ? w : h;
    static const double palette[8] = {0.60, 0.86, 0.12, 0.36, 0.94, 0.72, 0.05, 0.20};
    cairo_push_group(cr);
    for (int i = 0; i < 9; i++) {
        double band = sample(bands, nb, i * 4);
        double t = phase * (0.20 + 0.05 * (i % 3)) + i * 2.4;
        double x = w * 0.5 + cos(t) * w * (0.10 + 0.20 * field(i, 0.5, phase * 0.3));
        double y = h * 0.5 + sin(t * 0.83) * h * (0.08 + 0.24 * field(0.5, i, phase * 0.25));
        double radius = base * (0.10 + 0.16 * band) * (0.85 + 0.15 * sin(phase * 0.8 + i));
        double hue = palette[i % 8] + phase * 0.01;
        double rr = radius > 6 ? radius : 6;
        cairo_pattern_t *g = cairo_pattern_create_radial(x, y, 0, x, y, rr);
        double r0, g0, b0;
        hls_to_rgb(hue, 0.58, 0.80, &r0, &g0, &b0);
        cairo_pattern_add_color_stop_rgba(g, 0.0, r0, g0, b0, 0.34 + 0.20 * band);
        cairo_pattern_add_color_stop_rgba(g, 0.55, r0 * 0.6, g0 * 0.6, b0 * 0.9,
                                          0.14 + 0.10 * band);
        cairo_pattern_add_color_stop_rgba(g, 1.0, 0, 0, 0, 0);
        cairo_set_source(cr, g);
        cairo_arc(cr, x, y, rr, 0, TAU);
        cairo_fill(cr);
        cairo_pattern_destroy(g);
    }
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, 0.92);
    cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
    cairo_paint_with_alpha(cr, 0.30);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    cairo_set_line_width(cr, 1.4);
    for (int k = 0; k < 6; k++) {
        double amp = h * (0.05 + 0.03 * sample(bands, nb, k * 6));
        double y0 = h * (0.18 + 0.13 * k);
        set_hsl(cr, 0.55 + k * 0.03 + phase * 0.01, 0.50, 0.70,
                0.06 + 0.05 * energy);
        cairo_move_to(cr, 0, y0);
        double step = w / 48.0 > 8 ? w / 48.0 : 8;
        for (double x = 0; x <= w + step; x += step)
            cairo_line_to(cr, x, y0 + sin(x * 0.006 + phase * 1.2 + k) * amp
                                + field(x / w * 3.0, k * 0.7, phase * 0.4) * amp * 0.6);
        cairo_stroke(cr);
    }
}

static void draw_torus(cairo_t *cr, double w, double h, const float *bands,
                       int nb, double phase, double energy,
                       const float *wave, int nw) {
    const double ha[3] = {0.04, 0.07, 0.18}, hb[3] = {0.008, 0.010, 0.024};
    background(cr, w, h, energy * 0.5, ha, hb);
    double cx = w * 0.5, cy = h * 0.5, base = w < h ? w : h;
    double big = base * 0.29 * (1.0 + 0.05 * energy);
    double tube = base * 0.105 * (1.0 + 0.14 * energy);
    double scale = base * 0.80;
    double ay = phase * 0.42;
    double ax = 0.95 + 0.18 * sin(phase * 0.3);
    double ca = cos(ay), sa = sin(ay), cb = cos(ax), sb = sin(ax);

    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    for (int i = 0; i < 30; i++) {
        double u = i * TAU / 30;
        double band = sample(bands, nb, i);
        double prevx = 0, prevy = 0;
        int have = 0;
        for (int j = 0; j < 19; j++) {
            double v = j * TAU / 18;
            const float *src = (wave && nw > 0) ? wave : bands;
            int sn = (wave && nw > 0) ? nw : nb;
            double wobble = 1.0 + 0.20 * sample(src, sn, (int)((u / TAU) * 48));
            double rr = tube * wobble;
            double x = (big + rr * cos(v)) * cos(u);
            double y = (big + rr * cos(v)) * sin(u);
            double z = rr * sin(v);
            double x2 = x * ca + z * sa, z2 = -x * sa + z * ca;
            double y2 = y * cb - z2 * sb, z3 = y * sb + z2 * cb;
            double px = cx + x2 * scale / (base * 0.62);
            double py = cy + y2 * scale / (base * 0.62);
            if (have) {
                double depth = (z3 + tube) / (2.0 * tube + 1e-6);
                set_hsl(cr, 0.54 + 0.10 * depth + phase * 0.02, 0.55,
                        0.42 + 0.34 * depth,
                        0.10 + 0.30 * depth + 0.10 * band);
                cairo_set_line_width(cr, 0.7 + 1.5 * depth);
                cairo_move_to(cr, prevx, prevy);
                cairo_line_to(cr, px, py);
                cairo_stroke(cr);
            }
            prevx = px; prevy = py; have = 1;
        }
    }
    for (int j = 0; j < 14; j++) {
        double v = j * TAU / 14;
        double prevx = 0, prevy = 0;
        int have = 0;
        for (int i = 0; i < 49; i++) {
            double u = i * TAU / 48;
            double wobble = 1.0 + 0.20 * sample(bands, nb, (int)((u / TAU) * 48));
            double rr = tube * wobble;
            double x = (big + rr * cos(v)) * cos(u);
            double y = (big + rr * cos(v)) * sin(u);
            double z = rr * sin(v);
            double x2 = x * ca + z * sa, z2 = -x * sa + z * ca;
            double y2 = y * cb - z2 * sb, z3 = y * sb + z2 * cb;
            double px = cx + x2 * scale / (base * 0.62);
            double py = cy + y2 * scale / (base * 0.62);
            if (have) {
                double depth = (z3 + tube) / (2.0 * tube + 1e-6);
                set_hsl(cr, 0.58 + 0.08 * depth, 0.40,
                        0.50 + 0.30 * depth, 0.06 + 0.20 * depth);
                cairo_set_line_width(cr, 0.6 + 1.1 * depth);
                cairo_move_to(cr, prevx, prevy);
                cairo_line_to(cr, px, py);
                cairo_stroke(cr);
            }
            prevx = px; prevy = py; have = 1;
        }
    }
}

static void draw_vortex(cairo_t *cr, double w, double h, const float *bands,
                        int nb, double phase, double energy) {
    const double ha[3] = {0.05, 0.05, 0.18}, hb[3] = {0.008, 0.010, 0.022};
    background(cr, w, h, energy * 0.5, ha, hb);
    double cx = w * 0.5, cy = h * 0.5, base = (w < h ? w : h) * 0.55;
    int arms = 8, steps = 46;
    double spin = phase * (0.35 + 0.55 * energy);
    for (int arm = 0; arm < arms; arm++) {
        for (int k = 0; k < steps; k++) {
            double t = k / (double)(steps - 1);
            double band = sample(bands, nb, arm * 6 + k / 3);
            double ang = spin + arm * TAU / arms + t * 3.1;
            double rr = base * (0.05 + 0.92 * t) * (0.9 + 0.20 * band);
            double x = cx + cos(ang) * rr;
            double y = cy + sin(ang) * rr * 0.92;
            double size = 0.5 + 2.2 * t * (0.4 + band);
            double alpha = (1.0 - t) * (0.10 + 0.55 * band);
            set_hsl(cr, 0.52 + arm * 0.010 + t * 0.10, 0.62, 0.60, alpha);
            cairo_arc(cr, x, y, size, 0, TAU);
            cairo_fill(cr);
        }
    }
    double core = base * (0.05 + 0.03 * energy);
    cairo_pattern_t *g = cairo_pattern_create_radial(cx, cy, 0, cx, cy, core * 4);
    cairo_pattern_add_color_stop_rgba(g, 0.0, 1.0, 0.98, 0.95, 0.55);
    cairo_pattern_add_color_stop_rgba(g, 0.4, 0.55, 0.80, 1.0, 0.22);
    cairo_pattern_add_color_stop_rgba(g, 1.0, 0.20, 0.25, 0.80, 0.0);
    cairo_set_source(cr, g);
    cairo_arc(cr, cx, cy, core * 4, 0, TAU);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
}

static void draw_spiral(cairo_t *cr, double w, double h, const float *bands,
                        int nb, double phase, double energy) {
    const double ha[3] = {0.09, 0.05, 0.16}, hb[3] = {0.010, 0.008, 0.020};
    background(cr, w, h, energy * 0.45, ha, hb);
    double cx = w * 0.5, cy = h * 0.5, base = (w < h ? w : h) * 0.48;
    int steps = 190;
    double spin = phase * 0.30;
    for (int k = 0; k < steps; k++) {
        double t = k / (double)steps;
        double band = sample(bands, nb, k / 3);
        double ang = spin + t * 3.4 * TAU;
        double rr = base * (0.045 + 0.95 * t) * (0.94 + 0.08 * band);
        double x = cx + cos(ang) * rr;
        double y = cy + sin(ang) * rr;
        double size = 0.7 + 2.4 * (0.25 + band) * (1.0 - 0.4 * t);
        set_hsl(cr, 0.50 + t * 0.34, 0.60, 0.62,
                0.10 + 0.50 * band * (1.0 - 0.5 * t));
        cairo_arc(cr, x, y, size, 0, TAU);
        cairo_fill(cr);
    }
    cairo_pattern_t *g = cairo_pattern_create_radial(cx, cy, 0, cx, cy, base * 0.30);
    cairo_pattern_add_color_stop_rgba(g, 0.0, 1.0, 0.95, 0.88, 0.40);
    cairo_pattern_add_color_stop_rgba(g, 1.0, 0.55, 0.35, 1.0, 0.0);
    cairo_set_source(cr, g);
    cairo_arc(cr, cx, cy, base * 0.30, 0, TAU);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
}

typedef struct { double bx, by, rate, seed, size; } Particle;
static Particle particles[190];
static int particles_ready;
static void init_particles(void) {
    if (particles_ready) return;
    unsigned s = 1977u;
    for (int i = 0; i < 190; i++) {
        s = s * 1103515245u + 12345u;
        particles[i].bx = ((s >> 8) & 0xFFFFFF) / 16777216.0;
        s = s * 1103515245u + 12345u;
        particles[i].by = ((s >> 8) & 0xFFFFFF) / 16777216.0;
        s = s * 1103515245u + 12345u;
        particles[i].rate = 0.4 + 1.2 * (((s >> 8) & 0xFFFFFF) / 16777216.0);
        s = s * 1103515245u + 12345u;
        particles[i].seed = (((s >> 8) & 0xFFFFFF) / 16777216.0) * TAU;
        s = s * 1103515245u + 12345u;
        particles[i].size = 0.7 + 1.7 * (((s >> 8) & 0xFFFFFF) / 16777216.0);
    }
    particles_ready = 1;
}

static void draw_particles(cairo_t *cr, double w, double h, const float *bands,
                           int nb, double phase, double energy,
                           const float *wave, int nw) {
    const double ha[3] = {0.04, 0.06, 0.16}, hb[3] = {0.008, 0.010, 0.022};
    background(cr, w, h, energy * 0.55, ha, hb);
    init_particles();
    double speed = 0.6 + 0.9 * energy;

    double px[190], py[190], prad[190], phue[190], palpha[190];
    for (int index = 0; index < 190; index++) {
        Particle *p = &particles[index];
        double x = fmod(p->bx + 0.055 * sin(phase * p->rate * 0.30 * speed + p->seed)
                        + 0.012 * field(p->bx * 4, p->by * 4, phase * 0.20) + 1.0, 1.0);
        double y = fmod(p->by + 0.055 * cos(phase * p->rate * 0.23 * speed + p->seed * 1.7)
                        + 0.012 * field(p->bx * 4 + 3, p->by * 4 + 3, phase * 0.20) + 1.0, 1.0);
        double band = sample(bands, nb, index * 7 / 190 * 8);
        px[index] = x * w;
        py[index] = y * h;
        prad[index] = p->size * (0.8 + 2.2 * band);
        phue[index] = 0.55 + 0.14 * sin(p->seed);
        palpha[index] = 0.15 + 0.65 * band;
    }

    /* Constellation connections between nearby particles */
    double max_dist = (w < h ? w : h) * 0.12;
    double max_dist_sq = max_dist * max_dist;
    int check_count = 100;
    cairo_set_line_width(cr, 1.0);
    for (int i = 0; i < check_count; i++) {
        for (int j = i + 1; j < check_count; j++) {
            double dx = px[i] - px[j];
            double dy = py[i] - py[j];
            double dsq = dx * dx + dy * dy;
            if (dsq < max_dist_sq) {
                double dist = sqrt(dsq);
                double link_a = (1.0 - dist / max_dist) * (0.05 + 0.30 * energy);
                cairo_set_source_rgba(cr, 0.35, 0.80, 1.0, link_a);
                cairo_move_to(cr, px[i], py[i]);
                cairo_line_to(cr, px[j], py[j]);
                cairo_stroke(cr);
            }
        }
    }

    cairo_push_group(cr);
    for (int index = 0; index < 190; index++) {
        /* Soft halo */
        set_hsl(cr, phue[index], 0.65, 0.70, palpha[index] * 0.40);
        cairo_arc(cr, px[index], py[index], prad[index] * 2.2, 0, TAU);
        cairo_fill(cr);
        /* Bright core */
        set_hsl(cr, phue[index], 0.85, 0.92, palpha[index]);
        cairo_arc(cr, px[index], py[index], prad[index], 0, TAU);
        cairo_fill(cr);
    }
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, 0.90);

    if (wave && nw > 0) {
        cairo_set_line_width(cr, 2.0);
        cairo_set_source_rgba(cr, 0.45, 0.90, 1.0, 0.18 + 0.35 * energy);
        double mid = h * 0.62;
        for (int i = 0; i < nw; i++) {
            double x = w * i / (nw > 1 ? nw - 1 : 1);
            double y = mid + (wave[i] - 0.15) * h * 0.22;
            if (i == 0) cairo_move_to(cr, x, y);
            else cairo_line_to(cr, x, y);
        }
        cairo_stroke(cr);
    }
}

static void draw_tunnel(cairo_t *cr, double w, double h, const float *bands,
                        int nb, double phase, double energy,
                        const float *wave, int nw) {
    const double ha[3] = {0.03, 0.04, 0.14}, hb[3] = {0.006, 0.008, 0.018};
    background(cr, w, h, energy * 0.45, ha, hb);
    double cx = w * 0.5, cy = h * 0.5, base = w < h ? w : h;
    int count = 34;
    double speed = 0.10 + 0.10 * energy;
    double min_r = base * 0.02, max_r = base * 0.85;

    for (int k = 0; k < count; k++) {
        double u = fmod(k / (double)count + phase * speed, 1.0);
        double radius = min_r * pow(max_r / min_r, u);
        double band = sample(bands, nb, k);
        double alpha = (0.05 + 0.42 * u) * (0.55 + 0.55 * band);
        cairo_set_line_width(cr, 0.7 + 3.0 * u);
        set_hsl(cr, 0.52 + 0.20 * u + phase * 0.02, 0.62, 0.58, alpha);
        cairo_new_sub_path(cr);
        for (int i = 0; i <= 56; i++) {
            double a = i * TAU / 56 + phase * 0.25 + u * 1.4;
            double wob = 1.0 + 0.10 * field(i * 0.35, u * 3.0, phase * 0.5);
            if (wave && nw > 0) wob *= 1.0 + 0.10 * sample(wave, nw, i * 2);
            double x = cx + cos(a) * radius * wob;
            double y = cy + sin(a) * radius * wob * 0.94;
            if (i == 0) cairo_move_to(cr, x, y);
            else cairo_line_to(cr, x, y);
        }
        cairo_stroke(cr);
    }

    double core = base * (0.012 + 0.02 * energy);
    cairo_pattern_t *g = cairo_pattern_create_radial(cx, cy, 0, cx, cy, core * 6);
    cairo_pattern_add_color_stop_rgba(g, 0.0, 1, 1, 1, 0.70);
    cairo_pattern_add_color_stop_rgba(g, 1.0, 0.45, 0.55, 1.0, 0.0);
    cairo_set_source(cr, g);
    cairo_arc(cr, cx, cy, core * 6, 0, TAU);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
}

/* ------------------------------------------------------------- entry point */
void nava_vis_draw(cairo_t *cr, int w, int h, const NavaVis *v) {
    if (!cr || w < 8 || h < 8 || !v) return;
    if (nava_vis_is_off(v->mode)) return;
    float bands[NBANDS];
    int nb = NBANDS;
    if (v->sensitivity > 0.01) {
        double inv = 1.0 / v->sensitivity;
        for (int i = 0; i < NBANDS; i++) {
            double x = clampi(v->bands[i], 0, 1);
            bands[i] = (float)pow(x, inv);
        }
    } else {
        memcpy(bands, v->bands, sizeof(bands));
    }
    double energy = clampi(v->energy, 0, 1);
    switch (v->mode) {
    case 0: draw_cthugha(cr, w, h, bands, nb, v->phase, energy); break;
    case 1: draw_atari(cr, w, h, bands, nb, v->phase, energy); break;
    case 2: draw_cosmic(cr, w, h, bands, nb, v->phase, energy); break;
    case 3: draw_kaleido(cr, w, h, bands, nb, v->phase, energy); break;
    case 4: draw_plasma(cr, w, h, bands, nb, v->phase, energy); break;
    case 5: draw_torus(cr, w, h, bands, nb, v->phase, energy, v->wave, NWAVE); break;
    case 6: draw_vortex(cr, w, h, bands, nb, v->phase, energy); break;
    case 7: draw_spiral(cr, w, h, bands, nb, v->phase, energy); break;
    case 8: draw_particles(cr, w, h, bands, nb, v->phase, energy, v->wave, NWAVE); break;
    case 9: draw_tunnel(cr, w, h, bands, nb, v->phase, energy, v->wave, NWAVE); break;
    case 10: draw_bars(cr, w, h, bands, nb, v->phase, energy); break;
    case 11: draw_wave(cr, w, h, bands, nb, v->phase, energy, v->wave, NWAVE); break;
    case 12: draw_vu(cr, w, h, bands, nb, v->phase, energy); break;
    default: break;
    }
}
