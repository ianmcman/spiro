#ifndef SPIROLAB_GPU2D_H
#define SPIROLAB_GPU2D_H
#include "render.h"
#include <gtk/gtk.h>
typedef struct {
    gboolean ready, hardware;
    guint64 frames, uploads;
    char renderer[192], error[512];
} Sp2GpuStats;
typedef void (*Sp2GpuNotify)(GtkWidget *, gpointer);
GtkWidget *sp2_gpu_new(Sp2GpuNotify, gpointer);
void sp2_gpu_scene(GtkWidget *, const SpLayer *, size_t, const SpLayer *, double,
                   const SpViewTransform *, const SpParams *, const SpFourier *);
void sp2_gpu_geometry_changed(GtkWidget *);
void sp2_gpu_stats(GtkWidget *, Sp2GpuStats *);
gboolean sp2_gpu_snapshot(GtkWidget *, const char *, GError **);
#endif
