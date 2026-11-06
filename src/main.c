#include "app.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#endif

int main(int argc, char **argv) {
#ifdef _WIN32
    /* Request 1ms timer resolution on Windows to avoid the 15.6ms jitter */
    timeBeginPeriod(1);

    /* Enable hardware-accelerated OpenGL renderer for GTK4 GSK on Windows */
    if (!getenv("GSK_RENDERER")) {
        putenv("GSK_RENDERER=gl");
    }
    if (!getenv("GDK_WIN32_USE_OPENGL")) {
        putenv("GDK_WIN32_USE_OPENGL=1");
    }

    /* Locate bundled GLib schemas relative to exe */
    if (!getenv("GSETTINGS_SCHEMA_DIR")) {
        char exe_path[MAX_PATH];
        if (GetModuleFileNameA(NULL, exe_path, MAX_PATH)) {
            char *last_slash = strrchr(exe_path, '\\');
            if (last_slash) *last_slash = '\0';
            static char schema_env[MAX_PATH + 64];
            snprintf(schema_env, sizeof(schema_env), "GSETTINGS_SCHEMA_DIR=%s\\share\\glib-2.0\\schemas", exe_path);
            putenv(schema_env);
            static char xdg_env[MAX_PATH + 32];
            snprintf(xdg_env, sizeof(xdg_env), "XDG_DATA_DIRS=%s\\share", exe_path);
            putenv(xdg_env);
            static char pixbuf_env[MAX_PATH + 64];
            snprintf(pixbuf_env, sizeof(pixbuf_env), "GDK_PIXBUF_MODULE_FILE=%s\\lib\\gdk-pixbuf-2.0\\2.10.0\\loaders.cache", exe_path);
            putenv(pixbuf_env);
        }
    }
#endif

    int ret = nava_app_run(argc, argv);

#ifdef _WIN32
    timeEndPeriod(1);
#endif
    return ret;
}
