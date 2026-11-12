#include "document.h"
#include "image.h"
#include <assert.h>
#include <glib/gstdio.h>
#include <stdio.h>
int main(void) {
    SpDocument state, loaded;
    sp_document_init(&state);
    sp_document_init(&loaded);
    state.mode = state.drawn_mode = 1;
    state.terms = SP_MAX_TERMS;
    state.detail = SP_MAX_DETAIL;
    state.progress = 1;
    state.current.visible = 1;
    state.current.ink = state.ink;
    GError *error = NULL;
    assert(sp_build_trace(1, "SpiroLab", NULL, state.detail, .15, 0, state.terms,
                          &state.current.path, &state.fourier, &state.current.pen_down, &error));
    assert(!error && state.current.path.count == SP_MAX_POINTS &&
           state.fourier.count == SP_MAX_TERMS);
    char *dir = g_dir_make_tmp("spirolab-high-resolution.XXXXXX", &error);
    assert(dir && !error);
    char *file = g_build_filename(dir, "maximum.spiro", NULL);
    assert(sp_document_save(&state, file, &error));
    assert(sp_document_load(&loaded, file, &error));
    assert(!error && loaded.current.path.count == state.current.path.count &&
           loaded.fourier.count == state.fourier.count && loaded.terms == state.terms &&
           loaded.detail == state.detail);
    size_t at = SP_MAX_POINTS / 3;
    assert(loaded.current.path.points[at].x == state.current.path.points[at].x &&
           loaded.current.path.points[at].y == state.current.path.points[at].y &&
           loaded.current.pen_down[at] == state.current.pen_down[at]);
    sp_document_free(&state);
    sp_document_free(&loaded);
    g_unlink(file);
    g_rmdir(dir);
    g_free(file);
    g_free(dir);
    puts("maximum text/detail/FFT synthesis, pen lifts and full-resolution saved-design roundtrip "
         "passed");
}
