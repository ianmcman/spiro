#include "spiro.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
int main(void) {
    const double tau = 6.283185307179586;
    for (int outside = 0; outside < 2; outside++)
        for (int second_outside = 0; second_outside < 2; second_outside++) {
            SpParams p = sp_defaults();
            p.ring_teeth = 96;
            p.wheel_teeth = 24;
            p.outside = outside;
            p.second = 1;
            p.second_outside = second_outside;
            p.second_ratio = 1. / 3;
            p.second_speed = 5;
            p.rotation = 27;
            SpPath q = {0};
            assert(sp_generate(&p, 20000, &q));
            double r = .25, r2 = r / 3, sign = second_outside ? 1 : -1;
            for (size_t i = 0; i + 1 < q.count; i += 137) {
                double t = tau * i / (q.count - 1);
                double primary = outside ? 1 + r : 1 - r;
                double spin = outside ? (1 + r) / r * t + tau / 2 : -(1 - r) / r * t;
                double orbit = 5 * (outside ? -t : t) / r;
                double tip = sign * (3 + sign) * orbit;
                double x = primary * cos(t) + (r + sign * r2) * cos(spin + orbit) +
                           p.pen * r2 * cos(spin + tip);
                double y = primary * sin(t) + (r + sign * r2) * sin(spin + orbit) +
                           p.pen * r2 * sin(spin + tip);
                double rot = p.rotation * tau / 360;
                assert(hypot(q.points[i].x - (x * cos(rot) - y * sin(rot)),
                             q.points[i].y - (x * sin(rot) + y * cos(rot))) < 1e-10);
            }
            sp_path_free(&q);
            p.ring_shape = 9;
            assert(sp_generate(&p, 12000, &q));
            for (size_t i = 0; i < q.count; i++)
                assert(isfinite(q.points[i].x) && isfinite(q.points[i].y));
            assert(q.points[0].x == q.points[q.count - 1].x &&
                   q.points[0].y == q.points[q.count - 1].y);
            sp_path_free(&q);
        }
    SpParams p = sp_defaults();
    p.second = p.second_outside = 1;
    p.outside = 1;
    p.ring_teeth = 1000;
    p.wheel_teeth = 1;
    p.second_ratio = 1. / 12;
    p.second_speed = 12;
    SpPath q = {0};
    assert(sp_generate(&p, SP_MAX_POINTS, &q));
    assert(q.count == SP_MAX_POINTS);
    sp_path_free(&q);
    puts("two wheels: independent analytic inside/outside, polygon rolling and maximum-density "
         "closure passed");
}
