#include "document.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    SpDocument state, restored;
    sp_document_init(&state);
    sp_document_init(&restored);
    state.params.second = state.params.second_outside = 1;
    state.drawn_params = state.params;
    state.terms = 131072;
    state.detail = SP_MAX_DETAIL;
    state.current.ink = state.ink;
    state.current.ink.r = 1;
    state.current.ink.g = 0;
    state.current.visible = 1;
    assert(sp_generate(&state.params, 1000, &state.current.path));
    state.current.pen_down = calloc(state.current.path.count, 1);
    assert(state.current.pen_down);
    memset(state.current.pen_down, 1, state.current.path.count);
    state.current.pen_down[10] = 0;
    assert(render_layer_copy(&state.layers[0], &state.current));
    state.layer_count = 1;
    assert(sp_fourier(&state.current.path, 256, &state.fourier));
    state.progress = .6;
    g_free(state.text);
    state.text = g_strdup("Saved name");
    state.source = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 16, 24);
    gdk_pixbuf_fill(state.source, 0x33669988);
    state.prepared = gdk_pixbuf_copy(state.source);
    gdk_pixbuf_get_pixels(state.prepared)[0] = 0x12;
    state.mode = 2;
    state.tool = 4;
    GError *error = NULL;
    assert(sp_document_save(&state, "/tmp/spirolab-design-good.spiro", &error));
    assert(!error);
    GKeyFile *bad = g_key_file_new();
    assert(g_key_file_load_from_file(bad, "/tmp/spirolab-design-good.spiro", 0, &error));
    double points[] = {0, 0, NAN, 1};
    g_key_file_set_integer(bad, "Current", "count", 2);
    g_key_file_set_double_list(bad, "Current", "points", points, 4);
    assert(g_key_file_save_to_file(bad, "/tmp/spirolab-design-bad.spiro", &error));
    g_key_file_free(bad);
    SpPoint *original = state.current.path.points;
    assert(!sp_document_load(&state, "/tmp/spirolab-design-bad.spiro", &error));
    assert(error);
    g_clear_error(&error);
    assert(state.current.path.points == original && state.layer_count == 1);
    assert(!strcmp(state.text, "Saved name"));
    assert(sp_document_load(&restored, "/tmp/spirolab-design-good.spiro", &error));
    assert(!error && restored.current.path.count == state.current.path.count);
    assert(restored.params.second && restored.params.second_outside && restored.drawn_params.second_outside);
    assert(restored.terms == state.terms && restored.detail == state.detail);
    assert(fabs(restored.progress - .6) < 1e-12);
    assert(restored.layer_count == 1 && restored.mode == 2 && restored.tool == 4);
    assert(!strcmp(restored.text, "Saved name"));
    assert(restored.current.ink.r == 1 && restored.current.ink.g == 0);
    assert(restored.ink.r == state.ink.r);
    assert(restored.current.pen_down && !restored.current.pen_down[10] &&
           restored.current.pen_down[11]);
    assert(restored.layers[0].pen_down && !restored.layers[0].pen_down[10]);
    assert(restored.fourier.count == state.fourier.count);
    assert(restored.fourier.terms[0].frequency == state.fourier.terms[0].frequency);
    assert(fabs(restored.fourier.terms[0].re - state.fourier.terms[0].re) < 1e-12);
    for (size_t i = 0; i < state.current.path.count; i++) {
        assert(fabs(state.current.path.points[i].x - restored.current.path.points[i].x) < 1e-12);
        assert(fabs(state.current.path.points[i].y - restored.current.path.points[i].y) < 1e-12);
    }
    assert(gdk_pixbuf_get_width(restored.source) == 16);
    assert(gdk_pixbuf_get_height(restored.source) == 24);
    assert(gdk_pixbuf_get_pixels(restored.source)[0] == 0x33);
    assert(gdk_pixbuf_get_pixels(restored.source)[3] == 0x88);
    assert(gdk_pixbuf_get_pixels(restored.prepared)[0] == 0x12);
    GKeyFile *legacy = g_key_file_new();
    assert(g_key_file_load_from_file(legacy, "/tmp/spirolab-design-good.spiro", 0, &error));
    const char *keys[] = {"params", "drawn_params"};
    for (size_t i = 0; i < G_N_ELEMENTS(keys); i++) {
        gsize n;
        double *values = g_key_file_get_double_list(legacy, "Settings", keys[i], &n, &error);
        assert(values && n == 11);
        g_key_file_set_double_list(legacy, "Settings", keys[i], values, 10);
        g_free(values);
    }
    assert(g_key_file_save_to_file(legacy, "/tmp/spirolab-design-legacy.spiro", &error));
    g_key_file_free(legacy);
    assert(sp_document_load(&restored, "/tmp/spirolab-design-legacy.spiro", &error));
    assert(!restored.params.second_outside && !restored.drawn_params.second_outside);
    sp_document_free(&state);
    sp_document_free(&restored);
    puts("design roundtrip, separate inks, embedded images and corrupt-load preservation passed");
}
