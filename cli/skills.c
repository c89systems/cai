#define _GNU_SOURCE
#include "skills.h"
#include "identity.h"
#include "log.h"
#include <cai/skills.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
static int fail(cai_error *error, const char *operation, const char *path) {
  char message[1024];
  snprintf(message, sizeof(message), "%s %s: %s", operation, path,
           strerror(errno));
  return cai_cli_error(error, CAI_ERR_TRANSPORT, message);
}
static int invalid(cai_error *error, const char *message) {
  return cai_cli_error(error, CAI_ERR_INVALID, message);
}
static int inside(const char *path, const char *directory) {
  size_t length = strlen(directory);
  return strcmp(directory, "/") == 0 ||
         (strncmp(path, directory, length) == 0 &&
          (path[length] == '\0' || path[length] == '/'));
}
static int mkdir_root(char *path, const char *source, cai_error *error) {
  char *p;
  char parent[PATH_MAX] = "/";
  struct stat st;
  int created;
  for (p = path + 1;; p++) {
    char saved = *p;
    if (saved != '/' && saved != '\0')
      continue;
    *p = '\0';
    if (lstat(path, &st) != 0) {
      if (errno != ENOENT) {
        *p = saved;
        return fail(error, "inspect skills directory", path);
      }
      if (inside(parent, source)) {
        *p = saved;
        return invalid(
            error, "skills destination must not be inside the source package");
      }
    } else if (saved == '\0' && S_ISLNK(st.st_mode)) {
      *p = saved;
      return invalid(error,
                     "skills root must be a real directory, not a symlink");
    }
    created = mkdir(path, 0700) == 0;
    if ((!created && errno != EEXIST) || (created && chmod(path, 0700) != 0)) {
      *p = saved;
      return fail(error, "create skills directory", path);
    }
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
      *p = saved;
      return invalid(error,
                     "skills directory parents must resolve to directories");
    }
    if (realpath(path, parent) == NULL) {
      *p = saved;
      return fail(error, "resolve skills directory parent", path);
    }
    *p = saved;
    if (saved == '\0')
      break;
  }
  return CAI_OK;
}

/** Per-invocation scratch belongs to the heap, including callback reentry. */
typedef struct root_path_scratch {
  char relative[PATH_MAX], cwd[PATH_MAX];
} root_path_scratch;
static int root_path_with_scratch(const cai_cli_options *options, char *out,
                                  cai_error *error,
                                  root_path_scratch *scratch) {
  const char *base;

  int n;
  if (options->skills_dir != NULL)
    n = snprintf(scratch->relative, sizeof(scratch->relative), "%s",
                 options->skills_dir);
  else if (options->config_dir != NULL)
    n = snprintf(scratch->relative, sizeof(scratch->relative), "%s/skills",
                 options->config_dir);
  else {
    base = getenv("XDG_CONFIG_HOME");
    if (base != NULL && base[0] != '\0')
      n = snprintf(scratch->relative, sizeof(scratch->relative), "%s/%s/skills",
                   base, options->program_name);
    else {
      base = getenv("HOME");
      if (base == NULL || base[0] == '\0')
        return invalid(error,
                       "set HOME or XDG_CONFIG_HOME for the skills directory");
      n = snprintf(scratch->relative, sizeof(scratch->relative),
                   "%s/.config/%s/skills", base, options->program_name);
    }
  }
  if (n < 0 || (size_t)n >= sizeof(scratch->relative))
    return invalid(error, "skills directory path exceeds PATH_MAX");
  if (scratch->relative[0] == '/')
    n = snprintf(out, PATH_MAX, "%s", scratch->relative);
  else {
    if (getcwd(scratch->cwd, sizeof(scratch->cwd)) == NULL)
      return fail(error, "resolve skills directory", scratch->relative);
    n = snprintf(out, PATH_MAX, "%s/%s", scratch->cwd, scratch->relative);
  }
  return n >= 0 && n < PATH_MAX
             ? CAI_OK
             : invalid(error, "skills directory path exceeds PATH_MAX");
}

static int root_path(const cai_cli_options *options, char *out,
                     cai_error *error) {
  root_path_scratch *scratch =
      (root_path_scratch *)calloc(1U, sizeof(*scratch));
  int rc;
  if (scratch == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate skill paths");
  rc = root_path_with_scratch(options, out, error, scratch);
  free(scratch);
  return rc;
}

static int metadata_at(int fd, const char *fallback, cai_skill_metadata **out,
                       cai_error *error) {
  FILE *file;
  cai_source *source = NULL;
  struct stat st;
  int resource, rc;
  resource =
      openat(fd, "SKILL.md", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (resource < 0)
    return fail(error, "open SKILL.md in", fallback);
  if (fstat(resource, &st) != 0 || !S_ISREG(st.st_mode)) {
    close(resource);
    return invalid(error,
                   "SKILL.md must be a regular file, not a link/device/FIFO");
  }
  file = fdopen(resource, "rb");
  if (file == NULL) {
    close(resource);
    return fail(error, "read SKILL.md in", fallback);
  }
  rc = cai_source_file(file, 1, &source, error);
  if (rc != CAI_OK)
    fclose(file);
  else {
    rc = cai_skill_metadata_open(source, fallback, out, error);
    source->close(source);
  }
  return rc;
}
/* Both cleanup and copying stay below opened directories; symlinks are never
 * followed. Copy uses bounded buffers and preserves executable artifacts. */
static int copy_file(int src, int dst, const char *name, char *bytes,
                     size_t capacity, cai_error *error) {
  ssize_t n, written;
  size_t offset;
  while ((n = read(src, bytes, capacity)) != 0) {
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return fail(error, "read skill artifact", name);
    }
    for (offset = 0U; offset < (size_t)n; offset += (size_t)written) {
      written = write(dst, bytes + offset, (size_t)n - offset);
      if (written < 0 && errno == EINTR) {
        written = 0;
        continue;
      }
      if (written <= 0)
        return fail(error, "write skill artifact", name);
    }
  }
  return CAI_OK;
}
typedef struct skill_copy_frame {
  DIR *directory;
  int destination;
} skill_copy_frame;

typedef struct skill_copy {
  skill_copy_frame frames[129];
  char bytes[65536];
} skill_copy;

static int copy_tree(int source, int destination, cai_error *error) {
  skill_copy *copy;
  skill_copy_frame *frames, *frame;
  struct dirent *entry;
  struct stat st;
  unsigned int depth = 0U;
  int src, dst, rc = CAI_OK;
  copy = calloc(1U, sizeof(*copy));
  if (copy == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate skill traversal");
  frames = copy->frames;
  src = dup(source);
  frames[0].directory = src >= 0 ? fdopendir(src) : NULL;
  if (frames[0].directory == NULL && src >= 0)
    close(src);
  frames[0].destination = dup(destination);
  if (frames[0].directory == NULL || frames[0].destination < 0)
    rc = fail(error, "list skill source", "directory");
  while (rc == CAI_OK) {
    frame = &frames[depth];
    source = dirfd(frame->directory);
    destination = frame->destination;
    errno = 0;
    entry = readdir(frame->directory);
    if (entry == NULL) {
      if (errno != 0)
        rc = fail(error, "list skill source", "directory");
      if (rc != CAI_OK || depth == 0U)
        break;
      closedir(frame->directory);
      close(frame->destination);
      memset(frame, 0, sizeof(*frame));
      depth--;
      continue;
    }
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    if (fstatat(source, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
      rc = fail(error, "inspect skill artifact", entry->d_name);
      break;
    }
    if (S_ISDIR(st.st_mode)) {
      if (depth == 128U) {
        rc = invalid(error, "skill artifact tree exceeds 128 directory levels");
        break;
      }
      if (mkdirat(destination, entry->d_name, 0700) != 0) {
        rc = fail(error, "create skill artifact directory", entry->d_name);
        break;
      }
      src = openat(source, entry->d_name,
                   O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      dst = openat(destination, entry->d_name,
                   O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      if (src < 0 || dst < 0 || fchmod(dst, 0700) != 0)
        rc = fail(error, "open skill artifact directory", entry->d_name);
      else {
        DIR *child = fdopendir(src);
        if (child == NULL)
          rc = fail(error, "list skill source", entry->d_name);
        else {
          depth++;
          frames[depth].directory = child;
          frames[depth].destination = dst;
          src = dst = -1;
        }
      }
      if (src >= 0)
        close(src);
      if (dst >= 0)
        close(dst);
    } else if (S_ISREG(st.st_mode)) {
      src = openat(source, entry->d_name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
      dst = openat(destination, entry->d_name,
                   O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
      if (src < 0 || dst < 0 ||
          fchmod(dst, 0600 | ((st.st_mode & 0111U) ? 0100 : 0)) != 0)
        rc = fail(error, "open skill artifact", entry->d_name);
      else
        rc = copy_file(src, dst, entry->d_name, copy->bytes,
                       sizeof(copy->bytes), error);
      if (src >= 0)
        close(src);
      if (dst >= 0 && close(dst) != 0 && rc == CAI_OK)
        rc = fail(error, "close skill artifact", entry->d_name);
    } else
      rc = invalid(error, "skill packages may contain only regular files and "
                          "directories; symlinks/devices/FIFOs are rejected");
  }
  do {
    if (frames[depth].directory != NULL)
      closedir(frames[depth].directory);
    if (frames[depth].destination >= 0)
      close(frames[depth].destination);
  } while (depth-- != 0U);
  free(copy);
  return rc;
}
static int check_existing_name(int fd, const char *name, const char *except,
                               unsigned int depth, cai_error *error) {
  DIR **directories;
  struct dirent *entry;
  struct stat st;
  cai_skill_metadata *metadata = NULL;
  cai_error ignored;
  int child, rc = CAI_OK;
  directories = calloc(9U, sizeof(*directories));
  if (directories == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate skill name scan");
  child = dup(fd);
  directories[depth] = child >= 0 ? fdopendir(child) : NULL;
  if (directories[depth] == NULL && child >= 0)
    close(child);
  if (directories[depth] == NULL)
    rc = fail(error, "list installed skills", "directory");
  while (rc == CAI_OK) {
    errno = 0;
    entry = readdir(directories[depth]);
    if (entry == NULL) {
      if (errno != 0)
        rc = fail(error, "list installed skills", "directory");
      if (rc != CAI_OK || depth == 0U)
        break;
      closedir(directories[depth]);
      directories[depth--] = NULL;
      continue;
    }
    fd = dirfd(directories[depth]);
    if (entry->d_name[0] == '.' ||
        (depth == 0U && except != NULL && strcmp(entry->d_name, except) == 0) ||
        fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISDIR(st.st_mode))
      continue;
    child = openat(fd, entry->d_name,
                   O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (child < 0)
      continue;
    if (fstatat(child, "SKILL.md", &st, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISREG(st.st_mode)) {
      cai_error_init(&ignored);
      if (metadata_at(child, entry->d_name, &metadata, &ignored) == CAI_OK &&
          strcmp(metadata->name(metadata), name) == 0)
        rc = invalid(error, "skill name already belongs to another installed "
                            "package directory");
      cai_skill_metadata_close(metadata);
      metadata = NULL;
      cai_error_cleanup(&ignored);
    } else if (depth < 8U) {
      directories[depth + 1U] = fdopendir(child);
      if (directories[depth + 1U] != NULL) {
        child = -1;
        depth++;
      } else
        rc = fail(error, "list installed skills", "directory");
    }
    if (child >= 0)
      close(child);
  }
  do {
    if (directories[depth] != NULL)
      closedir(directories[depth]);
  } while (depth-- != 0U);
  free(directories);
  return rc;
}

/** Per-invocation scratch belongs to the heap, including callback reentry. */
typedef struct cai_cli_install_skill_scratch {
  char source_path[PATH_MAX], root[PATH_MAX], canonical[PATH_MAX],
      temporary[PATH_MAX];
} cai_cli_install_skill_scratch;
static int
cai_cli_install_skill_with_scratch(const cai_cli_options *options,
                                   pslog_logger *logger,
                                   cai_cli_install_skill_scratch *scratch) {
  cai_error error;
  cai_skill_metadata *metadata = NULL;
  struct stat st;

  const char *name, *stage;
  int source = -1, parent = -1, destination = -1, rc, exists = 0;
  cai_error_init(&error);
  scratch->temporary[0] = '\0';
  if (lstat(options->install_skill, &st) != 0 || !S_ISDIR(st.st_mode)) {
    rc = invalid(
        &error,
        "--install-skill requires a real directory containing SKILL.md");
    goto done;
  }
  if (realpath(options->install_skill, scratch->source_path) == NULL) {
    rc = fail(&error, "resolve skill source", options->install_skill);
    goto done;
  }
  source = open(scratch->source_path,
                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (source < 0) {
    rc = fail(&error, "open skill source", scratch->source_path);
    goto done;
  }
  rc = metadata_at(source, scratch->source_path, &metadata, &error);
  if (rc != CAI_OK)
    goto done;
  name = metadata->name(metadata);
  if (name[0] == '.') {
    rc = invalid(&error, "installed skill names must not begin with '.'");
    goto done;
  }
  rc = root_path(options, scratch->root, &error);
  if (rc != CAI_OK)
    goto done;
  rc = mkdir_root(scratch->root, scratch->source_path, &error);
  if (rc != CAI_OK)
    goto done;
  if (realpath(scratch->root, scratch->canonical) == NULL) {
    rc = fail(&error, "resolve skills directory", scratch->root);
    goto done;
  }
  if (snprintf(scratch->temporary, sizeof(scratch->temporary), "%s/%s",
               scratch->canonical, name) >= (int)sizeof(scratch->temporary)) {
    scratch->temporary[0] = '\0';
    rc = invalid(&error, "installed skill path exceeds PATH_MAX");
    goto done;
  }
  if (strcmp(scratch->temporary, scratch->source_path) == 0) {
    scratch->temporary[0] = '\0';
    rc = invalid(
        &error,
        "source and installed skill destination are the same directory");
    goto done;
  }
  scratch->temporary[0] = '\0';
  if (inside(scratch->canonical, scratch->source_path)) {
    rc = invalid(&error,
                 "skills destination must not be inside the source package");
    goto done;
  }
  parent =
      open(scratch->canonical, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (parent < 0) {
    rc = fail(&error, "open skills directory", scratch->canonical);
    goto done;
  }
  if (fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
    exists = 1;
    if (!options->force) {
      rc = invalid(&error,
                   "skill already exists; use --force or --yes to replace it");
      goto done;
    }
    if (!S_ISDIR(st.st_mode)) {
      rc = invalid(&error,
                   "refusing to replace a non-directory skill destination");
      goto done;
    }
    destination =
        openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (destination < 0 ||
        fstatat(destination, "SKILL.md", &st, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(st.st_mode)) {
      rc = invalid(
          &error, "refusing to replace a directory without a regular SKILL.md");
      goto done;
    }
    close(destination);
    destination = -1;
  } else if (errno != ENOENT) {
    rc = fail(&error, "inspect installed skill", name);
    goto done;
  }
  rc = check_existing_name(parent, name, exists ? name : NULL, 0U, &error);
  if (rc != CAI_OK)
    goto done;
  if (snprintf(scratch->temporary, sizeof(scratch->temporary),
               "%s/.skill-install-XXXXXX",
               scratch->canonical) >= (int)sizeof(scratch->temporary) ||
      mkdtemp(scratch->temporary) == NULL) {
    scratch->temporary[0] = '\0';
    rc = fail(&error, "create skill installation staging directory",
              scratch->canonical);
    goto done;
  }
  stage = strrchr(scratch->temporary, '/') + 1;
  destination =
      openat(parent, stage, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (destination < 0 || fchmod(destination, 0700) != 0) {
    rc = fail(&error, "open skill staging directory", scratch->canonical);
    goto done;
  }
  rc = copy_tree(source, destination, &error);
  if (rc != CAI_OK)
    goto done;
  close(destination);
  destination = -1;
  if (renameat2(parent, stage, parent, name,
                exists ? RENAME_EXCHANGE : RENAME_NOREPLACE) != 0) {
    rc = fail(&error, "publish skill (atomic rename required)", name);
    goto done;
  }
  if (exists && cai_cli_remove_at(parent, stage) != 0)
    cai_cli_log_message(logger, PSLOG_LEVEL_WARN, "skill_install_cleanup",
                        "diagnostic",
                        "Skill replaced; the previous package could not be "
                        "removed from the hidden staging directory.",
                        strlen("Skill replaced; the previous package could not "
                               "be removed from the hidden staging directory."),
                        NULL, 0U);
  scratch->temporary[0] = '\0';
  {
    pslog_field fields[3];
    fields[0] = pslog_str("skill", name);
    fields[1] = pslog_str("directory", scratch->canonical);
    fields[2] = pslog_bool("replaced", exists);
    cai_cli_log_message(logger, PSLOG_LEVEL_INFO, "skill_install", "diagnostic",
                        "Skill package installed",
                        strlen("Skill package installed"), fields, 3U);
  }
  printf("%s skill %s at %s/%s\n", exists ? "Replaced" : "Installed", name,
         scratch->canonical, name);
  rc = CAI_OK;
done:
  if (destination >= 0)
    close(destination);
  if (scratch->temporary[0] != '\0' && parent >= 0)
    (void)cai_cli_remove_at(parent, strrchr(scratch->temporary, '/') + 1);
  if (source >= 0)
    close(source);
  if (parent >= 0)
    close(parent);
  if (rc != CAI_OK)
    cai_cli_log_error(logger, "install skill", &error);
  cai_skill_metadata_close(metadata);
  cai_error_cleanup(&error);
  return rc == CAI_OK ? 0 : 1;
}

int cai_cli_install_skill(const cai_cli_options *options,
                          pslog_logger *logger) {
  cai_cli_install_skill_scratch *scratch =
      (cai_cli_install_skill_scratch *)calloc(1U, sizeof(*scratch));
  int rc;
  if (scratch == NULL) {
    cai_error failure;
    cai_error_init(&failure);
    (void)cai_cli_error(&failure, CAI_ERR_NOMEM,
                        "allocate skill installation paths");
    cai_cli_log_error(logger, "install skill", &failure);
    cai_error_cleanup(&failure);
    return 1;
  }
  rc = cai_cli_install_skill_with_scratch(options, logger, scratch);
  free(scratch);
  return rc;
}

typedef struct skill_inventory {
  int (*write)(void *, const char *, size_t);
  void *context;
  size_t count;
} skill_inventory;
static int skill_text(skill_inventory *out, const char *text,
                      cai_error *error) {
  return out->write(out->context, text, strlen(text)) == 0
             ? CAI_OK
             : cai_cli_error(error, CAI_ERR_TRANSPORT, "write skill inventory");
}
static int skill_cell(skill_inventory *out, const char *text,
                      cai_error *error) {
  char bytes[256];
  size_t used = 0U;
  const unsigned char *p = (const unsigned char *)text;
  for (; *p; p++) {
    if (used + 2U > sizeof(bytes)) {
      if (out->write(out->context, bytes, used) != 0)
        return cai_cli_error(error, CAI_ERR_TRANSPORT, "write skill inventory");
      used = 0U;
    }
    if (strchr("\\|`*_~[]<>!#", *p) != NULL)
      bytes[used++] = '\\';
    bytes[used++] = *p < 32U || *p == 127U ? ' ' : (char)*p;
  }
  if (used != 0U && out->write(out->context, bytes, used) != 0)
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "write skill inventory");
  return CAI_OK;
}
static int skill_row(void *context, const cai_skill_info *skill,
                     cai_error *error) {
  skill_inventory *out = context;
  int rc = CAI_OK;
  if (out->count++ == 0U)
    rc = skill_text(out, "| Skill | Description |\n| --- | --- |\n", error);
  if (rc == CAI_OK)
    rc = skill_text(out, "| **", error);
  if (rc == CAI_OK)
    rc = skill_cell(out, skill->name, error);
  if (rc == CAI_OK)
    rc = skill_text(out, "** | ", error);
  if (rc == CAI_OK)
    rc = skill_cell(out, skill->description, error);
  return rc == CAI_OK ? skill_text(out, " |\n", error) : rc;
}
int cai_cli_skills_markdown(cai_agent_runtime *runtime, int enabled,
                            int (*write)(void *, const char *, size_t),
                            void *context, cai_error *error) {
  skill_inventory out;
  int rc;
  if (write == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "skill inventory writer is required");
  out.write = write;
  out.context = context;
  out.count = 0U;
  rc = skill_text(&out, "## Skills\n\n", error);
  if (rc == CAI_OK && !enabled)
    return skill_text(
        &out,
        "Skills are disabled. Start with `--skills` to enable discovery.\n",
        error);
  if (rc == CAI_OK)
    rc = cai_agent_runtime_visit_skills(runtime, skill_row, &out, error);
  if (rc != CAI_OK)
    return rc;
  if (out.count == 0U)
    return skill_text(&out,
                      "No skills loaded. Install a package with "
                      "`--install-skill DIR`, then restart.\n",
                      error);
  return skill_text(&out,
                    "\nName a listed skill in your prompt, for example: "
                    "`Use the SKILL-NAME skill for this task.`\n",
                    error);
}
