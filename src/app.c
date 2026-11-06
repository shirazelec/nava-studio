/* app.c — Nava Studio, native GTK4 interface. */
#include "app.h"
#include "engine.h"
#include "vis.h"
#include "ui_helpers.h"

#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <gio/gio.h>

#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#include <gdk/win32/gdkwin32.h>
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define APP_ID "com.shirazelectronic.NavaStudio"
#define VERSION "1.0.0"
#define MAX_TRACKS 4096
#define EQ_BANDS 10
#define VIS_BANDS 64
#define VIS_WAVE 128

typedef struct {
    char path[4096];
    char title[512];
    char meta[128];
    gboolean is_video;
} Track;

typedef enum { REPEAT_OFF, REPEAT_ALL, REPEAT_ONE } RepeatMode;

typedef struct {
    GtkWidget *window, *root;
    GtkWidget *sidebar, *content, *deck;
    GtkWidget *drawer;          /* reveals the playlist card over the stage */
    GtkWidget *stage_overlay;   /* stage + effects OSD */
    GtkWidget *root_overlay;    /* player area; playlist drawer floats here
                                 * so windowed mode overlays the whole player,
                                 * not just the video picture */
    guint drawer_timer;
    gboolean drawer_syncing;
    gboolean drawer_edge_open;  /* opened by hovering the left edge in fullscreen */
    GtkWidget *transport, *now_header, *status_bar;
    GtkWidget *stage, *picture, *vis_draw, *stage_art, *stage_caption;
    GtkWidget *stage_title, *stage_sub, *empty_label, *exit_button;
    GtkWidget *now_title, *now_sub, *format_chip, *quality_chip;
    GtkWidget *track_list, *search, *count_label;
    GtkWidget *play_button, *seek, *time_now, *time_total;
    GtkWidget *vol_scale, *mute_button, *shuffle_button, *repeat_button, *speed_combo;
    GtkWidget *eq_scales[EQ_BANDS], *eq_values[EQ_BANDS], *eq_check, *eq_preset;
    GtkWidget *echo_scale, *fb_scale, *delay_scale, *comp_scale;
    GtkWidget *reverb_scale, *flanger_scale;
    GtkWidget *xf_check, *xf_scale, *fade_scale, *fade_value, *xf_value;
    GtkWidget *bright_scale, *contrast_scale, *satur_scale, *gamma_scale, *hue_scale;
    GtkWidget *rotation_combo;
    GtkWidget *vis_combo, *vis_sens, *vis_check;
    GtkWidget *skin_combo;
    GtkWidget *status_label, *engine_pill;
    GtkWidget *sidebar_btn, *effects_btn, *fullscreen_btn, *mini_btn;
    GtkWidget *menu_popover;
    GtkWidget *header, *deck_stack;
    GtkWidget *shuffle_label, *repeat_label;
    guint hide_timer;
    gboolean controls_visible;
    gboolean chrome_hidden;
    /* Some backends deliver motion events even when the pointer is parked.
     * Remember the last position so only real movement reveals the OSD. */
    double last_motion_x, last_motion_y;
    gboolean motion_tracked;

    Engine *engine;
    GdkTexture *texture;
    uint8_t *cover_pixels;
    int cover_w, cover_h;
    cairo_surface_t *cover_surface;
    double cover_phase;
    float cover_energy;
    GtkWidget *art_cache_widget;

    NavaVis vis;
    float raw_spec[VIS_BANDS];
    float raw_wave[VIS_WAVE];
    float peak, rms;
    double last_tick_us;

    Track tracks[MAX_TRACKS];
    int track_count;
    int current;
    char last_folder[4096];

    gboolean visualizer_on;
    gboolean showing_video;
    gboolean fullscreen;
    gboolean seek_dragging;
    gboolean updating_seek;
    gboolean updating_list;
    double vclock;
    gint64 vclock_wall;
    double seek_pending;
    guint seek_timer;
    guint seek_idle;
    gboolean suppress_fx;

    gboolean shuffle;
    gboolean at_end;
    int repeat;
    double rate;
    double volume;
    gboolean muted;
    float eq_gains[EQ_BANDS];
    gboolean eq_enabled;
    int vis_sensitivity;
    int rotation;

    gboolean sidebar_on;
    gboolean show_effects;

    /* Crossfade between tracks and click-free fades for transport. */
    gboolean crossfade_on;
    double crossfade_ms;
    double fade_ms;
    gboolean xf_armed;

    /* Remember the last played track so Play resumes where the user left off,
     * and keep the mini-player plus the stage transition state. */
    char last_file[4096];
    gboolean mini;
    double art_anim;      /* 0 = corner, 1 = centred */
    double art_target;
    gboolean art_init;
    gboolean pending_seek;
    double pending_seek_pos;
    int pending_seek_ticks;
    
    /* Skin system */
    int skin_id;
    char skin_path[4096];

    gboolean snapshot_mode;
    int cli_tracks, cli_start, snapshot_tries;
    char *open_uri;      /* desktop file-manager launch: convert once, then replay */
    guint tick_id, persist_id, visuals_id, frame_id;

    /* MPRIS2: the D-Bus service that delivers hardware media keys. */
    GDBusConnection *mpris_bus;
    guint mpris_reg_root, mpris_reg_player, mpris_owner;
} App;

/* Skin definitions */
static const char *SKIN_NAMES[] = {
    "Default Dark", "Midnight Blue", "Forest Green", "Sunset Orange", 
    "Royal Purple", "Cherry Red", "Ocean Teal", "Monochrome",
    NULL
};
static const char *SKIN_CSS_FILES[] = {
    "style.css", "skin-midnight.css", "skin-forest.css", "skin-sunset.css",
    "skin-royal.css", "skin-cherry.css", "skin-ocean.css", "skin-mono.css"
};
#define SKIN_COUNT 8

#define N_VIS_MODES 14
static const char *VIS_LABELS[] = {
    "Cthugha (Fomalhaut)", "Atari Video Music", "CosmicBelt", "Kaleidoscope",
    "Plasma", "Torus", "Vortex", "Spiral out", "Particles", "Tunnel",
    "Spectrum bars", "Oscilloscope", "VU meter", "Off", NULL
};
static const double EQ_FREQS[EQ_BANDS] = {29, 59, 119, 237, 474, 947, 1889, 3770, 7523, 15011};
static const char *EQ_PRESET_NAMES[] = {
    "Flat", "Rock", "Pop", "Jazz", "Classical", "Dance", "Bass Boost",
    "Vocal", "Treble", "Club", "Techno", NULL
};
static const float EQ_PRESETS[11][EQ_BANDS] = {
    {0,0,0,0,0,0,0,0,0,0},
    {8,5,-5.5f,-8,-3,4,8,11,11,11.5f},
    {-1.6f,4.5,7,8,5.6f,0,-2.5f,-2,-1.6f,-1.5f},
    {4,3,1.5f,2,-1.5f,-1.5f,0,1.5f,3,4},
    {4.5f,3.5f,3,2.5f,-1.5f,-1.5f,0,2,3,4},
    {9.6f,7,2.5f,0,0,-5.6f,-7,-7,0,0},
    {10,8,6,4,2,0,0,0,0,0},
    {-2,-1,0,2,4,5,4,2,0,-1},
    {0,0,0,0,0,1,3,5,7,9},
    {0,0,8,6,5.5f,5,3,0,0,0},
    {8,5.5f,0,-5.5f,-5,0,8,5.5f,0,5}
};
static const char *ROTATION_NAMES[] = {"Normal", "90°", "180°", "270°", "Mirror H", "Mirror V", NULL};
static const char *SPEED_NAMES[] = {"0.5×", "0.75×", "1×", "1.25×", "1.5×", "2×", NULL};
static const double SPEEDS[] = {0.5, 0.75, 1.0, 1.25, 1.5, 2.0};

static const char *const AUDIO_EXTS[] = {
    "mp3","m4a","aac","flac","ogg","oga","opus","wav","wma","alac","aiff","aif","mka","ac3","dts",NULL
};
static const char *const VIDEO_EXTS[] = {
    "mp4","m4v","mkv","webm","avi","mov","wmv","flv","ts","m2ts","mpg","mpeg","ogv","3gp","vob","mts",NULL
};

/* ------------------------------------------------------------- utilities -- */
static const char *base_name(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}
static void strip_ext(char *dst, size_t n, const char *path) {
    const char *b = base_name(path);
    snprintf(dst, n, "%s", b);
    char *dot = strrchr(dst, '.');
    if (dot && dot != dst) *dot = 0;
}
static gboolean has_ext(const char *path, const char *const *exts) {
    const char *dot = strrchr(path, '.');
    if (!dot) return FALSE;
    for (int i = 0; exts[i]; i++)
        if (g_ascii_strcasecmp(dot + 1, exts[i]) == 0) return TRUE;
    return FALSE;
}
static gboolean is_media(const char *path) {
    return has_ext(path, AUDIO_EXTS) || has_ext(path, VIDEO_EXTS);
}
static void format_time(double seconds, char *out, size_t n) {
    if (seconds < 0 || !isfinite(seconds)) seconds = 0;
    int total = (int)seconds;
    int h = total / 3600, m = (total % 3600) / 60, s = total % 60;
    if (h > 0) snprintf(out, n, "%d:%02d:%02d", h, m, s);
    else snprintf(out, n, "%d:%02d", m, s);
}
static void set_status(App *a, const char *text, gboolean error) {
    if (!a->status_label) return;
    gtk_label_set_text(GTK_LABEL(a->status_label), text);
    if (error) gtk_widget_add_css_class(a->status_label, "status-error");
    else gtk_widget_remove_css_class(a->status_label, "status-error");
}
static GtkWidget *param_slider(const char *label, double low, double high, double step,
                               double value, GCallback cb, gpointer data,
                               GtkWidget **out_scale) {
    GtkWidget *row = ui_box(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *name = ui_label(label, "param-label", 0.0f);
    gtk_widget_set_size_request(name, 110, -1);
    GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, low, high, step);
    gtk_widget_add_css_class(scale, "param-scale");
    gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
    gtk_widget_set_hexpand(scale, TRUE);
    gtk_range_set_value(GTK_RANGE(scale), value);
    if (cb) g_signal_connect(scale, "value-changed", cb, data);
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f", value);
    GtkWidget *readout = ui_label(buf, "scale-value", 1.0f);
    gtk_widget_set_size_request(readout, 52, -1);
    g_object_set_data(G_OBJECT(scale), "readout", readout);
    gtk_box_append(GTK_BOX(row), name);
    gtk_box_append(GTK_BOX(row), scale);
    gtk_box_append(GTK_BOX(row), readout);
    if (out_scale) *out_scale = scale;
    return row;
}

/* ------------------------------------------------------------ playlist -- */
static void refresh_list(App *a);
static void play_index(App *a, int index);
static void set_fullscreen(App *a, gboolean on);
static void set_mini(App *a, gboolean on);
static void on_mini_toggle(GtkToggleButton *b, gpointer data);
static void switch_track(App *a, int index);
static void apply_stage_layout(App *a);
static void update_chips(App *a);
static void mpris_update(App *a);
static void on_activate(GtkApplication *gtk_app, gpointer data);

static int add_track(App *a, const char *path) {
    if (a->track_count >= MAX_TRACKS || !is_media(path)) return -1;
    for (int i = 0; i < a->track_count; i++)
        if (strcmp(a->tracks[i].path, path) == 0) return i;
    Track *t = &a->tracks[a->track_count];
    snprintf(t->path, sizeof(t->path), "%s", path);
    strip_ext(t->title, sizeof(t->title), path);
    snprintf(t->meta, sizeof(t->meta), "%s", base_name(path));
    t->is_video = has_ext(path, VIDEO_EXTS);
    return a->track_count++;
}

static int add_directory(App *a, const char *dir) {
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return 0;
    int added = 0;
    const char *name;
    while ((name = g_dir_read_name(d))) {
        if (name[0] == '.') continue;
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        if (g_file_test(path, G_FILE_TEST_IS_DIR)) added += add_directory(a, path);
        else if (is_media(path) && add_track(a, path) >= 0) added++;
    }
    g_dir_close(d);
    return added;
}

static void remove_track_at(App *a, int idx) {
    if (idx < 0 || idx >= a->track_count) return;
    memmove(&a->tracks[idx], &a->tracks[idx + 1],
            (size_t)(a->track_count - idx - 1) * sizeof(Track));
    a->track_count--;
    if (a->current >= a->track_count) a->current = a->track_count - 1;
}

static void move_track(App *a, int from, int to) {
    if (from < 0 || from >= a->track_count) return;
    if (to < 0) to = 0;
    if (to >= a->track_count) to = a->track_count - 1;
    if (from == to) return;
    Track tmp = a->tracks[from];
    if (from < to)
        memmove(&a->tracks[from], &a->tracks[from + 1],
                (size_t)(to - from) * sizeof(Track));
    else
        memmove(&a->tracks[to + 1], &a->tracks[to],
                (size_t)(from - to) * sizeof(Track));
    a->tracks[to] = tmp;
    if (a->current == from) a->current = to;
    else if (from < a->current && to >= a->current) a->current--;
    else if (from > a->current && to <= a->current) a->current++;
}

static gboolean row_is_match(App *a, const Track *t, const char *filter) {
    if (!filter || !*filter) return TRUE;
    char low[1024], f[256];
    snprintf(low, sizeof(low), "%s %s", t->title, t->meta);
    g_strlcpy(f, filter, sizeof(f));
    for (char *p = low; *p; p++) *p = g_ascii_tolower(*p);
    for (char *p = f; *p; p++) *p = g_ascii_tolower(*p);
    return strstr(low, f) != NULL;
}

static void refresh_list(App *a) {
    if (!a->track_list) return;
    /* Rebuilding the rows resets GtkListBox's selection to the first child.
     * Suppress that by clearing the selection first, then re-select the
     * playing row once every row exists (see the end of this function). */
    a->updating_list = TRUE;
    gtk_list_box_unselect_all(GTK_LIST_BOX(a->track_list));
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(a->track_list)))
        gtk_list_box_remove(GTK_LIST_BOX(a->track_list), child);

    const char *filter = gtk_editable_get_text(GTK_EDITABLE(a->search));
    GtkWidget *playing_row = NULL;
    int shown = 0;
    for (int i = 0; i < a->track_count; i++) {
        if (!row_is_match(a, &a->tracks[i], filter)) continue;
        GtkWidget *row = gtk_list_box_row_new();
        g_object_set_data(G_OBJECT(row), "index", GINT_TO_POINTER(i));
        GtkWidget *box = ui_box(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_start(box, 8);
        gtk_widget_set_margin_end(box, 8);
        gtk_widget_set_margin_top(box, 4);
        gtk_widget_set_margin_bottom(box, 4);
        GtkWidget *icon = gtk_image_new_from_icon_name(
            a->tracks[i].is_video ? "video-x-generic-symbolic" : "audio-x-generic-symbolic");
        gtk_image_set_pixel_size(GTK_IMAGE(icon), 16);
        gtk_widget_add_css_class(icon, "track-icon");
        gtk_box_append(GTK_BOX(box), icon);
        GtkWidget *words = ui_box(GTK_ORIENTATION_VERTICAL, 1);
        gtk_widget_set_hexpand(words, TRUE);
        GtkWidget *title = ui_label(a->tracks[i].title, "track-title", 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
        GtkWidget *sub = ui_label(a->tracks[i].meta, "track-sub", 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(sub), PANGO_ELLIPSIZE_MIDDLE);
        gtk_box_append(GTK_BOX(words), title);
        gtk_box_append(GTK_BOX(words), sub);
        gtk_box_append(GTK_BOX(box), words);
        if (i == a->current) {
            GtkWidget *playing = ui_label("▶", "track-duration", 1.0f);
            gtk_box_append(GTK_BOX(box), playing);
        }
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
        gtk_widget_add_css_class(row, "track-row");
        gtk_list_box_append(GTK_LIST_BOX(a->track_list), row);
        if (i == a->current) {
            gtk_widget_add_css_class(row, "track-playing");
            playing_row = row;
        }
        shown++;
    }
    /* Apply the selection only after the whole model is rebuilt: doing it
     * mid-loop made GtkListBox snap back to the first row. */
    if (playing_row) {
        gtk_list_box_select_row(GTK_LIST_BOX(a->track_list),
                                GTK_LIST_BOX_ROW(playing_row));
        gtk_list_box_row_changed(GTK_LIST_BOX_ROW(playing_row));
    }
    a->updating_list = FALSE;
    /* The "stage is empty" overlay must follow the model, not just the
     * startup path: adding/removing files has to hide it immediately. */
    if (a->empty_label)
        gtk_widget_set_visible(a->empty_label, a->track_count == 0);
    if (a->count_label) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", a->track_count);
        gtk_label_set_text(GTK_LABEL(a->count_label), buf);
    }
    (void)shown;
}

/* ------------------------------------------------------------ persistence */
static void save_state(App *a) {
    if (!a->window) return;
    char *dir = g_build_filename(g_get_user_data_dir(), "nava-studio", NULL);
    g_mkdir_with_parents(dir, 0700);
    char *file = g_build_filename(dir, "playlist.m3u", NULL);
    GString *s = g_string_new("#EXTM3U\n# Nava Studio playlist\n");
    for (int i = 0; i < a->track_count; i++)
        g_string_append_printf(s, "%s\n", a->tracks[i].path);
    g_file_set_contents(file, s->str, (gssize)s->len, NULL);
    g_string_free(s, TRUE);
    g_free(file);

    char *cfg = g_build_filename(dir, "state.ini", NULL);
    GString *c = g_string_new("[ui]\n");
    g_string_append_printf(c, "last_folder=%s\n", a->last_folder);
    g_string_append_printf(c, "last_file=%s\n",
                           (a->current >= 0 && a->current < a->track_count) ? a->tracks[a->current].path : a->last_file);
    g_string_append_printf(c, "volume=%.4f\n", a->volume);
    g_string_append_printf(c, "muted=%d\n", a->muted);
    g_string_append_printf(c, "visualizer=%d\n", a->vis.mode);
    g_string_append_printf(c, "visualizer_on=%d\n", a->visualizer_on);
    g_string_append_printf(c, "vis_sensitivity=%d\n", a->vis_sensitivity);
    g_string_append_printf(c, "repeat=%d\n", a->repeat);
    g_string_append_printf(c, "shuffle=%d\n", a->shuffle);
    g_string_append_printf(c, "rate=%.3f\n", a->rate);
    g_string_append_printf(c, "rotation=%d\n", a->rotation);
    g_string_append_printf(c, "eq_enabled=%d\n", a->eq_enabled);
    g_string_append_printf(c, "eq_gains=");
    for (int i = 0; i < EQ_BANDS; i++)
        g_string_append_printf(c, "%s%.2f", i ? "," : "", a->eq_gains[i]);
    g_string_append(c, "\n");
    g_string_append_printf(c, "crossfade_on=%d\n", a->crossfade_on);
    g_string_append_printf(c, "crossfade_ms=%.0f\n", a->crossfade_ms);
    g_string_append_printf(c, "fade_ms=%.0f\n", a->fade_ms);
    g_string_append_printf(c, "mini=%d\n", a->mini);
    g_string_append_printf(c, "skin=%d\n", a->skin_id);
    g_string_append_printf(c, "sidebar=%d\n", a->sidebar_on);
    g_string_append_printf(c, "effects=%d\n", a->show_effects);
    g_file_set_contents(cfg, c->str, (gssize)c->len, NULL);
    g_string_free(c, TRUE);
    g_free(cfg);
    g_free(dir);
}

static void restore_state(App *a) {
    char *dir = g_build_filename(g_get_user_data_dir(), "nava-studio", NULL);
    char *cfg = g_build_filename(dir, "state.ini", NULL);
    char *text = NULL;
    if (g_file_get_contents(cfg, &text, NULL, NULL)) {
        char *p;
        if ((p = strstr(text, "last_folder="))) sscanf(p + 12, "%4095[^\n]", a->last_folder);
        if ((p = strstr(text, "last_file="))) sscanf(p + 10, "%4095[^\n]", a->last_file);
        if ((p = strstr(text, "mini="))) a->mini = atoi(p + 5) != 0;
        if ((p = strstr(text, "volume="))) a->volume = atof(p + 7);
        if ((p = strstr(text, "visualizer="))) a->vis.mode = atoi(p + 11);
        if ((p = strstr(text, "repeat="))) a->repeat = atoi(p + 7);
        if ((p = strstr(text, "rate="))) a->rate = atof(p + 5);
        if ((p = strstr(text, "rotation="))) a->rotation = atoi(p + 9);
        if ((p = strstr(text, "eq_gains="))) {
            char *q = p + 9, *end = NULL;
            for (int i = 0; i < EQ_BANDS && q && *q && *q != '\n'; i++) {
                a->eq_gains[i] = (float)g_ascii_strtod(q, &end);
                if (end == q) break;
                q = (*end == ',') ? end + 1 : end;
                if (*end == '\n') break;
            }
        }
        if ((p = strstr(text, "vis_sensitivity="))) a->vis_sensitivity = atoi(p + 16);
        if ((p = strstr(text, "shuffle="))) a->shuffle = atoi(p + 8) != 0;
        if ((p = strstr(text, "eq_enabled="))) a->eq_enabled = atoi(p + 11) != 0;
        if ((p = strstr(text, "muted="))) a->muted = atoi(p + 6) != 0;
        if ((p = strstr(text, "visualizer_on="))) a->visualizer_on = atoi(p + 14) != 0;
        if ((p = strstr(text, "crossfade_on="))) a->crossfade_on = atoi(p + 13) != 0;
        if ((p = strstr(text, "crossfade_ms="))) a->crossfade_ms = atof(p + 13);
        if ((p = strstr(text, "fade_ms="))) a->fade_ms = atof(p + 8);
        if ((p = strstr(text, "skin="))) a->skin_id = atoi(p + 5);
        if ((p = strstr(text, "sidebar="))) a->sidebar_on = atoi(p + 8) != 0;
        if ((p = strstr(text, "effects="))) a->show_effects = atoi(p + 8) != 0;
        g_free(text);
    }
    g_free(cfg);
    a->vis.sensitivity = (float)(0.2 + a->vis_sensitivity * 0.05);
    a->rate = a->rate < 0.5 ? 1.0 : (a->rate > 2.0 ? 2.0 : a->rate);
    char *pl = g_build_filename(dir, "playlist.m3u", NULL);
    char *data = NULL;
    /* Launched with explicit files (file manager / command line): the user
     * asked for those, so the restored playlist must not be merged in. */
    gboolean launched_with_files = a->track_count > 0;
    if (!launched_with_files && g_file_get_contents(pl, &data, NULL, NULL)) {
        char **lines = g_strsplit(data, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            char *line = g_strstrip(lines[i]);
            if (!*line || *line == '#') continue;
            char abs[4096];
            if (line[0] == '/') snprintf(abs, sizeof(abs), "%s", line);
            else {
                char *d2 = g_path_get_dirname(pl);
                snprintf(abs, sizeof(abs), "%s/%s", d2, line);
                g_free(d2);
            }
            add_track(a, abs);
        }
        g_strfreev(lines);
        g_free(data);
    }
    g_free(pl);
    g_free(dir);
}

static gboolean persist_cb(gpointer data) {
    save_state((App *)data);
    return G_SOURCE_CONTINUE;
}

/* --------------------------------------------------------------- visuals -- */
static void draw_visualizer(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data) {
    App *a = data;
    if (w < 8 || h < 8) return;
    if (!a->visualizer_on || a->showing_video) return;
    cairo_set_source_rgb(cr, 0.027, 0.039, 0.070);
    cairo_paint(cr);
    nava_vis_draw(cr, w, h, &a->vis);
}

static void rounded_path(cairo_t *cr, double x, double y, double w, double h,
                         double r);

static void draw_cover(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data) {
    App *a = data;
    if (w < 8 || h < 8) return;
    if (a->showing_video) return;

    /* When visualizer is active, only draw if we have album art */
    if (a->visualizer_on && !a->cover_surface) return;

    double mix = a->art_anim;
    if (mix < 0.0) mix = 0.0;
    if (mix > 1.0) mix = 1.0;
    mix = mix * mix * (3.0 - 2.0 * mix);          /* smoothstep easing */

    double corner_size = 96.0;
    double centre_size = (w < h ? w : h) - 48.0;
    if (centre_size > 340.0) centre_size = 340.0;
    double size = corner_size + (centre_size - corner_size) * mix;
    if (size <= 4) return;

    double centre_x = (w - size) / 2.0;
    double centre_y = (h - size) / 2.0;
    double corner_x = 20.0;
    double corner_y = 20.0;
    double x = corner_x + (centre_x - corner_x) * mix;
    double y = corner_y + (centre_y - corner_y) * mix;
    double radius = size * (0.14 + 0.02 * (1.0 - mix));

    if (mix > 0.05) {
        double breathe = 0.5 + 0.5 * sin(a->cover_phase * 1.4);
        cairo_pattern_t *glow = cairo_pattern_create_radial(
            centre_x + size / 2.0, centre_y + size / 2.0, size * 0.30,
            centre_x + size / 2.0, centre_y + size / 2.0, size * 0.70);
        cairo_pattern_add_color_stop_rgba(glow, 0.0, 0.30, 0.75, 1.0,
            0.10 + 0.16 * a->cover_energy + 0.03 * breathe);
        cairo_pattern_add_color_stop_rgba(glow, 1.0, 0.45, 0.30, 1.0, 0.0);
        cairo_set_source(cr, glow);
        cairo_arc(cr, centre_x + size / 2.0, centre_y + size / 2.0, size * 0.70, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_pattern_destroy(glow);
    }

    cairo_save(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.50);
    rounded_path(cr, x + 2, y + 5, size, size, radius);
    cairo_fill(cr);
    cairo_restore(cr);

    cairo_save(cr);
    rounded_path(cr, x, y, size, size, radius);
    cairo_clip(cr);
    if (a->cover_surface) {
        int sw = cairo_image_surface_get_width(a->cover_surface);
        int sh = cairo_image_surface_get_height(a->cover_surface);
        if (sw > 0 && sh > 0) {
            double sc = size / sw > size / sh ? size / sw : size / sh;
            double dw = sw * sc, dh = sh * sc;
            cairo_translate(cr, x + (size - dw) / 2.0, y + (size - dh) / 2.0);
            cairo_scale(cr, sc, sc);
            cairo_set_source_surface(cr, a->cover_surface, 0, 0);
            cairo_paint(cr);
        }
    } else {
        cairo_pattern_t *g = cairo_pattern_create_linear(x, y, x + size, y + size);
        cairo_pattern_add_color_stop_rgb(g, 0.0, 0.10, 0.18, 0.34);
        cairo_pattern_add_color_stop_rgb(g, 0.55, 0.16, 0.28, 0.52);
        cairo_pattern_add_color_stop_rgb(g, 1.0, 0.08, 0.12, 0.24);
        cairo_set_source(cr, g);
        cairo_paint(cr);
        cairo_pattern_destroy(g);
        /* Vector musical note — no font dependency, always crisp */
        double nc_x = x + size * 0.5;
        double nc_y = y + size * 0.5;
        double nw = size * 0.28;
        double nh = size * 0.32;
        cairo_save(cr);
        cairo_translate(cr, nc_x - nw * 0.5, nc_y - nh * 0.5);
        cairo_set_source_rgba(cr, 0.70, 0.90, 1.0, 0.40);
        /* Left note head */
        cairo_save(cr);
        cairo_translate(cr, nw * 0.22, nh * 0.82);
        cairo_rotate(cr, -0.35);
        cairo_scale(cr, nw * 0.20, nh * 0.14);
        cairo_arc(cr, 0, 0, 1.0, 0, 2.0 * M_PI);
        cairo_fill(cr);
        cairo_restore(cr);
        /* Right note head */
        cairo_save(cr);
        cairo_translate(cr, nw * 0.78, nh * 0.68);
        cairo_rotate(cr, -0.35);
        cairo_scale(cr, nw * 0.20, nh * 0.14);
        cairo_arc(cr, 0, 0, 1.0, 0, 2.0 * M_PI);
        cairo_fill(cr);
        cairo_restore(cr);
        /* Stems */
        cairo_set_line_width(cr, nw * 0.08);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_move_to(cr, nw * 0.30, nh * 0.80);
        cairo_line_to(cr, nw * 0.30, nh * 0.16);
        cairo_move_to(cr, nw * 0.86, nh * 0.66);
        cairo_line_to(cr, nw * 0.86, nh * 0.02);
        cairo_stroke(cr);
        /* Connecting beam */
        cairo_set_line_width(cr, nh * 0.13);
        cairo_move_to(cr, nw * 0.28, nh * 0.20);
        cairo_line_to(cr, nw * 0.88, nh * 0.06);
        cairo_stroke(cr);
        cairo_restore(cr);
    }
    cairo_restore(cr);

    cairo_save(cr);
    cairo_pattern_t *rim = cairo_pattern_create_linear(x, y, x + size, y + size);
    cairo_pattern_add_color_stop_rgba(rim, 0.0, 0.42, 0.90, 1.0, 0.55);
    cairo_pattern_add_color_stop_rgba(rim, 1.0, 0.65, 0.50, 1.0, 0.45);
    cairo_set_source(cr, rim);
    cairo_set_line_width(cr, 1.0 + 0.4 * mix);
    rounded_path(cr, x + 0.7, y + 0.7, size - 1.4, size - 1.4, radius);
    cairo_stroke(cr);
    cairo_pattern_destroy(rim);
    cairo_restore(cr);
}

static void apply_stage_layout(App *a) {
    if (!a->stage) return;
    gboolean visual_on = a->visualizer_on && !a->showing_video;
    if (a->showing_video) {
        gtk_widget_set_visible(a->vis_draw, FALSE);
        gtk_widget_set_visible(a->stage_art, FALSE);
        gtk_widget_set_visible(a->picture, TRUE);
        gtk_widget_set_visible(a->stage_caption, FALSE);
        gtk_widget_add_css_class(a->stage, "video-mode");
        gtk_widget_remove_css_class(a->stage, "audio-mode");
        return;
    }
    gtk_widget_set_visible(a->picture, FALSE);
    gtk_widget_set_visible(a->vis_draw, visual_on);
    gtk_widget_set_visible(a->stage_art, TRUE);
    gtk_widget_set_visible(a->stage_caption, TRUE);
    /* The widget always covers the stage; draw_cover() interpolates the
     * album art between the top-right corner and the centred layout, so
     * toggling the visualizer glides instead of jumping. */
    gtk_widget_set_size_request(a->stage_art, -1, -1);
    gtk_widget_set_hexpand(a->stage_art, TRUE);
    gtk_widget_set_vexpand(a->stage_art, TRUE);
    gtk_widget_set_halign(a->stage_art, GTK_ALIGN_FILL);
    gtk_widget_set_valign(a->stage_art, GTK_ALIGN_FILL);
    gtk_widget_set_margin_top(a->stage_art, 0);
    gtk_widget_set_margin_end(a->stage_art, 0);
    a->art_target = visual_on ? 0.0 : 1.0;
    if (!a->art_init) { a->art_anim = a->art_target; a->art_init = TRUE; }
    gtk_widget_remove_css_class(a->stage, "video-mode");
    gtk_widget_add_css_class(a->stage, "audio-mode");
    gtk_widget_queue_draw(a->stage_art);
    gtk_widget_queue_draw(a->vis_draw);
}

static void rounded_path(cairo_t *cr, double x, double y, double w, double h,
                         double r) {
    double max = w < h ? w : h;
    if (r > max / 2.0) r = max / 2.0;
    if (r < 0) r = 0;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 1.5 * M_PI);
    cairo_close_path(cr);
}

static void apply_skin(App *a, int skin_id) {
    if (skin_id < 0 || skin_id >= SKIN_COUNT) skin_id = 0;
    a->skin_id = skin_id;
    static GtkCssProvider *skin_provider = NULL;
    if (skin_provider) {
        gtk_style_context_remove_provider_for_display(
            gdk_display_get_default(), GTK_STYLE_PROVIDER(skin_provider));
        g_clear_object(&skin_provider);
    }
    GtkCssProvider *provider = gtk_css_provider_new();
    char *css_path = g_build_filename(g_get_user_data_dir(), "nava-studio",
                                       SKIN_CSS_FILES[skin_id], NULL);
    if (g_file_test(css_path, G_FILE_TEST_EXISTS)) {
        /* User-supplied override wins over the bundled copy. */
        gtk_css_provider_load_from_path(provider, css_path);
    } else {
        char *res = g_strconcat("/com/shirazelectronic/NavaStudio/",
                                SKIN_CSS_FILES[skin_id], NULL);
        GBytes *data = g_resources_lookup_data(res, G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
        if (data) {
            gtk_css_provider_load_from_string(provider,
                g_bytes_get_data(data, NULL));
            g_bytes_unref(data);
        } else {
            gtk_css_provider_load_from_string(provider, "/* fallback */");
        }
        g_free(res);
    }
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_USER + 1);
    skin_provider = provider;
    g_free(css_path);
}


static void set_cover_pixels(App *a, uint8_t *px, int w, int h) {
    free(a->cover_pixels);
    a->cover_pixels = px;
    a->cover_w = w;
    a->cover_h = h;
    if (a->cover_surface) {
        cairo_surface_destroy(a->cover_surface);
        a->cover_surface = NULL;
    }
    if (px && w > 0 && h > 0) {
        int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, w);
        uint8_t *buf = calloc(1, (size_t)stride * h);
        if (buf) {
            for (int j = 0; j < h; j++)
                for (int i = 0; i < w; i++) {
                    const uint8_t *src = px + ((size_t)j * w + i) * 4;
                    uint8_t *dst = buf + (size_t)j * stride + i * 4;
                    uint8_t r = src[0], g = src[1], b = src[2], alpha = src[3];
                    /* cairo ARGB32 is premultiplied, little-endian. */
                    dst[0] = (uint8_t)((b * alpha + 127) / 255);
                    dst[1] = (uint8_t)((g * alpha + 127) / 255);
                    dst[2] = (uint8_t)((r * alpha + 127) / 255);
                    dst[3] = alpha;
                }
            cairo_surface_t *surf = cairo_image_surface_create_for_data(
                buf, CAIRO_FORMAT_ARGB32, w, h, stride);
            if (cairo_surface_status(surf) == CAIRO_STATUS_SUCCESS) {
                a->cover_surface = cairo_surface_create_similar_image(
                    surf, CAIRO_FORMAT_ARGB32, w, h);
                cairo_t *c = cairo_create(a->cover_surface);
                cairo_set_source_surface(c, surf, 0, 0);
                cairo_paint(c);
                cairo_destroy(c);
            }
            cairo_surface_destroy(surf);
            free(buf);
        }
    }
    gtk_widget_queue_draw(a->stage_art);
}

static void update_chips(App *a) {
    if (a->current < 0 || a->current >= a->track_count) {
        gtk_label_set_text(GTK_LABEL(a->format_chip), "—");
        gtk_label_set_text(GTK_LABEL(a->quality_chip), "");
        gtk_widget_set_visible(a->quality_chip, FALSE);
        return;
    }
    const char *path = a->tracks[a->current].path;
    const char *dot = strrchr(path, '.');
    char suffix[16] = "MEDIA";
    if (dot && dot[1]) {
        size_t n = strlen(dot + 1);
        if (n >= sizeof(suffix)) n = sizeof(suffix) - 1;
        for (size_t i = 0; i < n; i++)
            suffix[i] = (char)g_ascii_toupper(dot[1 + i]);
        suffix[n] = '\0';
    }
    char buf[128];
    int has_video = engine_has_video(a->engine);
    if (has_video && engine_width(a->engine) && engine_height(a->engine))
        snprintf(buf, sizeof(buf), "%s  ·  %d×%d", suffix,
                 engine_width(a->engine), engine_height(a->engine));
    else
        snprintf(buf, sizeof(buf), "%s  ·  AUDIO", suffix);
    gtk_label_set_text(GTK_LABEL(a->format_chip), buf);

    char details[64] = "";
    if (has_video) g_strlcat(details, "Video", sizeof(details));
    if (engine_has_audio(a->engine)) {
        if (details[0]) g_strlcat(details, "  ·  ", sizeof(details));
        g_strlcat(details, "Audio", sizeof(details));
    }
    gtk_label_set_text(GTK_LABEL(a->quality_chip), details[0] ? details : "");
    gtk_widget_set_visible(a->quality_chip, details[0] ? TRUE : FALSE);
}

/* ---------------------------------------------------------------- player -- */
static void set_play_icon(App *a, gboolean playing) {
    gtk_button_set_icon_name(GTK_BUTTON(a->play_button),
                             playing ? "media-playback-pause-symbolic"
                                     : "media-playback-start-symbolic");
    if (playing) gtk_widget_add_css_class(a->play_button, "playing");
    else gtk_widget_remove_css_class(a->play_button, "playing");
}

static void update_repeat_button(App *a) {
    const char *icon = "media-playlist-repeat-symbolic";
    const char *tip = "Repeat: off → all → one";
    if (a->repeat == REPEAT_ONE) icon = "media-playlist-repeat-song-symbolic";
    else if (a->repeat == REPEAT_ALL) icon = "media-playlist-repeat-symbolic";
    gtk_button_set_icon_name(GTK_BUTTON(a->repeat_button), icon);
    gtk_widget_set_tooltip_text(a->repeat_button, tip);
    if (a->repeat != REPEAT_OFF) {
        gtk_widget_add_css_class(a->repeat_button, "active-toggle");
        gtk_widget_remove_css_class(a->repeat_button, "inactive-toggle");
    } else {
        gtk_widget_remove_css_class(a->repeat_button, "active-toggle");
        gtk_widget_add_css_class(a->repeat_button, "inactive-toggle");
    }
    if (a->repeat == REPEAT_ONE) tip = "Repeat: one (click for off)";
    else if (a->repeat == REPEAT_ALL) tip = "Repeat: all (click for one)";
    else tip = "Repeat: off (click for all)";
    gtk_widget_set_tooltip_text(a->repeat_button, tip);
    if (a->repeat_label) {
        gtk_label_set_text(GTK_LABEL(a->repeat_label),
                           a->repeat == REPEAT_ONE ? "One" :
                           a->repeat == REPEAT_ALL ? "All" : "Off");
        if (a->repeat != REPEAT_OFF) gtk_widget_add_css_class(a->repeat_label, "caption-on");
        else gtk_widget_remove_css_class(a->repeat_label, "caption-on");
    }
}

static void apply_engine_settings(App *a) {
    if (!a->engine) return;
    engine_set_volume(a->engine, (float)a->volume, a->muted);
    engine_set_eq(a->engine, a->eq_gains, EQ_BANDS, a->eq_enabled);
    engine_set_echo(a->engine,
                    (float)gtk_range_get_value(GTK_RANGE(a->echo_scale)),
                    (float)gtk_range_get_value(GTK_RANGE(a->fb_scale)),
                    (float)gtk_range_get_value(GTK_RANGE(a->delay_scale)));
    engine_set_compressor(a->engine, (float)gtk_range_get_value(GTK_RANGE(a->comp_scale)));
    engine_set_reverb(a->engine, (float)gtk_range_get_value(GTK_RANGE(a->reverb_scale)));
    engine_set_flanger(a->engine, (float)gtk_range_get_value(GTK_RANGE(a->flanger_scale)));
    engine_set_rate(a->engine, (float)a->rate);
    engine_set_fades(a->engine, a->crossfade_on, a->crossfade_ms, a->fade_ms);
    engine_set_rotation(a->engine, a->rotation);
    engine_set_video_effects(a->engine,
        (float)gtk_range_get_value(GTK_RANGE(a->bright_scale)),
        (float)gtk_range_get_value(GTK_RANGE(a->contrast_scale)),
        (float)gtk_range_get_value(GTK_RANGE(a->satur_scale)),
        (float)gtk_range_get_value(GTK_RANGE(a->gamma_scale)),
        (float)gtk_range_get_value(GTK_RANGE(a->hue_scale)));
}

static void play_index(App *a, int index) {
    if (index < 0 || index >= a->track_count) return;
    a->vclock_wall = 0;
    char err[256] = {0};
    int rc = engine_open(a->engine, a->tracks[index].path, err, sizeof(err));
    if (rc < 0) {
        set_status(a, err, TRUE);
        return;
    }
    a->current = index;
    g_strlcpy(a->last_file, a->tracks[index].path, sizeof(a->last_file));
    a->at_end = FALSE;
    a->xf_armed = FALSE;
    a->pending_seek = FALSE;
    a->showing_video = engine_has_video(a->engine) != 0;
    set_cover_pixels(a, NULL, 0, 0);
    uint8_t *px = NULL; int cw = 0, ch = 0;
    if (engine_cover(a->engine, &px, &cw, &ch)) set_cover_pixels(a, px, cw, ch);
    gtk_label_set_text(GTK_LABEL(a->now_title), a->tracks[index].title);
    gtk_label_set_text(GTK_LABEL(a->now_sub), a->tracks[index].meta);
    gtk_label_set_text(GTK_LABEL(a->stage_title), a->tracks[index].title);
    char *dir = g_path_get_dirname(a->tracks[index].path);
    gtk_label_set_text(GTK_LABEL(a->stage_sub), base_name(dir));
    g_free(dir);
    double dur = engine_duration(a->engine);
    a->updating_seek = TRUE;
    gtk_range_set_range(GTK_RANGE(a->seek), 0, dur > 0 ? dur : 1);
    gtk_range_set_value(GTK_RANGE(a->seek), 0);
    a->updating_seek = FALSE;
    apply_engine_settings(a);
    update_chips(a);
    apply_stage_layout(a);
    engine_play(a->engine);
    set_play_icon(a, TRUE);
    refresh_list(a);
    set_status(a, "Playing", FALSE);
    mpris_update(a);
}

static void on_list_pressed(GtkGestureClick *g, int n, double x, double y, gpointer data) {
    App *a = data;
    if (n != 2) return;
    GtkWidget *row = GTK_WIDGET(gtk_widget_pick(a->track_list, x, y, GTK_PICK_DEFAULT));
    while (row && !GTK_IS_LIST_BOX_ROW(row)) row = gtk_widget_get_parent(row);
    if (!row) return;
    int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "index"));
    if (index < 0 || index >= a->track_count) return;
    play_index(a, index);
    /* play_index() calls refresh_list(), so the row under the pointer is a
     * new widget now; look it up again and pin the selection to it. */
    GtkWidget *fresh = NULL;
    for (GtkWidget *c = gtk_widget_get_first_child(a->track_list); c; c = gtk_widget_get_next_sibling(c)) {
        if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(c), "index")) == index) { fresh = c; break; }
    }
    if (fresh)
        gtk_list_box_select_row(GTK_LIST_BOX(a->track_list), GTK_LIST_BOX_ROW(fresh));
}

static void on_play(GtkButton *b, gpointer data) {
    App *a = data;
    if (a->at_end) {
        /* Playlist finished: restart it from the beginning. */
        a->at_end = FALSE;
        if (a->track_count) { play_index(a, 0); return; }
    }
    if (!engine_active(a->engine)) {
        /* Prefer the track the user last had loaded, not always track 0. */
        int idx = a->current;
        if (idx < 0 && a->last_file[0]) {
            for (int i = 0; i < a->track_count; i++)
                if (strcmp(a->tracks[i].path, a->last_file) == 0) { idx = i; break; }
            if (idx < 0) idx = add_track(a, a->last_file);
            if (idx >= 0 && idx < a->track_count) refresh_list(a);
        }
        if (idx < 0 && a->track_count) idx = 0;
        if (idx >= 0) play_index(a, idx);
        return;
    }
    if (engine_playing(a->engine)) {
        engine_pause_faded(a->engine);
        set_play_icon(a, FALSE);
        set_status(a, "Pausing…", FALSE);
        mpris_update(a);
    } else {
        /* Resume without reopening the file: engine_play() unparks a paused
         * decode thread and starts a fresh one after Stop, keeping position. */
        engine_resume_faded(a->engine);
        set_play_icon(a, TRUE);
        set_status(a, "Playing", FALSE);
        mpris_update(a);
    }
}

static int next_index(App *a, gboolean auto_advance) {
    if (a->track_count == 0) return -1;
    if (a->shuffle) {
        if (a->track_count == 1) return 0;
        int idx = a->current;
        while (idx == a->current) idx = g_random_int_range(0, a->track_count);
        return idx;
    }
    if (a->repeat == REPEAT_ONE && auto_advance) return a->current;
    int n = a->current + 1;
    if (n >= a->track_count) {
        if (a->repeat == REPEAT_ALL) return 0;
        return -1;
    }
    return n;
}

static void on_stop(GtkButton *b, gpointer data) {
    (void)b;
    App *a = data;
    engine_stop_faded(a->engine);
    set_play_icon(a, FALSE);
    set_status(a, "Stopped", FALSE);
    mpris_update(a);
}
static void on_next(GtkButton *b, gpointer data) {
    App *a = data;
    int idx = next_index(a, FALSE);
    if (idx < 0 && a->repeat == REPEAT_ALL) idx = 0;
    if (idx >= 0) switch_track(a, idx);
}
static void on_prev(GtkButton *b, gpointer data) {
    App *a = data;
    if (a->track_count == 0) return;
    int idx = a->current - 1;
    if (idx < 0) idx = a->repeat == REPEAT_ALL ? a->track_count - 1 : 0;
    switch_track(a, idx);
}
static void on_xf_toggled(GtkCheckButton *b, gpointer data) {
    App *a = data;
    a->crossfade_on = gtk_check_button_get_active(b) != FALSE;
    apply_engine_settings(a);
}
static void on_xf_changed(GtkRange *r, gpointer data) {
    App *a = data;
    a->crossfade_ms = gtk_range_get_value(r);
    if (a->xf_value) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1fs", a->crossfade_ms / 1000.0);
        gtk_label_set_text(GTK_LABEL(a->xf_value), buf);
    }
    apply_engine_settings(a);
}
static void on_fade_changed(GtkRange *r, gpointer data) {
    App *a = data;
    a->fade_ms = gtk_range_get_value(r);
    if (a->fade_value) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%dms", (int)(a->fade_ms + 0.5));
        gtk_label_set_text(GTK_LABEL(a->fade_value), buf);
    }
    apply_engine_settings(a);
}

/* Refresh the "now playing" chrome for `index`.  Used both by a fresh open
 * and by a crossfade, where the engine keeps running while the labels move. */
static void apply_track_ui(App *a, int index) {
    if (index < 0 || index >= a->track_count) return;
    a->current = index;
    g_strlcpy(a->last_file, a->tracks[index].path, sizeof(a->last_file));
    a->at_end = FALSE;
    gtk_label_set_text(GTK_LABEL(a->now_title), a->tracks[index].title);
    gtk_label_set_text(GTK_LABEL(a->now_sub), a->tracks[index].meta);
    gtk_label_set_text(GTK_LABEL(a->stage_title), a->tracks[index].title);
    char *dir = g_path_get_dirname(a->tracks[index].path);
    gtk_label_set_text(GTK_LABEL(a->stage_sub), base_name(dir));
    g_free(dir);
    update_chips(a);
    apply_stage_layout(a);
    refresh_list(a);
}

/* Start `index`, blending it into the current track when crossfade is on. */
static void switch_track(App *a, int index) {
    if (index < 0 || index >= a->track_count) return;
    if (a->crossfade_on && a->crossfade_ms >= 100.0 &&
        engine_active(a->engine) && engine_playing(a->engine) &&
        a->current >= 0 && a->current != index &&
        !a->tracks[index].is_video) {
        char err[128] = {0};
        if (engine_crossfade_to(a->engine, a->tracks[index].path, err, sizeof(err)) == 0) {
            a->xf_armed = TRUE;
            a->vclock_wall = 0;
            apply_track_ui(a, index);
            set_play_icon(a, TRUE);
            set_status(a, "Crossfading", FALSE);
            mpris_update(a);
            return;
        }
    }
    a->xf_armed = FALSE;
    play_index(a, index);
}

static void seek_by(App *a, double delta) {
    if (!engine_active(a->engine)) return;
    double p = engine_position(a->engine) + delta;
    if (p < 0) p = 0;
    engine_seek_faded(a->engine, p);
}
static void on_back10(GtkButton *b, gpointer data) { seek_by(data, -10); }
static void on_fwd10(GtkButton *b, gpointer data) { seek_by(data, 10); }

static gboolean end_seek_drag(gpointer data) {
    App *a = data;
    a->seek_idle = 0;
    if (!a->seek_dragging) return G_SOURCE_REMOVE;
    a->seek_dragging = FALSE;
    /* Commit the final position exactly once. */
    a->seek_timer = 0;
    if (engine_active(a->engine)) {
        a->vclock_wall = 0;
        a->xf_armed = FALSE;
        a->pending_seek = TRUE;
        a->pending_seek_ticks = 0;
        a->pending_seek_pos = a->seek_pending;
        /* Show the target immediately; the engine catches up after its
         * fade-out, and tick_cb must not snap the handle back meanwhile. */
        char buf[32];
        format_time(a->seek_pending, buf, sizeof(buf));
        gtk_label_set_text(GTK_LABEL(a->time_now), buf);
        engine_seek_faded(a->engine, a->seek_pending);
    }
    return G_SOURCE_REMOVE;
}
static void on_seek_changed(GtkRange *r, gpointer data) {
    App *a = data;
    if (a->updating_seek) return;
    double dur = engine_duration(a->engine);
    if (dur <= 0) return;
    a->seek_dragging = TRUE;
    if (a->seek_idle) g_source_remove(a->seek_idle);
    a->seek_idle = g_timeout_add(260, end_seek_drag, a);
    a->seek_pending = gtk_range_get_value(r);
    char buf[32];
    format_time(a->seek_pending, buf, sizeof(buf));
    gtk_label_set_text(GTK_LABEL(a->time_now), buf);
    /* Only seek once the drag settles: re-seeking every 120 ms while the
     * user is still moving the handle makes playback stutter badly. */
    if (a->seek_timer) g_source_remove(a->seek_timer);
    a->seek_timer = 0;
}

static void on_volume(GtkRange *r, gpointer data) {
    App *a = data;
    a->volume = gtk_range_get_value(r);
    if (a->engine) engine_set_volume(a->engine, (float)a->volume, a->muted);
    mpris_update(a);
}
static void on_mute(GtkToggleButton *b, gpointer data) {
    App *a = data;
    a->muted = gtk_toggle_button_get_active(b);
    gtk_button_set_icon_name(GTK_BUTTON(b),
        a->muted ? "audio-volume-muted-symbolic" : "audio-volume-high-symbolic");
    if (a->engine) engine_set_volume(a->engine, (float)a->volume, a->muted);
}
static void update_shuffle_ui(App *a) {
    if (!a->shuffle_button) return;
    if (a->shuffle) {
        gtk_widget_add_css_class(a->shuffle_button, "active-toggle");
        gtk_widget_remove_css_class(a->shuffle_button, "inactive-toggle");
    } else {
        gtk_widget_remove_css_class(a->shuffle_button, "active-toggle");
        gtk_widget_add_css_class(a->shuffle_button, "inactive-toggle");
    }
    if (a->shuffle_label) {
        gtk_label_set_text(GTK_LABEL(a->shuffle_label), a->shuffle ? "On" : "Off");
        if (a->shuffle) gtk_widget_add_css_class(a->shuffle_label, "caption-on");
        else gtk_widget_remove_css_class(a->shuffle_label, "caption-on");
    }
}
static void on_shuffle(GtkToggleButton *b, gpointer data) {
    App *a = data;
    a->shuffle = gtk_toggle_button_get_active(b);
    update_shuffle_ui(a);
    mpris_update(a);
}
static void set_drawer(App *a, gboolean open) {
    if (!a->drawer) return;
    a->sidebar_on = open;
    if (!open) a->drawer_edge_open = FALSE;
    gtk_widget_set_can_target(a->drawer, open);
    gtk_revealer_set_reveal_child(GTK_REVEALER(a->drawer), open);
    if (a->sidebar_btn) {
        a->drawer_syncing = TRUE;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->sidebar_btn), open);
        a->drawer_syncing = FALSE;
    }
}
static gboolean hide_drawer_cb(gpointer data) {
    App *a = data;
    a->drawer_timer = 0;
    /* Keep the drawer alive while a context menu opened from it is up. */
    if (a->menu_popover) return G_SOURCE_REMOVE;
    set_drawer(a, FALSE);
    return G_SOURCE_REMOVE;
}
static void drawer_enter_cb(GtkEventControllerMotion *c, double x, double y, gpointer data) {
    (void)c; (void)x; (void)y;
    App *a = data;
    if (a->drawer_timer) { g_source_remove(a->drawer_timer); a->drawer_timer = 0; }
}
static void drawer_leave_cb(GtkEventControllerMotion *c, gpointer data) {
    (void)c;
    App *a = data;
    if (!a->sidebar_on || a->drawer_timer) return;
    a->drawer_timer = g_timeout_add(220, hide_drawer_cb, a);
}
static void on_sidebar_toggle(GtkToggleButton *b, gpointer data) {
    App *a = data;
    if (a->drawer_syncing) return;
    set_drawer(a, gtk_toggle_button_get_active(b));
}
static void on_close_settings(GtkButton *b, gpointer data) {
    (void)b;
    App *a = data;
    a->show_effects = FALSE;
    if (a->effects_btn)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->effects_btn), FALSE);
    if (a->deck) gtk_widget_set_visible(a->deck, FALSE);
}
static void on_effects_toggle(GtkToggleButton *b, gpointer data) {
    App *a = data;
    a->show_effects = gtk_toggle_button_get_active(b);
    if (a->deck)
        gtk_widget_set_visible(a->deck, a->show_effects && !a->mini);
}

/* Mini player: controls and the track title only, no playlist, no deck and
 * no stage.  Everything else keeps its own visibility flags so leaving mini
 * mode restores exactly the previous layout. */
static void set_mini(App *a, gboolean on) {
    if (!a->window) return;
    a->mini = on;
    if (on && a->fullscreen) set_fullscreen(a, FALSE);
    if (on) set_drawer(a, FALSE);
    gtk_widget_set_visible(a->deck, on ? FALSE : (a->show_effects && !a->fullscreen));
    if (a->stage) gtk_widget_set_visible(a->stage, on ? FALSE : TRUE);
    if (a->status_bar) gtk_widget_set_visible(a->status_bar, on ? FALSE : TRUE);
    if (a->content) {
        gtk_widget_set_margin_top(a->content, on ? 6 : 13);
        gtk_widget_set_margin_bottom(a->content, on ? 6 : 7);
        gtk_widget_set_margin_start(a->content, on ? 8 : 16);
        gtk_widget_set_margin_end(a->content, on ? 8 : 16);
    }
    /* GTK4 ignores default-size changes once the window is mapped; a size
     * request is the portable way to shrink and restore it live. */
    gtk_widget_set_size_request(a->window, on ? 430 : -1, on ? 170 : -1);
    if (on) gtk_window_set_default_size(GTK_WINDOW(a->window), 430, 170);
    else gtk_window_set_default_size(GTK_WINDOW(a->window), 1360, 880);
    if (a->mini_btn) {
        g_signal_handlers_block_by_func(a->mini_btn, on_mini_toggle, a);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->mini_btn), on);
        g_signal_handlers_unblock_by_func(a->mini_btn, on_mini_toggle, a);
    }
}
static void on_mini_toggle(GtkToggleButton *b, gpointer data) {
    set_mini((App *)data, gtk_toggle_button_get_active(b));
}
static void on_repeat(GtkButton *b, gpointer data) {
    App *a = data;
    a->repeat = (a->repeat + 1) % 3;
    update_repeat_button(a);
    mpris_update(a);
}
static void on_speed(GtkDropDown *d, GParamSpec *ps, gpointer data) {
    App *a = data;
    guint sel = gtk_drop_down_get_selected(d);
    if (sel < G_N_ELEMENTS(SPEEDS)) {
        a->rate = SPEEDS[sel];
        if (a->engine) engine_set_rate(a->engine, (float)a->rate);
    }
    mpris_update(a);
}


/* ----------------------------------------------------------------- MPRIS2 -- */
#if defined(_WIN32) || defined(__APPLE__)
static void mpris_update(App *a) { (void)a; }
static void mpris_start(App *a) { (void)a; }
static void mpris_stop(App *a) { (void)a; }
#else
/* Hardware media keys never arrive as key events on Wayland/GNOME: the shell
 * grabs them and forwards them over MPRIS2 on D-Bus.  Exporting that interface
 * is what actually makes play/pause/next/previous work from the keyboard. */
#define MPRIS_NAME         "org.mpris.MediaPlayer2.nava"
#define MPRIS_PATH         "/org/mpris/MediaPlayer2"
#define MPRIS_ROOT_IFACE   "org.mpris.MediaPlayer2"
#define MPRIS_PLAYER_IFACE "org.mpris.MediaPlayer2.Player"

static const char *MPRIS_XML =
    "<node>"
    " <interface name='org.mpris.MediaPlayer2'>"
    "  <method name='Raise'/>"
    "  <method name='Quit'/>"
    "  <property name='CanQuit' type='b' access='read'/>"
    "  <property name='CanRaise' type='b' access='read'/>"
    "  <property name='HasTrackList' type='b' access='read'/>"
    "  <property name='Identity' type='s' access='read'/>"
    "  <property name='DesktopEntry' type='s' access='read'/>"
    "  <property name='SupportedUriSchemes' type='as' access='read'/>"
    "  <property name='SupportedMimeTypes' type='as' access='read'/>"
    " </interface>"
    " <interface name='org.mpris.MediaPlayer2.Player'>"
    "  <method name='Next'/>"
    "  <method name='Previous'/>"
    "  <method name='Pause'/>"
    "  <method name='PlayPause'/>"
    "  <method name='Stop'/>"
    "  <method name='Play'/>"
    "  <method name='Seek'><arg name='Offset' type='x' direction='in'/></method>"
    "  <method name='SetPosition'>"
    "   <arg name='TrackId' type='o' direction='in'/>"
    "   <arg name='Position' type='x' direction='in'/>"
    "  </method>"
    "  <method name='OpenUri'><arg name='Uri' type='s' direction='in'/></method>"
    "  <property name='PlaybackStatus' type='s' access='read'/>"
    "  <property name='LoopStatus' type='s' access='readwrite'/>"
    "  <property name='Rate' type='d' access='readwrite'/>"
    "  <property name='Shuffle' type='b' access='readwrite'/>"
    "  <property name='Metadata' type='a{sv}' access='read'/>"
    "  <property name='Volume' type='d' access='readwrite'/>"
    "  <property name='Position' type='x' access='read'/>"
    "  <property name='MinimumRate' type='d' access='read'/>"
    "  <property name='MaximumRate' type='d' access='read'/>"
    "  <property name='CanGoNext' type='b' access='read'/>"
    "  <property name='CanGoPrevious' type='b' access='read'/>"
    "  <property name='CanPlay' type='b' access='read'/>"
    "  <property name='CanPause' type='b' access='read'/>"
    "  <property name='CanSeek' type='b' access='read'/>"
    "  <property name='CanControl' type='b' access='read'/>"
    " </interface>"
    "</node>";

static GDBusNodeInfo *mpris_node = NULL;
static GDBusInterfaceInfo *mpris_root_iface = NULL;
static GDBusInterfaceInfo *mpris_player_iface = NULL;

static const char *mpris_status(App *a) {
    if (!a->engine || !engine_active(a->engine)) return "Stopped";
    return engine_playing(a->engine) ? "Playing" : "Paused";
}
static const char *mpris_loop(App *a) {
    if (a->repeat == REPEAT_ONE) return "Track";
    if (a->repeat == REPEAT_ALL) return "Playlist";
    return "None";
}
static char *mpris_track_id(App *a) {
    if (a->current < 0 || a->current >= a->track_count)
        return g_strdup("/org/mpris/MediaPlayer2/TrackList/NoTrack");
    return g_strdup_printf("/com/shirazelectronic/NavaStudio/track/%d", a->current);
}
static GVariant *mpris_metadata(App *a) {
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    char *tid = mpris_track_id(a);
    g_variant_builder_add(&b, "{sv}", "mpris:trackid", g_variant_new_object_path(tid));
    g_free(tid);
    if (a->current >= 0 && a->current < a->track_count) {
        char *uri = g_filename_to_uri(a->tracks[a->current].path, NULL, NULL);
        if (uri) {
            g_variant_builder_add(&b, "{sv}", "xesam:url", g_variant_new_string(uri));
            g_free(uri);
        }
        g_variant_builder_add(&b, "{sv}", "xesam:title",
                              g_variant_new_string(a->tracks[a->current].title));
        double len = engine_duration(a->engine);
        if (len > 0)
            g_variant_builder_add(&b, "{sv}", "mpris:length",
                                  g_variant_new_int64((gint64)(len * 1000000.0)));
    }
    return g_variant_builder_end(&b);
}

static GVariant *mpris_prop(App *a, const char *iface, const char *prop) {
    if (!g_strcmp0(iface, MPRIS_ROOT_IFACE)) {
        if (!g_strcmp0(prop, "CanQuit") || !g_strcmp0(prop, "CanRaise"))
            return g_variant_new_boolean(TRUE);
        if (!g_strcmp0(prop, "HasTrackList")) return g_variant_new_boolean(FALSE);
        if (!g_strcmp0(prop, "Identity")) return g_variant_new_string("Nava Studio");
        if (!g_strcmp0(prop, "DesktopEntry")) return g_variant_new_string("nava-studio");
        if (!g_strcmp0(prop, "SupportedUriSchemes") ||
            !g_strcmp0(prop, "SupportedMimeTypes")) {
            const gchar *none[] = {NULL};
            return g_variant_new_strv(none, 0);
        }
        return NULL;
    }
    if (g_strcmp0(iface, MPRIS_PLAYER_IFACE)) return NULL;
    if (!g_strcmp0(prop, "PlaybackStatus")) return g_variant_new_string(mpris_status(a));
    if (!g_strcmp0(prop, "LoopStatus")) return g_variant_new_string(mpris_loop(a));
    if (!g_strcmp0(prop, "Rate")) return g_variant_new_double(a->rate);
    if (!g_strcmp0(prop, "Shuffle")) return g_variant_new_boolean(a->shuffle);
    if (!g_strcmp0(prop, "Metadata")) return mpris_metadata(a);
    if (!g_strcmp0(prop, "Volume"))
        return g_variant_new_double(a->muted ? 0.0 : a->volume);
    if (!g_strcmp0(prop, "Position")) {
        double p = (a->engine && engine_active(a->engine)) ? engine_position(a->engine) : 0.0;
        return g_variant_new_int64((gint64)(p * 1000000.0));
    }
    if (!g_strcmp0(prop, "MinimumRate")) return g_variant_new_double(0.5);
    if (!g_strcmp0(prop, "MaximumRate")) return g_variant_new_double(2.0);
    if (!g_strcmp0(prop, "CanGoNext") || !g_strcmp0(prop, "CanGoPrevious"))
        return g_variant_new_boolean(a->track_count > 0);
    if (!g_strcmp0(prop, "CanPlay") || !g_strcmp0(prop, "CanPause"))
        return g_variant_new_boolean(a->track_count > 0);
    if (!g_strcmp0(prop, "CanSeek"))
        return g_variant_new_boolean(a->engine && engine_active(a->engine));
    if (!g_strcmp0(prop, "CanControl")) return g_variant_new_boolean(TRUE);
    return NULL;
}

static void mpris_method_call(GDBusConnection *conn, const gchar *sender,
                              const gchar *path, const gchar *iface,
                              const gchar *method, GVariant *params,
                              GDBusMethodInvocation *inv, gpointer data) {
    (void)conn; (void)sender; (void)path;
    App *a = data;
    if (!g_strcmp0(iface, MPRIS_ROOT_IFACE)) {
        if (!g_strcmp0(method, "Raise")) {
            if (a->window) gtk_window_present(GTK_WINDOW(a->window));
        } else if (!g_strcmp0(method, "Quit")) {
            g_application_quit(g_application_get_default());
        }
        g_dbus_method_invocation_return_value(inv, NULL);
        return;
    }
    if (!g_strcmp0(method, "Play")) {
        if (a->track_count > 0 && !engine_playing(a->engine)) on_play(NULL, a);
    } else if (!g_strcmp0(method, "Pause")) {
        if (engine_playing(a->engine)) on_play(NULL, a);
    } else if (!g_strcmp0(method, "PlayPause") || !g_strcmp0(method, "Toggle")) {
        on_play(NULL, a);
    } else if (!g_strcmp0(method, "Stop")) {
        on_stop(NULL, a);
    } else if (!g_strcmp0(method, "Next")) {
        on_next(NULL, a);
    } else if (!g_strcmp0(method, "Previous")) {
        on_prev(NULL, a);
    } else if (!g_strcmp0(method, "Seek")) {
        gint64 off = 0;
        g_variant_get(params, "(x)", &off);
        seek_by(a, (double)off / 1000000.0);
    } else if (!g_strcmp0(method, "SetPosition")) {
        const gchar *tid = NULL;
        gint64 pos = 0;
        g_variant_get(params, "(&ox)", &tid, &pos);
        (void)tid;
        if (a->engine && engine_active(a->engine))
            engine_seek_faded(a->engine, (double)pos / 1000000.0);
    } else if (!g_strcmp0(method, "OpenUri")) {
        const gchar *uri = NULL;
        g_variant_get(params, "(&s)", &uri);
        gchar *file = g_filename_from_uri(uri, NULL, NULL);
        if (file) {
            int idx = add_track(a, file);
            if (idx >= 0) { play_index(a, idx); refresh_list(a); }
            g_free(file);
        }
    }
    g_dbus_method_invocation_return_value(inv, NULL);
}

static GVariant *mpris_get_property(GDBusConnection *c, const gchar *sender,
                                    const gchar *path, const gchar *iface,
                                    const gchar *prop, GError **error,
                                    gpointer data) {
    (void)c; (void)sender; (void)path; (void)error;
    return mpris_prop((App *)data, iface, prop);
}

static gboolean mpris_set_property(GDBusConnection *c, const gchar *sender,
                                   const gchar *path, const gchar *iface,
                                   const gchar *prop, GVariant *value,
                                   GError **error, gpointer data) {
    (void)c; (void)sender; (void)path; (void)iface; (void)error;
    App *a = data;
    if (!g_strcmp0(prop, "Volume")) {
        double v = g_variant_get_double(value);
        a->volume = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
        a->muted = a->volume <= 0.001;
        if (a->engine) engine_set_volume(a->engine, (float)a->volume, a->muted);
        if (a->vol_scale) gtk_range_set_value(GTK_RANGE(a->vol_scale), a->volume);
        if (a->mute_button)
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->mute_button), a->muted);
    } else if (!g_strcmp0(prop, "Rate")) {
        double r = g_variant_get_double(value);
        a->rate = r < 0.5 ? 0.5 : (r > 2.0 ? 2.0 : r);
        if (a->engine) engine_set_rate(a->engine, (float)a->rate);
    } else if (!g_strcmp0(prop, "LoopStatus")) {
        const char *v = g_variant_get_string(value, NULL);
        a->repeat = !g_strcmp0(v, "Track") ? REPEAT_ONE :
                    !g_strcmp0(v, "Playlist") ? REPEAT_ALL : REPEAT_OFF;
        update_repeat_button(a);
    } else if (!g_strcmp0(prop, "Shuffle")) {
        a->shuffle = g_variant_get_boolean(value);
        update_shuffle_ui(a);
        if (a->shuffle_button)
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->shuffle_button), a->shuffle);
    } else {
        return FALSE;
    }
    mpris_update(a);
    return TRUE;
}

static const GDBusInterfaceVTable mpris_vtable = {
    mpris_method_call, mpris_get_property, mpris_set_property, {0}
};

static void mpris_update(App *a) {
    if (!a || !a->mpris_bus) return;
    GVariantBuilder changed;
    g_variant_builder_init(&changed, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&changed, "{sv}", "PlaybackStatus",
                          g_variant_new_string(mpris_status(a)));
    g_variant_builder_add(&changed, "{sv}", "Metadata", mpris_metadata(a));
    g_variant_builder_add(&changed, "{sv}", "LoopStatus",
                          g_variant_new_string(mpris_loop(a)));
    g_variant_builder_add(&changed, "{sv}", "Shuffle", g_variant_new_boolean(a->shuffle));
    g_variant_builder_add(&changed, "{sv}", "Rate", g_variant_new_double(a->rate));
    g_variant_builder_add(&changed, "{sv}", "Volume",
                          g_variant_new_double(a->muted ? 0.0 : a->volume));
    /* "(sa{sv}as)" consumes the *builder* for the a{sv} element, not the
     * GVariant it produces; passing g_variant_builder_end() here aborts the
     * process with "expected array GVariantBuilder". */
    g_dbus_connection_emit_signal(a->mpris_bus, NULL, MPRIS_PATH,
        "org.freedesktop.DBus.Properties", "PropertiesChanged",
        g_variant_new("(sa{sv}as)", MPRIS_PLAYER_IFACE, &changed, NULL), NULL);
}

static void mpris_bus_acquired(GDBusConnection *conn, const gchar *name, gpointer data) {
    (void)name;
    App *a = data;
    GError *err = NULL;
    a->mpris_bus = g_object_ref(conn);
    a->mpris_reg_root = g_dbus_connection_register_object(conn, MPRIS_PATH,
        mpris_root_iface, &mpris_vtable, a, NULL, &err);
    if (err) { g_printerr("mpris: %s\n", err->message); g_clear_error(&err); }
    a->mpris_reg_player = g_dbus_connection_register_object(conn, MPRIS_PATH,
        mpris_player_iface, &mpris_vtable, a, NULL, &err);
    if (err) { g_printerr("mpris: %s\n", err->message); g_clear_error(&err); }
    mpris_update(a);
}

static void mpris_start(App *a) {
    if (a->mpris_owner || a->mpris_bus) return;
    GError *err = NULL;
    mpris_node = g_dbus_node_info_new_for_xml(MPRIS_XML, &err);
    if (!mpris_node) {
        g_printerr("mpris: %s\n", err ? err->message : "bad introspection data");
        g_clear_error(&err);
        return;
    }
    mpris_root_iface = g_dbus_node_info_lookup_interface(mpris_node, MPRIS_ROOT_IFACE);
    mpris_player_iface = g_dbus_node_info_lookup_interface(mpris_node, MPRIS_PLAYER_IFACE);
    a->mpris_owner = g_bus_own_name(G_BUS_TYPE_SESSION, MPRIS_NAME,
        G_BUS_NAME_OWNER_FLAGS_NONE, mpris_bus_acquired, NULL, NULL, a, NULL);
}

static void mpris_stop(App *a) {
    if (a->mpris_owner) { g_bus_unown_name(a->mpris_owner); a->mpris_owner = 0; }
    if (a->mpris_bus) {
        if (a->mpris_reg_root)
            g_dbus_connection_unregister_object(a->mpris_bus, a->mpris_reg_root);
        if (a->mpris_reg_player)
            g_dbus_connection_unregister_object(a->mpris_bus, a->mpris_reg_player);
        g_clear_object(&a->mpris_bus);
    }
    a->mpris_reg_root = a->mpris_reg_player = 0;
    if (mpris_node) {
        g_dbus_node_info_unref(mpris_node);
        mpris_node = NULL;
        mpris_root_iface = NULL;
        mpris_player_iface = NULL;
    }
}
#endif

/* -------------------------------------------------------------- effects -- */
static void eq_gather(App *a) {
    for (int i = 0; i < EQ_BANDS; i++)
        a->eq_gains[i] = (float)gtk_range_get_value(GTK_RANGE(a->eq_scales[i]));
}
static void eq_refresh_labels(App *a) {
    for (int i = 0; i < EQ_BANDS; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%+.1f", a->eq_gains[i]);
        gtk_label_set_text(GTK_LABEL(a->eq_values[i]), buf);
    }
}
static void on_eq_band(GtkRange *r, gpointer data) {
    App *a = data;
    if (a->suppress_fx) return;
    eq_gather(a);
    eq_refresh_labels(a);
    if (a->engine) engine_set_eq(a->engine, a->eq_gains, EQ_BANDS, a->eq_enabled);
}
static void on_eq_toggled(GtkCheckButton *b, gpointer data) {
    App *a = data;
    a->eq_enabled = gtk_check_button_get_active(b);
    eq_gather(a);
    if (a->engine) engine_set_eq(a->engine, a->eq_gains, EQ_BANDS, a->eq_enabled);
}
static void apply_preset(App *a, int preset) {
    if (preset < 0 || preset >= 11) preset = 0;
    a->suppress_fx = TRUE;
    for (int i = 0; i < EQ_BANDS; i++) {
        a->eq_gains[i] = EQ_PRESETS[preset][i];
        gtk_range_set_value(GTK_RANGE(a->eq_scales[i]), a->eq_gains[i]);
    }
    a->suppress_fx = FALSE;
    eq_refresh_labels(a);
    if (a->engine) engine_set_eq(a->engine, a->eq_gains, EQ_BANDS, a->eq_enabled);
    if (a->eq_preset) gtk_drop_down_set_selected(GTK_DROP_DOWN(a->eq_preset), preset);
}
static void on_skin(GtkDropDown *d, GParamSpec *ps, gpointer data) {
    (void)ps;
    App *a = data;
    guint sel = gtk_drop_down_get_selected(d);
    apply_skin(a, (int)sel);
}
static void on_preset(GtkDropDown *d, GParamSpec *ps, gpointer data) {
    App *a = data;
    apply_preset(a, (int)gtk_drop_down_get_selected(d));
}
static void on_reset_eq(GtkButton *b, gpointer data) { apply_preset(data, 0); }

static void on_param_fx(GtkRange *r, gpointer data) {
    App *a = data;
    GtkWidget *readout = g_object_get_data(G_OBJECT(r), "readout");
    if (readout) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.2f", gtk_range_get_value(r));
        gtk_label_set_text(GTK_LABEL(readout), buf);
    }
    apply_engine_settings(a);
}
static void on_rotation(GtkDropDown *d, GParamSpec *ps, gpointer data) {
    App *a = data;
    a->rotation = (int)gtk_drop_down_get_selected(d);
    if (a->engine) engine_set_rotation(a->engine, a->rotation);
}
static void on_vis_changed(GtkDropDown *d, GParamSpec *ps, gpointer data) {
    App *a = data;
    a->vis.mode = (int)gtk_drop_down_get_selected(d);
    a->visualizer_on = a->vis.mode != NAVA_VIS_COUNT - 1;
    apply_stage_layout(a);
}
static void on_sensitivity(GtkRange *r, gpointer data) {
    App *a = data;
    a->vis_sensitivity = (int)lround((gtk_range_get_value(r) - 0.2) / 0.05);
    a->vis.sensitivity = (float)gtk_range_get_value(r);
}

/* -------------------------------------------------------------- dialogs -- */
static void on_open_finished(GObject *src, GAsyncResult *res, gpointer data) {
    App *a = data;
    GError *err = NULL;
    GListModel *files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(src), res, &err);
    if (!files) { if (err) g_error_free(err); return; }
    guint n = g_list_model_get_n_items(files);
    int first = a->track_count;
    for (guint i = 0; i < n; i++) {
        GFile *f = g_list_model_get_item(files, i);
        char *path = g_file_get_path(f);
        if (path) {
            add_track(a, path);
            char *dir = g_path_get_dirname(path);
            g_strlcpy(a->last_folder, dir, sizeof(a->last_folder));
            g_free(dir);
            g_free(path);
        }
        g_object_unref(f);
    }
    g_object_unref(files);
    refresh_list(a);
    if (first < a->track_count) play_index(a, first);
    char msg[128];
    snprintf(msg, sizeof(msg), "%d file(s) added", a->track_count - first);
    set_status(a, msg, FALSE);
}
static void on_open(GtkButton *b, gpointer data) {
    App *a = data;
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "Add media files");
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Media files");
    for (int i = 0; AUDIO_EXTS[i]; i++) {
        char pat[16]; snprintf(pat, sizeof(pat), "*.%s", AUDIO_EXTS[i]);
        gtk_file_filter_add_pattern(filter, pat);
    }
    for (int i = 0; VIDEO_EXTS[i]; i++) {
        char pat[16]; snprintf(pat, sizeof(pat), "*.%s", VIDEO_EXTS[i]);
        gtk_file_filter_add_pattern(filter, pat);
    }
    GListStore *store = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(store, filter);
    gtk_file_dialog_set_filters(d, G_LIST_MODEL(store));
    if (a->last_folder[0]) {
        GFile *folder = g_file_new_for_path(a->last_folder);
        gtk_file_dialog_set_initial_folder(d, folder);
        g_object_unref(folder);
    }
    gtk_file_dialog_open_multiple(d, GTK_WINDOW(a->window), NULL, on_open_finished, a);
    g_object_unref(store);
    g_object_unref(filter);
    g_object_unref(d);
}
static void on_folder_finished(GObject *src, GAsyncResult *res, gpointer data) {
    App *a = data;
    GError *err = NULL;
    GFile *f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, &err);
    if (!f) { if (err) g_error_free(err); return; }
    char *path = g_file_get_path(f);
    if (path) {
        int before = a->track_count;
        add_directory(a, path);
        g_strlcpy(a->last_folder, path, sizeof(a->last_folder));
        refresh_list(a);
        if (a->track_count > before) play_index(a, before);
        set_status(a, "Folder added", FALSE);
        g_free(path);
    }
    g_object_unref(f);
}
static void on_add_folder(GtkButton *b, gpointer data) {
    App *a = data;
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "Add folder");
    if (a->last_folder[0]) {
        GFile *folder = g_file_new_for_path(a->last_folder);
        gtk_file_dialog_set_initial_folder(d, folder);
        g_object_unref(folder);
    }
    gtk_file_dialog_select_folder(d, GTK_WINDOW(a->window), NULL, on_folder_finished, a);
    g_object_unref(d);
}
/* Drops every queued track and resets the now-playing surfaces.  Shared by
 * the Clear button and by "open with Nava Studio", which replaces (rather
 * than appends to) whatever playlist the session had restored. */
static void clear_playlist(App *a, gboolean stop_engine) {
    if (stop_engine) engine_stop(a->engine);
    a->track_count = 0;
    a->current = -1;
    a->at_end = FALSE;
    a->xf_armed = FALSE;
    set_cover_pixels(a, NULL, 0, 0);
    a->showing_video = FALSE;
    apply_stage_layout(a);
    gtk_label_set_text(GTK_LABEL(a->now_title), "Nothing to play");
    gtk_label_set_text(GTK_LABEL(a->now_sub), "Drop a file here or pick one from the playlist");
    gtk_label_set_text(GTK_LABEL(a->format_chip), "—");
    gtk_label_set_text(GTK_LABEL(a->stage_title), "");
    gtk_label_set_text(GTK_LABEL(a->stage_sub), "");
    /* A stale filter would hide the files we are about to add. */
    if (a->search) gtk_editable_set_text(GTK_EDITABLE(a->search), "");
    refresh_list(a);
}
static void on_clear(App *a) {
    clear_playlist(a, TRUE);
    set_status(a, "Playlist cleared", FALSE);
}
static void on_clear_clicked(GtkButton *b, gpointer data) { on_clear(data); }

#if GTK_CHECK_VERSION(4, 10, 0)
static void on_save_playlist_finished(GObject *src, GAsyncResult *res, gpointer data) {
    App *a = data;
    GError *err = NULL;
    GFile *f = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, &err);
    if (!f) { if (err) g_error_free(err); return; }
    char *path = g_file_get_path(f);
    if (path) {
        GString *s = g_string_new("#EXTM3U\n# Nava Studio playlist\n");
        for (int i = 0; i < a->track_count; i++)
            g_string_append_printf(s, "%s\n", a->tracks[i].path);
        if (g_file_set_contents(path, s->str, (gssize)s->len, NULL))
            set_status(a, "Playlist saved", FALSE);
        g_string_free(s, TRUE);
        g_free(path);
    }
    g_object_unref(f);
}
static void on_save_playlist(GtkButton *b, gpointer data) {
    App *a = data;
    (void)b;
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "Save playlist");
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "M3U playlist");
    gtk_file_filter_add_suffix(filter, "m3u");
    GListStore *store = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(store, filter);
    gtk_file_dialog_set_filters(d, G_LIST_MODEL(store));
    gtk_file_dialog_set_initial_name(d, "nava-playlist.m3u");
    gtk_file_dialog_save(d, GTK_WINDOW(a->window), NULL, on_save_playlist_finished, a);
    g_object_unref(store);
    g_object_unref(filter);
    g_object_unref(d);
}
#else
static void on_save_playlist(GtkButton *b, gpointer data) {
    (void)b; (void)data;
}
#endif
static void on_about(GtkButton *b, gpointer data) {
    App *a = data;
    (void)b;
    const char *authors[] = {"Shiraz Electronic (shirazelec)", NULL};
    GtkAboutDialog *d = GTK_ABOUT_DIALOG(gtk_about_dialog_new());
    gtk_about_dialog_set_program_name(d, "Nava Studio");
    gtk_about_dialog_set_version(d, VERSION);
    gtk_about_dialog_set_comments(d, "Native C / GTK 4 media player powered by FFmpeg\nDesigned and developed by Shiraz Electronic");
    gtk_about_dialog_set_website(d, "https://shirazelectronic.ir");
    gtk_about_dialog_set_authors(d, authors);
    gtk_about_dialog_set_license_type(d, GTK_LICENSE_GPL_3_0);
    gtk_window_set_transient_for(GTK_WINDOW(d), GTK_WINDOW(a->window));
    gtk_window_present(GTK_WINDOW(d));
}

/* ----------------------------------------------------------- drag & drop -- */
static gboolean on_drop(GtkDropTarget *t, const GValue *value, double x, double y, gpointer data) {
    App *a = data;
    gtk_widget_remove_css_class(a->stage, "drop-active");
    int before = a->track_count;
    if (G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) {
        GSList *files = gdk_file_list_get_files(g_value_get_object(value));
        for (GSList *it = files; it; it = it->next) {
            char *path = g_file_get_path(G_FILE(it->data));
            if (!path) continue;
            if (g_file_test(path, G_FILE_TEST_IS_DIR)) add_directory(a, path);
            else add_track(a, path);
            g_free(path);
        }
        g_slist_free(files);
    } else if (G_VALUE_HOLDS_STRING(value)) {
        const char *text = g_value_get_string(value);
        char **lines = g_strsplit(text ? text : "", "\n", -1);
        for (int i = 0; lines[i]; i++) {
            char *line = g_strstrip(lines[i]);
            if (!*line || *line == '#') continue;
            char *path = line;
            if (g_str_has_prefix(line, "file://")) {
                char *dec = g_uri_unescape_string(line + 7, NULL);
                if (dec) { add_track(a, dec); g_free(dec); }
                continue;
            }
            if (g_file_test(path, G_FILE_TEST_EXISTS)) add_track(a, path);
        }
        g_strfreev(lines);
    }
    refresh_list(a);
    if (a->track_count > before) play_index(a, before);
    if (a->track_count != before) set_status(a, "Dropped files added and playing", FALSE);
    return TRUE;
}
static GdkDragAction on_drag_enter(GtkDropTarget *t, double x, double y, gpointer data) {
    App *a = data;
    gtk_widget_add_css_class(a->stage, "drop-active");
    return GDK_ACTION_COPY;
}
static void on_drag_leave(GtkDropTarget *t, gpointer data) {
    App *a = data;
    gtk_widget_remove_css_class(a->stage, "drop-active");
}
static void install_dnd(App *a, GtkWidget *w) {
    GtkDropTarget *file = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect(file, "drop", G_CALLBACK(on_drop), a);
    g_signal_connect(file, "enter", G_CALLBACK(on_drag_enter), a);
    g_signal_connect(file, "leave", G_CALLBACK(on_drag_leave), a);
    gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(file));
    GtkDropTarget *text = gtk_drop_target_new(G_TYPE_STRING, GDK_ACTION_COPY);
    g_signal_connect(text, "drop", G_CALLBACK(on_drop), a);
    g_signal_connect(text, "enter", G_CALLBACK(on_drag_enter), a);
    g_signal_connect(text, "leave", G_CALLBACK(on_drag_leave), a);
    gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(text));
}

/* ---------------------------------------------------- playlist context menu */
static int ctx_index(App *a) {
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(a->menu_popover), "index"));
}
static void close_menu(App *a) {
    if (a->menu_popover) gtk_popover_popdown(GTK_POPOVER(a->menu_popover));
}
static void ctx_play(GtkButton *b, gpointer data) {
    App *a = data; int idx = ctx_index(a); close_menu(a); play_index(a, idx);
}
static void ctx_next(GtkButton *b, gpointer data) {
    App *a = data; int idx = ctx_index(a); close_menu(a);
    if (idx < 0 || idx >= a->track_count) return;
    int last = a->track_count - 1;
    int target = a->current < 0 ? last : MIN(last, a->current + 1);
    if (idx != target) move_track(a, idx, target);
    refresh_list(a);
    play_index(a, target);
    set_status(a, "Queued to play next", FALSE);
}
static void ctx_end(GtkButton *b, gpointer data) {
    App *a = data; int idx = ctx_index(a); close_menu(a);
    move_track(a, idx, a->track_count - 1);
    refresh_list(a);
    set_status(a, "Moved to the end of the playlist", FALSE);
}
static void ctx_up(GtkButton *b, gpointer data) {
    App *a = data; int idx = ctx_index(a); close_menu(a);
    move_track(a, idx, idx - 1);
    refresh_list(a);
}
static void ctx_down(GtkButton *b, gpointer data) {
    App *a = data; int idx = ctx_index(a); close_menu(a);
    move_track(a, idx, idx + 1);
    refresh_list(a);
}
static void ctx_remove(GtkButton *b, gpointer data) {
    App *a = data; int idx = ctx_index(a); close_menu(a);
    remove_track_at(a, idx);
    refresh_list(a);
    set_status(a, "Removed from playlist", FALSE);
}
static void ctx_reveal(GtkButton *b, gpointer data) {
    App *a = data; int idx = ctx_index(a); close_menu(a);
    if (idx < 0 || idx >= a->track_count) return;
    GFile *f = g_file_new_for_path(a->tracks[idx].path);
    char *uri = g_file_get_uri(f);
    gtk_show_uri(GTK_WINDOW(a->window), "file:///", GDK_CURRENT_TIME);
    g_free(uri);
    g_object_unref(f);
}
static void ctx_copy(GtkButton *b, gpointer data) {
    App *a = data; int idx = ctx_index(a); close_menu(a);
    if (idx < 0 || idx >= a->track_count) return;
    gdk_clipboard_set_text(gtk_widget_get_clipboard(a->window), a->tracks[idx].path);
    set_status(a, "File path copied", FALSE);
}
static void popover_closed(GtkPopover *p, gpointer data) {
    App *a = data;
    a->menu_popover = NULL;
    gtk_widget_unparent(GTK_WIDGET(p));
    if (a->sidebar_on && a->drawer && !a->drawer_timer)
        a->drawer_timer = g_timeout_add(300, hide_drawer_cb, a);
}
static void on_right_click(GtkGestureClick *g, int n, double x, double y, gpointer data) {
    App *a = data;
    GtkWidget *row = GTK_WIDGET(gtk_widget_pick(a->track_list, x, y, GTK_PICK_DEFAULT));
    while (row && !GTK_IS_LIST_BOX_ROW(row)) row = gtk_widget_get_parent(row);
    if (!row) return;
    gtk_list_box_select_row(GTK_LIST_BOX(a->track_list), GTK_LIST_BOX_ROW(row));
    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "index"));
    GtkWidget *pop = gtk_popover_new();
    gtk_popover_set_has_arrow(GTK_POPOVER(pop), TRUE);
    a->menu_popover = pop;
    g_object_set_data(G_OBJECT(pop), "index", GINT_TO_POINTER(idx));
    GtkWidget *box = ui_box(GTK_ORIENTATION_VERTICAL, 2);
    static const char *labels[] = {
        "Play", "Play next", "Move to end", "Move up", "Move down",
        "Remove from playlist", "Show in folder", "Copy file path"
    };
    GCallback cbs[] = {
        G_CALLBACK(ctx_play), G_CALLBACK(ctx_next), G_CALLBACK(ctx_end),
        G_CALLBACK(ctx_up), G_CALLBACK(ctx_down), G_CALLBACK(ctx_remove),
        G_CALLBACK(ctx_reveal), G_CALLBACK(ctx_copy)
    };
    for (int i = 0; i < 8; i++) {
        GtkWidget *b = gtk_button_new_with_label(labels[i]);
        gtk_button_set_has_frame(GTK_BUTTON(b), FALSE);
        gtk_widget_set_halign(b, GTK_ALIGN_START);
        g_signal_connect(b, "clicked", cbs[i], a);
        gtk_box_append(GTK_BOX(box), b);
        if (i == 2 || i == 5) {
            GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
            gtk_box_append(GTK_BOX(box), sep);
        }
    }
    gtk_popover_set_child(GTK_POPOVER(pop), box);
    gtk_widget_set_parent(pop, row);
    graphene_point_t in = GRAPHENE_POINT_INIT((float)x, (float)y);
    graphene_point_t out = in;
    if (!gtk_widget_compute_point(a->track_list, row, &in, &out)) out = in;
    GdkRectangle rect = { (int)out.x, (int)out.y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(pop), &rect);
    g_signal_connect(pop, "closed", G_CALLBACK(popover_closed), a);
    gtk_popover_popup(GTK_POPOVER(pop));
}

/* ----------------------------------------------------------- fullscreen --- */
static gboolean fs_syncing = FALSE;

/* Hide the pointer only while it is over our window: setting the cursor on
 * the toplevel is scoped to that widget, so the desktop keeps its own cursor
 * everywhere else. */
static void set_app_cursor(App *a, gboolean hidden) {
    if (!a->window) return;
    if (!hidden) { gtk_widget_set_cursor(a->window, NULL); return; }
    GdkCursor *blank = gdk_cursor_new_from_name("none", NULL);
    if (!blank) {
        /* Not every cursor theme ships a "none" cursor: fall back to a fully
         * transparent 1x1 texture, which is guaranteed to be invisible. */
        guint8 zero[4] = {0, 0, 0, 0};
        GBytes *bytes = g_bytes_new_static(zero, sizeof zero);
        GdkTexture *tex = gdk_memory_texture_new(1, 1, GDK_MEMORY_R8G8B8A8,
                                                 bytes, 4);
        blank = gdk_cursor_new_from_texture(tex, 0, 0, NULL);
        g_object_unref(tex);
        g_bytes_unref(bytes);
    }
    if (blank) {
        gtk_widget_set_cursor(a->window, blank);
        g_object_unref(blank);
    }
}

/* Fade with opacity instead of removing widgets: hiding them would reflow
 * the stage on every reveal and make the video stutter. */
static void set_chrome_opacity(App *a, double opacity) {
    GtkWidget *parts[] = {a->transport, a->now_header, a->status_bar, a->header, a->exit_button, a->stage_caption};
    for (guint i = 0; i < G_N_ELEMENTS(parts); i++) {
        if (!parts[i]) continue;
        gtk_widget_set_opacity(parts[i], opacity);
        gtk_widget_set_can_target(parts[i], opacity > 0.01);
    }
    a->chrome_hidden = opacity < 0.01;
}

static gboolean hide_controls_cb(gpointer data) {
    App *a = data;
    a->hide_timer = 0;
    if (!a->fullscreen) return G_SOURCE_REMOVE;
    set_chrome_opacity(a, 0.0);
    set_app_cursor(a, TRUE);
    a->controls_visible = FALSE;
    return G_SOURCE_REMOVE;
}

static void reveal_controls(App *a) {
    if (!a->fullscreen || !a->transport) return;
    if (!a->controls_visible) {
        set_chrome_opacity(a, 1.0);
        set_app_cursor(a, FALSE);
        a->controls_visible = TRUE;
    }
    if (a->hide_timer) g_source_remove(a->hide_timer);
    a->hide_timer = g_timeout_add(2500, hide_controls_cb, a);
}

static void stop_autohide(App *a) {
    if (a->hide_timer) { g_source_remove(a->hide_timer); a->hide_timer = 0; }
    if (!a->transport) return;
    set_chrome_opacity(a, 1.0);
    set_app_cursor(a, FALSE);
    a->controls_visible = TRUE;
}

static void on_motion(GtkEventControllerMotion *c, double x, double y, gpointer data) {
    App *a = data;
    if (!a->fullscreen) {
        a->motion_tracked = FALSE;
        return;
    }
    /* Ignore the synthetic motion events that keep arriving while the mouse
     * is stationary; otherwise the auto-hide timer is reset forever and the
     * OSD (and pointer) never disappear. */
    if (a->motion_tracked &&
        fabs(x - a->last_motion_x) < 2.0 &&
        fabs(y - a->last_motion_y) < 2.0)
        return;
    a->last_motion_x = x;
    a->last_motion_y = y;
    a->motion_tracked = TRUE;
    reveal_controls(a);

    /* Fullscreen: pushing the pointer against the left edge pulls out the
     * playlist drawer, like the OSD controls that appear on movement.  The
     * drawer is left-aligned, so the pointer ends up over it and its own
     * leave handler hides it again. */
    if (a->fullscreen && a->drawer) {
        if (x <= 8) {
            if (a->drawer_timer) {
                g_source_remove(a->drawer_timer);
                a->drawer_timer = 0;
            }
            if (!a->sidebar_on) {
                a->drawer_edge_open = TRUE;
                set_drawer(a, TRUE);
            }
        }
    }
}

/* Fullscreen OSD: the transport and the now-playing header are re-parented
 * into the stage overlay so the video can use every pixel of the screen.  They
 * fade in and out with the rest of the chrome instead of reserving layout
 * space, which is what used to leave bars around the picture. */
static void osd_attach(App *a) {
    if (!a->stage || !a->transport || !a->now_header) return;
    if (gtk_widget_get_parent(a->transport) != a->stage) {
        GtkWidget *parent = gtk_widget_get_parent(a->transport);
        g_object_ref(a->transport);
        if (parent && GTK_IS_BOX(parent)) gtk_box_remove(GTK_BOX(parent), a->transport);
        else if (parent) gtk_widget_unparent(a->transport);
        gtk_overlay_add_overlay(GTK_OVERLAY(a->stage), a->transport);
        g_object_unref(a->transport);
    }
    if (gtk_widget_get_parent(a->now_header) != a->stage) {
        GtkWidget *parent = gtk_widget_get_parent(a->now_header);
        g_object_ref(a->now_header);
        if (parent && GTK_IS_BOX(parent)) gtk_box_remove(GTK_BOX(parent), a->now_header);
        else if (parent) gtk_widget_unparent(a->now_header);
        gtk_overlay_add_overlay(GTK_OVERLAY(a->stage), a->now_header);
        g_object_unref(a->now_header);
    }
    gtk_widget_add_css_class(a->now_header, "osd-header");
    gtk_widget_set_halign(a->transport, GTK_ALIGN_FILL);
    gtk_widget_set_valign(a->transport, GTK_ALIGN_END);
    gtk_widget_set_margin_start(a->transport, 18);
    gtk_widget_set_margin_end(a->transport, 18);
    gtk_widget_set_margin_bottom(a->transport, 18);
    gtk_widget_set_margin_top(a->transport, 0);
    gtk_widget_set_hexpand(a->transport, TRUE);
    gtk_widget_set_halign(a->now_header, GTK_ALIGN_FILL);
    gtk_widget_set_valign(a->now_header, GTK_ALIGN_START);
    gtk_widget_set_margin_start(a->now_header, 18);
    gtk_widget_set_margin_end(a->now_header, 18);
    gtk_widget_set_margin_top(a->now_header, 18);
    gtk_widget_set_margin_bottom(a->now_header, 0);
    gtk_widget_set_hexpand(a->now_header, TRUE);
    /* The floating chrome must never reserve layout space for the video. */
    gtk_overlay_set_measure_overlay(GTK_OVERLAY(a->stage), a->transport, FALSE);
    gtk_overlay_set_measure_overlay(GTK_OVERLAY(a->stage), a->now_header, FALSE);
}

static void osd_detach(App *a) {
    if (!a->content || !a->stage) return;
    if (a->now_header && gtk_widget_get_parent(a->now_header) == a->stage) {
        g_object_ref(a->now_header);
        gtk_widget_unparent(a->now_header);
        gtk_box_prepend(GTK_BOX(a->content), a->now_header);
        g_object_unref(a->now_header);
    }
    if (a->transport && gtk_widget_get_parent(a->transport) == a->stage) {
        g_object_ref(a->transport);
        gtk_widget_unparent(a->transport);
        /* The stage now lives inside the overlay, so the transport is
         * re-inserted right after that overlay, not after the stage. */
        GtkWidget *anchor = a->stage_overlay ? a->stage_overlay : a->stage;
        gtk_box_insert_child_after(GTK_BOX(a->content), a->transport, anchor);
        g_object_unref(a->transport);
    }
    GtkWidget *w[2] = {a->now_header, a->transport};
    for (int i = 0; i < 2; i++) {
        if (!w[i]) continue;
        gtk_widget_set_halign(w[i], GTK_ALIGN_FILL);
        gtk_widget_set_valign(w[i], GTK_ALIGN_FILL);
        gtk_widget_set_margin_start(w[i], 0);
        gtk_widget_set_margin_end(w[i], 0);
        gtk_widget_set_margin_top(w[i], 0);
        gtk_widget_set_margin_bottom(w[i], 0);
        gtk_widget_set_visible(w[i], TRUE);
    }
    if (a->now_header) gtk_widget_remove_css_class(a->now_header, "osd-header");
}

static void sync_fullscreen_ui(App *a, gboolean on) {
    a->motion_tracked = FALSE;
    gtk_widget_set_visible(a->exit_button, on);
    /* Fullscreen is TV-like: the picture covers every pixel (cropped, never
     * letterboxed).  Windowed mode keeps the natural aspect ratio. */
#if GTK_CHECK_VERSION(4, 8, 0)
    if (a->picture)
        gtk_picture_set_content_fit(GTK_PICTURE(a->picture),
                                    on ? GTK_CONTENT_FIT_COVER : GTK_CONTENT_FIT_CONTAIN);
#endif
    if (on) gtk_widget_add_css_class(a->stage, "fullscreen");
    else gtk_widget_remove_css_class(a->stage, "fullscreen");

    if (a->fullscreen_btn) {
        GtkWidget *img = gtk_button_get_child(GTK_BUTTON(a->fullscreen_btn));
        if (img) gtk_image_set_from_icon_name(GTK_IMAGE(img),
            on ? "view-restore-symbolic" : "view-fullscreen-symbolic");
        fs_syncing = TRUE;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->fullscreen_btn), on);
        fs_syncing = FALSE;
    }
    if (on) {
        /* True edge-to-edge: dissolve everything that would reserve layout
         * space, then float the transport/header over the video as an OSD. */
        if (a->header) gtk_widget_set_visible(a->header, FALSE);
        if (a->status_bar) gtk_widget_set_visible(a->status_bar, FALSE);
        if (a->content) gtk_widget_add_css_class(a->content, "fullscreen");
        if (a->window) gtk_widget_add_css_class(a->window, "fullscreen");
        osd_attach(a);
        reveal_controls(a);
    } else {
        if (a->content) gtk_widget_remove_css_class(a->content, "fullscreen");
        if (a->window) gtk_widget_remove_css_class(a->window, "fullscreen");
        osd_detach(a);
        if (a->header) gtk_widget_set_visible(a->header, TRUE);
        if (a->status_bar) gtk_widget_set_visible(a->status_bar, TRUE);
        stop_autohide(a);
    }
    if (on) {
        set_drawer(a, FALSE);
        /* Entering fullscreen dissolves the settings card; keep the toggle in
         * sync so the new control-bar button opens it rather than closing a
         * card that is no longer visible. */
        if (a->effects_btn &&
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->effects_btn)))
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->effects_btn), FALSE);
    } else if (a->drawer_edge_open) {
        set_drawer(a, FALSE);
    }
    gtk_widget_set_visible(a->deck, on ? FALSE : (a->show_effects && !a->mini));
    if (a->content) {
        gtk_widget_set_margin_top(a->content, on ? 0 : 13);
        gtk_widget_set_margin_bottom(a->content, on ? 0 : 7);
        gtk_widget_set_margin_start(a->content, on ? 0 : 16);
        gtk_widget_set_margin_end(a->content, on ? 0 : 16);
    }
}
static void set_fullscreen(App *a, gboolean on);

static gboolean snap_fullscreen_cb(gpointer data) {
    set_fullscreen((App *)data, TRUE);
    return G_SOURCE_REMOVE;
}

static void set_fullscreen(App *a, gboolean on) {
    if (a->snapshot_mode && !g_getenv("NAVA_SNAPSHOT_FS")) return;
    if (a->fullscreen == on &&
        gtk_window_is_fullscreen(GTK_WINDOW(a->window)) == on) {
        sync_fullscreen_ui(a, on);
        return;
    }
    a->fullscreen = on;
    if (gtk_window_is_fullscreen(GTK_WINDOW(a->window)) != on) {
        if (on) gtk_window_fullscreen(GTK_WINDOW(a->window));
        else gtk_window_unfullscreen(GTK_WINDOW(a->window));
    }
    sync_fullscreen_ui(a, on);
}
static void on_fullscreen_toggled(GtkToggleButton *b, gpointer data) {
    App *a = data;
    if (fs_syncing) return;
    set_fullscreen(a, gtk_toggle_button_get_active(b));
}
static void on_exit_fullscreen(GtkButton *b, gpointer data) {
    set_fullscreen((App *)data, FALSE);
}
static gboolean on_window_state(GtkWidget *w, GParamSpec *ps, gpointer data) {
    App *a = data;
    gboolean on = gtk_window_is_fullscreen(GTK_WINDOW(a->window));
    if (on == a->fullscreen) return FALSE;
    a->fullscreen = on;
    sync_fullscreen_ui(a, on);
    return FALSE;
}
static gboolean text_input_focused(App *a) {
    GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(a->window));
    if (!focus) return FALSE;
    if (GTK_IS_EDITABLE(focus) || GTK_IS_TEXT_VIEW(focus)) return TRUE;
    GtkWidget *parent = gtk_widget_get_parent(focus);
    if (parent && (GTK_IS_EDITABLE(parent) || GTK_IS_TEXT_VIEW(parent))) return TRUE;
    return FALSE;
}
/* Sliders, spin buttons and drop-downs use the arrow keys themselves. */
static gboolean focus_takes_arrows(App *a) {
    GtkWidget *f = a->window ? gtk_root_get_focus(GTK_ROOT(a->window)) : NULL;
    while (f) {
        if (GTK_IS_RANGE(f) || GTK_IS_DROP_DOWN(f) || GTK_IS_SPIN_BUTTON(f) ||
            GTK_IS_SCALE_BUTTON(f) || GTK_IS_EDITABLE(f))
            return TRUE;
        f = gtk_widget_get_parent(f);
    }
    return FALSE;
}

static gboolean on_key(GtkEventControllerKey *c, guint key, guint code,
                       GdkModifierType state, gpointer data) {
    App *a = data;
    gboolean ctrl = (state & GDK_CONTROL_MASK) != 0;
    gboolean shift = (state & GDK_SHIFT_MASK) != 0;
    if (key == GDK_KEY_Escape) {
        if (a->fullscreen) { set_fullscreen(a, FALSE); return TRUE; }
        return FALSE;
    }
    if (key == GDK_KEY_F11) { set_fullscreen(a, !a->fullscreen); return TRUE; }
    if (key == GDK_KEY_F1) { on_about(NULL, a); return TRUE; }
    if (text_input_focused(a)) {
        if (key == GDK_KEY_o && ctrl) {
            if (shift) on_add_folder(NULL, a); else on_open(NULL, a);
            return TRUE;
        }
        if (key == GDK_KEY_l && ctrl) {
            set_drawer(a, !a->sidebar_on);
            return TRUE;
        }
        return FALSE;
    }
    switch (key) {
    case GDK_KEY_f: set_fullscreen(a, !a->fullscreen); return TRUE;
    case GDK_KEY_space: on_play(NULL, a); return TRUE;
    case GDK_KEY_s: case GDK_KEY_S: on_stop(NULL, a); return TRUE;
    case GDK_KEY_Right:
        if (focus_takes_arrows(a)) return FALSE;
        seek_by(a, 10); return TRUE;
    case GDK_KEY_Left:
        if (focus_takes_arrows(a)) return FALSE;
        seek_by(a, -10); return TRUE;
    case GDK_KEY_Up:
        if (focus_takes_arrows(a)) return FALSE;
        on_prev(NULL, a); return TRUE;
    case GDK_KEY_Down:
        if (focus_takes_arrows(a)) return FALSE;
        on_next(NULL, a); return TRUE;
    case GDK_KEY_m: case GDK_KEY_M:
        if (ctrl) set_mini(a, !a->mini);
        else gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->mute_button), !a->muted);
        return TRUE;
    case GDK_KEY_o: case GDK_KEY_O:
        if (ctrl && shift) on_add_folder(NULL, a);
        else if (ctrl) on_open(NULL, a);
        return ctrl;
    case GDK_KEY_l: case GDK_KEY_L:
        if (ctrl) set_drawer(a, !a->sidebar_on);
        return ctrl;
    case GDK_KEY_AudioPlay:
    case GDK_KEY_AudioPause:
        on_play(NULL, a); return TRUE;
    case GDK_KEY_AudioStop:
        engine_stop_faded(a->engine);
        set_play_icon(a, FALSE);
        set_status(a, "Stopped", FALSE);
        return TRUE;
    case GDK_KEY_AudioNext:
        on_next(NULL, a); return TRUE;
    case GDK_KEY_AudioPrev:
        on_prev(NULL, a); return TRUE;
    default:
        return FALSE;
    }
}

/* -------------------------------------------------------- stage context menu -- */
static void ctx_vis_toggle(GtkButton *btn, gpointer ud) {
    (void)ud;
    App *ap = (App *)g_object_get_data(G_OBJECT(btn), "app");
    ap->visualizer_on = !ap->visualizer_on;
    apply_stage_layout(ap);
}
static void ctx_vis_select(GtkButton *btn, gpointer ud) {
    (void)ud;
    App *ap = (App *)g_object_get_data(G_OBJECT(btn), "app");
    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "vis-idx"));
    ap->vis.mode = idx;
    ap->visualizer_on = (idx != NAVA_VIS_COUNT - 1);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(ap->vis_combo), (guint)idx);
    apply_stage_layout(ap);
}
static void ctx_fullscreen_toggle(GtkButton *btn, gpointer ud) {
    (void)ud;
    App *ap = (App *)g_object_get_data(G_OBJECT(btn), "app");
    set_fullscreen(ap, !ap->fullscreen);
}

static void on_ctx_playlist(GtkButton *b, gpointer data) {
    (void)b;
    set_drawer((App *)data, !((App *)data)->sidebar_on);
}
static void on_ctx_settings(GtkButton *b, gpointer data) {
    (void)b;
    App *a = data;
    /* Just flip the button: on_effects_toggle() owns show_effects and the
     * deck visibility, so doing it here too would flip twice. */
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->effects_btn),
                                 !gtk_toggle_button_get_active(
                                     GTK_TOGGLE_BUTTON(a->effects_btn)));
}
static void stage_menu_closed(GtkPopover *p, gpointer data);

static gboolean submenu_popup_cb(gpointer data) {
    gtk_popover_popup(GTK_POPOVER(data));
    g_object_unref(data);
    return G_SOURCE_REMOVE;
}
static void submenu_popup_idle(GtkWidget *sub) {
    g_idle_add(submenu_popup_cb, g_object_ref(sub));
}

static void on_stage_right_click(GtkGestureClick *g, int n_press,
                                 double x, double y, gpointer data) {
    (void)g; (void)n_press;
    App *a = data;

    /* The playlist rows install their own right-click menu.  This gesture
     * runs in the capture phase on the toplevel, so it would fire first and
     * stack a second popover on top of the row menu.  Let the row handler
     * own clicks that land inside the track list. */
    if (a->track_list && gtk_widget_get_mapped(a->track_list)) {
        GtkWidget *w = gtk_widget_pick(GTK_WIDGET(a->window), x, y,
                                       GTK_PICK_DEFAULT);
        for (; w; w = gtk_widget_get_parent(w)) {
            if (w == a->track_list) return;
            if (w == GTK_WIDGET(a->window)) break;
        }
    }

    GtkWidget *pop = gtk_popover_new();
    gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
    GtkWidget *box = ui_box(GTK_ORIENTATION_VERTICAL, 2);

    /* ---- playback controls ---- */
    static const char *play_labels[] = {
        "▶  Play / Pause", "⏹  Stop", "⏮  Previous", "⏭  Next"
    };
    GCallback play_cbs[] = {
        G_CALLBACK(on_play), G_CALLBACK(on_stop),
        G_CALLBACK(on_prev), G_CALLBACK(on_next)
    };
    for (int i = 0; i < 4; i++) {
        GtkWidget *b = gtk_button_new_with_label(play_labels[i]);
        gtk_button_set_has_frame(GTK_BUTTON(b), FALSE);
        gtk_widget_set_halign(b, GTK_ALIGN_START);
        g_signal_connect_swapped(b, "clicked", G_CALLBACK(gtk_popover_popdown), pop);
        g_signal_connect(b, "clicked", play_cbs[i], a);
        gtk_box_append(GTK_BOX(box), b);
    }

    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    /* ---- playlist / settings OSD ---- */
    {
        GtkWidget *b = gtk_button_new_with_label("☰  Playlist");
        gtk_button_set_has_frame(GTK_BUTTON(b), FALSE);
        gtk_widget_set_halign(b, GTK_ALIGN_START);
        g_signal_connect_swapped(b, "clicked", G_CALLBACK(gtk_popover_popdown), pop);
        g_signal_connect(b, "clicked", G_CALLBACK(on_ctx_playlist), a);
        gtk_box_append(GTK_BOX(box), b);

        b = gtk_button_new_with_label("⚙  Settings");
        gtk_button_set_has_frame(GTK_BUTTON(b), FALSE);
        gtk_widget_set_halign(b, GTK_ALIGN_START);
        g_signal_connect_swapped(b, "clicked", G_CALLBACK(gtk_popover_popdown), pop);
        g_signal_connect(b, "clicked", G_CALLBACK(on_ctx_settings), a);
        gtk_box_append(GTK_BOX(box), b);
    }

    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    /* ---- visualizer toggle ---- */
    {
        const char *vis_lbl = a->visualizer_on ? "🌀  Hide Visualizer" : "🌀  Show Visualizer";
        GtkWidget *b = gtk_button_new_with_label(vis_lbl);
        gtk_button_set_has_frame(GTK_BUTTON(b), FALSE);
        gtk_widget_set_halign(b, GTK_ALIGN_START);
        g_object_set_data(G_OBJECT(b), "app", a);
        g_signal_connect_swapped(b, "clicked", G_CALLBACK(gtk_popover_popdown), pop);
        g_signal_connect(b, "clicked", G_CALLBACK(ctx_vis_toggle), NULL);
        gtk_box_append(GTK_BOX(box), b);
    }

    /* ---- vis mode selector (submenu keeps the main menu compact) ---- */
    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    {
        GtkWidget *trigger = gtk_button_new_with_label("🎛  Visualization mode  ▸");
        gtk_button_set_has_frame(GTK_BUTTON(trigger), FALSE);
        gtk_widget_set_halign(trigger, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(box), trigger);

        GtkWidget *sub = gtk_popover_new();
        gtk_popover_set_has_arrow(GTK_POPOVER(sub), FALSE);
        gtk_popover_set_position(GTK_POPOVER(sub), GTK_POS_RIGHT);

        GtkWidget *subbox = ui_box(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget *subsw = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(subsw),
                                       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(subsw), 360);
        gtk_scrolled_window_set_propagate_natural_height(
            GTK_SCROLLED_WINDOW(subsw), TRUE);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(subsw), subbox);
        gtk_popover_set_child(GTK_POPOVER(sub), subsw);

        for (int i = 0; i < N_VIS_MODES; i++) {
            GtkWidget *b = gtk_button_new_with_label(VIS_LABELS[i]);
            gtk_button_set_has_frame(GTK_BUTTON(b), FALSE);
            gtk_widget_set_halign(b, GTK_ALIGN_START);
            if (i == a->vis.mode)
                gtk_widget_add_css_class(b, "active-toggle");
            g_object_set_data(G_OBJECT(b), "app", a);
            g_object_set_data(G_OBJECT(b), "vis-idx", GINT_TO_POINTER(i));
            g_signal_connect(b, "clicked", G_CALLBACK(ctx_vis_select), NULL);
            g_signal_connect_swapped(b, "clicked", G_CALLBACK(gtk_popover_popdown), sub);
            g_signal_connect_swapped(b, "clicked", G_CALLBACK(gtk_popover_popdown), pop);
            gtk_box_append(GTK_BOX(subbox), b);
        }

        gtk_widget_set_parent(sub, trigger);
        /* Same lesson as the main menu: opening a popover during the click
         * that triggered it makes GTK treat that click as an outside click
         * and close it immediately.  Defer to idle. */
        g_signal_connect_swapped(trigger, "clicked",
                                 G_CALLBACK(submenu_popup_idle), sub);
        /* Closing the main menu must also tear down the submenu. */
        g_signal_connect_swapped(pop, "closed",
                                 G_CALLBACK(gtk_popover_popdown), sub);
    }

    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    /* ---- fullscreen toggle ---- */
    {
        const char *fs_lbl = a->fullscreen ? "⛶  Exit Fullscreen" : "⛶  Enter Fullscreen";
        GtkWidget *b = gtk_button_new_with_label(fs_lbl);
        gtk_button_set_has_frame(GTK_BUTTON(b), FALSE);
        gtk_widget_set_halign(b, GTK_ALIGN_START);
        g_object_set_data(G_OBJECT(b), "app", a);
        g_signal_connect_swapped(b, "clicked", G_CALLBACK(gtk_popover_popdown), pop);
        g_signal_connect(b, "clicked", G_CALLBACK(ctx_fullscreen_toggle), NULL);
        gtk_box_append(GTK_BOX(box), b);
    }

    /* The full menu is taller than a laptop screen, so cap it and scroll. */
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(sw), 460);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(sw), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), box);
    gtk_popover_set_child(GTK_POPOVER(pop), sw);
    GtkWidget *host = GTK_WIDGET(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g)));
    if (!host) host = a->stage;
    gtk_widget_set_parent(pop, host);
    GdkRectangle rect = { (int)x, (int)y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(pop), &rect);
    g_signal_connect(pop, "closed", G_CALLBACK(stage_menu_closed), NULL);
    /* This handler runs on the gesture's "released" signal: opening the
     * popover during the button press made GTK treat the very same click as
     * an outside click and dismiss it right away. */
    gtk_popover_popup(GTK_POPOVER(pop));
}

static void stage_menu_closed(GtkPopover *p, gpointer data) {
    (void)data;
    gtk_widget_unparent(GTK_WIDGET(p));
}

static void on_stage_pressed(GtkGestureClick *g, int n, double x, double y, gpointer data) {
    App *a = data;
    if (a->snapshot_mode) return;
    /* Ignore double-clicks that land on overlaid controls (exit button). */
    GtkWidget *hit = GTK_WIDGET(gtk_widget_pick(a->stage, x, y, GTK_PICK_DEFAULT));
    while (hit && hit != a->stage) {
        if (hit == a->exit_button || GTK_IS_BUTTON(hit)) return;
        hit = gtk_widget_get_parent(hit);
    }
    if (n == 2) set_fullscreen(a, !a->fullscreen);
}

/* ------------------------------------------------------------ status tick -- */
static gboolean tick_cb(gpointer data) {
    App *a = data;
    if (engine_active(a->engine)) {
        double pos = engine_position(a->engine);
        double dur = engine_duration(a->engine);
        if (a->pending_seek) {
            a->pending_seek_ticks++;
            gboolean settled = !engine_seek_pending(a->engine) &&
                               fabs(pos - a->pending_seek_pos) < 0.6;
            /* Safety net: never pin the slider forever if a seek is dropped. */
            if (settled || a->pending_seek_ticks > 60)
                a->pending_seek = FALSE;
        }
        if (a->pending_seek && !a->seek_dragging) {
            char buf[32];
            format_time(a->pending_seek_pos, buf, sizeof(buf));
            gtk_label_set_text(GTK_LABEL(a->time_now), buf);
            a->updating_seek = TRUE;
            gtk_range_set_value(GTK_RANGE(a->seek), a->pending_seek_pos);
            a->updating_seek = FALSE;
        } else if (!a->seek_dragging) {
            char buf[32];
            format_time(pos, buf, sizeof(buf));
            gtk_label_set_text(GTK_LABEL(a->time_now), buf);
            format_time(dur, buf, sizeof(buf));
            gtk_label_set_text(GTK_LABEL(a->time_total), buf);
            if (dur > 0) {
                a->updating_seek = TRUE;
                GtkAdjustment *adj = gtk_range_get_adjustment(GTK_RANGE(a->seek));
                if (adj && fabs(gtk_adjustment_get_upper(adj) - dur) > 0.5)
                    gtk_range_set_range(GTK_RANGE(a->seek), 0, dur);
                gtk_range_set_value(GTK_RANGE(a->seek), pos);
                a->updating_seek = FALSE;
            }
        }
        /* The engine swaps files when the blend finishes; the new track is
         * then far from its end, which is how we disarm the guard. */
        if (a->xf_armed && dur > 0 &&
            dur - pos > a->crossfade_ms / 1000.0 + 0.25)
            a->xf_armed = FALSE;
        /* Start the blend before the track actually ends so the outgoing
         * audio is still playing when the incoming one ramps up. */
        if (!a->xf_armed && a->crossfade_on && dur > 0 &&
            engine_playing(a->engine) && a->current >= 0) {
            int nidx = next_index(a, TRUE);
            if (nidx >= 0 && nidx != a->current &&
                dur - pos <= a->crossfade_ms / 1000.0 && dur - pos > 0.05)
                switch_track(a, nidx);
        }
        if (engine_eof(a->engine) && !a->xf_armed) {
            engine_clear_eof(a->engine);
            int idx = next_index(a, TRUE);
            if (idx >= 0) switch_track(a, idx);
            else { 
                set_play_icon(a, FALSE); 
                set_status(a, "End of playlist — press Play to restart", FALSE);
                /* Allow restart from beginning */
                a->current = -1;
                a->at_end = TRUE;
            }
        }
    }
    return G_SOURCE_CONTINUE;
}

static gboolean visuals_cb(gpointer data) {
    App *a = data;
    a->cover_phase += 0.05;
    gboolean active = engine_active(a->engine);

    /* Glide the album art between the corner and the centre over ~350 ms. */
    gboolean art_moving = FALSE;
    if (a->art_anim != a->art_target) {
        art_moving = TRUE;
        double step = 0.033 / 0.35;
        if (a->art_anim < a->art_target) {
            a->art_anim += step;
            if (a->art_anim > a->art_target) a->art_anim = a->art_target;
        } else {
            a->art_anim -= step;
            if (a->art_anim < a->art_target) a->art_anim = a->art_target;
        }
        if (a->stage_art) gtk_widget_queue_draw(a->stage_art);
    }
    if (active) {
        float peak = 0, rms = 0;
        engine_get_levels(a->engine, &peak, &rms);
        float energy = peak * 0.6f + rms * 0.7f;
        a->cover_energy = energy < 0 ? 0 : (energy > 1 ? 1 : energy);
    } else {
        a->cover_energy *= 0.85f;
    }
    if (a->stage_art && gtk_widget_get_visible(a->stage_art) && !a->showing_video) {
        if (active || art_moving || a->cover_energy > 0.01f) {
            gtk_widget_queue_draw(a->stage_art);
        }
    }
    if (!a->visualizer_on || a->showing_video) return G_SOURCE_CONTINUE;

    if (active) {
        int got = engine_get_spectrum(a->engine, a->raw_spec, VIS_BANDS);
        a->vis.phase += 0.045;
        if (got > 0) {
            for (int i = 0; i < VIS_BANDS; i++) {
                float target = a->raw_spec[i];
                float prev = a->vis.bands[i];
                a->vis.bands[i] = prev + (target - prev) * (target > prev ? 0.55f : 0.10f);
            }
            double sum = 0;
            for (int i = 0; i < VIS_BANDS; i++) sum += a->vis.bands[i];
            a->vis.energy = (float)(sum / VIS_BANDS);
        }
        int wg = engine_get_waveform(a->engine, a->raw_wave, VIS_WAVE);
        if (wg > 0) {
            for (int i = 0; i < VIS_WAVE; i++) {
                float v = a->raw_wave[i];
                a->vis.wave[i] = v < 0 ? 0 : (v > 1 ? 1 : v);
            }
        }
        gtk_widget_queue_draw(a->vis_draw);
    } else {
        if (a->vis.energy > 0.01f) {
            for (int i = 0; i < VIS_BANDS; i++) a->vis.bands[i] *= 0.80f;
            for (int i = 0; i < VIS_WAVE; i++) a->vis.wave[i] *= 0.80f;
            a->vis.energy *= 0.80f;
            gtk_widget_queue_draw(a->vis_draw);
        }
    }
    return G_SOURCE_CONTINUE;
}

static gboolean frame_cb(gpointer data) {
    App *a = data;
    if (!a->showing_video) return G_SOURCE_CONTINUE;
    /* Present by PTS against the audio clock so 24/25/30 fps content is
     * smooth instead of being sampled at a fixed 30 Hz. */
    double target = engine_position(a->engine);
    gint64 mono = g_get_monotonic_time();
    if (a->vclock_wall == 0 || fabs(a->vclock - target) > 1.0) {
        a->vclock = target;
    } else {
        double dt = (double)(mono - a->vclock_wall) / 1e6;
        if (dt > 0.25) dt = 0.25;
        a->vclock += dt;
        /* Slow servo (tau ~1.2 s): follow the average rate of the audio
         * clock without inheriting its per-fragment steps. */
        double alpha = 1.0 - exp(-dt / 1.2);
        double err = target - a->vclock;
        if (err > 0.15) err = 0.15;
        if (err < -0.15) err = -0.15;
        a->vclock += err * alpha;
    }
    a->vclock_wall = mono;
    double clock = a->vclock;
    for (int guard = 0; guard < 4; guard++) {
        double pts = engine_next_frame_pts(a->engine);
        if (pts >= 0.0 && clock > 0.0 && pts > clock + 0.006) break;
        uint8_t *px = NULL; int w = 0, h = 0;
        if (!engine_frame(a->engine, &px, &w, &h)) break;
        GBytes *bytes = g_bytes_new_take(px, (gsize)w * h * 4);
        GdkTexture *tex = gdk_memory_texture_new(w, h, GDK_MEMORY_R8G8B8A8, bytes,
                                                 (gsize)w * 4);
        g_bytes_unref(bytes);
        if (a->texture) g_object_unref(a->texture);
        a->texture = tex;
        gtk_picture_set_paintable(GTK_PICTURE(a->picture), GDK_PAINTABLE(tex));
        if (pts < 0.0) break;
    }
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------- UI --- */
static GtkWidget *build_now_header(App *a) {
    GtkWidget *box = ui_box(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *words = ui_box(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_hexpand(words, TRUE);
    a->now_title = ui_label("Nothing to play", "now-title", 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(a->now_title), PANGO_ELLIPSIZE_END);
    a->now_sub = ui_label("Drop a file here or pick one from the playlist", "now-sub", 0.0f);
    gtk_box_append(GTK_BOX(words), a->now_title);
    gtk_box_append(GTK_BOX(words), a->now_sub);
    gtk_box_append(GTK_BOX(box), words);

    GtkWidget *add = gtk_button_new_with_label("＋  Add media");
    gtk_widget_add_css_class(add, "accent-button");
    gtk_widget_set_tooltip_text(add, "Add files (Ctrl+O)");
    gtk_widget_set_valign(add, GTK_ALIGN_CENTER);
    g_signal_connect(add, "clicked", G_CALLBACK(on_open), a);
    gtk_box_append(GTK_BOX(box), add);

    a->format_chip = ui_label("—", "chip", 0.5f);
    gtk_widget_set_valign(a->format_chip, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), a->format_chip);
    a->quality_chip = ui_label("", "chip", 0.5f);
    gtk_widget_add_css_class(a->quality_chip, "accent");
    gtk_widget_set_valign(a->quality_chip, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(a->quality_chip, FALSE);
    gtk_box_append(GTK_BOX(box), a->quality_chip);
    return box;
}

static GtkWidget *build_stage(App *a) {
    GtkWidget *stage = gtk_overlay_new();
    gtk_widget_add_css_class(stage, "stage-card");
    gtk_widget_set_vexpand(stage, TRUE);
    a->stage = stage;

    a->picture = gtk_picture_new();
#if GTK_CHECK_VERSION(4, 8, 0)
    gtk_picture_set_content_fit(GTK_PICTURE(a->picture), GTK_CONTENT_FIT_CONTAIN);
#endif
    gtk_picture_set_can_shrink(GTK_PICTURE(a->picture), TRUE);
    gtk_widget_set_hexpand(a->picture, TRUE);
    gtk_widget_set_vexpand(a->picture, TRUE);
    gtk_widget_set_visible(a->picture, FALSE);
    gtk_overlay_set_child(GTK_OVERLAY(stage), a->picture);

    a->vis_draw = gtk_drawing_area_new();
    gtk_widget_add_css_class(a->vis_draw, "stage-visualizer");
    gtk_widget_set_hexpand(a->vis_draw, TRUE);
    gtk_widget_set_vexpand(a->vis_draw, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(a->vis_draw), draw_visualizer, a, NULL);
    gtk_widget_set_visible(a->vis_draw, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(stage), a->vis_draw);

    a->stage_art = gtk_drawing_area_new();
    gtk_widget_add_css_class(a->stage_art, "cover-art");
    gtk_widget_set_hexpand(a->stage_art, TRUE);
    gtk_widget_set_vexpand(a->stage_art, TRUE);
    gtk_widget_set_halign(a->stage_art, GTK_ALIGN_FILL);
    gtk_widget_set_valign(a->stage_art, GTK_ALIGN_FILL);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(a->stage_art), draw_cover, a, NULL);
    gtk_overlay_add_overlay(GTK_OVERLAY(stage), a->stage_art);

    a->stage_caption = ui_box(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_halign(a->stage_caption, GTK_ALIGN_START);
    gtk_widget_set_valign(a->stage_caption, GTK_ALIGN_END);
    gtk_widget_set_margin_start(a->stage_caption, 20);
    gtk_widget_set_margin_bottom(a->stage_caption, 16);
    gtk_widget_add_css_class(a->stage_caption, "stage-caption");
    a->stage_title = ui_label("", "stage-title", 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(a->stage_title), PANGO_ELLIPSIZE_END);
    a->stage_sub = ui_label("", "stage-sub", 0.0f);
    gtk_box_append(GTK_BOX(a->stage_caption), a->stage_title);
    gtk_box_append(GTK_BOX(a->stage_caption), a->stage_sub);
    gtk_widget_set_visible(a->stage_caption, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(stage), a->stage_caption);

    a->empty_label = ui_box(GTK_ORIENTATION_VERTICAL, 7);
    gtk_widget_add_css_class(a->empty_label, "empty-state");
    gtk_widget_set_halign(a->empty_label, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(a->empty_label, GTK_ALIGN_CENTER);
    GtkWidget *icon = ui_label("♫", "empty-icon", 0.5f);
    GtkWidget *et = ui_label("The stage is empty", "empty-title", 0.5f);
    GtkWidget *es = ui_label("Drop a file or folder on the window, or use “Add media”",
                             "empty-sub", 0.5f);
    gtk_label_set_wrap(GTK_LABEL(es), TRUE);
    gtk_label_set_justify(GTK_LABEL(es), GTK_JUSTIFY_CENTER);
    GtkWidget *pill = ui_label("Drag & Drop   ·   Ctrl+O   ·   Ctrl+Shift+O", "drop-pill", 0.5f);
    gtk_box_append(GTK_BOX(a->empty_label), icon);
    gtk_box_append(GTK_BOX(a->empty_label), et);
    gtk_box_append(GTK_BOX(a->empty_label), es);
    gtk_box_append(GTK_BOX(a->empty_label), pill);
    gtk_overlay_add_overlay(GTK_OVERLAY(stage), a->empty_label);

    a->exit_button = gtk_button_new_with_label("✕  Exit fullscreen");
    gtk_widget_add_css_class(a->exit_button, "fullscreen-exit");
    gtk_widget_set_halign(a->exit_button, GTK_ALIGN_END);
    gtk_widget_set_valign(a->exit_button, GTK_ALIGN_START);
    gtk_widget_set_visible(a->exit_button, FALSE);
    g_signal_connect(a->exit_button, "clicked",
                     G_CALLBACK(on_exit_fullscreen), a);
    gtk_overlay_add_overlay(GTK_OVERLAY(stage), a->exit_button);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "pressed", G_CALLBACK(on_stage_pressed), a);
    gtk_widget_add_controller(stage, GTK_EVENT_CONTROLLER(click));

    /* The right-click menu is installed once on the window (capture phase),
     * so it also fires over the stage and the visualizer. */

    return stage;
}

static GtkWidget *build_transport(App *a) {
    GtkWidget *card = ui_box(GTK_ORIENTATION_VERTICAL, 7);
    gtk_widget_add_css_class(card, "transport-card");

    GtkWidget *progress = ui_box(GTK_ORIENTATION_HORIZONTAL, 8);
    a->time_now = ui_label("0:00", "time-label", 0.5f);
    a->time_total = ui_label("0:00", "time-label", 0.5f);
    a->seek = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, 0.5);
    gtk_widget_add_css_class(a->seek, "seek-scale");
    gtk_scale_set_draw_value(GTK_SCALE(a->seek), FALSE);
    gtk_widget_set_hexpand(a->seek, TRUE);
    g_signal_connect(a->seek, "value-changed", G_CALLBACK(on_seek_changed), a);
    gtk_box_append(GTK_BOX(progress), a->time_now);
    gtk_box_append(GTK_BOX(progress), a->seek);
    gtk_box_append(GTK_BOX(progress), a->time_total);
    gtk_box_append(GTK_BOX(card), progress);

    GtkWidget *controls = ui_box(GTK_ORIENTATION_HORIZONTAL, 9);
    GtkWidget *left = ui_box(GTK_ORIENTATION_HORIZONTAL, 7);
    gtk_widget_set_valign(left, GTK_ALIGN_CENTER);
    a->mute_button = ui_toggle_button("audio-volume-high-symbolic", "Mute (M)", NULL, NULL, "flat");
    gtk_widget_add_css_class(a->mute_button, "icon-button");
    gtk_widget_set_tooltip_text(a->mute_button, "Mute (M)");
    g_signal_connect(a->mute_button, "toggled", G_CALLBACK(on_mute), a);
    gtk_box_append(GTK_BOX(left), a->mute_button);
    a->vol_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1.3, 0.01);
    gtk_widget_add_css_class(a->vol_scale, "volume-scale");
    gtk_range_set_value(GTK_RANGE(a->vol_scale), 0.8);
    gtk_scale_set_draw_value(GTK_SCALE(a->vol_scale), FALSE);
    gtk_widget_set_size_request(a->vol_scale, 100, -1);
    g_signal_connect(a->vol_scale, "value-changed", G_CALLBACK(on_volume), a);
    gtk_box_append(GTK_BOX(left), a->vol_scale);

    GtkWidget *centre = ui_box(GTK_ORIENTATION_HORIZONTAL, 9);
    gtk_widget_set_halign(centre, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(centre, TRUE);
    GtkWidget *prev = ui_icon_button("media-skip-backward-symbolic", "Previous",
                                     G_CALLBACK(on_prev), a, "transport-button");
    GtkWidget *back = ui_icon_button("media-seek-backward-symbolic", "Back 10 seconds",
                                     G_CALLBACK(on_back10), a, "transport-button");
    a->play_button = gtk_button_new_from_icon_name("media-playback-start-symbolic");
    gtk_widget_add_css_class(a->play_button, "play-button");
    gtk_widget_set_tooltip_text(a->play_button, "Play / pause (Space)");
    g_signal_connect(a->play_button, "clicked", G_CALLBACK(on_play), a);
    GtkWidget *stop = ui_icon_button("media-playback-stop-symbolic", "Stop (S)",
                                     G_CALLBACK(on_stop), a, "transport-button");
    GtkWidget *fwd = ui_icon_button("media-seek-forward-symbolic", "Forward 10 seconds",
                                    G_CALLBACK(on_fwd10), a, "transport-button");
    GtkWidget *next = ui_icon_button("media-skip-forward-symbolic", "Next",
                                     G_CALLBACK(on_next), a, "transport-button");
    for (GtkWidget *w = prev; w; w = NULL) gtk_box_append(GTK_BOX(centre), w);
    gtk_box_append(GTK_BOX(centre), back);
    gtk_box_append(GTK_BOX(centre), a->play_button);
    gtk_box_append(GTK_BOX(centre), stop);
    gtk_box_append(GTK_BOX(centre), fwd);
    gtk_box_append(GTK_BOX(centre), next);

    GtkWidget *right = ui_box(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_valign(right, GTK_ALIGN_CENTER);

    GtkWidget *shuf_col = ui_box(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_valign(shuf_col, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(shuf_col, GTK_ALIGN_CENTER);
    a->shuffle_button = ui_toggle_button("media-playlist-shuffle-symbolic",
                                         "Shuffle on/off", NULL, NULL, "flat");
    gtk_widget_add_css_class(a->shuffle_button, "icon-button");
    gtk_widget_set_halign(a->shuffle_button, GTK_ALIGN_CENTER);
    g_signal_connect(a->shuffle_button, "toggled", G_CALLBACK(on_shuffle), a);
    a->shuffle_label = ui_label("Off", "toggle-caption", 0.5f);
    gtk_box_append(GTK_BOX(shuf_col), a->shuffle_button);
    gtk_box_append(GTK_BOX(shuf_col), a->shuffle_label);
    gtk_box_append(GTK_BOX(right), shuf_col);

    GtkWidget *rep_col = ui_box(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_valign(rep_col, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(rep_col, GTK_ALIGN_CENTER);
    a->repeat_button = gtk_button_new_from_icon_name("media-playlist-repeat-symbolic");
    gtk_widget_add_css_class(a->repeat_button, "icon-button");
    gtk_widget_set_halign(a->repeat_button, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(a->repeat_button, "Repeat off / all / one");
    g_signal_connect(a->repeat_button, "clicked", G_CALLBACK(on_repeat), a);
    a->repeat_label = ui_label("Off", "toggle-caption", 0.5f);
    gtk_box_append(GTK_BOX(rep_col), a->repeat_button);
    gtk_box_append(GTK_BOX(rep_col), a->repeat_label);
    gtk_box_append(GTK_BOX(right), rep_col);

    GtkWidget *open_btn = ui_icon_button("document-open-symbolic",
                                         "Open files (Ctrl+O)",
                                         G_CALLBACK(on_open), a,
                                         "transport-chip");
    gtk_widget_set_valign(open_btn, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(right), open_btn);

    a->sidebar_btn = ui_toggle_button("sidebar-show-symbolic",
                                      "Playlist (Ctrl+L)",
                                      NULL, NULL, "transport-chip");
    gtk_widget_set_valign(a->sidebar_btn, GTK_ALIGN_CENTER);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->sidebar_btn), FALSE);
    gtk_box_append(GTK_BOX(right), a->sidebar_btn);

    a->effects_btn = ui_toggle_button("preferences-other-symbolic",
                                      "Settings",
                                      NULL, NULL, "transport-chip");
    gtk_widget_set_valign(a->effects_btn, GTK_ALIGN_CENTER);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->effects_btn), FALSE);
    gtk_box_append(GTK_BOX(right), a->effects_btn);

    a->mini_btn = ui_toggle_button("window-restore-symbolic",
                                   "Mini player (Ctrl+M)",
                                   NULL, NULL, "transport-chip");
    gtk_widget_set_valign(a->mini_btn, GTK_ALIGN_CENTER);
    g_signal_connect(a->mini_btn, "toggled", G_CALLBACK(on_mini_toggle), a);
    gtk_box_append(GTK_BOX(right), a->mini_btn);

    a->fullscreen_btn = ui_toggle_button("view-fullscreen-symbolic",
                                         "Fullscreen (F11)",
                                         NULL, NULL, "transport-chip");
    gtk_widget_set_valign(a->fullscreen_btn, GTK_ALIGN_CENTER);
    g_signal_connect(a->fullscreen_btn, "toggled",
                     G_CALLBACK(on_fullscreen_toggled), a);
    gtk_box_append(GTK_BOX(right), a->fullscreen_btn);

    a->speed_combo = gtk_drop_down_new_from_strings(SPEED_NAMES);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(a->speed_combo), 2);
    gtk_widget_set_tooltip_text(a->speed_combo, "Playback speed");
    g_signal_connect(a->speed_combo, "notify::selected", G_CALLBACK(on_speed), a);
    gtk_box_append(GTK_BOX(right), a->speed_combo);

    gtk_box_append(GTK_BOX(controls), left);
    gtk_box_append(GTK_BOX(controls), centre);
    gtk_box_append(GTK_BOX(controls), right);
    gtk_box_append(GTK_BOX(card), controls);
    return card;
}

static void ctx_remove_placeholder(GtkButton *b, gpointer data) {
    (void)b;
    App *a = data;
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(a->track_list));
    if (!row) { set_status(a, "Select a track to remove", TRUE); return; }
    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "index"));
    remove_track_at(a, idx);
    refresh_list(a);
    set_status(a, "Removed from playlist", FALSE);
}

static GtkWidget *build_sidebar(App *a) {
    GtkWidget *box = ui_box(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(box, "sidebar");
    gtk_widget_set_size_request(box, 270, -1);

    GtkWidget *head = ui_box(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *heading = ui_label("Playlist", "sidebar-title", 0.0f);
    gtk_widget_set_hexpand(heading, TRUE);
    a->count_label = ui_label("0", "count-pill", 0.5f);
    gtk_box_append(GTK_BOX(head), heading);
    gtk_box_append(GTK_BOX(head), a->count_label);
    GtkWidget *save_btn = ui_icon_button("document-save-symbolic",
                                         "Save playlist (M3U)",
                                         G_CALLBACK(on_save_playlist), a,
                                         "flat");
    gtk_box_append(GTK_BOX(head), save_btn);
    gtk_box_append(GTK_BOX(box), head);

    GtkWidget *hint = ui_label("Drag files here and drop them", "sidebar-hint", 0.0f);
    gtk_box_append(GTK_BOX(box), hint);

    a->search = gtk_search_entry_new();
    gtk_widget_add_css_class(a->search, "search-entry");
#if GTK_CHECK_VERSION(4, 10, 0)
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(a->search), "Search…");
#endif
    g_signal_connect_swapped(a->search, "search-changed", G_CALLBACK(refresh_list), a);
    gtk_box_append(GTK_BOX(box), a->search);

    GtkWidget *buttons = ui_box(GTK_ORIENTATION_HORIZONTAL, 5);
    const char *labels[] = {"＋ File", "▤ Folder", "✕ Remove", "Clear"};
    GCallback cbs[] = {G_CALLBACK(on_open), G_CALLBACK(on_add_folder),
                       G_CALLBACK(ctx_remove_placeholder), G_CALLBACK(on_clear_clicked)};
    for (int i = 0; i < 4; i++) {
        GtkWidget *b = gtk_button_new_with_label(labels[i]);
        gtk_widget_add_css_class(b, "pill-button");
        gtk_widget_set_hexpand(b, TRUE);
        g_signal_connect(b, "clicked", cbs[i], a);
        gtk_box_append(GTK_BOX(buttons), b);
    }
    gtk_box_append(GTK_BOX(box), buttons);

    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    a->track_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(a->track_list), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(a->track_list), FALSE);
    GtkGesture *row_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(row_click), GDK_BUTTON_PRIMARY);
    g_signal_connect(row_click, "pressed", G_CALLBACK(on_list_pressed), a);
    gtk_widget_add_controller(a->track_list, GTK_EVENT_CONTROLLER(row_click));
    GtkGesture *right = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right), GDK_BUTTON_SECONDARY);
    g_signal_connect(right, "pressed", G_CALLBACK(on_right_click), a);
    gtk_widget_add_controller(a->track_list, GTK_EVENT_CONTROLLER(right));
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), a->track_list);
    gtk_box_append(GTK_BOX(box), scroller);
    return box;
}

static GtkWidget *param_slider_cb(const char *label, double low, double high, double step,
                                  double value, App *a, GtkWidget **out) {
    return param_slider(label, low, high, step, value, G_CALLBACK(on_param_fx), a, out);
}

static GtkWidget *build_eq_page(App *a) {
    GtkWidget *page = ui_box(GTK_ORIENTATION_VERTICAL, 7);
    gtk_widget_add_css_class(page, "panel-page");
    GtkWidget *top = ui_box(GTK_ORIENTATION_HORIZONTAL, 9);
    a->eq_check = gtk_check_button_new_with_label("Enabled");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(a->eq_check), TRUE);
    g_signal_connect(a->eq_check, "toggled", G_CALLBACK(on_eq_toggled), a);
    gtk_box_append(GTK_BOX(top), a->eq_check);
    a->eq_preset = gtk_drop_down_new_from_strings(EQ_PRESET_NAMES);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(a->eq_preset), 0);
    g_signal_connect(a->eq_preset, "notify::selected", G_CALLBACK(on_preset), a);
    gtk_box_append(GTK_BOX(top), a->eq_preset);
    GtkWidget *reset = gtk_button_new_with_label("Reset");
    gtk_widget_add_css_class(reset, "pill-button");
    g_signal_connect(reset, "clicked", G_CALLBACK(on_reset_eq), a);
    gtk_box_append(GTK_BOX(top), reset);
    gtk_box_append(GTK_BOX(page), top);

    GtkWidget *bands = ui_box(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_halign(bands, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(bands, TRUE);
    for (int i = 0; i < EQ_BANDS; i++) {
        GtkWidget *col = ui_box(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_add_css_class(col, "eq-box");
        gtk_widget_set_hexpand(col, TRUE);
        GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL, -24, 12, 0.5);
        gtk_widget_add_css_class(scale, "eq-scale");
        gtk_range_set_inverted(GTK_RANGE(scale), TRUE);
        gtk_widget_set_size_request(scale, 28, 118);
        gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
        gtk_range_set_value(GTK_RANGE(scale), a->eq_gains[i]);
        g_signal_connect(scale, "value-changed", G_CALLBACK(on_eq_band), a);
        gtk_box_append(GTK_BOX(col), scale);
        a->eq_scales[i] = scale;
        char buf[16];
        snprintf(buf, sizeof(buf), "%+.1f", a->eq_gains[i]);
        a->eq_values[i] = ui_label(buf, "eq-value", 0.5f);
        gtk_box_append(GTK_BOX(col), a->eq_values[i]);
        char freq[16];
        snprintf(freq, sizeof(freq), "%g", EQ_FREQS[i]);
        gtk_box_append(GTK_BOX(col), ui_label(freq, "eq-freq", 0.5f));
        gtk_box_append(GTK_BOX(bands), col);
    }
    gtk_box_append(GTK_BOX(page), bands);
    return page;
}

static GtkWidget *build_audio_page(App *a) {
    GtkWidget *page = ui_box(GTK_ORIENTATION_VERTICAL, 7);
    gtk_widget_add_css_class(page, "panel-page");
    gtk_box_append(GTK_BOX(page), param_slider_cb("Echo intensity", 0, 1, 0.01, 0.0, a, &a->echo_scale));
    gtk_box_append(GTK_BOX(page), param_slider_cb("Echo feedback", 0, 0.9, 0.01, 0.0, a, &a->fb_scale));
    gtk_box_append(GTK_BOX(page), param_slider_cb("Echo delay", 10, 2000, 10, 120, a, &a->delay_scale));
    gtk_box_append(GTK_BOX(page), param_slider_cb("Compressor", 0, 1, 0.01, 0.0, a, &a->comp_scale));
    gtk_box_append(GTK_BOX(page), param_slider_cb("Reverb", 0, 1, 0.01, 0.0, a, &a->reverb_scale));
    gtk_box_append(GTK_BOX(page), param_slider_cb("Flanger", 0, 1, 0.01, 0.0, a, &a->flanger_scale));

    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append(GTK_BOX(page), sep);

    GtkWidget *xf_top = ui_box(GTK_ORIENTATION_HORIZONTAL, 9);
    a->xf_check = gtk_check_button_new_with_label("Crossfade tracks (next / prev / auto)");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(a->xf_check), a->crossfade_on);
    g_signal_connect(a->xf_check, "toggled", G_CALLBACK(on_xf_toggled), a);
    gtk_box_append(GTK_BOX(xf_top), a->xf_check);
    gtk_box_append(GTK_BOX(page), xf_top);

    gtk_box_append(GTK_BOX(page), param_slider("Crossfade time", 0.3, 10.0, 0.1,
                                               a->crossfade_ms / 1000.0,
                                               G_CALLBACK(on_xf_changed), a, &a->xf_scale));
    a->xf_value = g_object_get_data(G_OBJECT(a->xf_scale), "readout");
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1fs", a->crossfade_ms / 1000.0);
        gtk_label_set_text(GTK_LABEL(a->xf_value), buf);
    }

    gtk_box_append(GTK_BOX(page), param_slider("Transport fade", 20, 1000, 5,
                                               a->fade_ms,
                                               G_CALLBACK(on_fade_changed), a, &a->fade_scale));
    a->fade_value = g_object_get_data(G_OBJECT(a->fade_scale), "readout");
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%dms", (int)(a->fade_ms + 0.5));
        gtk_label_set_text(GTK_LABEL(a->fade_value), buf);
    }
    gtk_box_append(GTK_BOX(page), ui_label("Echo, reverb, flanger and compressor apply to playback immediately",
                                           "hint-label", 0.0f));
    return page;
}

static GtkWidget *build_video_page(App *a) {
    GtkWidget *page = ui_box(GTK_ORIENTATION_VERTICAL, 7);
    gtk_widget_add_css_class(page, "panel-page");
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 5);
    GtkWidget *bright, *contrast, *satur, *gamma, *hue;
    GtkWidget *rows[5];
    rows[0] = param_slider_cb("Brightness", -1, 1, 0.01, 0.0, a, &bright);
    rows[1] = param_slider_cb("Contrast", 0, 2, 0.01, 1.0, a, &contrast);
    rows[2] = param_slider_cb("Saturation", 0, 3, 0.01, 1.0, a, &satur);
    rows[3] = param_slider_cb("Hue", -180, 180, 1, 0.0, a, &hue);
    rows[4] = param_slider_cb("Gamma", 0.1, 10, 0.05, 1.0, a, &gamma);
    for (int i = 0; i < 5; i++)
        gtk_grid_attach(GTK_GRID(grid), rows[i], i % 2, i / 2, 1, 1);
    gtk_box_append(GTK_BOX(page), grid);
    a->bright_scale = bright;
    a->contrast_scale = contrast;
    a->satur_scale = satur;
    a->gamma_scale = gamma;
    a->hue_scale = hue;

    GtkWidget *row = ui_box(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(row), ui_label("Rotation / mirror", "param-label", 0.0f));
    a->rotation_combo = gtk_drop_down_new_from_strings(ROTATION_NAMES);
    g_signal_connect(a->rotation_combo, "notify::selected", G_CALLBACK(on_rotation), a);
    gtk_box_append(GTK_BOX(row), a->rotation_combo);
    gtk_box_append(GTK_BOX(page), row);
    return page;
}

static GtkWidget *build_vis_page(App *a) {
    GtkWidget *page = ui_box(GTK_ORIENTATION_VERTICAL, 7);
    gtk_widget_add_css_class(page, "panel-page");
    GtkWidget *top = ui_box(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(top), ui_label("Mode", NULL, 0.0f));
    a->vis_combo = gtk_drop_down_new_from_strings(VIS_LABELS);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(a->vis_combo), (guint)a->vis.mode);
    g_signal_connect(a->vis_combo, "notify::selected", G_CALLBACK(on_vis_changed), a);
    gtk_box_append(GTK_BOX(top), a->vis_combo);
    gtk_box_append(GTK_BOX(top), ui_label("Sensitivity", NULL, 0.0f));
    a->vis_sens = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.2, 3.0, 0.05);
    gtk_widget_add_css_class(a->vis_sens, "param-scale");
    gtk_range_set_value(GTK_RANGE(a->vis_sens), 1.0);
    gtk_scale_set_draw_value(GTK_SCALE(a->vis_sens), FALSE);
    gtk_widget_set_hexpand(a->vis_sens, TRUE);
    g_signal_connect(a->vis_sens, "value-changed", G_CALLBACK(on_sensitivity), a);
    gtk_box_append(GTK_BOX(top), a->vis_sens);
    gtk_box_append(GTK_BOX(page), top);
    gtk_box_append(GTK_BOX(page),
        ui_label("Visualizers render on the main stage behind the album art",
                 "hint-label", 0.0f));
    return page;
}

static void on_deck_tab(GtkToggleButton *b, gpointer data) {
    App *a = data;
    if (!gtk_toggle_button_get_active(b) || !a->deck_stack) return;
    const char *page = g_object_get_data(G_OBJECT(b), "page");
    if (page) gtk_stack_set_visible_child_name(GTK_STACK(a->deck_stack), page);
}

static GtkWidget *build_skin_page(App *a) {
    GtkWidget *page = ui_box(GTK_ORIENTATION_VERTICAL, 7);
    gtk_widget_add_css_class(page, "panel-page");
    
    GtkWidget *info = ui_label("Select a skin to change the appearance", "hint-label", 0.0f);
    gtk_box_append(GTK_BOX(page), info);
    
    GtkWidget *combo = gtk_drop_down_new_from_strings(SKIN_NAMES);
    a->skin_combo = combo;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(combo), (guint)a->skin_id);
    g_signal_connect(combo, "notify::selected", G_CALLBACK(on_skin), a);
    gtk_box_append(GTK_BOX(page), combo);
    
    GtkWidget *hint = ui_label("Skins are loaded from ~/.local/share/nava-studio/", "hint-label", 0.0f);
    gtk_box_append(GTK_BOX(page), hint);
    
    return page;
}

static GtkWidget *build_deck(App *a) {
    GtkWidget *card = ui_box(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(card, "deck-card");
    GtkWidget *head = ui_box(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *tb = ui_box(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(tb, TRUE);
    gtk_box_append(GTK_BOX(tb), ui_label("Effects studio", "deck-title", 0.0f));
    gtk_box_append(GTK_BOX(tb), ui_label("Equalizer, audio FX, color & light, visualizer",
                                          "deck-sub", 0.0f));
    gtk_box_append(GTK_BOX(head), tb);
    GtkWidget *stack = gtk_stack_new();
    a->deck_stack = stack;
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(stack), 180);
    gtk_stack_set_hhomogeneous(GTK_STACK(stack), FALSE);
    gtk_stack_set_vhomogeneous(GTK_STACK(stack), FALSE);

    static const char *PAGES[] = {"eq", "audio", "video", "vis"};
    static const char *LABELS[] = {"Equalizer", "Audio FX", "Video", "Visualizer"};
    GtkWidget *tabs = ui_box(GTK_ORIENTATION_HORIZONTAL, 9);
    gtk_widget_add_css_class(tabs, "deck-tabs");
    gtk_widget_set_halign(tabs, GTK_ALIGN_END);
    gtk_widget_set_valign(tabs, GTK_ALIGN_CENTER);
    GtkWidget *first = NULL;
    for (int i = 0; i < 4; i++) {
        GtkWidget *b = gtk_toggle_button_new_with_label(LABELS[i]);
        gtk_widget_add_css_class(b, "deck-tab");
        g_object_set_data(G_OBJECT(b), "page", (gpointer)PAGES[i]);
        if (!first) {
            first = b;
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), TRUE);
        } else {
            gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(b),
                                        GTK_TOGGLE_BUTTON(first));
        }
        g_signal_connect(b, "toggled", G_CALLBACK(on_deck_tab), a);
        gtk_box_append(GTK_BOX(tabs), b);
    }
    gtk_box_append(GTK_BOX(head), tabs);
    GtkWidget *close = ui_icon_button("window-close-symbolic", "Close settings",
                                      G_CALLBACK(on_close_settings), a, "icon-button");
    gtk_widget_set_valign(close, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(head), close);
    gtk_box_append(GTK_BOX(card), head);
    gtk_stack_add_titled(GTK_STACK(stack), build_eq_page(a), "eq", "Equalizer");
    gtk_stack_add_titled(GTK_STACK(stack), build_audio_page(a), "audio", "Audio FX");
    gtk_stack_add_titled(GTK_STACK(stack), build_video_page(a), "video", "Video");
    gtk_stack_add_titled(GTK_STACK(stack), build_vis_page(a), "vis", "Visualizer");
    gtk_stack_add_titled(GTK_STACK(stack), build_skin_page(a), "skin", "Skin");
    gtk_box_append(GTK_BOX(card), stack);
    return card;
}

static GtkWidget *build_status_bar(App *a) {
    GtkWidget *bar = ui_box(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(bar, "status-bar");
    a->status_label = ui_label("Ready — drop a file or folder", "status", 0.0f);
    gtk_widget_set_hexpand(a->status_label, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(a->status_label), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(bar), a->status_label);
    a->engine_pill = NULL;
    return bar;
}

/* GDK4 exposes monitors as a GListModel; the first one is the primary. */
static gboolean primary_monitor_geometry(GdkRectangle *geo) {
    GdkDisplay *dpy = gdk_display_get_default();
    if (!dpy) return FALSE;
    GListModel *mons = gdk_display_get_monitors(dpy);
    if (!mons || g_list_model_get_n_items(mons) == 0) return FALSE;
    GdkMonitor *mon = g_list_model_get_item(mons, 0);
    if (!mon) return FALSE;
    gdk_monitor_get_geometry(mon, geo);
    g_object_unref(mon);
    return TRUE;
}

static void build_window(App *a, GtkApplication *gtk_app) {
    a->window = gtk_application_window_new(gtk_app);
    /* The stylesheet targets window.nava-window; without this class the
     * black fullscreen background and the base palette never apply. */
    gtk_widget_add_css_class(a->window, "nava-window");
    gtk_window_set_title(GTK_WINDOW(a->window), "Nava Studio");
    /* Cap the initial size to the monitor so a small screen never gets a
     * window larger than the work area (the deck used to force growth). */
    int want_w = 1360, want_h = a->snapshot_mode ? 1240 : 880;
    GdkRectangle geo;
    if (primary_monitor_geometry(&geo)) {
        if (geo.width > 0)  want_w = MIN(want_w, geo.width  * 92 / 100);
        if (geo.height > 0) want_h = MIN(want_h, geo.height * 88 / 100);
    }
    gtk_window_set_default_size(GTK_WINDOW(a->window), want_w, want_h);
    a->header = NULL;

    GtkWidget *shell = ui_box(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(shell, "app-shell");
    a->root = shell;

    GtkWidget *main_box = ui_box(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_vexpand(main_box, TRUE);
    GtkWidget *content = ui_box(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(content, "content");
    gtk_widget_set_hexpand(content, TRUE);
    gtk_widget_set_vexpand(content, TRUE);
    gtk_widget_set_margin_top(content, 13);
    gtk_widget_set_margin_bottom(content, 7);
    gtk_widget_set_margin_start(content, 16);
    gtk_widget_set_margin_end(content, 16);
    a->content = content;
    a->now_header = build_now_header(a);
    gtk_box_append(GTK_BOX(content), a->now_header);
    gtk_box_append(GTK_BOX(content), build_stage(a));
    a->transport = build_transport(a);
    gtk_box_append(GTK_BOX(content), a->transport);
    gtk_box_append(GTK_BOX(shell), main_box);
    a->status_bar = build_status_bar(a);
    gtk_box_append(GTK_BOX(shell), a->status_bar);
    /* The playlist drawer is attached to this overlay rather than to the
     * stage: in windowed mode it must float over the whole application
     * window (header, video area, transport, status bar), not just over the
     * video picture.  In fullscreen the stage already fills the window, so
     * the same drawer still slides in from the very edge. */
    GtkWidget *root_overlay = gtk_overlay_new();
    gtk_widget_set_hexpand(root_overlay, TRUE);
    gtk_widget_set_vexpand(root_overlay, TRUE);
    gtk_overlay_set_child(GTK_OVERLAY(root_overlay), shell);
    a->root_overlay = root_overlay;
    gtk_window_set_child(GTK_WINDOW(a->window), root_overlay);

    /* The effects OSD floats over the stage; the playlist drawer is attached
     * to the window-level overlay further down so it covers the whole app. */
    GtkWidget *overlay = gtk_overlay_new();
    gtk_widget_set_hexpand(overlay, TRUE);
    gtk_widget_set_vexpand(overlay, TRUE);
    /* Re-parent the stage under the overlay: detach it from the content box
     * first, otherwise GTK refuses to adopt a parented widget. */
    /* Hold a reference across the reparent: gtk_box_remove() drops the
     * parent's only reference, so without this the stage would be finalized
     * before the overlay could adopt it. */
    g_object_ref(a->stage);
    gtk_box_remove(GTK_BOX(content), a->stage);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), a->stage);
    g_object_unref(a->stage);
    a->stage_overlay = overlay;

    /* Settings OSD: a translucent card floating over the stage, so the video
     * or visualizer playing behind it stays visible. */
    a->deck = build_deck(a);
    gtk_widget_add_css_class(a->deck, "settings-osd");
    gtk_widget_set_halign(a->deck, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(a->deck, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(a->deck, 24);
    gtk_widget_set_margin_end(a->deck, 24);
    gtk_widget_set_margin_bottom(a->deck, 24);
    gtk_widget_set_margin_top(a->deck, 24);
    gtk_widget_set_hexpand(a->deck, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), a->deck);
    gtk_overlay_set_measure_overlay(GTK_OVERLAY(overlay), a->deck, FALSE);

    /* Playlist drawer: slides in from the left when the playlist button is
     * clicked and retracts once the pointer leaves it. */
    a->sidebar = build_sidebar(a);
    gtk_widget_add_css_class(a->sidebar, "drawer-card");
    GtkWidget *revealer = gtk_revealer_new();
    a->drawer = revealer;
    gtk_widget_add_css_class(revealer, "drawer");
    gtk_revealer_set_transition_type(GTK_REVEALER(revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_RIGHT);
    gtk_revealer_set_transition_duration(GTK_REVEALER(revealer), 220);
    gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), FALSE);
    gtk_widget_set_can_target(revealer, FALSE);
    gtk_revealer_set_child(GTK_REVEALER(revealer), a->sidebar);
    gtk_widget_set_halign(revealer, GTK_ALIGN_START);
    gtk_widget_set_valign(revealer, GTK_ALIGN_FILL);
    gtk_widget_set_margin_top(revealer, 14);
    gtk_widget_set_margin_bottom(revealer, 14);
    gtk_widget_set_margin_start(revealer, 14);
    GtkWidget *drawer_host = a->root_overlay ? a->root_overlay : overlay;
    gtk_overlay_add_overlay(GTK_OVERLAY(drawer_host), revealer);
    gtk_overlay_set_measure_overlay(GTK_OVERLAY(drawer_host), revealer, FALSE);

    GtkEventController *drawer_motion = gtk_event_controller_motion_new();
    g_signal_connect(drawer_motion, "enter", G_CALLBACK(drawer_enter_cb), a);
    g_signal_connect(drawer_motion, "leave", G_CALLBACK(drawer_leave_cb), a);
    gtk_widget_add_controller(revealer, drawer_motion);


    /* Keep the player in its original slot: header, stage, transport. */
    gtk_box_insert_child_after(GTK_BOX(content), overlay, a->now_header);
    gtk_box_append(GTK_BOX(main_box), content);

    install_dnd(a, a->window);
    install_dnd(a, a->stage);
    install_dnd(a, a->sidebar);
    install_dnd(a, a->track_list);

    g_signal_connect(a->sidebar_btn, "toggled",
                     G_CALLBACK(on_sidebar_toggle), a);
    g_signal_connect(a->effects_btn, "toggled",
                     G_CALLBACK(on_effects_toggle), a);
    g_signal_connect(a->window, "notify::fullscreened",
                     G_CALLBACK(on_window_state), a);

    /* One right-click menu for the whole app.  CAPTURE phase so the stage,
     * the visualizer drawing area, list rows and controls cannot swallow it. */
    GtkGesture *rc = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rc), GDK_BUTTON_SECONDARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(rc),
                                               GTK_PHASE_CAPTURE);
    g_signal_connect(rc, "released", G_CALLBACK(on_stage_right_click), a);
    gtk_widget_add_controller(a->window, GTK_EVENT_CONTROLLER(rc));

    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), a);
    gtk_widget_add_controller(a->window, motion);

    /* CAPTURE phase: buttons, sliders and the list would otherwise consume
     * Space/arrows before the window-level shortcut handler ever sees them. */
    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), a);
    gtk_widget_add_controller(a->window, keys);
}

/* -------------------------------------------------------------- selftest -- */
static gboolean selftest_step(gpointer data) {
    static int step = 0;
    App *a = data;
    static int fails = 0;
    switch (step++) {
    case 0:
        /* Fullscreen is asynchronous (and there may be no window manager),
         * so only assert that the request path does not crash or recurse. */
        set_fullscreen(a, TRUE);
        if (!a->fullscreen) { g_printerr("selftest: fullscreen flag not set\n"); fails++; }
        break;
    case 1:
        /* The window should be fullscreen by now (async on Wayland).  When it
         * is, the stage must cover the whole window: any layout space taken by
         * the header/status bar or content margins shows up as black bands. */
        if (gtk_window_is_fullscreen(GTK_WINDOW(a->window))) {
            if (gtk_widget_get_parent(a->transport) != a->stage ||
                gtk_widget_get_parent(a->now_header) != a->stage) {
                g_printerr("selftest: OSD not overlaid in fullscreen\n"); fails++;
            }
            int ww = gtk_widget_get_width(a->window);
            int wh = gtk_widget_get_height(a->window);
            int sw = gtk_widget_get_width(a->stage);
            int sh = gtk_widget_get_height(a->stage);
            if (sw != ww || sh != wh) {
                g_printerr("selftest: stage %dx%d not edge-to-edge with window %dx%d\n",
                           sw, sh, ww, wh);
                fails++;
            }
        }
        set_fullscreen(a, FALSE);
        if (a->fullscreen) { g_printerr("selftest: exit fullscreen failed\n"); fails++; }
        break;
    case 2:
        set_drawer(a, TRUE);
        if (!a->sidebar_on) {
            g_printerr("selftest: drawer did not open\n"); fails++;
        }
        if (gtk_widget_get_parent(a->sidebar) == a->content) {
            g_printerr("selftest: playlist still docked beside the player\n"); fails++;
        }
        if (gtk_widget_get_parent(a->sidebar) != a->drawer) {
            g_printerr("selftest: playlist is not inside the drawer\n"); fails++;
        }
        if (gtk_widget_get_parent(a->drawer) != a->stage_overlay) {
            g_printerr("selftest: drawer is not overlaid on the stage\n"); fails++;
        }
        set_drawer(a, FALSE);
        if (a->sidebar_on) {
            g_printerr("selftest: drawer did not close\n"); fails++;
        }
        /* Pointer inside the drawer cancels the pending auto-hide; leaving it
         * schedules one, and the main loop runs it before the next step. */
        set_drawer(a, TRUE);
        drawer_leave_cb(NULL, a);
        if (a->drawer_timer == 0) {
            g_printerr("selftest: leaving the drawer did not arm auto-hide\n"); fails++;
        }
        drawer_enter_cb(NULL, 0, 0, a);
        if (a->drawer_timer != 0) {
            g_printerr("selftest: hovering the drawer did not cancel auto-hide\n"); fails++;
        }
        drawer_leave_cb(NULL, a);
        break;
    case 3: {
        if (a->sidebar_on) {
            g_printerr("selftest: drawer stayed open after the pointer left\n"); fails++;
        }
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->effects_btn), FALSE);
        if (gtk_widget_get_visible(a->deck)) { g_printerr("selftest: effects hide failed\n"); fails++; }
        int w0 = gtk_widget_get_width(a->window), h0 = gtk_widget_get_height(a->window);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->effects_btn), TRUE);
        if (!gtk_widget_get_visible(a->deck)) { g_printerr("selftest: effects show failed\n"); fails++; }
        /* Showing the effects studio must not balloon the window. */
        int w1 = gtk_widget_get_width(a->window), h1 = gtk_widget_get_height(a->window);
        GdkRectangle geo;
        if (primary_monitor_geometry(&geo) && geo.height > 0) {
            if (h1 > geo.height) {
                g_printerr("selftest: window %dx%d exceeds monitor height %d\n",
                           w1, h1, geo.height);
                fails++;
            }
        }
        if (h0 > 0 && h1 - h0 > 200) {
            g_printerr("selftest: effects grew the window by %d px\n", h1 - h0);
            fails++;
        }
        if (w0 > 0 && w1 - w0 > 200) {
            g_printerr("selftest: effects widened the window by %d px\n", w1 - w0);
            fails++;
        }
        break;
    }
    case 4:
        a->visualizer_on = !a->visualizer_on;
        apply_stage_layout(a);
        /* Verify pause: on_play toggles a playing engine to paused. */
        if (engine_active(a->engine)) {
            on_play(NULL, a);
            if (engine_playing(a->engine)) { g_printerr("selftest: pause failed\n"); fails++; }
        }
        break;
    case 5:
        /* Verify resume from the paused state established in the previous step. */
        if (engine_active(a->engine)) {
            on_play(NULL, a);
            if (!engine_playing(a->engine)) { g_printerr("selftest: resume failed\n"); fails++; }
        }
        break;
    case 6:
        gtk_drop_down_set_selected(GTK_DROP_DOWN(a->speed_combo), 5);
        break;
    case 7:
        gtk_drop_down_set_selected(GTK_DROP_DOWN(a->speed_combo), 0);
        break;
    case 8:
        gtk_drop_down_set_selected(GTK_DROP_DOWN(a->speed_combo), 2);
        break;
    case 9:
        /* Start from a known state: the persisted config may have left
         * repeat at any of the three modes. */
        a->repeat = REPEAT_OFF;
        update_repeat_button(a);
        on_repeat(NULL, a);
        on_repeat(NULL, a);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->shuffle_button), TRUE);
        if (!a->shuffle) { g_printerr("selftest: shuffle failed\n"); fails++; }
        break;
    case 10: {
        const char *rl = gtk_label_get_text(GTK_LABEL(a->repeat_label));
        const char *sl = gtk_label_get_text(GTK_LABEL(a->shuffle_label));
        if (a->repeat != REPEAT_ONE || g_strcmp0(rl, "One") != 0) {
            g_printerr("selftest: repeat caption '%s'\n", rl); fails++;
        }
        if (!a->shuffle || g_strcmp0(sl, "On") != 0) {
            g_printerr("selftest: shuffle caption '%s'\n", sl); fails++;
        }
        break;
    }
    case 11:
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->shuffle_button), FALSE);
        if (g_strcmp0(gtk_label_get_text(GTK_LABEL(a->shuffle_label)), "Off") != 0) {
            g_printerr("selftest: shuffle off caption\n"); fails++;
        }
        /* Drive auto-hide directly and verify in the same step: a synthetic
         * motion event arriving between timer ticks would legitimately
         * reveal the controls again. */
        a->fullscreen = TRUE;
        hide_controls_cb(a);
        if (!a->chrome_hidden || gtk_widget_get_opacity(a->transport) > 0.01) {
            g_printerr("selftest: controls did not auto-hide\n"); fails++;
        if (!gtk_widget_get_cursor(a->window)) {
            g_printerr("selftest: pointer not hidden with the OSD\n"); fails++;
        }
        }
        reveal_controls(a);
        if (gtk_widget_get_opacity(a->transport) < 0.99 || a->chrome_hidden) {
            g_printerr("selftest: controls did not reappear\n"); fails++;
        if (gtk_widget_get_cursor(a->window) != NULL) {
            g_printerr("selftest: pointer not restored on reveal\n"); fails++;
        }
        }
        break;
    case 12: {
        /* Playlist selection must follow the playing track, not snap back to
         * the first row when the rows are rebuilt. */
        if (a->track_count >= 2) {
            play_index(a, a->track_count - 1);
            GtkListBoxRow *sel = gtk_list_box_get_selected_row(GTK_LIST_BOX(a->track_list));
            int want = a->track_count - 1;
            int got = sel ? GPOINTER_TO_INT(g_object_get_data(G_OBJECT(sel), "index")) : -1;
            if (got != want) {
                g_printerr("selftest: playlist selection %d != playing %d\n", got, want);
                fails++;
            }
            if (sel && !gtk_widget_has_css_class(GTK_WIDGET(sel), "track-playing")) {
                g_printerr("selftest: playing row lost the track-playing class\n");
                fails++;
            }
        }
        break;
    }
    case 13:
        a->fullscreen = FALSE;
        stop_autohide(a);
        if (gtk_widget_get_opacity(a->transport) < 0.99 || a->chrome_hidden) {
            g_printerr("selftest: chrome not restored after exit\n"); fails++;
        }
        /* The empty-state overlay has to follow the model: hidden while the
         * playlist has tracks, back after Clear without any restart. */
        if (a->track_count > 0 && gtk_widget_get_visible(a->empty_label)) {
            g_printerr("selftest: empty-state visible with %d tracks\n", a->track_count);
            fails++;
        }
        clear_playlist(a, TRUE);
        if (a->track_count != 0 || !gtk_widget_get_visible(a->empty_label)) {
            g_printerr("selftest: clear did not restore the empty stage\n");
            fails++;
        }
        g_printerr("SELFTEST_%s fails=%d sidebar=%d effects=%d playing=%d repeat=%s shuffle=%s\n",
                   fails ? "FAIL" : "OK", fails,
                   gtk_widget_get_visible(a->sidebar),
                   gtk_widget_get_visible(a->deck), engine_playing(a->engine),
                   gtk_label_get_text(GTK_LABEL(a->repeat_label)),
                   gtk_label_get_text(GTK_LABEL(a->shuffle_label)));
        g_application_quit(g_application_get_default());
        return G_SOURCE_REMOVE;
    default:
        g_application_quit(g_application_get_default());
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

/* -------------------------------------------------------------- snapshot -- */
static gboolean snapshot_cb(gpointer data) {
    App *a = data;
    GtkWidget *target = a->root_overlay ? a->root_overlay
                                        : (a->root ? a->root : a->window);
    if (!target) { g_application_quit(g_application_get_default()); return G_SOURCE_REMOVE; }
    if (!gtk_widget_get_mapped(target) || gtk_widget_get_width(target) < 100 ||
        (a->engine && engine_has_video(a->engine) && !a->texture)) {
        if (++a->snapshot_tries < 120) return G_SOURCE_CONTINUE;
        g_printerr("snapshot: widget never became ready\n");
        g_application_quit(g_application_get_default());
        return G_SOURCE_REMOVE;
    }
    int w = gtk_widget_get_width(target), h = gtk_widget_get_height(target);
    GtkSnapshot *snapshot = gtk_snapshot_new();
    GtkWidget *child = (target == a->root || target == a->window)
                           ? gtk_widget_get_first_child(a->window) : target;
    if (!child) child = target;
    GtkWidget *parent = gtk_widget_get_parent(child);
    if (parent) gtk_widget_snapshot_child(parent, child, snapshot);
    else gtk_widget_snapshot_child(child, gtk_widget_get_first_child(child), snapshot);
    GskRenderNode *node = gtk_snapshot_free_to_node(snapshot);
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(surface);
    cairo_set_source_rgb(cr, 0.027, 0.039, 0.070);
    cairo_paint(cr);
    if (node) gsk_render_node_draw(node, cr);
    cairo_destroy(cr);
    const char *out = g_getenv("NAVA_SNAPSHOT");
    if (!out || !*out) out = "nava-ui.png";
    cairo_status_t st = cairo_surface_write_to_png(surface, out);
    g_printerr("snapshot: %s (%s) %dx%d\n", out, cairo_status_to_string(st), w, h);
    cairo_surface_destroy(surface);
    if (node) gsk_render_node_unref(node);
    g_application_quit(g_application_get_default());
    return G_SOURCE_REMOVE;
}

/* -------------------------------------------------------------- activate -- */
/* Second launch of the same media file (`nava-studio song.flac` while an
 * instance is already running).  GApplication forwards the files here and the
 * running window appends + plays them. */
static void on_app_open(GtkApplication *app, GFile **files, gint n_files,
                        const gchar *hint, gpointer data) {
    (void)app; (void)hint;
    App *a = data;
    if (!a->window) {
        a->cli_start = -1;
        if (a->open_uri) { g_free(a->open_uri); a->open_uri = NULL; }
        on_activate(app, data);
    }
    /* Opening media from the file manager replaces the playlist instead of
     * appending to the one restored from the previous session. */
    clear_playlist(a, TRUE);
    int first = -1;
    for (gint i = 0; i < n_files; i++) {
        char *path = g_file_get_path(files[i]);
        if (!path) continue;
        char *abs = g_canonicalize_filename(path, NULL);
        if (g_file_test(abs, G_FILE_TEST_IS_DIR)) {
            int before = a->track_count;
            a->track_count += add_directory(a, abs);
            if (first < 0 && a->track_count > before) first = before;
        } else if (is_media(abs)) {
            int idx = add_track(a, abs);
            if (idx >= 0 && first < 0) first = idx;
        }
        g_free(abs);
        g_free(path);
    }
    if (first >= 0) {
        refresh_list(a);
        play_index(a, first);
    }
    gtk_window_present(GTK_WINDOW(a->window));
#ifdef _WIN32
    GdkSurface *surf = gtk_native_get_surface(gtk_widget_get_native(GTK_WIDGET(a->window)));
    if (surf && GDK_IS_WIN32_SURFACE(surf)) {
        HWND hwnd = (HWND)gdk_win32_surface_get_handle(surf);
        if (hwnd) {
            BOOL dark = TRUE;
            DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
            int corner = 2; /* DWMWCP_ROUND */
            DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));
        }
    }
#endif
}

static void on_activate(GtkApplication *gtk_app, gpointer data) {
    App *a = data;
    static GtkCssProvider *css;
    /* A second launch of the app (or a file manager hand-off) activates the
     * running instance; rebuilding here would duplicate the whole window. */
    if (a->window) { gtk_window_present(GTK_WINDOW(a->window)); return; }

    /* Force GTK dark theme on all platforms */
    GtkSettings *gtk_settings = gtk_settings_get_default();
    if (gtk_settings) {
        g_object_set(gtk_settings, "gtk-application-prefer-dark-theme", TRUE, NULL);
    }

    /* Register bundled symbolic SVG icons from embedded GResource */
    GdkDisplay *display = gdk_display_get_default();
    if (display) {
        GtkIconTheme *icon_theme = gtk_icon_theme_get_for_display(display);
        gtk_icon_theme_add_resource_path(icon_theme, "/com/shirazelectronic/NavaStudio/icons");
    }

    if (!css) {
        css = gtk_css_provider_new();
        gtk_css_provider_load_from_resource(css, "/com/shirazelectronic/NavaStudio/style.css");
        gtk_style_context_add_provider_for_display(
            gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    build_window(a, gtk_app);
    restore_state(a);
    apply_skin(a, a->skin_id);
    if (a->skin_combo)
        gtk_drop_down_set_selected(GTK_DROP_DOWN(a->skin_combo), (guint)a->skin_id);
    /* Playlist and settings are on-demand OSDs now: both start hidden. */
    set_drawer(a, FALSE);
    if (a->deck)
        gtk_widget_set_visible(a->deck, a->show_effects && !a->mini && !a->fullscreen);
    if (a->mini) set_mini(a, TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->sidebar_btn), FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->effects_btn), a->show_effects);
    if (a->vis_sensitivity > 0) {
        a->vis.sensitivity = (float)(0.2 + a->vis_sensitivity * 0.05);
        gtk_range_set_value(GTK_RANGE(a->vis_sens), a->vis.sensitivity);
    }
    if (a->engine) {
        a->vis.mode = a->vis.mode < 0 || a->vis.mode >= NAVA_VIS_COUNT ? 2 : a->vis.mode;
    }
    gtk_drop_down_set_selected(GTK_DROP_DOWN(a->vis_combo), (guint)a->vis.mode);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(a->rotation_combo), (guint)a->rotation);
    int speed_idx = 2;
    for (int i = 0; i < 6; i++) if (fabs(SPEEDS[i] - a->rate) < 0.01) speed_idx = i;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(a->speed_combo), (guint)speed_idx);
    gtk_range_set_value(GTK_RANGE(a->vol_scale), a->volume);
    /* build_audio_page() runs before restore_state(), so push the persisted
     * crossfade/fade values back into the controls here. */
    if (a->xf_check)
        gtk_check_button_set_active(GTK_CHECK_BUTTON(a->xf_check), a->crossfade_on);
    if (a->xf_scale)
        gtk_range_set_value(GTK_RANGE(a->xf_scale), a->crossfade_ms / 1000.0);
    if (a->fade_scale)
        gtk_range_set_value(GTK_RANGE(a->fade_scale), a->fade_ms);
    if (a->muted) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->mute_button), TRUE);
    if (a->shuffle) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->shuffle_button), TRUE);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(a->eq_check), a->eq_enabled);
    update_repeat_button(a);
    update_shuffle_ui(a);
    refresh_list(a);
    apply_stage_layout(a);
    gtk_widget_set_visible(a->empty_label, a->track_count == 0);

    a->tick_id = g_timeout_add(120, tick_cb, a);
    a->visuals_id = g_timeout_add(33, visuals_cb, a);
    a->frame_id = g_timeout_add(10, frame_cb, a);
    a->persist_id = g_timeout_add_seconds(4, persist_cb, a);

    if (a->engine) apply_engine_settings(a);
    if (a->cli_start >= 0 && a->cli_start < a->track_count) play_index(a, a->cli_start);
    /* `xdg-open`/file manager double-click: start the requested file right
     * away instead of whatever the saved session last played. */
    if (a->open_uri) {
        int idx = add_track(a, a->open_uri);
        if (idx >= 0) {
            refresh_list(a);
            play_index(a, idx);
        }
    }
    gtk_window_present(GTK_WINDOW(a->window));
#ifdef _WIN32
    GdkSurface *surf = gtk_native_get_surface(gtk_widget_get_native(GTK_WIDGET(a->window)));
    if (surf && GDK_IS_WIN32_SURFACE(surf)) {
        HWND hwnd = (HWND)gdk_win32_surface_get_handle(surf);
        if (hwnd) {
            BOOL dark = TRUE;
            DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
            int corner = 2; /* DWMWCP_ROUND */
            DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));
        }
    }
#endif
    /* Without an explicit focus target the toplevel may never see key events. */
    gtk_widget_set_focusable(a->stage, TRUE);
    gtk_widget_grab_focus(a->stage);
    if (!a->snapshot_mode) mpris_start(a);

    if (a->snapshot_mode) {
        if (g_getenv("NAVA_SNAPSHOT_DECK")) {
            set_drawer(a, FALSE);
            gtk_widget_set_visible(a->now_header, FALSE);
            gtk_widget_set_visible(a->stage, FALSE);
            gtk_widget_set_visible(a->transport, FALSE);
            gtk_widget_set_visible(a->status_bar, FALSE);
            gtk_widget_set_visible(a->deck, TRUE);
        }
        if (g_getenv("NAVA_SNAPSHOT_DRAWER")) set_drawer(a, TRUE);
        if (g_getenv("NAVA_SNAPSHOT_SETTINGS")) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->effects_btn), TRUE);
            a->show_effects = TRUE;
            gtk_widget_set_visible(a->deck, TRUE);
        }
        if (g_getenv("NAVA_SNAPSHOT_MINI")) set_mini(a, TRUE);
        if (g_getenv("NAVA_SNAPSHOT_FS")) {
            const char *d = g_getenv("NAVA_SNAPSHOT_DELAY");
            guint delay = d && *d ? (guint)g_ascii_strtoull(d, NULL, 10) : 1600;
            g_timeout_add(250, snap_fullscreen_cb, a);
            g_timeout_add(delay, snapshot_cb, a);
        } else {
            g_timeout_add(400, snapshot_cb, a);
        }
    }
    if (g_getenv("NAVA_SELFTEST")) g_timeout_add(900, selftest_step, a);
}

int nava_app_run(int argc, char **argv) {
    static App a;
    memset(&a, 0, sizeof(a));
    a.engine = engine_new();
    a.vis.mode = 2;
    a.vis.sensitivity = 1.0f;
    a.volume = 0.8;
    a.rate = 1.0;
    a.eq_enabled = TRUE;
    a.visualizer_on = TRUE;
    a.sidebar_on = TRUE;
    a.show_effects = FALSE;
    a.crossfade_on = TRUE;
    a.crossfade_ms = 2000.0;
    a.fade_ms = 120.0;
    a.current = -1;
    a.cli_start = -1;
    for (int i = 0; i < EQ_BANDS; i++) a.eq_scales[i] = NULL;
    int cli_tracks = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        char *abs = g_canonicalize_filename(argv[i], NULL);
        if (g_file_test(abs, G_FILE_TEST_IS_DIR)) {
            int before = a.track_count;
            a.track_count += add_directory(&a, abs);
            if (a.cli_start < 0 && a.track_count > before) a.cli_start = before;
            (void)before;
        } else {
            int before = a.track_count;
            if (add_track(&a, abs) >= 0) {
                if (a.cli_start < 0) a.cli_start = before;
                cli_tracks += a.track_count - before;
            }
        }
        g_free(abs);
    }
    (void)cli_tracks;
    /* A desktop-environment launch may hand us `file:///...` URIs instead of
     * plain paths (MIME handlers commonly do).  Convert them up front and play
     * the first one, so double-clicking any registered media file works. */
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "file://", 7) != 0) continue;
        char *path = g_filename_from_uri(argv[i], NULL, NULL);
        if (!path) continue;
        char *abs = g_canonicalize_filename(path, NULL);
        int before = a.track_count;
        if (g_file_test(abs, G_FILE_TEST_IS_DIR)) {
            a.track_count += add_directory(&a, abs);
        } else if (is_media(abs) && add_track(&a, abs) >= 0) {
            /* fall through */
        }
        if (a.cli_start < 0 && a.track_count > before) a.cli_start = before;
        if (a.open_uri == NULL) a.open_uri = g_strdup(abs);
        g_free(abs);
        g_free(path);
    }
    const char *snap = g_getenv("NAVA_SNAPSHOT");
    a.snapshot_mode = snap != NULL;
    const char *selftest = g_getenv("NAVA_SELFTEST");
    GApplicationFlags flags = G_APPLICATION_HANDLES_OPEN;
    if (snap || selftest) flags |= G_APPLICATION_NON_UNIQUE;
    GtkApplication *gtk_app = gtk_application_new(APP_ID, flags);
    g_signal_connect(gtk_app, "activate", G_CALLBACK(on_activate), &a);
    g_signal_connect(gtk_app, "open", G_CALLBACK(on_app_open), &a);
    int status = g_application_run(G_APPLICATION(gtk_app), argc, argv);
    mpris_stop(&a);
    if (a.tick_id) g_source_remove(a.tick_id);
    if (a.visuals_id) g_source_remove(a.visuals_id);
    if (a.frame_id) g_source_remove(a.frame_id);
    if (a.persist_id) g_source_remove(a.persist_id);
    save_state(&a);
    engine_free(a.engine);
    if (a.texture) g_object_unref(a.texture);
    free(a.cover_pixels);
    if (a.cover_surface) cairo_surface_destroy(a.cover_surface);
    g_object_unref(gtk_app);
    return status;
}
