#include "spiro.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FAST_WEIGHT 0.03
#define INSIDE_CAP 0.97

const char *sp_shape_names[11] = {"circle", "ellipse",  "squircle", "heart", "rosette", "triangle",
                                  "square", "pentagon", "hexagon",  "star",  "burst"};

SpParams sp_defaults(void) {
    SpParams p;
    memset(&p, 0, sizeof(p));
    p.ring_teeth = 96;
    p.wheel_teeth = 52;
    p.pen = 0.8;
    p.second_ratio = 0.25;
    p.second_speed = 2.0;
    return p;
}

void sp_path_free(SpPath *path) {
    if (path) {
        free(path->points);
        path->points = NULL;
        path->count = 0;
    }
}

typedef struct {
    int k;
    int m;
} Closure;

static Closure calc_closure(double Pr, double Pw) {
    double ratio = (Pw > 0.0 ? Pr / Pw : 1.0);
    Closure best = {1, 1};
    double best_err = 1e30;
    for (int k = 1; k <= 400; k++) {
        double x = (double)k * ratio;
        int m = (int)round(x);
        if (m < 1)
            continue;
        double err = fabs(x - (double)m) / x;
        if (err < best_err) {
            best.k = k;
            best.m = m;
            best_err = err;
        }
        if (err < 1e-3) {
            return (Closure){k, m};
        }
    }
    return best;
}

typedef struct {
    double x, y;
} Vec2;

typedef struct {
    size_t n;
    double *xs;
    double *ys;
    double *cum;
    double *tx;
    double *ty;
    double *turn;
    double T;
    double P;
    double *wt;
    int fast;
} Curve;

static void free_curve(Curve *c) {
    if (!c)
        return;
    free(c->xs);
    free(c->ys);
    free(c->cum);
    free(c->tx);
    free(c->ty);
    free(c->turn);
    free(c->wt);
    memset(c, 0, sizeof(*c));
}

static size_t gen_shape_raw(int shape, Vec2 **out_pts) {
    if (shape < 5) {
        size_t n = 2048;
        Vec2 *pts = malloc(n * sizeof(Vec2));
        if (!pts)
            return 0;
        for (size_t i = 0; i < n; i++) {
            double t = (2.0 * M_PI * (double)i) / (double)n;
            if (shape == 0) {
                pts[i].x = cos(t);
                pts[i].y = sin(t);
            } else if (shape == 1) {
                pts[i].x = cos(t);
                pts[i].y = 0.6 * sin(t);
            } else if (shape == 2) {
                double c = cos(t), s = sin(t);
                double sgn_c = (c < 0.0 ? -1.0 : 1.0);
                double sgn_s = (s < 0.0 ? -1.0 : 1.0);
                pts[i].x = sgn_c * sqrt(fabs(c));
                pts[i].y = sgn_s * sqrt(fabs(s));
            } else if (shape == 3) {
                double st = sin(t);
                pts[i].x = 16.0 * st * st * st;
                pts[i].y =
                    -(13.0 * cos(t) - 5.0 * cos(2.0 * t) - 2.0 * cos(3.0 * t) - cos(4.0 * t));
            } else if (shape == 4) {
                double r = 0.7 + 0.3 * cos(6.0 * t);
                pts[i].x = r * cos(t);
                pts[i].y = r * sin(t);
            }
        }
        *out_pts = pts;
        return n;
    } else {
        int V = 0, sub = 0;
        if (shape == 5) {
            V = 3;
            sub = 240;
        } else if (shape == 6) {
            V = 4;
            sub = 180;
        } else if (shape == 7) {
            V = 5;
            sub = 150;
        } else if (shape == 8) {
            V = 6;
            sub = 120;
        } else if (shape == 9) {
            V = 10;
            sub = 72;
        } else if (shape == 10) {
            V = 24;
            sub = 30;
        }

        Vec2 v[24];
        for (int i = 0; i < V; i++) {
            double a = -M_PI / 2.0 + (2.0 * M_PI * (double)i) / (double)V;
            double r = 1.0;
            if (shape == 9) {
                r = (i % 2 ? 0.4 : 1.0);
            } else if (shape == 10) {
                r = (i % 2 ? 0.62 : 1.0);
            }
            v[i].x = r * cos(a);
            v[i].y = r * sin(a);
        }
        size_t n = (size_t)(V * sub);
        Vec2 *pts = malloc(n * sizeof(Vec2));
        if (!pts)
            return 0;
        size_t idx = 0;
        for (int i = 0; i < V; i++) {
            Vec2 a = v[i];
            Vec2 b = v[(i + 1) % V];
            for (int j = 0; j < sub; j++) {
                double t = (double)j / (double)sub;
                pts[idx].x = a.x + (b.x - a.x) * t;
                pts[idx].y = a.y + (b.y - a.y) * t;
                idx++;
            }
        }
        *out_pts = pts;
        return n;
    }
}

typedef struct {
    int i;
    double turn, ax, ay, bx, by;
    double t, r;
} Corner;

static int fillet_curve(double **p_xs, double **p_ys, size_t *p_n, uint8_t **p_fast, double rho,
                        double bend) {
    size_t n = *p_n;
    double *xs = *p_xs;
    double *ys = *p_ys;

    double *cum = malloc((n + 1) * sizeof(double));
    if (!cum)
        return 0;
    cum[0] = 0.0;
    for (size_t i = 0; i < n; i++) {
        size_t j = (i + 1) % n;
        cum[i + 1] = cum[i] + hypot(xs[j] - xs[i], ys[j] - ys[i]);
    }

    Corner *corners = malloc(n * sizeof(Corner));
    if (!corners) {
        free(cum);
        return 0;
    }
    size_t num_corners = 0;
    for (size_t i = 0; i < n; i++) {
        size_t h = (i + n - 1) % n;
        size_t j = (i + 1) % n;
        double ax = xs[i] - xs[h], ay = ys[i] - ys[h];
        double bx = xs[j] - xs[i], by = ys[j] - ys[i];
        double turn = atan2(ax * by - ay * bx, ax * bx + ay * by);
        if (turn * bend > 0.2 && fabs(turn) < 2.5) {
            corners[num_corners].i = (int)i;
            corners[num_corners].turn = turn;
            corners[num_corners].ax = ax;
            corners[num_corners].ay = ay;
            corners[num_corners].bx = bx;
            corners[num_corners].by = by;
            corners[num_corners].t = 0.0;
            corners[num_corners].r = 0.0;
            num_corners++;
        }
    }

    if (num_corners == 0) {
        free(cum);
        free(corners);
        *p_fast = NULL;
        return 1;
    }

    double P = cum[n];
    for (size_t q = 0; q < num_corners; q++) {
        Corner *c = &corners[q];
        Corner *prev = &corners[(q + num_corners - 1) % num_corners];
        Corner *next = &corners[(q + 1) % num_corners];

        double gap_prev = fmod(cum[c->i] - cum[prev->i], P);
        if (gap_prev < 0.0)
            gap_prev += P;
        if (gap_prev == 0.0)
            gap_prev = P;

        double gap_next = fmod(cum[next->i] - cum[c->i], P);
        if (gap_next < 0.0)
            gap_next += P;
        if (gap_next == 0.0)
            gap_next = P;

        double tn = tan(fabs(c->turn) / 2.0);
        double t_val = rho * tn;
        if (0.5 * gap_prev < t_val)
            t_val = 0.5 * gap_prev;
        if (0.5 * gap_next < t_val)
            t_val = 0.5 * gap_next;
        c->t = t_val;
        c->r = t_val / tn;
    }

    size_t cap = n + 16384;
    double *ox = malloc(cap * sizeof(double));
    double *oy = malloc(cap * sizeof(double));
    uint8_t *fast = malloc(cap * sizeof(uint8_t));
    if (!ox || !oy || !fast) {
        free(cum);
        free(corners);
        free(ox);
        free(oy);
        free(fast);
        return 0;
    }
    size_t out_len = 0;

    for (size_t q = 0; q < num_corners; q++) {
        Corner *c = &corners[q];
        double la = hypot(c->ax, c->ay);
        double sg = (c->turn < 0.0 ? -1.0 : (c->turn > 0.0 ? 1.0 : 0.0));
        double Ax = xs[c->i] - (c->ax / la) * c->t;
        double Ay = ys[c->i] - (c->ay / la) * c->t;
        double Cx = Ax - sg * (c->ay / la) * c->r;
        double Cy = Ay + sg * (c->ax / la) * c->r;
        double a0 = atan2(Ay - Cy, Ax - Cx);
        int M = (int)ceil(fabs(c->turn) / 0.005);
        if (M < 2)
            M = 2;

        for (int j = 0; j <= M; j++) {
            if (out_len + 1 >= cap) {
                cap *= 2;
                double *nox = realloc(ox, cap * sizeof(double));
                if (!nox) {
                    free(cum);
                    free(corners);
                    free(ox);
                    free(oy);
                    free(fast);
                    return 0;
                }
                ox = nox;
                double *noy = realloc(oy, cap * sizeof(double));
                if (!noy) {
                    free(cum);
                    free(corners);
                    free(ox);
                    free(oy);
                    free(fast);
                    return 0;
                }
                oy = noy;
                uint8_t *nfast = realloc(fast, cap * sizeof(uint8_t));
                if (!nfast) {
                    free(cum);
                    free(corners);
                    free(ox);
                    free(oy);
                    free(fast);
                    return 0;
                }
                fast = nfast;
            }
            double a = a0 + c->turn * (double)j / (double)M;
            ox[out_len] = Cx + c->r * cos(a);
            oy[out_len] = Cy + c->r * sin(a);
            fast[out_len] = (j < M ? 1 : 0);
            out_len++;
        }

        Corner *next = &corners[(q + 1) % num_corners];
        double end = fmod(cum[next->i] - cum[c->i], P);
        if (end < 0.0)
            end += P;
        if (end == 0.0)
            end = P;

        size_t k = ((size_t)c->i + 1) % n;
        double d = cum[c->i + 1] - cum[c->i];
        while (k != (size_t)next->i) {
            if (d > c->t + 1e-9 && d < end - next->t - 1e-9) {
                if (out_len + 1 >= cap) {
                    cap *= 2;
                    double *nox = realloc(ox, cap * sizeof(double));
                    if (!nox) {
                        free(cum);
                        free(corners);
                        free(ox);
                        free(oy);
                        free(fast);
                        return 0;
                    }
                    ox = nox;
                    double *noy = realloc(oy, cap * sizeof(double));
                    if (!noy) {
                        free(cum);
                        free(corners);
                        free(ox);
                        free(oy);
                        free(fast);
                        return 0;
                    }
                    oy = noy;
                    uint8_t *nfast = realloc(fast, cap * sizeof(uint8_t));
                    if (!nfast) {
                        free(cum);
                        free(corners);
                        free(ox);
                        free(oy);
                        free(fast);
                        return 0;
                    }
                    fast = nfast;
                }
                ox[out_len] = xs[k];
                oy[out_len] = ys[k];
                fast[out_len] = 0;
                out_len++;
            }
            d += cum[k + 1] - cum[k];
            k = (k + 1) % n;
        }
    }

    free(cum);
    free(corners);
    free(xs);
    free(ys);

    *p_xs = ox;
    *p_ys = oy;
    *p_n = out_len;
    *p_fast = fast;
    return 1;
}

static int curve_from(double *xs, double *ys, size_t n, uint8_t *fast, double fw, Curve *out) {
    memset(out, 0, sizeof(*out));
    out->n = n;
    out->xs = xs;
    out->ys = ys;

    out->cum = malloc((n + 1) * sizeof(double));
    out->tx = malloc(n * sizeof(double));
    out->ty = malloc(n * sizeof(double));
    out->turn = malloc((n + 1) * sizeof(double));
    out->wt = malloc(n * sizeof(double));

    if (!out->cum || !out->tx || !out->ty || !out->turn || !out->wt) {
        free_curve(out);
        return 0;
    }

    int has_fast = 0;
    for (size_t i = 0; i < n; i++) {
        out->wt[i] = (fast && fast[i] ? fw : 1.0);
        if (fast && fast[i])
            has_fast = 1;
    }
    out->fast = has_fast;

    out->cum[0] = 0.0;
    for (size_t i = 0; i < n; i++) {
        size_t j = (i + 1) % n;
        double dx = xs[j] - xs[i];
        double dy = ys[j] - ys[i];
        double len = hypot(dx, dy);
        out->cum[i + 1] = out->cum[i] + len;
        out->tx[i] = (len > 0.0 ? dx / len : 0.0);
        out->ty[i] = (len > 0.0 ? dy / len : 0.0);
    }
    out->P = out->cum[n];

    double T = 0.0;
    for (size_t v = 1; v <= n; v++) {
        double a = out->tx[v - 1];
        double b = out->ty[v - 1];
        double c = out->tx[v % n];
        double d = out->ty[v % n];
        out->turn[v] = atan2(a * d - b * c, a * c + b * d);
        T += fabs(out->turn[v]);
    }
    out->T = T;
    return 1;
}

static int make_curve(int shape, double R, double round_radius, double bend, double fw,
                      Curve *out) {
    Vec2 *raw = NULL;
    size_t raw_n = gen_shape_raw(shape, &raw);
    if (!raw_n || !raw)
        return 0;

    Vec2 *pts = malloc(raw_n * sizeof(Vec2));
    if (!pts) {
        free(raw);
        return 0;
    }

    size_t m = 0;
    for (size_t i = 0; i < raw_n; i++) {
        if (m == 0 || hypot(raw[i].x - pts[m - 1].x, raw[i].y - pts[m - 1].y) > 1e-9) {
            pts[m++] = raw[i];
        }
    }
    free(raw);

    while (m > 1 && hypot(pts[0].x - pts[m - 1].x, pts[0].y - pts[m - 1].y) <= 1e-9) {
        m--;
    }
    if (m < 3) {
        free(pts);
        return 0;
    }

    double A = 0.0;
    for (size_t i = 0; i < m; i++) {
        size_t j = (i + 1) % m;
        A += pts[i].x * pts[j].y - pts[j].x * pts[i].y;
    }
    A /= 2.0;
    if (A < 0.0) {
        for (size_t i = 0; i < m / 2; i++) {
            Vec2 tmp = pts[i];
            pts[i] = pts[m - 1 - i];
            pts[m - 1 - i] = tmp;
        }
        A = -A;
    }

    double cx = 0.0, cy = 0.0;
    for (size_t i = 0; i < m; i++) {
        size_t j = (i + 1) % m;
        double c = pts[i].x * pts[j].y - pts[j].x * pts[i].y;
        cx += (pts[i].x + pts[j].x) * c;
        cy += (pts[i].y + pts[j].y) * c;
    }
    cx /= (6.0 * A);
    cy /= (6.0 * A);

    double maxR = 0.0;
    for (size_t i = 0; i < m; i++) {
        double r = hypot(pts[i].x - cx, pts[i].y - cy);
        if (r > maxR)
            maxR = r;
    }
    double k = (maxR > 0.0 ? R / maxR : 1.0);

    double *xs = malloc(m * sizeof(double));
    double *ys = malloc(m * sizeof(double));
    if (!xs || !ys) {
        free(pts);
        free(xs);
        free(ys);
        return 0;
    }
    for (size_t i = 0; i < m; i++) {
        xs[i] = (pts[i].x - cx) * k;
        ys[i] = (pts[i].y - cy) * k;
    }
    free(pts);

    uint8_t *fast = NULL;
    if (round_radius > 0.0) {
        if (!fillet_curve(&xs, &ys, &m, &fast, round_radius, bend)) {
            free(xs);
            free(ys);
            return 0;
        }
    }

    int ok = curve_from(xs, ys, m, fast, fw, out);
    free(fast);
    return ok;
}

typedef struct {
    double x, y, tx, ty;
} LocateResult;

static LocateResult locate(const Curve *c, double s) {
    s = fmod(s, c->P);
    if (s < 0.0)
        s += c->P;
    size_t lo = 0, hi = c->n - 1;
    while (lo < hi) {
        size_t mid = (lo + hi + 1) >> 1;
        if (c->cum[mid] <= s)
            lo = mid;
        else
            hi = mid - 1;
    }
    double f = s - c->cum[lo];
    LocateResult res;
    res.x = c->xs[lo] + c->tx[lo] * f;
    res.y = c->ys[lo] + c->ty[lo] * f;
    res.tx = c->tx[lo];
    res.ty = c->ty[lo];
    return res;
}

typedef struct {
    double s;
    double turn;
} Event;

static Event ring_event(const Curve *c, int g) {
    int v = (g - 1) % (int)c->n + 1;
    double s = floor((double)(g - 1) / (double)c->n) * c->P + c->cum[v];
    return (Event){s, c->turn[v]};
}

static Event wheel_event(const Curve *c, int g, int inside) {
    if (inside) {
        int v = (g - 1) % (int)c->n + 1;
        double s = floor((double)(g - 1) / (double)c->n) * c->P + c->cum[v];
        return (Event){s, c->turn[v]};
    } else {
        int v = (int)c->n - g % (int)c->n;
        double s = floor((double)g / (double)c->n) * c->P + c->P - c->cum[v];
        return (Event){s, c->turn[v]};
    }
}

static inline double part_rot(int inside, int kind, double turn, double left) {
    return (kind == 1 || !inside) ? -left * turn : left * turn;
}

typedef struct {
    double s;
    double tau;
    int ri;
    int wi;
    int pend;
    double turn;
    double w;
} WalkState;

static double calc_ahead(const Curve *ring, const Curve *wheel, int inside, const WalkState *st,
                         double s_val) {
    double d = 0.0;
    Event re = ring_event(ring, st->ri);
    Event we = wheel_event(wheel, st->wi, inside);
    if (re.s < s_val)
        d += part_rot(inside, 1, re.turn, 1.0);
    if (we.s < s_val)
        d += part_rot(inside, 2, we.turn, 1.0);
    return d;
}

static void next_sample(const Curve *ring, const Curve *wheel, int inside, int clo_k, int clo_m,
                        double lambda, WalkState *st, double tau, double *out_s, double *out_d) {
    const double EPS = 1e-6;
    for (;;) {
        if (st->pend) {
            if (tau <= st->tau + st->w) {
                double left = 1.0 - (st->w > 0.0 ? (tau - st->tau) / st->w : 1.0);
                double s_val = st->s + EPS;
                double d_val = part_rot(inside, st->pend, st->turn, left) +
                               calc_ahead(ring, wheel, inside, st, s_val);
                *out_s = s_val;
                *out_d = d_val;
                return;
            }
            st->tau += st->w;
            st->pend = 0;
            continue;
        }
        Event NONE_EVENT = {1e30, 0.0};
        Event re = (st->ri <= clo_k * (int)ring->n) ? ring_event(ring, st->ri) : NONE_EVENT;
        Event we = (st->wi < clo_m * (int)wheel->n + (inside ? 1 : 0))
                       ? wheel_event(wheel, st->wi, inside)
                       : NONE_EVENT;
        int is_re = (re.s <= we.s);
        Event e = is_re ? re : we;
        int seg = (st->ri - 1) % (int)ring->n;
        double wt = ring->wt[seg];
        if (e.s >= 1e29 || tau < st->tau + (e.s - st->s) * wt) {
            double s_val = st->s + (tau - st->tau) / wt + EPS;
            double d_val = calc_ahead(ring, wheel, inside, st, s_val);
            *out_s = s_val;
            *out_d = d_val;
            return;
        }
        st->tau += (e.s - st->s) * wt;
        st->s = e.s;
        st->pend = is_re ? 1 : 2;
        st->turn = e.turn;
        double next_wt = ring->wt[(seg + 1) % (int)ring->n];
        double min_wt = (wt < next_wt ? wt : next_wt);
        st->w = lambda * fabs(e.turn) * (is_re ? min_wt : wt);
        if (is_re)
            st->ri++;
        else
            st->wi++;
    }
}

typedef struct {
    int on;
    double r2;
    int n;
    int ratio;
    int sgn;
    double c;
    double d;
    double a;
} W2;

typedef struct {
    double px, py;
} FrameResult;

static FrameResult compute_frame(const Curve *ring, const Curve *wheel, int inside, double penX,
                                 double penY, const W2 *w2, double s, double dpsi) {
    LocateResult Lr = locate(ring, s);
    double rx = Lr.x, ry = Lr.y;
    double nx = Lr.ty, ny = -Lr.tx;

    LocateResult Lw = locate(wheel, inside ? s : -s);
    double dx = Lw.x, dy = Lw.y;
    double wnx = Lw.ty, wny = -Lw.tx;

    double psi = atan2(ny, nx) - atan2(wny, wnx);
    if (!inside)
        psi += M_PI;
    psi += dpsi;

    double c = cos(psi), sn = sin(psi);
    double wx = rx - dx * c + dy * sn;
    double wy = ry - dx * sn - dy * c;

    double bx = penX, by = penY;
    if (w2 && w2->on) {
        double al = (double)w2->ratio * (inside ? s : -s) * (2.0 * M_PI) / wheel->P;
        double th = (double)w2->sgn * (double)w2->n * al + w2->a;
        double cx2 = w2->c * cos(al);
        double cy2 = w2->c * sin(al);
        bx = cx2 + w2->d * cos(th);
        by = cy2 + w2->d * sin(th);
    }
    FrameResult fr;
    fr.px = wx + bx * c - by * sn;
    fr.py = wy + bx * sn + by * c;
    return fr;
}

int sp_generate(const SpParams *p, size_t max_samples, SpPath *out) {
    if (out) {
        out->points = NULL;
        out->count = 0;
    }
    if (!p || !out)
        return 0;
    if (max_samples < 2)
        return 0;
    if (max_samples > SP_MAX_POINTS)
        max_samples = SP_MAX_POINTS;

    if (p->ring_shape < 0 || p->ring_shape > 10)
        return 0;
    if (p->wheel_shape < 0 || p->wheel_shape > 10)
        return 0;
    if (p->ring_teeth <= 0 || p->ring_teeth > 1000 || p->wheel_teeth <= 0 || p->wheel_teeth > 1000)
        return 0;
    if (p->outside != 0 && p->outside != 1)
        return 0;
    if (!isfinite(p->pen) || p->pen < 0.0 || p->pen > 1.5)
        return 0;
    if (!isfinite(p->rotation))
        return 0;
    if ((p->second_outside != 0 && p->second_outside != 1) || (p->second != 0 && p->second != 1))
        return 0;
    if (!isfinite(p->second_ratio) || !isfinite(p->second_speed))
        return 0;
    if (p->second) {
        if (p->second_ratio < 1.0 / 12.0 || p->second_ratio > 0.5 || p->second_speed < 1.0 ||
            p->second_speed > 12.0)
            return 0;
    }

    int inside = !p->outside;

    /* Analytic solution for classic circle gears */
    if (p->ring_shape == 0 && p->wheel_shape == 0) {
        double asked = (double)p->wheel_teeth / (double)p->ring_teeth;
        if (inside && asked > INSIDE_CAP)
            asked = INSIDE_CAP;
        Closure clo = calc_closure(1.0, asked);
        double snap = ((double)clo.k * 1.0 / (double)clo.m) / asked;
        double wheelR = asked * snap;
        double span = (double)clo.k * 2.0 * M_PI;

        int w2on = p->second;
        int div = 4;
        int ratio = 2;
        int w2n = 3;
        int sgn = -1;
        double c2 = 0.0, d2 = 0.0;
        if (w2on) {
            div = (int)round(1.0 / p->second_ratio);
            if (div < 2)
                div = 2;
            double r2 = wheelR / div;
            ratio = (int)round(p->second_speed);
            if (ratio < 1)
                ratio = 1;
            w2n = p->second_outside ? div + 1 : div - 1;
            sgn = p->second_outside ? 1 : -1;
            c2 = wheelR + sgn * r2;
            d2 = p->pen * r2;
        }

        size_t maxN = max_samples - 1;
        size_t min_target = (maxN < 3000 ? maxN : 3000);
        double est = round((double)clo.k * 1500.0);
        size_t N = (size_t)(est > 1.0 ? est : 1.0);
        if (N < min_target)
            N = min_target;
        if (N > maxN)
            N = maxN;

        if (w2on) {
            double est2 = round(60.0 * (1.0 + (double)w2n) * (double)ratio * (double)clo.m);
            size_t N2 = (size_t)(est2 > 1.0 ? est2 : 1.0);
            if (N < N2)
                N = N2;
            if (N > maxN)
                N = maxN;
        }
        if (N < 1)
            N = 1;

        size_t count = N + 1;
        SpPoint *pts = malloc(count * sizeof(SpPoint));
        if (!pts)
            return 0;

        double rot = p->rotation * (M_PI / 180.0);
        double cos_rot = cos(rot), sin_rot = sin(rot);
        double R = 1.0, r = wheelR, d = p->pen * r;

        for (size_t j = 0; j <= N; j++) {
            if (j == N) {
                pts[j] = pts[0];
                continue;
            }
            double s = ((double)j / (double)N) * span;
            double theta = s;
            double px, py;
            if (inside) {
                if (!w2on) {
                    px = (R - r) * cos(theta) + d * cos((R - r) / r * theta);
                    py = (R - r) * sin(theta) - d * sin((R - r) / r * theta);
                } else {
                    double wx = (R - r) * cos(theta);
                    double wy = (R - r) * sin(theta);
                    double psi = -((R - r) / r) * theta;
                    double al = (double)ratio * s / r;
                    double th = (double)sgn * (double)w2n * al;
                    double cx2 = c2 * cos(al);
                    double cy2 = c2 * sin(al);
                    double bx = cx2 + d2 * cos(th);
                    double by = cy2 + d2 * sin(th);
                    double c = cos(psi), sn = sin(psi);
                    px = wx + bx * c - by * sn;
                    py = wy + bx * sn + by * c;
                }
            } else {
                if (!w2on) {
                    px = (R + r) * cos(theta) - d * cos((R + r) / r * theta);
                    py = (R + r) * sin(theta) - d * sin((R + r) / r * theta);
                } else {
                    double wx = (R + r) * cos(theta);
                    double wy = (R + r) * sin(theta);
                    double psi = theta + (s / r) + M_PI;
                    double al = (double)ratio * (-s) / r;
                    double th = (double)sgn * (double)w2n * al;
                    double cx2 = c2 * cos(al);
                    double cy2 = c2 * sin(al);
                    double bx = cx2 + d2 * cos(th);
                    double by = cy2 + d2 * sin(th);
                    double c = cos(psi), sn = sin(psi);
                    px = wx + bx * c - by * sn;
                    py = wy + bx * sn + by * c;
                }
            }
            pts[j].x = px * cos_rot - py * sin_rot;
            pts[j].y = px * sin_rot + py * cos_rot;
        }

        out->points = pts;
        out->count = count;
        return 1;
    }

    /* General polyline rolling engine */
    Curve unit_curve;
    if (!make_curve(p->wheel_shape, 1.0, 0.0, 1.0, FAST_WEIGHT, &unit_curve)) {
        return 0;
    }
    double unitP = unit_curve.P;
    free_curve(&unit_curve);

    Curve outline;
    if (!make_curve(p->ring_shape, 1.0, 0.0, 1.0, FAST_WEIGHT, &outline)) {
        return 0;
    }

    double asked = (outline.P * (double)p->wheel_teeth / (double)p->ring_teeth) / unitP;
    if (inside && asked > INSIDE_CAP)
        asked = INSIDE_CAP;

    int smooth =
        (p->wheel_shape == 0 || p->wheel_shape == 1 || p->wheel_shape == 2 || p->wheel_shape == 4);
    double bend = (inside ? 1.0 : -1.0);

    int w2on = p->second && (p->wheel_shape == 0);
    int div = (w2on ? (int)round(1.0 / p->second_ratio) : 0);
    if (div < 2 && w2on)
        div = 2;
    int ratio = (w2on ? (int)round(p->second_speed) : 0);
    if (ratio < 1 && w2on)
        ratio = 1;
    int w2n = (w2on ? (p->second_outside ? div + 1 : div - 1) : 0);
    double fw = FAST_WEIGHT * (w2on ? 1.0 + (double)ratio * (1.0 + (double)w2n) : 1.0);
    if (fw > 1.0)
        fw = 1.0;

    Curve ring;
    if (smooth) {
        if (!make_curve(p->ring_shape, 1.0, asked, bend, fw, &ring)) {
            free_curve(&outline);
            return 0;
        }
        asked = (ring.P * (double)p->wheel_teeth / (double)p->ring_teeth) / unitP;
        if (inside && asked > INSIDE_CAP)
            asked = INSIDE_CAP;
        free_curve(&ring);
        if (!make_curve(p->ring_shape, 1.0, asked, bend, fw, &ring)) {
            free_curve(&outline);
            return 0;
        }
        free_curve(&outline);
    } else {
        ring = outline;
    }

    Closure clo = calc_closure(ring.P, unitP * asked);
    double snap = ((double)clo.k * ring.P / (double)clo.m) / unitP / asked;
    double wheelR = asked * snap;

    Curve wheel;
    if (!make_curve(p->wheel_shape, wheelR, 0.0, 1.0, FAST_WEIGHT, &wheel)) {
        free_curve(&ring);
        return 0;
    }

    double penX = p->pen * wheelR;
    double penY = 0.0;

    W2 w2;
    memset(&w2, 0, sizeof(w2));
    if (w2on) {
        double r2 = wheelR / div;
        w2.on = 1;
        w2.r2 = r2;
        w2.n = w2n;
        w2.ratio = ratio;
        w2.sgn = p->second_outside ? 1 : -1;
        w2.c = wheelR + w2.sgn * r2;
        w2.d = p->pen * r2;
        w2.a = 0.0;
    }

    double span = (double)clo.k * ring.P;
    double lambda = wheelR + (w2.on ? w2.c + w2.d : hypot(penX, penY));
    double tau = span + lambda * ((double)clo.k * ring.T + (double)clo.m * wheel.T);

    size_t maxN = max_samples - 1;
    size_t min_target = (maxN < 3000 ? maxN : 3000);
    double est = round((double)clo.k * 1500.0 * tau / span);
    size_t N = (size_t)(est > 1.0 ? est : 1.0);
    if (N < min_target)
        N = min_target;
    if (N > maxN)
        N = maxN;

    if (w2.on) {
        double est2 = round(60.0 * (1.0 + (double)w2.n) * (double)w2.ratio * (double)clo.m);
        size_t N2 = (size_t)(est2 > 1.0 ? est2 : 1.0);
        if (N < N2)
            N = N2;
        if (N > maxN)
            N = maxN;
    }
    if (N < 1)
        N = 1;

    WalkState st = {
        .s = 0.0, .tau = 0.0, .ri = 1, .wi = (inside ? 1 : 0), .pend = 0, .turn = 0.0, .w = 0.0};

    if (ring.fast) {
        double dummy_s = 0.0, dummy_d = 0.0;
        next_sample(&ring, &wheel, inside, clo.k, clo.m, lambda, &st, 1e30, &dummy_s, &dummy_d);
        tau = st.tau;
        st = (WalkState){.s = 0.0,
                         .tau = 0.0,
                         .ri = 1,
                         .wi = (inside ? 1 : 0),
                         .pend = 0,
                         .turn = 0.0,
                         .w = 0.0};
    }

    size_t count = N + 1;
    SpPoint *pts = malloc(count * sizeof(SpPoint));
    if (!pts) {
        free_curve(&ring);
        free_curve(&wheel);
        return 0;
    }

    double rot = p->rotation * (M_PI / 180.0);
    double cos_rot = cos(rot), sin_rot = sin(rot);

    for (size_t j = 0; j <= N; j++) {
        if (j == N) {
            pts[j] = pts[0];
            continue;
        }
        double cur_tau = ((double)j / (double)N) * tau;
        double s_val = 0.0, d_val = 0.0;
        next_sample(&ring, &wheel, inside, clo.k, clo.m, lambda, &st, cur_tau, &s_val, &d_val);
        FrameResult fr = compute_frame(&ring, &wheel, inside, penX, penY, &w2, s_val, d_val);
        pts[j].x = fr.px * cos_rot - fr.py * sin_rot;
        pts[j].y = fr.px * sin_rot + fr.py * cos_rot;
    }

    free_curve(&ring);
    free_curve(&wheel);

    out->points = pts;
    out->count = count;
    return 1;
}
