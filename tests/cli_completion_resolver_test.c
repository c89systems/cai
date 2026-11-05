#define _POSIX_C_SOURCE 200809L
#include "../cli/completion.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct resolver_fixture {
  cai_cli_completions completions;
  sl_completion_provider_t providers[3];
  char root[4096], path[4096], names[16][512];
  size_t count;
  int fail, calls, empty;
  size_t provider_count;
} resolver_fixture;
static resolver_fixture *fixture;
int __real_sl_set_completion_providers(sl_t *, const sl_completion_provider_t *,
                                       size_t);
int __wrap_sl_set_completion_providers(sl_t *, const sl_completion_provider_t *,
                                       size_t);
int __wrap_sl_set_completion_providers(sl_t *editor,
                                       const sl_completion_provider_t *items,
                                       size_t count) {
  assert(count == 2U || count == 3U);
  fixture->provider_count = count;
  memset(fixture->providers, 0, sizeof(fixture->providers));
  memcpy(fixture->providers, items, count * sizeof(*items));
  return __real_sl_set_completion_providers(editor, items, count);
}
static int collect(const char *name, void *context) {
  resolver_fixture *out = context;
  if (out->fail)
    return SL_ERROR_NOMEM;
  assert(out->count < 16U && strlen(name) < sizeof(out->names[0]));
  strcpy(out->names[out->count++], name);
  return SL_OK;
}
static int catalog(void *context, char trigger, sl_completion_emit_t emit,
                   void *emit_context) {
  resolver_fixture *out = context;
  out->calls++;
  if (out->empty && trigger == '$')
    return SL_OK;
  return emit(trigger == '/' ? "/status" : "$fixture-skill", emit_context);
}
static int contains(resolver_fixture *out, const char *name) {
  size_t i;
  for (i = 0U; i < out->count; ++i)
    if (strcmp(out->names[i], name) == 0)
      return 1;
  return 0;
}
static void path(resolver_fixture *out, const char *name) {
  int length = snprintf(out->path, sizeof(out->path), "%s/%s", out->root, name);
  assert(length >= 0 && (size_t)length < sizeof(out->path));
}
static void touch(resolver_fixture *out, const char *name) {
  FILE *file;
  path(out, name);
  file = fopen(out->path, "wb");
  assert(file != NULL && fclose(file) == 0);
}
static int resolve(sl_t *editor, size_t provider, const char *prefix) {
  sl_completion_provider_t *item = &fixture->providers[provider];
  fixture->count = 0U;
  return prefix == NULL
             ? item->callback(editor, item->trigger, collect, fixture,
                              item->userdata)
             : item->expand(editor, prefix, collect, fixture, item->userdata);
}
int main(int argc, char **argv) {
  sl_t *editor;
  int length;
  assert(argc == 2);
  fixture = calloc(1U, sizeof(*fixture));
  assert(fixture != NULL);
  length = snprintf(fixture->root, sizeof(fixture->root),
                    "%s/completion-XXXXXX", argv[1]);
  assert(length >= 0 && (size_t)length < sizeof(fixture->root));
  assert(mkdtemp(fixture->root) != NULL);
  path(fixture, "directory");
  assert(mkdir(fixture->path, 0700) == 0);
  path(fixture, ".git");
  assert(mkdir(fixture->path, 0700) == 0);
  touch(fixture, "file name");
  touch(fixture, ".hidden");
  touch(fixture, "unsafe\nname");
  touch(fixture, "unsafe\xc2\x9b");
  touch(fixture, "unsafe\xff");
  touch(fixture, "directory/child");
  path(fixture, "link");
  assert(symlink("directory", fixture->path) == 0);
  editor = sl_create();
  assert(editor != NULL &&
         cai_cli_completions_open(&fixture->completions, editor, fixture->root,
                                  catalog, fixture) == SL_OK);
  assert(fixture->providers[0].scope == SL_COMPLETION_START &&
         !fixture->providers[0].strip_trigger &&
         fixture->providers[1].scope == SL_COMPLETION_WORD &&
         fixture->providers[1].strip_trigger &&
         fixture->providers[2].scope == SL_COMPLETION_WORD &&
         fixture->providers[2].strip_trigger);
  assert(resolve(editor, 0U, NULL) == SL_OK && contains(fixture, "/status"));
  assert(resolve(editor, 2U, NULL) == SL_OK &&
         contains(fixture, "$fixture-skill") && fixture->calls == 3);
  assert(resolve(editor, 1U, NULL) == SL_OK && fixture->count == 4U &&
         contains(fixture, "@file name") && contains(fixture, "@.hidden") &&
         contains(fixture, "@directory/") && contains(fixture, "@link"));
  assert(resolve(editor, 1U, "@directory/") == SL_OK && fixture->count == 1U &&
         contains(fixture, "@directory/child"));
  assert(resolve(editor, 1U, "@file name") == SL_OK && fixture->count == 0U);
  assert(sl_set_buffer(editor, "draft kept") == SL_OK);
  assert(resolve(editor, 1U, "@missing/") == SL_ERROR_IO &&
         strcmp(sl_buffer(editor), "draft kept") == 0);
  fixture->fail = 1;
  assert(resolve(editor, 1U, NULL) == SL_ERROR_NOMEM &&
         resolve(editor, 0U, NULL) == SL_ERROR_NOMEM);
  fixture->fail = 0;
  assert(resolve(editor, 1U, NULL) == SL_OK && fixture->count == 4U);
  assert(cai_cli_completions_open(&fixture->completions, editor, "", catalog,
                                  fixture) == SL_ERROR_INVALID);
  fixture->empty = 1;
  assert(sl_set_buffer(editor, "!kill -TERM $$") == SL_OK);
  assert(cai_cli_completions_open(&fixture->completions, editor, fixture->root,
                                  catalog, fixture) == SL_OK &&
         fixture->provider_count == 2U &&
         strcmp(sl_buffer(editor), "!kill -TERM $$") == 0);
  fixture->empty = 0;
  assert(cai_cli_completions_open(&fixture->completions, editor, fixture->root,
                                  catalog, fixture) == SL_OK &&
         fixture->provider_count == 3U);
  sl_destroy(editor);
  path(fixture, "link");
  assert(unlink(fixture->path) == 0);
  path(fixture, "file name");
  assert(unlink(fixture->path) == 0);
  path(fixture, ".hidden");
  assert(unlink(fixture->path) == 0);
  path(fixture, "unsafe\nname");
  assert(unlink(fixture->path) == 0);
  path(fixture, "unsafe\xc2\x9b");
  assert(unlink(fixture->path) == 0);
  path(fixture, "unsafe\xff");
  assert(unlink(fixture->path) == 0);
  path(fixture, "directory/child");
  assert(unlink(fixture->path) == 0);
  path(fixture, "directory");
  assert(rmdir(fixture->path) == 0);
  path(fixture, ".git");
  assert(rmdir(fixture->path) == 0 && rmdir(fixture->root) == 0);
  free(fixture);
  return 0;
}
