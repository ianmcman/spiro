#include "spiro.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.141592653589793238462643383279502884
#endif

#ifndef M_TAU
#define M_TAU 6.283185307179586476925286766559005768
#endif

/* -------------------------------------------------------------------------
 * Zhang-Suen Thinning and Mask Tracing
 * ------------------------------------------------------------------------- */

typedef struct {
    int x;
    int y;
    int idx;
} SpPixel;

/*
 * sp_trace_mask:
 *   - Bounded mask <= SP_MAX_DETAIL squared with nonzero ink.
 *   - Thinned via Zhang-Suen algorithm with zero-padding border.
 *   - Ensures small blobs (e.g. 2x2 squares) are preserved if thinned away.
 *   - Walks components into a continuous path using nearest remaining endpoint/point.
 *   - Normalizes coordinates preserving aspect ratio into [-0.85, 0.85].
 *   - Removes duplicate neighbors.
 *   - Resets out and returns 0 on empty mask or invalid input.
 */
int sp_trace_mask(const unsigned char *mask, int width, int height, SpPath *out) {
    if (!out)
        return 0;
    *out = (SpPath){0};

    if (!mask || width <= 0 || height <= 0 || width > SP_MAX_DETAIL || height > SP_MAX_DETAIL) {
        return 0;
    }

    size_t w = (size_t)width;
    size_t h = (size_t)height;
    size_t total_pixels = w * h;

    size_t ink_count = 0;
    for (size_t i = 0; i < total_pixels; i++) {
        if (mask[i])
            ink_count++;
    }
    if (ink_count == 0) {
        return 0;
    }

    /* Padded working buffer (width+2) x (height+2) for boundary-safe thinning */
    size_t pw = w + 2;
    size_t ph = h + 2;
    size_t psize = pw * ph;

    unsigned char *b = calloc(psize, sizeof(unsigned char));
    size_t *del = malloc(psize * sizeof(size_t));
    if (!b || !del) {
        free(b);
        free(del);
        return 0;
    }

    for (size_t y = 0; y < h; y++) {
        for (size_t x = 0; x < w; x++) {
            if (mask[y * w + x]) {
                b[(y + 1) * pw + (x + 1)] = 1;
            }
        }
    }

    /* Zhang-Suen thinning loop */
    bool changed = true;
    for (int iter = 0; iter < 1000 && changed; iter++) {
        changed = false;
        for (int pass = 0; pass < 2; pass++) {
            size_t del_count = 0;
            for (size_t y = 1; y < ph - 1; y++) {
                for (size_t x = 1; x < pw - 1; x++) {
                    size_t i = y * pw + x;
                    if (!b[i])
                        continue;

                    unsigned char p2 = b[i - pw];
                    unsigned char p3 = b[i - pw + 1];
                    unsigned char p4 = b[i + 1];
                    unsigned char p5 = b[i + pw + 1];
                    unsigned char p6 = b[i + pw];
                    unsigned char p7 = b[i + pw - 1];
                    unsigned char p8 = b[i - 1];
                    unsigned char p9 = b[i - pw - 1];

                    int n = p2 + p3 + p4 + p5 + p6 + p7 + p8 + p9;
                    if (n < 2 || n > 6)
                        continue;

                    int t = (!p2 && p3) + (!p3 && p4) + (!p4 && p5) + (!p5 && p6) + (!p6 && p7) +
                            (!p7 && p8) + (!p8 && p9) + (!p9 && p2);
                    if (t != 1)
                        continue;

                    if (pass == 0) {
                        if ((p2 * p4 * p6 == 0) && (p4 * p6 * p8 == 0)) {
                            del[del_count++] = i;
                        }
                    } else {
                        if ((p2 * p4 * p8 == 0) && (p2 * p6 * p8 == 0)) {
                            del[del_count++] = i;
                        }
                    }
                }
            }
            if (del_count > 0) {
                changed = true;
                for (size_t d = 0; d < del_count; d++) {
                    b[del[d]] = 0;
                }
            }
        }
    }
    free(del);

    /* Copy thinned pixels back to width x height */
    unsigned char *thinned = calloc(total_pixels, sizeof(unsigned char));
    if (!thinned) {
        free(b);
        return 0;
    }
    for (size_t y = 0; y < h; y++) {
        for (size_t x = 0; x < w; x++) {
            if (b[(y + 1) * pw + (x + 1)]) {
                thinned[y * w + x] = 1;
            }
        }
    }
    free(b);

    /* Restore center pixels for any blobs thinned away (e.g. 2x2 squares) */
    unsigned char *orig_lab = calloc(total_pixels, sizeof(unsigned char));
    int *queue = malloc(total_pixels * sizeof(int));
    if (!orig_lab || !queue) {
        free(thinned);
        free(orig_lab);
        free(queue);
        return 0;
    }

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = y * width + x;
            if (!mask[idx] || orig_lab[idx])
                continue;

            int head = 0, tail = 0;
            queue[tail++] = idx;
            orig_lab[idx] = 1;
            bool has_thinned = false;
            double sum_x = 0.0, sum_y = 0.0;
            int blob_start = head;

            while (head < tail) {
                int cur = queue[head++];
                if (thinned[cur])
                    has_thinned = true;
                int cx = cur % width;
                int cy = cur / width;
                sum_x += cx;
                sum_y += cy;

                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        if (dx == 0 && dy == 0)
                            continue;
                        int nx = cx + dx;
                        int ny = cy + dy;
                        if (nx >= 0 && nx < width && ny >= 0 && ny < height) {
                            int nidx = ny * width + nx;
                            if (mask[nidx] && !orig_lab[nidx]) {
                                orig_lab[nidx] = 1;
                                queue[tail++] = nidx;
                            }
                        }
                    }
                }
            }

            if (!has_thinned && tail > blob_start) {
                int blob_size = tail - blob_start;
                double mx = sum_x / (double)blob_size;
                double my = sum_y / (double)blob_size;
                int best_idx = queue[blob_start];
                double best_d = -1.0;

                for (int q = blob_start; q < tail; q++) {
                    int pidx = queue[q];
                    double px = (double)(pidx % width);
                    double py = (double)(pidx / width);
                    double d = (px - mx) * (px - mx) + (py - my) * (py - my);
                    if (best_d < 0.0 || d < best_d) {
                        best_d = d;
                        best_idx = pidx;
                    }
                }
                thinned[best_idx] = 1;
            }
        }
    }
    free(orig_lab);
    free(queue);

    /* Collect all remaining thinned points */
    SpPixel *points = malloc(total_pixels * sizeof(SpPixel));
    unsigned char *deg = calloc(total_pixels, sizeof(unsigned char));
    if (!points || !deg) {
        free(thinned);
        free(points);
        free(deg);
        return 0;
    }

    size_t num_points = 0;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = y * width + x;
            if (thinned[idx]) {
                points[num_points++] = (SpPixel){x, y, idx};
            }
        }
    }
    if (num_points == 0) {
        free(thinned);
        free(points);
        free(deg);
        return 0;
    }

    /* Compute 8-neighbor degrees in thinned mask */
    for (size_t i = 0; i < num_points; i++) {
        int d = 0;
        int px = points[i].x;
        int py = points[i].y;
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                if (dx == 0 && dy == 0)
                    continue;
                int nx = px + dx;
                int ny = py + dy;
                if (nx >= 0 && nx < width && ny >= 0 && ny < height) {
                    if (thinned[ny * width + nx])
                        d++;
                }
            }
        }
        deg[points[i].idx] = (unsigned char)d;
    }

    /* Walk components into continuous path */
    unsigned char *walk_visited = calloc(total_pixels, sizeof(unsigned char));
    SpPoint *raw_path = malloc(num_points * sizeof(SpPoint));
    if (!walk_visited || !raw_path) {
        free(thinned);
        free(points);
        free(deg);
        free(walk_visited);
        free(raw_path);
        return 0;
    }

    size_t path_len = 0;
    size_t remaining = num_points;

    /* Start at an endpoint (deg <= 1) if available, else first point */
    SpPixel cur = points[0];
    for (size_t i = 0; i < num_points; i++) {
        if (deg[points[i].idx] <= 1) {
            cur = points[i];
            break;
        }
    }

    while (remaining > 0) {
        if (!walk_visited[cur.idx]) {
            walk_visited[cur.idx] = 1;
            raw_path[path_len++] = (SpPoint){(double)cur.x, (double)cur.y};
            remaining--;
        }
        if (remaining == 0)
            break;

        /* Look for unvisited 8-neighbor with minimum degree */
        SpPixel next = (SpPixel){-1, -1, -1};
        int min_d = 99;
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                if (dx == 0 && dy == 0)
                    continue;
                int nx = cur.x + dx;
                int ny = cur.y + dy;
                if (nx >= 0 && nx < width && ny >= 0 && ny < height) {
                    int nidx = ny * width + nx;
                    if (thinned[nidx] && !walk_visited[nidx]) {
                        int d = deg[nidx];
                        if (d < min_d) {
                            min_d = d;
                            next = (SpPixel){nx, ny, nidx};
                        }
                    }
                }
            }
        }

        if (next.idx != -1) {
            cur = next;
        } else {
            /* Find nearest remaining endpoint (deg <= 1) */
            int best_dist = -1;
            SpPixel best_pt = (SpPixel){-1, -1, -1};
            for (size_t i = 0; i < num_points; i++) {
                if (!walk_visited[points[i].idx] && deg[points[i].idx] <= 1) {
                    int ddx = points[i].x - cur.x;
                    int ddy = points[i].y - cur.y;
                    int dist = ddx * ddx + ddy * ddy;
                    if (best_dist < 0 || dist < best_dist) {
                        best_dist = dist;
                        best_pt = points[i];
                    }
                }
            }
            /* If no remaining endpoint, find nearest remaining point */
            if (best_pt.idx == -1) {
                for (size_t i = 0; i < num_points; i++) {
                    if (!walk_visited[points[i].idx]) {
                        int ddx = points[i].x - cur.x;
                        int ddy = points[i].y - cur.y;
                        int dist = ddx * ddx + ddy * ddy;
                        if (best_dist < 0 || dist < best_dist) {
                            best_dist = dist;
                            best_pt = points[i];
                        }
                    }
                }
            }
            if (best_pt.idx == -1)
                break;
            cur = best_pt;
        }
    }

    free(thinned);
    free(points);
    free(deg);
    free(walk_visited);

    if (path_len == 0) {
        free(raw_path);
        return 0;
    }

    /* Remove duplicate neighbors */
    SpPoint *dedup = malloc(path_len * sizeof(SpPoint));
    if (!dedup) {
        free(raw_path);
        return 0;
    }

    size_t dedup_len = 0;
    for (size_t i = 0; i < path_len; i++) {
        if (dedup_len == 0 || raw_path[i].x != dedup[dedup_len - 1].x ||
            raw_path[i].y != dedup[dedup_len - 1].y) {
            dedup[dedup_len++] = raw_path[i];
        }
    }
    free(raw_path);

    if (dedup_len == 0) {
        free(dedup);
        return 0;
    }

    /* Normalize preserving aspect ratio to [-0.85, 0.85] */
    double min_x = dedup[0].x, max_x = dedup[0].x;
    double min_y = dedup[0].y, max_y = dedup[0].y;
    for (size_t i = 1; i < dedup_len; i++) {
        if (dedup[i].x < min_x)
            min_x = dedup[i].x;
        if (dedup[i].x > max_x)
            max_x = dedup[i].x;
        if (dedup[i].y < min_y)
            min_y = dedup[i].y;
        if (dedup[i].y > max_y)
            max_y = dedup[i].y;
    }

    double span_x = max_x - min_x;
    double span_y = max_y - min_y;
    double span = span_x > span_y ? span_x : span_y;
    double cx = (min_x + max_x) * 0.5;
    double cy = (min_y + max_y) * 0.5;
    double scale = (span > 0.0) ? (1.7 / span) : 0.0;

    for (size_t i = 0; i < dedup_len; i++) {
        if (span > 0.0) {
            dedup[i].x = (dedup[i].x - cx) * scale;
            dedup[i].y = (dedup[i].y - cy) * scale;
        } else {
            dedup[i].x = 0.0;
            dedup[i].y = 0.0;
        }
    }

    out->points = dedup;
    out->count = dedup_len;
    return 1;
}

/* -------------------------------------------------------------------------
 * Fourier Transform and Resampling
 * ------------------------------------------------------------------------- */

/* In-place Cooley-Tukey radix-2 FFT (sign = -1 for forward DFT) */
static void fft_radix2(double *re, double *im, size_t n, int sign) {
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            double tr = re[i];
            re[i] = re[j];
            re[j] = tr;
            double ti = im[i];
            im[i] = im[j];
            im[j] = ti;
        }
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        size_t half = len >> 1;
        double ang = (double)sign * M_TAU / (double)len;
        double wr = cos(ang), wi = sin(ang);
        for (size_t i = 0; i < n; i += len) {
            double cr = 1.0, ci = 0.0;
            for (size_t k = 0; k < half; k++) {
                size_t a = i + k;
                size_t b = a + half;
                double tr = re[b] * cr - im[b] * ci;
                double ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
                double t = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = t;
            }
        }
    }
}

static int term_cmp_desc(const void *a, const void *b) {
    const SpTerm *ta = (const SpTerm *)a;
    const SpTerm *tb = (const SpTerm *)b;
    double ampa = hypot(ta->re, ta->im);
    double ampb = hypot(tb->re, tb->im);
    if (ampa > ampb)
        return -1;
    if (ampa < ampb)
        return 1;
    if (ta->frequency < tb->frequency)
        return -1;
    if (ta->frequency > tb->frequency)
        return 1;
    return 0;
}

/*
 * sp_fourier:
 *   - Uniformly resamples CLOSED polyline via cumulative length into power-of-two sample count
 * (16..SP_MAX_TERMS).
 *   - Computes radix-2 FFT.
 *   - Normalizes coefficients by sample_count.
 *   - Assigns signed frequencies: 0, 1..N/2-1, -N/2, -N/2+1..-1.
 *   - Sorts terms by descending amplitude.
 *   - Validates finite coordinates, bounds allocations, rejects degenerates.
 */
int sp_fourier(const SpPath *path, size_t sample_count, SpFourier *out) {
    if (!out)
        return 0;
    *out = (SpFourier){0};

    if (!path || !path->points || path->count < 2) {
        return 0;
    }
    if (sample_count < 16 || sample_count > SP_MAX_TERMS || (sample_count & (sample_count - 1)) != 0) {
        return 0;
    }

    size_t L = path->count;
    for (size_t i = 0; i < L; i++) {
        if (!isfinite(path->points[i].x) || !isfinite(path->points[i].y)) {
            return 0;
        }
    }

    /* Cumulative perimeter of closed polyline */
    double *seg = malloc(L * sizeof(double));
    double *cum = malloc((L + 1) * sizeof(double));
    if (!seg || !cum) {
        free(seg);
        free(cum);
        return 0;
    }

    cum[0] = 0.0;
    for (size_t k = 0; k < L; k++) {
        size_t k1 = (k + 1) % L;
        double dx = path->points[k1].x - path->points[k].x;
        double dy = path->points[k1].y - path->points[k].y;
        double d = hypot(dx, dy);
        seg[k] = d;
        cum[k + 1] = cum[k] + d;
    }

    double total_len = cum[L];
    if (total_len <= 1e-12 || !isfinite(total_len)) {
        free(seg);
        free(cum);
        return 0; /* Reject degenerate path */
    }

    /* Resample into sample_count points */
    double *rx = malloc(sample_count * sizeof(double));
    double *ry = malloc(sample_count * sizeof(double));
    if (!rx || !ry) {
        free(seg);
        free(cum);
        free(rx);
        free(ry);
        return 0;
    }

    size_t k = 0;
    for (size_t j = 0; j < sample_count; j++) {
        double t = ((double)j / (double)sample_count) * total_len;
        while (k < L - 1 && cum[k + 1] < t) {
            k++;
        }
        double seg_len = seg[k];
        double f = (seg_len > 1e-12) ? ((t - cum[k]) / seg_len) : 0.0;
        size_t k1 = (k + 1) % L;
        rx[j] = path->points[k].x + (path->points[k1].x - path->points[k].x) * f;
        ry[j] = path->points[k].y + (path->points[k1].y - path->points[k].y) * f;
    }
    free(seg);
    free(cum);

    /* Radix-2 forward FFT */
    fft_radix2(rx, ry, sample_count, -1);

    SpTerm *terms = malloc(sample_count * sizeof(SpTerm));
    if (!terms) {
        free(rx);
        free(ry);
        return 0;
    }

    double inv_n = 1.0 / (double)sample_count;
    for (size_t j = 0; j < sample_count; j++) {
        int f;
        if (j == 0) {
            f = 0;
        } else if (j < sample_count / 2) {
            f = (int)j;
        } else if (j == sample_count / 2) {
            f = -(int)(sample_count / 2);
        } else {
            f = (int)j - (int)sample_count;
        }
        terms[j].re = rx[j] * inv_n;
        terms[j].im = ry[j] * inv_n;
        terms[j].frequency = f;
    }
    free(rx);
    free(ry);

    qsort(terms, sample_count, sizeof(SpTerm), term_cmp_desc);

    out->terms = terms;
    out->count = sample_count;
    return 1;
}

/*
 * sp_synthesize:
 *   - Sums selected terms for normalized t including exactly closed final sample.
 *   - Clamps requested terms to available terms count.
 *   - Validates terms and samples in range [2, SP_MAX_POINTS].
 */
int sp_synthesize(const SpFourier *f, size_t terms, size_t samples, SpPath *out) {
    if (!out)
        return 0;
    *out = (SpPath){0};

    if (!f || !f->terms || f->count == 0 || terms == 0 || samples < 2 || samples > SP_MAX_POINTS) {
        return 0;
    }

    size_t num_terms = terms;
    if (num_terms > f->count) {
        num_terms = f->count;
    }

    for (size_t i = 0; i < num_terms; i++) {
        if (!isfinite(f->terms[i].re) || !isfinite(f->terms[i].im)) {
            return 0;
        }
    }

    SpPoint *pts = malloc(samples * sizeof(SpPoint));
    if (!pts) {
        return 0;
    }

    /* On a periodic power-of-two grid, an inverse FFT evaluates the same
       Fourier sum in O(N log N), including arbitrary signed frequencies. */
    size_t period = samples - 1;
    if (period >= 16 && (period & (period - 1)) == 0) {
        double *re = calloc(period, sizeof(double));
        double *im = calloc(period, sizeof(double));
        if (!re || !im) {
            free(re);
            free(im);
            free(pts);
            return 0;
        }
        for (size_t k = 0; k < num_terms; k++) {
            int64_t slot = (int64_t)f->terms[k].frequency % (int64_t)period;
            if (slot < 0)
                slot += (int64_t)period;
            re[slot] += f->terms[k].re;
            im[slot] += f->terms[k].im;
        }
        fft_radix2(re, im, period, 1);
        for (size_t j = 0; j < period; j++) {
            if (!isfinite(re[j]) || !isfinite(im[j])) {
                free(re);
                free(im);
                free(pts);
                return 0;
            }
            pts[j] = (SpPoint){re[j], im[j]};
        }
        pts[period] = pts[0];
        free(re);
        free(im);
        out->points = pts;
        out->count = samples;
        return 1;
    }

    double denom = (double)(samples - 1);
    for (size_t s = 0; s < samples; s++) {
        if (s == samples - 1) {
            pts[s] = pts[0];
            continue;
        }
        double t = (double)s / denom;
        double sum_x = 0.0;
        double sum_y = 0.0;
        for (size_t k = 0; k < num_terms; k++) {
            double theta = M_TAU * (double)f->terms[k].frequency * t;
            double c = cos(theta);
            double s_val = sin(theta);
            sum_x += f->terms[k].re * c - f->terms[k].im * s_val;
            sum_y += f->terms[k].re * s_val + f->terms[k].im * c;
        }
        pts[s].x = sum_x;
        pts[s].y = sum_y;
    }

    out->points = pts;
    out->count = samples;
    return 1;
}

/*
 * sp_fourier_free:
 *   - Resets fields null safe.
 */
void sp_fourier_free(SpFourier *f) {
    if (f) {
        free(f->terms);
        f->terms = NULL;
        f->count = 0;
    }
}
