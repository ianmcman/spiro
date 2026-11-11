#include "image.h"
#include <cairo.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <stdlib.h>
#define IMAGE_ERROR (g_quark_from_static_string("spirolab-image"))
static unsigned char *text_mask(const char *text, int side) {
    if (!text || !*text)
        return NULL;
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_A8, side, side);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(s);
        return NULL;
    }
    cairo_t *cr = cairo_create(s);
    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *f = pango_font_description_from_string("JetBrains Mono 64");
    pango_layout_set_font_description(l, f);
    pango_font_description_free(f);
    pango_layout_set_width(l, 700 * PANGO_SCALE);
    pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_text(l, text, -1);
    PangoRectangle box;
    pango_layout_get_pixel_extents(l, &box, NULL);
    double scale = .9 * side / fmax(1, fmax(box.width, box.height));
    cairo_translate(cr, (side - box.width * scale) / 2 - box.x * scale,
                    (side - box.height * scale) / 2 - box.y * scale);
    cairo_scale(cr, scale, scale);
    cairo_set_source_rgba(cr, 1, 1, 1, 1);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
    cairo_destroy(cr);
    cairo_surface_flush(s);
    unsigned char *mask = malloc((size_t)side * side);
    const unsigned char *data = cairo_image_surface_get_data(s);
    int stride = cairo_image_surface_get_stride(s);
    if (mask)
        for (int y = 0; y < side; y++)
            for (int x = 0; x < side; x++)
                mask[y * side + x] = data[y * stride + x] > 40;
    cairo_surface_destroy(s);
    return mask;
}
static unsigned char *image_mask(GdkPixbuf *input, int detail, double threshold, gboolean ink,
                                 int *w, int *h) {
    if (!input)
        return NULL;
    double scale = (double)detail / fmax(gdk_pixbuf_get_width(input), gdk_pixbuf_get_height(input));
    *w = MAX(1, (int)round(gdk_pixbuf_get_width(input) * scale));
    *h = MAX(1, (int)round(gdk_pixbuf_get_height(input) * scale));
    if (ink) {
        /* Preserve thin dark strokes when reducing prepared line art. Averaging
           white pixels around a one-pixel line can erase the line entirely. */
        unsigned char *mask = calloc((size_t)*w * *h, 1);
        if (!mask)
            return NULL;
        int W = gdk_pixbuf_get_width(input), H = gdk_pixbuf_get_height(input);
        int nc = gdk_pixbuf_get_n_channels(input), stride = gdk_pixbuf_get_rowstride(input);
        const guchar *data = gdk_pixbuf_read_pixels(input);
        for (int y = 0; y < *h; y++)
            for (int x = 0; x < *w; x++) {
                int x0 = x * W / *w, y0 = y * H / *h;
                int x1 = MIN(W, ((x + 1) * W + *w - 1) / *w);
                int y1 = MIN(H, ((y + 1) * H + *h - 1) / *h);
                for (int sy = y0; sy < y1 && !mask[y * *w + x]; sy++)
                    for (int sx = x0; sx < x1; sx++) {
                        const guchar *q = data + sy * stride + sx * nc;
                        double alpha = nc == 4 ? q[3] / 255. : 1;
                        double lum =
                            alpha * (.299 * q[0] + .587 * q[1] + .114 * q[2]) / 255 + 1 - alpha;
                        if (lum < .7) {
                            mask[y * *w + x] = 1;
                            break;
                        }
                    }
            }
        return mask;
    }
    GdkPixbuf *p = gdk_pixbuf_scale_simple(input, *w, *h, GDK_INTERP_BILINEAR);
    if (!p)
        return NULL;
    double *gray = malloc((size_t)*w * *h * sizeof(double));
    unsigned char *mask = calloc((size_t)*w * *h, 1);
    if (!gray || !mask) {
        free(gray);
        free(mask);
        g_object_unref(p);
        return NULL;
    }
    const guchar *data = gdk_pixbuf_read_pixels(p);
    int nc = gdk_pixbuf_get_n_channels(p), stride = gdk_pixbuf_get_rowstride(p);
    for (int y = 0; y < *h; y++)
        for (int x = 0; x < *w; x++) {
            const guchar *q = data + y * stride + x * nc;
            double a = nc == 4 ? q[3] / 255. : 1;
            gray[y * *w + x] = a * (.299 * q[0] + .587 * q[1] + .114 * q[2]) / 255 + (1 - a);
            if (ink)
                mask[y * *w + x] = gray[y * *w + x] < .7;
        }
    if (!ink)
        for (int y = 1; y < *h - 1; y++)
            for (int x = 1; x < *w - 1; x++) {
                int i = y * *w + x, W = *w;
                double gx = -gray[i - W - 1] + gray[i - W + 1] - 2 * gray[i - 1] + 2 * gray[i + 1] -
                            gray[i + W - 1] + gray[i + W + 1];
                double gy = -gray[i - W - 1] - 2 * gray[i - W] - gray[i - W + 1] + gray[i + W - 1] +
                            2 * gray[i + W] + gray[i + W + 1];
                mask[i] = hypot(gx, gy) / 4 >= threshold;
            }
    free(gray);
    g_object_unref(p);
    return mask;
}
static unsigned char *stroke_visibility(const SpPath *source, size_t samples, double limit) {
    double *cum = calloc(source->count + 1, sizeof(double));
    unsigned char *down = calloc(samples, 1);
    if (!cum || !down) {
        free(cum);
        free(down);
        return NULL;
    }
    for (size_t i = 0; i < source->count; i++) {
        SpPoint a = source->points[i], b = source->points[(i + 1) % source->count];
        cum[i + 1] = cum[i] + hypot(a.x - b.x, a.y - b.y);
    }
    size_t k = 0;
    for (size_t j = 1; j < samples; j++) {
        double distance = ((double)j - .5) / (samples - 1) * cum[source->count];
        while (k + 1 < source->count && cum[k + 1] < distance)
            k++;
        down[j] = cum[k + 1] - cum[k] <= limit;
    }
    free(cum);
    return down;
}
int sp_build_trace(int mode, const char *text, GdkPixbuf *image, int detail, double threshold,
                   int tool, int terms, SpPath *out, SpFourier *fourier, unsigned char **pen_down,
                   GError **error) {
    if (out)
        *out = (SpPath){0};
    if (pen_down)
        *pen_down = NULL;
    if (fourier)
        *fourier = (SpFourier){0};
    if (!out || (mode != 1 && mode != 2) || detail < 32 || detail > 512 || terms < 2 ||
        terms > 16384 || !isfinite(threshold) || threshold < .01 || threshold > 1) {
        g_set_error(error, IMAGE_ERROR, 1, "Invalid tracing settings");
        return 0;
    }
    int w = detail, h = detail;
    unsigned char *mask = mode == 1 ? text_mask(text, detail)
                                    : image_mask(image, detail, threshold, tool >= 3, &w, &h);
    SpPath path = {0};
    SpFourier f = {0};
    int ok = 0;
    *out = (SpPath){0};
    if (!mask || !sp_trace_mask(mask, w, h, &path) || path.count < 2)
        goto done;
    int x0 = w, y0 = h, x1 = 0, y1 = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (mask[y * w + x]) {
                x0 = MIN(x0, x);
                x1 = MAX(x1, x);
                y0 = MIN(y0, y);
                y1 = MAX(y1, y);
            }
    double join_limit = 3.0 * 1.7 / MAX(1, MAX(x1 - x0, y1 - y0));
    size_t n = 2048;
    while (n < (size_t)terms && n < 16384)
        n *= 2;
    if (!sp_fourier(&path, n, &f) || !sp_synthesize(&f, (size_t)terms, 2 * n + 1, out))
        goto done;
    if (pen_down) {
        *pen_down = stroke_visibility(&path, out->count, join_limit);
        if (!*pen_down) {
            sp_path_free(out);
            goto done;
        }
    }
    if (fourier) {
        f.count = MIN(f.count, (size_t)terms);
        *fourier = f;
        f = (SpFourier){0};
    }
    ok = 1;
done:
    free(mask);
    sp_path_free(&path);
    sp_fourier_free(&f);
    if (!ok)
        g_set_error(error, IMAGE_ERROR, 1,
                    "No usable outline found. Try more detail or a lower edge threshold.");
    return ok;
}
