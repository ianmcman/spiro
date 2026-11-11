#include "spiro.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    unsigned char mask[32 * 32] = {0};
    SpPath p = {0};
    SpFourier f = {0};
    assert(!sp_trace_mask(mask, 32, 32, &p));
    for (int i = 4; i < 28; i++)
        mask[16 * 32 + i] = 255;
    assert(sp_trace_mask(mask, 32, 32, &p));
    assert(p.count > 2);
    for (size_t i = 0; i < p.count; i++)
        assert(isfinite(p.points[i].x) && isfinite(p.points[i].y));
    assert(sp_fourier(&p, 256, &f));
    SpPath q = {0};
    assert(sp_synthesize(&f, f.count, 256, &q));
    assert(q.count >= 256);
    sp_path_free(&p);
    sp_path_free(&q);
    sp_fourier_free(&f);
    p.count = 257;
    p.points = calloc(p.count, sizeof(SpPoint));
    assert(p.points);
    for (size_t i = 0; i < 257; i++) {
        double t = 6.283185307179586 * (double)i / 256;
        p.points[i] = (SpPoint){cos(t), sin(t)};
    }
    assert(sp_fourier(&p, 256, &f));
    assert(sp_synthesize(&f, 1, 257, &q));
    for (size_t i = 0; i < q.count; i++)
        assert(fabs(hypot(q.points[i].x, q.points[i].y) - 1) < 1e-5);
    assert(hypot(q.points[0].x - q.points[q.count - 1].x, q.points[0].y - q.points[q.count - 1].y) <
           1e-6);
    sp_path_free(&p);
    sp_path_free(&q);
    sp_fourier_free(&f);
    assert(!sp_fourier(&p, 256, &f));
    assert(!sp_trace_mask(NULL, 10, 10, &p));
    sp_fourier_free(NULL);
    puts("tracing: empty masks, strokes, FFT circle reconstruction, closure and validation passed");
}
