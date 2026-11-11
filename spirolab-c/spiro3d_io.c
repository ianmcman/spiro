#include "spiro3d_io.h"
#include <glib/gstdio.h>
#include <math.h>
#include <stdlib.h>
#define TAU 6.283185307179586476925286766559
#define ERR (g_quark_from_static_string("spirolab-3d"))
static gboolean view_valid(Sp3View v);
typedef struct {
    double x, y, z, perspective;
    int visible;
} Screen;
typedef struct {
    Screen a, b;
    double depth, fraction;
} Segment;
static Sp3Vec camera(Sp3Vec p, const Sp3View *v) {
    double c = cos(v->yaw), s = sin(v->yaw);
    Sp3Vec q = {c * p.x + s * p.z, p.y, -s * p.x + c * p.z};
    c = cos(v->pitch);
    s = sin(v->pitch);
    return (Sp3Vec){q.x, c * q.y - s * q.z, s * q.y + c * q.z};
}
static Screen project(Sp3Vec p, const Sp3Curve *c, const Sp3View *v, int width, int height) {
    p = (Sp3Vec){(p.x - c->center.x) * c->scale, (p.y - c->center.y) * c->scale,
                 (p.z - c->center.z) * c->scale};
    p = camera(p, v);
    double m = MIN(width, height), perspective = 4.5 / (4.5 - p.z);
    Screen s = {width / 2. + v->pan_x * m + p.x * .39 * m * v->zoom * perspective,
                height / 2. + v->pan_y * m - p.y * .39 * m * v->zoom * perspective, p.z,
                perspective, 0};
    s.visible = p.z < 4.3 && isfinite(s.x) && isfinite(s.y) && fabs(s.x) < 1e7 && fabs(s.y) < 1e7;
    return s;
}
static int depth_order(const void *a, const void *b) {
    double x = ((const Segment *)a)->depth, y = ((const Segment *)b)->depth;
    return (x > y) - (x < y);
}
static void rgb(double hue, double value, double *r, double *g, double *b) {
    double h = hue / 60.;
    h = fmod(h, 6);
    if (h < 0)
        h += 6;
    double c = value * .75, x = c * (1 - fabs(fmod(h, 2) - 1)), m = value - c;
    double R = 0, G = 0, B = 0;
    switch ((int)h) {
    case 0:
        R = c;
        G = x;
        break;
    case 1:
        R = x;
        G = c;
        break;
    case 2:
        G = c;
        B = x;
        break;
    case 3:
        G = x;
        B = c;
        break;
    case 4:
        R = x;
        B = c;
        break;
    default:
        R = c;
        B = x;
        break;
    }
    *r = R + m;
    *g = G + m;
    *b = B + m;
}
static void stroke(cairo_t *cr, Segment s, double width, double r, double g, double b) {
    cairo_set_line_width(cr, width);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_move_to(cr, s.a.x, s.a.y);
    cairo_line_to(cr, s.b.x, s.b.y);
    cairo_stroke(cr);
}
typedef struct {
    Screen p[3];
    double depth, fraction, light;
} Face;
static int face_order(const void *a, const void *b) {
    double x = ((const Face *)a)->depth, y = ((const Face *)b)->depth;
    return (x > y) - (x < y);
}
static gboolean draw_tube(cairo_t *cr, int width, int height, const Sp3Config *cfg,
                          const Sp3Curve *curve, const Sp3View *view, double progress) {
    size_t n = MIN(curve->count - 1, (size_t)(width >= 2000 ? 4000 : 1200));
    Sp3Curve sampled = {.count = n + 1};
    sampled.points = malloc((n + 1) * sizeof(Sp3Vec));
    if (!sampled.points)
        return FALSE;
    for (size_t i = 0; i < n; i++)
        sampled.points[i] = curve->points[i * (curve->count - 1) / n];
    sampled.points[n] = sampled.points[0];
    unsigned sides = width >= 2000 ? 12 : 8;
    Sp3Mesh mesh = {0};
    if (!sp3_mesh(&sampled, cfg->size_mm, cfg->tube_mm, sides, &mesh)) {
        sp3_curve_free(&sampled);
        return FALSE;
    }
    sp3_curve_free(&sampled);
    Face *faces = malloc(mesh.triangle_count * sizeof(Face));
    if (!faces) {
        sp3_mesh_free(&mesh);
        return FALSE;
    }
    Sp3Curve fit = {.scale = 2. / cfg->size_mm};
    size_t count = 0;
    for (size_t i = 0; i < mesh.triangle_count; i++) {
        size_t ring = i / (2 * sides);
        if ((double)ring / n >= progress)
            continue;
        Face f = {.fraction = (double)ring / n};
        Sp3Vec v[3];
        gboolean visible = TRUE;
        for (int j = 0; j < 3; j++) {
            v[j] = mesh.vertices[mesh.indices[3 * i + j]];
            f.p[j] = project(v[j], &fit, view, width, height);
            visible = visible && f.p[j].visible;
            f.depth += f.p[j].z / 3;
        }
        if (!visible)
            continue;
        Sp3Vec a = {v[1].x - v[0].x, v[1].y - v[0].y, v[1].z - v[0].z};
        Sp3Vec b = {v[2].x - v[0].x, v[2].y - v[0].y, v[2].z - v[0].z};
        Sp3Vec normal = camera(
            (Sp3Vec){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}, view);
        double length = sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
        f.light = .3 + .7 * MAX(0., (-.4 * normal.x + .7 * normal.y + normal.z) /
                                        (1.28452325786651 * MAX(length, 1e-16)));
        faces[count++] = f;
    }
    qsort(faces, count, sizeof(Face), face_order);
    cairo_set_line_width(cr, .6);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    for (size_t i = 0; i < count; i++) {
        Face f = faces[i];
        double r, g, b;
        rgb(cfg->hue + cfg->hue_range * f.fraction, .98, &r, &g, &b);
        cairo_set_source_rgb(cr, r * f.light, g * f.light, b * f.light);
        cairo_move_to(cr, f.p[0].x, f.p[0].y);
        cairo_line_to(cr, f.p[1].x, f.p[1].y);
        cairo_line_to(cr, f.p[2].x, f.p[2].y);
        cairo_close_path(cr);
        cairo_fill_preserve(cr);
        cairo_stroke(cr);
    }
    free(faces);
    sp3_mesh_free(&mesh);
    return TRUE;
}
void sp3_draw(cairo_t *cr, int width, int height, const Sp3Config *cfg, const Sp3Curve *curve,
              const Sp3View *view, double progress, double arm_phase) {
    cairo_set_source_rgb(cr, .055, .06, .08);
    cairo_paint(cr);
    if (!curve || !curve->points || curve->count < 2 || width < 1 || height < 1)
        return;
    if (!cfg->tube || !draw_tube(cr, width, height, cfg, curve, view, progress)) {
        size_t n = curve->count - 1, shown = (size_t)(CLAMP(progress, 0., 1.) * n),
               max_segments = width >= 2000 ? 16000 : (8000),
               step = MAX((size_t)1, (n + max_segments - 1) / max_segments);
        Segment *segments = malloc((n / step + 2) * sizeof *segments);
        if (!segments)
            return;
        size_t count = 0;
        for (size_t i = 0; i < shown; i += step) {
            size_t end = MIN(i + step, shown);
            Screen a = project(curve->points[i], curve, view, width, height),
                   b = project(curve->points[end], curve, view, width, height);
            if (a.visible && b.visible)
                segments[count++] = (Segment){a, b, (a.z + b.z) / 2., (double)i / n};
        }
        qsort(segments, count, sizeof *segments, depth_order);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        for (size_t i = 0; i < count; i++) {
            Segment s = segments[i];
            double r, g, b;
            rgb(cfg->hue + cfg->hue_range * s.fraction,
                .7 + .25 * CLAMP((s.depth + 1.8) / 3.6, 0., 1.), &r, &g, &b);
            stroke(cr, s, MAX(1., MIN(width, height) / 650.), r, g, b);
        }
        free(segments);
    }
    if (cfg->show_arms) {
        Sp3Vec joints[17];
        if (sp3_joints(cfg, arm_phase * TAU, joints)) {
            for (size_t i = 0; i < cfg->arm_count; i++) {
                Screen a = project(joints[i], curve, view, width, height),
                       b = project(joints[i + 1], curve, view, width, height);
                if (!a.visible || !b.visible)
                    continue;
                cairo_set_source_rgba(cr, 1, .88, .48, .8);
                cairo_set_line_width(cr, 1.6);
                cairo_move_to(cr, a.x, a.y);
                cairo_line_to(cr, b.x, b.y);
                cairo_stroke(cr);
                cairo_arc(cr, a.x, a.y, 3, 0, TAU);
                cairo_fill(cr);
                if (i + 1 == cfg->arm_count) {
                    cairo_set_source_rgb(cr, 1, .3, .2);
                    cairo_arc(cr, b.x, b.y, 4, 0, TAU);
                    cairo_fill(cr);
                }
            }
        }
    }
    const Sp3Vec axes[] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const char *labels[] = {"X", "Y", "Z"};
    cairo_set_font_size(cr, 11);
    for (int i = 0; i < 3; i++) {
        Sp3Vec a = camera(axes[i], view);
        cairo_set_source_rgb(cr, i == 0 ? .95 : .35, i == 1 ? .9 : .35, i == 2 ? 1 : .35);
        cairo_set_line_width(cr, 2);
        cairo_move_to(cr, 45, height - 45);
        cairo_line_to(cr, 45 + 25 * a.x, height - 45 - 25 * a.y);
        cairo_stroke(cr);
        cairo_move_to(cr, 45 + 32 * a.x, height - 45 - 32 * a.y);
        cairo_show_text(cr, labels[i]);
    }
}
gboolean sp3_png(const char *file, const Sp3Config *cfg, const Sp3Curve *curve, const Sp3View *view,
                 GError **error) {
    if (!sp3_valid(cfg) || !curve || !curve->points || !view || !view_valid(*view)) {
        g_set_error(error, ERR, 1, "Invalid 3D image settings");
        return FALSE;
    }
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 2048, 2048);
    cairo_t *cr = cairo_create(s);
    sp3_draw(cr, 2048, 2048, cfg, curve, view, 1, 1);
    cairo_destroy(cr);
    cairo_status_t status = cairo_surface_write_to_png(s, file);
    cairo_surface_destroy(s);
    if (status != CAIRO_STATUS_SUCCESS) {
        g_set_error(error, ERR, 1, "PNG export failed: %s", cairo_status_to_string(status));
        return FALSE;
    }
    return TRUE;
}
gboolean sp3_save(const char *file, const Sp3Config *c, const Sp3View *v, GError **error) {
    if (!sp3_valid(c) || !v || !view_valid(*v)) {
        g_set_error(error, ERR, 1, "Invalid 3D settings");
        return FALSE;
    }
    GKeyFile *k = g_key_file_new();
    g_key_file_set_integer(k, "SpiroLab3D", "version", 1);
    g_key_file_set_integer(k, "SpiroLab3D", "arms", c->arm_count);
    g_key_file_set_integer(k, "SpiroLab3D", "samples", c->samples);
    double settings[] = {c->hue, c->hue_range, c->size_mm, c->tube_mm};
    g_key_file_set_double_list(k, "SpiroLab3D", "settings", settings, 4);
    g_key_file_set_boolean(k, "SpiroLab3D", "tube", c->tube);
    g_key_file_set_boolean(k, "SpiroLab3D", "spin", c->spin);
    g_key_file_set_boolean(k, "SpiroLab3D", "show_arms", c->show_arms);
    double view[] = {v->yaw, v->pitch, v->zoom, v->pan_x, v->pan_y};
    g_key_file_set_double_list(k, "SpiroLab3D", "view", view, 5);
    for (size_t i = 0; i < c->arm_count; i++) {
        char group[32];
        g_snprintf(group, sizeof group, "Arm%zu", i);
        Sp3Arm a = c->arms[i];
        double x[] = {a.length, a.tilt, a.azimuth, a.phase};
        g_key_file_set_double_list(k, group, "geometry", x, 4);
        g_key_file_set_integer(k, group, "speed", a.speed);
    }
    gsize length = 0;
    char *data = g_key_file_to_data(k, &length, error);
    gboolean ok = data && g_file_set_contents_full(file, data, length,
                                                   G_FILE_SET_CONTENTS_CONSISTENT |
                                                       G_FILE_SET_CONTENTS_DURABLE,
                                                   0600, error);
    g_free(data);
    g_key_file_free(k);
    return ok;
}
static gboolean view_valid(Sp3View v) {
    return isfinite(v.yaw) && fabs(v.yaw) <= 1e6 && isfinite(v.pitch) && fabs(v.pitch) <= 1.55 &&
           isfinite(v.zoom) && v.zoom >= .15 && v.zoom <= 10 && isfinite(v.pan_x) &&
           fabs(v.pan_x) <= 10 && isfinite(v.pan_y) && fabs(v.pan_y) <= 10;
}
gboolean sp3_load(const char *file, Sp3Config *out, Sp3View *view, GError **error) {
    if (!out || !view) {
        g_set_error(error, ERR, 1, "Missing output for 3D design");
        return FALSE;
    }
    GStatBuf st;
    GError *e = NULL;
    GKeyFile *k = g_key_file_new();
    Sp3Config c = sp3_defaults();
    Sp3View v = sp3_default_view();
    gboolean ok = FALSE;
    if (g_stat(file, &st) || st.st_size < 1 || st.st_size > 65536)
        goto done;
    if (!g_key_file_load_from_file(k, file, 0, &e))
        goto done;
    if (g_key_file_get_integer(k, "SpiroLab3D", "version", &e) != 1 || e)
        goto done;
    int n = g_key_file_get_integer(k, "SpiroLab3D", "arms", &e);
    if (e || n < 1 || n > 16)
        goto done;
    c.arm_count = n;
    n = g_key_file_get_integer(k, "SpiroLab3D", "samples", &e);
    if (e || n < 1000 || n > SP3_MAX_SAMPLES)
        goto done;
    c.samples = n;
    gsize count = 0;
    double *x = g_key_file_get_double_list(k, "SpiroLab3D", "settings", &count, &e);
    if (!x)
        goto done;
    if (count != 4) {
        g_free(x);
        goto done;
    }
    c.hue = x[0];
    c.hue_range = x[1];
    c.size_mm = x[2];
    c.tube_mm = x[3];
    g_free(x);
    c.tube = g_key_file_get_boolean(k, "SpiroLab3D", "tube", &e);
    if (e)
        goto done;
    c.spin = g_key_file_get_boolean(k, "SpiroLab3D", "spin", &e);
    if (e)
        goto done;
    c.show_arms = g_key_file_get_boolean(k, "SpiroLab3D", "show_arms", &e);
    if (e)
        goto done;
    x = g_key_file_get_double_list(k, "SpiroLab3D", "view", &count, &e);
    if (!x)
        goto done;
    if (count != 5) {
        g_free(x);
        goto done;
    }
    v = (Sp3View){x[0], x[1], x[2], x[3], x[4]};
    g_free(x);
    for (size_t i = 0; i < c.arm_count; i++) {
        char group[32];
        g_snprintf(group, sizeof group, "Arm%zu", i);
        x = g_key_file_get_double_list(k, group, "geometry", &count, &e);
        if (!x)
            goto done;
        if (count != 4) {
            g_free(x);
            goto done;
        }
        c.arms[i] = (Sp3Arm){x[0], 0, x[1], x[2], x[3]};
        g_free(x);
        c.arms[i].speed = g_key_file_get_integer(k, group, "speed", &e);
        if (e)
            goto done;
    }
    if (!sp3_valid(&c) || !view_valid(v))
        goto done;
    *out = c;
    *view = v;
    ok = TRUE;
done:
    g_key_file_free(k);
    if (!ok) {
        if (!e)
            g_set_error(&e, ERR, 1, "Invalid 3D design; current settings were preserved");
        g_propagate_error(error, e);
    }
    return ok;
}
gboolean sp3_export_stl(const char *file, const Sp3Config *cfg, GError **error) {
    if (!sp3_valid(cfg)) {
        g_set_error(error, ERR, 1, "Invalid 3D tube settings");
        return FALSE;
    }
    Sp3Config c = *cfg;
    Sp3Curve curve = {0};
    Sp3Mesh mesh = {0};
    unsigned char *data = NULL;
    size_t length = 0;
    gboolean ok = sp3_curve(&c, &curve) && sp3_mesh(&curve, c.size_mm, c.tube_mm, 12, &mesh) &&
                  sp3_stl(&mesh, &data, &length);
    if (ok)
        ok = g_file_set_contents_full(file, (const char *)data, length,
                                      G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE,
                                      0600, error);
    else
        g_set_error(
            error, ERR, 1,
            "Cannot build a tube from this curve. Give at least one moving arm a nonzero length.");
    free(data);
    sp3_mesh_free(&mesh);
    sp3_curve_free(&curve);
    return ok;
}
