#include "../viewer3d.c"
#include "render.h"
#include <assert.h>
#include <epoxy/gl.h>
#include <stdio.h>
static void frame(Sp3Window *w) {
    queue(w);
    gtk_gl_area_make_current(GTK_GL_AREA(w->gpu));
    gtk_gl_area_attach_buffers(GTK_GL_AREA(w->gpu));
    gboolean ok = FALSE;
    g_signal_emit_by_name(w->gpu, "render", gtk_gl_area_get_context(GTK_GL_AREA(w->gpu)), &ok);
    glFinish();
    assert(ok && glGetError() == GL_NO_ERROR);
}
static gboolean run(gpointer data) {
    Sp3Window *w = data;
    Sp3GpuStats s;
    sp3_gpu_stats(w->gpu, &s);
    assert(s.ready && s.hardware);
    printf("GPU %s\n", s.renderer);
    for (size_t n = 65536; n <= 2097152; n *= 2) {
        size_t count = 2 * n + 1;
        SpPoint source[4097];
        for (int i = 0; i <= 4096; i++) {
            double t = 6.283185307179586 * i / 4096;
            source[i] =
                (SpPoint){.7 * cos(3 * t) + .2 * cos(127 * t), .7 * sin(3 * t) + .2 * sin(127 * t)};
        }
        SpPath input = {source, 4097}, path = {0};
        SpFourier f = {0};
        assert(sp_fourier(&input, n, &f) && sp_synthesize(&f, n, count, &path));
        sp_fourier_free(&f);
        sp3_curve_free(&w->curve);
        w->curve = (Sp3Curve){.points = malloc(count * sizeof(Sp3Vec)), .count = count, .scale = 1};
        assert(w->curve.points);
        for (size_t i = 0; i < count; i++)
            w->curve.points[i] = (Sp3Vec){path.points[i].x, path.points[i].y, 0};
        sp3_gpu_geometry_changed(w->gpu);
        w->config.tube = 0;
        w->config.spin = 0;
        w->config.show_arms = 0;
        w->config.hue_range = 0;
        w->view = (Sp3View){.zoom = 4.5 / (1.1 * 3.51)};
        w->progress = 1;
        gint64 start = g_get_monotonic_time();
        frame(w);
        double upload = (g_get_monotonic_time() - start) / 1000.;
        int W = gtk_widget_get_allocated_width(w->gpu), H = gtk_widget_get_allocated_height(w->gpu);
        SpLayer layer = {.path = path, .ink = render_default_ink(), .visible = 1};
        SpViewTransform view = render_default_view();
        cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
        cairo_t *cr = cairo_create(surf);
        start = g_get_monotonic_time();
        render_scene_cairo(cr, W, H, NULL, 0, &layer, 1, &view, 0, 1);
        cairo_surface_flush(surf);
        double cpu = (g_get_monotonic_time() - start) / 1000.;
        cairo_destroy(cr);
        cairo_surface_destroy(surf);
        start = g_get_monotonic_time();
        for (int j = 0; j < 40; j++) {
            w->view.pan_x = .001 * (j % 5);
            frame(w);
        }
        double gpu = (g_get_monotonic_time() - start) / 40000.;
        printf("%zu points %dx%d: Cairo_preview=%.2f ms OpenGL=%.3f ms upload=%.2f ms\n", count, W,
               H, cpu, gpu, upload);
        fflush(stdout);
        sp_path_free(&path);
    }
    gtk_widget_destroy(w->window);
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}
int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    GtkWidget *win = sp3_window(NULL, NULL);
    g_timeout_add(300, run, g_object_get_data(G_OBJECT(win), "spiro3d-state"));
    gtk_main();
}
