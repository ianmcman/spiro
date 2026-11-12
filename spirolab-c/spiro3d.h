#ifndef SPIRO3D_H
#define SPIRO3D_H
#include "spiro.h"
#include <stddef.h>
#include <stdint.h>
#define SP3_MAX_ARMS 16
#define SP3_MAX_SAMPLES 128000
typedef struct {
    double x, y, z;
} Sp3Vec;
typedef struct {
    double length;
    int speed;
    double tilt, azimuth, phase;
} Sp3Arm;
typedef struct {
    Sp3Arm arms[SP3_MAX_ARMS];
    size_t arm_count, samples;
    double hue, hue_range, size_mm, tube_mm;
    int tube, spin, show_arms;
} Sp3Config;
typedef struct {
    Sp3Vec *points;
    size_t count;
    Sp3Vec center;
    double scale;
} Sp3Curve;
typedef struct {
    double yaw, pitch, zoom, pan_x, pan_y;
} Sp3View;
typedef struct {
    Sp3Vec *vertices;
    uint32_t *indices;
    size_t vertex_count, triangle_count;
} Sp3Mesh;
Sp3Config sp3_defaults(void);
Sp3View sp3_default_view(void);
int sp3_valid(const Sp3Config *c);
int sp3_joints(const Sp3Config *c, double angle, Sp3Vec out[SP3_MAX_ARMS + 1]);
int sp3_point(const Sp3Config *c, double angle, Sp3Vec *out);
int sp3_curve(const Sp3Config *c, Sp3Curve *out);
void sp3_curve_free(Sp3Curve *c);
int sp3_lift(const SpPath *path, Sp3Config *out);
int sp3_mesh(const Sp3Curve *curve, double size_mm, double radius_mm, unsigned sides, Sp3Mesh *out);
void sp3_mesh_free(Sp3Mesh *mesh);
int sp3_stl(const Sp3Mesh *mesh, unsigned char **bytes, size_t *size);
#endif
