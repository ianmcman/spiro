#include "ai.h"
#include "document.h"
#include "image.h"
#include "viewer3d.h"
#include <gtk/gtk.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    SpDocument doc;
    GtkWidget *window, *sidebar, *actions, *canvas, *status, *progress, *notebook, *play;
    GtkWidget *shape[2], *teeth[2], *outside, *pen, *rotation, *second, *second_options, *second_outside, *ratio,
        *second_speed;
    GtkWidget *text, *terms, *harmonics, *detail, *threshold, *tool, *preview, *image_name, *color,
        *rainbow, *width, *speed, *overlay;
    SpViewTransform view;
    cairo_surface_t *cache;
    int cache_w, cache_h;
    gboolean dirty, playing, busy, closing, dragging, syncing;
    double last_x, last_y, animation_speed;
    gint64 last_tick;
    guint timer;
    char *model_dir;
} App;
typedef struct {
    App *app;
    gboolean prepare;
    SpParams params;
    SpInk ink;
    int mode, terms, detail, tool, faces;
    double threshold;
    char *text, *model_dir;
    GdkPixbuf *image, *result_image;
    SpPath path;
    SpFourier fourier;
    unsigned char *pen_down;
    GError *error;
} Job;
static void draw_action(GtkWidget *, gpointer);
static void fit_action(GtkWidget *, gpointer);
static void sync_controls(App *);
static void invalidate(App *a) {
    a->dirty = TRUE;
    gtk_widget_queue_draw(a->canvas);
}
static void message(App *a, const char *s) {
    gtk_label_set_text(GTK_LABEL(a->status), s);
}
static void report(App *a, const char *prefix, GError *e) {
    char *s = g_strdup_printf("%s: %s", prefix, e ? e->message : "operation failed");
    message(a, s);
    g_free(s);
    g_clear_error(&e);
}
static void play_label(App *a) {
    gtk_button_set_label(GTK_BUTTON(a->play), a->playing ? "Pause" : "Play");
}
static void ready_status(App *a) {
    char *s = g_strdup_printf("%zu kept layers · %zu points · %.0f%% zoom", a->doc.layer_count,
                              a->doc.current.path.count, a->view.zoom * 100);
    message(a, s);
    g_free(s);
}
static void set_busy(App *a, gboolean busy) {
    a->busy = busy;
    gtk_widget_set_sensitive(a->sidebar, !busy);
    gtk_widget_set_sensitive(a->actions, !busy);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(a->progress), a->doc.progress);
}
static void collect(App *a) {
    SpDocument *d = &a->doc;
    d->mode = gtk_notebook_get_current_page(GTK_NOTEBOOK(a->notebook));
    d->params.ring_shape = gtk_combo_box_get_active(GTK_COMBO_BOX(a->shape[0]));
    d->params.wheel_shape = gtk_combo_box_get_active(GTK_COMBO_BOX(a->shape[1]));
    d->params.ring_teeth = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(a->teeth[0]));
    d->params.wheel_teeth = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(a->teeth[1]));
    d->params.outside = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->outside));
    d->params.pen = gtk_range_get_value(GTK_RANGE(a->pen));
    d->params.rotation = gtk_range_get_value(GTK_RANGE(a->rotation));
    d->params.second =
        d->params.wheel_shape == 0 && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->second));
    d->params.second_outside = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->second_outside));
    d->params.second_ratio = gtk_range_get_value(GTK_RANGE(a->ratio));
    d->params.second_speed = gtk_range_get_value(GTK_RANGE(a->second_speed));
    g_free(d->text);
    d->text = g_strdup(gtk_entry_get_text(GTK_ENTRY(a->text)));
    d->terms = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(a->terms));
    d->detail = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(a->detail));
    d->threshold = gtk_range_get_value(GTK_RANGE(a->threshold));
    d->tool = gtk_combo_box_get_active(GTK_COMBO_BOX(a->tool));
    GdkRGBA c;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(a->color), &c);
    d->ink = (SpInk){c.red,
                     c.green,
                     c.blue,
                     c.alpha,
                     gtk_range_get_value(GTK_RANGE(a->width)),
                     gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->rainbow))};
    a->animation_speed = gtk_range_get_value(GTK_RANGE(a->speed));
}
static void preview(App *a) {
    GdkPixbuf *p = a->doc.prepared ? a->doc.prepared : a->doc.source;
    if (!p) {
        gtk_image_clear(GTK_IMAGE(a->preview));
        gtk_label_set_text(GTK_LABEL(a->image_name), "Choose a local picture");
        return;
    }
    double s = MIN(1., 180. / MAX(gdk_pixbuf_get_width(p), gdk_pixbuf_get_height(p)));
    GdkPixbuf *t =
        gdk_pixbuf_scale_simple(p, MAX(1, (int)(gdk_pixbuf_get_width(p) * s)),
                                MAX(1, (int)(gdk_pixbuf_get_height(p) * s)), GDK_INTERP_BILINEAR);
    gtk_image_set_from_pixbuf(GTK_IMAGE(a->preview), t);
    g_object_unref(t);
    char *label =
        g_strdup_printf("%d × %d pixels", gdk_pixbuf_get_width(p), gdk_pixbuf_get_height(p));
    gtk_label_set_text(GTK_LABEL(a->image_name), label);
    g_free(label);
}
static void mode_changed(GtkNotebook *n, GtkWidget *page, guint index, gpointer data) {
    (void)n;
    (void)page;
    App *a = data;
    if (a->syncing)
        return;
    a->doc.mode = (int)index;
    gtk_widget_set_visible(a->harmonics, index != 0);
}
static void second_changed(GtkWidget *w, gpointer data) {
    (void)w;
    App *a = data;
    if (a->syncing)
        return;
    int shape = gtk_combo_box_get_active(GTK_COMBO_BOX(a->shape[1]));
    gtk_widget_set_sensitive(a->second, shape == 0);
    gtk_widget_set_visible(a->second_options, shape == 0 && gtk_toggle_button_get_active(
                                                                GTK_TOGGLE_BUTTON(a->second)));
}
static void sync_controls(App *a) {
    a->syncing = TRUE;
    SpDocument *d = &a->doc;
    gtk_notebook_set_current_page(GTK_NOTEBOOK(a->notebook), d->mode);
    gtk_widget_set_visible(a->harmonics, d->mode != 0);
    for (int i = 0; i < 2; i++) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->shape[i]),
                                 i ? d->params.wheel_shape : d->params.ring_shape);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->teeth[i]),
                                  i ? d->params.wheel_teeth : d->params.ring_teeth);
    }
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->outside), d->params.outside);
    gtk_range_set_value(GTK_RANGE(a->pen), d->params.pen);
    gtk_range_set_value(GTK_RANGE(a->rotation), d->params.rotation);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->second), d->params.second);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->second_outside), d->params.second_outside);
    gtk_range_set_value(GTK_RANGE(a->ratio), d->params.second_ratio);
    gtk_range_set_value(GTK_RANGE(a->second_speed), d->params.second_speed);
    gtk_entry_set_text(GTK_ENTRY(a->text), d->text);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->terms), d->terms);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->detail), d->detail);
    gtk_range_set_value(GTK_RANGE(a->threshold), d->threshold);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->tool), d->tool);
    GdkRGBA c = {d->ink.r, d->ink.g, d->ink.b, d->ink.a};
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(a->color), &c);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->rainbow), d->ink.color_cycle);
    gtk_range_set_value(GTK_RANGE(a->width), d->ink.line_width);
    a->syncing = FALSE;
    second_changed(NULL, a);
    preview(a);
}
static gboolean canvas_draw(GtkWidget *w, cairo_t *cr, gpointer data) {
    App *a = data;
    int W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    if (!a->cache || W != a->cache_w || H != a->cache_h) {
        if (a->cache)
            cairo_surface_destroy(a->cache);
        a->cache = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
        a->cache_w = W;
        a->cache_h = H;
        a->dirty = TRUE;
    }
    if (a->dirty) {
        cairo_t *c = cairo_create(a->cache);
        render_scene_cairo(c, W, H, a->doc.layers, a->doc.layer_count, NULL, 1, &a->view, 0, 1);
        cairo_destroy(c);
        a->dirty = FALSE;
    }
    cairo_set_source_surface(cr, a->cache, 0, 0);
    cairo_paint(cr);
    render_scene_cairo(cr, W, H, NULL, 0, &a->doc.current, a->doc.progress, &a->view, 0, 0);
    if (a->doc.current.path.count && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->overlay))) {
        if (a->doc.drawn_mode == 0)
            render_wheel_overlay(cr, W, H, &a->doc.drawn_params, a->doc.progress, &a->view);
        else
            render_fourier_overlay(cr, W, H, &a->doc.fourier, &a->doc.current.path, a->doc.progress,
                                   &a->view);
    }
    return FALSE;
}
static gboolean tick(gpointer data) {
    App *a = data;
    gint64 now = g_get_monotonic_time();
    double dt = MIN(.1, (now - a->last_tick) / 1e6);
    a->last_tick = now;
    if (a->busy) {
        gtk_progress_bar_pulse(GTK_PROGRESS_BAR(a->progress));
        return G_SOURCE_CONTINUE;
    }
    if (a->playing && a->doc.current.path.count) {
        a->doc.progress = MIN(1., a->doc.progress + dt * a->animation_speed / 10.);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(a->progress), a->doc.progress);
        gtk_widget_queue_draw(a->canvas);
        if (a->doc.progress >= 1) {
            a->playing = FALSE;
            play_label(a);
        }
    }
    return G_SOURCE_CONTINUE;
}
static void fit_action(GtkWidget *w, gpointer data) {
    (void)w;
    App *a = data;
    double x0, y0, x1, y1;
    render_calculate_bounds(a->doc.layers, a->doc.layer_count, &a->doc.current, &x0, &y0, &x1, &y1);
    a->view.zoom = CLAMP(1.9 / MAX(.01, MAX(x1 - x0, y1 - y0)), .05, 50.);
    double scale =
        MIN(gtk_widget_get_allocated_width(a->canvas), gtk_widget_get_allocated_height(a->canvas)) /
        2.2 * a->view.zoom;
    a->view.pan_x = -(x0 + x1) / 2 * scale;
    a->view.pan_y = -(y0 + y1) / 2 * scale;
    invalidate(a);
    ready_status(a);
}
static gboolean scroll_event(GtkWidget *w, GdkEventScroll *e, gpointer data) {
    App *a = data;
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
    double old = a->view.zoom;
    a->view.zoom = CLAMP(old * pow(1.18, -dy), .05, 50.);
    double r = a->view.zoom / old, cx = e->x - gtk_widget_get_allocated_width(w) / 2.,
           cy = e->y - gtk_widget_get_allocated_height(w) / 2.;
    a->view.pan_x = cx - (cx - a->view.pan_x) * r;
    a->view.pan_y = cy - (cy - a->view.pan_y) * r;
    invalidate(a);
    ready_status(a);
    return TRUE;
}
static gboolean button_event(GtkWidget *w, GdkEventButton *e, gpointer data) {
    App *a = data;
    gtk_widget_grab_focus(w);
    if (e->type == GDK_BUTTON_PRESS &&
        (e->button == 2 || (e->button == 1 && (e->state & GDK_SHIFT_MASK)))) {
        a->dragging = TRUE;
        a->last_x = e->x;
        a->last_y = e->y;
        return TRUE;
    }
    if (e->type == GDK_BUTTON_RELEASE)
        a->dragging = FALSE;
    return FALSE;
}
static gboolean motion_event(GtkWidget *w, GdkEventMotion *e, gpointer data) {
    (void)w;
    App *a = data;
    if (!a->dragging)
        return FALSE;
    a->view.pan_x += e->x - a->last_x;
    a->view.pan_y += e->y - a->last_y;
    a->last_x = e->x;
    a->last_y = e->y;
    invalidate(a);
    return TRUE;
}
static GdkPixbuf *load_picture(const char *file, GError **e) {
    int w = 0, h = 0;
    if (!gdk_pixbuf_get_file_info(file, &w, &h) || w < 1 || h < 1) {
        g_set_error(e, G_FILE_ERROR, G_FILE_ERROR_FAILED, "Unsupported or damaged image");
        return NULL;
    }
    if (MAX(w, h) > SP_MAX_DETAIL)
        return gdk_pixbuf_new_from_file_at_scale(file, SP_MAX_DETAIL, SP_MAX_DETAIL, TRUE, e);
    return gdk_pixbuf_new_from_file(file, e);
}
static char *choose(App *a, const char *title, gboolean save, const char *suggest) {
    GtkWidget *d = gtk_file_chooser_dialog_new(
        title, GTK_WINDOW(a->window),
        save ? GTK_FILE_CHOOSER_ACTION_SAVE : GTK_FILE_CHOOSER_ACTION_OPEN, "Cancel",
        GTK_RESPONSE_CANCEL, save ? "Save" : "Open", GTK_RESPONSE_ACCEPT, NULL);
    if (save) {
        gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(d), TRUE);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(d), suggest);
    }
    char *file = NULL;
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT)
        file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));
    gtk_widget_destroy(d);
    return file;
}
static void job_free(Job *j) {
    sp_path_free(&j->path);
    sp_fourier_free(&j->fourier);
    free(j->pen_down);
    g_clear_object(&j->image);
    g_clear_object(&j->result_image);
    g_clear_error(&j->error);
    g_free(j->text);
    g_free(j->model_dir);
    g_free(j);
}
static gboolean job_done(gpointer data) {
    Job *j = data;
    App *a = j->app;
    if (a->closing) {
        job_free(j);
        a->busy = FALSE;
        gtk_main_quit();
        return G_SOURCE_REMOVE;
    }
    set_busy(a, FALSE);
    if (j->error) {
        report(a, j->prepare ? "Image processing failed" : "Drawing failed",
               g_steal_pointer(&j->error));
    } else if (j->prepare) {
        g_clear_object(&a->doc.prepared);
        a->doc.prepared = g_steal_pointer(&j->result_image);
        preview(a);
        char *s =
            j->faces ? g_strdup_printf("Prepared locally · %d faces detected. Press Draw to trace.",
                                       j->faces)
                     : g_strdup("Prepared locally. Press Draw to trace, or save the PNG.");
        message(a, s);
        g_free(s);
    } else {
        render_layer_free(&a->doc.current);
        sp_fourier_free(&a->doc.fourier);
        a->doc.fourier = j->fourier;
        j->fourier = (SpFourier){0};
        a->doc.current = (SpLayer){j->path, j->ink, 1, j->pen_down};
        j->pen_down = NULL;
        j->path = (SpPath){0};
        a->doc.drawn_params = j->params;
        a->doc.drawn_mode = j->mode;
        a->doc.progress = 0;
        a->playing = TRUE;
        play_label(a);
        fit_action(NULL, a);
    }
    job_free(j);
    return G_SOURCE_REMOVE;
}
static gpointer work(gpointer data) {
    Job *j = data;
    if (j->prepare) {
        SpAi *ai = sp_ai_new(j->model_dir, &j->error);
        if (ai) {
            int W = gdk_pixbuf_get_width(j->image), H = gdk_pixbuf_get_height(j->image);
            double scale = MIN(1., 4096. / MAX(W, H));
            GdkPixbuf *input = scale < 1 ? gdk_pixbuf_scale_simple(j->image,
                MAX(1, (int)round(W * scale)), MAX(1, (int)round(H * scale)), GDK_INTERP_BILINEAR)
                : g_object_ref(j->image);
            if (input) {
                j->result_image = sp_ai_apply(ai, input, (SpAiMode)(j->tool - 1), &j->faces, &j->error);
                g_object_unref(input);
            } else
                g_set_error(&j->error, G_FILE_ERROR, G_FILE_ERROR_NOMEM, "Cannot resize inference input");
            sp_ai_free(ai);
        }
    } else if (j->mode == 0) {
        if (!sp_generate(&j->params, SP_MAX_POINTS, &j->path))
            g_set_error(&j->error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                        "The gear settings cannot produce a valid curve");
    } else
        sp_build_trace(j->mode, j->text, j->image, j->detail, j->threshold, j->tool, j->terms,
                       &j->path, &j->fourier, &j->pen_down, &j->error);
    g_idle_add(job_done, j);
    return NULL;
}
static void start_job(App *a, gboolean prepare) {
    if (a->busy)
        return;
    collect(a);
    if ((prepare || a->doc.mode == 2) && !a->doc.source) {
        message(a, "Choose a picture first.");
        return;
    }
    if (prepare && a->doc.tool == 0) {
        g_clear_object(&a->doc.prepared);
        preview(a);
        message(a, "Using the original picture.");
        return;
    }
    if (!prepare && a->doc.mode == 2 && a->doc.tool != 0 && !a->doc.prepared) {
        message(a, "Press Prepare image before drawing with this tool.");
        return;
    }
    Job *j = g_new0(Job, 1);
    j->app = a;
    j->prepare = prepare;
    j->params = a->doc.params;
    j->ink = a->doc.ink;
    j->mode = a->doc.mode;
    j->terms = a->doc.terms;
    j->detail = a->doc.detail;
    j->threshold = a->doc.threshold;
    j->tool = a->doc.tool;
    j->text = g_strdup(a->doc.text);
    j->model_dir = g_strdup(a->model_dir);
    GdkPixbuf *p = prepare ? a->doc.source : (a->doc.prepared ? a->doc.prepared : a->doc.source);
    if (p)
        j->image = g_object_ref(p);
    a->playing = FALSE;
    play_label(a);
    set_busy(a, TRUE);
    message(a, prepare ? "Processing image locally…" : "Computing the drawing…");
    GThread *t = g_thread_new("spirolab-work", work, j);
    g_thread_unref(t);
}
static void draw_action(GtkWidget *w, gpointer p) {
    (void)w;
    start_job(p, FALSE);
}
static void prepare_action(GtkWidget *w, gpointer p) {
    (void)w;
    start_job(p, TRUE);
}
static void image_open(GtkWidget *w, gpointer data) {
    (void)w;
    App *a = data;
    char *file = choose(a, "Open picture", FALSE, NULL);
    if (!file)
        return;
    GError *e = NULL;
    GdkPixbuf *p = load_picture(file, &e);
    g_free(file);
    if (!p) {
        report(a, "Cannot open picture", e);
        return;
    }
    g_clear_object(&a->doc.source);
    g_clear_object(&a->doc.prepared);
    a->doc.source = p;
    preview(a);
    message(a, "Picture loaded locally. Choose a tool, then Prepare image or Draw.");
}
static void tool_changed(GtkComboBox *w, gpointer data) {
    (void)w;
    App *a = data;
    if (a->syncing)
        return;
    a->doc.tool = gtk_combo_box_get_active(GTK_COMBO_BOX(a->tool));
    if (a->doc.tool >= 3) {
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->terms), 8192);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->detail), 512);
    }
    g_clear_object(&a->doc.prepared);
    preview(a);
}
static void save_picture(GtkWidget *w, gpointer data) {
    (void)w;
    App *a = data;
    GdkPixbuf *p = a->doc.prepared ? a->doc.prepared : a->doc.source;
    if (!p) {
        message(a, "Choose and prepare a picture first.");
        return;
    }
    char *f = choose(a, "Save prepared picture", TRUE, "prepared.png");
    if (!f)
        return;
    GError *e = NULL;
    if (!gdk_pixbuf_save(p, f, "png", &e, NULL))
        report(a, "Cannot save PNG", e);
    else
        message(a, "Prepared PNG saved.");
    g_free(f);
}
static void play_action(GtkWidget *w, gpointer data) {
    (void)w;
    App *a = data;
    if (a->busy)
        return;
    if (!a->doc.current.path.count) {
        draw_action(NULL, a);
        return;
    }
    if (a->doc.progress >= 1)
        a->doc.progress = 0;
    a->playing = !a->playing;
    play_label(a);
}
static void finish_action(GtkWidget *w, gpointer data) {
    (void)w;
    App *a = data;
    a->playing = FALSE;
    a->doc.progress = 1;
    play_label(a);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(a->progress), 1);
    gtk_widget_queue_draw(a->canvas);
}
static void keep_action(GtkWidget *w, gpointer data) {
    (void)w;
    App *a = data;
    if (!a->doc.current.path.count)
        return;
    if (a->doc.layer_count >= SP_MAX_LAYERS) {
        message(a, "All 32 layers are in use. Undo a layer before adding another.");
        return;
    }
    a->doc.layers[a->doc.layer_count++] = a->doc.current;
    a->doc.current = (SpLayer){0};
    sp_fourier_free(&a->doc.fourier);
    a->doc.progress = 0;
    a->playing = FALSE;
    play_label(a);
    invalidate(a);
    ready_status(a);
}
static void undo_action(GtkWidget *w, gpointer data) {
    (void)w;
    App *a = data;
    if (a->doc.layer_count)
        render_layer_free(&a->doc.layers[--a->doc.layer_count]);
    invalidate(a);
    ready_status(a);
}
static void clear_action(GtkWidget *w, gpointer data) {
    (void)w;
    App *a = data;
    sp_document_clear(&a->doc);
    a->playing = FALSE;
    play_label(a);
    invalidate(a);
    ready_status(a);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(a->progress), 0);
}
static void open_design(GtkWidget *w, gpointer data) {
    (void)w;
    App *a = data;
    char *f = choose(a, "Open design", FALSE, NULL);
    if (!f)
        return;
    GError *e = NULL;
    if (!sp_document_load(&a->doc, f, &e))
        report(a, "Cannot open design", e);
    else {
        a->playing = FALSE;
        play_label(a);
        sync_controls(a);
        fit_action(NULL, a);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(a->progress), a->doc.progress);
    }
    g_free(f);
}
static void save_design(GtkWidget *w, gpointer data) {
    (void)w;
    App *a = data;
    collect(a);
    char *f = choose(a, "Save design", TRUE, "drawing.spiro");
    if (!f)
        return;
    GError *e = NULL;
    if (!sp_document_save(&a->doc, f, &e))
        report(a, "Cannot save design", e);
    else
        message(a, "Design saved, including layers and source pictures.");
    g_free(f);
}
static void export_action(GtkWidget *w, gpointer data) {
    App *a = data;
    int kind = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "kind"));
    const char *names[] = {"drawing.png", "drawing.svg", "drawing.pdf"};
    char *f = choose(a, "Export complete drawing", TRUE, names[kind]);
    if (!f)
        return;
    gboolean ok =
        kind == 0 ? render_export_png(f, 2048, 2048, a->doc.layers, a->doc.layer_count,
                                      &a->doc.current, 1)
        : kind == 1
            ? render_export_svg(f, 800, 800, a->doc.layers, a->doc.layer_count, &a->doc.current, 1)
            : render_export_pdf(f, 595, 842, a->doc.layers, a->doc.layer_count, &a->doc.current, 1);
    message(a,
            ok ? "Drawing exported." : "Export failed. Check the destination and available space.");
    g_free(f);
}
static void preset_changed(GtkComboBox *w, gpointer data) {
    App *a = data;
    int i = gtk_combo_box_get_active(w);
    if (a->syncing || i <= 0)
        return;
    a->doc.params = sp_defaults();
    a->doc.mode = 0;
    switch (i) {
    case 2:
        a->doc.params.ring_teeth = 105;
        a->doc.params.wheel_teeth = 45;
        break;
    case 3:
        a->doc.params.wheel_teeth = 36;
        break;
    case 4:
        a->doc.params.ring_shape = 1;
        break;
    case 5:
        a->doc.params.wheel_shape = 9;
        break;
    case 6:
        a->doc.params.second = 1;
        break;
    }
    sync_controls(a);
    draw_action(NULL, a);
}
static void shuffle_action(GtkWidget *w, gpointer data) {
    (void)w;
    App *a = data;
    a->doc.params = sp_defaults();
    a->doc.mode = 0;
    a->doc.params.ring_shape = g_random_int_range(0, 11);
    a->doc.params.wheel_shape = g_random_int_range(0, 11);
    a->doc.params.wheel_teeth = g_random_int_range(24, 85);
    a->doc.params.pen = g_random_double_range(.2, 1.3);
    a->doc.params.outside = g_random_int_range(0, 2);
    a->doc.params.rotation = g_random_double_range(0, 360);
    a->doc.params.second = a->doc.params.wheel_shape == 0 && g_random_int_range(0, 5) == 0;
    a->doc.params.second_outside = g_random_int_range(0, 2);
    sync_controls(a);
    draw_action(NULL, a);
}
static void changed_redraw(GtkWidget *w, gpointer data) {
    (void)w;
    App *a = data;
    gtk_widget_queue_draw(a->canvas);
}
static void speed_changed(GtkRange *w, gpointer data) {
    App *a = data;
    a->animation_speed = gtk_range_get_value(w);
}
static gboolean key_event(GtkWidget *w, GdkEventKey *e, gpointer data) {
    (void)w;
    App *a = data;
    if (a->busy)
        return FALSE;
    GtkWidget *f = gtk_window_get_focus(GTK_WINDOW(a->window));
    if (GTK_IS_EDITABLE(f) || GTK_IS_TEXT_VIEW(f))
        return FALSE;
    if (e->state & GDK_CONTROL_MASK) {
        if (e->keyval == GDK_KEY_s) {
            save_design(NULL, a);
            return TRUE;
        }
        if (e->keyval == GDK_KEY_o) {
            open_design(NULL, a);
            return TRUE;
        }
        return FALSE;
    }
    switch (e->keyval) {
    case GDK_KEY_space:
        play_action(NULL, a);
        break;
    case GDK_KEY_d:
    case GDK_KEY_D:
        draw_action(NULL, a);
        break;
    case GDK_KEY_f:
    case GDK_KEY_F:
        finish_action(NULL, a);
        break;
    case GDK_KEY_k:
    case GDK_KEY_K:
        keep_action(NULL, a);
        break;
    case GDK_KEY_u:
    case GDK_KEY_U:
        undo_action(NULL, a);
        break;
    case GDK_KEY_c:
    case GDK_KEY_C:
        clear_action(NULL, a);
        break;
    case GDK_KEY_0:
        fit_action(NULL, a);
        break;
    default:
        return FALSE;
    }
    return TRUE;
}
static gboolean close_event(GtkWidget *w, GdkEvent *e, gpointer data) {
    (void)w;
    (void)e;
    App *a = data;
    a->closing = TRUE;
    a->playing = FALSE;
    gtk_widget_hide(a->window);
    if (!a->busy)
        gtk_main_quit();
    return TRUE;
}
static GtkWidget *box(GtkWidget *parent, int spacing) {
    GtkWidget *b = gtk_box_new(GTK_ORIENTATION_VERTICAL, spacing);
    gtk_container_add(GTK_CONTAINER(parent), b);
    return b;
}
static GtkWidget *button(GtkWidget *row, const char *name, GCallback cb, App *a) {
    GtkWidget *w = gtk_button_new_with_label(name);
    gtk_box_pack_start(GTK_BOX(row), w, FALSE, FALSE, 0);
    g_signal_connect(w, "clicked", cb, a);
    return w;
}
static GtkWidget *row(GtkWidget *parent) {
    GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(parent), r, FALSE, FALSE, 0);
    return r;
}
static GtkWidget *scale(GtkWidget *parent, const char *label, double lo, double hi, double step,
                        double initial) {
    GtkWidget *l = gtk_label_new(label);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(parent), l, FALSE, FALSE, 0);
    GtkWidget *s = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, lo, hi, step);
    gtk_range_set_value(GTK_RANGE(s), initial);
    gtk_scale_set_digits(GTK_SCALE(s), step < .1 ? 2 : step < 1 ? 1 : 0);
    gtk_box_pack_start(GTK_BOX(parent), s, FALSE, FALSE, 0);
    return s;
}
static GtkWidget *spin(GtkWidget *parent, const char *label, int lo, int hi, int value) {
    GtkWidget *r = row(parent), *l = gtk_label_new(label);
    gtk_box_pack_start(GTK_BOX(r), l, FALSE, FALSE, 0);
    GtkWidget *s = gtk_spin_button_new_with_range(lo, hi, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s), value);
    gtk_box_pack_end(GTK_BOX(r), s, FALSE, FALSE, 0);
    return s;
}
static GtkWidget *check(GtkWidget *parent, const char *label) {
    GtkWidget *w = gtk_check_button_new_with_label(label);
    gtk_box_pack_start(GTK_BOX(parent), w, FALSE, FALSE, 0);
    return w;
}
static GtkWidget *tab(GtkWidget *notebook, const char *title) {
    GtkWidget *b = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(b), 10);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), b, gtk_label_new(title));
    return b;
}
static void open_3d(GtkWidget *button, gpointer data) {
    (void)button;
    App *a = data;
    if (!sp3_window(GTK_WINDOW(a->window), a->doc.drawn_mode == 0 ? &a->doc.current.path : NULL))
        message(a, "Cannot create the 3D window");
}
static void build_ui(App *a) {
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
                                    "window { background: #202126; } .sidebar { padding: 10px; } "
                                    ".toolbar { padding: 8px; } .status { padding: 6px 12px; }",
                                    -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, NULL);
    a->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(a->window), "SpiroLab — Native C");
    gtk_window_set_default_size(GTK_WINDOW(a->window), 1200, 820);
    g_signal_connect(a->window, "delete-event", G_CALLBACK(close_event), a);
    g_signal_connect(a->window, "key-press-event", G_CALLBACK(key_event), a);
    GtkWidget *outer = box(a->window, 0);
    a->actions = row(outer);
    gtk_style_context_add_class(gtk_widget_get_style_context(a->actions), "toolbar");
    button(a->actions, "Draw", G_CALLBACK(draw_action), a);
    a->play = button(a->actions, "Play", G_CALLBACK(play_action), a);
    button(a->actions, "Finish", G_CALLBACK(finish_action), a);
    button(a->actions, "Keep layer", G_CALLBACK(keep_action), a);
    button(a->actions, "Undo layer", G_CALLBACK(undo_action), a);
    button(a->actions, "Fit", G_CALLBACK(fit_action), a);
    button(a->actions, "Clear all", G_CALLBACK(clear_action), a);
    button(a->actions, "3D Spiro", G_CALLBACK(open_3d), a);
    GtkWidget *spacer = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(a->actions), spacer, TRUE, TRUE, 0);
    button(a->actions, "Open", G_CALLBACK(open_design), a);
    button(a->actions, "Save", G_CALLBACK(save_design), a);
    GtkWidget *exp = gtk_menu_button_new();
    gtk_button_set_label(GTK_BUTTON(exp), "Export");
    gtk_box_pack_start(GTK_BOX(a->actions), exp, FALSE, FALSE, 0);
    GtkWidget *menu = gtk_menu_new();
    const char *formats[] = {"PNG · 2048 px", "SVG · vector", "PDF · A4"};
    for (int i = 0; i < 3; i++) {
        GtkWidget *m = gtk_menu_item_new_with_label(formats[i]);
        g_object_set_data(G_OBJECT(m), "kind", GINT_TO_POINTER(i));
        g_signal_connect(m, "activate", G_CALLBACK(export_action), a);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), m);
    }
    gtk_widget_show_all(menu);
    gtk_menu_button_set_popup(GTK_MENU_BUTTON(exp), menu);
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(outer), paned, TRUE, TRUE, 0);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, 325, -1);
    gtk_paned_pack1(GTK_PANED(paned), scroll, FALSE, FALSE);
    a->sidebar = box(scroll, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(a->sidebar), "sidebar");
    GtkWidget *gallery = row(a->sidebar), *presets = gtk_combo_box_text_new();
    const char *names[] = {"Choose a preset…", "Classic 96/52", "105/45",    "96/36",
                           "Ellipse ring",     "Star wheel",    "Two wheels"};
    for (size_t i = 0; i < G_N_ELEMENTS(names); i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(presets), names[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(presets), 0);
    gtk_box_pack_start(GTK_BOX(gallery), presets, TRUE, TRUE, 0);
    g_signal_connect(presets, "changed", G_CALLBACK(preset_changed), a);
    button(gallery, "Shuffle", G_CALLBACK(shuffle_action), a);
    a->notebook = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(a->sidebar), a->notebook, FALSE, FALSE, 0);
    GtkWidget *g = tab(a->notebook, "Gears"), *t = tab(a->notebook, "Text"),
              *p = tab(a->notebook, "Picture");
    for (int i = 0; i < 2; i++) {
        GtkWidget *r = row(g);
        gtk_box_pack_start(GTK_BOX(r), gtk_label_new(i ? "Wheel shape" : "Ring shape"), FALSE,
                           FALSE, 0);
        a->shape[i] = gtk_combo_box_text_new();
        for (int j = 0; j < 11; j++)
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->shape[i]), sp_shape_names[j]);
        gtk_box_pack_end(GTK_BOX(r), a->shape[i], FALSE, FALSE, 0);
        a->teeth[i] = spin(g, i ? "Wheel teeth" : "Ring teeth", 1, 1000, i ? 52 : 96);
    }
    a->outside = check(g, "Roll outside the ring");
    a->pen = scale(g, "Pen radius", 0, 1.5, .01, .8);
    a->rotation = scale(g, "Rotation · degrees", 0, 360, 1, 0);
    a->second = check(g, "Second wheel");
    a->second_options = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(g), a->second_options, FALSE, FALSE, 0);
    a->second_outside = check(a->second_options, "Second wheel rolls outside");
    a->ratio = scale(a->second_options, "Second wheel / main wheel", 1. / 12, .5, 1. / 120, .25);
    a->second_speed = scale(a->second_options, "Orbit speed", 1, 12, 1, 2);
    g_signal_connect(a->shape[1], "changed", G_CALLBACK(second_changed), a);
    g_signal_connect(a->second, "toggled", G_CALLBACK(second_changed), a);
    a->text = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(a->text), 80);
    gtk_entry_set_placeholder_text(GTK_ENTRY(a->text), "A name or a short message");
    gtk_box_pack_start(GTK_BOX(t), a->text, FALSE, FALSE, 0);
    GtkWidget *hint = gtk_label_new("The rotating circles follow the letter strokes.");
    gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
    gtk_box_pack_start(GTK_BOX(t), hint, FALSE, FALSE, 0);
    GtkWidget *pr = row(p);
    button(pr, "Open image…", G_CALLBACK(image_open), a);
    a->image_name = gtk_label_new("Choose a local picture");
    gtk_label_set_ellipsize(GTK_LABEL(a->image_name), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_pack_start(GTK_BOX(p), a->image_name, FALSE, FALSE, 0);
    a->tool = gtk_combo_box_text_new();
    const char *tools[] = {"Original edges", "Remove background", "Subject outline", "Face drawing",
                           "Face + subject outline"};
    for (int i = 0; i < 5; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->tool), tools[i]);
    gtk_box_pack_start(GTK_BOX(p), a->tool, FALSE, FALSE, 0);
    g_signal_connect(a->tool, "changed", G_CALLBACK(tool_changed), a);
    pr = row(p);
    button(pr, "Prepare image", G_CALLBACK(prepare_action), a);
    button(pr, "Save PNG…", G_CALLBACK(save_picture), a);
    a->preview = gtk_image_new();
    gtk_box_pack_start(GTK_BOX(p), a->preview, FALSE, FALSE, 0);
    a->detail = spin(p, "Trace detail · pixels", 32, SP_MAX_DETAIL, 384);
    a->threshold = scale(p, "Edge threshold", .01, 1, .01, .15);
    a->harmonics = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(a->sidebar), a->harmonics, FALSE, FALSE, 0);
    a->terms = spin(a->harmonics, "Circles", 2, SP_MAX_TERMS, 256);
    a->overlay = check(a->sidebar, "Show drawing wheels");
    gtk_widget_set_tooltip_text(a->overlay, "Round gears, or up to 128 largest Fourier circles. "
                                            "The drawing uses all selected circles.");
    g_signal_connect(a->overlay, "toggled", G_CALLBACK(changed_redraw), a);
    GtkWidget *ink = gtk_frame_new("Ink");
    gtk_box_pack_start(GTK_BOX(a->sidebar), ink, FALSE, FALSE, 0);
    GtkWidget *ib = box(ink, 4);
    gtk_container_set_border_width(GTK_CONTAINER(ib), 8);
    GtkWidget *ir = row(ib);
    a->color = gtk_color_button_new();
    gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(a->color), TRUE);
    gtk_box_pack_start(GTK_BOX(ir), a->color, FALSE, FALSE, 0);
    a->rainbow = check(ir, "Rainbow ink");
    a->width = scale(ib, "Line width", .5, 4, .1, 2);
    a->speed = scale(a->sidebar, "Playback speed", .05, 8, .05, 1);
    g_signal_connect(a->speed, "value-changed", G_CALLBACK(speed_changed), a);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_paned_pack2(GTK_PANED(paned), right, TRUE, FALSE);
    a->canvas = gtk_drawing_area_new();
    gtk_widget_set_hexpand(a->canvas, TRUE);
    gtk_widget_set_vexpand(a->canvas, TRUE);
    gtk_widget_set_can_focus(a->canvas, TRUE);
    gtk_widget_add_events(a->canvas, GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK |
                                         GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                                         GDK_POINTER_MOTION_MASK);
    g_signal_connect(a->canvas, "draw", G_CALLBACK(canvas_draw), a);
    g_signal_connect(a->canvas, "scroll-event", G_CALLBACK(scroll_event), a);
    g_signal_connect(a->canvas, "button-press-event", G_CALLBACK(button_event), a);
    g_signal_connect(a->canvas, "button-release-event", G_CALLBACK(button_event), a);
    g_signal_connect(a->canvas, "motion-notify-event", G_CALLBACK(motion_event), a);
    gtk_box_pack_start(GTK_BOX(right), a->canvas, TRUE, TRUE, 0);
    a->progress = gtk_progress_bar_new();
    gtk_box_pack_start(GTK_BOX(right), a->progress, FALSE, FALSE, 0);
    a->status = gtk_label_new("Ready");
    gtk_label_set_ellipsize(GTK_LABEL(a->status), PANGO_ELLIPSIZE_END);
    gtk_widget_set_halign(a->status, GTK_ALIGN_FILL);
    gtk_label_set_xalign(GTK_LABEL(a->status), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(a->status), "status");
    gtk_box_pack_start(GTK_BOX(outer), a->status, FALSE, FALSE, 0);
    g_signal_connect(a->notebook, "switch-page", G_CALLBACK(mode_changed), a);
    gtk_widget_show_all(a->window);
    sync_controls(a);
    gtk_widget_grab_focus(a->canvas);
}
static char *default_models(void) {
    const char *env = g_getenv("SPIROLAB_MODEL_DIR");
    if (env && *env)
        return g_strdup(env);
    char *exe = g_file_read_link("/proc/self/exe", NULL);
    char *dir = exe ? g_path_get_dirname(exe) : g_get_current_dir();
    char *result = g_build_filename(dir, "models", NULL);
    g_free(dir);
    g_free(exe);
    return result;
}
static void help(const char *exe) {
    g_print("SpiroLab · native C desktop application\n\nUsage: %s [--model-dir DIR]\n  "
            "--3d [DESIGN.spiro3d]\n  --render-3d FILE.png [DESIGN.spiro3d]\n"
            "  --stl-3d FILE.stl [DESIGN.spiro3d]\n  --render-demo FILE.png\n  --ai "
            "cutout|silhouette|face|face-outline INPUT OUTPUT.png\n "
            " --trace-text TEXT OUTPUT.png [--terms N] [--detail N]\n  --trace-image INPUT "
            "OUTPUT.png [--ink] [--terms N] [--detail N]\n  --check-design FILE.spiro\n  --open "
            "FILE.spiro\n\n--model-dir DIR also applies to --ai. Default: models beside the "
            "executable.\nKeyboard: Space play/pause, D draw, F finish, K keep, U undo, C clear, 0 "
            "fit.\nMouse wheel zooms at the pointer; middle-drag or Shift-drag pans.\n",
            exe);
}
static gboolean integer(const char *s, int lo, int hi, int *out) {
    char *end = NULL;
    gint64 n = g_ascii_strtoll(s, &end, 10);
    if (!*s || *end || n < lo || n > hi)
        return FALSE;
    *out = (int)n;
    return TRUE;
}
int main(int argc, char **argv) {
    if (argc > 1 && (!strcmp(argv[1], "--3d") || !strcmp(argv[1], "--render-3d") ||
                     !strcmp(argv[1], "--stl-3d")))
        return sp3_cli(argc, argv);
    const char *cmd = NULL, *input = NULL, *output = NULL, *ai_mode = NULL, *open_file = NULL;
    char *models = default_models();
    int terms = 256, detail = 384;
    gboolean ink = FALSE;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            help(argv[0]);
            g_free(models);
            return 0;
        }
        if (!strcmp(argv[i], "--model-dir") && i + 1 < argc) {
            g_free(models);
            models = g_strdup(argv[++i]);
        } else if (!strcmp(argv[i], "--terms") && i + 1 < argc) {
            if (!integer(argv[++i], 2, SP_MAX_TERMS, &terms))
                goto bad;
        } else if (!strcmp(argv[i], "--detail") && i + 1 < argc) {
            if (!integer(argv[++i], 32, SP_MAX_DETAIL, &detail))
                goto bad;
        } else if (!strcmp(argv[i], "--ink"))
            ink = TRUE;
        else if (!strcmp(argv[i], "--open") && i + 1 < argc && !open_file) {
            open_file = argv[++i];
        } else if (!cmd && !strcmp(argv[i], "--render-demo") && i + 1 < argc) {
            cmd = "demo";
            output = argv[++i];
        } else if (!cmd && !strcmp(argv[i], "--check-design") && i + 1 < argc) {
            cmd = "check";
            input = argv[++i];
        } else if (!cmd && !strcmp(argv[i], "--ai") && i + 3 < argc) {
            cmd = "ai";
            ai_mode = argv[++i];
            input = argv[++i];
            output = argv[++i];
        } else if (!cmd &&
                   (!strcmp(argv[i], "--trace-text") || !strcmp(argv[i], "--trace-image")) &&
                   i + 2 < argc) {
            cmd = !strcmp(argv[i], "--trace-text") ? "text" : "image";
            input = argv[++i];
            output = argv[++i];
        } else
            goto bad;
    }
    if (cmd) {
        GError *e = NULL;
        gboolean ok = FALSE;
        if (!strcmp(cmd, "demo"))
            ok = render_demo_headless(output);
        else if (!strcmp(cmd, "check")) {
            SpDocument d;
            sp_document_init(&d);
            ok = sp_document_load(&d, input, &e);
            sp_document_free(&d);
        } else if (!strcmp(cmd, "ai")) {
            const char *names[] = {"cutout", "silhouette", "face", "face-outline"};
            int kind = -1;
            for (int k = 0; k < 4; k++)
                if (!strcmp(ai_mode, names[k]))
                    kind = k;
            if (kind < 0)
                goto bad;
            GdkPixbuf *p = load_picture(input, &e);
            if (p) {
                SpAi *ai = sp_ai_new(models, &e);
                if (ai) {
                    int faces = 0;
                    GdkPixbuf *r = sp_ai_apply(ai, p, (SpAiMode)kind, &faces, &e);
                    if (r) {
                        ok = gdk_pixbuf_save(r, output, "png", &e, NULL);
                        if (ok)
                            g_print("%d faces · %s\n", faces, output);
                        g_object_unref(r);
                    }
                    sp_ai_free(ai);
                }
                g_object_unref(p);
            }
        } else {
            GdkPixbuf *p = NULL;
            gboolean text = !strcmp(cmd, "text");
            if (!text)
                p = load_picture(input, &e);
            SpLayer layer = {.ink = render_default_ink(), .visible = 1};
            if ((text || p) &&
                sp_build_trace(text ? 1 : 2, text ? input : NULL, p, detail, .15, ink ? 3 : 0,
                               terms, &layer.path, NULL, &layer.pen_down, &e))
                ok = render_export_png(output, 2048, 2048, NULL, 0, &layer, 1);
            render_layer_free(&layer);
            g_clear_object(&p);
        }
        if (!ok)
            g_printerr("SpiroLab: %s\n", e ? e->message : "operation failed");
        g_clear_error(&e);
        g_free(models);
        return ok ? 0 : 1;
    }
    gtk_init(&argc, &argv);
    App a = {0};
    sp_document_init(&a.doc);
    a.model_dir = models;
    a.view = render_default_view();
    a.animation_speed = 1;
    a.dirty = TRUE;
    build_ui(&a);
    if (open_file) {
        GError *e = NULL;
        if (sp_document_load(&a.doc, open_file, &e)) {
            sync_controls(&a);
            fit_action(NULL, &a);
        } else
            report(&a, "Cannot open design", e);
    } else
        draw_action(NULL, &a);
    a.last_tick = g_get_monotonic_time();
    a.timer = g_timeout_add(33, tick, &a);
    gtk_main();
    g_source_remove(a.timer);
    gtk_widget_destroy(a.window);
    if (a.cache)
        cairo_surface_destroy(a.cache);
    sp_document_free(&a.doc);
    g_free(a.model_dir);
    return 0;
bad:
    g_printerr("Invalid arguments. Use --help for usage.\n");
    g_free(models);
    return 2;
}
