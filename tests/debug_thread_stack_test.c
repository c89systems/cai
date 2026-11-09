#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef CAI_STACK_EMBEDDED
#include <cai/cai.h>
#endif
#ifdef CAI_STACK_FILTERED
#include "../src/cai_process_environment.h"

static void check_child_environment(void) {
  static const char *const entries[] = {"HOME=/test", "PRIVATE=value",
                                        "GLIBC_TUNABLES=glibc.malloc.arena_max="
                                        "2:glibc.pthread.stack_cache_size=8192",
                                        NULL};
  cai_environment *view = NULL;
  char **copy;
  size_t i;
  int yolo;
  assert(cai_environment_open(entries, &view, NULL) == CAI_OK);
  for (yolo = 0; yolo <= 1; yolo++) {
    assert(cai_process_environment_new(view, yolo, NULL, &copy, NULL) ==
           CAI_OK);
    for (i = 0U; copy[i] != NULL; i++) {
      if (strncmp(copy[i], "GLIBC_TUNABLES=", 15U) == 0)
        break;
      assert(yolo || strcmp(copy[i], "HOME=/test") == 0);
    }
    assert(copy[i] != NULL);
    assert(strcmp(copy[i],
                  yolo ? "GLIBC_TUNABLES=glibc.malloc.arena_max=2:glibc."
                         "pthread.stack_cache_size=0"
                       : "GLIBC_TUNABLES=glibc.pthread.stack_cache_size=0") ==
           0);
    cai_process_environment_free(copy);
  }
  cai_environment_close(view);
}
#endif

#define SMALL_STACK (128U * 1024U)
#ifndef CAI_STACK_CONTROL
typedef struct worker_check {
  size_t size, guard;
  int nested;
} worker_check;

/** Fault the actual pthread guard in a disposable child. This also handles
 * lightweight kernel guards whose mapping permissions look writable. */
static void __attribute__((no_sanitize_address, no_sanitize_undefined))
guard_fault(void *base) {
  pid_t child = fork();
  int status;
  assert(child >= 0);
  if (child == 0) {
    struct rlimit limit;
    limit.rlim_cur = limit.rlim_max = 0;
    (void)setrlimit(RLIMIT_CORE, &limit);
    /* Piped core handlers can ignore RLIMIT_CORE and stall this probe. */
    if (prctl(PR_SET_DUMPABLE, 0) != 0)
      _exit(125);
    (void)signal(SIGSEGV, SIG_DFL);
    (void)signal(SIGBUS, SIG_DFL);
    *((volatile unsigned char *)base - 1) = 1U;
    _exit(0);
  }
  assert(waitpid(child, &status, 0) == child);
  assert(WIFSIGNALED(status));
  assert(WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS);
}

static void *inspect_worker(void *context) {
  worker_check *check = context;
  pthread_attr_t attributes;
  pthread_t nested;
  worker_check next;
  void *base;
  size_t size, guard;
  assert(pthread_getattr_np(pthread_self(), &attributes) == 0);
  assert(pthread_attr_getstack(&attributes, &base, &size) == 0);
  assert(pthread_attr_getguardsize(&attributes, &guard) == 0);
  assert(pthread_attr_destroy(&attributes) == 0);
  assert(size == check->size && guard >= check->guard);
  guard_fault(base);
  if (check->nested) {
    next.size = SMALL_STACK;
    next.guard = (size_t)sysconf(_SC_PAGESIZE);
    next.nested = 0;
    assert(pthread_create(&nested, NULL, inspect_worker, &next) == 0);
    assert(pthread_join(nested, NULL) == 0);
  }
  return NULL;
}

static void exercise_workers(size_t page) {
  pthread_t worker;
  pthread_attr_t attributes;
  worker_check check;
  /* A larger completed worker must not supply a cached stack to the next
   * default worker. Its explicit stack and guard requests must survive. */
  check.size = 1024U * 1024U;
  check.guard = 2U * page;
  check.nested = 0;
  assert(pthread_attr_init(&attributes) == 0);
  assert(pthread_attr_setstacksize(&attributes, check.size) == 0);
  assert(pthread_attr_setguardsize(&attributes, check.guard) == 0);
  assert(pthread_create(&worker, &attributes, inspect_worker, &check) == 0);
  assert(pthread_join(worker, NULL) == 0);
  assert(pthread_attr_destroy(&attributes) == 0);
  check.size = SMALL_STACK;
  check.guard = page;
  check.nested = 1;
  assert(pthread_create(&worker, NULL, inspect_worker, &check) == 0);
  assert(pthread_join(worker, NULL) == 0);
  /* Initialized attributes with no explicit stack size use the new default. */
  assert(pthread_attr_init(&attributes) == 0);
  assert(pthread_create(&worker, &attributes, inspect_worker, &check) == 0);
  assert(pthread_join(worker, NULL) == 0);
  assert(pthread_attr_destroy(&attributes) == 0);
}
#endif

int main(int argc, char **argv) {
  pthread_attr_t attributes;
  struct rlimit before, after;
  void *base;
  size_t size, guard, main_size;
  long page = sysconf(_SC_PAGESIZE);
  pid_t child;
  int status;
  char current[32], maximum[32];
#ifdef CAI_STACK_FILTERED
  char **environment = NULL;
#endif
  assert(page > 0 && getrlimit(RLIMIT_STACK, &before) == 0);
  if (argc == 3) {
    assert((unsigned long long)before.rlim_cur == strtoull(argv[1], NULL, 10));
    assert((unsigned long long)before.rlim_max == strtoull(argv[2], NULL, 10));
  } else
    assert(argc == 1);
  assert(pthread_getattr_default_np(&attributes) == 0);
  assert(pthread_attr_getstacksize(&attributes, &size) == 0);
  assert(pthread_attr_getguardsize(&attributes, &guard) == 0);
  assert(pthread_attr_destroy(&attributes) == 0);
  assert(pthread_getattr_np(pthread_self(), &attributes) == 0);
  assert(pthread_attr_getstack(&attributes, &base, &main_size) == 0);
  assert(pthread_attr_destroy(&attributes) == 0);
  assert(main_size > SMALL_STACK && guard >= (size_t)page);
#ifdef CAI_STACK_CONTROL
  assert(size > SMALL_STACK);
#else
  assert(size == SMALL_STACK);
  assert(getenv("GLIBC_TUNABLES") != NULL);
  assert(strstr(getenv("GLIBC_TUNABLES"), "glibc.pthread.stack_cache_size=0") !=
         NULL);
#ifdef CAI_STACK_EMBEDDED
  {
    cai_sink *sink = NULL;
    /* Link the allocator's archive member, then check its constructor already
     * ran before any API call. Shared hosts exercise the same boundary. */
    assert(cai_sink_file(stdout, 0, &sink, NULL) == CAI_OK);
    cai_sink_close(sink);
  }
#endif
  exercise_workers((size_t)page);
#endif
  assert(getrlimit(RLIMIT_STACK, &after) == 0);
  assert(before.rlim_cur == after.rlim_cur &&
         before.rlim_max == after.rlim_max);
  if (argc == 1) {
#ifdef CAI_STACK_FILTERED
    check_child_environment();
    /* This embedding child takes the same restricted environment path as
     * terminal/plugin subprocesses, which normally discard loader tunables. */
    assert(cai_process_environment_new(NULL, 0, NULL, &environment, NULL) ==
           CAI_OK);
#endif
    snprintf(current, sizeof(current), "%llu",
             (unsigned long long)before.rlim_cur);
    snprintf(maximum, sizeof(maximum), "%llu",
             (unsigned long long)before.rlim_max);
    child = fork();
    assert(child >= 0);
    if (child == 0) {
#ifdef CAI_STACK_FILTERED
      char *arguments[4];
      arguments[0] = argv[0];
      arguments[1] = current;
      arguments[2] = maximum;
      arguments[3] = NULL;
      execve(argv[0], arguments, environment);
#else
      execl(argv[0], argv[0], current, maximum, (char *)NULL);
#endif
      _exit(127);
    }
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
#ifdef CAI_STACK_FILTERED
    cai_process_environment_free(environment);
#endif
  }
  printf("default=%lu guard=%lu main=%lu\n", (unsigned long)size,
         (unsigned long)guard, (unsigned long)main_size);
  return 0;
}
