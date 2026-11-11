#include "render.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
int main(void) {
    SpPoint points[] = {{-.5, -.5}, {.5, -.5}, {-.5, .5}, {.5, .5}};
    unsigned char down[] = {0, 1, 0, 1};
    SpLayer layer = {.path = {points, 4},
                     .ink = {.r = 1, .g = .3, .b = .2, .a = 1, .line_width = 3},
                     .visible = 1,
                     .pen_down = down};
    SpViewTransform view = render_default_view();
    for (int rainbow = 0; rainbow < 2; rainbow++) {
        layer.ink.color_cycle = rainbow;
        cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 320, 320);
        cairo_t *cr = cairo_create(s);
        render_scene_cairo(cr, 320, 320, &layer, 1, NULL, 1, &view, 0, 1);
        cairo_surface_flush(s);
        int stride = cairo_image_surface_get_stride(s);
        unsigned char *p = cairo_image_surface_get_data(s);
        uint32_t center = ((uint32_t *)(p + 160 * stride))[160],
                 line = ((uint32_t *)(p + 87 * stride))[160];
        assert(((center >> 16) & 255) < 40 && ((center >> 8) & 255) < 40 && (center & 255) < 40);
        assert(((line >> 16) & 255) > 100 || ((line >> 8) & 255) > 100 || (line & 255) > 100);
        cairo_destroy(cr);
        cairo_surface_destroy(s);
    }
    puts("pen-up travel is invisible with both solid and rainbow ink");
}
