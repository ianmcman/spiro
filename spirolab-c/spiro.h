#ifndef SPIRO_H
#define SPIRO_H
#include <stddef.h>
#define SP_MAX_POINTS 4194305
#define SP_MAX_TERMS 2097152
#define SP_MAX_DETAIL 8192
/* All coordinates normalized near [-1,1], ownership belongs to caller. */
typedef struct {
    double x, y;
} SpPoint;
typedef struct {
    SpPoint *points;
    size_t count;
} SpPath;
typedef struct {
    int ring_shape, wheel_shape; /* circle ellipse squircle heart rosette triangle square pentagon
                                    hexagon star burst */
    int ring_teeth, wheel_teeth, outside;
    double pen, rotation; /* radius fraction 0..1.5; degrees */
    int second;
    double second_ratio, second_speed;
    int second_outside;
} SpParams;
typedef struct {
    double re, im;
    int frequency;
} SpTerm;
typedef struct {
    SpTerm *terms;
    size_t count;
} SpFourier;
extern const char *sp_shape_names[11];
SpParams sp_defaults(void);
/* Return 1 on success, 0 on invalid input/allocation failure. out is reset on failure. */
int sp_generate(const SpParams *p, size_t max_samples, SpPath *out);
void sp_path_free(SpPath *path);
/* mask row-major width*height, nonzero represents ink. Bounded detail <=SP_MAX_DETAIL. */
int sp_trace_mask(const unsigned char *mask, int width, int height, SpPath *out);
/* Closed, uniformly resampled path, radix-2 FFT. Terms sorted largest amplitude first. */
int sp_fourier(const SpPath *path, size_t sample_count, SpFourier *out);
int sp_synthesize(const SpFourier *f, size_t terms, size_t samples, SpPath *out);
void sp_fourier_free(SpFourier *f);
#endif
