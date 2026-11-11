#include "image.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
int main(void) {
    GdkPixbuf *p = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 2048, 512);
    gdk_pixbuf_fill(p, 0xffffffff);
    guchar *data = gdk_pixbuf_get_pixels(p);
    int stride = gdk_pixbuf_get_rowstride(p);
    /* One-pixel ink, thinner than one output pixel, must survive reduction. */
    for (int y = 40; y < 470; y++)
        for (int c = 0; c < 3; c++)
            data[y * stride + 1023 * 3 + c] = 0;
    SpPath q = {0};
    unsigned char *pen_down = NULL;
    SpFourier f = {0};
    GError *e = NULL;
    assert(sp_build_trace(2, NULL, p, 128, .15, 3, 256, &q, &f, &pen_down, &e));
    assert(!e && q.count > 2 && f.count == 256);
    double ymin = 1e9, ymax = -1e9;
    for (size_t i = 0; i < q.count; i++) {
        ymin = fmin(ymin, q.points[i].y);
        ymax = fmax(ymax, q.points[i].y);
    }
    assert(ymax - ymin > 1.5);
    assert(pen_down);
    gboolean ink = FALSE, lift = FALSE;
    for (size_t i = 1; i < q.count; i++) {
        if (pen_down[i])
            ink = TRUE;
        else
            lift = TRUE;
    }
    assert(ink && lift);
    free(pen_down);
    sp_path_free(&q);
    sp_fourier_free(&f);
    g_object_unref(p);
    puts("subpixel face/line-art strokes survive detail reduction");
}
