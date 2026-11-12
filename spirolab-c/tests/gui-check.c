/* Exercise the real GTK callbacks and asynchronous jobs in the desktop module. */
#define main spirolab_entry
#include "../desktop.c"
#undef main
#include <assert.h>
static int stage;
static const char *fixture;
static void capture(App *a, const char *file) {
    while (gtk_events_pending())
        gtk_main_iteration();
    gtk_container_check_resize(GTK_CONTAINER(a->window));
    int w = gtk_widget_get_allocated_width(a->window),
        h = gtk_widget_get_allocated_height(a->window);
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(s);
    gtk_widget_draw(a->window, cr);
    cairo_destroy(cr);
    assert(cairo_surface_write_to_png(s, file) == CAIRO_STATUS_SUCCESS);
    cairo_surface_destroy(s);
}
static gboolean exercise(gpointer data) {
    App *a = data;
    if (a->busy)
        return G_SOURCE_CONTINUE;
    assert(a->doc.current.path.count > 0 || stage == 3);
    if (stage == 0) {
        assert(a->doc.drawn_params.second && a->doc.drawn_params.second_outside);
        finish_action(NULL, a);
        capture(a, "/tmp/spirolab-final-gears.png");
        gtk_notebook_set_current_page(GTK_NOTEBOOK(a->notebook), 1);
        gtk_entry_set_text(GTK_ENTRY(a->text), "Ian");
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->terms), 131072);
        draw_action(NULL, a);
    } else if (stage == 1) {
        assert(a->doc.fourier.count == 131072);
        a->playing = FALSE;
        a->doc.progress = .55;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->overlay), TRUE);
        capture(a, "/tmp/spirolab-final-text.png");
        GError *e = NULL;
        a->doc.source = load_picture(fixture, &e);
        assert(a->doc.source && !e);
        gtk_notebook_set_current_page(GTK_NOTEBOOK(a->notebook), 2);
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->tool), 3);
        preview(a);
        prepare_action(NULL, a);
    } else if (stage == 2) {
        assert(a->doc.prepared);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->overlay), FALSE);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->terms), 8192);
        draw_action(NULL, a);
    } else if (stage == 3) {
        assert(a->doc.current.path.count > 4000 && a->doc.drawn_mode == 2);
        finish_action(NULL, a);
        capture(a, "/tmp/spirolab-final-face.png");
        keep_action(NULL, a);
        assert(a->doc.layer_count == 1 && !a->doc.current.path.count);
        undo_action(NULL, a);
        assert(!a->doc.layer_count);
        prepare_action(NULL, a);
        assert(a->busy);
        close_event(a->window, NULL, a);
        return G_SOURCE_REMOVE;
    }
    stage++;
    return G_SOURCE_CONTINUE;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    fixture = argv[1];
    gtk_init(&argc, &argv);
    App a = {0};
    sp_document_init(&a.doc);
    a.model_dir = default_models(); /* Test binary lives in /tmp, override explicitly. */
    g_free(a.model_dir);
    a.model_dir = g_canonicalize_filename("models", NULL);
    a.view = render_default_view();
    a.animation_speed = 1;
    a.dirty = TRUE;
    build_ui(&a);
    a.last_tick = g_get_monotonic_time();
    a.timer = g_timeout_add(33, tick, &a);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a.second), TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a.second_outside), TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a.overlay), TRUE);
    assert(gtk_adjustment_get_upper(gtk_spin_button_get_adjustment(GTK_SPIN_BUTTON(a.terms))) == SP_MAX_TERMS);
    assert(gtk_adjustment_get_upper(gtk_spin_button_get_adjustment(GTK_SPIN_BUTTON(a.detail))) == SP_MAX_DETAIL);
    draw_action(NULL, &a);
    g_timeout_add(250, exercise, &a);
    gtk_main();
    assert(a.closing && !a.busy);
    g_source_remove(a.timer);
    gtk_widget_destroy(a.window);
    if (a.cache)
        cairo_surface_destroy(a.cache);
    sp_document_free(&a.doc);
    g_free(a.model_dir);
    puts("GTK gear/text/face jobs, Fourier overlay, layer actions and close-during-inference "
         "passed");
}
