#include "ai.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
    assert(argc == 3);
    GError *e = NULL;
    SpAi *ai = sp_ai_new(argv[1], &e);
    assert(ai && !e);
    GdkPixbuf *input = gdk_pixbuf_new_from_file(argv[2], &e);
    if (!input) {
        fprintf(stderr, "%s\n", e->message);
        return 1;
    }
    for (int mode = 0; mode <= 3; mode++) {
        int faces = 0;
        GdkPixbuf *out = sp_ai_apply(ai, input, (SpAiMode)mode, &faces, &e);
        if (!out) {
            fprintf(stderr, "mode %d: %s\n", mode, e ? e->message : "no error");
            return 1;
        }
        assert(gdk_pixbuf_get_width(out) == gdk_pixbuf_get_width(input));
        assert(gdk_pixbuf_get_height(out) == gdk_pixbuf_get_height(input));
        int w = gdk_pixbuf_get_width(out), h = gdk_pixbuf_get_height(out),
            stride = gdk_pixbuf_get_rowstride(out), nc = gdk_pixbuf_get_n_channels(out);
        unsigned char *p = gdk_pixbuf_get_pixels(out);
        size_t ink = 0, transparent = 0;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                unsigned char *q = p + y * stride + x * nc;
                if (q[0] < 128 && q[1] < 128 && q[2] < 128)
                    ink++;
                if (nc == 4 && q[3] < 128)
                    transparent++;
            }
        if (mode == 0)
            assert(transparent > 100 && transparent < (size_t)w * h - 100);
        else if (mode == 1)
            assert(ink > 100 && ink < (size_t)w * h - 100);
        else
            assert(ink > 100 && ink < (size_t)w * h / 2);
        if (mode >= 2)
            assert(faces >= 1);
        char file[128];
        snprintf(file, sizeof file, "/tmp/spirolab-ai-mode-%d.png", mode);
        assert(gdk_pixbuf_save(out, file, "png", &e, NULL));
        printf("AI mode %d: %d faces, %zu ink pixels, %zu transparent pixels; %s\n", mode, faces,
               ink, transparent, file);
        g_object_unref(out);
    }
    GdkPixbuf *bad = sp_ai_apply(ai, input, (SpAiMode)100, NULL, &e);
    assert(!bad && e);
    g_clear_error(&e);
    sp_ai_free(ai);
    sp_ai_free(NULL);
    g_object_unref(input);
    ai = sp_ai_new("/tmp/spirolab-nonexistent-models", &e);
    if (ai) {
        GdkPixbuf *blank = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 32, 32);
        gdk_pixbuf_fill(blank, 0xffffffff);
        bad = sp_ai_apply(ai, blank, SP_AI_CUTOUT, NULL, &e);
        assert(!bad && e);
        g_object_unref(blank);
        sp_ai_free(ai);
    } else
        assert(e);
    g_clear_error(&e);
    puts("AI inference and missing-model errors passed");
    return 0;
}
