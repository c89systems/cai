#include "cai_instruction_writer.h"
#include "cai_internal.h"

#include <string.h>

#define IDENTITY_TOKEN "{{agent_identity}}"
#define TOOLS_TOKEN "{{agent_tools}}"

static const char patch_fragment[] =
    "Use `apply_patch` for local file edits. Do not create or edit files "
    "with `cat` or other shell write tricks. Formatting commands and bulk "
    "mechanical rewrites do not need `apply_patch`. Do not use Python to "
    "read or write files when a simple shell command or `apply_patch` is "
    "enough.\n";
static const char terminal_fragment[] =
    "- Exercise caution when escaping text for exec_command calls - "
    "backticks and `$()` passed to the `cmd` argument will still execute. "
    "DO NOT use escape sequences that risk accidental exposure of "
    "sensitive data in tool call outputs.\n";

/** One first-occurrence filter. Chaining preserves the previous patch-then-
 * terminal removal order, including matches spanning template substitutions.
 * Only a candidate fragment is retained, never an instruction block. */
typedef struct instruction_filter {
  struct instruction_filter *next;
  const char *fragment;
  size_t length;
  size_t pending_length;
  char pending[sizeof(patch_fragment) + sizeof(terminal_fragment)];
} instruction_filter;

typedef struct instruction_writer {
  cai_sink *sink;
  size_t length;
  cai_error *error;
} instruction_writer;

static int instruction_emit(instruction_writer *writer, const char *bytes,
                            size_t length) {
  size_t take;
  int rc;
  if (length > SIZE_MAX - writer->length)
    return cai_set_error(writer->error, CAI_ERR_INVALID,
                         "agent preset instructions are too large");
  while (length != 0U) {
    take = length > 4096U ? 4096U : length;
    rc = writer->sink != NULL
             ? cai_sink_write(writer->sink, bytes, take, writer->error)
             : CAI_OK;
    if (rc != CAI_OK)
      return rc;
    writer->length += take;
    bytes += take;
    length -= take;
  }
  return CAI_OK;
}

static int instruction_filter_write(instruction_writer *writer,
                                    instruction_filter *filter,
                                    const char *bytes, size_t length) {
  size_t take;
  const char *candidate;
  int rc;
  if (filter == NULL)
    return instruction_emit(writer, bytes, length);
  while (length != 0U) {
    if (filter->fragment == NULL)
      return instruction_filter_write(writer, filter->next, bytes, length);
    if (filter->pending_length == 0U) {
      candidate = memchr(bytes, filter->fragment[0], length);
      take = candidate != NULL ? (size_t)(candidate - bytes) : length;
      rc = instruction_filter_write(writer, filter->next, bytes, take);
      if (rc != CAI_OK)
        return rc;
      bytes += take;
      length -= take;
      if (length == 0U)
        break;
    }
    filter->pending[filter->pending_length++] = *bytes++;
    length--;
    while (filter->pending_length != 0U &&
           memcmp(filter->pending, filter->fragment, filter->pending_length) !=
               0) {
      rc = instruction_filter_write(writer, filter->next, filter->pending, 1U);
      if (rc != CAI_OK)
        return rc;
      filter->pending_length--;
      memmove(filter->pending, filter->pending + 1U, filter->pending_length);
    }
    if (filter->pending_length == filter->length) {
      filter->pending_length = 0U;
      filter->fragment = NULL;
    }
  }
  return CAI_OK;
}

static int instruction_filter_finish(instruction_writer *writer,
                                     instruction_filter *filter) {
  int rc;
  if (filter == NULL)
    return CAI_OK;
  rc = instruction_filter_write(writer, filter->next, filter->pending,
                                filter->pending_length);
  filter->pending_length = 0U;
  return rc == CAI_OK ? instruction_filter_finish(writer, filter->next) : rc;
}

int cai_instruction_template_write(const cai_instruction_template *input,
                                   cai_sink *sink, size_t *length,
                                   size_t *prefix_length, cai_error *error) {
  instruction_writer writer;
  instruction_filter patch, terminal, *filter;
  const char *cursor, *identity, *tools, *token, *last_tools, *replacement;
  const char *suffixes[3];
  size_t i, token_length;
  int rc;
  *length = 0U;
  *prefix_length = 0U;
  writer.sink = sink;
  writer.length = 0U;
  writer.error = error;
  memset(&patch, 0, sizeof(patch));
  memset(&terminal, 0, sizeof(terminal));
  patch.next = &terminal;
  patch.fragment = (input->capabilities & CAI_AGENT_PRESET_TOOL_APPLY_PATCH)
                       ? NULL
                       : patch_fragment;
  patch.length = sizeof(patch_fragment) - 1U;
  terminal.fragment = (input->capabilities & CAI_AGENT_PRESET_TOOL_TERMINAL)
                          ? NULL
                          : terminal_fragment;
  terminal.length = sizeof(terminal_fragment) - 1U;
  last_tools = NULL;
  if (input->tools != NULL)
    for (cursor = input->text; (token = strstr(cursor, TOOLS_TOKEN)) != NULL;
         cursor = token + sizeof(TOOLS_TOKEN) - 1U)
      last_tools = token;
  filter = last_tools != NULL ? &patch : NULL;
  cursor = input->text;
  rc = CAI_OK;
  while (rc == CAI_OK) {
    identity = strstr(cursor, IDENTITY_TOKEN);
    tools = input->tools != NULL ? strstr(cursor, TOOLS_TOKEN) : NULL;
    token = tools == NULL || (identity != NULL && identity < tools) ? identity
                                                                    : tools;
    rc = instruction_filter_write(&writer, filter, cursor,
                                  token != NULL ? (size_t)(token - cursor)
                                                : strlen(cursor));
    if (rc != CAI_OK || token == NULL)
      break;
    if (token == last_tools) {
      rc = instruction_filter_finish(&writer, filter);
      if (rc != CAI_OK)
        break;
      filter = NULL;
      *prefix_length = writer.length;
    }
    replacement = token == identity ? input->identity : input->tools;
    token_length = token == identity ? sizeof(IDENTITY_TOKEN) - 1U
                                     : sizeof(TOOLS_TOKEN) - 1U;
    rc = instruction_filter_write(&writer, filter, replacement,
                                  strlen(replacement));
    cursor = token + token_length;
  }
  if (rc == CAI_OK)
    rc = instruction_filter_finish(&writer, filter);
  suffixes[0] = input->repository;
  suffixes[1] = input->skills;
  suffixes[2] = input->extension;
  for (i = 0U; rc == CAI_OK && i < 3U; i++) {
    if (suffixes[i] == NULL || suffixes[i][0] == '\0')
      continue;
    rc = instruction_emit(&writer, "\n\n", 2U);
    if (rc == CAI_OK)
      rc = instruction_emit(&writer, suffixes[i], strlen(suffixes[i]));
  }
  *length = writer.length;
  return rc;
}
