#include "../viewer3d.c"
#include <epoxy/gl.h>
#include <stdio.h>
#include <sys/resource.h>
static size_t points;
static void frame(Sp3Window *w) {
    queue(w);
    gtk_gl_area_make_current(GTK_GL_AREA(w->gpu));
    gtk_gl_area_attach_buffers(GTK_GL_AREA(w->gpu));
    gboolean handled = FALSE;
    g_signal_emit_by_name(w->gpu, "render", gtk_gl_area_get_context(GTK_GL_AREA(w->gpu)), &handled);
    glFinish();
    Sp3GpuStats s = {0};
    sp3_gpu_stats(w->gpu, &s);
    if (!s.ready || !handled || glGetError() != GL_NO_ERROR) {
        fprintf(stderr, "RENDER_ERROR %s\n", s.error);
        exit(2);
    }
}
static int cmp(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static void measure(Sp3Window *w, const char *mode) {
    double ms[40];
    for (int i = 0; i < 40; i++) {
        w->view.yaw += .025;
        w->view.pitch += .002;
        gint64 begin = g_get_monotonic_time();
        frame(w);
        ms[i] = (g_get_monotonic_time() - begin) / 1000.;
    }
    qsort(ms, 40, sizeof(double), cmp);
    Sp3GpuStats s = {0};
    sp3_gpu_stats(w->gpu, &s);
    struct rusage r;
    getrusage(RUSAGE_SELF, &r);
    printf("%s n=%zu median_ms=%.3f p95_ms=%.3f max_ms=%.3f rss_peak_MiB=%.1f uploads=%llu/%llu\n",
           mode, points, ms[20], ms[37], ms[39], r.ru_maxrss / 1024.,
           (unsigned long long)s.line_uploads, (unsigned long long)s.tube_uploads);
    fflush(stdout);
}
static gboolean run(gpointer data) {
    Sp3Window *w = data;
    Sp3GpuStats s = {0};
    sp3_gpu_stats(w->gpu, &s);
    if (!s.ready || !s.hardware) {
        fprintf(stderr, "Hardware required: %s %s\n", s.renderer, s.error);
        exit(3);
    }
    Sp3Config c = sp3_defaults();
    c.samples = points;
    c.tube = FALSE;
    c.spin = FALSE;
    c.show_arms = FALSE;
    gint64 begin = g_get_monotonic_time();
    if (!replace(w, c)) {
        fprintf(stderr, "GENERATION_ERROR\n");
        exit(4);
    }
    printf("GENERATE n=%zu ms=%.3f\n", points, (g_get_monotonic_time() - begin) / 1000.);
    fflush(stdout);
    frame(w);
    measure(w, "LINE");
    w->config.tube = TRUE;
    begin = g_get_monotonic_time();
    frame(w);
    printf("TUBE_BUILD_UPLOAD n=%zu ms=%.3f\n", points, (g_get_monotonic_time() - begin) / 1000.);
    fflush(stdout);
    measure(w, "TUBE");
    printf("VRAM n=%zu ", points);
    fflush(stdout);
    int rc = system("nvidia-smi --query-gpu=memory.used,temperature.gpu --format=csv,noheader");
    (void)rc;
    gtk_widget_destroy(w->window);
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}
int main(int argc, char **argv) {
    points = argc > 1 ? strtoull(argv[1], NULL, 10) : 128000;
    if (points < 1000 || points > 8000000) {
        fprintf(stderr, "Use 1000 to 8000000 points\n");
        return 1;
    }
    gtk_init(&argc, &argv);
    GtkWidget *win = sp3_window(NULL, NULL);
    Sp3Window *w = g_object_get_data(G_OBJECT(win), "spiro3d-state");
    g_timeout_add(300, run, w);
    gtk_main();
}
