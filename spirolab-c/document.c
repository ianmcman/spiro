#include "document.h"
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>
#define FILE_LIMIT (128u * 1024u * 1024u)
#define TOTAL_POINTS 2000000u
#define DOC_ERROR (g_quark_from_static_string("spirolab-document"))
void sp_document_init(SpDocument *d) {
    memset(d, 0, sizeof *d);
    d->params = sp_defaults();
    d->drawn_params = d->params;
    d->ink = render_default_ink();
    d->text = g_strdup("SpiroLab");
    d->terms = 256;
    d->detail = 384;
    d->threshold = .15;
}
void sp_document_clear(SpDocument *d) {
    for (size_t i = 0; i < d->layer_count; i++)
        render_layer_free(&d->layers[i]);
    d->layer_count = 0;
    sp_fourier_free(&d->fourier);
    render_layer_free(&d->current);
    d->progress = 0;
}
void sp_document_free(SpDocument *d) {
    sp_document_clear(d);
    g_free(d->text);
    g_clear_object(&d->source);
    g_clear_object(&d->prepared);
    memset(d, 0, sizeof *d);
}
static void set_params(GKeyFile *k, const char *key, const SpParams *p) {
    double v[] = {p->ring_shape, p->wheel_shape, p->ring_teeth, p->wheel_teeth,  p->outside,
                  p->pen,        p->rotation,    p->second,     p->second_ratio, p->second_speed};
    g_key_file_set_double_list(k, "Settings", key, v, G_N_ELEMENTS(v));
}
static gboolean get_params(GKeyFile *k, const char *key, SpParams *p, GError **e) {
    gsize n = 0;
    double *v = g_key_file_get_double_list(k, "Settings", key, &n, e);
    if (!v)
        return FALSE;
    gboolean ok = n == 10;
    for (gsize i = 0; i < n; i++)
        if (!isfinite(v[i]))
            ok = FALSE;
    if (ok) {
        const int ints[] = {0, 1, 2, 3, 4, 7};
        for (size_t i = 0; i < G_N_ELEMENTS(ints); i++)
            if (v[ints[i]] != floor(v[ints[i]]))
                ok = FALSE;
        ok = ok && v[0] >= 0 && v[0] <= 10 && v[1] >= 0 && v[1] <= 10 && v[2] >= 1 &&
             v[2] <= 1000 && v[3] >= 1 && v[3] <= 1000 && v[4] >= 0 && v[4] <= 1 && v[5] >= 0 &&
             v[5] <= 1.5 && fabs(v[6]) <= 1e9 && v[7] >= 0 && v[7] <= 1 && v[8] >= 1. / 12 &&
             v[8] <= .5 && v[9] >= 1 && v[9] <= 12;
        if (ok)
            *p = (SpParams){(int)v[0], (int)v[1], (int)v[2], (int)v[3], (int)v[4],
                            v[5],      v[6],      (int)v[7], v[8],      v[9]};
    }
    g_free(v);
    return ok;
}
static void set_ink(GKeyFile *k, const char *g, const char *key, const SpInk *p) {
    double v[] = {p->r, p->g, p->b, p->a, p->line_width, p->color_cycle};
    g_key_file_set_double_list(k, g, key, v, G_N_ELEMENTS(v));
}
static gboolean get_ink(GKeyFile *k, const char *g, const char *key, SpInk *p, GError **e) {
    gsize n = 0;
    double *v = g_key_file_get_double_list(k, g, key, &n, e);
    if (!v)
        return FALSE;
    gboolean ok = n == 6;
    for (gsize i = 0; i < n; i++)
        if (!isfinite(v[i]))
            ok = FALSE;
    if (ok) {
        for (int i = 0; i < 4; i++)
            if (v[i] < 0 || v[i] > 1)
                ok = FALSE;
        ok = ok && v[4] >= .5 && v[4] <= 15 && (v[5] == 0 || v[5] == 1);
        if (ok)
            *p = (SpInk){v[0], v[1], v[2], v[3], v[4], (int)v[5]};
    }
    g_free(v);
    return ok;
}
static gboolean set_layer(GKeyFile *k, const char *g, const SpLayer *l) {
    g_key_file_set_integer(k, g, "count", (int)l->path.count);
    set_ink(k, g, "ink", &l->ink);
    g_key_file_set_boolean(k, g, "visible", l->visible);
    if (!l->path.count)
        return TRUE;
    double *v = g_try_new(double, l->path.count * 2);
    if (!v)
        return FALSE;
    for (size_t i = 0; i < l->path.count; i++) {
        v[2 * i] = l->path.points[i].x;
        v[2 * i + 1] = l->path.points[i].y;
    }
    g_key_file_set_double_list(k, g, "points", v, l->path.count * 2);
    if (l->pen_down) {
        char *data = g_base64_encode(l->pen_down, l->path.count);
        g_key_file_set_string(k, g, "pen_down", data);
        g_free(data);
    }
    g_free(v);
    return TRUE;
}
static gboolean get_layer(GKeyFile *k, const char *g, SpLayer *l, size_t *total, GError **e) {
    int n = g_key_file_get_integer(k, g, "count", e);
    if (*e || n < 0 || n > 250000 || *total + (size_t)n > TOTAL_POINTS)
        return FALSE;
    *total += (size_t)n;
    l->visible = g_key_file_get_boolean(k, g, "visible", e);
    if (*e || !get_ink(k, g, "ink", &l->ink, e))
        return FALSE;
    if (!n)
        return TRUE;
    gsize len = 0;
    double *v = g_key_file_get_double_list(k, g, "points", &len, e);
    if (!v)
        return FALSE;
    if (len != (gsize)n * 2) {
        g_free(v);
        return FALSE;
    }
    for (gsize i = 0; i < len; i++)
        if (!isfinite(v[i]) || fabs(v[i]) > 1e6) {
            g_free(v);
            return FALSE;
        }
    l->path.points = malloc((size_t)n * sizeof(SpPoint));
    if (!l->path.points) {
        g_free(v);
        return FALSE;
    }
    l->path.count = n;
    for (int i = 0; i < n; i++)
        l->path.points[i] = (SpPoint){v[2 * i], v[2 * i + 1]};
    if (g_key_file_has_key(k, g, "pen_down", NULL)) {
        char *data = g_key_file_get_string(k, g, "pen_down", e);
        if (!data) {
            g_free(v);
            return FALSE;
        }
        gsize bytes = 0;
        guchar *flags = g_base64_decode(data, &bytes);
        g_free(data);
        if (bytes != (gsize)n) {
            g_free(flags);
            g_free(v);
            return FALSE;
        }
        for (gsize i = 0; i < bytes; i++)
            if (flags[i] > 1) {
                g_free(flags);
                g_free(v);
                return FALSE;
            }
        l->pen_down = flags;
    }
    g_free(v);
    return TRUE;
}
static gboolean set_image(GKeyFile *k, const char *key, GdkPixbuf *p, GError **e) {
    if (!p)
        return TRUE;
    gchar *data = NULL;
    gsize len = 0;
    if (!gdk_pixbuf_save_to_buffer(p, &data, &len, "png", e, NULL))
        return FALSE;
    char *b64 = g_base64_encode((guchar *)data, len);
    g_key_file_set_string(k, "Images", key, b64);
    g_free(b64);
    g_free(data);
    return TRUE;
}
static void check_image_size(GdkPixbufLoader *l, int w, int h, gpointer p) {
    gboolean *bad = p;
    if (w < 1 || h < 1 || w > 2048 || h > 2048) {
        *bad = TRUE;
        gdk_pixbuf_loader_set_size(l, 1, 1);
    }
}
static gboolean get_image(GKeyFile *k, const char *key, GdkPixbuf **out, GError **e) {
    if (!g_key_file_has_key(k, "Images", key, NULL))
        return TRUE;
    char *b64 = g_key_file_get_string(k, "Images", key, e);
    if (!b64)
        return FALSE;
    gsize n = 0;
    guchar *data = g_base64_decode(b64, &n);
    g_free(b64);
    if (!n || n > 24u * 1024u * 1024u) {
        g_free(data);
        return FALSE;
    }
    GdkPixbufLoader *l = gdk_pixbuf_loader_new_with_type("png", e);
    if (!l) {
        g_free(data);
        return FALSE;
    }
    gboolean bad = FALSE;
    g_signal_connect(l, "size-prepared", G_CALLBACK(check_image_size), &bad);
    gboolean ok = gdk_pixbuf_loader_write(l, data, n, e);
    g_free(data);
    if (ok)
        ok = gdk_pixbuf_loader_close(l, e);
    else
        gdk_pixbuf_loader_close(l, NULL);
    GdkPixbuf *p = gdk_pixbuf_loader_get_pixbuf(l);
    if (ok && !bad && p)
        *out = g_object_ref(p);
    else
        ok = FALSE;
    g_object_unref(l);
    return ok;
}
static gboolean set_fourier(GKeyFile *k, const SpFourier *f) {
    g_key_file_set_integer(k, "Fourier", "count", (int)f->count);
    if (!f->count)
        return TRUE;
    double *values = g_try_new(double, f->count * 3);
    if (!values)
        return FALSE;
    for (size_t i = 0; i < f->count; i++) {
        values[3 * i] = f->terms[i].re;
        values[3 * i + 1] = f->terms[i].im;
        values[3 * i + 2] = f->terms[i].frequency;
    }
    g_key_file_set_double_list(k, "Fourier", "terms", values, f->count * 3);
    g_free(values);
    return TRUE;
}
static gboolean get_fourier(GKeyFile *k, SpFourier *f, GError **error) {
    if (!g_key_file_has_group(k, "Fourier"))
        return TRUE;
    int count = g_key_file_get_integer(k, "Fourier", "count", error);
    if (*error || count < 0 || count > 16384)
        return FALSE;
    if (!count)
        return TRUE;
    gsize n = 0;
    double *v = g_key_file_get_double_list(k, "Fourier", "terms", &n, error);
    if (!v)
        return FALSE;
    gboolean valid = n == (gsize)count * 3;
    for (gsize i = 0; i < n; i++)
        if (!isfinite(v[i]) || fabs(v[i]) > (i % 3 == 2 ? 16384 : 1000))
            valid = FALSE;
    if (valid)
        for (int i = 0; i < count; i++)
            if (floor(v[3 * i + 2]) != v[3 * i + 2])
                valid = FALSE;
    if (!valid) {
        g_free(v);
        return FALSE;
    }
    f->terms = malloc((size_t)count * sizeof(SpTerm));
    if (!f->terms) {
        g_free(v);
        return FALSE;
    }
    f->count = count;
    for (int i = 0; i < count; i++)
        f->terms[i] = (SpTerm){v[3 * i], v[3 * i + 1], (int)v[3 * i + 2]};
    g_free(v);
    return TRUE;
}
gboolean sp_document_save(const SpDocument *d, const char *file, GError **error) {
    GKeyFile *k = g_key_file_new();
    GError *e = NULL;
    gboolean ok = FALSE;
    size_t total = d->current.path.count;
    for (size_t i = 0; i < d->layer_count; i++)
        total += d->layers[i].path.count;
    if (total > TOTAL_POINTS)
        goto done;
    g_key_file_set_integer(k, "SpiroLab", "version", 1);
    g_key_file_set_integer(k, "Settings", "mode", d->mode);
    g_key_file_set_integer(k, "Settings", "drawn_mode", d->drawn_mode);
    set_params(k, "params", &d->params);
    set_params(k, "drawn_params", &d->drawn_params);
    set_ink(k, "Settings", "ink", &d->ink);
    g_key_file_set_string(k, "Settings", "text", d->text);
    g_key_file_set_integer(k, "Settings", "terms", d->terms);
    g_key_file_set_integer(k, "Settings", "detail", d->detail);
    g_key_file_set_integer(k, "Settings", "tool", d->tool);
    g_key_file_set_double(k, "Settings", "threshold", d->threshold);
    g_key_file_set_double(k, "Settings", "progress", d->progress);
    g_key_file_set_integer(k, "Layers", "count", d->layer_count);
    for (size_t i = 0; i < d->layer_count; i++) {
        char g[32];
        g_snprintf(g, sizeof g, "Layer%zu", i);
        if (!set_layer(k, g, &d->layers[i]))
            goto done;
    }
    SpLayer current = d->current;
    if (!current.path.count)
        current.ink = d->ink;
    if (!set_fourier(k, &d->fourier) || !set_layer(k, "Current", &current) ||
        !set_image(k, "source", d->source, &e) || !set_image(k, "prepared", d->prepared, &e))
        goto done;
    gsize n = 0;
    char *data = g_key_file_to_data(k, &n, &e);
    if (!data)
        goto done;
    if (n <= FILE_LIMIT)
        ok = g_file_set_contents_full(file, data, (gssize)n,
                                      G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE,
                                      0600, &e);
    g_free(data);
done:
    g_key_file_free(k);
    if (!ok) {
        if (!e)
            g_set_error(&e, DOC_ERROR, 1, "Design exceeds the size limit or cannot be saved");
        g_propagate_error(error, e);
    }
    return ok;
}
gboolean sp_document_load(SpDocument *d, const char *file, GError **error) {
    GError *e = NULL;
    GStatBuf st;
    GKeyFile *k = g_key_file_new();
    SpDocument t;
    sp_document_init(&t);
    gboolean ok = FALSE;
    size_t total = 0;
    if (g_stat(file, &st) || st.st_size < 1 || st.st_size > FILE_LIMIT)
        goto done;
    if (!g_key_file_load_from_file(k, file, G_KEY_FILE_NONE, &e))
        goto done;
#define READ_INT(field, key, lo, hi)                                                               \
    do {                                                                                           \
        t.field = g_key_file_get_integer(k, "Settings", key, &e);                                  \
        if (e || t.field < (lo) || t.field > (hi))                                                 \
            goto done;                                                                             \
    } while (0)
    if (g_key_file_get_integer(k, "SpiroLab", "version", &e) != 1 || e)
        goto done;
    READ_INT(mode, "mode", 0, 2);
    READ_INT(drawn_mode, "drawn_mode", 0, 2);
    READ_INT(terms, "terms", 2, 16384);
    READ_INT(detail, "detail", 32, 512);
    READ_INT(tool, "tool", 0, 4);
    if (!get_params(k, "params", &t.params, &e) ||
        !get_params(k, "drawn_params", &t.drawn_params, &e) ||
        !get_ink(k, "Settings", "ink", &t.ink, &e))
        goto done;
    char *text = g_key_file_get_string(k, "Settings", "text", &e);
    if (!text)
        goto done;
    g_free(t.text);
    t.text = text;
    if (!g_utf8_validate(text, -1, NULL) || g_utf8_strlen(text, -1) > 80)
        goto done;
    t.threshold = g_key_file_get_double(k, "Settings", "threshold", &e);
    if (e || !isfinite(t.threshold) || t.threshold < .01 || t.threshold > 1)
        goto done;
    t.progress = g_key_file_get_double(k, "Settings", "progress", &e);
    if (e || !isfinite(t.progress) || t.progress < 0 || t.progress > 1)
        goto done;
    int n = g_key_file_get_integer(k, "Layers", "count", &e);
    if (e || n < 0 || n > SP_MAX_LAYERS)
        goto done;
    t.layer_count = n;
    for (int i = 0; i < n; i++) {
        char g[32];
        g_snprintf(g, sizeof g, "Layer%d", i);
        if (!get_layer(k, g, &t.layers[i], &total, &e))
            goto done;
    }
    if (!get_fourier(k, &t.fourier, &e) || !get_layer(k, "Current", &t.current, &total, &e) ||
        !get_image(k, "source", &t.source, &e) || !get_image(k, "prepared", &t.prepared, &e))
        goto done;
    sp_document_free(d);
    *d = t;
    memset(&t, 0, sizeof t);
    ok = TRUE;
done:
    sp_document_free(&t);
    g_key_file_free(k);
    if (!ok) {
        if (!e)
            g_set_error(&e, DOC_ERROR, 1,
                        "Invalid or unsupported design; the current drawing was preserved");
        g_propagate_error(error, e);
    }
    return ok;
#undef READ_INT
}
