#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "session_commands.h"
#include "identity.h"
#include "markdown_export.h"
#include "messages.h"

#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wchar.h>

static int session_select(cai_cli_pouch *pouch, const char *workspace,
                          const char *choice,
                          char out[CAI_AGENT_SESSION_ID_MAX], int deleting,
                          cai_error *error) {
  cai_cli_session *sessions = NULL;
  unsigned long long number;
  size_t count = 0U, i;
  char *end;
  int rc;
  if (choice == NULL || choice[0] == '\0')
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "select a conversation ID or list number");
  if (strspn(choice, "0123456789") != strlen(choice)) {
    if (strlen(choice) >= CAI_AGENT_SESSION_ID_MAX)
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "conversation ID is too long");
    strcpy(out, choice);
    return CAI_OK;
  }
  rc = cai_cli_pouch_list(pouch, workspace, &sessions, &count, error);
  if (rc != CAI_OK)
    goto done;
  for (i = 0U; i < count; i++) {
    if (strcmp(choice, sessions[i].id) == 0) {
      strcpy(out, sessions[i].id);
      free(sessions);
      return CAI_OK;
    }
  }
  if (deleting) {
    rc = cai_cli_sessions_deletable(pouch, sessions, &count, error);
    if (rc != CAI_OK)
      goto done;
  }
  errno = 0;
  number = strtoull(choice, &end, 10);
  if (errno != 0 || *end != '\0' || number == 0U || number > count)
    rc = cai_cli_error(error, CAI_ERR_INVALID,
                       "No conversation with that number in this directory. "
                       "List conversations with --resume or --delete.");
  else
    strcpy(out, sessions[(size_t)number - 1U].id);
done:
  free(sessions);
  return rc;
}

int cai_cli_session_select(cai_cli_pouch *pouch, const char *workspace,
                           const char *choice,
                           char out[CAI_AGENT_SESSION_ID_MAX],
                           cai_error *error) {
  return session_select(pouch, workspace, choice, out, 0, error);
}
int cai_cli_session_select_delete(cai_cli_pouch *pouch, const char *workspace,
                                  const char *choice,
                                  char out[CAI_AGENT_SESSION_ID_MAX],
                                  cai_error *error) {
  return session_select(pouch, workspace, choice, out, 1, error);
}
int cai_cli_sessions_deletable(cai_cli_pouch *pouch, cai_cli_session *sessions,
                               size_t *count, cai_error *error) {
  size_t i, kept = 0U;
  int eligible, rc;
  for (i = 0U; i < *count; i++) {
    rc = cai_cli_pouch_deletable(pouch, sessions[i].id, &eligible, error);
    if (rc != CAI_OK)
      return rc;
    if (eligible)
      sessions[kept++] = sessions[i];
  }
  *count = kept;
  return CAI_OK;
}
static int session_file_id(const char *id) {
  return id != NULL && id[0] != '\0' && strlen(id) < CAI_AGENT_SESSION_ID_MAX &&
         strspn(id, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
                    "0123456789-_") == strlen(id);
}
/* Sanitize and clip a UTF-8 field by displayed cells, preserving codepoints. */
static void row_field(char *out, size_t capacity, const char *text, int width) {
  mbstate_t state;
  wchar_t code;
  const char *cursor;
  size_t n;
  size_t used;
  int cells;
  int visible;
  int clipped;
  int pass;
  clipped = 0;
  used = 0U;
  for (pass = 0; pass < 2; pass++) {
    memset(&state, 0, sizeof(state));
    cursor = text;
    used = 0U;
    cells = 0;
    while (*cursor != '\0' && width > 0) {
      n = mbrtowc(&code, cursor, MB_CUR_MAX, &state);
      if (n == (size_t)-1 || n == (size_t)-2) {
        n = 1U;
        code = L'?';
        memset(&state, 0, sizeof(state));
      }
      visible = wcwidth(code);
      if (visible < 0)
        visible = 1;
      if (cells + visible > width || used + n + 1U > capacity)
        break;
      if (pass != 0) {
        if (code == L'?' || wcwidth(code) < 0)
          out[used] = code == L'?' ? '?' : ' ';
        else
          memcpy(out + used, cursor, n);
      }
      used += code == L'?' || wcwidth(code) < 0 ? 1U : n;
      cells += visible;
      cursor += n;
    }
    if (pass == 0 && *cursor != '\0' && width >= 3) {
      clipped = 1;
      width -= 3;
    }
  }
  out[used] = '\0';
  if (clipped && used + 4U <= capacity)
    memcpy(out + used, "...", 4U);
}

/** UTF-8 presentation must not depend on, or change, the embedding process's
 * locale. Install this view only around character sizing, never host sinks. */
static locale_t row_locale(void) {
  locale_t locale = newlocale(LC_CTYPE_MASK, "C.UTF-8", (locale_t)0);
  if (locale == (locale_t)0)
    locale = newlocale(LC_CTYPE_MASK, "en_US.UTF-8", (locale_t)0);
  return locale;
}

static int row_cells(const char *text) {
  mbstate_t state;
  wchar_t code;
  size_t n;
  int cells;
  int width;
  memset(&state, 0, sizeof(state));
  cells = 0;
  while (*text != '\0') {
    n = mbrtowc(&code, text, MB_CUR_MAX, &state);
    if (n == (size_t)-1 || n == (size_t)-2) {
      n = 1U;
      width = 1;
      memset(&state, 0, sizeof(state));
    } else {
      width = wcwidth(code);
    }
    cells += width > 0 ? width : 0;
    text += n;
  }
  return cells;
}

void cai_cli_history_row(char *out, size_t capacity,
                         const cai_agent_history_entry *entry, int width) {
  char line[1200];
  size_t length;
  locale_t locale, previous = (locale_t)0;
  snprintf(line, sizeof(line), "%lu. %s%s", (unsigned long)entry->remove_count,
           entry->kind == CAI_AGENT_HISTORY_SHELL ? "Shell: " : "",
           entry->summary);
  if (capacity < 2U)
    return;
  locale = row_locale();
  if (locale != (locale_t)0)
    previous = uselocale(locale);
  row_field(out, capacity - 1U, line, width > 0 ? width : 1);
  if (locale != (locale_t)0) {
    (void)uselocale(previous);
    freelocale(locale);
  }
  length = strlen(out);
  out[length++] = '\n';
  out[length] = '\0';
}

int cai_cli_sessions_print(cai_sink *destination,
                           const cai_cli_session *sessions, size_t count,
                           int numbered, int width, cai_error *error) {
  size_t i;
  char directory[PATH_MAX];
  char prompt[2048];
  char line[PATH_MAX + 2304];
  char clipped[PATH_MAX + 2304];
  char number[32];
  int dir_width;
  int prompt_width;
  int prefix_width;
  int rc = CAI_OK;
  locale_t locale, previous = (locale_t)0;
  if (width <= 0)
    width = 80;
  if (count == 0U &&
      cai_sink_write(destination, "No conversations.\n", 18U, error) != CAI_OK)
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "write session list");
  locale = row_locale();
  for (i = 0U; i < count; i++) {
    if (locale != (locale_t)0)
      previous = uselocale(locale);
    number[0] = '\0';
    if (numbered)
      snprintf(number, sizeof(number), "%lu  ", (unsigned long)(i + 1U));
    prefix_width = (int)(strlen(number) + strlen(sessions[i].id) + 4U);
    dir_width = width > prefix_width ? (width - prefix_width) / 2 : 0;
    if (dir_width > 32)
      dir_width = 32;
    row_field(directory, sizeof(directory), sessions[i].workspace, dir_width);
    /* A conservative directory allocation keeps a prompt column on every row.
     */
    prompt_width = width - prefix_width - row_cells(directory);
    row_field(prompt, sizeof(prompt), sessions[i].first_prompt, prompt_width);
    snprintf(line, sizeof(line), "%s%s  %s  %s", number, sessions[i].id,
             directory, prompt);
    row_field(clipped, sizeof(clipped), line, width);
    if (locale != (locale_t)0)
      (void)uselocale(previous);
    if (cai_sink_write(destination, clipped, strlen(clipped), error) !=
            CAI_OK ||
        cai_sink_write(destination, "\n", 1U, error) != CAI_OK) {
      rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "write session list");
      break;
    }
  }
  if (locale != (locale_t)0)
    freelocale(locale);
  return rc;
}

static int export_path(const cai_cli_pouch *pouch, char *path, size_t capacity,
                       const char *root, const char *id, cai_error *error) {
  const char *base;
  int n;
  if (root != NULL)
    n = snprintf(path, capacity, "%s/%s", root, id);
  else {
    base = getenv("XDG_DATA_HOME");
    if (base != NULL && base[0] == '/')
      n = snprintf(path, capacity, "%s/%s/exports/%s", base,
                   pouch->program_name, id);
    else {
      base = getenv("HOME");
      if (base == NULL || base[0] != '/')
        return cai_cli_error(
            error, CAI_ERR_INVALID,
            "set HOME or XDG_DATA_HOME for conversation exports");
      n = snprintf(path, capacity, "%s/.local/share/%s/exports/%s", base,
                   pouch->program_name, id);
    }
  }
  if (n < 0 || (size_t)n >= capacity)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "export directory path too long");
  return CAI_OK;
}

static int export_directory(char *path, int require_private, cai_error *error) {
  char *cursor;
  struct stat status;
  for (cursor = path + 1;; cursor++) {
    if (*cursor != '/' && *cursor != '\0')
      continue;
    if (*cursor == '/') {
      *cursor = '\0';
      if (mkdir(path, 0700) != 0 && errno != EEXIST) {
        *cursor = '/';
        return cai_cli_error(error, CAI_ERR_TRANSPORT,
                             "create export directory");
      }
      *cursor = '/';
    } else {
      if (mkdir(path, 0700) != 0 && errno != EEXIST)
        return cai_cli_error(error, CAI_ERR_TRANSPORT,
                             "create session export directory");
      break;
    }
  }
  if ((require_private ? lstat(path, &status) : stat(path, &status)) != 0 ||
      !S_ISDIR(status.st_mode))
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "export parent is not a directory");
  if (require_private &&
      (status.st_uid != geteuid() || (status.st_mode & 077U) != 0U))
    return cai_cli_error(
        error, CAI_ERR_INVALID,
        "session export directory must be private (mode 0700)");
  return CAI_OK;
}

static int export_file(const char *path, char *temporary, size_t capacity,
                       FILE **out, cai_error *error) {
  const char *slash = strrchr(path, '/');
  int fd, n;
  /* A fixed sibling name supports long destination basenames without
   * exceeding the filesystem's per-component limit just for staging. */
  n = slash == NULL ? snprintf(temporary, capacity, ".export-XXXXXX")
                    : snprintf(temporary, capacity, "%.*s/.export-XXXXXX",
                               (int)(slash - path), path);
  if (n < 0 || (size_t)n >= capacity) {
    temporary[0] = '\0';
    return cai_cli_error(error, CAI_ERR_INVALID, "export path too long");
  }
  fd = mkstemp(temporary);
  if (fd < 0) {
    temporary[0] = '\0';
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "create private export file");
  }
  *out = fdopen(fd, "wb");
  if (*out == NULL) {
    close(fd);
    unlink(temporary);
    temporary[0] = '\0';
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "open export output stream");
  }
  return CAI_OK;
}

/* Named destinations are workspace-relative, independent of the managed
 * export root. Only an exact .md/.jsonl suffix selects one projection. */
static int export_targets(const cai_cli_pouch *pouch, const char *workspace,
                          const char *root, const char *id,
                          const char *basename, char paths[2][PATH_MAX],
                          cai_error *error) {
  static const char *const suffixes[] = {".md", ".jsonl"};
  char directory[PATH_MAX], resolved[PATH_MAX];
  char *slash;
  size_t length, i;
  int selected = -1, n, rc;
  struct stat status;
  memset(paths, 0, 2U * PATH_MAX);
  if (basename != NULL) {
    rc = cai_cli_workspace_path(resolved, sizeof(resolved), workspace, basename,
                                error);
    if (rc != CAI_OK)
      return rc;
    basename = resolved;
  }
  if (basename == NULL) {
    rc = export_path(pouch, directory, sizeof(directory), root, id, error);
    if (rc == CAI_OK)
      rc = export_directory(directory, 1, error);
    if (rc != CAI_OK)
      return rc;
    for (i = 0U; i < 2U; i++) {
      n = snprintf(paths[i], PATH_MAX, "%s/%s%s", directory, id, suffixes[i]);
      if (n < 0 || n >= PATH_MAX)
        return cai_cli_error(error, CAI_ERR_INVALID,
                             "export file path too long");
    }
  } else {
    length = strlen(basename);
    slash = strrchr(basename, '/');
    if (length == 0U || length >= PATH_MAX || basename[length - 1U] == '/' ||
        strcmp(slash != NULL ? slash + 1U : basename, ".") == 0 ||
        strcmp(slash != NULL ? slash + 1U : basename, "..") == 0)
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "export basename must name a file");
    for (i = 0U; i < 2U; i++) {
      size_t suffix_length = strlen(suffixes[i]);
      if (length >= suffix_length &&
          strcmp(basename + length - suffix_length, suffixes[i]) == 0)
        selected = (int)i;
    }
    for (i = 0U; i < 2U; i++) {
      if (selected != -1 && selected != (int)i)
        continue;
      n = snprintf(paths[i], PATH_MAX, "%s%s", basename,
                   selected == -1 ? suffixes[i] : "");
      if (n < 0 || n >= PATH_MAX)
        return cai_cli_error(error, CAI_ERR_INVALID,
                             "export file path too long");
    }
    strcpy(directory, basename);
    slash = strrchr(directory, '/');
    if (slash == directory)
      slash[1] = '\0';
    else if (slash != NULL)
      *slash = '\0';
    else
      strcpy(directory, ".");
    rc = export_directory(directory, 0, error);
    if (rc != CAI_OK)
      return rc;
  }
  /* Catch invalid destinations before publishing either projection. Existing
   * regular files are replaced via rename; their contents are never truncated
   * while an export is still being generated. */
  for (i = 0U; i < 2U; i++) {
    if (paths[i][0] == '\0')
      continue;
    if (lstat(paths[i], &status) == 0) {
      if (!S_ISREG(status.st_mode) && !S_ISLNK(status.st_mode))
        return cai_cli_error(error, CAI_ERR_INVALID,
                             "export destination must be a file");
    } else if (errno != ENOENT)
      return cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "cannot inspect export destination");
  }
  return CAI_OK;
}

struct cai_cli_export {
  cai_cli_pouch *pouch;
  cai_cli_session metadata;
  cai_cli_options instruction_options;
  cai_cli_pouch_transcript *reader;
  cai_cli_journal_view *view;
  lc_collection_head_res head;
  char workspace[PATH_MAX], paths[2][PATH_MAX], temporary[2][PATH_MAX];
  struct cai_cli_session_guard *use;
  FILE *file;
  int (*cancelled)(void *);
  void *cancel_context;
  int started;
};
void cai_cli_export_close(cai_cli_export *e) {
  size_t i;
  if (e == NULL)
    return;
  for (i = 0U; i < 2U; ++i)
    if (e->temporary[i][0] != '\0')
      unlink(e->temporary[i]);
  cai_cli_journal_view_close(e->view);
  cai_cli_pouch_transcript_close(e->reader);
  cai_cli_pouch_end_read(e->use);
  free(e);
}
int cai_cli_export_prepare(cai_cli_pouch *pouch, const char *id,
                           const char *workspace, const char *export_root,
                           const char *basename, cai_cli_export **out,
                           const cai_cli_options *options, cai_error *error) {
  cai_cli_export *e;
  const char *cursor;
  lc_lease *lease = NULL;
  lc_error detail;
  int rc;
  *out = NULL;
  for (cursor = id; *cursor != '\0'; cursor++) {
    if (!((*cursor >= 'a' && *cursor <= 'z') ||
          (*cursor >= 'A' && *cursor <= 'Z') ||
          (*cursor >= '0' && *cursor <= '9') || *cursor == '-' ||
          *cursor == '_'))
      return cai_cli_error(
          error, CAI_ERR_INVALID,
          "export ID must contain only letters, digits, '-' or '_'");
  }
  if (id[0] == '\0')
    return cai_cli_error(error, CAI_ERR_INVALID, "export session ID is empty");
  if (strlen(workspace) >= PATH_MAX)
    return cai_cli_error(error, CAI_ERR_LIMIT,
                         "export workspace path too long");
  e = calloc(1U, sizeof(*e));
  if (e == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate export context");
  e->pouch = pouch;
  e->instruction_options = *options;
  strcpy(e->workspace, workspace);
  rc = cai_cli_pouch_begin_read(pouch, id, &e->use, error);
  if (rc == CAI_OK)
    rc = cai_cli_pouch_lock(pouch, id, &lease, error);
  if (rc == CAI_OK)
    rc = cai_cli_pouch_session(pouch, id, &e->metadata, error);
  if (rc == CAI_OK)
    rc = export_targets(pouch, workspace, export_root, id, basename, e->paths,
                        error);
  if (rc == CAI_OK)
    rc = cai_cli_pouch_transcript_open(pouch, e->metadata.workspace, id,
                                       &e->reader, error);
  lc_error_init(&detail);
  if (rc == CAI_OK)
    if (e->reader->collection->head(e->reader->collection, &e->head, &detail) !=
        LC_OK)
      rc = cai_cli_lc_error(error, &detail);
  lc_error_cleanup(&detail);
  /* The read pin owns deletion exclusion. Release the lookup lease before
   * returning so durable queue/goal updates can continue during the export. */
  rc = cai_cli_pouch_unlock(lease, rc, error);
  if (rc != CAI_OK)
    cai_cli_export_close(e);
  else
    *out = e;
  return rc;
}
static int export_cancelled(cai_cli_export *e, cai_error *error) {
  return e->cancelled != NULL && e->cancelled(e->cancel_context)
             ? cai_cli_error(error, CAI_ERR_CANCELLED,
                             "conversation export cancelled")
             : CAI_OK;
}
static int export_write(void *context, const void *bytes, size_t count,
                        cai_error *error) {
  cai_cli_export *e = context;
  int rc = export_cancelled(e, error);
  if (rc == CAI_OK && fwrite(bytes, 1U, count, e->file) != count)
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "write conversation export");
  return rc;
}
int cai_cli_export_run(cai_cli_export *e, const cai_handover_config *active,
                       int (*cancelled)(void *), void *cancel_context,
                       cai_sink *destination, cai_error *error) {
  cai_cli_journal_options options;
  cai_sink sink;
  size_t i;
  int rc;
  if (e == NULL || e->started || destination == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "unused export and destination required");
  e->started = 1;
  e->cancelled = cancelled;
  e->cancel_context = cancel_context;
  memset(&options, 0, sizeof(options));
  options.head = &e->head;
  options.cancelled = cancelled;
  options.cancel_context = cancel_context;
  memset(&sink, 0, sizeof(sink));
  sink.callbacks.write = export_write;
  sink.callbacks.context = e;
  rc = cai_cli_journal_view_open(e->reader->collection, &options, &e->view,
                                 error);
  /* Finish every selected stream before publishing any output. */
  for (i = 0U; rc == CAI_OK && i < 2U; i++) {
    if (e->paths[i][0] == '\0')
      continue;
    rc = export_file(e->paths[i], e->temporary[i], sizeof(e->temporary[i]),
                     &e->file, error);
    if (rc == CAI_OK && i == 0U)
      rc = cai_cli_markdown_export(e->view, e->pouch, &e->metadata,
                                   e->workspace, active,
                                   &e->instruction_options, &sink, error);
    if (rc == CAI_OK && i == 1U)
      rc = cai_cli_journal_view_jsonl(e->view, &sink, error);
    if (e->file != NULL) {
      if (rc == CAI_OK && (fflush(e->file) != 0 || fsync(fileno(e->file)) != 0))
        rc =
            cai_cli_error(error, CAI_ERR_TRANSPORT, "sync conversation export");
      if (fclose(e->file) != 0 && rc == CAI_OK)
        rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "close conversation export");
      e->file = NULL;
    }
  }
  /* Publication is the cancellation decision point. Once accepted, finish
   * publishing the already-generated siblings without reporting a late cancel.
   */
  if (rc == CAI_OK)
    rc = export_cancelled(e, error);
  for (i = 0U; rc == CAI_OK && i < 2U; i++)
    if (e->paths[i][0] != '\0' && rename(e->temporary[i], e->paths[i]) != 0)
      rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "publish conversation export");
  for (i = 0U; i < 2U; i++)
    if (e->temporary[i][0] != '\0') {
      unlink(e->temporary[i]);
      e->temporary[i][0] = '\0';
    }
  /* Preserve the CLI utility's JSONL-then-Markdown listing order. */
  for (i = 2U; rc == CAI_OK && i > 0U; i--) {
    const char *path = e->paths[i - 1U];
    if (path[0] != '\0') {
      rc = cai_sink_write(destination, path, strlen(path), error);
      if (rc == CAI_OK)
        rc = cai_sink_write(destination, "\n", 1U, error);
    }
  }
  return rc;
}

int cai_cli_session_export(cai_cli_pouch *pouch, const char *id,
                           const char *workspace, const char *directory,
                           const char *basename,
                           const cai_handover_config *active,
                           const cai_cli_options *options,
                           cai_sink *destination, cai_error *error) {
  cai_cli_export *e = NULL;
  int rc = cai_cli_export_prepare(pouch, id, workspace, directory, basename, &e,
                                  options, error);
  if (rc == CAI_OK)
    rc = cai_cli_export_run(e, active, NULL, NULL, destination, error);
  cai_cli_export_close(e);
  return rc;
}

typedef struct session_cleanup {
  cai_cli_pouch *pouch;
  cai_cli_pouch *messaging;
  const cai_cli_options *options;
} session_cleanup;

static int remove_session_child(const char *directory, const char *name,
                                int tree, cai_error *error) {
  int fd, rc, saved;
  char message[PATH_MAX + 256];
  if (directory == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "conversation cleanup directory is not configured");
  fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0 && errno == ENOENT)
    return CAI_OK;
  if (fd < 0)
    rc = -1;
  else {
    rc = tree ? cai_cli_remove_at(fd, name) : unlinkat(fd, name, 0);
    if (rc != 0 && errno == ENOENT)
      rc = 0;
    saved = errno;
    close(fd);
    errno = saved;
  }
  if (rc == 0)
    return CAI_OK;
  snprintf(message, sizeof(message), "remove conversation data %s/%s: %s",
           directory, name, strerror(errno));
  return cai_cli_error(error, CAI_ERR_TRANSPORT, message);
}
static int cleanup_session_files(void *context, const char *id,
                                 cai_error *error) {
  session_cleanup *cleanup = context;
  char name[CAI_AGENT_SESSION_ID_MAX + 8U], exports[PATH_MAX], *slash;
  int rc;
  if (!session_file_id(id))
    return cai_cli_error(error, CAI_ERR_INVALID, "unsafe conversation file ID");
  rc = cai_cli_messages_delete_session(cleanup->messaging, id, error);
  if (rc == CAI_OK) {
    snprintf(name, sizeof(name), "%s.log", id);
    rc = remove_session_child(cleanup->options->log_dir, name, 0, error);
  }
  if (rc == CAI_OK)
    rc = export_path(cleanup->pouch, exports, sizeof(exports),
                     cleanup->options->export_dir, id, error);
  if (rc == CAI_OK) {
    slash = strrchr(exports, '/');
    *slash = '\0';
    rc = remove_session_child(exports, id, 1, error);
  }
  return rc;
}
int cai_cli_session_delete(cai_cli_pouch *pouch, cai_cli_pouch *messaging,
                           const cai_cli_options *options,
                           const char *workspace, const char *choice,
                           char id[CAI_AGENT_SESSION_ID_MAX],
                           cai_error *error) {
  session_cleanup cleanup;
  int rc = cai_cli_session_select_delete(pouch, workspace, choice, id, error);
  cleanup.pouch = pouch;
  cleanup.messaging = messaging;
  cleanup.options = options;
  return rc == CAI_OK
             ? cai_cli_pouch_delete(pouch, workspace, id, cleanup_session_files,
                                    &cleanup, error)
             : rc;
}
