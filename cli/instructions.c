#define _POSIX_C_SOURCE 200809L
#include "instructions.h"
#include "error.h"
#include "identity.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int instruction_error(cai_error *error, const char *path,
                             const char *why) {
  char message[1024];
  snprintf(message, sizeof(message), "%s: %s", path, why);
  return cai_cli_error(error, CAI_ERR_INVALID, message);
}

static int load(const char *path, char **out, cai_error *error) {
  struct stat st;
  size_t length, used = 0U;
  ssize_t n;
  int fd, rc = CAI_OK;
  char extra;
  if (path == NULL)
    return CAI_OK;
  fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0)
    return instruction_error(error, path, strerror(errno));
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
      st.st_size > 1024L * 1024L) {
    close(fd);
    return instruction_error(
        error, path, "instructions must be a regular file of at most 1 MiB");
  }
  length = (size_t)st.st_size;
  *out = malloc(length + 1U);
  if (*out == NULL) {
    close(fd);
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate instruction file");
  }
  while (used < length) {
    n = read(fd, *out + used, length - used);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      break;
    used += (size_t)n;
  }
  do {
    n = read(fd, &extra, 1U);
  } while (n < 0 && errno == EINTR);
  if (used != length || n != 0)
    rc =
        instruction_error(error, path, "cannot read complete instruction file");
  else if (memchr(*out, '\0', length) != NULL)
    rc = instruction_error(error, path, "instructions contain a NUL byte");
  (*out)[used] = '\0';
  close(fd);
  return rc;
}

/** Export destinations are ordinary user directories, unlike private store
 * roots. Create missing components without changing existing permissions. */
static int make_directories(char *path, cai_error *error) {
  struct stat st;
  char *cursor;
  for (cursor = path + 1;; cursor++) {
    if (*cursor == '/' || *cursor == '\0') {
      char saved = *cursor;
      *cursor = '\0';
      if (mkdir(path, 0700) != 0 &&
          (errno != EEXIST || stat(path, &st) != 0 || !S_ISDIR(st.st_mode))) {
        *cursor = saved;
        return instruction_error(error, path,
                                 "cannot create instruction output directory");
      }
      *cursor = saved;
      if (saved == '\0')
        return CAI_OK;
    }
  }
}

int cai_cli_instruction_files_open(const cai_cli_options *o,
                                   cai_cli_instruction_files *files,
                                   cai_error *error) {
  int rc;
  memset(files, 0, sizeof(*files));
  rc = load(o->developer_instructions_file, &files->agent, error);
  if (rc == CAI_OK)
    rc = load(o->reviewer_instructions_file, &files->reviewer, error);
  if (rc == CAI_OK)
    rc = load(o->developer_instructions_override_file, &files->initial, error);
  return rc;
}

void cai_cli_instruction_files_close(cai_cli_instruction_files *files) {
  free(files->agent);
  free(files->reviewer);
  free(files->initial);
  memset(files, 0, sizeof(*files));
}

int cai_cli_dump_instructions(const cai_cli_options *o, cai_error *error) {
  cai_cli_instruction_files files;
  cai_agent_preset preset;
  cai_agent_preset_config config;
  cai_sink *sink = NULL;
  char *directory, *path, *name;
  FILE *outputs[2] = {NULL, NULL};
  size_t i, j, length = strlen(o->identity);
  int rc, fd, created = 0;
  /* Large path storage belongs to the heap on default musl worker stacks. */
  directory = malloc(PATH_MAX);
  path = malloc(2U * PATH_MAX);
  name = malloc(length + 1U);
  if (directory == NULL || path == NULL || name == NULL) {
    free(directory);
    free(path);
    free(name);
    return cai_cli_error(error, CAI_ERR_NOMEM,
                         "allocate instruction dump paths");
  }
  for (i = j = 0U; i < length; i++) {
    unsigned char c = (unsigned char)o->identity[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '_')
      name[j++] = c >= 'A' && c <= 'Z' ? (char)(c + 'a' - 'A') : (char)c;
    else if (c == ' ')
      name[j++] = '_';
  }
  name[j] = '\0';
  rc = cai_cli_instruction_files_open(o, &files, error);
  if (rc == CAI_OK && j == 0U)
    rc = cai_cli_error(error, CAI_ERR_INVALID,
                       "identity has no usable filename characters");
  if (rc == CAI_OK)
    rc = cai_cli_workspace_path(directory, PATH_MAX, o->directory,
                                o->dump_developer_instructions, error);
  if (rc == CAI_OK)
    rc = make_directories(directory, error);
  for (i = 0U; rc == CAI_OK && i < 2U; i++) {
    char *p = path + i * PATH_MAX;
    if (snprintf(p, PATH_MAX, "%s/%s_%s_instructions.md", directory, name,
                 i == 0U ? "developer" : "reviewer") >= PATH_MAX) {
      rc = cai_cli_error(error, CAI_ERR_INVALID,
                         "instruction dump path is too long");
      break;
    }
    fd = open(p, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
      rc = instruction_error(error, p, strerror(errno));
      break;
    }
    created++;
    outputs[i] = fdopen(fd, "wb");
    if (outputs[i] == NULL) {
      close(fd);
      rc = instruction_error(error, p, "open instruction output");
    }
  }
  cai_agent_preset_from_smith(&preset);
  for (i = 0U; rc == CAI_OK && i < 2U; i++) {
    cai_agent_preset_config_init(&config);
    config.agent_identity = o->identity;
    config.model =
        i == 1U && o->review_model != NULL ? o->review_model : o->model;
    config.reasoning_effort = i == 1U && o->review_reasoning_effort != NULL
                                  ? o->review_reasoning_effort
                                  : o->reasoning_effort;
    config.developer_instructions_override =
        files.initial != NULL && ((i == 1U) == !!o->review)
            ? files.initial
            : (i == 0U ? files.agent : files.reviewer);
    config.developer_instructions_extension = o->developer_instructions;
    rc = cai_sink_file(outputs[i], 0, &sink, error);
    if (rc == CAI_OK)
      rc = cai_agent_preset_write_base_instructions(&preset, &config, (int)i,
                                                    sink, error);
    cai_sink_close(sink);
    sink = NULL;
  }
  for (i = 0U; i < 2U; i++)
    if (outputs[i] != NULL && fclose(outputs[i]) != 0 && rc == CAI_OK)
      rc = instruction_error(error, path + i * PATH_MAX,
                             "close instruction output");
  if (rc != CAI_OK)
    for (i = 0U; i < (size_t)created; i++)
      (void)unlink(path + i * PATH_MAX);
  cai_cli_instruction_files_close(&files);
  free(directory);
  free(path);
  free(name);
  return rc;
}
