#ifndef SPIRO_AI_H
#define SPIRO_AI_H
#include <gdk-pixbuf/gdk-pixbuf.h>
typedef struct SpAi SpAi;
typedef enum { SP_AI_CUTOUT, SP_AI_SILHOUETTE, SP_AI_FACE, SP_AI_FACE_OUTLINE } SpAiMode;
SpAi *sp_ai_new(const char *model_dir, GError **error);
/* Output newly owned; face modes return clean black strokes on white.
 * CUTOUT alpha encodes subject, SILHOUETTE black subject on white. */
GdkPixbuf *sp_ai_apply(SpAi *ai, GdkPixbuf *input, SpAiMode mode, int *faces, GError **error);
void sp_ai_free(SpAi *ai);
#endif
