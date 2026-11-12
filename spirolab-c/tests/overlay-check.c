#include "render.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
int main(void) {
    for (int second = 0; second < 3; second++)
    for (int side = 0; side < 2; side++)
        for (int cap = 0; cap < 2; cap++) {
            SpParams p = sp_defaults();
            p.rotation = 23;
            p.second = second != 0;
            p.second_outside = second == 2;
            p.outside = side;
            if (cap) {
                p.ring_teeth = 100;
                p.wheel_teeth = 200;
            }
            SpPath q = {0};
            assert(sp_generate(&p, 12000, &q));
            size_t at = q.count / 7;
            double progress = (double)at / (q.count - 1);
            SpViewTransform view = render_default_view();
            view.zoom = .2;
            cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 800, 800);
            cairo_t *cr = cairo_create(s);
            render_wheel_overlay(cr, 800, 800, &p, progress, &view);
            cairo_surface_flush(s);
            int x = (int)round(400 + q.points[at].x * 800 / 2.2 * .2),
                y = (int)round(400 + q.points[at].y * 800 / 2.2 * .2);
            assert(x >= 0 && x < 800 && y >= 0 && y < 800);
            uint32_t pixel = ((uint32_t *)(cairo_image_surface_get_data(s) +
                                           y * cairo_image_surface_get_stride(s)))[x];
            assert(((pixel >> 16) & 255) > 180 && ((pixel >> 8) & 255) < 100 &&
                   (pixel & 255) < 100);
            cairo_destroy(cr);
            cairo_surface_destroy(s);
            sp_path_free(&q);
        }
    puts("overlay pen positions match actual curves inside/outside, rotated and capped");
}
