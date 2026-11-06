#pragma once
#include <gtk/gtk.h>

static GtkWidget *ui_box(GtkOrientation o, int spacing) { return gtk_box_new(o, spacing); }
static GtkWidget *ui_label(const char *text, const char *css, float xalign) {
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), xalign);
    if (css) gtk_widget_add_css_class(l, css);
    return l;
}
static GtkWidget *ui_icon_button(const char *icon, const char *tip, GCallback cb,
                                 gpointer data, const char *css) {
    GtkWidget *b = gtk_button_new_from_icon_name(icon);
    if (css) gtk_widget_add_css_class(b, css);
    if (tip) gtk_widget_set_tooltip_text(b, tip);
    if (cb) g_signal_connect(b, "clicked", cb, data);
    return b;
}

static GtkWidget *ui_toggle_button(const char *icon, const char *tip, GCallback cb,
                                   gpointer data, const char *css) {
    GtkWidget *b = gtk_toggle_button_new();
    gtk_button_set_child(GTK_BUTTON(b), gtk_image_new_from_icon_name(icon));
    if (css) gtk_widget_add_css_class(b, css);
    if (tip) gtk_widget_set_tooltip_text(b, tip);
    if (cb) g_signal_connect(b, "clicked", cb, data);
    return b;
}
