#define _POSIX_C_SOURCE 200809L
#include "identity.h"
#include "pouch_stream.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct cai_remove_frame {
  struct cai_remove_frame *parent;
  DIR *directory;
  int failed;
  char *name;
} cai_remove_frame;

static void remove_failure(cai_remove_frame *frame, int *saved) {
  if (*saved == 0)
    *saved = errno != 0 ? errno : EIO;
  if (frame != NULL)
    frame->failed = 1;
}

/** Postorder cleanup owns directory cursors on the heap, including rollback
 * of rejected deep skill packages and temporary workspaces. Never follows
 * links. */
int cai_cli_remove_at(int parent, const char *name) {
  cai_remove_frame *top = NULL, *frame;
  const char *next = name;
  struct dirent *entry;
  struct stat st;
  int fd, base = parent, saved = 0;
  if (name == NULL || name[0] == '\0' || strchr(name, '/') != NULL ||
      strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
    errno = EINVAL;
    return -1;
  }
  for (;;) {
    if (next != NULL) {
      if (fstatat(base, next, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno != ENOENT)
          remove_failure(top, &saved);
      } else if (!S_ISDIR(st.st_mode)) {
        if (unlinkat(base, next, 0) != 0)
          remove_failure(top, &saved);
      } else {
        frame = calloc(1U, sizeof(*frame));
        if (frame == NULL) {
          remove_failure(top, &saved);
          break;
        }
        frame->name = strdup(next);
        if (frame->name == NULL) {
          remove_failure(top, &saved);
          free(frame);
          break;
        }
        fd =
            openat(base, next, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0 || fchmod(fd, 0700) != 0 ||
            (frame->directory = fdopendir(fd)) == NULL) {
          remove_failure(top, &saved);
          if (fd >= 0)
            close(fd);
          free(frame->name);
          free(frame);
        } else {
          frame->parent = top;
          top = frame;
        }
      }
    }
    next = NULL;
    if (top == NULL)
      break;
    errno = 0;
    entry = readdir(top->directory);
    if (entry != NULL) {
      if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
        next = entry->d_name;
        base = dirfd(top->directory);
      }
      continue;
    }
    if (errno != 0)
      remove_failure(top, &saved);
    frame = top;
    top = frame->parent;
    if (closedir(frame->directory) != 0)
      remove_failure(frame, &saved);
    base = top != NULL ? dirfd(top->directory) : parent;
    if (!frame->failed && unlinkat(base, frame->name, AT_REMOVEDIR) != 0)
      remove_failure(frame, &saved);
    if (frame->failed && top != NULL)
      top->failed = 1;
    free(frame->name);
    free(frame);
  }
  while (top != NULL) {
    frame = top;
    top = frame->parent;
    closedir(frame->directory);
    free(frame->name);
    free(frame);
  }
  if (saved != 0)
    errno = saved;
  return saved != 0 ? -1 : 0;
}

int cai_cli_identity_valid(const char *name) {
  return name != NULL && name[0] >= 'a' && name[0] <= 'z' &&
         strlen(name) <= 32U &&
         strspn(name, "abcdefghijklmnopqrstuvwxyz0123456789_-") == strlen(name);
}
int cai_cli_environment_prefix(const cai_app_config *app, char out[65],
                               cai_error *error) {
  const char *name = app != NULL && app->program_name != NULL
                         ? app->program_name
                         : CAI_APP_DEFAULT_PROGRAM_NAME;
  size_t i, n;
  if (!cai_cli_identity_valid(name))
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "invalid application program_name");
  if (app != NULL && app->environment_prefix != NULL) {
    name = app->environment_prefix;
    n = strlen(name);
    if (n == 0U || n > 64U || name[n - 1U] != '_' ||
        (name[0] >= '0' && name[0] <= '9') ||
        strspn(name, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz01234"
                     "56789_") != n)
      return cai_cli_error(
          error, CAI_ERR_INVALID,
          "environment_prefix must be a variable prefix ending in '_'");
    strcpy(out, name);
  } else {
    n = strlen(name);
    for (i = 0U; i < n; i++)
      out[i] = name[i] == '-'                     ? '_'
               : name[i] >= 'a' && name[i] <= 'z' ? (char)(name[i] - 'a' + 'A')
                                                  : name[i];
    out[n++] = '_';
    out[n] = '\0';
  }
  return CAI_OK;
}
int cai_cli_xdg_path(char *out, size_t size, const char *variable,
                     const char *fallback, const char *name, cai_error *error) {
  return cai_cli_xdg_path_values(out, size, getenv(variable), getenv("HOME"),
                                 fallback, name, error);
}
int cai_cli_xdg_path_values(char *out, size_t size, const char *base,
                            const char *home, const char *fallback,
                            const char *name, cai_error *error) {
  int n;
  if (base != NULL && base[0] == '/')
    n = snprintf(out, size, "%s/%s", base, name);
  else {
    base = home;
    if (base == NULL || base[0] != '/')
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "set HOME or an absolute XDG root");
    n = snprintf(out, size, "%s/%s/%s", base, fallback, name);
  }
  return n < 0 || (size_t)n >= size
             ? cai_cli_error(error, CAI_ERR_INVALID,
                             "application path is too long")
             : CAI_OK;
}
int cai_cli_workspace_path(char *out, size_t size, const char *workspace,
                           const char *path, cai_error *error) {
  int n;
  if (out == NULL || size == 0U || workspace == NULL || workspace[0] != '/' ||
      path == NULL || path[0] == '\0')
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "filename and absolute workspace are required");
  n = path[0] == '/' ? snprintf(out, size, "%s", path)
                     : snprintf(out, size, "%s/%s", workspace, path);
  return n >= 0 && (size_t)n < size
             ? CAI_OK
             : cai_cli_error(error, CAI_ERR_INVALID,
                             "workspace filename is too long");
}
int cai_cli_private_directory(const char *path, cai_error *error) {
  char copy[4096], *cursor;
  struct stat status;
  if (path == NULL || path[0] != '/' || strlen(path) >= sizeof(copy))
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "private directory must be an absolute path");
  strcpy(copy, path);
  for (cursor = copy + 1;; cursor++) {
    if (*cursor == '/' || *cursor == '\0') {
      char saved = *cursor;
      *cursor = '\0';
      if (mkdir(copy, 0700) != 0 && errno != EEXIST)
        return cai_cli_error(error, CAI_ERR_TRANSPORT,
                             "create private application directory");
      *cursor = saved;
      if (saved == '\0')
        break;
    }
  }
  if (lstat(copy, &status) != 0 || !S_ISDIR(status.st_mode) ||
      status.st_uid != geteuid() || (status.st_mode & 077U) != 0U)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "application directory must be owned by you with mode "
                         "0700 and must not be a symlink");
  return CAI_OK;
}
