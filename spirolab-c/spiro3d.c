#include "spiro3d.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#define TAU 6.283185307179586476925286766559
static Sp3Vec add(Sp3Vec a, Sp3Vec b) {
    return (Sp3Vec){a.x + b.x, a.y + b.y, a.z + b.z};
}
static Sp3Vec sub(Sp3Vec a, Sp3Vec b) {
    return (Sp3Vec){a.x - b.x, a.y - b.y, a.z - b.z};
}
static Sp3Vec mul(Sp3Vec a, double k) {
    return (Sp3Vec){a.x * k, a.y * k, a.z * k};
}
static double dot(Sp3Vec a, Sp3Vec b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
static Sp3Vec cross(Sp3Vec a, Sp3Vec b) {
    return (Sp3Vec){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
static double norm(Sp3Vec a) {
    return sqrt(dot(a, a));
}
static Sp3Vec unit(Sp3Vec a) {
    double n = norm(a);
    return n > 1e-14 ? mul(a, 1 / n) : (Sp3Vec){1, 0, 0};
}
static Sp3Vec rotate(Sp3Vec v, Sp3Vec axis, double angle) {
    double c = cos(angle), s = sin(angle);
    return add(add(mul(v, c), mul(cross(axis, v), s)), mul(axis, dot(axis, v) * (1 - c)));
}
static Sp3Vec perpendicular(Sp3Vec v) {
    return unit(cross(v, fabs(v.x) < .9 ? (Sp3Vec){1, 0, 0} : (Sp3Vec){0, 0, 1}));
}
static void frames(const Sp3Config *c, Sp3Vec axis[16], Sp3Vec direction[16]) {
    for (size_t i = 0; i < c->arm_count; i++) {
        Sp3Arm a = c->arms[i];
        double t = a.tilt * TAU / 360, z = a.azimuth * TAU / 360;
        axis[i] = (Sp3Vec){sin(t) * cos(z), cos(t), sin(t) * sin(z)};
        direction[i] = mul(rotate(perpendicular(axis[i]), axis[i], a.phase * TAU / 360), a.length);
    }
}
Sp3Config sp3_defaults(void) {
    Sp3Config c = {.arm_count = 3,
                   .samples = 8000,
                   .hue = 190,
                   .hue_range = 240,
                   .size_mm = 80,
                   .tube_mm = 1.2,
                   .spin = 1};
    c.arms[0] = (Sp3Arm){1, 1, 90, 0, 0};
    c.arms[1] = (Sp3Arm){.6, 5, 40, 70, 0};
    c.arms[2] = (Sp3Arm){.35, -9, 110, 200, 0};
    return c;
}
Sp3View sp3_default_view(void) {
    return (Sp3View){.yaw = .6, .pitch = .4, .zoom = 1};
}
int sp3_valid(const Sp3Config *c) {
    if (!c || c->arm_count < 1 || c->arm_count > 16 || c->samples < 1000 ||
        c->samples > SP3_MAX_SAMPLES)
        return 0;
    if (!isfinite(c->hue) || c->hue < 0 || c->hue > 360 || !isfinite(c->hue_range) ||
        c->hue_range < 0 || c->hue_range > 360 || !isfinite(c->size_mm) || c->size_mm < 1 ||
        c->size_mm > 1000 || !isfinite(c->tube_mm) || c->tube_mm < .05 || c->tube_mm > 20)
        return 0;
    if ((c->tube != 0 && c->tube != 1) || (c->spin != 0 && c->spin != 1) ||
        (c->show_arms != 0 && c->show_arms != 1))
        return 0;
    double total = 0;
    for (size_t i = 0; i < c->arm_count; i++) {
        Sp3Arm a = c->arms[i];
        if (!isfinite(a.length) || a.length < 0 || a.length > 3 || a.speed < -300 ||
            a.speed > 300 || !isfinite(a.tilt) || a.tilt < 0 || a.tilt > 180 ||
            !isfinite(a.azimuth) || a.azimuth < 0 || a.azimuth > 360 || !isfinite(a.phase) ||
            a.phase < 0 || a.phase > 360)
            return 0;
        total += a.length;
    }
    return total > 1e-9;
}
int sp3_joints(const Sp3Config *c, double angle, Sp3Vec out[17]) {
    if (!out || !sp3_valid(c) || !isfinite(angle) || fabs(angle) > 1e9)
        return 0;
    Sp3Vec axis[16], direction[16];
    frames(c, axis, direction);
    out[0] = (Sp3Vec){0};
    for (size_t k = 0; k < c->arm_count; k++) {
        Sp3Vec v = {0};
        for (size_t j = k + 1; j-- > 0;)
            v = rotate(add(v, direction[j]), axis[j], c->arms[j].speed * angle);
        out[k + 1] = v;
    }
    return 1;
}
int sp3_point(const Sp3Config *c, double angle, Sp3Vec *out) {
    Sp3Vec p[17];
    if (!out || !sp3_joints(c, angle, p))
        return 0;
    *out = p[c->arm_count];
    return 1;
}
int sp3_curve(const Sp3Config *c, Sp3Curve *out) {
    if (!out)
        return 0;
    *out = (Sp3Curve){0};
    if (!sp3_valid(c))
        return 0;
    Sp3Vec *p = malloc((c->samples + 1) * sizeof *p);
    if (!p)
        return 0;
    Sp3Vec axis[16], direction[16], lo = {0}, hi = {0};
    frames(c, axis, direction);
    for (size_t i = 0; i < c->samples; i++) {
        double t = TAU * i / c->samples;
        Sp3Vec v = {0};
        for (size_t k = c->arm_count; k-- > 0;)
            v = rotate(add(v, direction[k]), axis[k], c->arms[k].speed * t);
        p[i] = v;
        if (!i)
            lo = hi = v;
        lo = (Sp3Vec){fmin(lo.x, v.x), fmin(lo.y, v.y), fmin(lo.z, v.z)};
        hi = (Sp3Vec){fmax(hi.x, v.x), fmax(hi.y, v.y), fmax(hi.z, v.z)};
    }
    p[c->samples] = p[0];
    out->points = p;
    out->count = c->samples + 1;
    out->center = mul(add(lo, hi), .5);
    double span = fmax(hi.x - lo.x, fmax(hi.y - lo.y, hi.z - lo.z));
    out->scale = span > 1e-6 ? 2 / span : 1;
    return 1;
}
void sp3_curve_free(Sp3Curve *c) {
    if (c) {
        free(c->points);
        *c = (Sp3Curve){0};
    }
}
int sp3_lift(const SpPath *path, Sp3Config *out) {
    if (!out || !path || path->count < 3 || path->count > 250000)
        return 0;
    SpFourier f = {0};
    if (!sp_fourier(path, 4096, &f))
        return 0;
    Sp3Config c = sp3_defaults();
    c.arm_count = 0;
    int cumulative = 0;
    double largest = 0;
    for (size_t i = 0; i < f.count && c.arm_count < 16; i++) {
        SpTerm t = f.terms[i];
        double length = hypot(t.re, t.im);
        if (!t.frequency || abs(t.frequency) > 150 || length < 1e-6)
            continue;
        int speed = -t.frequency - cumulative;
        if (speed < -300 || speed > 300)
            continue;
        if (!c.arm_count)
            largest = length;
        double phase = atan2(-t.re, -t.im) * 360 / TAU;
        if (phase < 0)
            phase += 360;
        c.arms[c.arm_count++] = (Sp3Arm){length / largest, speed, 0, 0, phase};
        cumulative = -t.frequency;
    }
    sp_fourier_free(&f);
    if (!c.arm_count)
        return 0;
    if (c.arm_count > 1)
        c.arms[1].tilt = 30;
    *out = c;
    return 1;
}
static Sp3Vec transport(Sp3Vec normal, Sp3Vec from, Sp3Vec to) {
    Sp3Vec axis = cross(from, to);
    double s = norm(axis), c = fmax(-1, fmin(1, dot(from, to)));
    if (s > 1e-12)
        normal = rotate(normal, mul(axis, 1 / s), atan2(s, c));
    normal = sub(normal, mul(to, dot(normal, to)));
    return norm(normal) > 1e-12 ? unit(normal) : perpendicular(to);
}
int sp3_mesh(const Sp3Curve *curve, double size_mm, double radius, unsigned sides, Sp3Mesh *out) {
    if (!out)
        return 0;
    *out = (Sp3Mesh){0};
    if (!curve || !curve->points || curve->count < 4 || curve->count > SP3_MAX_SAMPLES + 1 ||
        !isfinite(size_mm) || size_mm < 1 || size_mm > 1000 || !isfinite(radius) || radius < .05 ||
        radius > 20 || sides < 3 || sides > 32)
        return 0;
    size_t n = curve->count - 1;
    Sp3Vec lo = curve->points[0], hi = lo;
    for (size_t i = 0; i < n; i++) {
        Sp3Vec p = curve->points[i];
        if (!isfinite(p.x) || !isfinite(p.y) || !isfinite(p.z) || fabs(p.x) > 1e6 ||
            fabs(p.y) > 1e6 || fabs(p.z) > 1e6)
            return 0;
        lo = (Sp3Vec){fmin(lo.x, p.x), fmin(lo.y, p.y), fmin(lo.z, p.z)};
        hi = (Sp3Vec){fmax(hi.x, p.x), fmax(hi.y, p.y), fmax(hi.z, p.z)};
    }
    Sp3Vec closing = sub(curve->points[n], curve->points[0]);
    if (!isfinite(closing.x) || !isfinite(closing.y) || !isfinite(closing.z) ||
        norm(closing) > 1e-9)
        return 0;
    double span = fmax(hi.x - lo.x, fmax(hi.y - lo.y, hi.z - lo.z));
    if (!isfinite(span) || span < 1e-9)
        return 0;
    Sp3Vec center = mul(add(lo, hi), .5);
    double scale = size_mm / span;
    Sp3Vec *t = malloc(n * sizeof *t), *normal = malloc(n * sizeof *normal),
           *vertices = malloc(n * sides * sizeof *vertices);
    uint32_t *idx = malloc(n * sides * 6 * sizeof *idx);
    if (!t || !normal || !vertices || !idx) {
        free(t);
        free(normal);
        free(vertices);
        free(idx);
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        Sp3Vec d = sub(curve->points[(i + 1) % n], curve->points[(i + n - 1) % n]);
        if (norm(d) < 1e-12)
            d = sub(curve->points[(i + 1) % n], curve->points[i]);
        t[i] = norm(d) > 1e-12 ? unit(d) : (i ? t[i - 1] : (Sp3Vec){1, 0, 0});
    }
    normal[0] = perpendicular(t[0]);
    for (size_t i = 1; i < n; i++)
        normal[i] = transport(normal[i - 1], t[i - 1], t[i]);
    Sp3Vec end = transport(normal[n - 1], t[n - 1], t[0]);
    double twist = atan2(dot(t[0], cross(end, normal[0])), dot(end, normal[0]));
    for (size_t i = 0; i < n; i++) {
        Sp3Vec a = rotate(normal[i], t[i], twist * (double)i / n), b = cross(t[i], a),
               p = mul(sub(curve->points[i], center), scale);
        for (unsigned j = 0; j < sides; j++) {
            double angle = TAU * j / sides;
            vertices[i * sides + j] =
                add(p, mul(add(mul(a, cos(angle)), mul(b, sin(angle))), radius));
            uint32_t A = (uint32_t)(i * sides + j), B = (uint32_t)(((i + 1) % n) * sides + j),
                     C = (uint32_t)(((i + 1) % n) * sides + (j + 1) % sides),
                     D = (uint32_t)(i * sides + (j + 1) % sides);
            size_t q = (i * sides + j) * 6;
            idx[q] = A;
            idx[q + 1] = D;
            idx[q + 2] = C;
            idx[q + 3] = A;
            idx[q + 4] = C;
            idx[q + 5] = B;
        }
    }
    free(t);
    free(normal);
    *out = (Sp3Mesh){vertices, idx, n * sides, n * sides * 2};
    return 1;
}
void sp3_mesh_free(Sp3Mesh *m) {
    if (m) {
        free(m->vertices);
        free(m->indices);
        *m = (Sp3Mesh){0};
    }
}
static void put32(unsigned char *p, uint32_t v) {
    for (int i = 0; i < 4; i++)
        p[i] = (unsigned char)(v >> (8 * i));
}
static void putfloat(unsigned char *p, double d) {
    float f = (float)d;
    uint32_t u;
    memcpy(&u, &f, 4);
    put32(p, u);
}
int sp3_stl(const Sp3Mesh *m, unsigned char **bytes, size_t *size) {
    if (!bytes || !size)
        return 0;
    *bytes = NULL;
    *size = 0;
    if (!m || !m->vertices || !m->indices || !m->triangle_count || m->triangle_count > 2048000 ||
        m->vertex_count > 1024000)
        return 0;
    size_t length = 84 + m->triangle_count * 50;
    unsigned char *b = calloc(length, 1);
    if (!b)
        return 0;
    memcpy(b, "SpiroLab native C closed tube (millimetres)", 41);
    put32(b + 80, (uint32_t)m->triangle_count);
    for (size_t i = 0; i < m->triangle_count; i++) {
        Sp3Vec v[3];
        for (int j = 0; j < 3; j++) {
            uint32_t id = m->indices[i * 3 + j];
            if (id >= m->vertex_count) {
                free(b);
                return 0;
            }
            v[j] = m->vertices[id];
            if (!isfinite(v[j].x) || !isfinite(v[j].y) || !isfinite(v[j].z) || fabs(v[j].x) > 1e7 ||
                fabs(v[j].y) > 1e7 || fabs(v[j].z) > 1e7) {
                free(b);
                return 0;
            }
        }
        Sp3Vec normal = unit(cross(sub(v[1], v[0]), sub(v[2], v[0])));
        unsigned char *p = b + 84 + 50 * i;
        putfloat(p, normal.x);
        putfloat(p + 4, normal.y);
        putfloat(p + 8, normal.z);
        for (int j = 0; j < 3; j++) {
            putfloat(p + 12 + 12 * j, v[j].x);
            putfloat(p + 16 + 12 * j, v[j].y);
            putfloat(p + 20 + 12 * j, v[j].z);
        }
    }
    *bytes = b;
    *size = length;
    return 1;
}
