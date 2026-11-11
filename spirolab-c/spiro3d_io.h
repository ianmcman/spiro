#ifndef SPIRO3D_IO_H
#define SPIRO3D_IO_H
#include "spiro3d.h"
#include <cairo.h>
#include <glib.h>
void sp3_draw(cairo_t *cr, int width, int height, const Sp3Config *cfg, const Sp3Curve *curve,
              const Sp3View *view, double progress, double arm_phase);
gboolean sp3_png(const char *file, const Sp3Config *cfg, const Sp3Curve *curve, const Sp3View *view,
                 GError **error);
gboolean sp3_save(const char *file, const Sp3Config *cfg, const Sp3View *view, GError **error);
gboolean sp3_load(const char *file, Sp3Config *cfg, Sp3View *view, GError **error);
gboolean sp3_export_stl(const char *file, const Sp3Config *cfg, GError **error);
#endif
