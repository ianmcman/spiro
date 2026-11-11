#include "spiro3d_io.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
int main(void) {
    Sp3Config c = sp3_defaults(), loaded = c;
    Sp3View v = sp3_default_view(), restored = v;
    v.yaw = 1.7;
    v.zoom = 1.3;
    c.tube = 1;
    c.show_arms = 1;
    GError *e = NULL;
    assert(sp3_save("/tmp/spirolab-3d-test.spiro3d", &c, &v, &e));
    assert(sp3_load("/tmp/spirolab-3d-test.spiro3d", &loaded, &restored, &e));
    assert(loaded.arm_count == 3 && loaded.tube && loaded.show_arms && loaded.arms[2].speed == -9 &&
           restored.yaw == v.yaw && restored.zoom == v.zoom);
    GKeyFile *k = g_key_file_new();
    assert(g_key_file_load_from_file(k, "/tmp/spirolab-3d-test.spiro3d", 0, &e));
    g_key_file_set_integer(k, "Arm0", "speed", 999);
    assert(g_key_file_save_to_file(k, "/tmp/spirolab-3d-bad.spiro3d", &e));
    g_key_file_free(k);
    assert(!sp3_load("/tmp/spirolab-3d-bad.spiro3d", &loaded, &restored, &e));
    assert(e && loaded.arms[0].speed == c.arms[0].speed && restored.zoom == v.zoom);
    g_clear_error(&e);
    v.zoom = NAN;
    assert(!sp3_save("/tmp/spirolab-3d-invalid.spiro3d", &c, &v, &e));
    g_clear_error(&e);
    v = sp3_default_view();
    Sp3Curve curve = {0};
    assert(sp3_curve(&c, &curve));
    assert(sp3_png("/tmp/spirolab-3d-tube.png", &c, &curve, &v, &e));
    assert(!sp3_png("/dev/full", &c, &curve, &v, &e));
    assert(e);
    g_clear_error(&e);
    assert(sp3_export_stl("/tmp/spirolab-3d-test.stl", &c, &e));
    assert(!sp3_export_stl("/tmp/spirolab-no-directory/out.stl", &c, &e));
    g_clear_error(&e);
    sp3_curve_free(&curve);
    puts("3D saved camera/arms roundtrip, corrupt-load preservation, PNG/STL export and write "
         "errors passed");
}
