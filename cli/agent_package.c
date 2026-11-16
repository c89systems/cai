/** @file Agent source publication, revision caches and native credentials.
 * Attachments flow through bounded sources; directory traversal uses heap
 * cursors. Metadata and credentials have ownership independent of file bytes.
 */
#define _GNU_SOURCE
#include "agent_package.h"
#include "config.h"
#include "editor.h"
#include "identity.h"
#include "instructions.h"
#include "stores.h"
#include <cai/auth.h>
#include <cai/plugins.h>
#include <cai/smith.h>
#ifdef CAI_APP_LUA_TOOLS
#include <cai/lua_tools.h>
#endif
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Reuse the pinned coding template; the example replaces its model-specific
 * opening and resolves the identity with the ordinary instruction compiler. */
static const unsigned char example_template[] = {
#include "cai_smith_gpt_6_1_sol_prompt.inc"
};

typedef struct agent_file {
  char *path, *hash;
  long long mode;
} agent_file;
typedef struct agent_record {
  char kind[16], name[65], revision[21];
  lonejson_object_array files;
  lonejson_json_value authentication;
  lonejson *runtime;
} agent_record;
static const lonejson_field file_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(agent_file, path, "path"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(agent_file, hash, "sha256"),
    LONEJSON_FIELD_I64(agent_file, mode, "mode")};
LONEJSON_MAP_DEFINE(file_map, agent_file, file_fields);
static const lonejson_field record_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(agent_record, kind, "kind",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED_REQ(agent_record, name, "name",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED(agent_record, revision, "revision",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_OBJECT_ARRAY(agent_record, files, "files", agent_file,
                                &file_map, LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_JSON_VALUE_OMIT_NULL(agent_record, authentication,
                                        "authentication")};
LONEJSON_MAP_DEFINE(record_map, agent_record, record_fields);
static const lonejson_field auth_fields[] = {
    LONEJSON_FIELD_JSON_VALUE_OMIT_NULL(agent_record, authentication,
                                        "authentication")};
LONEJSON_MAP_DEFINE(auth_map, agent_record, auth_fields);

/** Metadata is bounded independently of attachment sizes. File bytes never
 * enter this record or a complete-document staging buffer. */
typedef struct agent_package {
  cai_cli_pouch repository;
  cai_cli_options store;
  agent_record record;
  cai_config *configuration;
  char *directory;
  int root;
  const char *auth_file;
  const cai_cli_options *options;
  const char *export_root;
  int global_text;
  int editing;
  char expected_revision[21], expected_name[65];
} agent_package;

static int invalid(cai_error *error, const char *message) {
  return cai_cli_error(error, CAI_ERR_INVALID, message);
}

int cai_cli_agent_name(const char *input, char out[65], cai_error *error) {
  size_t used = 0U;
  const unsigned char *p = (const unsigned char *)input;
  out[0] = '\0';
  if (p == NULL)
    return invalid(error, "agent name is required");
  for (; *p != '\0'; ++p) {
    unsigned char c = *p;
    if (c >= 'A' && c <= 'Z')
      c = (unsigned char)(c + ('a' - 'A'));
    if (c == ' ')
      c = '_';
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
          c == '-'))
      continue;
    if (used == 64U)
      return invalid(error, "canonical agent name exceeds 64 bytes");
    out[used++] = (char)c;
  }
  out[used] = '\0';
  return used != 0U ? CAI_OK
                    : invalid(error, "agent name has no usable characters");
}

static int safe_path(const char *path) {
  const char *part = path, *p;
  size_t n;
  if (path == NULL || *path == '\0' || *path == '/' || strlen(path) >= PATH_MAX)
    return 0;
  for (p = path;; ++p) {
    if ((unsigned char)*p < 32U && *p != '\0')
      return 0;
    if (*p != '/' && *p != '\0')
      continue;
    n = (size_t)(p - part);
    if (n == 0U || (n == 1U && part[0] == '.') ||
        (n == 2U && part[0] == '.' && part[1] == '.'))
      return 0;
    if (*p == '\0')
      return 1;
    part = p + 1;
  }
}

/** Descend using directory descriptors; source symlinks cannot escape the
 * package, and output parents cannot redirect writes outside the destination.
 */
static int open_path(int root, const char *path, int flags, mode_t mode,
                     int parents, cai_error *error) {
  char *copy = NULL, *part, *next;
  int current = -1, opened = -1;
  if (!safe_path(path)) {
    invalid(error, "invalid agent resource path");
    return -1;
  }
  copy = strdup(path);
  current = dup(root);
  if (copy == NULL || current < 0)
    goto done;
  part = copy;
  while ((next = strchr(part, '/')) != NULL) {
    *next = '\0';
    if (parents && mkdirat(current, part, 0700) != 0 && errno != EEXIST)
      goto done;
    opened =
        openat(current, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (opened < 0)
      goto done;
    close(current);
    current = opened;
    opened = -1;
    part = next + 1;
  }
  opened =
      openat(current, part, flags | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, mode);
done:
  if (current >= 0)
    close(current);
  free(copy);
  if (opened < 0)
    cai_cli_error(error, CAI_ERR_TRANSPORT, "cannot open agent resource");
  return opened;
}

static int record_open(agent_record *record, cai_error *error) {
  lonejson_error local;
  lonejson_config config = lonejson_default_config();
  lonejson *json;
  config.clear_destination_by_default = 0;
  /* Native authentication is an intentionally retained small JSON value,
   * independent of streamed config/resources. Never capture arbitrary files. */
  config.json_value_max_total_bytes = 1024U * 1024U;
  config.json_value_max_string_bytes = 256U * 1024U;
  json = lonejson_new(&config, &local);
  memset(record, 0, sizeof(*record));
  if (json == NULL ||
      json->init(json, &record_map, record, &local) != LONEJSON_STATUS_OK) {
    lonejson_free(json);
    return cai_cli_lj_error(error, &local);
  }
  record->runtime = json;
  if (lonejson_json_value_enable_parse_capture(&record->authentication,
                                               &local) != LONEJSON_STATUS_OK) {
    json->cleanup(json, &record_map, record, NULL);
    lonejson_free(json);
    record->runtime = NULL;
    return cai_cli_lj_error(error, &local);
  }
  return CAI_OK;
}

static void record_close(agent_record *record) {
  lonejson *json = record->runtime;
  if (json != NULL)
    json->cleanup(json, &record_map, record, NULL);
  lonejson_free(json);
  memset(record, 0, sizeof(*record));
}

static int read_record(cai_cli_pouch *pouch, const char *name,
                       agent_record *record, cai_error *error) {
  cai_source *source = NULL;
  lonejson_error local;
  lonejson *json = record->runtime;
  int rc;
  json->cleanup(json, &record_map, record, NULL);
  rc = json->init(json, &record_map, record, &local) == LONEJSON_STATUS_OK
           ? CAI_OK
           : cai_cli_lj_error(error, &local);
  record->runtime = json;
  if (rc == CAI_OK &&
      lonejson_json_value_enable_parse_capture(&record->authentication,
                                               &local) != LONEJSON_STATUS_OK)
    rc = cai_cli_lj_error(error, &local);
  if (rc == CAI_OK)
    rc = cai_cli_pouch_agent_read(pouch, name, NULL, &source, error);
  if (rc == CAI_OK)
    rc = cai_cli_stream_ready(source, error);
  if (rc == CAI_OK &&
      lonejson_parse_reader(json, &record_map, record, cai_cli_json_read,
                            source, &local) != LONEJSON_STATUS_OK)
    rc = cai_cli_lj_error(error, &local);
  if (rc == CAI_OK &&
      (strcmp(record->kind, "agent") || strcmp(record->name, name)))
    rc = invalid(error, "stored agent metadata does not match its name");
  cai_source_close(source);
  record->runtime = json;
  return rc;
}

static int write_metadata(lc_lease *lease, agent_record *record,
                          cai_error *error) {
  cai_source *source = NULL;
  lc_source *input = NULL;
  lc_error local;
  int rc = cai_cli_mapped_source(&record_map, record, &source, error);
  lc_error_init(&local);
  if (rc == CAI_OK)
    rc = cai_cli_lc_source(source, &input, error);
  if (rc == CAI_OK && lc_lease_update(lease, input, NULL, &local) != LC_OK)
    rc = cai_cli_lc_error(error, &local);
  lc_source_close(input);
  cai_source_close(source);
  lc_error_cleanup(&local);
  return rc;
}

typedef struct verified_source {
  cai_source *source;
  EVP_MD_CTX *hash;
  char expected[65], actual[65];
  int finished;
} verified_source;
static size_t verified_read(void *context, void *bytes, size_t count,
                            cai_error *error) {
  verified_source *v = context;
  size_t n;
  size_t i;
  if (count == 0U)
    return 0U;
  n = cai_source_read(v->source, bytes, count, error);
  if (n != 0U) {
    if (EVP_DigestUpdate(v->hash, bytes, n) != 1)
      cai_cli_error(error, CAI_ERR_TRANSPORT, "hash agent configuration");
  } else if (!v->finished && error->code == CAI_OK) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int size;
    char hex[65];
    v->finished = 1;
    if (EVP_DigestFinal_ex(v->hash, digest, &size) != 1 || size != 32U)
      cai_cli_error(error, CAI_ERR_TRANSPORT,
                    "finish agent configuration digest");
    else {
      for (i = 0U; i < 32U; ++i)
        snprintf(hex + i * 2U, 3U, "%02x", (unsigned int)digest[i]);
      memcpy(v->actual, hex, sizeof(hex));
      if (v->expected[0] != '\0' && strcmp(hex, v->expected))
        invalid(error, "agent configuration changed during loading; retry");
    }
  }
  return error->code == CAI_OK ? n : 0U;
}
static void verified_close(void *context) {
  verified_source *v = context;
  cai_source_close(v->source);
  EVP_MD_CTX_free(v->hash);
  free(v);
}
static int authentication_write(void *context, cai_sink *sink,
                                cai_error *error) {
  agent_record *record = context;
  lonejson_error local;
  return lonejson_json_value_write_to_sink(&record->authentication,
                                           cai_cli_json_write, sink,
                                           &local) == LONEJSON_STATUS_OK
             ? CAI_OK
             : cai_cli_lj_error(error, &local);
}
static void authentication_close(void *context) {
  agent_record *record = context;
  record_close(record);
  free(record);
}
static int authentication_load(void *context, const char *key, cai_source **out,
                               cai_error *error) {
  cai_cli_agent_auth *auth = context;
  agent_record *record = calloc(1U, sizeof(*record));
  int rc;
  *out = NULL;
  if (record == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate agent credentials");
  rc = strcmp(key, "auth.json") == 0
           ? record_open(record, error)
           : invalid(error, "agent credentials support only auth.json");
  if (rc == CAI_OK)
    rc = read_record(auth->pouch, auth->name, record, error);
  if (rc == CAI_OK && record->authentication.kind != LONEJSON_JSON_VALUE_NULL) {
    rc = cai_cli_stream_source(authentication_write, record,
                               authentication_close, out, error);
    if (rc == CAI_OK)
      return rc;
  }
  authentication_close(record);
  return rc;
}
typedef struct authentication_update {
  cai_cli_agent_auth *auth;
  cai_source *value;
} authentication_update;
static int authentication_publish(void *context, lc_lease *lease,
                                  cai_error *error) {
  authentication_update *update = context;
  agent_record *record = calloc(1U, sizeof(*record));
  cai_source *previous = NULL;
  lonejson_error local;
  int rc;
  if (record == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate credential update");
  rc = record_open(record, error);
  if (rc == CAI_OK)
    rc = cai_cli_pouch_agent_private(lease, &previous, error);
  if (rc == CAI_OK && lonejson_parse_reader(record->runtime, &record_map,
                                            record, cai_cli_json_read, previous,
                                            &local) != LONEJSON_STATUS_OK)
    rc = cai_cli_lj_error(error, &local);
  if (rc == CAI_OK && (strcmp(record->kind, "agent") != 0 ||
                       strcmp(record->name, update->auth->name) != 0))
    rc = invalid(error, "credential update requires an existing agent");
  if (rc == CAI_OK && lonejson_json_value_set_reader(
                          &record->authentication, cai_cli_json_read,
                          update->value, &local) != LONEJSON_STATUS_OK)
    rc = cai_cli_lj_error(error, &local);
  if (rc == CAI_OK)
    rc = write_metadata(lease, record, error);
  cai_source_close(previous);
  authentication_close(record);
  return rc;
}
static int authentication_replace(void *context, const char *key,
                                  cai_source *value, cai_error *error) {
  cai_cli_agent_auth *auth = context;
  authentication_update update;
  if (strcmp(key, "auth.json") != 0)
    return invalid(error, "agent credentials support only auth.json");
  update.auth = auth;
  update.value = value;
  return cai_cli_pouch_agent_publish(auth->pouch, auth->name, 1,
                                     authentication_publish, &update, error);
}
int cai_cli_agent_auth_init(cai_cli_agent_auth *auth, cai_cli_pouch *pouch,
                            const char *name, cai_error *error) {
  int rc;
  memset(auth, 0, sizeof(*auth));
  rc = cai_cli_agent_name(name, auth->name, error);
  if (rc == CAI_OK) {
    auth->pouch = pouch;
    auth->storage.context = auth;
    auth->storage.load = authentication_load;
    auth->storage.replace = authentication_replace;
  }
  return rc;
}

static int append_file(agent_record *record, const char *path, const char *hash,
                       long long mode, cai_error *error) {
  lonejson *json = record->runtime;
  lonejson_error local;
  agent_file *file;
  size_t i;
  for (i = 0U; i < record->files.count; ++i) {
    file = (agent_file *)record->files.items + i;
    if (strcmp(file->path, path) == 0) {
      if (json->record_assign_string(json, &file_map, file, &file_fields[1],
                                     hash, strlen(hash),
                                     &local) != LONEJSON_STATUS_OK)
        return cai_cli_lj_error(error, &local);
      file->mode = mode;
      return CAI_OK;
    }
  }
  if (record->files.count >= 8192U)
    return invalid(error, "agent package exceeds 8192 resources");
  file = json->record_object_array_append_slot(
      json, &record_map, record, &record_fields[3], &record->files, &local);
  if (file == NULL)
    return cai_cli_lj_error(error, &local);
  if (json->init(json, &file_map, file, &local) != LONEJSON_STATUS_OK ||
      json->record_assign_string(json, &file_map, file, &file_fields[0], path,
                                 strlen(path), &local) != LONEJSON_STATUS_OK ||
      json->record_assign_string(json, &file_map, file, &file_fields[1], hash,
                                 strlen(hash), &local) != LONEJSON_STATUS_OK)
    return cai_cli_lj_error(error, &local);
  file->mode = mode;
  return CAI_OK;
}

/** Own source on every return; hash during the actual upload. */
static int attach_source(agent_package *package, lc_lease *lease,
                         const char *path, cai_source *source, long long mode,
                         cai_error *error) {
  lc_source *input = NULL;
  lc_attach_req request;
  lc_attach_res result = {0};
  verified_source *digest = NULL;
  cai_source *hashed = NULL;
  cai_source_callbacks callbacks = {0};
  lc_error local;
  int rc = CAI_OK;
  lc_error_init(&local);
  lc_attach_req_init(&request);
  request.name = path;
  request.content_type = "application/octet-stream";
  if (rc == CAI_OK) {
    digest = calloc(1U, sizeof(*digest));
    if (digest == NULL)
      rc = cai_cli_error(error, CAI_ERR_NOMEM, "allocate resource digest");
  }
  if (rc == CAI_OK) {
    digest->source = source;
    source = NULL;
    digest->hash = EVP_MD_CTX_new();
    if (digest->hash == NULL ||
        EVP_DigestInit_ex(digest->hash, EVP_sha256(), NULL) != 1)
      rc = cai_cli_error(error, CAI_ERR_NOMEM, "initialize resource digest");
    callbacks.context = digest;
    callbacks.read = verified_read;
    callbacks.close = verified_close;
    if (rc == CAI_OK)
      rc = cai_source_from_callbacks(&callbacks, &hashed, error);
  }
  if (rc == CAI_OK)
    rc = cai_cli_lc_source(hashed, &input, error);
  if (rc == CAI_OK &&
      lc_lease_attach(lease, &request, input, &result, &local) != LC_OK)
    rc = cai_cli_lc_error(error, &local);
  if (rc == CAI_OK && !digest->finished)
    rc = invalid(error, "attachment upload did not consume its resource");
  if (rc == CAI_OK && result.attachment.plaintext_sha256 != NULL &&
      strcmp(digest->actual, result.attachment.plaintext_sha256) != 0)
    rc = invalid(error, "attachment upload digest mismatch");
  if (rc == CAI_OK)
    rc = append_file(&package->record, path, digest->actual, mode, error);
  lc_source_close(input);
  if (hashed != NULL)
    cai_source_close(hashed);
  else if (digest != NULL)
    verified_close(digest);
  cai_source_close(source);
  lc_attach_res_cleanup(&result);
  lc_error_cleanup(&local);
  return rc;
}

static int attach_file_as(agent_package *package, lc_lease *lease,
                          const char *path, const char *name,
                          cai_error *error) {
  struct stat status;
  FILE *file = NULL;
  cai_source *source = NULL;
  int fd = open_path(package->root, path, O_RDONLY, 0, 0, error), rc;
  if (fd < 0)
    return error->code;
  rc = fstat(fd, &status) == 0 && S_ISREG(status.st_mode)
           ? CAI_OK
           : invalid(error, "agent resource must be a regular file");
  if (rc == CAI_OK && (file = fdopen(fd, "rb")) == NULL)
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "open agent resource stream");
  if (file == NULL)
    close(fd);
  if (rc == CAI_OK)
    rc = cai_source_file(file, 1, &source, error);
  if (source == NULL && file != NULL)
    fclose(file);
  if (rc == CAI_OK)
    return attach_source(package, lease, name, source,
                         (status.st_mode & 0111) != 0 ? 0700 : 0600, error);
  cai_source_close(source);
  return rc;
}
static int attach_file(agent_package *package, lc_lease *lease,
                       const char *path, cai_error *error) {
  return attach_file_as(package, lease, path, path, error);
}

typedef struct scan_frame {
  struct scan_frame *parent;
  DIR *directory;
  char *path;
} scan_frame;

/** One cursor per directory on the heap. No recursive traversal or PATH_MAX
 * arrays occupy a worker stack, even for deeply nested tool packages. */
typedef int (*resource_visitor)(void *, const char *, cai_error *);
static int walk_tree(int root_fd, const char *root, resource_visitor visit,
                     void *context, cai_error *error) {
  scan_frame *top = NULL, *frame;
  struct dirent *entry;
  struct stat status;
  char *next = strdup(root);
  int fd, rc = next != NULL ? CAI_OK : CAI_ERR_NOMEM;
  while (rc == CAI_OK) {
    if (next != NULL) {
      fd = *next != '\0' ? open_path(root_fd, next, O_RDONLY, 0, 0, error)
                         : dup(root_fd);
      if (fd < 0) {
        rc = error->code;
        break;
      }
      if (fstat(fd, &status) != 0) {
        close(fd);
        rc = CAI_ERR_TRANSPORT;
        break;
      }
      if (S_ISDIR(status.st_mode)) {
        frame = calloc(1U, sizeof(*frame));
        if (frame == NULL || (frame->directory = fdopendir(fd)) == NULL) {
          free(frame);
          close(fd);
          rc = CAI_ERR_NOMEM;
          break;
        }
        frame->path = next;
        frame->parent = top;
        top = frame;
        next = NULL;
      } else {
        close(fd);
        rc = S_ISREG(status.st_mode)
                 ? visit(context, next, error)
                 : invalid(
                       error,
                       "agent resources must be regular files or directories");
        free(next);
        next = NULL;
      }
    }
    if (rc != CAI_OK || top == NULL)
      break;
    errno = 0;
    entry = readdir(top->directory);
    if (entry == NULL) {
      if (errno != 0) {
        rc = CAI_ERR_TRANSPORT;
        break;
      }
      frame = top;
      top = frame->parent;
      closedir(frame->directory);
      free(frame->path);
      free(frame);
    } else if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
      if (asprintf(&next, "%s%s%s", top->path, *top->path != '\0' ? "/" : "",
                   entry->d_name) < 0)
        rc = CAI_ERR_NOMEM;
    }
  }
  free(next);
  while (top != NULL) {
    frame = top;
    top = frame->parent;
    closedir(frame->directory);
    free(frame->path);
    free(frame);
  }
  return rc != CAI_OK && error->code == CAI_OK
             ? cai_cli_error(error, rc, "read agent resource directory")
             : rc;
}

typedef struct attach_context {
  agent_package *package;
  lc_lease *lease;
} attach_context;
static int attach_visit(void *context, const char *path, cai_error *error) {
  attach_context *attach = context;
  return attach_file(attach->package, attach->lease, path, error);
}
static int attach_tree(agent_package *package, lc_lease *lease,
                       const char *root, cai_error *error) {
  attach_context context;
  context.package = package;
  context.lease = lease;
  return walk_tree(package->root, root, attach_visit, &context, error);
}

typedef struct seed_package {
  agent_package package;
  cai_cli_producer produce;
  void *context;
  cai_config *resources;
  lc_lease *lease;
  const char *prefix;
  size_t skip;
} seed_package;

static int seed_visit(void *context, const char *path, cai_error *error) {
  seed_package *seed = context;
  char *name = NULL;
  int rc;
  if (asprintf(&name, "%s/%s", seed->prefix, path + seed->skip) < 0)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate seeded resource name");
  rc = attach_file_as(&seed->package, seed->lease, path, name, error);
  free(name);
  return rc;
}

static int default_instructions(void *context, cai_sink *sink,
                                cai_error *error) {
  static const char text[] = "{{runtime_default}}\n";
  (void)context;
  return cai_sink_write(sink, text, sizeof(text) - 1U, error);
}

/** Capture selected global resources through the normal inventory/uploader.
 * The cloned configuration is rewritten before its producer starts. */
static int text_file(int, const char *, int, int *, cai_error *);
static int validate_tool_directory(const char *, cai_error *);

static int seed_publish(void *context, lc_lease *lease, cai_error *error) {
  static const char *const sections[] = {"agent", "paths", "plugins", "paths",
                                         "review"};
  static const char *const keys[] = {"developer_instructions_file", "agents_md",
                                     "directory", "skills_dir",
                                     "developer_instructions_file"};
  static const char *const names[] = {"SYSTEM_PROMPT.md", "AGENTS.md", "tools",
                                      "skills", "files/reviewer.md"};
  seed_package *seed = context;
  agent_package *package = &seed->package;
  cai_config *config = NULL;
  cai_source *source = NULL;
  lc_error local;
  const char *path;
  size_t i;
  int rc, instructions = 0;
  lc_error_init(&local);
  seed->lease = lease;
  strcpy(package->record.name, "default");
  strcpy(package->record.kind, "agent");
  rc = lc_xid_new(package->record.revision, &local) == LC_OK
           ? CAI_OK
           : cai_cli_lc_error(error, &local);
  if (rc == CAI_OK)
    rc = write_metadata(lease, &package->record, error);
  if (rc == CAI_OK && seed->resources != NULL)
    rc = cai_config_clone(seed->resources, &config, error);
  if (rc == CAI_OK && config != NULL) {
    package->root = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (package->root < 0)
      rc = invalid(error, "open seeded resource root");
  }
  for (i = 0U;
       rc == CAI_OK && config != NULL && i < sizeof(keys) / sizeof(keys[0]);
       ++i) {
    char *absolute;
    path = cai_config_get(config, sections[i], keys[i]);
    if (path == NULL || *path == '\0')
      continue;
    rc = cai_config_resolve_path(config, sections[i], keys[i], &path, error);
    if (rc != CAI_OK)
      break;
    absolute = realpath(path, NULL);
    if (absolute == NULL) {
      rc = invalid(error, "configured agent resource is unavailable");
      break;
    }
    if (i == 2U || i == 3U) {
      seed->skip = strcmp(absolute, "/") == 0 ? 0U : strlen(absolute);
      seed->prefix = names[i];
      if (i == 2U)
        rc = validate_tool_directory(absolute, error);
      if (rc == CAI_OK)
        rc = walk_tree(package->root, absolute + 1U, seed_visit, seed, error);
    } else {
      int has_text;
      rc = text_file(package->root, absolute + 1U, 1, &has_text, error);
      if (rc == CAI_OK && (i != 1U || has_text))
        rc = attach_file_as(package, lease, absolute + 1U, names[i], error);
      if (rc == CAI_OK && i == 1U && !has_text) {
        free(absolute);
        rc = cai_config_set(config, sections[i], keys[i], "", error);
        continue;
      }
      if (rc == CAI_OK && i == 0U)
        instructions = 1;
    }
    free(absolute);
    if (rc == CAI_OK)
      rc = cai_config_set(config, sections[i], keys[i], i == 0U ? "" : names[i],
                          error);
  }
  if (package->root >= 0) {
    close(package->root);
    package->root = -1;
  }
  if (rc == CAI_OK && !instructions)
    rc =
        cai_cli_stream_source(default_instructions, NULL, NULL, &source, error);
  if (rc == CAI_OK && !instructions) {
    rc = attach_source(package, lease, "SYSTEM_PROMPT.md", source, 0600, error);
    source = NULL;
  }
  if (rc == CAI_OK)
    rc = cai_cli_stream_source(
        config != NULL ? cai_cli_agent_configuration_write : seed->produce,
        config != NULL ? config : seed->context, NULL, &source, error);
  if (rc == CAI_OK) {
    rc = attach_source(package, lease, "config.ini", source, 0600, error);
    source = NULL;
  }
  if (rc == CAI_OK)
    rc = write_metadata(lease, &package->record, error);
  cai_source_close(source);
  cai_config_close(config);
  lc_error_cleanup(&local);
  return rc;
}

int cai_cli_agent_package_initialize(cai_cli_pouch *pouch,
                                     cai_cli_producer produce, void *context,
                                     cai_config *resources, cai_error *error) {
  seed_package *seed = calloc(1U, sizeof(*seed));
  int rc;
  if (seed == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM,
                         "allocate default agent publication");
  seed->package.root = -1;
  seed->produce = produce;
  seed->context = context;
  seed->resources = resources;
  rc = record_open(&seed->package.record, error);
  if (rc == CAI_OK)
    rc = cai_cli_pouch_agent_initialize(pouch, "default", seed_publish, seed,
                                        error);
  record_close(&seed->package.record);
  free(seed);
  return rc;
}

static int publish(void *context, lc_lease *lease, cai_error *error) {
  agent_package *package = context;
  static const char *const files[] = {"config.ini", "SYSTEM_PROMPT.md",
                                      "AGENTS.md"};
  static const char *const trees[] = {"tools", "skills", "files"};
  lc_error local;
  struct stat status;
  size_t i;
  int rc = CAI_OK, deleted;
  cai_source *previous = NULL;
  lonejson_error json_error;
  lc_error_init(&local);
  /* Preserve the latest native credentials under this same publication lease.
   * A concurrent refresh cannot be overwritten by an older exported copy. */
  if (lease->state_etag != NULL && lease->state_etag[0] != '\0') {
    rc = cai_cli_pouch_agent_private(lease, &previous, error);
    if (rc == CAI_OK &&
        lonejson_parse_reader(package->record.runtime, &auth_map,
                              &package->record, cai_cli_json_read, previous,
                              &json_error) != LONEJSON_STATUS_OK)
      rc = cai_cli_lj_error(error, &json_error);
    cai_source_close(previous);
  }
  strcpy(package->record.kind, "agent");
  if (rc == CAI_OK)
    rc = lc_xid_new(package->record.revision, &local) == LC_OK
             ? CAI_OK
             : cai_cli_lc_error(error, &local);
  if (rc == CAI_OK && package->auth_file != NULL) {
    if (lonejson_json_value_set_path(&package->record.authentication,
                                     package->auth_file,
                                     &json_error) != LONEJSON_STATUS_OK)
      rc = cai_cli_lj_error(error, &json_error);
  }
  if (rc == CAI_OK)
    rc = write_metadata(lease, &package->record, error);
  if (rc == CAI_OK &&
      lc_lease_delete_all_attachments(lease, &deleted, &local) != LC_OK)
    rc = cai_cli_lc_error(error, &local);
  for (i = 0U; rc == CAI_OK && i < sizeof(files) / sizeof(files[0]); ++i) {
    if (i == 2U && !package->global_text)
      continue;
    if (fstatat(package->root, files[i], &status, AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno != ENOENT || i < 2U)
        rc = invalid(error,
                     "agent package requires config.ini and SYSTEM_PROMPT.md");
    } else
      rc = attach_file(package, lease, files[i], error);
  }
  for (i = 0U; rc == CAI_OK && i < sizeof(trees) / sizeof(trees[0]); ++i) {
    if (fstatat(package->root, trees[i], &status, AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno != ENOENT)
        rc = invalid(error, "cannot inspect agent resource root");
    } else if (!S_ISDIR(status.st_mode))
      rc = invalid(error, "agent resource root must be a directory");
    else
      rc = attach_tree(package, lease, trees[i], error);
  }
  if (rc == CAI_OK)
    rc = write_metadata(lease, &package->record, error);
  lc_error_cleanup(&local);
  return rc;
}

static int write_all(int fd, const void *bytes, size_t count,
                     cai_error *error) {
  const unsigned char *p = bytes;
  while (count != 0U) {
    ssize_t n = write(fd, p, count);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return cai_cli_error(error, CAI_ERR_TRANSPORT, "write agent resource");
    p += (size_t)n;
    count -= (size_t)n;
  }
  return CAI_OK;
}

static int download_file(cai_cli_pouch *pouch, const char *name,
                         const agent_file *file, int root, cai_error *error) {
  unsigned char bytes[2048], digest[EVP_MAX_MD_SIZE];
  char hex[65];
  unsigned int size;
  size_t n, i;
  cai_source *source = NULL;
  EVP_MD_CTX *hash = EVP_MD_CTX_new();
  int fd = -1, rc = hash != NULL ? CAI_OK : CAI_ERR_NOMEM;
  if (!safe_path(file->path) || file->hash == NULL ||
      strlen(file->hash) != 64U || (file->mode != 0600 && file->mode != 0700))
    rc = invalid(error, "invalid stored agent file inventory");
  if (rc == CAI_OK && EVP_DigestInit_ex(hash, EVP_sha256(), NULL) != 1)
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                       "initialize agent resource digest");
  if (rc == CAI_OK)
    rc = cai_cli_pouch_agent_read(pouch, name, file->path, &source, error);
  if (rc == CAI_OK) {
    fd = open_path(root, file->path, O_WRONLY | O_CREAT | O_EXCL, 0600, 1,
                   error);
    if (fd < 0)
      rc = error->code;
  }
  while (rc == CAI_OK &&
         (n = cai_source_read(source, bytes, sizeof(bytes), error)) != 0U) {
    if (EVP_DigestUpdate(hash, bytes, n) != 1)
      rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "hash agent resource");
    if (rc == CAI_OK)
      rc = write_all(fd, bytes, n, error);
  }
  if (rc == CAI_OK)
    rc = error->code;
  if (rc == CAI_OK && EVP_DigestFinal_ex(hash, digest, &size) != 1)
    rc =
        cai_cli_error(error, CAI_ERR_TRANSPORT, "finish agent resource digest");
  if (rc == CAI_OK) {
    for (i = 0U; i < 32U; ++i)
      snprintf(hex + i * 2U, 3U, "%02x", (unsigned int)digest[i]);
    if (strcmp(hex, file->hash) != 0)
      rc = invalid(
          error, "agent resource changed during download; retry the operation");
  }
  if (rc == CAI_OK && fchmod(fd, (mode_t)file->mode) != 0)
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                       "set agent resource permissions");
  if (fd >= 0 && close(fd) != 0 && rc == CAI_OK)
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "close agent resource");
  EVP_MD_CTX_free(hash);
  cai_source_close(source);
  return rc;
}

static int materialize(cai_cli_pouch *pouch, const agent_record *record,
                       int root, cai_error *error) {
  const agent_file *files = record->files.items;
  size_t i;
  int config = 0, instructions = 0, rc = CAI_OK;
  for (i = 0U; rc == CAI_OK && i < record->files.count; ++i) {
    if (strcmp(files[i].path, "config.ini") == 0)
      ++config;
    if (strcmp(files[i].path, "SYSTEM_PROMPT.md") == 0)
      ++instructions;
    if (strcmp(files[i].path, "config.ini") != 0)
      rc = download_file(pouch, record->name, &files[i], root, error);
  }
  if (rc == CAI_OK && (config != 1 || instructions != 1))
    rc = invalid(error, "stored agent lacks config.ini or SYSTEM_PROMPT.md");
  return rc;
}

/** Editing exposes only configuration and instruction documents. Other
 * attachments remain in lockd and retain their existing inventory entries. */
static int materialize_editor(cai_cli_pouch *pouch, const agent_record *record,
                              int root, cai_error *error) {
  const agent_file *files = record->files.items;
  size_t i;
  int rc = CAI_OK, instructions = 0;
  for (i = 0U; rc == CAI_OK && i < record->files.count; ++i) {
    if (strcmp(files[i].path, "SYSTEM_PROMPT.md") == 0 ||
        strcmp(files[i].path, "AGENTS.md") == 0) {
      rc = download_file(pouch, record->name, &files[i], root, error);
      if (strcmp(files[i].path, "SYSTEM_PROMPT.md") == 0)
        instructions = 1;
    }
  }
  return rc == CAI_OK && !instructions
             ? invalid(error, "stored agent lacks SYSTEM_PROMPT.md")
             : rc;
}

static int config_source(cai_cli_pouch *pouch, agent_record *record,
                         cai_source **out, cai_error *error) {
  const agent_file *files = record->files.items;
  verified_source *v = calloc(1U, sizeof(*v));
  cai_source_callbacks callbacks = {0};
  size_t i;
  int rc = CAI_OK;
  *out = NULL;
  if (v == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM,
                         "allocate agent configuration reader");
  for (i = 0U; i < record->files.count; ++i)
    if (strcmp(files[i].path, "config.ini") == 0)
      break;
  if (i == record->files.count || files[i].hash == NULL ||
      strlen(files[i].hash) != 64U)
    rc = invalid(error, "agent inventory lacks a configuration digest");
  if (rc == CAI_OK) {
    strcpy(v->expected, files[i].hash);
    v->hash = EVP_MD_CTX_new();
    if (v->hash == NULL || EVP_DigestInit_ex(v->hash, EVP_sha256(), NULL) != 1)
      rc = cai_cli_error(error, CAI_ERR_NOMEM,
                         "initialize agent configuration digest");
  }
  if (rc == CAI_OK)
    rc = cai_cli_pouch_agent_read(pouch, record->name, "config.ini", &v->source,
                                  error);
  callbacks.context = v;
  callbacks.read = verified_read;
  callbacks.close = verified_close;
  if (rc == CAI_OK)
    rc = cai_source_from_callbacks(&callbacks, out, error);
  if (rc != CAI_OK)
    verified_close(v);
  return rc;
}

int cai_cli_agent_package_load(cai_cli_pouch *pouch, const char *name,
                               const char *cache, cai_source **config,
                               char **directory, char revision[21],
                               cai_error *error) {
  agent_record *record = calloc(1U, sizeof(*record));
  char *parent = NULL, *target = NULL, *stage = NULL;
  struct stat status;
  int root = -1, parent_fd = -1, marker = -1, rc;
  *config = NULL;
  *directory = NULL;
  revision[0] = '\0';
  if (record == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM,
                         "allocate stored agent metadata");
  rc = record_open(record, error);
  if (rc == CAI_OK)
    rc = read_record(pouch, name, record, error);
  if (rc == CAI_OK && (!lc_xid_is_valid(record->revision) ||
                       asprintf(&parent, "%s/agents/%s", cache, name) < 0))
    rc = invalid(error, "invalid stored agent revision");
  if (rc == CAI_OK)
    rc = cai_cli_private_directory(parent, error);
  if (rc == CAI_OK && asprintf(&target, "%s/%s", parent, record->revision) < 0)
    rc = CAI_ERR_NOMEM;
  if (rc == CAI_OK) {
    parent_fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    root = openat(parent_fd, record->revision,
                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root >= 0 &&
        fstatat(root, ".complete", &status, AT_SYMLINK_NOFOLLOW) != 0) {
      close(root);
      root = -1;
    }
  }
  if (rc == CAI_OK && root < 0) {
    if (asprintf(&stage, "%s/.materialize-XXXXXX", parent) < 0 ||
        mkdtemp(stage) == NULL)
      rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "create agent cache staging directory");
    if (rc == CAI_OK) {
      root = open(stage, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (root < 0)
        rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "open agent cache staging directory");
    }
    if (rc == CAI_OK)
      rc = materialize(pouch, record, root, error);
    if (rc == CAI_OK) {
      marker = openat(root, ".complete",
                      O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
      if (marker < 0)
        rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "complete agent cache");
    }
    if (marker >= 0)
      close(marker);
    if (rc == CAI_OK && rename(stage, target) != 0) {
      int winner = openat(parent_fd, record->revision,
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (winner < 0 ||
          fstatat(winner, ".complete", &status, AT_SYMLINK_NOFOLLOW) != 0)
        rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "publish agent cache directory");
      if (winner >= 0)
        close(winner);
    }
  }
  if (rc == CAI_OK)
    rc = config_source(pouch, record, config, error);
  if (rc == CAI_OK) {
    memcpy(revision, record->revision, sizeof(record->revision));
    *directory = target;
    target = NULL;
  }
  if (root >= 0)
    close(root);
  if (stage != NULL && parent_fd >= 0)
    (void)cai_cli_remove_at(parent_fd, strrchr(stage, '/') + 1U);
  if (parent_fd >= 0)
    close(parent_fd);
  free(parent);
  free(target);
  free(stage);
  record_close(record);
  free(record);
  return rc != CAI_OK && error->code == CAI_OK
             ? cai_cli_error(error, rc, "load stored agent package")
             : rc;
}

static int validate_setting(void *context, const char *section, const char *key,
                            const cai_config_field *field,
                            const char *const *values, size_t count,
                            cai_error *error) {
  (void)context;
  (void)field;
  (void)values;
  (void)count;
  return cai_cli_agent_bootstrap_field(section, key)
             ? invalid(error, "agent source configuration cannot contain "
                              "host/bootstrap settings")
             : CAI_OK;
}

static int text_file(int root, const char *path, int required, int *has_text,
                     cai_error *error) {
  unsigned char bytes[2048];
  struct stat st;
  ssize_t n;
  size_t i, total = 0U;
  int fd, rc = CAI_OK;
  *has_text = 0;
  if (!required && fstatat(root, path, &st, AT_SYMLINK_NOFOLLOW) != 0 &&
      errno == ENOENT)
    return CAI_OK;
  fd = open_path(root, path, O_RDONLY, 0, 0, error);
  if (fd < 0)
    return error->code;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
    rc = invalid(error, "agent instructions must be a regular file");
  while (rc == CAI_OK) {
    n = read(fd, bytes, sizeof(bytes));
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0) {
      rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "read agent instructions");
      break;
    }
    if (n == 0)
      break;
    total += (size_t)n;
    if (total > 1024U * 1024U || memchr(bytes, 0, (size_t)n) != NULL) {
      rc = invalid(
          error,
          "agent instruction files must be NUL-free text of at most 1 MiB");
      break;
    }
    for (i = 0U; i < (size_t)n; ++i)
      if (!isspace(bytes[i]))
        *has_text = 1;
  }
  close(fd);
  return rc;
}

static int create_empty_directory(const char *path, int *out,
                                  cai_error *error) {
  DIR *directory;
  struct dirent *entry;
  char *copy = strdup(path), *cursor;
  struct stat status;
  int fd, scan, rc = CAI_OK;
  *out = -1;
  if (copy == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate agent output path");
  for (cursor = copy + 1U;; ++cursor) {
    if (*cursor == '/' || *cursor == '\0') {
      char saved = *cursor;
      *cursor = '\0';
      if (mkdir(copy, 0700) != 0 &&
          (errno != EEXIST || lstat(copy, &status) != 0 ||
           !S_ISDIR(status.st_mode)))
        rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "create agent output directory");
      *cursor = saved;
      if (saved == '\0' || rc != CAI_OK)
        break;
    }
  }
  free(copy);
  if (rc != CAI_OK)
    return rc;
  fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0)
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "open agent output directory");
  scan = dup(fd);
  directory = scan >= 0 ? fdopendir(scan) : NULL;
  if (directory == NULL) {
    if (scan >= 0)
      close(scan);
    close(fd);
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "inspect agent output directory");
  }
  errno = 0;
  while ((entry = readdir(directory)) != NULL) {
    if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
      rc = invalid(error, "agent output directory must be empty; existing "
                          "files are never overwritten");
      break;
    }
  }
  if (rc == CAI_OK && errno != 0)
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "list agent output directory");
  closedir(directory);
  if (rc == CAI_OK)
    *out = fd;
  else
    close(fd);
  return rc;
}

static int write_literal_file(int root, const char *path, const char *text,
                              cai_error *error) {
  int fd = open_path(root, path, O_WRONLY | O_CREAT | O_EXCL, 0600, 1, error);
  int rc = fd >= 0 ? write_all(fd, text, strlen(text), error) : error->code;
  if (fd >= 0 && close(fd) != 0 && rc == CAI_OK)
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "close agent source file");
  return rc;
}

static int write_configuration(int root, cai_config *config, const char *name,
                               int credentials, cai_error *error) {
  cai_sink *sink = NULL;
  FILE *file = NULL;
  int fd = open_path(root, "config.ini", O_WRONLY | O_CREAT | O_EXCL, 0600, 0,
                     error);
  int rc = fd >= 0 ? CAI_OK : error->code;
  if (rc == CAI_OK && (file = fdopen(fd, "wb")) == NULL) {
    close(fd);
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                       "open agent configuration output");
  }
  if (rc == CAI_OK)
    rc = cai_sink_file(file, 0, &sink, error);
  if (rc == CAI_OK)
    rc = cai_cli_agent_configuration_export(config, name, credentials, sink,
                                            error);
  cai_sink_close(sink);
  if (file != NULL && fclose(file) != 0 && rc == CAI_OK)
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                       "close agent configuration output");
  return rc;
}

static int write_example(int root, const cai_cli_options *options,
                         cai_error *error) {
  cai_agent_preset preset;
  cai_agent_preset_config config;
  cai_sink *sink = NULL;
  FILE *file = NULL;
  const char *identity = options->identity != NULL ? options->identity : "CAI";
  int fd = open_path(root, "EXAMPLE_SYSTEM_PROMPT.md",
                     O_CREAT | O_EXCL | O_WRONLY, 0600, 0, error);
  int rc = fd >= 0 ? CAI_OK : error->code;
  if (rc == CAI_OK && (file = fdopen(fd, "wb")) == NULL) {
    close(fd);
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "open example instructions");
  }
  if (rc == CAI_OK)
    rc = cai_sink_file(file, 0, &sink, error);
  if (rc == CAI_OK)
    rc = cai_sink_write(sink, "You are ", 8U, error);
  if (rc == CAI_OK)
    rc = cai_sink_write(sink, identity, strlen(identity), error);
  if (rc == CAI_OK) {
    const char *intro =
        ", a software engineering agent. You and the user share "
        "one workspace; collaborate until their intended goal "
        "is completely handled.\n";
    rc = cai_sink_write(sink, intro, strlen(intro), error);
  }
  cai_agent_preset_from_smith(&preset);
  cai_agent_preset_config_init(&config);
  config.agent_identity = identity;
  preset.developer_instructions =
      strchr((const char *)example_template, '\n') + 1U;
  if (rc == CAI_OK)
    rc = cai_agent_preset_write_base_instructions(&preset, &config, 0, sink,
                                                  error);
  cai_sink_close(sink);
  if (file != NULL && fclose(file) != 0 && rc == CAI_OK)
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "close example instructions");
  return rc;
}

/** Output was verified empty before writing. Only this invocation's children
 * can be removed on failure; an existing nonempty destination is untouched. */
static void clear_output(int root) {
  int scan = dup(root);
  DIR *directory = scan >= 0 ? fdopendir(scan) : NULL;
  struct dirent *entry;
  if (directory == NULL) {
    if (scan >= 0)
      close(scan);
    return;
  }
  rewinddir(directory);
  while ((entry = readdir(directory)) != NULL)
    if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
      (void)cai_cli_remove_at(root, entry->d_name);
  closedir(directory);
}

static int export_package(agent_package *package, const char *selected,
                          const char *target, const char *output_name,
                          cai_error *error) {
  cai_source *config = NULL;
  cai_config *parsed = NULL;
  char *origin = NULL;
  int root = -1, rc;
  rc = read_record(&package->repository, selected, &package->record, error);
  if (rc == CAI_OK)
    rc = create_empty_directory(target, &root, error);
  if (rc == CAI_OK) {
    rc = package->editing
             ? materialize_editor(&package->repository, &package->record, root,
                                  error)
             : materialize(&package->repository, &package->record, root, error);
    if (rc == CAI_OK)
      rc =
          config_source(&package->repository, &package->record, &config, error);
  }
  if (rc == CAI_OK && asprintf(&origin, "%s/config.ini", target) < 0)
    rc = CAI_ERR_NOMEM;
  if (rc == CAI_OK)
    rc = cai_cli_agent_source_configuration(
        NULL, target, package->options->environment, &parsed, error);
  if (rc == CAI_OK)
    rc = cai_config_load(parsed, config, origin, error);
  if (rc == CAI_OK)
    rc = write_configuration(
        root, parsed, output_name,
        package->editing || package->options->include_credentials, error);
  if (rc == CAI_OK && !package->editing &&
      package->options->include_credentials &&
      package->record.authentication.kind != LONEJSON_JSON_VALUE_NULL) {
    cai_sink *sink = NULL;
    FILE *file = NULL;
    lonejson_error local;
    int fd = open_path(root, "AUTH.json", O_WRONLY | O_CREAT | O_EXCL, 0600, 0,
                       error);
    if (fd >= 0)
      file = fdopen(fd, "wb");
    if (file == NULL) {
      if (fd >= 0)
        close(fd);
      rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "open explicit credential export");
    } else {
      rc = cai_sink_file(file, 0, &sink, error);
      if (rc == CAI_OK &&
          lonejson_json_value_write_to_sink(&package->record.authentication,
                                            cai_cli_json_write, sink,
                                            &local) != LONEJSON_STATUS_OK)
        rc = cai_cli_lj_error(error, &local);
      cai_sink_close(sink);
      if (fclose(file) != 0 && rc == CAI_OK)
        rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "close explicit credential export");
    }
  }
  if (rc == CAI_OK && package->options->agent_build != NULL)
    rc = write_example(root, package->options, error);
  if (root >= 0) {
    if (rc != CAI_OK)
      clear_output(root);
    close(root);
  }
  free(origin);
  cai_source_close(config);
  cai_config_close(parsed);
  return rc;
}

static int edit_publish(void *context, lc_lease *lease, cai_error *error) {
  agent_package *package = context;
  agent_record *record = &package->record;
  cai_source *source = NULL;
  lonejson_error json_error;
  lc_error local;
  int rc, deleted;
  size_t i;
  lc_error_init(&local);
  record_close(record);
  rc = record_open(record, error);
  if (rc == CAI_OK)
    rc = cai_cli_pouch_agent_private(lease, &source, error);
  if (rc == CAI_OK && lonejson_parse_reader(record->runtime, &record_map,
                                            record, cai_cli_json_read, source,
                                            &json_error) != LONEJSON_STATUS_OK)
    rc = cai_cli_lj_error(error, &json_error);
  cai_source_close(source);
  if (rc == CAI_OK &&
      (strcmp(record->name, package->expected_name) != 0 ||
       strcmp(record->revision, package->expected_revision) != 0))
    rc = invalid(
        error,
        "agent changed while editing; reopen --edit-agent to resolve it");
  if (rc == CAI_OK && !package->global_text) {
    agent_file *files = record->files.items;
    lc_attachment_selector selector = {0};
    selector.name = "AGENTS.md";
    if (lc_lease_delete_attachment(lease, &selector, &deleted, &local) != LC_OK)
      rc = cai_cli_lc_error(error, &local);
    for (i = 0U; rc == CAI_OK && i < record->files.count; ++i) {
      if (strcmp(files[i].path, "AGENTS.md") == 0) {
        /* The mapped array owns its flat elements. Move the final element
         * rather than duplicating ownership of its allocated strings. */
        record->runtime->cleanup(record->runtime, &file_map, &files[i], NULL);
        --record->files.count;
        if (i != record->files.count)
          files[i] = files[record->files.count];
        memset(&files[record->files.count], 0, sizeof(*files));
        break;
      }
    }
  }
  if (rc == CAI_OK)
    rc = attach_file(package, lease, "config.ini", error);
  if (rc == CAI_OK)
    rc = attach_file(package, lease, "SYSTEM_PROMPT.md", error);
  if (rc == CAI_OK && package->global_text)
    rc = attach_file(package, lease, "AGENTS.md", error);
  if (rc == CAI_OK && lc_xid_new(record->revision, &local) != LC_OK)
    rc = cai_cli_lc_error(error, &local);
  if (rc == CAI_OK)
    rc = write_metadata(lease, record, error);
  lc_error_cleanup(&local);
  return rc;
}

/** A local edit is an explicitly requested plaintext handoff. The private
 * tree, including editor backups, is removed on every ordinary return. */
static int edit_package(agent_package *package, const char *name, int stop_fd,
                        int (*stop)(void *), void *context, cai_error *error) {
  static const char *const names[] = {"config.ini", "SYSTEM_PROMPT.md",
                                      "AGENTS.md"};
  const char *temporary =
      cai_environment_get(package->options->environment, "TMPDIR");
  const char *edited_name;
  char *tree = NULL, *parent = NULL, *paths[3] = {NULL, NULL, NULL};
  char canonical[65];
  struct stat status;
  size_t i;
  int parent_fd = -1, rc = CAI_OK, has_text = 0;
  if (temporary == NULL || *temporary == '\0')
    temporary = "/tmp";
  if (temporary[0] != '/')
    return invalid(error, "TMPDIR must be absolute for agent editing");
  if (asprintf(&tree, "%s/%s-agent-edit.XXXXXX", temporary,
               package->options->program_name) < 0)
    return cai_cli_error(error, CAI_ERR_NOMEM,
                         "allocate private agent edit path");
  if (mkdtemp(tree) == NULL) {
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                       "create private agent edit directory");
    goto done;
  }
  parent = strdup(tree);
  if (parent == NULL) {
    (void)rmdir(tree);
    rc = cai_cli_error(error, CAI_ERR_NOMEM, "retain agent edit parent");
    goto done;
  }
  *strrchr(parent, '/') = '\0';
  parent_fd = open(*parent ? parent : "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (parent_fd < 0) {
    (void)rmdir(tree);
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "open agent edit parent");
    goto done;
  }
  package->editing = 1;
  rc = export_package(package, name, tree, name, error);
  if (rc == CAI_OK) {
    strcpy(package->expected_name, name);
    strcpy(package->expected_revision, package->record.revision);
    package->root = open(tree, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (package->root < 0)
      rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "open private edit directory");
  }
  if (rc == CAI_OK &&
      fstatat(package->root, "AGENTS.md", &status, AT_SYMLINK_NOFOLLOW) != 0) {
    if (errno == ENOENT)
      rc = write_literal_file(package->root, "AGENTS.md", "", error);
    else
      rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "inspect editable global instructions");
  }
  for (i = 0U; rc == CAI_OK && i < 3U; ++i)
    if (asprintf(&paths[i], "%s/%s", tree, names[i]) < 0)
      rc = cai_cli_error(error, CAI_ERR_NOMEM, "allocate editable filename");
  if (rc == CAI_OK)
    rc = cai_cli_editor_run(package->options->environment,
                            (const char *const *)paths, 3U, stop_fd, stop,
                            context, error);
  if (rc == CAI_OK)
    rc = cai_cli_agent_source_configuration(paths[0], tree,
                                            package->options->environment,
                                            &package->configuration, error);
  if (rc == CAI_OK)
    rc =
        cai_config_visit(package->configuration, validate_setting, NULL, error);
  if (rc == CAI_OK) {
    edited_name = cai_config_get(package->configuration, "agent", "name");
    rc = cai_cli_agent_name(edited_name != NULL && *edited_name ? edited_name
                                                                : name,
                            canonical, error);
    if (rc == CAI_OK && strcmp(canonical, name) != 0)
      rc = invalid(error, "--edit-agent keeps its name; use --build and --make "
                          "to create a named copy");
  }
  if (rc == CAI_OK)
    rc = text_file(package->root, "SYSTEM_PROMPT.md", 1, &has_text, error);
  if (rc == CAI_OK)
    rc = text_file(package->root, "AGENTS.md", 0, &has_text, error);
  package->global_text = has_text;
  if (rc == CAI_OK && stop != NULL && stop(context))
    rc = cai_cli_error(error, CAI_ERR_CANCELLED, "agent edit cancelled");
  if (rc == CAI_OK)
    rc = cai_cli_pouch_agent_publish(&package->repository, name, 1,
                                     edit_publish, package, error);
  if (rc == CAI_OK && printf("Updated agent %s.\n", name) < 0)
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "write agent edit result");
done:
  if (parent_fd >= 0) {
    if (cai_cli_remove_at(parent_fd, strrchr(tree, '/') + 1U) != 0)
      rc = cai_cli_error(
          error, CAI_ERR_TRANSPORT,
          "agent edit cleanup failed; remove its private temporary directory");
    close(parent_fd);
  }
  for (i = 0U; i < 3U; ++i)
    free(paths[i]);
  free(parent);
  free(tree);
  return rc;
}

int cai_cli_agent_package_configure(cai_config *config, const char *directory,
                                    cai_error *error) {
  static const char token[] = "{{runtime_default}}";
  static const char *const sections[] = {"paths", "plugins", "paths"};
  static const char *const keys[] = {"agents_md", "directory", "skills_dir"};
  static const char *const names[] = {"AGENTS.md", "tools", "skills"};
  char *path = NULL;
  char window[sizeof(token)];
  size_t used = 0U, i;
  struct stat status;
  FILE *file;
  int c, inherited = 0, rc = CAI_OK;
  if (asprintf(&path, "%s/SYSTEM_PROMPT.md", directory) < 0)
    return cai_cli_error(error, CAI_ERR_NOMEM,
                         "allocate bundled instruction path");
  file = fopen(path, "rb");
  if (file == NULL)
    rc = invalid(error, "bundled SYSTEM_PROMPT.md is unavailable");
  while (rc == CAI_OK && (c = fgetc(file)) != EOF) {
    window[used++] = (char)c;
    if (used == sizeof(token) - 1U) {
      if (memcmp(window, token, used) == 0) {
        inherited = 1;
        break;
      }
      memmove(window, window + 1U, --used);
    }
  }
  if (file != NULL) {
    if (ferror(file))
      rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "read bundled instructions");
    fclose(file);
  }
  if (rc == CAI_OK)
    rc = cai_config_set(config, "agent", "developer_instructions_file",
                        inherited ? "" : path, error);
  free(path);
  for (i = 0U; rc == CAI_OK && i < sizeof(names) / sizeof(names[0]); ++i) {
    if (cai_config_get(config, sections[i], keys[i]) != NULL)
      continue;
    if (asprintf(&path, "%s/%s", directory, names[i]) < 0)
      return cai_cli_error(error, CAI_ERR_NOMEM,
                           "allocate bundled resource path");
    if (stat(path, &status) == 0)
      rc = cai_config_set(config, sections[i], keys[i], path, error);
    free(path);
    path = NULL;
  }
  return rc;
}

static int validate_tool_directory(const char *path, cai_error *error) {
  cai_plugins_config config;
  cai_plugins *plugins = NULL;
#ifdef CAI_APP_LUA_TOOLS
  cai_lua_tools *lua = NULL;
  cai_lua_tools_config adapter;
#endif
  int rc = CAI_OK;
  cai_plugins_config_init(&config);
  config.directories = &path;
  config.directory_count = 1U;
  rc = cai_plugins_open(&config, &plugins, error);
#ifdef CAI_APP_LUA_TOOLS
  cai_lua_tools_config_init(&adapter);
  if (rc == CAI_OK)
    rc = cai_lua_tools_open(&adapter, &lua, error);
  if (rc == CAI_OK)
    rc = cai_lua_tools_attach(lua, plugins, error);
#endif
  if (rc == CAI_OK)
    rc = cai_plugins_reload(plugins, error);
  cai_plugins_close(plugins);
#ifdef CAI_APP_LUA_TOOLS
  cai_lua_tools_close(lua);
#endif
  return rc;
}

static int validate_tools(agent_package *package, cai_error *error) {
  char *path = NULL;
  struct stat status;
  int rc;
  if (fstatat(package->root, "tools", &status, AT_SYMLINK_NOFOLLOW) != 0)
    return errno == ENOENT ? CAI_OK : invalid(error, "inspect bundled tools");
  if (!S_ISDIR(status.st_mode))
    return invalid(error, "bundled tools root must be a real directory");
  if (asprintf(&path, "%s/tools", package->directory) < 0)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate tool validation root");
  rc = validate_tool_directory(path, error);
  free(path);
  return rc;
}

static int package_path(const cai_cli_options *options, const char *path,
                        char **out, cai_error *error) {
  *out = malloc(PATH_MAX);
  if (*out == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate agent source path");
  return cai_cli_workspace_path(*out, PATH_MAX, options->directory, path,
                                error);
}

static int export_one(void *context, const char *name, cai_error *error) {
  agent_package *package = context;
  char *target = NULL;
  int rc;
  if (asprintf(&target, "%s/%s", package->export_root, name) < 0)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate agent export path");
  rc = export_package(package, name, target, name, error);
  free(target);
  return rc;
}

int cai_cli_agent_package_run(const cai_cli_options *options,
                              cai_config *config,
                              const cai_app_config *application,
                              const cai_config_context *startup, int stop_fd,
                              int (*stop)(void *), void *context,
                              cai_error *error) {
  agent_package *package = calloc(1U, sizeof(*package));
  const char *operation = options->agent_make != NULL    ? options->agent_make
                          : options->agent_build != NULL ? options->agent_build
                                                         : options->agent_dump;
  char selected[65], inferred[65];
  char *path = NULL, *source_config = NULL, *auth_path = NULL;
  const char *name;
  int has_text = 0, rc;
  (void)application;
  if (package == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM,
                         "allocate agent package operation");
  package->root = -1;
  package->options = options;
  rc = record_open(&package->record, error);
  if (rc == CAI_OK)
    rc = cai_cli_store_options(config, options, options->agents_store,
                               &package->store, error);
  if (rc == CAI_OK)
    rc = cai_cli_pouch_open_options(&package->repository, &package->store, NULL,
                                    error);
  if (rc == CAI_OK)
    rc = cai_cli_agent_name(options->agent_use != NULL ? options->agent_use
                                                       : "default",
                            selected, error);
  if (rc == CAI_OK && options->agent_edit != NULL) {
    rc = cai_cli_agent_name(options->agent_edit, selected, error);
    if (rc == CAI_OK)
      rc = edit_package(package, selected, stop_fd, stop, context, error);
    goto done;
  }
  if (rc == CAI_OK && options->agent_dump_all != NULL) {
    rc = package_path(options, options->agent_dump_all, &path, error);
    if (rc == CAI_OK) {
      package->export_root = path;
      rc = cai_cli_pouch_agents_visit(&package->repository, export_one, package,
                                      error);
    }
    goto done;
  }
  if (rc == CAI_OK && operation == NULL)
    rc = invalid(error, "agent package operation is required");
  if (rc == CAI_OK && options->agent_dump != NULL) {
    rc = cai_cli_agent_name(options->agent_dump, selected, error);
    operation = selected;
  }
  if (rc == CAI_OK)
    rc = package_path(options, operation, &path, error);
  if (rc == CAI_OK && options->agent_make != NULL) {
    package->directory = realpath(path, NULL);
    if (package->directory == NULL)
      rc = invalid(error, "cannot resolve agent source directory");
    if (rc == CAI_OK) {
      package->root = open(package->directory,
                           O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (package->root < 0)
        rc = invalid(error, "agent source must be a directory");
    }
    if (rc == CAI_OK &&
        asprintf(&source_config, "%s/config.ini", package->directory) < 0)
      rc = CAI_ERR_NOMEM;
    if (rc == CAI_OK)
      rc = cai_cli_agent_source_configuration(
          source_config, package->directory,
          startup != NULL ? startup->environment : NULL,
          &package->configuration, error);
    if (rc == CAI_OK)
      rc = cai_config_visit(package->configuration, validate_setting, NULL,
                            error);
    if (rc == CAI_OK) {
      name = cai_config_get(package->configuration, "agent", "name");
      if (name == NULL || *name == '\0')
        name = strrchr(package->directory, '/') + 1U;
      rc = cai_cli_agent_name(name, package->record.name, error);
    }
    if (rc == CAI_OK)
      rc = text_file(package->root, "SYSTEM_PROMPT.md", 1, &has_text, error);
    if (rc == CAI_OK)
      rc = text_file(package->root, "AGENTS.md", 0, &has_text, error);
    package->global_text = has_text;
    if (rc == CAI_OK && options->agent_auth_import != NULL) {
      cai_chatgpt_auth_config auth_config;
      cai_chatgpt_auth *auth = NULL;
      rc = package_path(options, options->agent_auth_import, &auth_path, error);
      cai_chatgpt_auth_config_init(&auth_config);
      auth_config.auth_json_path = auth_path;
      auth_config.logger_disabled = 1;
      if (rc == CAI_OK)
        rc = cai_chatgpt_auth_open(&auth_config, &auth, error);
      cai_chatgpt_auth_close(auth);
      package->auth_file = auth_path;
    }
    if (rc == CAI_OK)
      rc = validate_tools(package, error);
    if (rc == CAI_OK)
      rc = cai_cli_pouch_agent_publish(&package->repository,
                                       package->record.name, options->force,
                                       publish, package, error);
    if (rc == CAI_OK &&
        printf("Published agent %s.\n", package->record.name) < 0)
      rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "write agent publication result");
  } else if (rc == CAI_OK) {
    const char *base;
    if (options->agent_build != NULL) {
      int output = -1;
      rc = create_empty_directory(path, &output, error);
      if (output >= 0)
        close(output);
      if (rc == CAI_OK) {
        char *resolved = realpath(path, NULL);
        if (resolved == NULL)
          rc = invalid(error, "cannot resolve agent output directory");
        else {
          free(path);
          path = resolved;
        }
      }
    }
    base = strrchr(path, '/');
    if (rc == CAI_OK)
      rc = cai_cli_agent_name(options->agent_build != NULL
                                  ? (base != NULL ? base + 1U : path)
                                  : selected,
                              inferred, error);
    if (rc == CAI_OK)
      rc = export_package(package, selected, path, inferred, error);
    if (rc == CAI_OK && printf("Agent source written to %s.\n", path) < 0)
      rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "write agent export result");
  }
done:
  if (package->root >= 0)
    close(package->root);
  cai_cli_pouch_close(&package->repository);
  record_close(&package->record);
  cai_config_close(package->configuration);
  free(package->directory);
  free(path);
  free(source_config);
  free(auth_path);
  free(package);
  return rc != CAI_OK && error->code == CAI_OK
             ? cai_cli_error(error, rc, "agent package operation failed")
             : rc;
}
