/** @file cai/utf8.h Strict incremental Unicode validation shared by transports.
 */
#ifndef CAI_PUBLIC_UTF8_H
#define CAI_PUBLIC_UTF8_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/** Zero initialization starts a new stream. No input bytes are retained. */
typedef struct cai_utf8_state {
  uint32_t scalar, minimum;
  unsigned int pending;
  int failed;
} cai_utf8_state;
/** Return one for a valid prefix, zero for an invalid stream. Invalid state is
 * sticky. NUL is valid Unicode; text protocols reject it separately. */
int cai_utf8_feed(cai_utf8_state *, const void *, size_t);
/** Return one only for a valid stream ending on a Unicode scalar boundary. */
int cai_utf8_complete(const cai_utf8_state *);
#ifdef __cplusplus
}
#endif
#endif
