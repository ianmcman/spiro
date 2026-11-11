#include "render.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    SpPoint pts[] = {{-.5, -.5}, {.5, -.5}, {.5, .5}, {-.5, .5}, {-.5, -.5}};
    SpLayer held = {.path = {pts, 5},
                    .ink = {.r = 1, .g = 1, .b = 1, .a = 1, .line_width = 3, .color_cycle = 1},
                    .visible = 1};
    cairo_surface_t *a = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 320, 320),
                    *b = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 320, 320);
    cairo_t *ca = cairo_create(a), *cb = cairo_create(b);
    SpViewTransform view = render_default_view();
    render_scene_cairo(ca, 320, 320, &held, 1, NULL, 1, &view, 0, 1);
    render_scene_cairo(cb, 320, 320, &held, 1, NULL, 1, &view, 2, 1);
    cairo_surface_flush(a);
    cairo_surface_flush(b);
    assert(!memcmp(cairo_image_surface_get_data(a), cairo_image_surface_get_data(b),
                   (size_t)cairo_image_surface_get_stride(a) * 320));
    assert(render_export_svg("/tmp/spirolab-render-check.svg", 320, 320, &held, 1, NULL, 1));
    assert(render_export_pdf("/tmp/spirolab-render-check.pdf", 320, 320, &held, 1, NULL, 1));
    assert(!render_export_svg("/dev/full", 320, 320, &held, 1, NULL, 1));
    assert(!render_export_pdf("/dev/full", 320, 320, &held, 1, NULL, 1));
    cairo_destroy(ca);
    cairo_destroy(cb);
    cairo_surface_destroy(a);
    cairo_surface_destroy(b);
    puts("layer colour stability and export error reporting passed");
}
