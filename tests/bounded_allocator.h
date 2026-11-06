#ifndef CAI_TEST_BOUNDED_ALLOCATOR_H
#define CAI_TEST_BOUNDED_ALLOCATOR_H
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int bounded;
static size_t allocation_attempts;
static size_t allocation_fail_at = SIZE_MAX;
/* Static musl calloc/realloc can call the wrapped malloc/free internally.
 * Count each public allocation once, like the dynamically linked native case.
 */
static int allocation_depth;
static struct allocation {
  void *pointer;
  size_t size;
} allocations[1024];
static size_t live_bytes, peak_bytes;
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void __real_free(void *);
void *__wrap_malloc(size_t);
void *__wrap_calloc(size_t, size_t);
void *__wrap_realloc(void *, size_t);
void __wrap_free(void *);
static size_t allocation_at(void *pointer) {
  size_t i;
  if (pointer != NULL)
    for (i = 0U; i < 1024U; i++)
      if (allocations[i].pointer == pointer)
        return i;
  return 1024U;
}
static void remember(void *pointer, size_t size) {
  size_t i;
  if (pointer == NULL || !bounded)
    return;
  for (i = 0U; i < 1024U && allocations[i].pointer != NULL; i++) {
  }
  assert(i != 1024U);
  allocations[i].pointer = pointer;
  allocations[i].size = size;
  live_bytes += size;
  if (peak_bytes < live_bytes)
    peak_bytes = live_bytes;
}
void *__wrap_malloc(size_t n) {
  void *pointer;
  if (allocation_depth)
    return __real_malloc(n);
  if (bounded && allocation_attempts++ == allocation_fail_at)
    return NULL;
  if (bounded && (n > 65536U || n > 1048576U - live_bytes))
    return NULL;
  allocation_depth++;
  pointer = __real_malloc(n);
  allocation_depth--;
  remember(pointer, n);
  return pointer;
}
void *__wrap_calloc(size_t n, size_t width) {
  void *pointer;
  if (allocation_depth)
    return __real_calloc(n, width);
  if (bounded && allocation_attempts++ == allocation_fail_at)
    return NULL;
  if (bounded && width != 0U &&
      (n > 65536U / width || n > (1048576U - live_bytes) / width))
    return NULL;
  allocation_depth++;
  pointer = __real_calloc(n, width);
  allocation_depth--;
  remember(pointer, n * width);
  return pointer;
}
void *__wrap_realloc(void *p, size_t n) {
  size_t at = allocation_at(p), old = at == 1024U ? 0U : allocations[at].size;
  void *pointer;
  if (allocation_depth)
    return __real_realloc(p, n);
  if (bounded && allocation_attempts++ == allocation_fail_at)
    return NULL;
  if (bounded && (n > 65536U || n > 1048576U - (live_bytes - old)))
    return NULL;
  allocation_depth++;
  pointer = __real_realloc(p, n);
  allocation_depth--;
  if (pointer != NULL || n == 0U) {
    if (at != 1024U) {
      live_bytes -= old;
      allocations[at].pointer = NULL;
    }
    remember(pointer, n);
  }
  return pointer;
}
void __wrap_free(void *pointer) {
  size_t at = allocation_at(pointer);
  if (allocation_depth) {
    __real_free(pointer);
    return;
  }
  if (at != 1024U) {
    live_bytes -= allocations[at].size;
    allocations[at].pointer = NULL;
  }
  allocation_depth++;
  __real_free(pointer);
  allocation_depth--;
}
#endif
