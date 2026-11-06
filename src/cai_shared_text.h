/** @file Immutable owned text shared by a runtime and detached readers. */
#ifndef CAI_SHARED_TEXT_H
#define CAI_SHARED_TEXT_H
#include <cai/cai.h>
#include <pthread.h>
typedef struct cai_shared_text {
  cai_allocator allocator;
  pthread_mutex_t lock;
  size_t references, length;
  char *data;
} cai_shared_text;
/** Adopt an immutable allocation on success only. NULL text is absent. */
int cai_shared_text_adopt(const cai_allocator *, char *, cai_shared_text **,
                          cai_error *);
/** Copy an initial caller value; subsequent readers retain this allocation. */
int cai_shared_text_create(const cai_allocator *, const char *,
                           cai_shared_text **, cai_error *);
/** The caller must already own a reference. NULL is harmless. */
void cai_shared_text_retain(cai_shared_text *);
void cai_shared_text_release(cai_shared_text *);
#endif
