#define _POSIX_C_SOURCE 200809L
#include "completion.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int filename_printable(const char *name) {
  const unsigned char *p = (const unsigned char *)name;
  while (*p != 0U) {
    size_t need, i;
    if (*p < 32U || *p == 127U)
      return 0;
    if (*p < 128U) {
      ++p;
      continue;
    }
    need = *p >= 0xc2U && *p < 0xe0U    ? 2U
           : *p >= 0xe0U && *p < 0xf0U  ? 3U
           : *p >= 0xf0U && *p <= 0xf4U ? 4U
                                        : 0U;
    if (need == 0U)
      return 0;
    for (i = 1U; i < need; ++i)
      if ((p[i] & 0xc0U) != 0x80U)
        return 0;
    if ((*p == 0xc2U && p[1] < 0xa0U) || (*p == 0xe0U && p[1] < 0xa0U) ||
        (*p == 0xedU && p[1] >= 0xa0U) || (*p == 0xf0U && p[1] < 0x90U) ||
        (*p == 0xf4U && p[1] >= 0x90U))
      return 0;
    p += need;
  }
  return 1;
}

static int complete_files(cai_cli_completions *completions, const char *prefix,
                          sl_completion_emit_t emit, void *context) {
  size_t length = strlen(prefix), workspace = strlen(completions->workspace);
  DIR *directory;
  char *path;
  int rc = SL_OK;
  if (length > 1U && prefix[length - 1U] != '/')
    return SL_OK;
  if (workspace > (size_t)-1 - length - 2U)
    return SL_ERROR_NOMEM;
  path = malloc(workspace + length + 2U);
  if (path == NULL)
    return SL_ERROR_NOMEM;
  if (prefix[1] == '/')
    memcpy(path, prefix + 1U, length);
  else {
    memcpy(path, completions->workspace, workspace);
    path[workspace] = '/';
    memcpy(path + workspace + 1U, prefix + 1U, length);
  }
  directory = opendir(path);
  free(path);
  if (directory == NULL)
    return SL_ERROR_IO;
  for (;;) {
    struct dirent *entry;
    struct stat info;
    char *candidate;
    size_t name, used;
    errno = 0;
    entry = readdir(directory);
    if (entry == NULL) {
      if (errno != 0)
        rc = SL_ERROR_IO;
      break;
    }
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 ||
        strcmp(entry->d_name, ".git") == 0 ||
        !filename_printable(entry->d_name))
      continue;
    if (fstatat(dirfd(directory), entry->d_name, &info, AT_SYMLINK_NOFOLLOW) !=
        0)
      continue;
    name = strlen(entry->d_name);
    if (length > (size_t)-1 - name - 2U) {
      rc = SL_ERROR_NOMEM;
      break;
    }
    candidate = malloc(length + name + 2U);
    if (candidate == NULL) {
      rc = SL_ERROR_NOMEM;
      break;
    }
    memcpy(candidate, prefix, length);
    memcpy(candidate + length, entry->d_name, name);
    used = length + name;
    if (S_ISDIR(info.st_mode))
      candidate[used++] = '/';
    candidate[used] = '\0';
    rc = emit(candidate, context);
    free(candidate);
    if (rc != SL_OK)
      break;
  }
  if (closedir(directory) != 0 && rc == SL_OK)
    rc = SL_ERROR_IO;
  return rc;
}

static int complete(sl_t *editor, char trigger, sl_completion_emit_t emit,
                    void *emit_context, void *context) {
  cai_cli_completions *completions = context;
  (void)editor;
  return trigger == '@' ? complete_files(completions, "@", emit, emit_context)
                        : completions->catalog(completions->context, trigger,
                                               emit, emit_context);
}
static int expand_files(sl_t *editor, const char *completed,
                        sl_completion_emit_t emit, void *emit_context,
                        void *context) {
  (void)editor;
  return complete_files(context, completed, emit, emit_context);
}
int cai_cli_completions_open(cai_cli_completions *completions, sl_t *editor,
                             const char *workspace,
                             cai_cli_completion_catalog_fn catalog,
                             void *context) {
  sl_completion_provider_t providers[] = {
      {'/', SL_COMPLETION_START, 0, complete, NULL, NULL},
      {'@', SL_COMPLETION_WORD, 1, complete, NULL, expand_files},
      {'$', SL_COMPLETION_WORD, 1, complete, NULL, NULL}};
  size_t i;
  if (completions == NULL || workspace == NULL || workspace[0] == '\0' ||
      catalog == NULL)
    return SL_ERROR_INVALID;
  completions->workspace = workspace;
  completions->catalog = catalog;
  completions->context = context;
  for (i = 0U; i < sizeof(providers) / sizeof(providers[0]); ++i)
    providers[i].userdata = completions;
  return sl_set_completion_providers(editor, providers,
                                     sizeof(providers) / sizeof(providers[0]));
}
