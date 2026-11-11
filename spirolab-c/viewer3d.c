#include "viewer3d.h"
#include "gpu3d.h"
#include "spiro3d_io.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
    Sp3Config config, lifted;
    Sp3Curve curve;
    Sp3View view;
    GtkWidget *window, *canvas, *viewport, *gpu, *software, *grid, *status, *play, *mode, *spin,
        *arms, *hue, *range, *samples, *mm, *radius, *speed;
    GtkWidget *edit[16][5];
    gboolean syncing, dragging, panning, playing, can_lift, gpu_active;
    double progress, last_x, last_y;
    gint64 last_tick;
    guint timer;
} Sp3Window;
static void sync_widgets(Sp3Window *);
static void status(Sp3Window *w, const char *s) {
    gtk_label_set_text(GTK_LABEL(w->status), s);
}
static void failure(Sp3Window *w, GError *error) {
    status(w, error ? error->message : "3D operation failed");
    g_clear_error(&error);
}
static void queue(Sp3Window *w) {
    if (w->gpu_active)
        sp3_gpu_scene(w->gpu, &w->config, &w->curve, &w->view, w->progress, w->progress);
    else
        gtk_widget_queue_draw(w->software);
}
static void label_play(Sp3Window *w) {
    gtk_button_set_label(GTK_BUTTON(w->play), w->playing ? "Pause" : "Play");
}
static void summary(Sp3Window *w) {
    Sp3GpuStats stats = {0};
    sp3_gpu_stats(w->gpu, &stats);
    const char *backend =
        w->gpu_active
            ? (stats.ready ? (stats.hardware ? "GPU" : "OpenGL software") : "Starting OpenGL")
            : "Cairo software";
    char *s = g_strdup_printf(
        "%s%s%s · %zu arms · %zu samples · drag to orbit · Shift-drag to pan · wheel to zoom",
        backend, w->gpu_active && stats.ready ? " · " : "",
        w->gpu_active && stats.ready ? stats.renderer : "", w->config.arm_count, w->config.samples);
    status(w, s);
    g_free(s);
    if (stats.error[0])
        gtk_widget_set_tooltip_text(w->status, stats.error);
}
static void gpu_notice(GtkWidget *area, gpointer data) {
    Sp3Window *w = data;
    Sp3GpuStats stats = {0};
    sp3_gpu_stats(area, &stats);
    w->gpu_active = stats.ready && g_strcmp0(g_getenv("SPIROLAB_SOFTWARE_3D"), "1") != 0;
    w->canvas = w->gpu_active ? w->gpu : w->software;
    gtk_stack_set_visible_child_name(GTK_STACK(w->viewport), w->gpu_active ? "gpu" : "software");
    summary(w);
    queue(w);
}
static gboolean read_config(Sp3Window *w, Sp3Config *out) {
    Sp3Config c = w->config;
    for (size_t i = 0; i < c.arm_count; i++) {
        c.arms[i] = (Sp3Arm){gtk_spin_button_get_value(GTK_SPIN_BUTTON(w->edit[i][0])),
                             gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(w->edit[i][1])),
                             gtk_spin_button_get_value(GTK_SPIN_BUTTON(w->edit[i][2])),
                             gtk_spin_button_get_value(GTK_SPIN_BUTTON(w->edit[i][3])),
                             gtk_spin_button_get_value(GTK_SPIN_BUTTON(w->edit[i][4]))};
    }
    c.samples = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(w->samples));
    c.hue = gtk_spin_button_get_value(GTK_SPIN_BUTTON(w->hue));
    c.hue_range = gtk_spin_button_get_value(GTK_SPIN_BUTTON(w->range));
    c.size_mm = gtk_spin_button_get_value(GTK_SPIN_BUTTON(w->mm));
    c.tube_mm = gtk_spin_button_get_value(GTK_SPIN_BUTTON(w->radius));
    c.tube = gtk_combo_box_get_active(GTK_COMBO_BOX(w->mode));
    c.spin = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->spin));
    c.show_arms = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->arms));
    if (!sp3_valid(&c)) {
        status(w, "Use 1–16 arms with at least one nonzero length and valid settings.");
        return FALSE;
    }
    *out = c;
    return TRUE;
}
static gboolean replace(Sp3Window *w, Sp3Config c) {
    Sp3Curve curve = {0};
    if (!sp3_curve(&c, &curve)) {
        status(w, "Cannot allocate the 3D curve. The current drawing was kept.");
        return FALSE;
    }
    sp3_curve_free(&w->curve);
    w->curve = curve;
    if (w->gpu)
        sp3_gpu_geometry_changed(w->gpu);
    w->config = c;
    w->progress = 1;
    w->playing = FALSE;
    label_play(w);
    queue(w);
    summary(w);
    return TRUE;
}
static gboolean restore_design(Sp3Window *w, Sp3Config config, Sp3View view) {
    if (!replace(w, config))
        return FALSE;
    w->view = view;
    sync_widgets(w);
    queue(w);
    return TRUE;
}
static void apply(GtkWidget *widget, gpointer data) {
    (void)widget;
    Sp3Window *w = data;
    Sp3Config c;
    if (read_config(w, &c))
        replace(w, c);
}
static void style_changed(GtkWidget *widget, gpointer data) {
    (void)widget;
    Sp3Window *w = data;
    if (w->syncing)
        return;
    w->config.tube = gtk_combo_box_get_active(GTK_COMBO_BOX(w->mode));
    w->config.spin = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->spin));
    w->config.show_arms = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->arms));
    w->config.hue = gtk_spin_button_get_value(GTK_SPIN_BUTTON(w->hue));
    w->config.hue_range = gtk_spin_button_get_value(GTK_SPIN_BUTTON(w->range));
    w->config.size_mm = gtk_spin_button_get_value(GTK_SPIN_BUTTON(w->mm));
    w->config.tube_mm = gtk_spin_button_get_value(GTK_SPIN_BUTTON(w->radius));
    queue(w);
}
static gboolean draw(GtkWidget *canvas, cairo_t *cr, gpointer data) {
    Sp3Window *w = data;
    sp3_draw(cr, gtk_widget_get_allocated_width(canvas), gtk_widget_get_allocated_height(canvas),
             &w->config, &w->curve, &w->view, w->progress, w->progress);
    return FALSE;
}
static gboolean tick(gpointer data) {
    Sp3Window *w = data;
    gint64 now = g_get_monotonic_time();
    double dt = MIN(.1, (now - w->last_tick) / 1e6);
    w->last_tick = now;
    gboolean dirty = FALSE;
    if (w->config.spin && !w->dragging) {
        w->view.yaw = remainder(w->view.yaw + dt * .22, 6.283185307179586);
        dirty = TRUE;
    }
    if (w->playing) {
        w->progress = MIN(1., w->progress + dt * .1 * gtk_range_get_value(GTK_RANGE(w->speed)));
        if (w->progress >= 1) {
            w->playing = FALSE;
            label_play(w);
        }
        dirty = TRUE;
    }
    if (dirty)
        queue(w);
    return G_SOURCE_CONTINUE;
}
static gboolean frame_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
    (void)widget;
    (void)clock;
    return tick(data);
}
static void play(GtkWidget *b, gpointer data) {
    (void)b;
    Sp3Window *w = data;
    if (w->progress >= 1)
        w->progress = 0;
    w->playing = !w->playing;
    label_play(w);
}
static void replay(GtkWidget *b, gpointer data) {
    (void)b;
    Sp3Window *w = data;
    w->progress = 0;
    w->playing = TRUE;
    label_play(w);
    queue(w);
}
static void finish(GtkWidget *b, gpointer data) {
    (void)b;
    Sp3Window *w = data;
    w->progress = 1;
    w->playing = FALSE;
    label_play(w);
    queue(w);
}
static void fit(GtkWidget *b, gpointer data) {
    (void)b;
    Sp3Window *w = data;
    w->view.zoom = 1;
    w->view.pan_x = w->view.pan_y = 0;
    queue(w);
}
static gboolean button_event(GtkWidget *canvas, GdkEventButton *e, gpointer data) {
    Sp3Window *w = data;
    gtk_widget_grab_focus(canvas);
    if (e->type == GDK_BUTTON_PRESS) {
        w->dragging = TRUE;
        w->panning = e->button != 1 || (e->state & GDK_SHIFT_MASK);
        w->last_x = e->x;
        w->last_y = e->y;
        return TRUE;
    }
    w->dragging = FALSE;
    return FALSE;
}
static gboolean motion(GtkWidget *canvas, GdkEventMotion *e, gpointer data) {
    Sp3Window *w = data;
    if (!w->dragging)
        return FALSE;
    double dx = e->x - w->last_x, dy = e->y - w->last_y;
    w->last_x = e->x;
    w->last_y = e->y;
    if (w->panning) {
        double m = MAX(1, MIN(gtk_widget_get_allocated_width(canvas),
                              gtk_widget_get_allocated_height(canvas)));
        w->view.pan_x = CLAMP(w->view.pan_x + dx / m, -10., 10.);
        w->view.pan_y = CLAMP(w->view.pan_y + dy / m, -10., 10.);
    } else {
        w->view.yaw = remainder(w->view.yaw + dx * .008, 6.283185307179586);
        w->view.pitch = CLAMP(w->view.pitch + dy * .008, -1.55, 1.55);
    }
    queue(w);
    return TRUE;
}
static gboolean scroll_event(GtkWidget *canvas, GdkEventScroll *e, gpointer data) {
    Sp3Window *w = data;
    double dy = 0;
    if (e->direction == GDK_SCROLL_UP)
        dy = -1;
    else if (e->direction == GDK_SCROLL_DOWN)
        dy = 1;
    else {
        double dx;
        if (!gdk_event_get_scroll_deltas((GdkEvent *)e, &dx, &dy))
            return FALSE;
    }
    double old = w->view.zoom;
    w->view.zoom = CLAMP(old * pow(1.18, -dy), .15, 10.);
    double m = MAX(1, MIN(gtk_widget_get_allocated_width(canvas),
                          gtk_widget_get_allocated_height(canvas))),
           x = (e->x - gtk_widget_get_allocated_width(canvas) / 2.) / m,
           y = (e->y - gtk_widget_get_allocated_height(canvas) / 2.) / m, r = w->view.zoom / old;
    w->view.pan_x = CLAMP(x - (x - w->view.pan_x) * r, -10., 10.);
    w->view.pan_y = CLAMP(y - (y - w->view.pan_y) * r, -10., 10.);
    queue(w);
    return TRUE;
}
static gboolean key(GtkWidget *widget, GdkEventKey *e, gpointer data) {
    (void)widget;
    Sp3Window *w = data;
    GtkWidget *f = gtk_window_get_focus(GTK_WINDOW(w->window));
    if (GTK_IS_EDITABLE(f))
        return FALSE;
    switch (e->keyval) {
    case GDK_KEY_space:
        play(NULL, w);
        break;
    case GDK_KEY_r:
    case GDK_KEY_R:
        replay(NULL, w);
        break;
    case GDK_KEY_f:
    case GDK_KEY_F:
        finish(NULL, w);
        break;
    case GDK_KEY_0:
        fit(NULL, w);
        break;
    default:
        return FALSE;
    }
    return TRUE;
}
static void add_arm(GtkWidget *b, gpointer data) {
    (void)b;
    Sp3Window *w = data;
    Sp3Config c;
    if (!read_config(w, &c))
        return;
    if (c.arm_count == 16) {
        status(w, "The chain already has 16 arms.");
        return;
    }
    c.arms[c.arm_count++] = (Sp3Arm){.2, 3, 0, 0, 0};
    if (replace(w, c))
        sync_widgets(w);
}
static void remove_arm(GtkWidget *b, gpointer data) {
    (void)b;
    Sp3Window *w = data;
    Sp3Config c;
    if (!read_config(w, &c))
        return;
    if (c.arm_count == 1)
        return;
    c.arm_count--;
    if (!sp3_valid(&c)) {
        status(w, "Give a remaining arm a nonzero length first.");
        return;
    }
    if (replace(w, c))
        sync_widgets(w);
}
static void reset(GtkWidget *b, gpointer data) {
    (void)b;
    Sp3Window *w = data;
    restore_design(w, sp3_defaults(), sp3_default_view());
}
static void lift(GtkWidget *b, gpointer data) {
    (void)b;
    Sp3Window *w = data;
    if (w->can_lift && replace(w, w->lifted)) {
        sync_widgets(w);
        status(w, "Lifted the 2D curve's strongest Fourier terms; the second arm tilts out of the "
                  "plane.");
    }
}
static void randomize(GtkWidget *b, gpointer data) {
    (void)b;
    Sp3Window *w = data;
    Sp3Config c = sp3_defaults();
    for (int i = 0; i < 3; i++)
        c.arms[i] = (Sp3Arm){i ? g_random_double_range(.15, .9) : 1,
                             g_random_int_range(0, 2) ? g_random_int_range(1 + i * 3, 6 + i * 7)
                                                      : -g_random_int_range(1 + i * 3, 6 + i * 7),
                             g_random_int_range(0, 181), g_random_int_range(0, 361),
                             g_random_int_range(0, 361)};
    c.hue = g_random_int_range(0, 361);
    if (replace(w, c))
        sync_widgets(w);
}
static char *choose(Sp3Window *w, const char *title, gboolean save, const char *name) {
    GtkWidget *d = gtk_file_chooser_dialog_new(
        title, GTK_WINDOW(w->window),
        save ? GTK_FILE_CHOOSER_ACTION_SAVE : GTK_FILE_CHOOSER_ACTION_OPEN, "Cancel",
        GTK_RESPONSE_CANCEL, save ? "Save" : "Open", GTK_RESPONSE_ACCEPT, NULL);
    if (save) {
        gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(d), TRUE);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(d), name);
    }
    char *file = NULL;
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT)
        file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));
    gtk_widget_destroy(d);
    return file;
}
static void open_design(GtkWidget *b, gpointer data) {
    (void)b;
    Sp3Window *w = data;
    char *file = choose(w, "Open 3D design", FALSE, NULL);
    if (!file)
        return;
    GError *e = NULL;
    Sp3Config c;
    Sp3View v;
    if (!sp3_load(file, &c, &v, &e))
        failure(w, e);
    else
        restore_design(w, c, v);
    g_free(file);
}
static void save_design(GtkWidget *b, gpointer data) {
    (void)b;
    Sp3Window *w = data;
    Sp3Config c;
    if (!read_config(w, &c) || !replace(w, c))
        return;
    char *file = choose(w, "Save 3D design", TRUE, "drawing.spiro3d");
    if (!file)
        return;
    GError *e = NULL;
    if (!sp3_save(file, &w->config, &w->view, &e))
        failure(w, e);
    else
        status(w, "3D arms, appearance and camera saved.");
    g_free(file);
}
static void png(GtkWidget *b, gpointer data) {
    (void)b;
    Sp3Window *w = data;
    Sp3Config c;
    if (!read_config(w, &c) || !replace(w, c))
        return;
    char *file = choose(w, "Export 3D image", TRUE, "spiro-3d.png");
    if (!file)
        return;
    GError *e = NULL;
    if (!sp3_png(file, &w->config, &w->curve, &w->view, &e))
        failure(w, e);
    else
        status(w, "2048-pixel PNG saved with the current camera view.");
    g_free(file);
}
static void stl(GtkWidget *b, gpointer data) {
    (void)b;
    Sp3Window *w = data;
    Sp3Config c;
    if (!read_config(w, &c) || !replace(w, c))
        return;
    char *file = choose(w, "Export closed tube", TRUE, "spiro-3d.stl");
    if (!file)
        return;
    GError *e = NULL;
    if (!sp3_export_stl(file, &w->config, &e))
        failure(w, e);
    else {
        char *s = g_strdup_printf("Closed STL saved · curve %.1f mm across · tube radius %.2f mm",
                                  c.size_mm, c.tube_mm);
        status(w, s);
        g_free(s);
    }
    g_free(file);
}
static GtkWidget *button(GtkWidget *box, const char *title, GCallback cb, Sp3Window *w) {
    GtkWidget *b = gtk_button_new_with_label(title);
    gtk_box_pack_start(GTK_BOX(box), b, FALSE, FALSE, 0);
    g_signal_connect(b, "clicked", cb, w);
    return b;
}
static GtkWidget *spin_row(GtkWidget *box, const char *label, double lo, double hi, double step,
                           int digits) {
    GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(box), r, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(r), gtk_label_new(label), FALSE, FALSE, 0);
    GtkWidget *s = gtk_spin_button_new_with_range(lo, hi, step);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(s), digits);
    gtk_entry_set_width_chars(GTK_ENTRY(s), 7);
    gtk_box_pack_end(GTK_BOX(r), s, FALSE, FALSE, 0);
    return s;
}
static void sync_widgets(Sp3Window *w) {
    w->syncing = TRUE;
    GList *children = gtk_container_get_children(GTK_CONTAINER(w->grid));
    for (GList *p = children; p; p = p->next)
        gtk_widget_destroy(p->data);
    g_list_free(children);
    const char *headers[] = {"Arm", "Length", "Speed", "Tilt", "Azimuth", "Phase"};
    for (int j = 0; j < 6; j++)
        gtk_grid_attach(GTK_GRID(w->grid), gtk_label_new(headers[j]), j, 0, 1, 1);
    const double lo[] = {0, -300, 0, 0, 0}, hi[] = {3, 300, 180, 360, 360},
                 step[] = {.01, 1, 1, 1, 1};
    for (size_t i = 0; i < w->config.arm_count; i++) {
        char label[8];
        g_snprintf(label, sizeof label, "%zu", i + 1);
        gtk_grid_attach(GTK_GRID(w->grid), gtk_label_new(label), 0, i + 1, 1, 1);
        Sp3Arm a = w->config.arms[i];
        double values[] = {a.length, a.speed, a.tilt, a.azimuth, a.phase};
        for (int j = 0; j < 5; j++) {
            GtkWidget *s = gtk_spin_button_new_with_range(lo[j], hi[j], step[j]);
            gtk_entry_set_width_chars(GTK_ENTRY(s), 4);
            gtk_spin_button_set_digits(GTK_SPIN_BUTTON(s), j ? 0 : 2);
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(s), values[j]);
            gtk_grid_attach(GTK_GRID(w->grid), s, j + 1, i + 1, 1, 1);
            w->edit[i][j] = s;
        }
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(w->mode), w->config.tube);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->spin), w->config.spin);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->arms), w->config.show_arms);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->hue), w->config.hue);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->range), w->config.hue_range);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->samples), w->config.samples);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->mm), w->config.size_mm);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->radius), w->config.tube_mm);
    gtk_widget_show_all(w->grid);
    w->syncing = FALSE;
}
static void destroyed(GtkWidget *window, gpointer data) {
    (void)window;
    Sp3Window *w = data;
    if (w->timer)
        gtk_widget_remove_tick_callback(w->viewport, w->timer);
    sp3_curve_free(&w->curve);
    g_free(w);
}
GtkWidget *sp3_window(GtkWindow *parent, const SpPath *source) {
    Sp3Window *w = g_new0(Sp3Window, 1);
    w->config = sp3_defaults();
    w->view = sp3_default_view();
    w->progress = 1;
    w->can_lift = source && sp3_lift(source, &w->lifted);
    if (!sp3_curve(&w->config, &w->curve)) {
        g_free(w);
        return NULL;
    }
    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, NULL);
    w->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(w->window), "SpiroLab · 3D");
    gtk_window_set_default_size(GTK_WINDOW(w->window), 1360, 850);
    if (parent) {
        gtk_window_set_transient_for(GTK_WINDOW(w->window), parent);
        gtk_window_set_destroy_with_parent(GTK_WINDOW(w->window), TRUE);
    }
    g_signal_connect(w->window, "destroy", G_CALLBACK(destroyed), w);
    g_signal_connect(w->window, "key-press-event", G_CALLBACK(key), w);
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_add(GTK_CONTAINER(w->window), outer);
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(toolbar), 8);
    gtk_box_pack_start(GTK_BOX(outer), toolbar, FALSE, FALSE, 0);
    button(toolbar, "Apply arms", G_CALLBACK(apply), w);
    w->play = button(toolbar, "Play", G_CALLBACK(play), w);
    button(toolbar, "Replay", G_CALLBACK(replay), w);
    button(toolbar, "Finish", G_CALLBACK(finish), w);
    button(toolbar, "Fit", G_CALLBACK(fit), w);
    button(toolbar, "Randomize", G_CALLBACK(randomize), w);
    button(toolbar, "Reset", G_CALLBACK(reset), w);
    if (w->can_lift)
        button(toolbar, "Lift 2D curve", G_CALLBACK(lift), w);
    GtkWidget *spacer = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(toolbar), spacer, TRUE, TRUE, 0);
    button(toolbar, "Open", G_CALLBACK(open_design), w);
    button(toolbar, "Save", G_CALLBACK(save_design), w);
    button(toolbar, "PNG", G_CALLBACK(png), w);
    button(toolbar, "STL", G_CALLBACK(stl), w);
    GtkWidget *pane = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(outer), pane, TRUE, TRUE, 0);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_paned_pack1(GTK_PANED(pane), scroll, FALSE, FALSE);
    GtkWidget *side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(side), 12);
    gtk_container_add(GTK_CONTAINER(scroll), side);
    GtkWidget *hint = gtk_label_new(
        "Arms carry every arm after them. Integer speeds close the curve after one turn.");
    gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(hint), 45);
    gtk_box_pack_start(GTK_BOX(side), hint, FALSE, FALSE, 0);
    w->grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(w->grid), 4);
    gtk_grid_set_row_spacing(GTK_GRID(w->grid), 5);
    gtk_box_pack_start(GTK_BOX(side), w->grid, FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(side), row, FALSE, FALSE, 0);
    button(row, "Add arm", G_CALLBACK(add_arm), w);
    button(row, "Remove last", G_CALLBACK(remove_arm), w);
    w->mode = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(w->mode), "Line");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(w->mode), "Tube");
    gtk_box_pack_start(GTK_BOX(side), w->mode, FALSE, FALSE, 0);
    w->spin = gtk_check_button_new_with_label("Auto orbit");
    gtk_box_pack_start(GTK_BOX(side), w->spin, FALSE, FALSE, 0);
    w->arms = gtk_check_button_new_with_label("Show arm chain");
    gtk_box_pack_start(GTK_BOX(side), w->arms, FALSE, FALSE, 0);
    w->hue = spin_row(side, "Starting hue", 0, 360, 1, 0);
    w->range = spin_row(side, "Hue range", 0, 360, 1, 0);
    w->samples = spin_row(side, "Samples", 1000, 32000, 1000, 0);
    w->mm = spin_row(side, "Curve size · mm", 1, 1000, 1, 1);
    w->radius = spin_row(side, "Tube radius · mm", .05, 20, .05, 2);
    GtkWidget *speed_label = gtk_label_new("Playback speed");
    gtk_widget_set_halign(speed_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(side), speed_label, FALSE, FALSE, 0);
    w->speed = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, .05, 8, .05);
    gtk_range_set_value(GTK_RANGE(w->speed), 1);
    gtk_box_pack_start(GTK_BOX(side), w->speed, FALSE, FALSE, 0);
    g_signal_connect(w->mode, "changed", G_CALLBACK(style_changed), w);
    g_signal_connect(w->spin, "toggled", G_CALLBACK(style_changed), w);
    g_signal_connect(w->arms, "toggled", G_CALLBACK(style_changed), w);
    GtkWidget *styles[] = {w->hue, w->range, w->mm, w->radius};
    for (size_t i = 0; i < G_N_ELEMENTS(styles); i++)
        g_signal_connect(styles[i], "value-changed", G_CALLBACK(style_changed), w);
    w->viewport = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(w->viewport), GTK_STACK_TRANSITION_TYPE_NONE);
    w->gpu = sp3_gpu_new(gpu_notice, w);
    w->software = gtk_drawing_area_new();
    gtk_stack_add_named(GTK_STACK(w->viewport), w->gpu, "gpu");
    gtk_stack_add_named(GTK_STACK(w->viewport), w->software, "software");
    w->gpu_active = g_strcmp0(g_getenv("SPIROLAB_SOFTWARE_3D"), "1") != 0;
    w->canvas = w->gpu_active ? w->gpu : w->software;
    gtk_stack_set_visible_child_name(GTK_STACK(w->viewport), w->gpu_active ? "gpu" : "software");
    gtk_widget_set_hexpand(w->viewport, TRUE);
    gtk_widget_set_vexpand(w->viewport, TRUE);
    gtk_paned_pack2(GTK_PANED(pane), w->viewport, TRUE, FALSE);
    GtkWidget *canvases[] = {w->gpu, w->software};
    for (size_t i = 0; i < G_N_ELEMENTS(canvases); i++) {
        GtkWidget *canvas = canvases[i];
        gtk_widget_set_hexpand(canvas, TRUE);
        gtk_widget_set_vexpand(canvas, TRUE);
        gtk_widget_set_can_focus(canvas, TRUE);
        gtk_widget_add_events(canvas, GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK |
                                          GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                                          GDK_POINTER_MOTION_MASK);
        g_signal_connect(canvas, "button-press-event", G_CALLBACK(button_event), w);
        g_signal_connect(canvas, "button-release-event", G_CALLBACK(button_event), w);
        g_signal_connect(canvas, "motion-notify-event", G_CALLBACK(motion), w);
        g_signal_connect(canvas, "scroll-event", G_CALLBACK(scroll_event), w);
    }
    g_signal_connect(w->software, "draw", G_CALLBACK(draw), w);
    w->status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(w->status), 0);
    gtk_label_set_ellipsize(GTK_LABEL(w->status), PANGO_ELLIPSIZE_END);
    gtk_widget_set_margin_start(w->status, 12);
    gtk_widget_set_margin_end(w->status, 12);
    gtk_widget_set_margin_bottom(w->status, 8);
    gtk_box_pack_start(GTK_BOX(outer), w->status, FALSE, FALSE, 0);
    sync_widgets(w);
    summary(w);
    queue(w);
    gtk_widget_show_all(w->window);
    gtk_widget_grab_focus(w->canvas);
    w->last_tick = g_get_monotonic_time();
    w->timer = gtk_widget_add_tick_callback(w->viewport, frame_tick, w, NULL);
    g_object_set_data(G_OBJECT(w->window), "spiro3d-state", w);
    return w->window;
}
static void quit(GtkWidget *w, gpointer data) {
    (void)w;
    (void)data;
    gtk_main_quit();
}
int sp3_cli(int argc, char **argv) {
    gboolean gui = !strcmp(argv[1], "--3d");
    int min = gui ? 2 : 3, max = gui ? 3 : 4;
    if (argc < min || argc > max) {
        g_printerr("Usage: --3d [DESIGN.spiro3d], --render-3d FILE.png [DESIGN.spiro3d], or "
                   "--stl-3d FILE.stl [DESIGN.spiro3d]\n");
        return 2;
    }
    const char *input = argc == max ? argv[argc - 1] : NULL;
    Sp3Config c = sp3_defaults();
    Sp3View v = sp3_default_view();
    GError *e = NULL;
    if (input && !sp3_load(input, &c, &v, &e)) {
        g_printerr("%s\n", e->message);
        g_clear_error(&e);
        return 1;
    }
    if (gui) {
        gtk_init(&argc, &argv);
        GtkWidget *window = sp3_window(NULL, NULL);
        if (!window)
            return 1;
        Sp3Window *w = g_object_get_data(G_OBJECT(window), "spiro3d-state");
        if (input)
            restore_design(w, c, v);
        g_signal_connect(window, "destroy", G_CALLBACK(quit), NULL);
        gtk_main();
        return 0;
    }
    gboolean ok = FALSE;
    if (!strcmp(argv[1], "--stl-3d"))
        ok = sp3_export_stl(argv[2], &c, &e);
    else {
        Sp3Curve curve = {0};
        if (sp3_curve(&c, &curve))
            ok = sp3_png(argv[2], &c, &curve, &v, &e);
        sp3_curve_free(&curve);
    }
    if (!ok)
        g_printerr("3D export failed: %s\n", e ? e->message : "invalid curve");
    else
        g_print("Saved %s\n", argv[2]);
    g_clear_error(&e);
    return ok ? 0 : 1;
}
