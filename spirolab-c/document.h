#ifndef SPIRO_DOCUMENT_H
#define SPIRO_DOCUMENT_H
#include "render.h"
#include <gdk-pixbuf/gdk-pixbuf.h>
typedef struct {
    SpParams params, drawn_params;
    int mode, drawn_mode, terms, detail, tool;
    double threshold, progress;
    char *text;
    SpInk ink;
    SpFourier fourier;
    SpLayer layers[SP_MAX_LAYERS], current;
    size_t layer_count;
    GdkPixbuf *source, *prepared;
} SpDocument;
void sp_document_init(SpDocument *d);
void sp_document_clear(SpDocument *d);
void sp_document_free(SpDocument *d);
gboolean sp_document_save(const SpDocument *d, const char *file, GError **error);
gboolean sp_document_load(SpDocument *d, const char *file, GError **error);
#endif
