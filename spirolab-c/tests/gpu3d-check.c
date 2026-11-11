#include "../viewer3d.c"
#include <assert.h>
#include <epoxy/gl.h>
#include <stdio.h>
static void draw_current(Sp3Window *w) {
    gtk_gl_area_make_current(GTK_GL_AREA(w->gpu));
    gtk_gl_area_attach_buffers(GTK_GL_AREA(w->gpu));
    gboolean handled = FALSE;
    g_signal_emit_by_name(w->gpu, "render", gtk_gl_area_get_context(GTK_GL_AREA(w->gpu)), &handled);
    glFinish();
    Sp3GpuStats s = {0};
    sp3_gpu_stats(w->gpu, &s);
    if (!s.ready) {
        fprintf(stderr, "GPU error: %s\n", s.error);
        abort();
    }
    assert(handled && glGetError() == GL_NO_ERROR);
}
static void frame(Sp3Window *w) {
    queue(w);
    draw_current(w);
}
static gboolean check(gpointer data) {
    Sp3Window *w = data;
    Sp3GpuStats initial = {0};
    sp3_gpu_stats(w->gpu, &initial);
    if (!w->gpu_active || !initial.ready) {
        fprintf(stderr, "OpenGL unavailable: %s\n", initial.error);
        abort();
    }
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->spin), FALSE);
    frame(w);
    Sp3GpuStats a = {0};
    sp3_gpu_stats(w->gpu, &a);
    assert(a.line_uploads == 1);
    gtk_combo_box_set_active(GTK_COMBO_BOX(w->mode), 1);
    frame(w);
    sp3_gpu_stats(w->gpu, &a);
    assert(a.tube_uploads == 1);
    int scale = gtk_widget_get_scale_factor(w->gpu),
        W = gtk_widget_get_allocated_width(w->gpu) * scale,
        H = gtk_widget_get_allocated_height(w->gpu) * scale;
    gint64 start = g_get_monotonic_time();
    for (int i = 0; i < 60; i++) {
        w->view.yaw += .01;
        frame(w);
    }
    double gl_ms = (g_get_monotonic_time() - start) / 60000.;
    Sp3GpuStats b = {0};
    sp3_gpu_stats(w->gpu, &b);
    assert(b.line_uploads == a.line_uploads && b.tube_uploads == a.tube_uploads &&
           b.frames >= a.frames + 60);
    printf("OpenGL renderer: %s (%s)\n", b.renderer, b.hardware ? "hardware" : "software");
    printf("Completed tube orbit: %.2f ms/frame, %dx%d; submission %.3f ms; timer %.3f ms; "
           "geometry uploads unchanged (%llu line, %llu tube)\n",
           gl_ms, W, H, b.submit_ms, b.gpu_ms, (unsigned long long)b.line_uploads,
           (unsigned long long)b.tube_uploads);
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
    cairo_t *cr = cairo_create(surface);
    start = g_get_monotonic_time();
    for (int i = 0; i < 15; i++) {
        w->view.yaw += .01;
        sp3_draw(cr, W, H, &w->config, &w->curve, &w->view, 1, 1);
    }
    printf("Cairo completed tube orbit, same viewport: %.2f ms/frame\n",
           (g_get_monotonic_time() - start) / 15000.);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->hue), 20);
    frame(w);
    sp3_gpu_stats(w->gpu, &b);
    assert(b.tube_uploads == a.tube_uploads);
    w->view.zoom = 1.2;
    w->view.pan_x = .05;
    frame(w);
    sp3_gpu_stats(w->gpu, &b);
    assert(b.tube_uploads == a.tube_uploads);
    Sp3View old_view = w->view;
    GdkEventButton press = {.type = GDK_BUTTON_PRESS, .button = 1, .x = 200, .y = 200};
    button_event(w->canvas, &press, w);
    GdkEventMotion movement = {.x = 230, .y = 220};
    motion(w->canvas, &movement, w);
    press.type = GDK_BUTTON_RELEASE;
    button_event(w->canvas, &press, w);
    assert(w->view.yaw != old_view.yaw && w->view.pitch != old_view.pitch);
    GdkEventScroll wheel = {.direction = GDK_SCROLL_UP, .x = 250, .y = 250};
    scroll_event(w->canvas, &wheel, w);
    assert(w->view.zoom > old_view.zoom);
    frame(w);
    sp3_gpu_stats(w->gpu, &b);
    assert(b.tube_uploads == a.tube_uploads);
    GError *e = NULL;
    assert(sp3_gpu_snapshot(w->gpu, "/tmp/spirolab-gpu-tube.png", &e));
    assert(!e);
    replay(NULL, w);
    w->progress = .4;
    frame(w);
    sp3_gpu_stats(w->gpu, &b);
    assert(b.tube_uploads == a.tube_uploads);
    finish(NULL, w);
    frame(w);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->arms), TRUE);
    frame(w);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->radius), 1.5);
    frame(w);
    sp3_gpu_stats(w->gpu, &b);
    assert(b.tube_uploads == a.tube_uploads + 1);
    Sp3Config c = sp3_defaults();
    c.arm_count = 1;
    c.arms[0] = (Sp3Arm){1, 1, 90, 90, 0};
    c.spin = FALSE;
    c.show_arms = FALSE;
    Sp3View saved_view = {.zoom = .8, .pan_x = .08, .pan_y = .02};
    assert(restore_design(w, c, saved_view));
    /* Loading a paused design must push its saved camera without a timer or input event. */
    draw_current(w);
    assert(sp3_gpu_snapshot(w->gpu, "/tmp/spirolab-gpu-circle.png", &e));
    cairo_surface_t *png = cairo_image_surface_create_from_png("/tmp/spirolab-gpu-circle.png");
    assert(cairo_surface_status(png) == CAIRO_STATUS_SUCCESS);
    unsigned char *p = cairo_image_surface_get_data(png);
    int stride = cairo_image_surface_get_stride(png);
    int center_x = (int)round(W / 2. + saved_view.pan_x * MIN(W, H));
    int center_y = (int)round(H / 2. + saved_view.pan_y * MIN(W, H));
    uint32_t center = ((uint32_t *)(p + center_y * stride))[center_x];
    assert(((center >> 16) & 255) < 40 && ((center >> 8) & 255) < 40 && (center & 255) < 40);
    int top = (int)round(H / 2. + saved_view.pan_y * MIN(W, H) - .39 * MIN(W, H) * saved_view.zoom);
    gboolean found = FALSE;
    for (int y = MAX(0, top - 4); y <= MIN(H - 1, top + 4); y++)
        for (int x = center_x - 4; x <= center_x + 4; x++) {
            uint32_t q = ((uint32_t *)(p + y * stride))[x];
            if (((q >> 16) & 255) > 90 || ((q >> 8) & 255) > 90 || (q & 255) > 90)
                found = TRUE;
        }
    assert(found);
    cairo_surface_destroy(png);
    c = sp3_defaults();
    c.samples = 32000;
    c.tube = TRUE;
    c.spin = FALSE;
    assert(replace(w, c));
    sync_widgets(w);
    frame(w);
    sp3_gpu_stats(w->gpu, &a);
    for (int i = 0; i < 5; i++) {
        w->view.yaw += .02;
        frame(w);
    }
    sp3_gpu_stats(w->gpu, &b);
    assert(b.tube_uploads == a.tube_uploads);
    gtk_window_resize(GTK_WINDOW(w->window), 1500, 940);
    gdk_display_sync(gtk_widget_get_display(w->window));
    while (gtk_events_pending())
        gtk_main_iteration();
    gtk_container_check_resize(GTK_CONTAINER(w->window));
    frame(w);
    sp3_gpu_stats(w->gpu, &b);
    assert(b.tube_uploads == a.tube_uploads);
    puts("OpenGL line/tube/shading, projection readback, playback, arm overlay, max-sample buffers "
         "and camera-only cache checks passed");
    gtk_widget_destroy(w->window);
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}
int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    GtkWidget *window = sp3_window(NULL, NULL);
    assert(window);
    Sp3Window *w = g_object_get_data(G_OBJECT(window), "spiro3d-state");
    g_timeout_add(250, check, w);
    gtk_main();
    return 0;
}
