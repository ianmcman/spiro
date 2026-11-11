#ifndef RENDER_H
#define RENDER_H

#include "spiro.h"
#include <cairo.h>
#include <stddef.h>

#define SP_MAX_LAYERS 32

/* Stroke ink and styling */
typedef struct {
    double r, g, b, a;
    double line_width;
    int color_cycle; /* 0: solid color, 1: cycling hue rainbow */
} SpInk;

/* Drawing layer owning path and ink attributes */
typedef struct {
    SpPath path;
    SpInk ink;
    int visible;
    unsigned char *pen_down; /* optional: segment arriving at point i is visible */
} SpLayer;

/* 2D pan and zoom viewport transform */
typedef struct {
    double pan_x;
    double pan_y;
    double zoom;
} SpViewTransform;

/* Coordinate mapping for Cairo viewports */
typedef struct {
    double cx;
    double cy;
    double scale;
    double line_width_scale;
} SpCoordMap;

/* Default constructors */
SpInk render_default_ink(void);
SpViewTransform render_default_view(void);

/* Layer memory management */
void render_layer_free(SpLayer *layer);
int render_layer_copy(SpLayer *dst, const SpLayer *src);

/* Bounds calculation for Zoom-to-Fit */
int render_calculate_bounds(const SpLayer *layers, size_t layer_count, const SpLayer *current_layer,
                            double *min_x, double *min_y, double *max_x, double *max_y);

/* Cairo rendering onto any cairo_t target */
void render_scene_cairo(cairo_t *cr, int width, int height, const SpLayer *layers,
                        size_t layer_count, const SpLayer *current_layer, double current_progress,
                        const SpViewTransform *view, double time_sec, int dark_background);

/* Optional physical spirograph gear overlay (Circle + Circle only) */
void render_wheel_overlay(cairo_t *cr, int width, int height, const SpParams *params,
                          double progress, const SpViewTransform *view);

void render_fourier_overlay(cairo_t *cr, int width, int height, const SpFourier *f,
                            const SpPath *path, double progress, const SpViewTransform *view);

/* Vector and image exports */
int render_export_png(const char *filename, int width, int height, const SpLayer *layers,
                      size_t layer_count, const SpLayer *current_layer, double progress);

int render_export_svg(const char *filename, double width_pt, double height_pt,
                      const SpLayer *layers, size_t layer_count, const SpLayer *current_layer,
                      double progress);

int render_export_pdf(const char *filename, double width_pt, double height_pt,
                      const SpLayer *layers, size_t layer_count, const SpLayer *current_layer,
                      double progress);

/* Headless demo generator without GTK initialization */
int render_demo_headless(const char *filename);

#endif /* RENDER_H */
