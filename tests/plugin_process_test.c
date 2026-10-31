#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include <assert.h>
#include <cai/plugins.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define PAYLOAD (2U * 1024U * 1024U)
typedef struct transfer {
  size_t read, written;
  int cancellation, fail_sink;
} transfer;
static int child(const char *mode) {
  unsigned char bytes[2048];
  ssize_t n;
  size_t i;
  memset(bytes, 'a', sizeof(bytes));
  if (strcmp(mode, "wait") == 0) {
    for (;;)
      pause();
  }
  if (strcmp(mode, "exit") == 0) {
    close(0);
    fputs("EXPECTED FAILURE", stderr);
    return 7;
  }
  assert(write(1, "ACK", 3) == 3);
  for (i = 0U; i < 64U; i++)
    assert(write(2, bytes, sizeof(bytes)) == (ssize_t)sizeof(bytes));
  while ((n = read(0, bytes, sizeof(bytes))) > 0)
    assert(write(1, bytes, (size_t)n) == n);
  return n == 0 ? 0 : 1;
}
static size_t produce(void *context, void *bytes, size_t capacity,
                      cai_error *error) {
  transfer *t = context;
  size_t size = PAYLOAD - t->read;
  (void)error;
  if (size > capacity)
    size = capacity;
  memset(bytes, 'a', size);
  if (t->read == 0U && size != 0U)
    ((char *)bytes)[0] = '"';
  t->read += size;
  if (t->read == PAYLOAD && size != 0U)
    ((char *)bytes)[size - 1U] = '"';
  return size;
}
static int consume(void *context, const void *bytes, size_t size,
                   cai_error *error) {
  transfer *t = context;
  (void)bytes;
  (void)error;
  if (t->fail_sink)
    return CAI_ERR_TRANSPORT;
  if (t->written == 0U)
    assert(t->read < PAYLOAD); /* Real output before input EOF. */
  t->written += size;
  return CAI_OK;
}
static int cancelled(void *context) {
  transfer *t = context;
  return ++t->cancellation > 2;
}
static int descriptors(void) {
  DIR *dir = opendir("/proc/self/fd");
  int n = 0;
  if (dir == NULL)
    return 0;
  while (readdir(dir) != NULL)
    n++;
  closedir(dir);
  return n;
}
static int invoke(cai_plugins *p, cai_plugin_call *call, transfer *t) {
  cai_source_callbacks in;
  cai_sink_callbacks out;
  cai_source *source = NULL;
  cai_sink *sink = NULL;
  cai_error error;
  int rc;
  memset(&in, 0, sizeof(in));
  memset(&out, 0, sizeof(out));
  in.context = t;
  in.read = produce;
  out.context = t;
  out.write = consume;
  cai_error_init(&error);
  assert(cai_source_from_callbacks(&in, &source, &error) == CAI_OK);
  assert(cai_sink_from_callbacks(&out, &sink, &error) == CAI_OK);
  rc = cai_plugins_run(p, "process_test", source, sink, call, &error);
  if (rc != CAI_OK && error.detail != NULL)
    assert(strstr(error.detail, "EXPECTED FAILURE") != NULL);
  cai_source_close(source);
  cai_sink_close(sink);
  cai_error_cleanup(&error);
  return rc;
}
static void package(const char *manifest, const char *executable,
                    const char *mode) {
  FILE *f = fopen(manifest, "wb");
  assert(f != NULL);
  assert(fprintf(f,
                 "{\"name\":\"process_test\",\"description\":\"Process "
                 "fixture\",\"parameters\":{\"type\":\"object\"},\"executor\":"
                 "\"process\",\"argv\":[\"%s\",\"--child\",\"%s\"]}",
                 executable, mode) > 0);
  assert(fclose(f) == 0);
}
typedef struct worker {
  const char *build, *binary;
} worker;
static void *exercise(void *context) {
  worker *w = context;
  struct paths {
    char root[4096], manifest[4096], link[4096];
  } *paths = calloc(1U, sizeof(*paths));
  cai_plugins *p = NULL;
  cai_plugins_config config;
  cai_plugin_call call;
  cai_error error;
  const char *roots[1];
  transfer t;
  int before = descriptors();
  assert(paths != NULL);
  assert(snprintf(paths->root, sizeof(paths->root), "%s/process-XXXXXX",
                  w->build) < (int)sizeof(paths->root));
  assert(mkdtemp(paths->root) != NULL);
  assert(snprintf(paths->manifest, sizeof(paths->manifest), "%s/tool.json",
                  paths->root) < (int)sizeof(paths->manifest));
  cai_error_init(&error);
  cai_plugins_config_init(&config);
  cai_plugin_call_init(&call);
  roots[0] = paths->root;
  config.directories = roots;
  config.directory_count = 1U;
  call.workspace = paths->root;
  assert(cai_plugins_open(&config, &p, &error) == CAI_OK);
  package(paths->manifest, w->binary, "echo");
  assert(cai_plugins_load(p, paths->root, &error) == CAI_OK);
  memset(&t, 0, sizeof(t));
  assert(invoke(p, &call, &t) == CAI_OK && t.read == PAYLOAD &&
         t.written == PAYLOAD + 3U);
  call.max_output_bytes = 1U;
  memset(&t, 0, sizeof(t));
  assert(invoke(p, &call, &t) == CAI_ERR_LIMIT);
  call.max_output_bytes = 0U;
  memset(&t, 0, sizeof(t));
  t.fail_sink = 1;
  assert(invoke(p, &call, &t) == CAI_ERR_TRANSPORT);
  package(paths->manifest, w->binary, "exit");
  assert(cai_plugins_load(p, paths->root, &error) == CAI_OK);
  memset(&t, 0, sizeof(t));
  assert(invoke(p, &call, &t) == CAI_ERR_INVALID);
  package(paths->manifest, w->binary, "wait");
  assert(cai_plugins_load(p, paths->root, &error) == CAI_OK);
  call.timeout_ms = 2L;
  memset(&t, 0, sizeof(t));
  assert(invoke(p, &call, &t) == CAI_ERR_LIMIT);
  call.timeout_ms = 120000L;
  call.cancelled = cancelled;
  call.cancel_context = &t;
  memset(&t, 0, sizeof(t));
  assert(invoke(p, &call, &t) == CAI_ERR_CANCELLED);
  cai_plugins_close(p);
  cai_error_cleanup(&error);
  assert(unlink(paths->manifest) == 0 && rmdir(paths->root) == 0);
  free(paths);
  assert(descriptors() == before);
  return NULL;
}
int main(int argc, char **argv) {
  worker w;
  pthread_t thread;
  const pthread_attr_t *attr = NULL;
  char *binary;
#ifdef __GLIBC__
  pthread_attr_t bounded;
  assert(pthread_attr_init(&bounded) == 0 &&
         pthread_attr_setstacksize(&bounded, 128U * 1024U) == 0);
  attr = &bounded;
#endif
  if (argc == 3 && strcmp(argv[1], "--child") == 0)
    return child(argv[2]);
  assert(argc == 2);
  binary = realpath(argv[0], NULL);
  assert(binary != NULL);
  w.build = argv[1];
  w.binary = binary;
  assert(pthread_create(&thread, attr, exercise, &w) == 0);
  assert(pthread_join(thread, NULL) == 0);
  free(binary);
#ifdef __GLIBC__
  pthread_attr_destroy(&bounded);
#endif
  return 0;
}
