#include "spiro.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
static void valid(SpPath *q) {
    assert(q->count >= 2 && q->count <= 12000);
    for (size_t i = 0; i < q->count; i++)
        assert(isfinite(q->points[i].x) && isfinite(q->points[i].y));
    assert(hypot(q->points[0].x - q->points[q->count - 1].x,
                 q->points[0].y - q->points[q->count - 1].y) < 1e-8);
}
int main(void) {
    SpParams p = sp_defaults();
    SpPath q = {0};
    assert(sp_generate(&p, 12000, &q));
    valid(&q);
    double x = q.points[0].x, y = q.points[0].y;
    assert(fabs(y) < .01);
    assert(x > .1);
    sp_path_free(&q);
    p.rotation = 90;
    assert(sp_generate(&p, 12000, &q));
    assert(fabs(q.points[0].x + y) < 1e-6 && fabs(q.points[0].y - x) < 1e-6);
    sp_path_free(&q);
    p.rotation = 0;
    for (int a = 0; a < 11; a++)
        for (int b = 0; b < 11; b++)
            for (int side = 0; side < 2; side++) {
                p.ring_shape = a;
                p.wheel_shape = b;
                p.outside = side;
                assert(sp_generate(&p, 12000, &q));
                valid(&q);
                sp_path_free(&q);
            }
    p = sp_defaults();
    p.second = 1;
    assert(sp_generate(&p, 12000, &q));
    valid(&q);
    assert(fabs(q.points[0].x - x) > .001);
    sp_path_free(&q);
    p.pen = NAN;
    assert(!sp_generate(&p, 12000, &q));
    p = sp_defaults();
    p.ring_teeth = 0;
    assert(!sp_generate(&p, 12000, &q));
    p = sp_defaults();
    assert(!sp_generate(&p, 0, &q));
    p.ring_shape = 11;
    assert(!sp_generate(&p, 12000, &q));
    p = sp_defaults();
    p.ring_teeth = 2147483647;
    assert(!sp_generate(&p, 12000, &q));
    p = sp_defaults();
    p.second = 1;
    p.second_ratio = 1e-300;
    assert(!sp_generate(&p, 12000, &q));
    p.second_ratio = .25;
    p.second_speed = 1e300;
    assert(!sp_generate(&p, 12000, &q));
    sp_path_free(NULL);
    puts("geometry: shapes, closure, rotation, second wheel and validation passed");
}
