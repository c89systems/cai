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

/* Renderer guidance belongs to libcai_app. The runtime's existing instruction
 * extension carries it to children and model changes without a libmdf
 * dependency in libcai. Keep each C89 string below its required 509 bytes. */
static const char *const chart_guide[] = {
    "## Markdown charts (on request)\n\n"
    "Use these libmdf chart fences when the user requests charts, including a "
    "standing request, and the required output format permits them. Otherwise "
    "use prose or tables; do not turn ordinary numeric results into charts. "
    "Bodies are two-column CSV: label,value followed by nonnegative raw "
    "numbers. Use normal CSV quoting for labels. Use tables for negative "
    "values; never alter data to fit.\n\n",
    "Horizontal bars (`mdf-bar-chart` is an alias):\n"
    "```mdf-horizontal-bar-chart\nlabel,value\nBuild,40\nTest,25\n```\n\n"
    "Vertical bars:\n"
    "```mdf-vertical-bar-chart\nlabel,value\nBuild,40\nTest,25\n```\n\n",
    "Part-of-whole (\"bar pie\"): tile charts render a single 100% stacked "
    "bar, not a circular pie. Percentages are calculated from raw values.\n"
    "```mdf-tile-chart\nlabel,value\nUsed,60\nFree,40\n```"};

/** One owned extension for live runtime construction, prompt dumps and offline
 * export. Explicit user additions follow the optional renderer guide. */
static int extension(const cai_cli_options *o, char **out, cai_error *error) {
  const char *user = o->developer_instructions;
  size_t i, guide = 0U, added = user != NULL ? strlen(user) : 0U, used = 0U;
  if (!o->disable_chart_instructions)
    for (i = 0U; i < sizeof(chart_guide) / sizeof(chart_guide[0]); ++i)
      guide += strlen(chart_guide[i]);
  if (guide == 0U && user == NULL)
    return CAI_OK;
  if (added > (size_t)-1 - guide - 3U)
    return cai_cli_error(error, CAI_ERR_LIMIT,
                         "instruction additions too large");
  *out = malloc(guide + added + 3U);
  if (*out == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM,
                         "allocate instruction additions");
  if (guide != 0U)
    for (i = 0U; i < sizeof(chart_guide) / sizeof(chart_guide[0]); ++i) {
      size_t n = strlen(chart_guide[i]);
      memcpy(*out + used, chart_guide[i], n);
      used += n;
    }
  if (guide != 0U && added != 0U) {
    memcpy(*out + used, "\n\n", 2U);
    used += 2U;
  }
  if (added != 0U) {
    memcpy(*out + used, user, added);
    used += added;
  }
  (*out)[used] = '\0';
  return CAI_OK;
}

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
  if (rc == CAI_OK)
    rc = extension(o, &files->extension, error);
  return rc;
}

void cai_cli_instruction_files_close(cai_cli_instruction_files *files) {
  free(files->agent);
  free(files->reviewer);
  free(files->initial);
  free(files->extension);
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
    config.developer_instructions_extension = files.extension;
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
