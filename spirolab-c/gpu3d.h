#ifndef SPIROLAB_GPU3D_H
#define SPIROLAB_GPU3D_H
#include "spiro3d.h"
#include <gtk/gtk.h>
typedef struct {
    gboolean ready, hardware;
    guint64 frames, line_uploads, tube_uploads;
    double submit_ms, gpu_ms;
    char renderer[192], error[512];
} Sp3GpuStats;
typedef void (*Sp3GpuNotify)(GtkWidget *area, gpointer data);
GtkWidget *sp3_gpu_new(Sp3GpuNotify notify, gpointer data);
void sp3_gpu_scene(GtkWidget *area, const Sp3Config *config, const Sp3Curve *curve,
                   const Sp3View *view, double progress, double arm_phase);
void sp3_gpu_geometry_changed(GtkWidget *area);
void sp3_gpu_stats(GtkWidget *area, Sp3GpuStats *out);
gboolean sp3_gpu_snapshot(GtkWidget *area, const char *file, GError **error);
#endif
