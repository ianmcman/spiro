#define main spirolab_entry
#include "../desktop.c"
#undef main
#include <assert.h>
static int stage;
static gint64 began;
static gboolean run(gpointer data) {
    App *a = data;
    assert(g_get_monotonic_time() - began < 90000000);
    if (stage == 0) {
        if (a->busy || a->live_timer || !a->doc.current.path.count)
            return G_SOURCE_CONTINUE;
        assert(a->doc.drawn_mode == 1 && a->doc.progress == 1 && !a->playing);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->terms), SP_MAX_TERMS);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->detail), SP_MAX_DETAIL);
        stage = 1;
    } else if (stage == 1) {
        if (!a->busy)
            return G_SOURCE_CONTINUE;
        assert(gtk_widget_is_sensitive(a->text));
        gtk_entry_set_text(GTK_ENTRY(a->text), "Newest");
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->terms), 4096);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->detail), 512);
        stage = 2;
    } else if (stage == 2) {
        if (a->busy || a->live_timer)
            return G_SOURCE_CONTINUE;
        assert(a->doc.drawn_mode == 1 && a->doc.fourier.count == 4096 &&
               a->doc.current.path.count == 8193);
        assert(!strcmp(a->doc.text, "Newest") && a->doc.progress == 1);
        SpPath q = {0};
        GError *e = NULL;
        assert(sp_build_trace(1, "Newest", NULL, 512, .15, 0, 4096, &q, NULL, NULL, &e));
        assert(q.count == a->doc.current.path.count);
        for (size_t i = 0; i < q.count; i += 31)
            assert(hypot(q.points[i].x - a->doc.current.path.points[i].x,
                         q.points[i].y - a->doc.current.path.points[i].y) < 1e-10);
        sp_path_free(&q);
        if (a->gpu_active) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->overlay), TRUE);
            queue_canvas(a);
            assert(sp2_gpu_snapshot(a->gpu, "/tmp/spirolab-live-text.png", &e));
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->overlay), FALSE);
        }
        gtk_notebook_set_current_page(GTK_NOTEBOOK(a->notebook), 0);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->second), TRUE);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->second_outside), TRUE);
        stage = 3;
    } else if (stage == 3) {
        if (a->busy || a->live_timer)
            return G_SOURCE_CONTINUE;
        assert(a->doc.drawn_mode == 0 && a->doc.drawn_params.second_outside &&
               a->doc.drawn_params.second);
        gtk_range_set_value(GTK_RANGE(a->width), 3);
        assert(a->doc.current.ink.line_width == 3 && !a->live_timer);
        gtk_entry_set_text(GTK_ENTRY(a->text), "Pending close");
        assert(a->live_timer);
        clear_action(NULL, a);
        assert(!a->live_timer && !a->doc.current.path.count);
        gtk_entry_set_text(GTK_ENTRY(a->text), "Pending close again");
        assert(a->live_timer);
        close_event(a->window, NULL, a);
        assert(!a->live_timer);
        puts("live text/gear edits, latest-result wins during background tracing, style updates "
             "and pending-close cleanup passed");
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}
int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    App a = {0};
    sp_document_init(&a.doc);
    a.view = render_default_view();
    a.model_dir = default_models();
    a.animation_speed = 1;
    build_ui(&a);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(a.notebook), 1);
    gtk_entry_set_text(GTK_ENTRY(a.text), "First");
    began = g_get_monotonic_time();
    g_timeout_add(10, run, &a);
    gtk_main();
    gtk_widget_destroy(a.window);
    if (a.cache)
        cairo_surface_destroy(a.cache);
    sp_document_free(&a.doc);
    g_free(a.model_dir);
}
