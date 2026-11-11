#include "ai.h"
#include <cairo.h>
#include <math.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <onnxruntime_c_api.h>
#pragma GCC diagnostic pop
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPIRO_AI_ERROR (g_quark_from_static_string("spiro-ai"))

struct SpAi {
    const OrtApi *ort;
    OrtEnv *env;
    OrtSessionOptions *opts;
    char *model_dir;
    OrtSession *u2net_session;
    OrtSession *det_session;
    OrtSession *lm_session;
};

/* Validate the buffer before interpreting tensor bytes as float32. */
static bool float_tensor(SpAi *ai, OrtValue *value, size_t expected, float **data, GError **error) {
    OrtTensorTypeAndShapeInfo *info = NULL;
    size_t count = 0;
    ONNXTensorElementDataType type = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    OrtStatus *st = ai->ort->GetTensorTypeAndShape(value, &info);
    if (!st)
        st = ai->ort->GetTensorElementType(info, &type);
    if (!st)
        st = ai->ort->GetTensorShapeElementCount(info, &count);
    if (info)
        ai->ort->ReleaseTensorTypeAndShapeInfo(info);
    if (st) {
        g_set_error(error, SPIRO_AI_ERROR, 1, "Cannot inspect model output: %s",
                    ai->ort->GetErrorMessage(st));
        ai->ort->ReleaseStatus(st);
        return false;
    }
    if (type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || count != expected) {
        g_set_error(error, SPIRO_AI_ERROR, 1,
                    "Unexpected model output type/size (expected %zu float32 values, got %zu)",
                    expected, count);
        return false;
    }
    st = ai->ort->GetTensorMutableData(value, (void **)data);
    if (st) {
        g_set_error(error, SPIRO_AI_ERROR, 1, "Cannot read model output: %s",
                    ai->ort->GetErrorMessage(st));
        ai->ort->ReleaseStatus(st);
        return false;
    }
    return true;
}

typedef struct {
    float x;
    float y;
} Point;

typedef struct {
    float score;
    float x1;
    float y1;
    float x2;
    float y2;
} FaceBox;

typedef enum { BIN_MIN, BIN_MAX, BIN_MID } BinMode;

typedef struct {
    Point pt;
    float angle;
} AnglePoint;

static const int JAW_INDICES[33] = {1,  9,  10, 11, 12, 13, 14, 15, 16, 2,  3,
                                    4,  5,  6,  7,  8,  0,  24, 23, 22, 21, 20,
                                    19, 18, 32, 31, 30, 29, 28, 27, 26, 25, 17};

SpAi *sp_ai_new(const char *model_dir, GError **error) {
    if (!model_dir || model_dir[0] == '\0') {
        g_set_error(error, SPIRO_AI_ERROR, 1, "Model directory must be specified");
        return NULL;
    }

    const OrtApiBase *base = OrtGetApiBase();
    if (!base) {
        g_set_error(error, SPIRO_AI_ERROR, 1, "Failed to get ONNX Runtime API base");
        return NULL;
    }

    const OrtApi *ort = base->GetApi(ORT_API_VERSION);
    if (!ort) {
        g_set_error(error, SPIRO_AI_ERROR, 1, "Failed to initialize ONNX Runtime API version %u",
                    (unsigned)ORT_API_VERSION);
        return NULL;
    }

    SpAi *ai = g_new0(SpAi, 1);
    ai->ort = ort;
    ai->model_dir = g_strdup(model_dir);

    OrtStatus *st = ai->ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "spiro_ai", &ai->env);
    if (st) {
        g_set_error(error, SPIRO_AI_ERROR, 1, "CreateEnv failed: %s", ai->ort->GetErrorMessage(st));
        ai->ort->ReleaseStatus(st);
        g_free(ai->model_dir);
        g_free(ai);
        return NULL;
    }

    st = ai->ort->CreateSessionOptions(&ai->opts);
    if (st) {
        g_set_error(error, SPIRO_AI_ERROR, 1, "CreateSessionOptions failed: %s",
                    ai->ort->GetErrorMessage(st));
        ai->ort->ReleaseStatus(st);
        ai->ort->ReleaseEnv(ai->env);
        g_free(ai->model_dir);
        g_free(ai);
        return NULL;
    }

    st = ai->ort->SetIntraOpNumThreads(ai->opts, 4);
    if (st) {
        g_set_error(error, SPIRO_AI_ERROR, 1, "SetIntraOpNumThreads failed: %s",
                    ai->ort->GetErrorMessage(st));
        ai->ort->ReleaseStatus(st);
        ai->ort->ReleaseSessionOptions(ai->opts);
        ai->ort->ReleaseEnv(ai->env);
        g_free(ai->model_dir);
        g_free(ai);
        return NULL;
    }

    return ai;
}

void sp_ai_free(SpAi *ai) {
    if (!ai)
        return;

    if (ai->u2net_session)
        ai->ort->ReleaseSession(ai->u2net_session);
    if (ai->det_session)
        ai->ort->ReleaseSession(ai->det_session);
    if (ai->lm_session)
        ai->ort->ReleaseSession(ai->lm_session);

    if (ai->opts)
        ai->ort->ReleaseSessionOptions(ai->opts);
    if (ai->env)
        ai->ort->ReleaseEnv(ai->env);

    if (ai->model_dir)
        g_free(ai->model_dir);
    g_free(ai);
}

static OrtSession *get_u2net_session(SpAi *ai, GError **error) {
    if (ai->u2net_session)
        return ai->u2net_session;

    char *path = g_build_filename(ai->model_dir, "u2net.onnx", NULL);
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_free(path);
        path = g_build_filename(ai->model_dir, "u2netp.onnx", NULL);
        if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
            g_free(path);
            g_set_error(error, SPIRO_AI_ERROR, 1, "Neither u2net.onnx nor u2netp.onnx found in %s",
                        ai->model_dir);
            return NULL;
        }
    }

    OrtStatus *st = ai->ort->CreateSession(ai->env, path, ai->opts, &ai->u2net_session);
    g_free(path);
    if (st) {
        g_set_error(error, SPIRO_AI_ERROR, 1, "Failed to load U2-Net session: %s",
                    ai->ort->GetErrorMessage(st));
        ai->ort->ReleaseStatus(st);
        return NULL;
    }
    return ai->u2net_session;
}

static OrtSession *get_det_session(SpAi *ai, GError **error) {
    if (ai->det_session)
        return ai->det_session;

    char *path = g_build_filename(ai->model_dir, "det_500m.onnx", NULL);
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_free(path);
        g_set_error(error, SPIRO_AI_ERROR, 1, "det_500m.onnx not found in %s", ai->model_dir);
        return NULL;
    }

    OrtStatus *st = ai->ort->CreateSession(ai->env, path, ai->opts, &ai->det_session);
    g_free(path);
    if (st) {
        g_set_error(error, SPIRO_AI_ERROR, 1, "Failed to load SCRFD detector session: %s",
                    ai->ort->GetErrorMessage(st));
        ai->ort->ReleaseStatus(st);
        return NULL;
    }
    return ai->det_session;
}

static OrtSession *get_lm_session(SpAi *ai, GError **error) {
    if (ai->lm_session)
        return ai->lm_session;

    char *path = g_build_filename(ai->model_dir, "2d106det.onnx", NULL);
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_free(path);
        g_set_error(error, SPIRO_AI_ERROR, 1, "2d106det.onnx not found in %s", ai->model_dir);
        return NULL;
    }

    OrtStatus *st = ai->ort->CreateSession(ai->env, path, ai->opts, &ai->lm_session);
    g_free(path);
    if (st) {
        g_set_error(error, SPIRO_AI_ERROR, 1, "Failed to load landmark session: %s",
                    ai->ort->GetErrorMessage(st));
        ai->ort->ReleaseStatus(st);
        return NULL;
    }
    return ai->lm_session;
}

static float *compute_u2net_mask(SpAi *ai, GdkPixbuf *input, GError **error) {
    OrtSession *session = get_u2net_session(ai, error);
    if (!session)
        return NULL;

    OrtAllocator *allocator = NULL;
    OrtStatus *st = ai->ort->GetAllocatorWithDefaultOptions(&allocator);
    if (st) {
        g_set_error(error, SPIRO_AI_ERROR, 1, "GetAllocatorWithDefaultOptions failed: %s",
                    ai->ort->GetErrorMessage(st));
        ai->ort->ReleaseStatus(st);
        return NULL;
    }

    char *in_name = NULL;
    st = ai->ort->SessionGetInputName(session, 0, allocator, &in_name);
    if (st) {
        g_set_error(error, SPIRO_AI_ERROR, 1, "SessionGetInputName failed: %s",
                    ai->ort->GetErrorMessage(st));
        ai->ort->ReleaseStatus(st);
        return NULL;
    }

    char *out_name = NULL;
    st = ai->ort->SessionGetOutputName(session, 0, allocator, &out_name);
    if (st) {
        g_set_error(error, SPIRO_AI_ERROR, 1, "SessionGetOutputName failed: %s",
                    ai->ort->GetErrorMessage(st));
        ai->ort->ReleaseStatus(st);
        allocator->Free(allocator, in_name);
        return NULL;
    }

    const int S = 320;
    GdkPixbuf *scaled = gdk_pixbuf_scale_simple(input, S, S, GDK_INTERP_BILINEAR);
    if (!scaled) {
        g_set_error(error, SPIRO_AI_ERROR, 1, "Failed to scale image for U2-Net");
        allocator->Free(allocator, in_name);
        allocator->Free(allocator, out_name);
        return NULL;
    }

    int sc_channels = gdk_pixbuf_get_n_channels(scaled);
    int sc_rowstride = gdk_pixbuf_get_rowstride(scaled);
    const guchar *sc_pixels = gdk_pixbuf_get_pixels(scaled);

    float max_rgb = 1e-6f;
    for (int y = 0; y < S; y++) {
        const guchar *row = sc_pixels + y * sc_rowstride;
        for (int x = 0; x < S; x++) {
            const guchar *px = row + x * sc_channels;
            if (px[0] > max_rgb)
                max_rgb = px[0];
            if (px[1] > max_rgb)
                max_rgb = px[1];
            if (px[2] > max_rgb)
                max_rgb = px[2];
        }
    }

    const float mean[3] = {0.485f, 0.456f, 0.406f};
    const float sd[3] = {0.229f, 0.224f, 0.225f};
    float *in_data = g_new(float, 3 * S * S);

    for (int y = 0; y < S; y++) {
        const guchar *row = sc_pixels + y * sc_rowstride;
        for (int x = 0; x < S; x++) {
            const guchar *px = row + x * sc_channels;
            int idx = y * S + x;
            in_data[0 * S * S + idx] = ((px[0] / max_rgb) - mean[0]) / sd[0];
            in_data[1 * S * S + idx] = ((px[1] / max_rgb) - mean[1]) / sd[1];
            in_data[2 * S * S + idx] = ((px[2] / max_rgb) - mean[2]) / sd[2];
        }
    }
    g_object_unref(scaled);

    OrtMemoryInfo *mem_info = NULL;
    st = ai->ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem_info);
    if (st) {
        g_set_error(error, SPIRO_AI_ERROR, 1, "CreateCpuMemoryInfo failed: %s",
                    ai->ort->GetErrorMessage(st));
        ai->ort->ReleaseStatus(st);
        g_free(in_data);
        allocator->Free(allocator, in_name);
        allocator->Free(allocator, out_name);
        return NULL;
    }

    int64_t in_shape[4] = {1, 3, S, S};
    OrtValue *in_tensor = NULL;
    st = ai->ort->CreateTensorWithDataAsOrtValue(mem_info, in_data, 3 * S * S * sizeof(float),
                                                 in_shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                                                 &in_tensor);
    ai->ort->ReleaseMemoryInfo(mem_info);

    if (st) {
        g_set_error(error, SPIRO_AI_ERROR, 1, "CreateTensorWithDataAsOrtValue failed: %s",
                    ai->ort->GetErrorMessage(st));
        ai->ort->ReleaseStatus(st);
        g_free(in_data);
        allocator->Free(allocator, in_name);
        allocator->Free(allocator, out_name);
        return NULL;
    }

    const char *in_names[1] = {in_name};
    const char *out_names[1] = {out_name};
    OrtValue *out_tensor = NULL;

    st = ai->ort->Run(session, NULL, in_names, (const OrtValue *const *)&in_tensor, 1, out_names, 1,
                      &out_tensor);
    ai->ort->ReleaseValue(in_tensor);
    g_free(in_data);
    allocator->Free(allocator, in_name);
    allocator->Free(allocator, out_name);

    if (st) {
        g_set_error(error, SPIRO_AI_ERROR, 1, "U2-Net inference failed: %s",
                    ai->ort->GetErrorMessage(st));
        ai->ort->ReleaseStatus(st);
        return NULL;
    }

    float *out_raw = NULL;
    if (!float_tensor(ai, out_tensor, (size_t)S * S, &out_raw, error)) {
        ai->ort->ReleaseValue(out_tensor);
        return NULL;
    }
    for (int i = 0; i < S * S; i++)
        if (!isfinite(out_raw[i])) {
            g_set_error(error, SPIRO_AI_ERROR, 1, "Nonfinite segmentation output");
            ai->ort->ReleaseValue(out_tensor);
            return NULL;
        }

    float lo = 1e30f, hi = -1e30f;
    for (int i = 0; i < S * S; i++) {
        float val = out_raw[i];
        if (val < lo)
            lo = val;
        if (val > hi)
            hi = val;
    }
    float span = hi - lo;
    if (fabsf(span) < 1e-6f)
        span = 1.0f;

    GdkPixbuf *small_mask = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, S, S);
    int sm_stride = gdk_pixbuf_get_rowstride(small_mask);
    guchar *sm_pixels = gdk_pixbuf_get_pixels(small_mask);

    for (int y = 0; y < S; y++) {
        guchar *row = sm_pixels + y * sm_stride;
        for (int x = 0; x < S; x++) {
            float v = (out_raw[y * S + x] - lo) / span;
            if (v < 0.0f)
                v = 0.0f;
            if (v > 1.0f)
                v = 1.0f;
            guchar byte_val = (guchar)roundf(v * 255.0f);
            row[3 * x + 0] = byte_val;
            row[3 * x + 1] = byte_val;
            row[3 * x + 2] = byte_val;
        }
    }
    ai->ort->ReleaseValue(out_tensor);

    int W = gdk_pixbuf_get_width(input);
    int H = gdk_pixbuf_get_height(input);
    GdkPixbuf *big_mask = gdk_pixbuf_scale_simple(small_mask, W, H, GDK_INTERP_BILINEAR);
    g_object_unref(small_mask);

    if (!big_mask) {
        g_set_error(error, SPIRO_AI_ERROR, 1, "Failed to upscale mask to original dimensions");
        return NULL;
    }

    int bm_stride = gdk_pixbuf_get_rowstride(big_mask);
    const guchar *bm_pixels = gdk_pixbuf_get_pixels(big_mask);
    float *mask = g_new(float, W *H);

    for (int y = 0; y < H; y++) {
        const guchar *row = bm_pixels + y * bm_stride;
        for (int x = 0; x < W; x++) {
            mask[y * W + x] = row[3 * x] / 255.0f;
        }
    }
    g_object_unref(big_mask);

    return mask;
}

static int compare_boxes(const void *a, const void *b) {
    float sa = ((const FaceBox *)a)->score;
    float sb = ((const FaceBox *)b)->score;
    if (sb > sa)
        return 1;
    if (sb < sa)
        return -1;
    return 0;
}

static FaceBox *detect_faces(SpAi *ai, GdkPixbuf *input, int *out_count, GError **error) {
    *out_count = 0;
    OrtSession *session = get_det_session(ai, error);
    if (!session)
        return NULL;

    OrtAllocator *allocator = NULL;
    OrtStatus *st = ai->ort->GetAllocatorWithDefaultOptions(&allocator);
    if (st) {
        g_set_error(error, SPIRO_AI_ERROR, 1, "GetAllocatorWithDefaultOptions failed: %s",
                    ai->ort->GetErrorMessage(st));
        ai->ort->ReleaseStatus(st);
        return NULL;
    }

    char *in_name = NULL;
    st = ai->ort->SessionGetInputName(session, 0, allocator, &in_name);
    if (st) {
        g_set_error(error, SPIRO_AI_ERROR, 1, "SessionGetInputName failed: %s",
                    ai->ort->GetErrorMessage(st));
        ai->ort->ReleaseStatus(st);
        return NULL;
    }

    size_t num_outputs = 0;
    st = ai->ort->SessionGetOutputCount(session, &num_outputs);
    if (st || num_outputs < 9) {
        g_set_error(error, SPIRO_AI_ERROR, 1, "SCRFD expected at least 9 outputs, found %zu",
                    num_outputs);
        if (st)
            ai->ort->ReleaseStatus(st);
        allocator->Free(allocator, in_name);
        return NULL;
    }

    char *out_names[9];
    for (size_t i = 0; i < 9; i++) {
        out_names[i] = NULL;
        st = ai->ort->SessionGetOutputName(session, i, allocator, &out_names[i]);
        if (st) {
            g_set_error(error, SPIRO_AI_ERROR, 1, "SessionGetOutputName(%zu) failed: %s", i,
                        ai->ort->GetErrorMessage(st));
            ai->ort->ReleaseStatus(st);
            for (size_t j = 0; j <= i; j++) {
                if (out_names[j])
                    allocator->Free(allocator, out_names[j]);
            }
            allocator->Free(allocator, in_name);
            return NULL;
        }
    }

    int W = gdk_pixbuf_get_width(input);
    int H = gdk_pixbuf_get_height(input);
    const int S = 640;
    float k = fminf((float)S / (float)W, (float)S / (float)H);
    int sw = (int)roundf((float)W * k);
    int sh = (int)roundf((float)H * k);
    if (sw > S)
        sw = S;
    if (sh > S)
        sh = S;

    GdkPixbuf *scaled = gdk_pixbuf_scale_simple(input, sw, sh, GDK_INTERP_BILINEAR);
    if (!scaled) {
        g_set_error(error, SPIRO_AI_ERROR, 1, "Failed to scale image for face detection");
        allocator->Free(allocator, in_name);
        for (int i = 0; i < 9; i++)
            allocator->Free(allocator, out_names[i]);
        return NULL;
    }

    float *in_data = g_new(float, 3 * S * S);
    float pad_val = (0.0f - 127.5f) / 128.0f;
    for (int i = 0; i < 3 * S * S; i++)
        in_data[i] = pad_val;

    int sc_channels = gdk_pixbuf_get_n_channels(scaled);
    int sc_rowstride = gdk_pixbuf_get_rowstride(scaled);
    const guchar *sc_pixels = gdk_pixbuf_get_pixels(scaled);

    for (int y = 0; y < sh; y++) {
        const guchar *row = sc_pixels + y * sc_rowstride;
        for (int x = 0; x < sw; x++) {
            const guchar *px = row + x * sc_channels;
            int idx = y * S + x;
            in_data[0 * S * S + idx] = ((float)px[0] - 127.5f) / 128.0f;
            in_data[1 * S * S + idx] = ((float)px[1] - 127.5f) / 128.0f;
            in_data[2 * S * S + idx] = ((float)px[2] - 127.5f) / 128.0f;
        }
    }
    g_object_unref(scaled);

    OrtMemoryInfo *mem_info = NULL;
    st = ai->ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem_info);
    if (st) {
        g_set_error(error, SPIRO_AI_ERROR, 1, "CreateCpuMemoryInfo failed: %s",
                    ai->ort->GetErrorMessage(st));
        ai->ort->ReleaseStatus(st);
        g_free(in_data);
        allocator->Free(allocator, in_name);
        for (int i = 0; i < 9; i++)
            allocator->Free(allocator, out_names[i]);
        return NULL;
    }

    int64_t in_shape[4] = {1, 3, S, S};
    OrtValue *in_tensor = NULL;
    st = ai->ort->CreateTensorWithDataAsOrtValue(mem_info, in_data, 3 * S * S * sizeof(float),
                                                 in_shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                                                 &in_tensor);
    ai->ort->ReleaseMemoryInfo(mem_info);

    if (st) {
        g_set_error(error, SPIRO_AI_ERROR, 1, "CreateTensorWithDataAsOrtValue failed: %s",
                    ai->ort->GetErrorMessage(st));
        ai->ort->ReleaseStatus(st);
        g_free(in_data);
        allocator->Free(allocator, in_name);
        for (int i = 0; i < 9; i++)
            allocator->Free(allocator, out_names[i]);
        return NULL;
    }

    const char *in_names[1] = {in_name};
    const char *out_name_ptrs[9];
    for (int i = 0; i < 9; i++)
        out_name_ptrs[i] = out_names[i];
    OrtValue *out_tensors[9] = {NULL};

    st = ai->ort->Run(session, NULL, in_names, (const OrtValue *const *)&in_tensor, 1,
                      out_name_ptrs, 9, out_tensors);
    ai->ort->ReleaseValue(in_tensor);
    g_free(in_data);
    allocator->Free(allocator, in_name);
    for (int i = 0; i < 9; i++)
        allocator->Free(allocator, out_names[i]);

    if (st) {
        for (int j = 0; j < 9; j++)
            if (out_tensors[j])
                ai->ort->ReleaseValue(out_tensors[j]);
        g_set_error(error, SPIRO_AI_ERROR, 1, "SCRFD inference failed: %s",
                    ai->ort->GetErrorMessage(st));
        ai->ort->ReleaseStatus(st);
        return NULL;
    }

    const int strides[3] = {8, 16, 32};
    GArray *candidates = g_array_new(FALSE, FALSE, sizeof(FaceBox));

    for (int i = 0; i < 3; i++) {
        int stride = strides[i];
        int cols = S / stride;

        size_t sc_count = (size_t)cols * cols * 2;
        size_t bb_count = sc_count * 4;
        float *scores = NULL, *bboxes = NULL;
        if (!float_tensor(ai, out_tensors[i], sc_count, &scores, error) ||
            !float_tensor(ai, out_tensors[i + 3], bb_count, &bboxes, error)) {
            for (int j = 0; j < 9; j++)
                if (out_tensors[j])
                    ai->ort->ReleaseValue(out_tensors[j]);
            g_array_free(candidates, TRUE);
            return NULL;
        }

        for (size_t n = 0; n < sc_count; n++) {
            float score = scores[n];
            if (!isfinite(score) || score < 0.5f)
                continue;
            if (4 * n + 3 >= bb_count)
                break;

            int a = (int)(n / 2);
            float x = (float)((a % cols) * stride);
            float y = (float)((a / cols) * stride);

            float x1 = (x - bboxes[4 * n + 0] * (float)stride) / k;
            float y1 = (y - bboxes[4 * n + 1] * (float)stride) / k;
            float x2 = (x + bboxes[4 * n + 2] * (float)stride) / k;
            float y2 = (y + bboxes[4 * n + 3] * (float)stride) / k;

            if (!isfinite(x1) || !isfinite(y1) || !isfinite(x2) || !isfinite(y2))
                continue;
            if (x1 < 0.0f)
                x1 = 0.0f;
            if (y1 < 0.0f)
                y1 = 0.0f;
            if (x2 > (float)W)
                x2 = (float)W;
            if (y2 > (float)H)
                y2 = (float)H;

            if (x2 > x1 && y2 > y1) {
                FaceBox fb = {score, x1, y1, x2, y2};
                g_array_append_val(candidates, fb);
            }
        }
    }

    for (int i = 0; i < 9; i++) {
        if (out_tensors[i])
            ai->ort->ReleaseValue(out_tensors[i]);
    }

    if (candidates->len == 0) {
        g_array_free(candidates, TRUE);
        return NULL;
    }

    qsort(candidates->data, candidates->len, sizeof(FaceBox), compare_boxes);

    GArray *keep = g_array_new(FALSE, FALSE, sizeof(FaceBox));
    for (guint i = 0; i < candidates->len && keep->len < 32; i++) {
        FaceBox b = g_array_index(candidates, FaceBox, i);
        bool suppress = false;
        for (guint j = 0; j < keep->len; j++) {
            FaceBox q = g_array_index(keep, FaceBox, j);
            float iw = fminf(b.x2, q.x2) - fmaxf(b.x1, q.x1);
            float ih = fminf(b.y2, q.y2) - fmaxf(b.y1, q.y1);
            if (iw > 0.0f && ih > 0.0f) {
                float inter = iw * ih;
                float union_area =
                    (b.x2 - b.x1) * (b.y2 - b.y1) + (q.x2 - q.x1) * (q.y2 - q.y1) - inter;
                if (union_area > 0.0f && (inter / union_area) >= 0.4f) {
                    suppress = true;
                    break;
                }
            }
        }
        if (!suppress) {
            g_array_append_val(keep, b);
        }
    }
    g_array_free(candidates, TRUE);

    *out_count = (int)keep->len;
    return (FaceBox *)g_array_free(keep, FALSE);
}

static void sample_pixbuf_bilinear(GdkPixbuf *pb, float sx, float sy, float *rgb) {
    int w = gdk_pixbuf_get_width(pb);
    int h = gdk_pixbuf_get_height(pb);
    if (sx < 0.0f || sx >= (float)w || sy < 0.0f || sy >= (float)h) {
        rgb[0] = 0.0f;
        rgb[1] = 0.0f;
        rgb[2] = 0.0f;
        return;
    }

    int x0 = (int)floorf(sx);
    int y0 = (int)floorf(sy);
    int x1 = (x0 + 1 < w) ? x0 + 1 : x0;
    int y1 = (y0 + 1 < h) ? y0 + 1 : y0;

    float fx = sx - (float)x0;
    float fy = sy - (float)y0;
    float w00 = (1.0f - fx) * (1.0f - fy);
    float w10 = fx * (1.0f - fy);
    float w01 = (1.0f - fx) * fy;
    float w11 = fx * fy;

    int stride = gdk_pixbuf_get_rowstride(pb);
    int channels = gdk_pixbuf_get_n_channels(pb);
    const guchar *p = gdk_pixbuf_get_pixels(pb);

    const guchar *p00 = p + y0 * stride + x0 * channels;
    const guchar *p10 = p + y0 * stride + x1 * channels;
    const guchar *p01 = p + y1 * stride + x0 * channels;
    const guchar *p11 = p + y1 * stride + x1 * channels;

    for (int c = 0; c < 3; c++) {
        rgb[c] = w00 * p00[c] + w10 * p10[c] + w01 * p01[c] + w11 * p11[c];
    }
}

static bool extract_landmarks(SpAi *ai, GdkPixbuf *input, FaceBox box, Point *pts, GError **error) {
    OrtSession *session = get_lm_session(ai, error);
    if (!session)
        return false;

    OrtAllocator *allocator = NULL;
    OrtStatus *st = ai->ort->GetAllocatorWithDefaultOptions(&allocator);
    if (st) {
        g_set_error(error, SPIRO_AI_ERROR, 1, "GetAllocatorWithDefaultOptions failed: %s",
                    ai->ort->GetErrorMessage(st));
        ai->ort->ReleaseStatus(st);
        return false;
    }

    char *in_name = NULL;
    st = ai->ort->SessionGetInputName(session, 0, allocator, &in_name);
    if (st) {
        g_set_error(error, SPIRO_AI_ERROR, 1, "SessionGetInputName failed: %s",
                    ai->ort->GetErrorMessage(st));
        ai->ort->ReleaseStatus(st);
        return false;
    }

    char *out_name = NULL;
    st = ai->ort->SessionGetOutputName(session, 0, allocator, &out_name);
    if (st) {
        g_set_error(error, SPIRO_AI_ERROR, 1, "SessionGetOutputName failed: %s",
                    ai->ort->GetErrorMessage(st));
        ai->ort->ReleaseStatus(st);
        allocator->Free(allocator, in_name);
        return false;
    }

    const int S = 192;
    float bw = box.x2 - box.x1;
    float bh = box.y2 - box.y1;
    float cx0 = (box.x1 + box.x2) * 0.5f;
    float cy0 = (box.y1 + box.y2) * 0.5f;
    float sc = (float)S / (fmaxf(bw, bh) * 1.5f);

    float *in_data = g_new(float, 3 * S * S);
    for (int py = 0; py < S; py++) {
        for (int px = 0; px < S; px++) {
            float sx = cx0 + ((float)px - (float)S * 0.5f) / sc;
            float sy = cy0 + ((float)py - (float)S * 0.5f) / sc;
            float rgb[3];
            sample_pixbuf_bilinear(input, sx, sy, rgb);
            int idx = py * S + px;
            in_data[0 * S * S + idx] = rgb[0];
            in_data[1 * S * S + idx] = rgb[1];
            in_data[2 * S * S + idx] = rgb[2];
        }
    }

    OrtMemoryInfo *mem_info = NULL;
    st = ai->ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem_info);
    if (st) {
        g_set_error(error, SPIRO_AI_ERROR, 1, "CreateCpuMemoryInfo failed: %s",
                    ai->ort->GetErrorMessage(st));
        ai->ort->ReleaseStatus(st);
        g_free(in_data);
        allocator->Free(allocator, in_name);
        allocator->Free(allocator, out_name);
        return false;
    }

    int64_t in_shape[4] = {1, 3, S, S};
    OrtValue *in_tensor = NULL;
    st = ai->ort->CreateTensorWithDataAsOrtValue(mem_info, in_data, 3 * S * S * sizeof(float),
                                                 in_shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                                                 &in_tensor);
    ai->ort->ReleaseMemoryInfo(mem_info);

    if (st) {
        g_set_error(error, SPIRO_AI_ERROR, 1, "CreateTensorWithDataAsOrtValue failed: %s",
                    ai->ort->GetErrorMessage(st));
        ai->ort->ReleaseStatus(st);
        g_free(in_data);
        allocator->Free(allocator, in_name);
        allocator->Free(allocator, out_name);
        return false;
    }

    const char *in_names[1] = {in_name};
    const char *out_names[1] = {out_name};
    OrtValue *out_tensor = NULL;

    st = ai->ort->Run(session, NULL, in_names, (const OrtValue *const *)&in_tensor, 1, out_names, 1,
                      &out_tensor);
    ai->ort->ReleaseValue(in_tensor);
    g_free(in_data);
    allocator->Free(allocator, in_name);
    allocator->Free(allocator, out_name);

    if (st) {
        g_set_error(error, SPIRO_AI_ERROR, 1, "Landmark inference failed: %s",
                    ai->ort->GetErrorMessage(st));
        ai->ort->ReleaseStatus(st);
        return false;
    }

    float *lm_data = NULL;
    if (!float_tensor(ai, out_tensor, 212, &lm_data, error)) {
        ai->ort->ReleaseValue(out_tensor);
        return false;
    }
    for (int i = 0; i < 212; i++)
        if (!isfinite(lm_data[i]) || fabsf(lm_data[i]) > 4) {
            g_set_error(error, SPIRO_AI_ERROR, 1, "Invalid face landmark output");
            ai->ort->ReleaseValue(out_tensor);
            return false;
        }

    for (int i = 0; i < 106; i++) {
        pts[i].x = cx0 + (lm_data[2 * i + 0] * 96.0f) / sc;
        pts[i].y = cy0 + (lm_data[2 * i + 1] * 96.0f) / sc;
    }

    ai->ort->ReleaseValue(out_tensor);
    return true;
}

static Point mid_point(Point a, Point b) {
    Point r = {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
    return r;
}

static void cairo_quad_to(cairo_t *cr, Point a, Point ctl, Point e) {
    double c1x = a.x + (2.0 / 3.0) * (ctl.x - a.x);
    double c1y = a.y + (2.0 / 3.0) * (ctl.y - a.y);
    double c2x = e.x + (2.0 / 3.0) * (ctl.x - e.x);
    double c2y = e.y + (2.0 / 3.0) * (ctl.y - e.y);
    cairo_curve_to(cr, c1x, c1y, c2x, c2y, e.x, e.y);
}

static void draw_smooth_curve(cairo_t *cr, const Point *pts, int n, bool closed) {
    if (n < 2)
        return;
    if (n == 2) {
        cairo_move_to(cr, pts[0].x, pts[0].y);
        cairo_line_to(cr, pts[1].x, pts[1].y);
        cairo_stroke(cr);
        return;
    }

    if (closed) {
        Point cur = mid_point(pts[n - 1], pts[0]);
        cairo_move_to(cr, cur.x, cur.y);
        for (int i = 0; i < n; i++) {
            Point e = mid_point(pts[i], pts[(i + 1) % n]);
            cairo_quad_to(cr, cur, pts[i], e);
            cur = e;
        }
        cairo_close_path(cr);
        cairo_stroke(cr);
    } else {
        Point cur = pts[0];
        cairo_move_to(cr, cur.x, cur.y);
        for (int i = 1; i < n - 1; i++) {
            Point e = mid_point(pts[i], pts[i + 1]);
            cairo_quad_to(cr, cur, pts[i], e);
            cur = e;
        }
        Point end_ctl = mid_point(cur, pts[n - 1]);
        cairo_quad_to(cr, cur, end_ctl, pts[n - 1]);
        cairo_stroke(cr);
    }
}

static Point compute_centre(const Point *pts, int n) {
    float sx = 0.0f, sy = 0.0f;
    for (int i = 0; i < n; i++) {
        sx += pts[i].x;
        sy += pts[i].y;
    }
    return (Point){sx / (float)n, sy / (float)n};
}

static int compare_angle_pt(const void *a, const void *b) {
    float ang_a = ((const AnglePoint *)a)->angle;
    float ang_b = ((const AnglePoint *)b)->angle;
    if (ang_a < ang_b)
        return -1;
    if (ang_a > ang_b)
        return 1;
    return 0;
}

static void sort_by_angle(const Point *in, int n, Point *out) {
    Point c = compute_centre(in, n);
    AnglePoint ap[64];
    int count = (n < 64) ? n : 64;
    for (int i = 0; i < count; i++) {
        ap[i].pt = in[i];
        ap[i].angle = atan2f(in[i].y - c.y, in[i].x - c.x);
    }
    qsort(ap, count, sizeof(AnglePoint), compare_angle_pt);
    for (int i = 0; i < count; i++) {
        out[i] = ap[i].pt;
    }
}

static bool in_poly(Point p, const Point *poly, int n) {
    bool ins = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        if (((poly[i].y > p.y) != (poly[j].y > p.y)) &&
            (p.x <
             (poly[j].x - poly[i].x) * (p.y - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x)) {
            ins = !ins;
        }
    }
    return ins;
}

static void draw_iris(cairo_t *cr, const Point *eye_sorted, int n_eye) {
    Point c = compute_centre(eye_sorted, n_eye);
    float min_x = eye_sorted[0].x, max_x = eye_sorted[0].x;
    float min_y = eye_sorted[0].y, max_y = eye_sorted[0].y;
    for (int i = 1; i < n_eye; i++) {
        if (eye_sorted[i].x < min_x)
            min_x = eye_sorted[i].x;
        if (eye_sorted[i].x > max_x)
            max_x = eye_sorted[i].x;
        if (eye_sorted[i].y < min_y)
            min_y = eye_sorted[i].y;
        if (eye_sorted[i].y > max_y)
            max_y = eye_sorted[i].y;
    }
    float ew = max_x - min_x;
    float eh = max_y - min_y;
    float r = fmaxf(ew * 0.18f, eh * 0.5f);
    int m = (int)ceilf(r * 2.0f * (float)G_PI / 1.2f);
    if (m < 8)
        m = 8;
    if (m > 256)
        m = 256;

    Point arc[260];
    int arc_len = 0;
    for (int i = 0; i <= m; i++) {
        float a = 2.0f * (float)G_PI * (float)i / (float)m;
        Point p = {c.x + r * cosf(a), c.y + r * sinf(a)};
        if (in_poly(p, eye_sorted, n_eye)) {
            arc[arc_len++] = p;
        } else {
            if (arc_len > 2) {
                draw_smooth_curve(cr, arc, arc_len, false);
            }
            arc_len = 0;
        }
    }
    if (arc_len > 2) {
        draw_smooth_curve(cr, arc, arc_len, false);
    }
}

static int compute_bins(const Point *pts, int n_pts, int n_bins, BinMode mode, Point *out_pts) {
    if (n_pts == 0 || n_bins <= 0)
        return 0;
    float lo = pts[0].x, hi = pts[0].x;
    for (int i = 1; i < n_pts; i++) {
        if (pts[i].x < lo)
            lo = pts[i].x;
        if (pts[i].x > hi)
            hi = pts[i].x;
    }
    float span = hi - lo;
    int count = 0;
    for (int b = 0; b < n_bins; b++) {
        float a = lo + span * (float)b / (float)n_bins;
        float c = lo + span * (float)(b + 1) / (float)n_bins;
        float sum_x = 0.0f, sum_y = 0.0f;
        float min_y = 1e30f, max_y = -1e30f;
        int in_bin = 0;
        for (int i = 0; i < n_pts; i++) {
            bool matches =
                (pts[i].x >= a) && ((b == n_bins - 1) ? (pts[i].x <= c) : (pts[i].x < c));
            if (matches) {
                sum_x += pts[i].x;
                sum_y += pts[i].y;
                if (pts[i].y < min_y)
                    min_y = pts[i].y;
                if (pts[i].y > max_y)
                    max_y = pts[i].y;
                in_bin++;
            }
        }
        if (in_bin > 0) {
            float out_x = sum_x / (float)in_bin;
            float out_y = (mode == BIN_MIN)   ? min_y
                          : (mode == BIN_MAX) ? max_y
                                              : (sum_y / (float)in_bin);
            out_pts[count++] = (Point){out_x, out_y};
        }
    }
    return count;
}

static void draw_silhouette_outline(cairo_t *cr, const float *mask, int w, int h) {
    for (int y = 0; y < h - 1; y++) {
        for (int x = 0; x < w - 1; x++) {
            bool tl = mask[y * w + x] > 0.5f;
            bool tr = mask[y * w + (x + 1)] > 0.5f;
            bool br = mask[(y + 1) * w + (x + 1)] > 0.5f;
            bool bl = mask[(y + 1) * w + x] > 0.5f;
            int cfg = (tl ? 1 : 0) | (tr ? 2 : 0) | (br ? 4 : 0) | (bl ? 8 : 0);
            if (cfg == 0 || cfg == 15)
                continue;

            double tx = (double)x + 0.5, ty = (double)y;
            double rx = (double)x + 1.0, ry = (double)y + 0.5;
            double bx = (double)x + 0.5, by = (double)y + 1.0;
            double lx = (double)x, ly = (double)y + 0.5;

            switch (cfg) {
            case 1:
            case 14:
                cairo_move_to(cr, lx, ly);
                cairo_line_to(cr, tx, ty);
                break;
            case 2:
            case 13:
                cairo_move_to(cr, tx, ty);
                cairo_line_to(cr, rx, ry);
                break;
            case 3:
            case 12:
                cairo_move_to(cr, lx, ly);
                cairo_line_to(cr, rx, ry);
                break;
            case 4:
            case 11:
                cairo_move_to(cr, rx, ry);
                cairo_line_to(cr, bx, by);
                break;
            case 5:
                cairo_move_to(cr, lx, ly);
                cairo_line_to(cr, tx, ty);
                cairo_move_to(cr, rx, ry);
                cairo_line_to(cr, bx, by);
                break;
            case 6:
            case 9:
                cairo_move_to(cr, tx, ty);
                cairo_line_to(cr, bx, by);
                break;
            case 7:
            case 8:
                cairo_move_to(cr, lx, ly);
                cairo_line_to(cr, bx, by);
                break;
            case 10:
                cairo_move_to(cr, tx, ty);
                cairo_line_to(cr, rx, ry);
                cairo_move_to(cr, lx, ly);
                cairo_line_to(cr, bx, by);
                break;
            }
        }
    }
    cairo_stroke(cr);
}

GdkPixbuf *sp_ai_apply(SpAi *ai, GdkPixbuf *input, SpAiMode mode, int *faces, GError **error) {
    if (faces)
        *faces = 0;

    if (!ai) {
        g_set_error(error, SPIRO_AI_ERROR, 1, "SpAi instance is NULL");
        return NULL;
    }
    if (!input) {
        g_set_error(error, SPIRO_AI_ERROR, 1, "Input pixbuf is NULL");
        return NULL;
    }
    if (mode != SP_AI_CUTOUT && mode != SP_AI_SILHOUETTE && mode != SP_AI_FACE &&
        mode != SP_AI_FACE_OUTLINE) {
        g_set_error(error, SPIRO_AI_ERROR, 1, "Invalid SpAiMode: %d", (int)mode);
        return NULL;
    }

    int W = gdk_pixbuf_get_width(input);
    int H = gdk_pixbuf_get_height(input);
    if (W <= 0 || H <= 0 || W > 4096 || H > 4096) {
        g_set_error(error, SPIRO_AI_ERROR, 1, "Invalid dimensions %dx%d (must be 1..4096)", W, H);
        return NULL;
    }

    int n_channels = gdk_pixbuf_get_n_channels(input);
    int bpc = gdk_pixbuf_get_bits_per_sample(input);
    if (bpc != 8 || (n_channels != 3 && n_channels != 4)) {
        g_set_error(error, SPIRO_AI_ERROR, 1, "Unsupported pixbuf format (channels=%d, bpc=%d)",
                    n_channels, bpc);
        return NULL;
    }

    if (mode == SP_AI_CUTOUT || mode == SP_AI_SILHOUETTE) {
        float *mask = compute_u2net_mask(ai, input, error);
        if (!mask)
            return NULL;

        GdkPixbuf *out = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, W, H);
        if (!out) {
            g_set_error(error, SPIRO_AI_ERROR, 1, "Failed to allocate output pixbuf");
            g_free(mask);
            return NULL;
        }

        int in_stride = gdk_pixbuf_get_rowstride(input);
        const guchar *in_px = gdk_pixbuf_get_pixels(input);

        int out_stride = gdk_pixbuf_get_rowstride(out);
        guchar *out_px = gdk_pixbuf_get_pixels(out);

        for (int y = 0; y < H; y++) {
            const guchar *s_row = in_px + y * in_stride;
            guchar *d_row = out_px + y * out_stride;
            for (int x = 0; x < W; x++) {
                int p = y * W + x;
                float m = mask[p];
                if (mode == SP_AI_SILHOUETTE) {
                    guchar c = (m > 0.5f) ? 0 : 255;
                    d_row[4 * x + 0] = c;
                    d_row[4 * x + 1] = c;
                    d_row[4 * x + 2] = c;
                    d_row[4 * x + 3] = 255;
                } else {
                    const guchar *sp = s_row + x * n_channels;
                    float src_alpha = (n_channels == 4) ? (sp[3] / 255.0f) : 1.0f;
                    float final_alpha = src_alpha * m;
                    if (final_alpha < 0.0f)
                        final_alpha = 0.0f;
                    if (final_alpha > 1.0f)
                        final_alpha = 1.0f;

                    d_row[4 * x + 0] = sp[0];
                    d_row[4 * x + 1] = sp[1];
                    d_row[4 * x + 2] = sp[2];
                    d_row[4 * x + 3] = (guchar)roundf(final_alpha * 255.0f);
                }
            }
        }
        g_free(mask);
        return out;
    }

    /* Face modes: SP_AI_FACE and SP_AI_FACE_OUTLINE */
    int num_detected = 0;
    FaceBox *boxes = detect_faces(ai, input, &num_detected, error);
    if (!boxes || num_detected == 0) {
        if (boxes)
            g_free(boxes);
        if (error && *error == NULL) {
            g_set_error(error, SPIRO_AI_ERROR, 2, "No faces detected in input image");
        }
        return NULL;
    }

    if (faces)
        *faces = num_detected;

    float *outline_mask = NULL;
    if (mode == SP_AI_FACE_OUTLINE) {
        outline_mask = compute_u2net_mask(ai, input, error);
        if (!outline_mask) {
            g_free(boxes);
            return NULL;
        }
    }

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        g_set_error(error, SPIRO_AI_ERROR, 1, "Failed to create Cairo surface (%dx%d)", W, H);
        if (outline_mask)
            g_free(outline_mask);
        g_free(boxes);
        cairo_surface_destroy(surface);
        return NULL;
    }

    cairo_t *cr = cairo_create(surface);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);

    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    cairo_set_line_width(cr, 1.5);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    if (mode == SP_AI_FACE_OUTLINE && outline_mask) {
        draw_silhouette_outline(cr, outline_mask, W, H);
        g_free(outline_mask);
        outline_mask = NULL;
    }

    for (int b = 0; b < num_detected; b++) {
        Point pts[106];
        if (!extract_landmarks(ai, input, boxes[b], pts, error)) {
            cairo_destroy(cr);
            cairo_surface_destroy(surface);
            g_free(boxes);
            return NULL;
        }

        if (mode == SP_AI_FACE) {
            Point jaw[33];
            for (int i = 0; i < 33; i++)
                jaw[i] = pts[JAW_INDICES[i]];
            draw_smooth_curve(cr, jaw, 33, false);
        }

        Point brow_l[9], brow_r[9];
        for (int i = 0; i < 9; i++) {
            brow_l[i] = pts[43 + i];
            brow_r[i] = pts[97 + i];
        }
        Point bl_bins[6], br_bins[6];
        int n_bl = compute_bins(brow_l, 9, 6, BIN_MID, bl_bins);
        int n_br = compute_bins(brow_r, 9, 6, BIN_MID, br_bins);
        draw_smooth_curve(cr, bl_bins, n_bl, false);
        draw_smooth_curve(cr, br_bins, n_br, false);

        Point eye_l[10], eye_r[10];
        for (int i = 0; i < 10; i++) {
            eye_l[i] = pts[33 + i];
            eye_r[i] = pts[87 + i];
        }
        Point eye_l_sorted[10], eye_r_sorted[10];
        sort_by_angle(eye_l, 10, eye_l_sorted);
        sort_by_angle(eye_r, 10, eye_r_sorted);

        draw_smooth_curve(cr, eye_l_sorted, 10, true);
        draw_smooth_curve(cr, eye_r_sorted, 10, true);

        draw_iris(cr, eye_l_sorted, 10);
        draw_iris(cr, eye_r_sorted, 10);

        int nose_idx[9] = {76, 77, 78, 79, 80, 85, 84, 83, 82};
        Point nose_pts[9];
        for (int i = 0; i < 9; i++)
            nose_pts[i] = pts[nose_idx[i]];
        draw_smooth_curve(cr, nose_pts, 9, false);

        cairo_new_sub_path(cr);
        cairo_arc(cr, pts[79].x, pts[79].y, 2.2, 0, 2.0 * G_PI);
        cairo_stroke(cr);

        cairo_new_sub_path(cr);
        cairo_arc(cr, pts[84].x, pts[84].y, 2.2, 0, 2.0 * G_PI);
        cairo_stroke(cr);

        Point mouth[20];
        for (int i = 0; i < 20; i++)
            mouth[i] = pts[52 + i];
        Point cl = mouth[0], cr_pt = mouth[0];
        for (int i = 1; i < 20; i++) {
            if (mouth[i].x < cl.x)
                cl = mouth[i];
            if (mouth[i].x > cr_pt.x)
                cr_pt = mouth[i];
        }
        float an = atan2f(cr_pt.y - cl.y, cr_pt.x - cl.x);
        Point mc = compute_centre(mouth, 20);

        Point flat[20];
        float cos_neg = cosf(-an), sin_neg = sinf(-an);
        for (int i = 0; i < 20; i++) {
            float dx = mouth[i].x - mc.x, dy = mouth[i].y - mc.y;
            flat[i].x = mc.x + dx * cos_neg - dy * sin_neg;
            flat[i].y = mc.y + dx * sin_neg + dy * cos_neg;
        }

        Point top_flat[6], bot_flat[6], mid_flat[6];
        int n_top = compute_bins(flat, 20, 6, BIN_MIN, top_flat);
        int n_bot = compute_bins(flat, 20, 6, BIN_MAX, bot_flat);
        int n_mid = compute_bins(flat, 20, 6, BIN_MID, mid_flat);

        float cos_pos = cosf(an), sin_pos = sinf(an);
        Point top[6], bot[6], mid[6];
        for (int i = 0; i < n_top; i++) {
            float dx = top_flat[i].x - mc.x, dy = top_flat[i].y - mc.y;
            top[i].x = mc.x + dx * cos_pos - dy * sin_pos;
            top[i].y = mc.y + dx * sin_pos + dy * cos_pos;
        }
        for (int i = 0; i < n_bot; i++) {
            float dx = bot_flat[i].x - mc.x, dy = bot_flat[i].y - mc.y;
            bot[i].x = mc.x + dx * cos_pos - dy * sin_pos;
            bot[i].y = mc.y + dx * sin_pos + dy * cos_pos;
        }
        for (int i = 0; i < n_mid; i++) {
            float dx = mid_flat[i].x - mc.x, dy = mid_flat[i].y - mc.y;
            mid[i].x = mc.x + dx * cos_pos - dy * sin_pos;
            mid[i].y = mc.y + dx * sin_pos + dy * cos_pos;
        }

        Point lip_loop[12];
        int loop_len = 0;
        for (int i = 0; i < n_top; i++)
            lip_loop[loop_len++] = top[i];
        for (int i = n_bot - 1; i >= 0; i--)
            lip_loop[loop_len++] = bot[i];
        draw_smooth_curve(cr, lip_loop, loop_len, true);

        draw_smooth_curve(cr, mid, n_mid, false);
    }
    g_free(boxes);

    cairo_surface_flush(surface);
    const unsigned char *cairo_data = cairo_image_surface_get_data(surface);
    int cairo_stride = cairo_image_surface_get_stride(surface);

    GdkPixbuf *out = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, W, H);
    if (!out) {
        g_set_error(error, SPIRO_AI_ERROR, 1, "Failed to allocate final output pixbuf");
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        return NULL;
    }

    int out_stride = gdk_pixbuf_get_rowstride(out);
    guchar *out_pixels = gdk_pixbuf_get_pixels(out);

    for (int y = 0; y < H; y++) {
        const uint32_t *src_row = (const uint32_t *)(cairo_data + y * cairo_stride);
        guchar *dst_row = out_pixels + y * out_stride;
        for (int x = 0; x < W; x++) {
            uint32_t px = src_row[x];
            uint8_t r = (px >> 16) & 0xFF;
            uint8_t g = (px >> 8) & 0xFF;
            uint8_t b = px & 0xFF;
            dst_row[4 * x + 0] = r;
            dst_row[4 * x + 1] = g;
            dst_row[4 * x + 2] = b;
            dst_row[4 * x + 3] = 255;
        }
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    return out;
}
