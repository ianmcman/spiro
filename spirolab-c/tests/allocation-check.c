#include "spiro.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static int attempt, fail_at;
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void *__wrap_malloc(size_t n) {
    return ++attempt == fail_at ? NULL : __real_malloc(n);
}
void *__wrap_calloc(size_t n, size_t s) {
    return ++attempt == fail_at ? NULL : __real_calloc(n, s);
}
void *__wrap_realloc(void *p, size_t n) {
    return ++attempt == fail_at ? NULL : __real_realloc(p, n);
}
int main(void) {
    SpParams p = sp_defaults();
    p.ring_shape = 9;
    for (int i = 1; i <= 100; i++) {
        attempt = 0;
        fail_at = i;
        SpPath q = {0};
        int ok = sp_generate(&p, 1000, &q);
        fail_at = 0;
        if (ok)
            assert(q.points && q.count >= 2);
        else
            assert(!q.points && !q.count);
        sp_path_free(&q);
    }
    puts("geometry handles each injected allocation failure without corrupt output");
}
