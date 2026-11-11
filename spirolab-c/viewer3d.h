#ifndef SPIRO_VIEWER3D_H
#define SPIRO_VIEWER3D_H
#include "spiro.h"
#include <gtk/gtk.h>
GtkWidget *sp3_window(GtkWindow *parent, const SpPath *source);
int sp3_cli(int argc, char **argv);
#endif
