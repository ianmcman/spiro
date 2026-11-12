#include "gpu2d.h"
#include <epoxy/gl.h>
#include <math.h>
#include <stddef.h>
#include <string.h>
typedef struct {
    float ends[4], index;
} Segment;
typedef struct {
    GLuint vao, vbo;
    const SpPoint *points;
    const unsigned char *flags;
    size_t count, segments;
    unsigned *ends;
    gboolean dirty;
} Buffer;
typedef struct {
    Sp2GpuStats stats;
    Sp2GpuNotify notify;
    gpointer data;
    const SpLayer *layers, *current;
    size_t layer_count;
    SpViewTransform view;
    double progress;
    const SpParams *wheels;
    const SpFourier *fourier;
    Buffer buffers[SP_MAX_LAYERS + 1];
    GLuint program, texture_program, texture_vao, overlay_texture;
    GLuint layer_fbo, layer_texture;
    int width, height;
} Gpu;
static Gpu *state(GtkWidget *w) {
    return g_object_get_data(G_OBJECT(w), "spiro2d-gpu");
}
static const char *vs =
    "#version 330 core\nlayout(location=0) in vec4 ends;layout(location=1) in float index;"
    "uniform vec2 viewport,pan;uniform float zoom,width,total;"
    "out vec2 local;flat out float length_px;flat out float step_index;"
    "void main(){float s=min(viewport.x,viewport.y)/2.2*zoom;"
    "vec2 a=ends.xy*s+viewport*.5+pan,b=ends.zw*s+viewport*.5+pan;"
    "vec2 d=b-a;float len=length(d);vec2 u=len>1e-6?d/len:vec2(1,0);"
    "vec2 v=vec2(-u.y,u.x);float r=width*.5+.75;"
    "bool far_end=(gl_VertexID>=2);float x=far_end?len+r:-r;"
    "float y=(gl_VertexID%2==0)?-r:r;vec2 p=a+u*x+v*y;"
    "gl_Position=vec4(p.x/viewport.x*2-1,1-p.y/viewport.y*2,0,1);"
    "local=vec2(x,y);length_px=len;step_index=index;}";
static const char *fs =
    "#version 330 core\nin vec2 local;flat in float length_px;flat in float step_index;"
    "uniform vec4 ink;uniform float width,total;uniform int rainbow;out vec4 color;"
    "vec3 hsv(float h){vec3 p=abs(fract(vec3(h)+vec3(0,2./3.,1./3.))*6.-3.);"
    "return mix(vec3(1),clamp(p-1.,0.,1.),.95);}"
    "void main(){float x=clamp(local.x,0.,length_px);"
    "float dist=length(vec2(local.x-x,local.y));"
    "float alpha=ink.a*(1.-smoothstep(width*.5-.5,width*.5+.5,dist));"
    "vec3 rgb=ink.rgb;if(rainbow!=0){float "
    "f=(step_index-1.+x/max(length_px,1e-6))/max(total,1.);rgb=hsv(f*2.5);}"
    "color=vec4(rgb*alpha,alpha);}";
static const char *texture_vs = "#version 330 core\nuniform int flip;out vec2 uv;void main(){vec2 "
                                "p=vec2(gl_VertexID/2,gl_VertexID%2);"
                                "uv=vec2(p.x,flip!=0?1.-p.y:p.y);gl_Position=vec4(p*2.-1.,0,1);}";
static const char *texture_fs =
    "#version 330 core\nin vec2 uv;uniform sampler2D image;uniform float opacity;out vec4 color;"
    "void main(){color=texture(image,uv)*opacity;}";
static void fail(GtkGLArea *area, const char *s) {
    Gpu *g = state(GTK_WIDGET(area));
    g_strlcpy(g->stats.error, s, sizeof g->stats.error);
    g->stats.ready = FALSE;
    GError *e = g_error_new_literal(g_quark_from_static_string("spiro2d-gl"), 1, s);
    gtk_gl_area_set_error(area, e);
    g_error_free(e);
    if (g->notify)
        g->notify(GTK_WIDGET(area), g->data);
}
static GLuint shader(GtkGLArea *area, GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        glDeleteShader(s);
        fail(area, log);
        return 0;
    }
    return s;
}
static GLuint program(GtkGLArea *area, const char *v, const char *f) {
    GLuint a = shader(area, GL_VERTEX_SHADER, v);
    if (!a)
        return 0;
    GLuint b = shader(area, GL_FRAGMENT_SHADER, f);
    if (!b) {
        glDeleteShader(a);
        return 0;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, a);
    glAttachShader(p, b);
    glLinkProgram(p);
    glDeleteShader(a);
    glDeleteShader(b);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        glDeleteProgram(p);
        fail(area, log);
        return 0;
    }
    return p;
}
static void realize(GtkGLArea *area, gpointer data) {
    (void)data;
    Gpu *g = state(GTK_WIDGET(area));
    gtk_gl_area_make_current(area);
    GError *e = gtk_gl_area_get_error(area);
    if (e) {
        char s[512];
        g_strlcpy(s, e->message, sizeof s);
        fail(area, s);
        return;
    }
    if (!epoxy_is_desktop_gl() || epoxy_gl_version() < 33) {
        fail(area, "OpenGL 3.3 is required");
        return;
    }
    const char *r = (const char *)glGetString(GL_RENDERER);
    g_strlcpy(g->stats.renderer, r ? r : "Unknown", sizeof g->stats.renderer);
    char *lower = g_ascii_strdown(g->stats.renderer, -1);
    g->stats.hardware = !strstr(lower, "llvmpipe") && !strstr(lower, "softpipe") &&
                        !strstr(lower, "software") && !strstr(lower, "swrast");
    g_free(lower);
    g->program = program(area, vs, fs);
    if (!g->program)
        return;
    g->texture_program = program(area, texture_vs, texture_fs);
    if (!g->texture_program)
        return;
    glGenVertexArrays(1, &g->texture_vao);
    glGenTextures(1, &g->overlay_texture);
    glGenFramebuffers(1, &g->layer_fbo);
    glGenTextures(1, &g->layer_texture);
    for (size_t i = 0; i < G_N_ELEMENTS(g->buffers); i++)
        g->buffers[i].dirty = TRUE;
    g->stats.ready = TRUE;
    g->stats.error[0] = 0;
    if (g->notify)
        g->notify(GTK_WIDGET(area), g->data);
}
static void unrealize(GtkGLArea *area, gpointer data) {
    (void)data;
    Gpu *g = state(GTK_WIDGET(area));
    GdkGLContext *c = gtk_gl_area_get_context(area);
    if (c)
        gdk_gl_context_make_current(c);
    gboolean valid = c && gdk_gl_context_get_current() == c;
    for (size_t i = 0; i < G_N_ELEMENTS(g->buffers); i++) {
        Buffer *b = &g->buffers[i];
        if (valid) {
            if (b->vao)
                glDeleteVertexArrays(1, &b->vao);
            if (b->vbo)
                glDeleteBuffers(1, &b->vbo);
        }
        g_free(b->ends);
        *b = (Buffer){.dirty = TRUE};
    }
    if (valid) {
        if (g->program)
            glDeleteProgram(g->program);
        if (g->texture_program)
            glDeleteProgram(g->texture_program);
        if (g->texture_vao)
            glDeleteVertexArrays(1, &g->texture_vao);
        if (g->overlay_texture)
            glDeleteTextures(1, &g->overlay_texture);
        if (g->layer_texture)
            glDeleteTextures(1, &g->layer_texture);
        if (g->layer_fbo)
            glDeleteFramebuffers(1, &g->layer_fbo);
    }
    g->program = g->texture_program = g->texture_vao = g->overlay_texture = g->layer_texture =
        g->layer_fbo = 0;
    g->width = g->height = 0;
    g->stats.ready = FALSE;
}
static gboolean upload(GtkGLArea *area, Buffer *b, const SpLayer *l) {
    Gpu *g = state(GTK_WIDGET(area));
    if (!b->dirty && b->points == l->path.points && b->count == l->path.count &&
        b->flags == l->pen_down)
        return TRUE;
    if (l->path.count > SP_MAX_POINTS) {
        fail(area, "2D curve exceeds the supported point limit");
        return FALSE;
    }
    size_t segments = 0;
    for (size_t i = 1; i < l->path.count; i++)
        if (!l->pen_down || l->pen_down[i])
            segments++;
    Segment *v = g_try_new(Segment, segments);
    unsigned *ends = g_try_new(unsigned, segments);
    if (segments && (!v || !ends)) {
        g_free(v);
        g_free(ends);
        fail(area, "Cannot allocate 2D GPU geometry");
        return FALSE;
    }
    size_t at = 0;
    for (size_t i = 1; i < l->path.count; i++)
        if (!l->pen_down || l->pen_down[i]) {
            SpPoint a = l->path.points[i - 1], z = l->path.points[i];
            v[at] = (Segment){{a.x, a.y, z.x, z.y}, i};
            ends[at++] = i;
        }
    if (!b->vao) {
        glGenVertexArrays(1, &b->vao);
        glGenBuffers(1, &b->vbo);
    }
    glBindVertexArray(b->vao);
    glBindBuffer(GL_ARRAY_BUFFER, b->vbo);
    glBufferData(GL_ARRAY_BUFFER, segments * sizeof *v, v, GL_STATIC_DRAW);
    g_free(v);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(Segment), (void *)0);
    glVertexAttribDivisor(0, 1);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, sizeof(Segment),
                          (void *)offsetof(Segment, index));
    glVertexAttribDivisor(1, 1);
    g_free(b->ends);
    b->ends = ends;
    b->segments = segments;
    b->points = l->path.points;
    b->count = l->path.count;
    b->flags = l->pen_down;
    b->dirty = FALSE;
    g->stats.uploads++;
    return TRUE;
}
static void texture_settings(void) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}
static void composite(Gpu *g, GLuint texture, gboolean flip, float opacity) {
    glUseProgram(g->texture_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform1i(glGetUniformLocation(g->texture_program, "image"), 0);
    glUniform1f(glGetUniformLocation(g->texture_program, "opacity"), opacity);
    /* FBO textures have bottom-origin rows, unlike uploaded Cairo surfaces. */
    glBindVertexArray(g->texture_vao);
    glUniform1i(glGetUniformLocation(g->texture_program, "flip"), flip);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}
static gboolean draw_layer(GtkGLArea *area, Gpu *g, Buffer *b, const SpLayer *l, double progress,
                           GLuint target, int W, int H) {
    if (!l || !l->visible || !l->path.points || l->path.count < 2)
        return TRUE;
    if (!upload(area, b, l))
        return FALSE;
    size_t drawn = (size_t)(CLAMP(progress, 0., 1.) * l->path.count), lo = 0, hi = b->segments;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (b->ends[mid] < drawn)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (!lo)
        return TRUE;
    gboolean isolated = l->ink.a < .999;
    if (isolated) {
        glBindFramebuffer(GL_FRAMEBUFFER, g->layer_fbo);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glBlendEquation(GL_FUNC_ADD);
    }
    glUseProgram(g->program);
#define U(name) glGetUniformLocation(g->program, name)
    glUniform2f(U("viewport"), W, H);
    glUniform2f(U("pan"), g->view.pan_x, g->view.pan_y);
    glUniform1f(U("zoom"), g->view.zoom);
    glUniform1f(U("width"), fmax(.5, l->ink.line_width));
    glUniform1f(U("total"), l->path.count - 1);
    glUniform4f(U("ink"), l->ink.r, l->ink.g, l->ink.b, isolated ? 1 : l->ink.a);
    glUniform1i(U("rainbow"), l->ink.color_cycle);
    glBindVertexArray(b->vao);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, lo);
#undef U
    if (isolated) {
        glBlendEquation(GL_FUNC_ADD);
        glBindFramebuffer(GL_FRAMEBUFFER, target);
        composite(g, g->layer_texture, FALSE, l->ink.a);
    }
    return TRUE;
}
static gboolean render(GtkGLArea *area, GdkGLContext *c, gpointer data) {
    (void)c;
    (void)data;
    Gpu *g = state(GTK_WIDGET(area));
    if (!g->stats.ready)
        return TRUE;
    GtkWidget *w = GTK_WIDGET(area);
    int W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w),
        scale = gtk_widget_get_scale_factor(w);
    int width = W * scale, height = H * scale;
    GLint target;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &target);
    if (W < 1 || H < 1)
        return TRUE;
    if (g->width != width || g->height != height) {
        g->width = width;
        g->height = height;
        glBindTexture(GL_TEXTURE_2D, g->layer_texture);
        texture_settings();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glBindFramebuffer(GL_FRAMEBUFFER, g->layer_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               g->layer_texture, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            glBindFramebuffer(GL_FRAMEBUFFER, target);
            fail(area, "Cannot allocate the 2D layer framebuffer");
            return TRUE;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, target);
    }
    glViewport(0, 0, width, height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(.055, .06, .08, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    for (size_t i = 0; i < g->layer_count; i++)
        if (!draw_layer(area, g, &g->buffers[i], g->layers + i, 1, target, W, H))
            return TRUE;
    if (!draw_layer(area, g, &g->buffers[SP_MAX_LAYERS], g->current, g->progress, target, W, H))
        return TRUE;
    if (g->wheels || g->fourier) {
        cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
        if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(s);
            fail(area, "Cannot allocate the overlay surface");
            return TRUE;
        }
        cairo_t *cr = cairo_create(s);
        cairo_scale(cr, scale, scale);
        if (g->wheels)
            render_wheel_overlay(cr, W, H, g->wheels, g->progress, &g->view);
        else if (g->current)
            render_fourier_overlay(cr, W, H, g->fourier, &g->current->path, g->progress, &g->view);
        cairo_destroy(cr);
        cairo_surface_flush(s);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g->overlay_texture);
        texture_settings();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE,
                     cairo_image_surface_get_data(s));
        cairo_surface_destroy(s);
        composite(g, g->overlay_texture, TRUE, 1);
    }
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    g->stats.frames++;
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        char s[128];
        g_snprintf(s, sizeof s, "2D OpenGL error 0x%x", error);
        fail(area, s);
    }
    return TRUE;
}
GtkWidget *sp2_gpu_new(Sp2GpuNotify notify, gpointer data) {
    GtkWidget *w = gtk_gl_area_new();
    gtk_gl_area_set_use_es(GTK_GL_AREA(w), FALSE);
    gtk_gl_area_set_required_version(GTK_GL_AREA(w), 3, 3);
    gtk_gl_area_set_auto_render(GTK_GL_AREA(w), FALSE);
    Gpu *g = g_new0(Gpu, 1);
    g->notify = notify;
    g->data = data;
    g_object_set_data_full(G_OBJECT(w), "spiro2d-gpu", g, g_free);
    g_signal_connect(w, "realize", G_CALLBACK(realize), NULL);
    g_signal_connect(w, "unrealize", G_CALLBACK(unrealize), NULL);
    g_signal_connect(w, "render", G_CALLBACK(render), NULL);
    return w;
}
void sp2_gpu_scene(GtkWidget *w, const SpLayer *layers, size_t n, const SpLayer *current,
                   double progress, const SpViewTransform *view, const SpParams *wheels,
                   const SpFourier *fourier) {
    Gpu *g = state(w);
    g->layers = layers;
    g->layer_count = MIN(n, SP_MAX_LAYERS);
    g->current = current;
    g->progress = progress;
    g->view = *view;
    g->wheels = wheels;
    g->fourier = fourier;
    gtk_gl_area_queue_render(GTK_GL_AREA(w));
}
void sp2_gpu_geometry_changed(GtkWidget *w) {
    Gpu *g = state(w);
    for (size_t i = 0; i < G_N_ELEMENTS(g->buffers); i++)
        g->buffers[i].dirty = TRUE;
}
void sp2_gpu_stats(GtkWidget *w, Sp2GpuStats *out) {
    *out = state(w)->stats;
}
gboolean sp2_gpu_snapshot(GtkWidget *w, const char *file, GError **error) {
    gtk_gl_area_make_current(GTK_GL_AREA(w));
    Gpu *g = state(w);
    if (!g->stats.ready) {
        g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "2D OpenGL is unavailable");
        return FALSE;
    }
    gtk_gl_area_attach_buffers(GTK_GL_AREA(w));
    render(GTK_GL_AREA(w), gtk_gl_area_get_context(GTK_GL_AREA(w)), NULL);
    int W = g->width, H = g->height;
    guchar *p = g_try_malloc((gsize)W * H * 4);
    if (!p)
        return FALSE;
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, p);
    GdkPixbuf *raw =
        gdk_pixbuf_new_from_data(p, GDK_COLORSPACE_RGB, TRUE, 8, W, H, W * 4, NULL, NULL);
    GdkPixbuf *flipped = gdk_pixbuf_flip(raw, FALSE);
    gboolean ok = flipped && gdk_pixbuf_save(flipped, file, "png", error, NULL);
    g_clear_object(&flipped);
    g_object_unref(raw);
    g_free(p);
    return ok;
}
