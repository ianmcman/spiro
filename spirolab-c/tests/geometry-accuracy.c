#include "spiro.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
static int gcd(int a, int b) {
    while (b) {
        int t = a % b;
        a = b;
        b = t;
    }
    return a;
}
int main(void) {
    const double tau = 6.283185307179586;
    const int gears[][2] = {{96, 52}, {105, 45}, {144, 60}, {150, 84}};
    for (int s = 0; s < 2; s++)
        for (size_t k = 0; k < sizeof gears / sizeof gears[0]; k++) {
            SpParams p = sp_defaults();
            p.ring_teeth = gears[k][0];
            p.wheel_teeth = gears[k][1];
            p.outside = s;
            p.rotation = 23;
            SpPath q = {0};
            assert(sp_generate(&p, 12000, &q));
            double r = (double)p.wheel_teeth / p.ring_teeth, d = r * p.pen;
            int laps = p.wheel_teeth / gcd(p.ring_teeth, p.wheel_teeth);
            double error = 0, rot = p.rotation * tau / 360;
            for (size_t i = 0; i < q.count; i++) {
                double t = tau * laps * (double)i / (q.count - 1), x, y;
                if (!s) {
                    x = (1 - r) * cos(t) + d * cos((1 - r) / r * t);
                    y = (1 - r) * sin(t) - d * sin((1 - r) / r * t);
                } else {
                    x = (1 + r) * cos(t) - d * cos((1 + r) / r * t);
                    y = (1 + r) * sin(t) - d * sin((1 + r) / r * t);
                }
                double a = x * cos(rot) - y * sin(rot), b = x * sin(rot) + y * cos(rot);
                error = fmax(error, hypot(a - q.points[i].x, b - q.points[i].y));
            }
            printf("%d/%d %s analytic maximum error %.6g\n", p.ring_teeth, p.wheel_teeth,
                   s ? "outside" : "inside", error);
            assert(error < .01);
            sp_path_free(&q);
        }
}
