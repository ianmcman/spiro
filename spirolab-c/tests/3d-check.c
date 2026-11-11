#include "spiro3d.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PI 3.1415926535897932384626433832795
static double distance(Sp3Vec a, Sp3Vec b) {
    return hypot(hypot(a.x - b.x, a.y - b.y), a.z - b.z);
}
typedef struct {
    uint32_t a, b;
    int direction;
} Edge;
static int order(const void *a, const void *b) {
    const Edge *x = a, *y = b;
    if (x->a != y->a)
        return (x->a > y->a) - (x->a < y->a);
    return (x->b > y->b) - (x->b < y->b);
}
static uint32_t u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
int main(void) {
    Sp3Config c = sp3_defaults();
    assert(sp3_valid(&c));
    c.arm_count = 1;
    c.arms[0] = (Sp3Arm){1, 1, 0, 0, 0};
    for (int i = 0; i < 32; i++) {
        double t = 2 * PI * i / 32;
        Sp3Vec p;
        assert(sp3_point(&c, t, &p));
        assert(distance(p, (Sp3Vec){-sin(t), 0, -cos(t)}) < 1e-12);
    }
    c.arm_count = 2;
    c.arms[1] = (Sp3Arm){.6, 5, 0, 0, 0};
    for (int i = 0; i < 32; i++) {
        double t = 2 * PI * i / 32;
        Sp3Vec p;
        assert(sp3_point(&c, t, &p));
        assert(distance(p, (Sp3Vec){-sin(t) - .6 * sin(6 * t), 0, -cos(t) - .6 * cos(6 * t)}) <
               1e-12);
    }
    c.arms[1] = (Sp3Arm){.6, 1, 90, 0, 0};
    Sp3Vec p;
    assert(sp3_point(&c, PI / 2, &p));
    assert(distance(p, (Sp3Vec){-1.6, 0, 0}) < 1e-12);
    c = sp3_defaults();
    c.arm_count = 16;
    for (int i = 0; i < 16; i++)
        c.arms[i] = (Sp3Arm){.1 + i * .1, i % 2 ? -(i + 1) : i + 1, i * 11, i * 23, i * 17};
    Sp3Vec start, end, joints[17];
    assert(sp3_point(&c, 0, &start));
    assert(sp3_point(&c, 2 * PI, &end));
    assert(distance(start, end) < 1e-12);
    assert(sp3_joints(&c, .4321, joints));
    assert(sp3_point(&c, .4321, &p));
    assert(distance(p, joints[16]) < 1e-12);
    Sp3Curve curve = {0};
    assert(sp3_curve(&c, &curve));
    assert(curve.count == c.samples + 1 &&
           distance(curve.points[0], curve.points[curve.count - 1]) == 0);
    sp3_curve_free(&curve);
    c.arm_count = 17;
    assert(!sp3_valid(&c));
    c = sp3_defaults();
    c.arms[0].length = NAN;
    assert(!sp3_curve(&c, &curve));
    c = sp3_defaults();
    c.arms[0].speed = 301;
    assert(!sp3_valid(&c));
    c = sp3_defaults();
    c.arm_count = 1;
    c.samples = 1000;
    c.arms[0] = (Sp3Arm){1, 1, 0, 0, 0};
    assert(sp3_curve(&c, &curve));
    Sp3Mesh mesh = {0};
    assert(sp3_mesh(&curve, 80, 1.2, 12, &mesh));
    assert(mesh.vertex_count == 12000 && mesh.triangle_count == 24000);
    size_t edges = mesh.triangle_count * 3;
    Edge *e = malloc(edges * sizeof *e);
    assert(e);
    double volume = 0;
    for (size_t i = 0; i < mesh.triangle_count; i++) {
        Sp3Vec v[3];
        for (int j = 0; j < 3; j++) {
            uint32_t a = mesh.indices[3 * i + j], b = mesh.indices[3 * i + (j + 1) % 3];
            assert(a < mesh.vertex_count && b < mesh.vertex_count);
            e[3 * i + j] = (Edge){a < b ? a : b, a < b ? b : a, a < b ? 1 : -1};
            v[j] = mesh.vertices[a];
        }
        volume += (v[0].x * (v[1].y * v[2].z - v[1].z * v[2].y) +
                   v[0].y * (v[1].z * v[2].x - v[1].x * v[2].z) +
                   v[0].z * (v[1].x * v[2].y - v[1].y * v[2].x)) /
                  6;
    }
    qsort(e, edges, sizeof *e, order);
    for (size_t i = 0; i < edges; i += 2) {
        assert(e[i].a == e[i + 1].a && e[i].b == e[i + 1].b &&
               e[i].direction == -e[i + 1].direction);
        if (i + 2 < edges)
            assert(e[i].a != e[i + 2].a || e[i].b != e[i + 2].b);
    }
    free(e);
    double expected = 2 * PI * 40 * (12. / 2 * 1.2 * 1.2 * sin(2 * PI / 12));
    assert(volume > 0 && fabs(volume / expected - 1) < .002);
    unsigned char *bytes = NULL;
    size_t length = 0;
    assert(sp3_stl(&mesh, &bytes, &length));
    assert(length == 84 + 50 * mesh.triangle_count && u32(bytes + 80) == mesh.triangle_count);
    free(bytes);
    sp3_mesh_free(&mesh);
    curve.points[curve.count - 1].x += 1;
    assert(!sp3_mesh(&curve, 80, 1.2, 12, &mesh));
    sp3_curve_free(&curve);
    c.arms[0].speed = 0;
    assert(sp3_curve(&c, &curve));
    assert(!sp3_mesh(&curve, 80, 1.2, 12, &mesh));
    sp3_curve_free(&curve);
    SpPoint circle[257];
    for (int i = 0; i <= 256; i++)
        circle[i] = (SpPoint){cos(2 * PI * i / 256), sin(2 * PI * i / 256)};
    SpPath path = {circle, 257};
    assert(sp3_lift(&path, &c));
    assert(sp3_valid(&c) && c.arm_count >= 1 && c.arm_count <= 16);
    printf("3D: analytic chains, rotated axes, 16-arm closure, joints, watertight oriented mesh, "
           "torus volume %.3f, binary STL and validation passed\n",
           volume);
}
