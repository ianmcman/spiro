#ifndef SPIRO_IMAGE_H
#define SPIRO_IMAGE_H
#include "spiro.h"
#include <gdk-pixbuf/gdk-pixbuf.h>
int sp_build_trace(int mode, const char *text, GdkPixbuf *image, int detail, double threshold,
                   int tool, int terms, SpPath *out, SpFourier *fourier, unsigned char **pen_down,
                   GError **error);
#endif
