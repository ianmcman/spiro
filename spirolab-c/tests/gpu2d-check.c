#define main spirolab_entry
#include "../desktop.c"
#undef main
#include <assert.h>
#include <epoxy/gl.h>
#include <stdio.h>
static void frame(App *a) {
    queue_canvas(a);
    gtk_gl_area_make_current(GTK_GL_AREA(a->gpu));
    gtk_gl_area_attach_buffers(GTK_GL_AREA(a->gpu));
    gboolean ok = FALSE;
    g_signal_emit_by_name(a->gpu, "render", gtk_gl_area_get_context(GTK_GL_AREA(a->gpu)), &ok);
    glFinish();
    Sp2GpuStats s;
    sp2_gpu_stats(a->gpu, &s);
    if (!s.ready)
        fprintf(stderr, "GL: %s\n", s.error);
    assert(ok && s.ready && glGetError() == GL_NO_ERROR);
}
static GdkPixbuf *snapshot(App *a) {
    GError *e = NULL;
    assert(sp2_gpu_snapshot(a->gpu, "/tmp/spirolab-gpu2d-check.png", &e));
    GdkPixbuf *p = gdk_pixbuf_new_from_file("/tmp/spirolab-gpu2d-check.png", &e);
    assert(p && !e);
    return p;
}
static unsigned pixel(GdkPixbuf *p, double x, double y) {
    int W = gdk_pixbuf_get_width(p), H = gdk_pixbuf_get_height(p);
    int X = lround(W / 2. + x * MIN(W, H) / 2.2), Y = lround(H / 2. + y * MIN(W, H) / 2.2);
    assert(X >= 0 && Y >= 0 && X < W && Y < H);
    guchar *q = gdk_pixbuf_get_pixels(p) + Y * gdk_pixbuf_get_rowstride(p) +
                X * gdk_pixbuf_get_n_channels(p);
    return (q[0] << 16) | (q[1] << 8) | q[2];
}
static gboolean run(gpointer data) {
    App *a = data;
    Sp2GpuStats stats;
    sp2_gpu_stats(a->gpu, &stats);
    if (!stats.ready)
        fprintf(stderr, "GPU init: %s renderer=%s\n", stats.error, stats.renderer);
    assert(a->gpu_active && stats.ready);
    printf("2D renderer: %s (%s)\n", stats.renderer, stats.hardware ? "hardware" : "software");
    SpPoint points[] = {{-.5, -.5}, {0, -.5}, {.5, -.5}, {-.5, .5}, {.5, .5}};
    unsigned char down[] = {0, 1, 1, 0, 1};
    a->view = render_default_view();
    a->doc.current =
        (SpLayer){.path = {points, 5}, .ink = {1, 0, 0, 1, 4, 0}, .visible = 1, .pen_down = down};
    a->doc.progress = 1;
    geometry_changed(a);
    frame(a);
    GdkPixbuf *p = snapshot(a);
    assert((pixel(p, 0, -.5) >> 16) > 220);
    assert((pixel(p, 0, .5) >> 16) > 220);
    assert((pixel(p, 0, 0) & 0xffffff) < 0x282828);
    g_object_unref(p);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->overlay), TRUE);
    frame(a);
    p = snapshot(a);
    double tip = 1. - 52. / 96 + .8 * 52. / 96;
    unsigned pen = pixel(p, tip, 0);
    assert((pen >> 16) > 180 && ((pen >> 8) & 255) < 100);
    g_object_unref(p);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->overlay), FALSE);
    a->doc.current.ink.a = .5;
    frame(a);
    p = snapshot(a);
    unsigned red = pixel(p, 0, -.5) >> 16;
    assert(red > 110 && red < 160);
    assert((pixel(p, 0, .5) >> 16) > 110);
    g_object_unref(p);
    a->doc.layers[0] = a->doc.current;
    a->doc.layer_count = 1;
    a->doc.current.ink = (SpInk){0, 1, 0, .5, 4, 0};
    geometry_changed(a);
    frame(a);
    p = snapshot(a);
    unsigned color = pixel(p, 0, -.5);
    assert((color >> 16) > 50 && (color >> 16) < 90 && ((color >> 8) & 255) > 110 &&
           ((color >> 8) & 255) < 160);
    g_object_unref(p);
    a->doc.layer_count = 0;
    a->doc.current.ink = (SpInk){1, 0, 0, 1, 4, 1};
    frame(a);
    p = snapshot(a);
    assert(pixel(p, -.3, -.5) != pixel(p, .3, -.5));
    g_object_unref(p);
    sp2_gpu_stats(a->gpu, &stats);
    guint64 uploads = stats.uploads;
    for (int i = 0; i < 10; i++) {
        a->view.pan_x += 1;
        a->view.zoom += .01;
        frame(a);
    }
    sp2_gpu_stats(a->gpu, &stats);
    assert(stats.uploads == uploads);
    a->doc.current = (SpLayer){0};
    a->doc.layers[0] = (SpLayer){0};
    size_t n = SP_MAX_POINTS;
    a->doc.current.path.points = malloc(n * sizeof(SpPoint));
    assert(a->doc.current.path.points);
    a->doc.current.path.count = n;
    a->doc.current.visible = 1;
    a->doc.current.ink = render_default_ink();
    for (size_t i = 0; i < n; i++) {
        double t = 6.283185307179586 * i / (n - 1);
        a->doc.current.path.points[i] =
            (SpPoint){.7 * cos(3 * t) + .2 * cos(127 * t), .7 * sin(3 * t) + .2 * sin(127 * t)};
    }
    a->view = render_default_view();
    geometry_changed(a);
    gint64 start = g_get_monotonic_time();
    frame(a);
    printf("%zu-point stroked 2D buffer upload/draw: %.2f ms\n", n,
           (g_get_monotonic_time() - start) / 1000.);
    sp2_gpu_stats(a->gpu, &stats);
    uploads = stats.uploads;
    start = g_get_monotonic_time();
    for (int i = 0; i < 10; i++) {
        a->view.pan_x += 1;
        frame(a);
    }
    printf("%zu-point 2D pan: %.2f ms/frame\n", n, (g_get_monotonic_time() - start) / 10000.);
    sp2_gpu_stats(a->gpu, &stats);
    assert(stats.uploads == uploads);
    a->doc.progress = .5;
    frame(a);
    gtk_window_resize(GTK_WINDOW(a->window), 1400, 900);
    while (gtk_events_pending())
        gtk_main_iteration();
    frame(a);
    a->doc.current.ink.a = .5;
    frame(a);
    a->doc.progress = 1;
    frame(a);
    p = snapshot(a);
    g_object_unref(p);
    puts("2D GPU pixels: disconnected strokes, round joins, opacity/layers, rainbow, cached "
         "camera, playback, maximum points and resize passed");
    close_event(a->window, NULL, a);
    return G_SOURCE_REMOVE;
}
int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    App a = {0};
    sp_document_init(&a.doc);
    a.view = render_default_view();
    a.model_dir = default_models();
    a.animation_speed = 1;
    build_ui(&a);
    a.live_enabled = FALSE;
    g_timeout_add(300, run, &a);
    gtk_main();
    gtk_widget_destroy(a.window);
    if (a.cache)
        cairo_surface_destroy(a.cache);
    sp_document_free(&a.doc);
    g_free(a.model_dir);
}
