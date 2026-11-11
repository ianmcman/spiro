#include "spiro.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    SpPoint triangle[] = {{0, 0}, {1, 0}, {0, 1}, {0, 0}};
    SpPath p = {triangle, 4}, out = {0};
    SpFourier f = {0};
    assert(sp_fourier(&p, 256, &f));
    assert(sp_synthesize(&f, f.count, 257, &out));
    assert(out.count == 257);
    double perimeter = 2 + sqrt(2.), err = 0;
    for (size_t i = 0; i < 257; i++) {
        double d = perimeter * i / 256., x, y;
        if (d <= 1) {
            x = d;
            y = 0;
        } else if (d <= 1 + sqrt(2.)) {
            double u = (d - 1) / sqrt(2.);
            x = 1 - u;
            y = u;
        } else {
            x = 0;
            y = perimeter - d;
        }
        err = fmax(err, hypot(out.points[i].x - x, out.points[i].y - y));
    }
    printf("triangle all-term reconstruction max error %.9g\n", err);
    assert(err < 1e-6);
    sp_path_free(&out);
    sp_fourier_free(&f);
    unsigned char mask[96 * 96];
    unsigned seed = 1;
    for (size_t i = 0; i < sizeof mask; i++) {
        seed = 1664525u * seed + 1013904223u;
        mask[i] = (seed >> 24) > 190;
    }
    assert(sp_trace_mask(mask, 96, 96, &out));
    for (size_t i = 0; i < out.count; i++) {
        assert(isfinite(out.points[i].x) && isfinite(out.points[i].y));
        assert(fabs(out.points[i].x) <= 1 && fabs(out.points[i].y) <= 1);
    }
    printf("disconnected noisy mask: %zu path points\n", out.count);
    sp_path_free(&out);
    p.points[0].x = NAN;
    assert(!sp_fourier(&p, 256, &f));
    puts("FFT roundtrip and disconnected-mask checks passed");
}
