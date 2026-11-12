#include "image.h"
#include "render.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <sys/resource.h>
int main(int argc, char **argv) {
    size_t n = argc > 1 ? strtoul(argv[1], NULL, 10) : 65536;
    int detail = argc > 2 ? atoi(argv[2]) : 512;
    SpPoint source[4097];
    for (int i = 0; i <= 4096; i++) {
        double t = 2 * 3.141592653589793 * i / 4096;
        source[i] =
            (SpPoint){.7 * cos(3 * t) + .2 * cos(127 * t), .7 * sin(3 * t) + .2 * sin(127 * t)};
    }
    SpPath input = {source, 4097}, out = {0};
    SpFourier f = {0};
    gint64 start = g_get_monotonic_time();
    assert(sp_fourier(&input, n, &f));
    printf("FFT %zu terms %.1f ms\n", n, (g_get_monotonic_time() - start) / 1000.);
    fflush(stdout);
    start = g_get_monotonic_time();
    assert(sp_synthesize(&f, n, 2 * n + 1, &out));
    printf("SYNTH %zu points %.1f ms\n", out.count, (g_get_monotonic_time() - start) / 1000.);
    fflush(stdout);
    SpLayer layer = {.path = out, .ink = render_default_ink(), .visible = 1};
    SpViewTransform view = render_default_view();
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1000, 800);
    cairo_t *cr = cairo_create(s);
    for (int color = 0; color < 2; color++) {
        layer.ink.color_cycle = color;
        start = g_get_monotonic_time();
        render_scene_cairo(cr, 1000, 800, NULL, 0, &layer, 1, &view, 0, 1);
        cairo_surface_flush(s);
        printf("DRAW %zu points %s %.1f ms\n", out.count, color ? "rainbow" : "solid",
               (g_get_monotonic_time() - start) / 1000.);
        fflush(stdout);
    }
    cairo_destroy(cr);
    cairo_surface_destroy(s);
    sp_path_free(&out);
    sp_fourier_free(&f);
    start = g_get_monotonic_time();
    unsigned char *mask = calloc((size_t)detail, detail);
    assert(mask);
    for (int y = 10; y < detail - 10; y++)
        mask[(size_t)y * detail + detail / 2] = 1;
    assert(sp_trace_mask(mask, detail, detail, &out));
    printf("TRACE sparse %d px %.1f ms\n", detail, (g_get_monotonic_time() - start) / 1000.);
    fflush(stdout);
    free(mask);
    sp_path_free(&out);
    start = g_get_monotonic_time();
    GError *e = NULL;
    assert(sp_build_trace(1, "SpiroLab", NULL, detail, .15, 0, n, &out, &f, NULL, &e));
    printf("TEXT %d px %zu circles %.1f ms\n", detail, n, (g_get_monotonic_time() - start) / 1000.);
    fflush(stdout);
    sp_path_free(&out);
    sp_fourier_free(&f);
    struct rusage r;
    getrusage(RUSAGE_SELF, &r);
    printf("RSS peak %.1f MiB\n", r.ru_maxrss / 1024.);
}
