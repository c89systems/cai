#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <cai/environment.h>
#include <libmdf/mdf.h>
#include <signal.h>
#include <softline/softline.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int handing_off;
static cai_environment *saved_environment;
static char *saved_directory;
mdf_status __real_mdf_pager_file(const char *, const mdf_options *,
                                 mdf_pager_format, const mdf_pager_control *);
mdf_status __wrap_mdf_pager_file(const char *, const mdf_options *,
                                 mdf_pager_format, const mdf_pager_control *);
mdf_status __real_mdf_pager_seekable_source(const char *, mdf_seekable_source *,
                                            const mdf_options *,
                                            mdf_pager_format,
                                            const mdf_pager_control *,
                                            const mdf_pager_options *);
mdf_status __wrap_mdf_pager_seekable_source(const char *, mdf_seekable_source *,
                                            const mdf_options *,
                                            mdf_pager_format,
                                            const mdf_pager_control *,
                                            const mdf_pager_options *);
int __real_sl_terminal_handoff(sl_t *, sl_terminal_callback_t, void *);
int __wrap_sl_terminal_handoff(sl_t *, sl_terminal_callback_t, void *);
int __real_sl_queue_backend_update(sl_t *, const sl_queue_change_t *);
int __wrap_sl_queue_backend_update(sl_t *, const sl_queue_change_t *);
int __real_sl_history_add(sl_t *, const char *);
int __wrap_sl_history_add(sl_t *, const char *);
int __real_sl_set_status_message(sl_t *, const char *);
int __wrap_sl_set_status_message(sl_t *, const char *);
static void guard_begin(void) {
  assert(!handing_off);
  saved_directory = getcwd(NULL, 0U);
  assert(saved_directory != NULL);
  assert(cai_environment_open(NULL, &saved_environment, NULL) == CAI_OK);
  handing_off = 1;
}
static void guard_end(void) {
  cai_environment *after;
  const char *const *a, *const *b;
  char *restored;
  size_t i;
  handing_off = 0;
  restored = getcwd(NULL, 0U);
  assert(restored != NULL && strcmp(saved_directory, restored) == 0);
  assert(cai_environment_open(NULL, &after, NULL) == CAI_OK);
  assert(cai_environment_count(saved_environment) ==
         cai_environment_count(after));
  a = cai_environment_entries(saved_environment);
  b = cai_environment_entries(after);
  for (i = 0U; a[i] != NULL; ++i)
    assert(strcmp(a[i], b[i]) == 0);
  cai_environment_close(saved_environment);
  cai_environment_close(after);
  free(saved_directory);
  free(restored);
}
int __wrap_sl_terminal_handoff(sl_t *sl, sl_terminal_callback_t callback,
                               void *context) {
  int rc;
  guard_begin();
  rc = __real_sl_terminal_handoff(sl, callback, context);
  guard_end();
  return rc;
}
mdf_status __wrap_mdf_pager_file(const char *path, const mdf_options *options,
                                 mdf_pager_format format,
                                 const mdf_pager_control *control) {
  static const int signals[] = {SIGCHLD, SIGINT, SIGTERM,
                                SIGQUIT, SIGHUP, SIGWINCH};
  struct sigaction before[6], after;
  size_t i;
  int signal_number;
  unsigned int flags = SA_RESTART | SA_SIGINFO | SA_NODEFER | SA_RESETHAND |
                       SA_NOCLDSTOP | SA_NOCLDWAIT;
  mdf_status rc;
  for (i = 0U; i < sizeof(signals) / sizeof(signals[0]); ++i)
    assert(sigaction(signals[i], NULL, &before[i]) == 0);
  guard_begin();
  rc = __real_mdf_pager_file(path, options, format, control);
  guard_end();
  for (i = 0U; i < sizeof(signals) / sizeof(signals[0]); ++i) {
    assert(sigaction(signals[i], NULL, &after) == 0);
    assert(before[i].sa_handler == after.sa_handler &&
           ((unsigned int)before[i].sa_flags & flags) ==
               ((unsigned int)after.sa_flags & flags));
    for (signal_number = 1; signal_number <= 64; ++signal_number)
      assert(sigismember(&before[i].sa_mask, signal_number) ==
             sigismember(&after.sa_mask, signal_number));
  }
  return rc;
}
mdf_status __wrap_mdf_pager_seekable_source(
    const char *name, mdf_seekable_source *source, const mdf_options *options,
    mdf_pager_format format, const mdf_pager_control *control,
    const mdf_pager_options *pager_options) {
  mdf_status rc;
  guard_begin();
  assert(source->seek != NULL);
  rc = __real_mdf_pager_seekable_source(name, source, options, format, control,
                                        pager_options);
  guard_end();
  return rc;
}
int __wrap_sl_queue_backend_update(sl_t *sl, const sl_queue_change_t *change) {
  assert(!handing_off);
  return __real_sl_queue_backend_update(sl, change);
}
int __wrap_sl_history_add(sl_t *sl, const char *text) {
  assert(!handing_off);
  return __real_sl_history_add(sl, text);
}
int __wrap_sl_set_status_message(sl_t *sl, const char *text) {
  assert(!handing_off);
  return __real_sl_set_status_message(sl, text);
}
