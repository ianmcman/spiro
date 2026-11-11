#include "gpu3d.h"
#include <cairo.h>
#include <epoxy/gl.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#define GPU_ERROR (g_quark_from_static_string("spirolab-opengl"))
typedef struct {
    GLfloat position[3], normal[3], fraction;
} Vertex;
typedef struct {
    GLuint vao, vbo, ebo;
    GLsizei count;
} Buffer;
typedef struct {
    Sp3Config config;
    Sp3Curve curve; /* Borrowed only until the owning window destroys the GL area. */
    Sp3View view;
    double progress, phase, last_arm_phase;
    gboolean line_dirty, tube_dirty, arm_dirty;
    Sp3GpuStats stats;
    Sp3GpuNotify notify;
    gpointer notify_data;
    GLuint program, msaa, color, depth, timer[2];
    int width, height, samples;
    GLfloat line_range[2];
    gboolean query_pending[2];
    unsigned query_slot;
    Buffer line, tube, arms, axes;
    size_t tube_rings;
    GLint rotation, aspect, pan, zoom, hue, lit, tint, points, point_size, overlay, viewport;
} Gpu;
static const char *vertex_shader =
    "#version 330 core\n"
    "layout(location=0) in vec3 position;\n"
    "layout(location=1) in vec3 normal;\n"
    "layout(location=2) in float fraction;\n"
    "uniform mat3 rotation;uniform vec2 aspect,pan,viewport;\n"
    "uniform float zoom,point_size;uniform int overlay;\n"
    "out vec3 n;out float t;out float depth;\n"
    "void main(){vec3 p=rotation*position;n=rotation*normal;t=fraction;depth=p.z;\n"
    "if(overlay==1){gl_Position=vec4(vec2(-1.0)+vec2(90.0)/viewport+vec2(50.0)/"
    "viewport*p.xy,0,1);}\n"
    "else{float d=4.5-p.z;gl_Position=vec4(p.xy*3.51*zoom*aspect+pan*d,1.002002*d-0.2002002,d);}\n"
    "gl_PointSize=point_size;}\n";
static const char *fragment_shader =
    "#version 330 core\n"
    "in vec3 n;in float t;in float depth;out vec4 color;\n"
    "uniform vec2 hue;uniform int lit,points;uniform vec4 tint;\n"
    "vec3 hsv(float h){vec3 p=abs(fract(vec3(h)+vec3(0,2.0/3.0,1.0/3.0))*6.0-3.0);"
    "return 0.98*mix(vec3(1),clamp(p-1.0,0.0,1.0),0.75);}\n"
    "void main(){if(points==1 && length(gl_PointCoord-vec2(0.5))>0.5)discard;\n"
    "if(tint.a>0){color=tint;return;}\n"
    "float light=lit==1?0.3+0.7*max(dot(normalize(n),normalize(vec3(-0.4,0.7,1))),0.0):"
    "0.72+0.25*clamp((depth+1.8)/3.6,0.0,1.0);\n"
    "color=vec4(hsv((hue.x+hue.y*t)/360.0)*light,1);}\n";
static Gpu *state(GtkWidget *area) {
    return g_object_get_data(G_OBJECT(area), "spiro3d-gpu");
}
static void report(GtkGLArea *area, Gpu *g, const char *message) {
    if (g->stats.error[0])
        return;
    g_strlcpy(g->stats.error, message, sizeof g->stats.error);
    g->stats.ready = FALSE;
    GError *e = g_error_new(GPU_ERROR, 1, "%s", message);
    gtk_gl_area_set_error(area, e);
    g_error_free(e);
    if (g->notify)
        g->notify(GTK_WIDGET(area), g->notify_data);
}
static GLuint shader(GtkGLArea *area, Gpu *g, GLenum kind, const char *source) {
    GLuint s = glCreateShader(kind);
    glShaderSource(s, 1, &source, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        glDeleteShader(s);
        report(area, g, log);
        return 0;
    }
    return s;
}
static void release_buffer(Buffer *b) {
    if (b->ebo)
        glDeleteBuffers(1, &b->ebo);
    if (b->vbo)
        glDeleteBuffers(1, &b->vbo);
    if (b->vao)
        glDeleteVertexArrays(1, &b->vao);
    *b = (Buffer){0};
}
static void create_buffer(Buffer *b) {
    glGenVertexArrays(1, &b->vao);
    glBindVertexArray(b->vao);
    glGenBuffers(1, &b->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, b->vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (void *)offsetof(Vertex, position));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (void *)offsetof(Vertex, normal));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (void *)offsetof(Vertex, fraction));
}
static void release_msaa(Gpu *g) {
    if (g->msaa)
        glDeleteFramebuffers(1, &g->msaa);
    if (g->color)
        glDeleteRenderbuffers(1, &g->color);
    if (g->depth)
        glDeleteRenderbuffers(1, &g->depth);
    g->msaa = g->color = g->depth = 0;
    g->width = g->height = 0;
}
static void unrealize(GtkGLArea *area, gpointer unused) {
    (void)unused;
    Gpu *g = state(GTK_WIDGET(area));
    GdkGLContext *context = gtk_gl_area_get_context(area);
    if (context)
        gdk_gl_context_make_current(context);
    if (context && gdk_gl_context_get_current() == context) {
        release_buffer(&g->line);
        release_buffer(&g->tube);
        release_buffer(&g->arms);
        release_buffer(&g->axes);
        release_msaa(g);
        if (g->program)
            glDeleteProgram(g->program);
        if (g->timer[0])
            glDeleteQueries(2, g->timer);
    }
    g->program = 0;
    g->timer[0] = g->timer[1] = 0;
    g->stats.ready = FALSE;
    g->line_dirty = g->tube_dirty = g->arm_dirty = TRUE;
    memset(g->query_pending, 0, sizeof g->query_pending);
}
static void realize(GtkGLArea *area, gpointer unused) {
    (void)unused;
    Gpu *g = state(GTK_WIDGET(area));
    gtk_gl_area_make_current(area);
    GError *e = gtk_gl_area_get_error(area);
    if (e) {
        report(area, g, e->message);
        return;
    }
    if (!epoxy_is_desktop_gl() || epoxy_gl_version() < 33) {
        report(area, g, "OpenGL 3.3 is required for accelerated 3D rendering");
        return;
    }
    const char *r = (const char *)glGetString(GL_RENDERER);
    g_strlcpy(g->stats.renderer, r ? r : "Unknown OpenGL renderer", sizeof g->stats.renderer);
    char *lower = g_ascii_strdown(g->stats.renderer, -1);
    g->stats.hardware = !strstr(lower, "llvmpipe") && !strstr(lower, "softpipe") &&
                        !strstr(lower, "software") && !strstr(lower, "swrast");
    g_free(lower);
    GLuint vs = shader(area, g, GL_VERTEX_SHADER, vertex_shader);
    if (!vs)
        return;
    GLuint fs = shader(area, g, GL_FRAGMENT_SHADER, fragment_shader);
    if (!fs) {
        glDeleteShader(vs);
        return;
    }
    g->program = glCreateProgram();
    glAttachShader(g->program, vs);
    glAttachShader(g->program, fs);
    glLinkProgram(g->program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(g->program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(g->program, sizeof log, NULL, log);
        report(area, g, log);
        return;
    }
#define UNIFORM(name) g->name = glGetUniformLocation(g->program, #name)
    UNIFORM(rotation);
    UNIFORM(aspect);
    UNIFORM(pan);
    UNIFORM(zoom);
    UNIFORM(hue);
    UNIFORM(lit);
    UNIFORM(tint);
    UNIFORM(points);
    UNIFORM(point_size);
    UNIFORM(overlay);
    UNIFORM(viewport);
#undef UNIFORM
    create_buffer(&g->line);
    create_buffer(&g->tube);
    glGenBuffers(1, &g->tube.ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g->tube.ebo);
    create_buffer(&g->arms);
    create_buffer(&g->axes);
    Vertex axes[6] = {0};
    axes[1].position[0] = 1;
    axes[3].position[1] = 1;
    axes[5].position[2] = 1;
    glBufferData(GL_ARRAY_BUFFER, sizeof axes, axes, GL_STATIC_DRAW);
    g->axes.count = 6;
    glGetFloatv(GL_ALIASED_LINE_WIDTH_RANGE, g->line_range);
    glGenQueries(2, g->timer);
    GLint samples = 0;
    glGetIntegerv(GL_MAX_SAMPLES, &samples);
    g->samples = MIN(g->stats.hardware ? 4 : 2, samples);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    g->stats.error[0] = 0;
    g->stats.ready = TRUE;
    if (g->notify)
        g->notify(GTK_WIDGET(area), g->notify_data);
}
static void msaa_target(Gpu *g, int width, int height, GLuint target) {
    if (g->width == width && g->height == height)
        return;
    release_msaa(g);
    g->width = width;
    g->height = height;
    if (g->samples < 2)
        return;
    glGenFramebuffers(1, &g->msaa);
    glBindFramebuffer(GL_FRAMEBUFFER, g->msaa);
    glGenRenderbuffers(1, &g->color);
    glBindRenderbuffer(GL_RENDERBUFFER, g->color);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, g->samples, GL_RGBA8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g->color);
    glGenRenderbuffers(1, &g->depth);
    glBindRenderbuffer(GL_RENDERBUFFER, g->depth);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, g->samples, GL_DEPTH_COMPONENT24, width,
                                     height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g->depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        release_msaa(g);
        g->width = width;
        g->height = height;
    }
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, target);
}
static Vertex vertex(Sp3Vec p, Sp3Vec normal, float fraction) {
    return (Vertex){{(float)p.x, (float)p.y, (float)p.z},
                    {(float)normal.x, (float)normal.y, (float)normal.z},
                    fraction};
}
static Sp3Vec normalized(Sp3Vec p, const Sp3Curve *c) {
    return (Sp3Vec){(p.x - c->center.x) * c->scale, (p.y - c->center.y) * c->scale,
                    (p.z - c->center.z) * c->scale};
}
static gboolean upload_line(GtkGLArea *area, Gpu *g) {
    size_t n = g->curve.count;
    Vertex *v = g_try_new(Vertex, n);
    if (!v) {
        report(area, g, "Cannot allocate the 3D line buffer");
        return FALSE;
    }
    for (size_t i = 0; i < n; i++)
        v[i] = vertex(normalized(g->curve.points[i], &g->curve), (Sp3Vec){0}, (float)i / (n - 1));
    glBindBuffer(GL_ARRAY_BUFFER, g->line.vbo);
    glBufferData(GL_ARRAY_BUFFER, n * sizeof *v, v, GL_STATIC_DRAW);
    g_free(v);
    g->line.count = (GLsizei)n;
    g->line_dirty = FALSE;
    g->stats.line_uploads++;
    return TRUE;
}
static gboolean upload_tube(GtkGLArea *area, Gpu *g) {
    Sp3Mesh mesh = {0};
    if (!sp3_mesh(&g->curve, g->config.size_mm, g->config.tube_mm, 12, &mesh)) {
        /* A stationary pen is a valid scene, but cannot form a tube. */
        g->tube.count = 0;
        g->tube_dirty = FALSE;
        return TRUE;
    }
    Vertex *v = g_try_new(Vertex, mesh.vertex_count);
    if (!v) {
        sp3_mesh_free(&mesh);
        report(area, g, "Cannot allocate the 3D tube buffer");
        return FALSE;
    }
    size_t rings = g->curve.count - 1;
    double k = 2. / g->config.size_mm;
    for (size_t i = 0; i < mesh.vertex_count; i++) {
        Sp3Vec p = {mesh.vertices[i].x * k, mesh.vertices[i].y * k, mesh.vertices[i].z * k},
               center = normalized(g->curve.points[i / 12], &g->curve);
        Sp3Vec normal = {p.x - center.x, p.y - center.y, p.z - center.z};
        double d = sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
        if (d > 1e-14) {
            normal.x /= d;
            normal.y /= d;
            normal.z /= d;
        } else
            normal = (Sp3Vec){0, 1, 0};
        v[i] = vertex(p, normal, (float)(i / 12) / rings);
    }
    /* Software OpenGL still benefits from retained buffers, but needs a lighter
       preview. Keep full vertices/normals; coarsen only its index connections.
       Hardware and STL exports retain the full selected sample count. */
    size_t step = (!g->stats.hardware && g_strcmp0(g_getenv("SPIROLAB_GL_FULL_MESH"), "1"))
                      ? MAX((size_t)1, (rings + 1599) / 1600)
                      : 1;
    size_t displayed = (rings + step - 1) / step, index_count = displayed * 12 * 6;
    uint32_t *indices = mesh.indices;
    if (step > 1) {
        indices = g_try_new(uint32_t, index_count);
        if (!indices) {
            g_free(v);
            sp3_mesh_free(&mesh);
            report(area, g, "Cannot allocate the preview index buffer");
            return FALSE;
        }
        size_t at = 0;
        for (size_t ring = 0; ring < rings; ring += step) {
            size_t next = ring + step < rings ? ring + step : 0;
            for (unsigned j = 0; j < 12; j++) {
                uint32_t a = (uint32_t)(ring * 12 + j), d = (uint32_t)(ring * 12 + (j + 1) % 12);
                uint32_t b = (uint32_t)(next * 12 + j), c = (uint32_t)(next * 12 + (j + 1) % 12);
                indices[at++] = a;
                indices[at++] = d;
                indices[at++] = c;
                indices[at++] = a;
                indices[at++] = c;
                indices[at++] = b;
            }
        }
    }
    g->tube_rings = displayed;
    glBindVertexArray(g->tube.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g->tube.vbo);
    glBufferData(GL_ARRAY_BUFFER, mesh.vertex_count * sizeof *v, v, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g->tube.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, index_count * sizeof(uint32_t), indices, GL_STATIC_DRAW);
    g->tube.count = (GLsizei)index_count;
    if (step > 1)
        g_free(indices);
    g_free(v);
    sp3_mesh_free(&mesh);
    g->tube_dirty = FALSE;
    g->stats.tube_uploads++;
    return TRUE;
}
static void upload_arms(Gpu *g) {
    Sp3Vec joints[17];
    if (!sp3_joints(&g->config, g->phase * 6.283185307179586, joints)) {
        g->arms.count = 0;
        return;
    }
    Vertex v[17];
    for (size_t i = 0; i <= g->config.arm_count; i++)
        v[i] = vertex(normalized(joints[i], &g->curve), (Sp3Vec){0}, 0);
    glBindBuffer(GL_ARRAY_BUFFER, g->arms.vbo);
    glBufferData(GL_ARRAY_BUFFER, (g->config.arm_count + 1) * sizeof(Vertex), v, GL_STREAM_DRAW);
    g->arms.count = g->config.arm_count + 1;
    g->arm_dirty = FALSE;
    g->last_arm_phase = g->phase;
}
static gboolean render(GtkGLArea *area, GdkGLContext *context, gpointer unused) {
    (void)context;
    (void)unused;
    Gpu *g = state(GTK_WIDGET(area));
    if (!g->stats.ready || !g->curve.points || g->curve.count < 2)
        return TRUE;
    gint64 start = g_get_monotonic_time();
    GLint target = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &target);
    if (g->line_dirty && !upload_line(area, g))
        return TRUE;
    if (g->config.tube && g->tube_dirty && !upload_tube(area, g))
        return TRUE;
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        char message[128];
        g_snprintf(message, sizeof message, "OpenGL buffer upload failed (0x%x)", error);
        report(area, g, message);
        return TRUE;
    }
    int scale = gtk_widget_get_scale_factor(GTK_WIDGET(area)),
        width = gtk_widget_get_allocated_width(GTK_WIDGET(area)) * scale,
        height = gtk_widget_get_allocated_height(GTK_WIDGET(area)) * scale;
    if (width < 1 || height < 1)
        return TRUE;
    msaa_target(g, width, height, target);
    glBindFramebuffer(GL_FRAMEBUFFER, g->msaa ? g->msaa : (GLuint)target);
    glViewport(0, 0, width, height);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_MULTISAMPLE);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(.055f, .06f, .08f, 1);
    glClearDepth(1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    unsigned slot = g->query_slot++ % 2;
    gboolean timed = FALSE;
    if (g->query_pending[slot]) {
        GLuint available = 0;
        glGetQueryObjectuiv(g->timer[slot], GL_QUERY_RESULT_AVAILABLE, &available);
        if (available) {
            GLuint64 ns = 0;
            glGetQueryObjectui64v(g->timer[slot], GL_QUERY_RESULT, &ns);
            g->stats.gpu_ms = ns / 1e6;
            g->query_pending[slot] = FALSE;
        }
    }
    if (!g->query_pending[slot]) {
        glBeginQuery(GL_TIME_ELAPSED, g->timer[slot]);
        timed = TRUE;
    }
    glUseProgram(g->program);
    double cy = cos(g->view.yaw), sy = sin(g->view.yaw), cp = cos(g->view.pitch),
           sp = sin(g->view.pitch);
    GLfloat rotation[9] = {(float)cy, (float)(sp * sy),  (float)(-cp * sy), 0, (float)cp, (float)sp,
                           (float)sy, (float)(-sp * cy), (float)(cp * cy)};
    glUniformMatrix3fv(g->rotation, 1, GL_FALSE, rotation);
    float m = MIN(width, height);
    glUniform2f(g->aspect, m / width, m / height);
    glUniform2f(g->pan, (float)(2 * g->view.pan_x * m / width),
                (float)(-2 * g->view.pan_y * m / height));
    glUniform1f(g->zoom, (float)g->view.zoom);
    glUniform2f(g->hue, (float)g->config.hue, (float)g->config.hue_range);
    glUniform4f(g->tint, 0, 0, 0, 0);
    glUniform1i(g->points, 0);
    glUniform1i(g->overlay, 0);
    glUniform2f(g->viewport, (float)width / scale, (float)height / scale);
    glUniform1f(g->point_size, 6.f * scale);
    glEnable(GL_PROGRAM_POINT_SIZE);
    if (g->config.tube && g->tube.count) {
        glUniform1i(g->lit, 1);
        glBindVertexArray(g->tube.vao);
        size_t rings = (size_t)(CLAMP(g->progress, 0., 1.) * g->tube_rings);
        GLsizei count = (GLsizei)MIN(rings * 12 * 6, (size_t)g->tube.count);
        glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_INT, 0);
    } else {
        glUniform1i(g->lit, 0);
        glBindVertexArray(g->line.vao);
        glLineWidth(CLAMP(1.6f * scale, g->line_range[0], g->line_range[1]));
        size_t count = (size_t)(CLAMP(g->progress, 0., 1.) * (g->curve.count - 1)) + 1;
        glDrawArrays(GL_LINE_STRIP, 0, (GLsizei)MIN(count, (size_t)g->line.count));
    }
    glDisable(GL_DEPTH_TEST);
    glUniform1i(g->lit, 0);
    if (g->config.show_arms) {
        if (g->arm_dirty || g->phase != g->last_arm_phase)
            upload_arms(g);
        glBindVertexArray(g->arms.vao);
        glUniform4f(g->tint, 1, .88f, .48f, .85f);
        glLineWidth(CLAMP(1.f * scale, g->line_range[0], g->line_range[1]));
        glDrawArrays(GL_LINE_STRIP, 0, g->arms.count);
        glUniform1i(g->points, 1);
        glDrawArrays(GL_POINTS, 0, g->arms.count);
        if (g->arms.count) {
            glUniform4f(g->tint, 1, .3f, .2f, 1);
            glUniform1f(g->point_size, 8.f * scale);
            glDrawArrays(GL_POINTS, g->arms.count - 1, 1);
        }
        glUniform1i(g->points, 0);
    }
    glUniform1i(g->overlay, 1);
    glBindVertexArray(g->axes.vao);
    glLineWidth(CLAMP(1.f * scale, g->line_range[0], g->line_range[1]));
    const float colors[3][3] = {{.95f, .35f, .35f}, {.35f, .9f, .35f}, {.35f, .35f, 1}};
    for (int i = 0; i < 3; i++) {
        glUniform4f(g->tint, colors[i][0], colors[i][1], colors[i][2], 1);
        glDrawArrays(GL_LINES, 2 * i, 2);
    }
    if (timed) {
        glEndQuery(GL_TIME_ELAPSED);
        g->query_pending[slot] = TRUE;
    }
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
    if (g->msaa) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g->msaa);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
        glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT,
                          GL_NEAREST);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    g->stats.frames++;
    g->stats.submit_ms = (g_get_monotonic_time() - start) / 1000.;
    error = glGetError();
    if (error != GL_NO_ERROR) {
        char message[128];
        g_snprintf(message, sizeof message, "OpenGL rendering failed (0x%x)", error);
        report(area, g, message);
    }
    return TRUE;
}
GtkWidget *sp3_gpu_new(Sp3GpuNotify notify, gpointer data) {
    GtkWidget *area = gtk_gl_area_new();
    gtk_gl_area_set_use_es(GTK_GL_AREA(area), FALSE);
    gtk_gl_area_set_required_version(GTK_GL_AREA(area), 3, 3);
    gtk_gl_area_set_has_depth_buffer(GTK_GL_AREA(area), TRUE);
    gtk_gl_area_set_auto_render(GTK_GL_AREA(area), FALSE);
    Gpu *g = g_new0(Gpu, 1);
    g->notify = notify;
    g->notify_data = data;
    g->line_dirty = g->tube_dirty = g->arm_dirty = TRUE;
    g->last_arm_phase = -1;
    g_object_set_data_full(G_OBJECT(area), "spiro3d-gpu", g, g_free);
    g_signal_connect(area, "realize", G_CALLBACK(realize), NULL);
    g_signal_connect(area, "unrealize", G_CALLBACK(unrealize), NULL);
    g_signal_connect(area, "render", G_CALLBACK(render), NULL);
    return area;
}
void sp3_gpu_scene(GtkWidget *area, const Sp3Config *config, const Sp3Curve *curve,
                   const Sp3View *view, double progress, double phase) {
    Gpu *g = state(area);
    if (!g)
        return;
    if (g->curve.points != curve->points || g->curve.count != curve->count)
        g->line_dirty = g->tube_dirty = g->arm_dirty = TRUE;
    if (g->config.size_mm != config->size_mm || g->config.tube_mm != config->tube_mm)
        g->tube_dirty = TRUE;
    g->config = *config;
    g->curve = *curve;
    g->view = *view;
    g->progress = progress;
    g->phase = phase;
    gtk_gl_area_queue_render(GTK_GL_AREA(area));
}
void sp3_gpu_geometry_changed(GtkWidget *area) {
    Gpu *g = state(area);
    if (g)
        g->line_dirty = g->tube_dirty = g->arm_dirty = TRUE;
}
void sp3_gpu_stats(GtkWidget *area, Sp3GpuStats *out) {
    Gpu *g = state(area);
    if (out)
        *out = g ? g->stats : (Sp3GpuStats){0};
}
gboolean sp3_gpu_snapshot(GtkWidget *widget, const char *file, GError **error) {
    GtkGLArea *area = GTK_GL_AREA(widget);
    Gpu *g = state(widget);
    if (!g || !g->stats.ready) {
        g_set_error(error, GPU_ERROR, 1, "OpenGL is not ready");
        return FALSE;
    }
    gtk_gl_area_make_current(area);
    gtk_gl_area_attach_buffers(area);
    render(area, gtk_gl_area_get_context(area), NULL);
    if (!g->stats.ready) {
        g_set_error(error, GPU_ERROR, 1, "%s", g->stats.error);
        return FALSE;
    }
    int width = gtk_widget_get_allocated_width(widget) * gtk_widget_get_scale_factor(widget),
        height = gtk_widget_get_allocated_height(widget) * gtk_widget_get_scale_factor(widget);
    unsigned char *p = g_try_malloc_n((gsize)width * height, 4);
    if (!p) {
        g_set_error(error, GPU_ERROR, 1, "Cannot allocate screenshot");
        return FALSE;
    }
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, p);
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        g_free(p);
        cairo_surface_destroy(s);
        g_set_error(error, GPU_ERROR, 1, "Cannot allocate screenshot surface");
        return FALSE;
    }
    unsigned char *d = cairo_image_surface_get_data(s);
    int stride = cairo_image_surface_get_stride(s);
    for (int y = 0; y < height; y++) {
        uint32_t *row = (uint32_t *)(d + y * stride);
        for (int x = 0; x < width; x++) {
            unsigned char *v = p + 4 * ((height - 1 - y) * width + x);
            row[x] = (uint32_t)v[3] << 24 | (uint32_t)v[0] << 16 | (uint32_t)v[1] << 8 | v[2];
        }
    }
    g_free(p);
    cairo_surface_mark_dirty(s);
    cairo_status_t status = cairo_surface_write_to_png(s, file);
    cairo_surface_destroy(s);
    if (status != CAIRO_STATUS_SUCCESS) {
        g_set_error(error, GPU_ERROR, 1, "Screenshot failed: %s", cairo_status_to_string(status));
        return FALSE;
    }
    return TRUE;
}
