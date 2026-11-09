/* Included only by glibc Debug test builds. The allocator's archive member
 * carries this constructor into embedding hosts too; executable startup also
 * covers standalone fixtures. Reapplying the same defaults is harmless. */
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

/** Set future pthread-owned stacks; do not intercept explicit attributes or
 * change the process/main-thread stack limit. Keep existing nonzero guards. */
static void __attribute__((constructor)) cai_test_thread_defaults(void) {
  pthread_attr_t attributes;
  size_t guard = 0U;
  long page;
  int rc = pthread_getattr_default_np(&attributes);
  if (rc == 0) {
    rc = pthread_attr_getguardsize(&attributes, &guard);
    if (rc == 0 && guard == 0U) {
      page = sysconf(_SC_PAGESIZE);
      rc = page > 0 ? pthread_attr_setguardsize(&attributes, (size_t)page) : -1;
    }
    if (rc == 0)
      rc = pthread_attr_setstacksize(&attributes, 128U * 1024U);
    if (rc == 0)
      rc = pthread_setattr_default_np(&attributes);
    (void)pthread_attr_destroy(&attributes);
  }
  if (rc != 0) {
    fprintf(stderr, "cai: initialize Debug test worker stacks (code %d)\n", rc);
    _exit(127);
  }
}
