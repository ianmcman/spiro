#include "../viewer3d.c"
#include <stdio.h>
static gboolean done(gpointer data) {
    Sp3Window *w = data;
    Sp3GpuStats s = {0};
    sp3_gpu_stats(w->gpu, &s);
    printf(
        "3D backend: %s\nRenderer: %s\nOpenGL ready: %d; hardware: %d; frames: %llu\nError: %s\n",
        w->gpu_active ? "OpenGL" : "Cairo fallback", s.renderer, s.ready, s.hardware,
        (unsigned long long)s.frames, s.error);
    gtk_widget_destroy(w->window);
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}
int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    GtkWidget *window = sp3_window(NULL, NULL);
    Sp3Window *w = g_object_get_data(G_OBJECT(window), "spiro3d-state");
    g_timeout_add(500, done, w);
    gtk_main();
}
