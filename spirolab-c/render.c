#define _POSIX_C_SOURCE 200809L
#include <cairo-pdf.h>
#include <cairo-svg.h>
#include <cairo.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "render.h"
#include "spiro.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

SpInk render_default_ink(void) {
    SpInk ink;
    ink.r = 0.22;
    ink.g = 0.74;
    ink.b = 0.97;
    ink.a = 1.0;
    ink.line_width = 2.0;
    ink.color_cycle = 0;
    return ink;
}

SpViewTransform render_default_view(void) {
    SpViewTransform v;
    v.pan_x = 0.0;
    v.pan_y = 0.0;
    v.zoom = 1.0;
    return v;
}

void render_layer_free(SpLayer *layer) {
    if (!layer)
        return;
    if (layer->path.points) {
        sp_path_free(&layer->path);
        layer->path.points = NULL;
    }
    free(layer->pen_down);
    layer->pen_down = NULL;
    layer->path.count = 0;
    layer->visible = 0;
}

int render_layer_copy(SpLayer *dst, const SpLayer *src) {
    if (!dst || !src)
        return 0;
    dst->pen_down = NULL;
    dst->ink = src->ink;
    dst->visible = src->visible;
    dst->path.count = src->path.count;
    if (src->path.count > 0 && src->path.points) {
        dst->path.points = (SpPoint *)malloc(sizeof(SpPoint) * src->path.count);
        if (!dst->path.points) {
            dst->path.count = 0;
            return 0;
        }
        memcpy(dst->path.points, src->path.points, sizeof(SpPoint) * src->path.count);
    } else {
        dst->path.points = NULL;
        dst->path.count = 0;
    }
    if (src->pen_down && dst->path.count) {
        dst->pen_down = malloc(dst->path.count);
        if (!dst->pen_down) {
            sp_path_free(&dst->path);
            return 0;
        }
        memcpy(dst->pen_down, src->pen_down, dst->path.count);
    }

    return 1;
}

static void hsv_to_rgb(double h, double s, double v, double *r, double *g, double *b) {
    h = fmod(h, 1.0);
    if (h < 0.0)
        h += 1.0;
    double c = v * s;
    double x = c * (1.0 - fabs(fmod(h * 6.0, 2.0) - 1.0));
    double m = v - c;
    double r1 = 0.0, g1 = 0.0, b1 = 0.0;
    int sector = (int)(h * 6.0);
    switch (sector % 6) {
    case 0:
        r1 = c;
        g1 = x;
        b1 = 0.0;
        break;
    case 1:
        r1 = x;
        g1 = c;
        b1 = 0.0;
        break;
    case 2:
        r1 = 0.0;
        g1 = c;
        b1 = x;
        break;
    case 3:
        r1 = 0.0;
        g1 = x;
        b1 = c;
        break;
    case 4:
        r1 = x;
        g1 = 0.0;
        b1 = c;
        break;
    default:
        r1 = c;
        g1 = 0.0;
        b1 = x;
        break;
    }
    *r = r1 + m;
    *g = g1 + m;
    *b = b1 + m;
}

int render_calculate_bounds(const SpLayer *layers, size_t layer_count, const SpLayer *current_layer,
                            double *min_x, double *min_y, double *max_x, double *max_y) {
    int found = 0;
    double minx = 1e9, miny = 1e9, maxx = -1e9, maxy = -1e9;

    for (size_t i = 0; i < layer_count; i++) {
        if (!layers[i].visible || layers[i].path.count == 0 || !layers[i].path.points)
            continue;
        for (size_t p = 0; p < layers[i].path.count; p++) {
            double px = layers[i].path.points[p].x;
            double py = layers[i].path.points[p].y;
            if (!isfinite(px) || !isfinite(py))
                continue;
            if (!found) {
                minx = maxx = px;
                miny = maxy = py;
                found = 1;
            } else {
                if (px < minx)
                    minx = px;
                if (px > maxx)
                    maxx = px;
                if (py < miny)
                    miny = py;
                if (py > maxy)
                    maxy = py;
            }
        }
    }

    if (current_layer && current_layer->visible && current_layer->path.count > 0 &&
        current_layer->path.points) {
        for (size_t p = 0; p < current_layer->path.count; p++) {
            double px = current_layer->path.points[p].x;
            double py = current_layer->path.points[p].y;
            if (!isfinite(px) || !isfinite(py))
                continue;
            if (!found) {
                minx = maxx = px;
                miny = maxy = py;
                found = 1;
            } else {
                if (px < minx)
                    minx = px;
                if (px > maxx)
                    maxx = px;
                if (py < miny)
                    miny = py;
                if (py > maxy)
                    maxy = py;
            }
        }
    }

    if (found) {
        *min_x = minx;
        *min_y = miny;
        *max_x = maxx;
        *max_y = maxy;
        return 1;
    }
    *min_x = -1.0;
    *min_y = -1.0;
    *max_x = 1.0;
    *max_y = 1.0;
    return 0;
}

static void draw_path_mapped(cairo_t *cr, const SpPoint *points, size_t count,
                             const unsigned char *pen_down, double progress, const SpInk *ink,
                             double time_sec, const SpCoordMap *map) {
    (void)time_sec;
    if (!points || count < 2)
        return;
    if (progress < 0.0)
        progress = 0.0;
    if (progress > 1.0)
        progress = 1.0;

    size_t draw_count = (size_t)(progress * (double)count);
    if (draw_count < 2)
        return;
    if (draw_count > count)
        draw_count = count;

    double effective_width = ink->line_width * map->line_width_scale;
    if (effective_width < 0.5)
        effective_width = 0.5;

    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    if (ink->color_cycle) {
        const size_t chunk_size = 16;
        for (size_t i = 0; i < draw_count - 1; i += chunk_size) {
            size_t end = i + chunk_size;
            if (end >= draw_count)
                end = draw_count - 1;

            double frac = (double)i / (double)(count > 1 ? (count - 1) : 1);
            double hue = fmod(frac * 2.5, 1.0);
            double r, g, b;
            hsv_to_rgb(hue, 0.95, 1.0, &r, &g, &b);

            cairo_new_path(cr);
            cairo_set_source_rgba(cr, r, g, b, ink->a);
            cairo_set_line_width(cr, effective_width);

            double sx = map->cx + points[i].x * map->scale;
            double sy = map->cy + points[i].y * map->scale;
            cairo_move_to(cr, sx, sy);
            for (size_t j = i + 1; j <= end; j++) {
                sx = map->cx + points[j].x * map->scale;
                sy = map->cy + points[j].y * map->scale;
                if (!pen_down || pen_down[j])
                    cairo_line_to(cr, sx, sy);
                else
                    cairo_move_to(cr, sx, sy);
            }
            cairo_stroke(cr);
        }
    } else {
        cairo_new_path(cr);
        cairo_set_source_rgba(cr, ink->r, ink->g, ink->b, ink->a);
        cairo_set_line_width(cr, effective_width);

        double sx = map->cx + points[0].x * map->scale;
        double sy = map->cy + points[0].y * map->scale;
        cairo_move_to(cr, sx, sy);
        for (size_t i = 1; i < draw_count; i++) {
            sx = map->cx + points[i].x * map->scale;
            sy = map->cy + points[i].y * map->scale;
            if (!pen_down || pen_down[i])
                cairo_line_to(cr, sx, sy);
            else
                cairo_move_to(cr, sx, sy);
        }
        cairo_stroke(cr);
    }
}

void render_scene_cairo(cairo_t *cr, int width, int height, const SpLayer *layers,
                        size_t layer_count, const SpLayer *current_layer, double current_progress,
                        const SpViewTransform *view, double time_sec, int dark_background) {
    if (!cr || width <= 0 || height <= 0)
        return;

    if (dark_background) {
        cairo_set_source_rgb(cr, 0.07, 0.07, 0.09); /* #121217 dark canvas */
        cairo_paint(cr);
    }

    double min_dim = (width < height ? width : height);
    SpCoordMap map;
    map.scale = (min_dim / 2.2) * (view ? view->zoom : 1.0);
    map.cx = (double)width / 2.0 + (view ? view->pan_x : 0.0);
    map.cy = (double)height / 2.0 + (view ? view->pan_y : 0.0);
    map.line_width_scale = 1.0;

    /* Render all kept layers to completion */
    for (size_t i = 0; i < layer_count; i++) {
        if (!layers[i].visible || layers[i].path.count < 2 || !layers[i].path.points)
            continue;
        draw_path_mapped(cr, layers[i].path.points, layers[i].path.count, layers[i].pen_down, 1.0,
                         &layers[i].ink, time_sec, &map);
    }

    /* Render currently animating layer */
    if (current_layer && current_layer->visible && current_layer->path.count >= 2 &&
        current_layer->path.points) {
        draw_path_mapped(cr, current_layer->path.points, current_layer->path.count,
                         current_layer->pen_down, current_progress, &current_layer->ink, time_sec,
                         &map);
    }
}

void render_wheel_overlay(cairo_t *cr, int width, int height, const SpParams *params,
                          double progress, const SpViewTransform *view) {
    if (!cr || !params || width <= 0 || height <= 0)
        return;
    if (params->ring_shape != 0 || params->wheel_shape != 0)
        return; /* Circles only */

    double min_dim = (width < height ? width : height);
    double scale = (min_dim / 2.2) * (view ? view->zoom : 1.0);
    double cx = (double)width / 2.0 + (view ? view->pan_x : 0.0);
    double cy = (double)height / 2.0 + (view ? view->pan_y : 0.0);

    if (params->second)
        return;
    double r = (double)params->wheel_teeth / params->ring_teeth;
    if (!params->outside && r > .97)
        r = .97;
    int laps = 1;
    double best_error = 1e30;
    for (int k = 1; k <= 400; k++) {
        double m = round(k / r), error = fabs(k / r - m) / (k / r);
        if (m < 1)
            continue;
        if (error < best_error) {
            best_error = error;
            laps = k;
        }
        if (error < .001) {
            laps = k;
            break;
        }
    }
    r = (double)laps / round(laps / r);
    double R_norm = 1, r_norm = r;
    double theta = progress * laps * 2 * M_PI;
    double center_radius = params->outside ? 1 + r : 1 - r;
    double wx = center_radius * cos(theta), wy = center_radius * sin(theta);
    double psi = params->outside ? (1 + r) / r * theta + M_PI : -(1 - r) / r * theta;
    double px = wx + params->pen * r * cos(psi), py = wy + params->pen * r * sin(psi);
    double rot = params->rotation * M_PI / 180, c = cos(rot), sn = sin(rot);
    double wheel_cx = wx * c - wy * sn, wheel_cy = wx * sn + wy * c;
    double pen_x = px * c - py * sn, pen_y = px * sn + py * c;

    /* Fixed ring circle */
    cairo_save(cr);
    cairo_set_line_width(cr, 1.5);
    const double dashed[] = {4.0, 4.0};
    cairo_set_dash(cr, dashed, 2, 0.0);
    cairo_set_source_rgba(cr, 0.3, 0.7, 1.0, 0.35);
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, R_norm * scale, 0.0, 2.0 * M_PI);
    cairo_stroke(cr);

    /* Rolling wheel circle */
    double scr_wcx = cx + wheel_cx * scale;
    double scr_wcy = cy + wheel_cy * scale;
    cairo_set_source_rgba(cr, 1.0, 0.8, 0.2, 0.5);
    cairo_new_path(cr);
    cairo_arc(cr, scr_wcx, scr_wcy, r_norm * scale, 0.0, 2.0 * M_PI);
    cairo_stroke(cr);

    /* Solid arm and pen */
    cairo_set_dash(cr, NULL, 0, 0.0);
    cairo_set_source_rgba(cr, 1.0, 0.9, 0.4, 0.75);
    cairo_new_path(cr);
    cairo_arc(cr, scr_wcx, scr_wcy, 3.0, 0.0, 2.0 * M_PI);
    cairo_fill(cr);

    double scr_px = cx + pen_x * scale;
    double scr_py = cy + pen_y * scale;
    cairo_set_source_rgba(cr, 1.0, 0.4, 0.2, 0.8);
    cairo_set_line_width(cr, 1.5);
    cairo_new_path(cr);
    cairo_move_to(cr, scr_wcx, scr_wcy);
    cairo_line_to(cr, scr_px, scr_py);
    cairo_stroke(cr);

    cairo_set_source_rgba(cr, 1.0, 0.2, 0.2, 0.95);
    cairo_new_path(cr);
    cairo_arc(cr, scr_px, scr_py, 4.0, 0.0, 2.0 * M_PI);
    cairo_fill(cr);
    cairo_restore(cr);
}

void render_fourier_overlay(cairo_t *cr, int width, int height, const SpFourier *f,
                            const SpPath *path, double progress, const SpViewTransform *view) {
    if (!f || !f->count || !path || path->count < 2)
        return;
    double scale = fmin(width, height) / 2.2 * view->zoom;
    double cx = width / 2. + view->pan_x, cy = height / 2. + view->pan_y;
    double x = 0, y = 0;
    cairo_save(cr);
    cairo_set_line_width(cr, 1);
    for (size_t i = 0; i < f->count && i < 128; i++) {
        SpTerm v = f->terms[i];
        double angle = 2 * M_PI * v.frequency * progress;
        double nx = x + v.re * cos(angle) - v.im * sin(angle);
        double ny = y + v.re * sin(angle) + v.im * cos(angle);
        double radius = hypot(v.re, v.im) * scale;
        if (v.frequency && radius >= 1) {
            cairo_set_source_rgba(cr, 1, .8, .3, .23);
            cairo_new_sub_path(cr);
            cairo_arc(cr, cx + x * scale, cy + y * scale, radius, 0, 2 * M_PI);
            cairo_stroke(cr);
        }
        cairo_set_source_rgba(cr, .9, .9, .9, .4);
        cairo_move_to(cr, cx + x * scale, cy + y * scale);
        cairo_line_to(cr, cx + nx * scale, cy + ny * scale);
        cairo_stroke(cr);
        x = nx;
        y = ny;
    }
    size_t index = (size_t)(progress * (path->count - 1));
    if (index >= path->count)
        index = path->count - 1;
    SpPoint pen = path->points[index];
    cairo_set_source_rgba(cr, 1, .5, .2, .65);
    cairo_move_to(cr, cx + x * scale, cy + y * scale);
    cairo_line_to(cr, cx + pen.x * scale, cy + pen.y * scale);
    cairo_stroke(cr);
    cairo_set_source_rgb(cr, 1, .3, .2);
    cairo_arc(cr, cx + pen.x * scale, cy + pen.y * scale, 3, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_restore(cr);
}

static void setup_export_coord_map(SpCoordMap *map, double target_w, double target_h,
                                   const SpLayer *layers, size_t layer_count,
                                   const SpLayer *current_layer) {
    double min_x = -1.0, min_y = -1.0, max_x = 1.0, max_y = 1.0;
    render_calculate_bounds(layers, layer_count, current_layer, &min_x, &min_y, &max_x, &max_y);

    double span_x = max_x - min_x;
    double span_y = max_y - min_y;
    double span = (span_x > span_y ? span_x : span_y);
    if (span < 0.001)
        span = 2.0;

    double mid_x = (min_x + max_x) / 2.0;
    double mid_y = (min_y + max_y) / 2.0;

    double min_dim = (target_w < target_h ? target_w : target_h);
    map->scale = (min_dim * 0.88) / span;
    map->cx = target_w / 2.0 - mid_x * map->scale;
    map->cy = target_h / 2.0 - mid_y * map->scale;
    map->line_width_scale = min_dim / 800.0;
    if (map->line_width_scale < 1.0)
        map->line_width_scale = 1.0;
}

int render_export_png(const char *filename, int width, int height, const SpLayer *layers,
                      size_t layer_count, const SpLayer *current_layer, double progress) {
    if (!filename || width <= 0 || height <= 0)
        return 0;

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return 0;
    }

    cairo_t *cr = cairo_create(surf);
    cairo_set_source_rgb(cr, 0.06, 0.06, 0.08); /* #0f0f14 */
    cairo_paint(cr);

    SpCoordMap map;
    setup_export_coord_map(&map, width, height, layers, layer_count, current_layer);

    for (size_t i = 0; i < layer_count; i++) {
        if (!layers[i].visible || layers[i].path.count < 2 || !layers[i].path.points)
            continue;
        draw_path_mapped(cr, layers[i].path.points, layers[i].path.count, layers[i].pen_down, 1.0,
                         &layers[i].ink, 0.0, &map);
    }
    if (current_layer && current_layer->visible && current_layer->path.count >= 2 &&
        current_layer->path.points) {
        draw_path_mapped(cr, current_layer->path.points, current_layer->path.count,
                         current_layer->pen_down, progress, &current_layer->ink, 0.0, &map);
    }

    cairo_destroy(cr);
    cairo_status_t st = cairo_surface_write_to_png(surf, filename);
    cairo_surface_destroy(surf);
    return (st == CAIRO_STATUS_SUCCESS) ? 1 : 0;
}

int render_export_svg(const char *filename, double width_pt, double height_pt,
                      const SpLayer *layers, size_t layer_count, const SpLayer *current_layer,
                      double progress) {
    if (!filename || width_pt <= 0.0 || height_pt <= 0.0)
        return 0;

    cairo_surface_t *surf = cairo_svg_surface_create(filename, width_pt, height_pt);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return 0;
    }

    cairo_t *cr = cairo_create(surf);
    cairo_set_source_rgb(cr, 0.06, 0.06, 0.08);
    cairo_paint(cr);

    SpCoordMap map;
    setup_export_coord_map(&map, width_pt, height_pt, layers, layer_count, current_layer);

    for (size_t i = 0; i < layer_count; i++) {
        if (!layers[i].visible || layers[i].path.count < 2 || !layers[i].path.points)
            continue;
        draw_path_mapped(cr, layers[i].path.points, layers[i].path.count, layers[i].pen_down, 1.0,
                         &layers[i].ink, 0.0, &map);
    }
    if (current_layer && current_layer->visible && current_layer->path.count >= 2 &&
        current_layer->path.points) {
        draw_path_mapped(cr, current_layer->path.points, current_layer->path.count,
                         current_layer->pen_down, progress, &current_layer->ink, 0.0, &map);
    }

    cairo_destroy(cr);
    cairo_surface_finish(surf);
    cairo_status_t status = cairo_surface_status(surf);
    cairo_surface_destroy(surf);
    return status == CAIRO_STATUS_SUCCESS;
}

int render_export_pdf(const char *filename, double width_pt, double height_pt,
                      const SpLayer *layers, size_t layer_count, const SpLayer *current_layer,
                      double progress) {
    if (!filename || width_pt <= 0.0 || height_pt <= 0.0)
        return 0;

    cairo_surface_t *surf = cairo_pdf_surface_create(filename, width_pt, height_pt);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return 0;
    }

    cairo_t *cr = cairo_create(surf);
    cairo_set_source_rgb(cr, 0.06, 0.06, 0.08);
    cairo_paint(cr);

    SpCoordMap map;
    setup_export_coord_map(&map, width_pt, height_pt, layers, layer_count, current_layer);

    for (size_t i = 0; i < layer_count; i++) {
        if (!layers[i].visible || layers[i].path.count < 2 || !layers[i].path.points)
            continue;
        draw_path_mapped(cr, layers[i].path.points, layers[i].path.count, layers[i].pen_down, 1.0,
                         &layers[i].ink, 0.0, &map);
    }
    if (current_layer && current_layer->visible && current_layer->path.count >= 2 &&
        current_layer->path.points) {
        draw_path_mapped(cr, current_layer->path.points, current_layer->path.count,
                         current_layer->pen_down, progress, &current_layer->ink, 0.0, &map);
    }

    cairo_destroy(cr);
    cairo_surface_finish(surf);
    cairo_status_t status = cairo_surface_status(surf);
    cairo_surface_destroy(surf);
    return status == CAIRO_STATUS_SUCCESS;
}

int render_demo_headless(const char *filename) {
    if (!filename)
        return 0;

    SpParams p = sp_defaults();
    SpPath path = {0, 0};
    int ok = sp_generate(&p, 8000, &path);

    if (!ok || path.count < 2) {
        sp_path_free(&path);
        return 0;
    }

    SpLayer layer = {0};
    layer.path = path;
    layer.ink = render_default_ink();
    layer.ink.color_cycle = 1;
    layer.ink.line_width = 2.4;
    layer.visible = 1;

    int res = render_export_png(filename, 2048, 2048, &layer, 1, NULL, 1.0);
    render_layer_free(&layer);
    return res;
}
