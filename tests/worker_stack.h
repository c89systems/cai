#ifndef CAI_TEST_WORKER_STACK_H
#define CAI_TEST_WORKER_STACK_H
#include <pthread.h>

/** Exercise musl's actual default; emulate its budget on native glibc. */
static int test_worker_start(pthread_t *thread, void *(*run)(void *),
                             void *context) {
  int rc;
#ifdef __GLIBC__
  pthread_attr_t attributes;
  if (pthread_attr_init(&attributes) != 0)
    return -1;
  rc = pthread_attr_setstacksize(&attributes, 128U * 1024U);
  if (rc == 0)
    rc = pthread_create(thread, &attributes, run, context);
  pthread_attr_destroy(&attributes);
#else
  rc = pthread_create(thread, NULL, run, context);
#endif
  return rc;
}
#endif
