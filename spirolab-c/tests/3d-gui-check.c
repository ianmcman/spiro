#include "../viewer3d.c"
#include <assert.h>
static int stage;
static void capture(Sp3Window *w, const char *file) {
    while (gtk_events_pending())
        gtk_main_iteration();
    gtk_container_check_resize(GTK_CONTAINER(w->window));
    int W = gtk_widget_get_allocated_width(w->window),
        H = gtk_widget_get_allocated_height(w->window);
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
    cairo_t *c = cairo_create(s);
    gtk_widget_draw(w->window, c);
    cairo_destroy(c);
    assert(cairo_surface_write_to_png(s, file) == CAIRO_STATUS_SUCCESS);
    cairo_surface_destroy(s);
}
static gboolean exercise(gpointer data) {
    Sp3Window *w = data;
    if (stage == 0) {
        capture(w, "/tmp/spirolab-3d-window-line.png");
        assert(w->can_lift);
        lift(NULL, w);
        assert(sp3_valid(&w->config));
        reset(NULL, w);
        add_arm(NULL, w);
        assert(w->config.arm_count == 4);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->edit[0][1]), -3);
        apply(NULL, w);
        assert(w->config.arms[0].speed == -3);
        gtk_combo_box_set_active(GTK_COMBO_BOX(w->mode), 1);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->arms), TRUE);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->spin), FALSE);
    } else if (stage == 1) {
        Sp3View before = w->view;
        GdkEventButton press = {.type = GDK_BUTTON_PRESS, .button = 1, .x = 300, .y = 300};
        button_event(w->canvas, &press, w);
        GdkEventMotion move = {.x = 340, .y = 320};
        motion(w->canvas, &move, w);
        press.type = GDK_BUTTON_RELEASE;
        button_event(w->canvas, &press, w);
        assert(w->view.yaw != before.yaw && w->view.pitch != before.pitch);
        GdkEventScroll scroll_event_data = {.direction = GDK_SCROLL_UP, .x = 300, .y = 300};
        scroll_event(w->canvas, &scroll_event_data, w);
        assert(w->view.zoom > before.zoom);
        press = (GdkEventButton){.type = GDK_BUTTON_PRESS, .button = 2, .x = 300, .y = 300};
        button_event(w->canvas, &press, w);
        move = (GdkEventMotion){.x = 325, .y = 320};
        motion(w->canvas, &move, w);
        press.type = GDK_BUTTON_RELEASE;
        button_event(w->canvas, &press, w);
        assert(w->view.pan_x != before.pan_x);
        fit(NULL, w);
        assert(w->view.zoom == 1 && w->view.pan_x == 0);
        replay(NULL, w);
        w->last_tick -= 100000;
        tick(w);
        assert(w->playing && w->progress > 0);
        play(NULL, w);
        assert(!w->playing);
        finish(NULL, w);
        assert(w->progress == 1);
        capture(w, "/tmp/spirolab-3d-window-tube.png");
    } else {
        while (w->config.arm_count < 16)
            add_arm(NULL, w);
        assert(w->config.arm_count == 16);
        add_arm(NULL, w);
        assert(w->config.arm_count == 16);
        Sp3Vec *previous = w->curve.points;
        for (int i = 0; i < 16; i++)
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->edit[i][0]), 0);
        apply(NULL, w);
        assert(w->curve.points == previous);
        sync_widgets(w);
        remove_arm(NULL, w);
        assert(w->config.arm_count == 15);
        randomize(NULL, w);
        assert(w->config.arm_count == 3 && sp3_valid(&w->config));
        gtk_widget_destroy(w->window);
        return G_SOURCE_REMOVE;
    }
    stage++;
    return G_SOURCE_CONTINUE;
}
static void stop(GtkWidget *w, gpointer data) {
    (void)w;
    (void)data;
    gtk_main_quit();
}
int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    SpPoint points[257];
    for (int i = 0; i <= 256; i++)
        points[i] = (SpPoint){cos(6.283185307179586 * i / 256), sin(6.283185307179586 * i / 256)};
    SpPath path = {points, 257};
    GtkWidget *window = sp3_window(NULL, &path);
    assert(window);
    Sp3Window *w = g_object_get_data(G_OBJECT(window), "spiro3d-state");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->spin), FALSE);
    g_signal_connect(window, "destroy", G_CALLBACK(stop), NULL);
    g_timeout_add(250, exercise, w);
    gtk_main();
    puts("3D GTK: arm editing/add/remove/limits, 2D lift, line/tube views, orbit/pan/zoom, "
         "replay/pause/finish, invalid settings and cleanup passed");
}
