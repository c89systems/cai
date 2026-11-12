#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define LONEJSON_WITH_CURL
#include "pouch.h"
#include "identity.h"
#include "log.h"
#include <cai/auth.h>
#include <cai/network.h>

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Store contexts retain these handles through teardown. SDK clients and
 * their app-owned logging state close only when the final borrower exits. */
typedef struct cai_cli_pouch_client {
  struct cai_cli_pouch_client *next;
  lc_client *client;
  cai_cli_log *logging;
  size_t references;
  int local;
  int compression;
  int write_uncertain;
  dev_t device, key_device;
  ino_t inode, key_inode;
  char *endpoint, *client_pem;
} cai_cli_pouch_client;
static pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;
static cai_cli_pouch_client *clients;

typedef struct cai_cli_journal_context {
  struct cai_cli_journal_context *next;
  cai_cli_session session;
  lc_collection *collection;
  struct cai_cli_session_guard *guard;
} cai_cli_journal_context;

static cai_cli_journal_context *journal_context_find(cai_cli_pouch *pouch,
                                                     const char *id) {
  cai_cli_journal_context *entry;
  for (entry = pouch->journal_contexts; entry != NULL; entry = entry->next)
    if (strcmp(entry->session.id, id) == 0)
      return entry;
  return NULL;
}
static void journal_contexts_close(cai_cli_pouch *pouch);
static void collection_owners_close(cai_cli_pouch *pouch);

static int same_pouch_options(const char *a, const char *b) {
  size_t na, nb;
  a += 8U + strcspn(a + 8U, "?#");
  b += 8U + strcspn(b + 8U, "?#");
  na = *a == '?' ? strcspn(++a, "#") : 0U;
  nb = *b == '?' ? strcspn(++b, "#") : 0U;
  return na == nb && memcmp(a, b, na) == 0;
}

static int shared_client_open(cai_cli_pouch *pouch,
                              const lc_client_config *config, const char *root,
                              int local, const char *client_pem,
                              cai_error *error) {
  cai_cli_pouch_client *entry;
  struct stat directory, key;
  lc_error detail;
  int exists, rc;
  pthread_mutex_lock(&clients_lock);
  exists = local && stat(root, &directory) == 0;
  for (entry = clients; entry != NULL; entry = entry->next) {
    if (entry->local != local ||
        (local ? !exists || entry->device != directory.st_dev ||
                     entry->inode != directory.st_ino
               : strcmp(entry->endpoint, config->endpoints[0]) != 0))
      continue;
    if ((local &&
         (entry->compression !=
              (strcmp(config->pouch_settings->compression, "zlib") == 0) ||
          stat(pouch->key_path, &key) != 0 || key.st_dev != entry->key_device ||
          key.st_ino != entry->key_inode ||
          !same_pouch_options(entry->endpoint, config->endpoints[0]))) ||
        (!local && strcmp(entry->client_pem,
                          client_pem != NULL ? client_pem : "") != 0)) {
      pthread_mutex_unlock(&clients_lock);
      return cai_cli_error(
          error, CAI_ERR_INVALID,
          "conflicting key, compression or TLS configuration for an "
          "already open store; select the same key file");
    }
    entry->references++;
    pouch->connection = entry;
    pouch->sessions = entry->client;
    pthread_mutex_unlock(&clients_lock);
    return CAI_OK;
  }
  if (local && exists && stat(pouch->key_path, &key) != 0) {
    pthread_mutex_unlock(&clients_lock);
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "pouch.key is missing for the existing encrypted "
                         "store; restore the original key");
  }
  entry = calloc(1U, sizeof(*entry));
  if (entry != NULL) {
    entry->endpoint = strdup(config->endpoints[0]);
    entry->client_pem = strdup(client_pem != NULL ? client_pem : "");
  }
  if (entry == NULL || entry->endpoint == NULL || entry->client_pem == NULL) {
    if (entry != NULL) {
      free(entry->endpoint);
      free(entry->client_pem);
      free(entry);
    }
    pthread_mutex_unlock(&clients_lock);
    return cai_cli_error(error, CAI_ERR_NOMEM, "retain store connection");
  }
  lc_error_init(&detail);
  rc = lc_client_open(config, &entry->client, &detail);
  rc = rc == LC_OK ? CAI_OK : cai_cli_lc_error(error, &detail);
  lc_error_cleanup(&detail);
  if (rc == CAI_OK && local &&
      (stat(root, &directory) != 0 || stat(pouch->key_path, &key) != 0)) {
    char message[1200];
    snprintf(message, sizeof(message),
             "identify opened pouch root %s and key %s: %s", root,
             pouch->key_path, strerror(errno));
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT, message);
  }
  if (rc != CAI_OK) {
    if (entry->client != NULL)
      entry->client->close(entry->client);
    free(entry->endpoint);
    free(entry->client_pem);
    free(entry);
    pthread_mutex_unlock(&clients_lock);
    return rc;
  }
  entry->local = local;
  entry->compression =
      local && strcmp(config->pouch_settings->compression, "zlib") == 0;
  if (local) {
    entry->device = directory.st_dev;
    entry->inode = directory.st_ino;
    entry->key_device = key.st_dev;
    entry->key_inode = key.st_ino;
  }
  entry->references = 1U;
  entry->logging = cai_cli_log_retain(config->logger);
  entry->next = clients;
  clients = entry;
  pouch->connection = entry;
  pouch->sessions = entry->client;
  pthread_mutex_unlock(&clients_lock);
  return CAI_OK;
}

static const lonejson_field session_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(cai_cli_session, id, "id",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED_REQ(cai_cli_session, workspace, "workspace",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED(cai_cli_session, first_prompt, "first_prompt",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED(cai_cli_session, history_backend,
                                "history_backend", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_U64(cai_cli_session, commit_position, "commit_position"),
    LONEJSON_FIELD_U64(cai_cli_session, commit_ns, "commit_ns"),
    LONEJSON_FIELD_U64(cai_cli_session, applied_sequence, "applied_sequence"),
    LONEJSON_FIELD_I64(cai_cli_session, published, "published"),
    LONEJSON_FIELD_I64(cai_cli_session, retention_override,
                       "retention_override"),
    LONEJSON_FIELD_I64(cai_cli_session, retention_days, "retention_days")};
LONEJSON_MAP_DEFINE(session_map, cai_cli_session, session_fields);

/* A derived directory index, kept separately from session documents.
 * Dirty is durable before a commit/publication changes; an interrupted
 * writer therefore causes reconstruction from authoritative session metadata.
 */
typedef struct latest_index {
  char workspace[PATH_MAX];
  lonejson_int64 dirty;
  cai_cli_session latest;
} latest_index;
static const lonejson_field latest_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(latest_index, workspace, "workspace",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_I64_REQ(latest_index, dirty, "dirty"),
    LONEJSON_FIELD_OBJECT(latest_index, latest, "latest", &session_map)};
LONEJSON_MAP_DEFINE(latest_map, latest_index, latest_fields);
/* Internal children are recovered by their recorded session ID, never by
 * workspace auto-resume. Their journals/metadata do not need a latest index. */
static int scope_indexed(const char *scope) {
  return strncmp(scope, "smith-review:", 13U) != 0 &&
         strncmp(scope, "smith-subagent:", 15U) != 0;
}
static int latest_open(cai_cli_pouch *, const char *, lc_lease **,
                       latest_index *, cai_error *);
static int latest_write(cai_cli_pouch *, lc_lease *, latest_index *,
                        cai_error *);
static int session_exists(cai_cli_pouch *, const char *, int *, cai_error *);
static int latest_rebuild(cai_cli_pouch *, latest_index *, cai_error *);
static int session_compare(const void *, const void *);
static int session_refresh(cai_cli_pouch *, cai_cli_session *, cai_error *);
static int latest_key(const char *, char[65], cai_error *);

typedef struct scope_predicate {
  char field[24];
  char value[PATH_MAX];
} scope_predicate;
static const lonejson_field scope_predicate_fields[] = {
    LONEJSON_FIELD_STRING_FIXED(scope_predicate, field, "field",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED(scope_predicate, value, "value",
                                LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(scope_predicate_map, scope_predicate,
                    scope_predicate_fields);
typedef struct scope_query {
  scope_predicate eq;
} scope_query;
static const lonejson_field scope_fields[] = {
    LONEJSON_FIELD_OBJECT(scope_query, eq, "eq", &scope_predicate_map)};
LONEJSON_MAP_DEFINE(scope_map, scope_query, scope_fields);

typedef struct cli_record {
  char record_type[24];
  lonejson_uint64 sequence;
  lonejson_uint64 timestamp_ns;
  lonejson_int64 utc_offset_seconds;
  lonejson_uint64 applied_event_sequence;
  lonejson_uint64 commit_created_at_ns;
  const char *type;
  const char *data;
  lonejson_json_value state;
  lonejson_json_value data_value;
} cli_record;
static const lonejson_field journal_event_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(cli_record, record_type, "record_type",
                                    LONEJSON_OVERFLOW_FAIL),
    /* Ordered reads also encounter commits. Validate required event fields at
     * the authenticated collection boundary instead of filtering/decrypting
     * every entry in LQL before parsing the same body again. */
    LONEJSON_FIELD_U64(cli_record, sequence, "sequence"),
    LONEJSON_FIELD_U64(cli_record, timestamp_ns, "timestamp_ns"),
    LONEJSON_FIELD_I64(cli_record, utc_offset_seconds, "utc_offset_seconds"),
    LONEJSON_FIELD_STRING_ALLOC(cli_record, type, "type"),
    LONEJSON_FIELD_STRING_ALLOC(cli_record, data, "data"),
    LONEJSON_FIELD_JSON_VALUE(cli_record, state, "state")};
LONEJSON_MAP_DEFINE(journal_event_map, cli_record, journal_event_fields);
static const lonejson_field commit_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(cli_record, record_type, "record_type",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_U64_REQ(cli_record, commit_created_at_ns,
                           "commit_created_at_ns"),
    LONEJSON_FIELD_U64_REQ(cli_record, applied_event_sequence,
                           "applied_event_sequence"),
    LONEJSON_FIELD_JSON_VALUE_REQ(cli_record, state, "state")};
LONEJSON_MAP_DEFINE(commit_map, cli_record, commit_fields);

static const lonejson_field import_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(cli_record, record_type, "record_type",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_U64(cli_record, sequence, "sequence"),
    LONEJSON_FIELD_U64(cli_record, commit_created_at_ns,
                       "commit_created_at_ns"),
    LONEJSON_FIELD_U64(cli_record, applied_event_sequence,
                       "applied_event_sequence"),
    LONEJSON_FIELD_STRING_ALLOC(cli_record, type, "type"),
    LONEJSON_FIELD_JSON_VALUE(cli_record, data_value, "data"),
    LONEJSON_FIELD_JSON_VALUE(cli_record, state, "state")};
LONEJSON_MAP_DEFINE(import_map, cli_record, import_fields);

typedef struct json_source {
  lonejson *json;
  lonejson_generator generator;
} json_source;

static size_t json_read(void *context, void *bytes, size_t count,
                        cai_error *error) {
  json_source *source;
  size_t n;
  int eof;
  source = (json_source *)context;
  n = 0U;
  if (lonejson_generator_read(&source->generator, bytes, count, &n, &eof) !=
      LONEJSON_STATUS_OK)
    cai_cli_lj_error(error, &source->generator.error);
  return n;
}

static void json_close(void *context) {
  json_source *source;
  source = (json_source *)context;
  lonejson_generator_cleanup(&source->generator);
  lonejson_free(source->json);
  free(source);
}

int cai_cli_mapped_source(const lonejson_map *map, const void *doc,
                          cai_source **out, cai_error *error) {
  json_source *source;
  cai_source_callbacks callbacks;
  lonejson_error local;
  int rc;
  source = (json_source *)calloc(1U, sizeof(*source));
  if (source == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate JSON generator");
  source->json = lonejson_new(NULL, &local);
  if (source->json == NULL) {
    free(source);
    return cai_cli_lj_error(error, &local);
  }
  if (lonejson_generator_init(source->json, &source->generator, map, doc) !=
      LONEJSON_STATUS_OK) {
    rc = cai_cli_lj_error(error, &source->generator.error);
    lonejson_free(source->json);
    free(source);
    return rc;
  }
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.read = json_read;
  callbacks.close = json_close;
  callbacks.context = source;
  rc = cai_source_from_callbacks(&callbacks, out, error);
  if (rc != CAI_OK)
    json_close(source);
  return rc;
}

typedef struct download {
  lc_client *client;
  lc_lease *lease;
  char *key;
  char *attachment;
  const char *ns;
} download;

static void download_close(void *context) {
  download *request;
  request = (download *)context;
  free(request->key);
  free(request->attachment);
  free(request);
}

static int download_write(void *context, cai_sink *sink, cai_error *error) {
  download *request;
  lc_sink *destination;
  lc_error local;
  lc_get_opts options;
  lc_get_res response;
  lc_attachment_get_op attachment;
  lc_attachment_get_res attached;
  int rc;
  request = (download *)context;
  destination = NULL;
  lc_error_init(&local);
  rc = cai_cli_lc_sink(sink, &destination, error);
  if (rc == CAI_OK) {
    if (request->lease != NULL && request->attachment != NULL) {
      lc_attachment_get_req private_request;
      lc_attachment_get_req_init(&private_request);
      memset(&attached, 0, sizeof(attached));
      private_request.selector.name = request->attachment;
      rc = lc_lease_get_attachment(request->lease, &private_request,
                                   destination, &attached, &local);
      lc_attachment_get_res_cleanup(&attached);
    } else if (request->lease != NULL) {
      memset(&response, 0, sizeof(response));
      rc = lc_lease_get(request->lease, destination, NULL, &response, &local);
      lc_get_res_cleanup(&response);
    } else if (request->attachment != NULL) {
      lc_attachment_get_op_init(&attachment);
      memset(&attached, 0, sizeof(attached));
      attachment.lease.ns = request->ns;
      attachment.lease.key = request->key;
      attachment.selector.name = request->attachment;
      attachment.public_read = 1;
      rc = lc_get_attachment(request->client, &attachment, destination,
                             &attached, &local);
      lc_attachment_get_res_cleanup(&attached);
    } else {
      memset(&response, 0, sizeof(response));
      memset(&options, 0, sizeof(options));
      options.public_read = 1;
      rc = lc_get_in_namespace(request->client, request->ns, request->key,
                               &options, destination, &response, &local);
      lc_get_res_cleanup(&response);
    }
    if (rc != LC_OK)
      rc = cai_cli_lc_error(error, &local);
  }
  lc_sink_close(destination);
  lc_error_cleanup(&local);
  return rc;
}

static int download_source_ns(lc_client *client, const char *ns,
                              const char *key, const char *attachment,
                              cai_source **out, cai_error *error) {
  download *request;
  int rc;
  request = (download *)calloc(1U, sizeof(*request));
  if (request == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate pouch download");
  request->client = client;
  request->ns = ns;
  request->key = strdup(key);
  request->attachment = attachment != NULL ? strdup(attachment) : NULL;
  if (request->key == NULL ||
      (attachment != NULL && request->attachment == NULL)) {
    download_close(request);
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate pouch download key");
  }
  rc = cai_cli_stream_source(download_write, request, download_close, out,
                             error);
  if (rc != CAI_OK)
    download_close(request);
  return rc;
}

static int download_source(cai_cli_pouch *pouch, const char *key,
                           const char *attachment, cai_source **out,
                           cai_error *error) {
  return download_source_ns(pouch->sessions, pouch->sessions_namespace, key,
                            attachment, out, error);
}

static int download_private(lc_lease *lease, const char *attachment,
                            cai_source **out, cai_error *error) {
  download *request = calloc(1U, sizeof(*request));
  int rc;
  if (request == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate private download");
  request->lease = lease;
  request->attachment = attachment != NULL ? strdup(attachment) : NULL;
  if (attachment != NULL && request->attachment == NULL) {
    download_close(request);
    return cai_cli_error(error, CAI_ERR_NOMEM, "copy private attachment name");
  }
  rc = cai_cli_stream_source(download_write, request, download_close, out,
                             error);
  if (rc != CAI_OK) {
    download_close(request);
    return rc;
  }
  rc = cai_cli_stream_ready(*out, error);
  if (rc != CAI_OK) {
    cai_source_close(*out);
    *out = NULL;
  }
  return rc;
}

typedef struct parse_input {
  cai_source *source;
  cai_error error;
  size_t bytes_read;
} parse_input;

static lonejson_read_result parse_read(void *context, unsigned char *bytes,
                                       size_t capacity) {
  parse_input *input;
  lonejson_read_result result;
  input = (parse_input *)context;
  result = lonejson_default_read_result();
  result.bytes_read =
      cai_source_read(input->source, bytes, capacity, &input->error);
  input->bytes_read += result.bytes_read;
  result.eof = result.bytes_read == 0U && input->error.code == CAI_OK;
  result.error_code = input->error.code == CAI_OK ? 0 : EIO;
  return result;
}

static int parse_source_optional(cai_source *source, const lonejson_map *map,
                                 void *doc, int *has_body, int *string_limit,
                                 cai_error *error) {
  lonejson *json;
  lonejson_error local;
  int rc;
  lonejson_status status;
  char message[512];
  parse_input input;
  if (string_limit != NULL)
    *string_limit = 0;
  json = lonejson_new(NULL, &local);
  if (json == NULL)
    return cai_cli_lj_error(error, &local);
  input.source = source;
  input.bytes_read = 0U;
  cai_error_init(&input.error);
  status = lonejson_parse_reader(json, map, doc, parse_read, &input, &local);
  /* lonejson reports all size overflows with one status. Restrict recovery to
   * its dynamic-string ceiling; malformed JSON, fixed identity overflow and
   * source/transport errors must still fail normally. */
  if (string_limit != NULL && input.error.code == CAI_OK &&
      local.code == LONEJSON_STATUS_OVERFLOW &&
      strstr(local.message, "exceeds configured max dynamic string bytes") !=
          NULL)
    *string_limit = 1;
  if (has_body != NULL)
    *has_body = input.bytes_read != 0U;
  rc = status == LONEJSON_STATUS_OK ||
               (has_body != NULL && !*has_body && input.error.code == CAI_OK)
           ? CAI_OK
           : cai_cli_lj_error(error, &local);
  if (rc != CAI_OK) {
    snprintf(message, sizeof(message), "parse stored %s: %s", map->name,
             input.error.message != NULL ? input.error.message : local.message);
    rc = cai_cli_error(error, rc, message);
    error->http_status = input.error.http_status;
  }
  cai_error_cleanup(&input.error);
  lonejson_free(json);
  return rc;
}

static int parse_source(cai_source *source, const lonejson_map *map, void *doc,
                        cai_error *error) {
  return parse_source_optional(source, map, doc, NULL, NULL, error);
}

/* Collection receivers borrow the existing shared client. Opening a receiver
 * never creates another Pouch root, client, or background worker. */
static int history_supported(cai_cli_pouch *pouch, cai_error *error) {
  lc_collection_capabilities caps;
  lc_error local;
  int rc;
  lc_error_init(&local);
  rc = lc_client_collection_capabilities(pouch->sessions, &caps, &local);
  if (rc == LC_OK &&
      (!caps.collections || !caps.leases || !caps.lease_takeover)) {
    lc_error_cleanup(&local);
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "Session history requires a Pouch endpoint; remote "
                         "lockd collections are not supported yet");
  }
  if (rc != LC_OK)
    rc = cai_cli_lc_error(error, &local);
  lc_error_cleanup(&local);
  return rc;
}
static int open_history_collection_options(cai_cli_pouch *pouch, const char *id,
                                           const char *kind, int create,
                                           int optional,
                                           const lc_collection_lease_req *lease,
                                           int *created, lc_collection **out,
                                           cai_error *error) {
  lc_collection_config config;
  lc_error local;
  char name[192];
  int rc;
  *out = NULL;
  if (created != NULL)
    *created = 0;
  lc_error_init(&local);
  rc = history_supported(pouch, error);
  if (rc != CAI_OK)
    return rc;
  lc_collection_config_init(&config);
  snprintf(name, sizeof(name), "%s.%s", kind, id);
  config.ns = pouch->sessions_namespace;
  config.name = name;
  rc = lc_client_open_collection(pouch->sessions, &config, out, &local);
  if (rc != LC_OK && optional && !create && local.server_code != NULL &&
      strcmp(local.server_code, "collection_not_found") == 0) {
    lc_error_cleanup(&local);
    return CAI_OK;
  }
  if (rc != LC_OK && create && local.server_code != NULL &&
      strcmp(local.server_code, "collection_not_found") == 0) {
    lc_error_cleanup(&local);
    lc_error_init(&local);
    if (strcmp(kind, "events") == 0) {
      config.has_retention_seconds = 1;
      config.retention_seconds = pouch->conversation_retention_seconds;
    }
    config.lease = lease;
    rc = lc_client_create_collection(pouch->sessions, &config, out, &local);
    if (rc == LC_OK && created != NULL)
      *created = 1;
    if (rc != LC_OK && local.server_code != NULL &&
        strcmp(local.server_code, "collection_exists") == 0) {
      lc_error_cleanup(&local);
      lc_error_init(&local);
      config.has_retention_seconds = 0;
      config.lease = NULL;
      rc = lc_client_open_collection(pouch->sessions, &config, out, &local);
    }
  }
  if (rc != LC_OK)
    rc = cai_cli_lc_error(error, &local);
  lc_error_cleanup(&local);
  return rc;
}
static int open_history_collection(cai_cli_pouch *pouch, const char *id,
                                   const char *kind, int create, int optional,
                                   lc_collection **out, cai_error *error) {
  return open_history_collection_options(pouch, id, kind, create, optional,
                                         NULL, NULL, out, error);
}

/* SDK maintenance owns expiry. Availability checks test only whether cleanup
 * has removed a collection, never compare clocks or delete data themselves. */
static int session_available(cai_cli_pouch *pouch, const char *id,
                             int *available, cai_error *error) {
  lc_collection *collection = NULL;
  int rc =
      open_history_collection(pouch, id, "events", 0, 1, &collection, error);
  *available = collection != NULL;
  lc_collection_close(collection);
  if (rc == CAI_OK && *available)
    rc = session_exists(pouch, id, available, error);
  return rc;
}

/* A stable read anchor avoids repeated close-time projection checkpoints.
 * Appends commit independently; lease release never publishes/rolls them back.
 * Callers own separate public readers, including streaming sources. */
static int history_collection(cai_cli_pouch *pouch, const char *id,
                              const char *kind, int create, lc_collection **out,
                              cai_error *error) {
  lc_collection *anchor = NULL;
  char name[192];
  int rc;
  *out = NULL;
  rc = history_supported(pouch, error);
  if (rc != CAI_OK)
    return rc;
  snprintf(name, sizeof(name), "%s.%s", kind, id);
  pthread_mutex_lock(&pouch->history_lock);
  if (pouch->history_anchor == NULL ||
      strcmp(pouch->history_anchor->name, name) != 0) {
    rc = open_history_collection(pouch, id, kind, create, 0, &anchor, error);
    if (rc == CAI_OK) {
      if (pouch->history_anchor != NULL)
        pouch->history_anchor->close(pouch->history_anchor);
      pouch->history_anchor = anchor;
    }
  }
  if (rc == CAI_OK)
    rc = open_history_collection(pouch, id, kind, 0, 0, out, error);
  pthread_mutex_unlock(&pouch->history_lock);
  return rc;
}

static int journal_write_ready(cai_cli_pouch *pouch, cai_error *error) {
  int uncertain;
  int rc = cai_cli_pouch_ownership_check(pouch, error);
  if (rc != CAI_OK)
    return rc;
  pthread_mutex_lock(&clients_lock);
  uncertain = pouch->connection != NULL && pouch->connection->write_uncertain;
  pthread_mutex_unlock(&clients_lock);
  return uncertain ? cai_cli_error(
                         error, CAI_ERR_TRANSPORT,
                         "A journal append has an unresolved outcome; reopen "
                         "the store before writing again")
                   : CAI_OK;
}

static int collection_append_source(cai_cli_pouch *pouch,
                                    lc_collection *collection,
                                    const char *append_id, cai_source *source,
                                    const long *retention_seconds,
                                    unsigned long long *position,
                                    int *duplicate, cai_error *error) {
  lc_collection_append_req request;
  lc_collection_append_res result;
  lc_source *input = NULL;
  lc_error local;
  int rc = journal_write_ready(pouch, error);
  if (rc != CAI_OK)
    return rc;
  lc_error_init(&local);
  lc_collection_append_req_init(&request);
  request.append_id = append_id;
  if (retention_seconds != NULL) {
    request.has_collection_retention_seconds = 1;
    request.collection_retention_seconds = *retention_seconds;
  }
  rc = cai_cli_lc_source(source, &input, error);
  if (rc == CAI_OK) {
    rc = collection->append(collection, &request, input, &result, &local);
    if (rc == LC_ERR_TRANSPORT || rc == LC_ERR_TIMEOUT) {
      lc_collection_append_status_res status;
      lc_error observed;
      lc_error_init(&observed);
      if (collection->append_status(collection, append_id, &status,
                                    &observed) == LC_OK) {
        if (status.state == LC_COLLECTION_APPEND_COMMITTED) {
          result = status.entry;
          rc = LC_OK;
          cai_error_cleanup(error);
          cai_error_init(error);
        }
      } else {
        pthread_mutex_lock(&clients_lock);
        if (pouch->connection != NULL)
          pouch->connection->write_uncertain = 1;
        pthread_mutex_unlock(&clients_lock);
      }
      lc_error_cleanup(&observed);
    }
    if (rc != LC_OK)
      rc = cai_cli_lc_error(error, &local);
    if (rc == LC_OK && result.deleted)
      rc = cai_cli_error(error, CAI_ERR_INVALID,
                         "session append identity was already deleted");
    else if (rc == LC_OK) {
      if (position != NULL)
        *position = result.position;
      if (duplicate != NULL)
        *duplicate = result.duplicate;
    }
  }
  lc_source_close(input);
  lc_error_cleanup(&local);
  return rc;
}

static int session_retention(cai_cli_pouch *pouch,
                             const cai_cli_session *session, long *seconds,
                             cai_error *error) {
  if ((session->retention_override != 0 && session->retention_override != 1) ||
      session->retention_days < 0 ||
      session->retention_days > LONG_MAX / 86400L)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "invalid stored session retention");
  *seconds = session->retention_override
                 ? (long)session->retention_days * 86400L
                 : pouch->conversation_retention_seconds;
  return CAI_OK;
}
static int collection_retain(lc_collection *collection, long seconds,
                             lc_error *error) {
  lc_collection_metadata_req request;
  lc_collection_info info;
  lc_collection_metadata_req_init(&request);
  request.has_retention_seconds = 1;
  request.retention_seconds = seconds;
  return collection->metadata(collection, &request, &info, error);
}
static int journal_append_source(cai_cli_pouch *pouch,
                                 const cai_cli_session *session,
                                 lc_collection *collection, const char *id,
                                 cai_source *source,
                                 unsigned long long *position,
                                 cai_error *error) {
  long seconds = 0L;
  int rc = session_retention(pouch, session, &seconds, error);
  if (rc == CAI_OK)
    rc = collection_append_source(pouch, collection, id, source, &seconds,
                                  position, NULL, error);
  return rc;
}

static int collection_copy_selected(lc_collection *collection, uint64_t after,
                                    uint64_t through, const char *selector,
                                    cai_sink *sink, cai_error *error) {
  lc_collection_read_opts options;
  lc_collection_read_res result;
  lc_sink *output = NULL;
  lc_error local;
  int rc;
  lc_error_init(&local);
  lc_collection_read_opts_init(&options);
  lc_collection_read_res_init(&result);
  options.has_after = 1;
  options.after = after;
  options.has_through = 1;
  options.through = through;
  options.selector_lql = selector;
  rc = cai_cli_lc_sink(sink, &output, error);
  if (rc == CAI_OK &&
      collection->read(collection, &options, output, &result, &local) != LC_OK)
    rc = cai_cli_lc_error(error, &local);
  if (rc == CAI_OK && through > after && result.completed_records == 0U)
    rc = cai_cli_error(error, CAI_ERR_INVALID,
                       "session commit or journal range is missing");
  lc_sink_close(output);
  lc_collection_read_res_cleanup(&result);
  lc_error_cleanup(&local);
  return rc;
}
static int collection_copy(lc_collection *collection, uint64_t after,
                           uint64_t through, cai_sink *sink, cai_error *error) {
  return collection_copy_selected(collection, after, through, NULL, sink,
                                  error);
}

typedef struct commit_download {
  lc_collection *collection;
} commit_download;

static int commit_download_write(void *context, cai_sink *sink,
                                 cai_error *error) {
  commit_download *request = context;
  /* The journal parser consumes commits and discards observation payloads
   * incrementally. Read the ordered collection without a redundant selector
   * pass. Final trim visibility is known before observation recovery starts. */
  return collection_copy(request->collection, 0U, UINT64_MAX, sink, error);
}
static void commit_download_close(void *context) {
  commit_download *request = context;
  request->collection->close(request->collection);
  free(request);
}
static int commit_source(cai_cli_pouch *pouch, const char *id, cai_source **out,
                         cai_error *error) {
  commit_download *request;
  int rc;
  request = calloc(1U, sizeof(*request));
  if (request == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate commit reader");
  rc = history_collection(pouch, id, "events", 0, &request->collection, error);
  if (rc == CAI_OK)
    rc = cai_cli_stream_source(commit_download_write, request,
                               commit_download_close, out, error);
  if (rc != CAI_OK) {
    if (request->collection != NULL)
      request->collection->close(request->collection);
    free(request);
  }
  return rc;
}
static int session_read(cai_cli_pouch *pouch, const char *id,
                        cai_cli_session *out, int *has_body, cai_error *error) {
  cai_source *source;
  int rc, found = 0;
  memset(out, 0, sizeof(*out));
  source = NULL;
  rc = download_source(pouch, id, NULL, &source, error);
  if (rc == CAI_OK)
    rc = parse_source_optional(source, &session_map, out, &found, NULL, error);
  if (has_body != NULL)
    *has_body = found;
  if (has_body == NULL && ((rc == CAI_OK && !found) ||
                           (rc != CAI_OK && error->http_status == 404))) {
    char message[256];
    snprintf(message, sizeof(message), "Session '%s' was not found.", id);
    rc = cai_cli_error(error, CAI_ERR_INVALID, message);
  }
  if (rc != CAI_OK) {
    char message[640];
    long http_status;
    http_status = error->http_status;
    snprintf(message, sizeof(message), "load session %s: %s", id,
             error->message != NULL ? error->message : "read failed");
    rc = cai_cli_error(error, rc, message);
    error->http_status = http_status;
  }
  cai_source_close(source);
  return rc;
}

int cai_cli_pouch_session(cai_cli_pouch *pouch, const char *id,
                          cai_cli_session *out, cai_error *error) {
  return session_read(pouch, id, out, NULL, error);
}

#include "pouch_guard.inc"
#include "pouch_lease_guard.inc"

/* Collection ownership reuses the native liveness-guard helpers above. */
#include "pouch_collection_owner.inc"

static int acquire_wait(cai_cli_pouch *pouch, const char *ns, const char *key,
                        long wait_seconds, lc_lease **out, cai_error *error) {
  lc_acquire_req request;
  lc_error local;
  cai_cli_lease_guard *guard = NULL;
  int rc;
  *out = NULL;
  ns = ns != NULL ? ns : pouch->sessions_namespace;
  rc = lease_guard_open(pouch, ns, key, wait_seconds, &guard, error);
  if (rc != CAI_OK)
    return rc;
  lc_acquire_req_init(&request);
  lc_error_init(&local);
  request.ns = ns;
  request.key = key;
  request.owner = pouch->program_name;
  request.ttl_seconds = 3600;
  request.block_seconds = guard != NULL ? 0L : wait_seconds;
  rc = pouch->sessions->acquire(pouch->sessions, &request, out, &local);
  /* Pouch reports a held lease as LC_ERR_INVALID. Inspect through the public
   * SDK instead of matching its human-readable error message. */
  if (rc == LC_ERR_INVALID && guard != NULL) {
    lc_error_cleanup(&local);
    lc_error_init(&local);
    rc = lease_guard_recover(pouch, ns, key, &local);
    if (rc == LC_OK)
      rc = pouch->sessions->acquire(pouch->sessions, &request, out, &local);
  }
  if (rc == LC_OK && guard != NULL) {
    guard->lease = *out;
    pthread_mutex_lock(&clients_lock);
    guard->next = lease_guards;
    lease_guards = guard;
    pthread_mutex_unlock(&clients_lock);
    guard = NULL;
  }
  if (rc != LC_OK)
    rc = cai_cli_lc_error(error, &local);
  lc_error_cleanup(&local);
  lease_guard_close(guard);
  return rc;
}

static int acquire_ns(cai_cli_pouch *pouch, const char *ns, const char *key,
                      lc_lease **out, cai_error *error) {
  return acquire_wait(pouch, ns, key, 5L, out, error);
}

static int acquire(cai_cli_pouch *pouch, const char *key, lc_lease **out,
                   cai_error *error) {
  return acquire_ns(pouch, NULL, key, out, error);
}

static int release_mode(lc_lease *lease, int rollback, int rc,
                        cai_error *error) {
  cai_cli_lease_guard *guard;
  lc_release_req request;
  lc_error local;
  int result;
  if (lease == NULL)
    return rc;
  guard = lease_guard_take(lease);
  lc_release_req_init(&request);
  request.rollback = rollback;
  lc_error_init(&local);
  result = lease->release(lease, &request, &local);
  if (result != LC_OK) {
    lc_lease_close(lease);
    if (rc == CAI_OK)
      rc = cai_cli_lc_error(error, &local);
  }
  lease_guard_close(guard);
  lc_error_cleanup(&local);
  return rc;
}

static int release(lc_lease *lease, int rc, cai_error *error) {
  return release_mode(lease, 0, rc, error);
}

static int update(lc_lease *lease, cai_source *source, cai_error *error) {
  lc_source *input;
  lc_error local;
  int rc;
  input = NULL;
  lc_error_init(&local);
  rc = cai_cli_lc_source(source, &input, error);
  if (rc == CAI_OK) {
    rc = lease->update(lease, input, NULL, &local);
    if (rc != LC_OK)
      rc = cai_cli_lc_error(error, &local);
  }
  lc_source_close(input);
  lc_error_cleanup(&local);
  return rc;
}

static int update_session(lc_lease *lease, cai_cli_session *session,
                          cai_error *error) {
  cai_source *source;
  lc_source *input = NULL;
  lc_update_opts options;
  lc_error local;
  int rc;
  source = NULL;
  lc_error_init(&local);
  lc_update_opts_init(&options);
  /* A lookup is a derived index. Only its backing collection owns expiry;
   * renewing a second state object on every stream delta is unnecessary. */
  options.has_retention_seconds = 1;
  options.retention_seconds = 0L;
  rc = cai_cli_mapped_source(&session_map, session, &source, error);
  if (rc == CAI_OK)
    rc = cai_cli_lc_source(source, &input, error);
  if (rc == CAI_OK && lease->update(lease, input, &options, &local) != LC_OK)
    rc = cai_cli_lc_error(error, &local);
  lc_source_close(input);
  cai_source_close(source);
  lc_error_cleanup(&local);
  return rc;
}

static int get_session(lc_lease *lease, const char *scope, const char *id,
                       cai_cli_session *session, cai_error *error) {
  lc_error local;
  lc_get_res response;
  int rc;
  memset(session, 0, sizeof(*session));
  if (lease->state_etag != NULL && lease->state_etag[0] != '\0') {
    lc_error_init(&local);
    memset(&response, 0, sizeof(response));
    rc = lease->load(lease, &session_map, session, NULL, &response, &local);
    if (rc != LC_OK)
      rc = cai_cli_lc_error(error, &local);
    lc_get_res_cleanup(&response);
    lc_error_cleanup(&local);
    if (rc != CAI_OK)
      return rc;
    if (strcmp(session->history_backend, "collections") != 0)
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "Session uses retired attachment-backed history; "
                           "start a new session with --new");
    if (scope != NULL && strcmp(session->workspace, scope) != 0)
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "session belongs to another directory");
  } else {
    if (scope == NULL)
      return cai_cli_error(error, CAI_ERR_INVALID, "session was not found");
    memset(session, 0, sizeof(*session));
    if (strlen(scope) >= sizeof(session->workspace) ||
        strlen(id) >= sizeof(session->id))
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "session identifier or workspace too long");
    strcpy(session->workspace, scope);
    strcpy(session->id, id);
    strcpy(session->history_backend, "collections");
    session->published = 1;
  }
  return CAI_OK;
}

int cai_cli_pouch_retention(cai_cli_pouch *pouch, const char *id, long *days,
                            cai_error *error) {
  cai_cli_session session;
  long seconds = 0L;
  int rc = cai_cli_pouch_session(pouch, id, &session, error);
  if (rc == CAI_OK)
    rc = session_retention(pouch, &session, &seconds, error);
  if (rc == CAI_OK)
    *days = seconds / 86400L;
  return rc;
}
int cai_cli_pouch_set_retention(cai_cli_pouch *pouch, const char *id, long days,
                                cai_error *error) {
  cai_cli_session session;
  lc_lease *lease = NULL;
  lc_collection *collection = NULL;
  lc_error local;
  int rc;
  if (days < 0L || days > LONG_MAX / 86400L)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "session retention day count is out of range");
  lc_error_init(&local);
  pthread_mutex_lock(&pouch->journal_lock);
  rc = journal_write_ready(pouch, error);
  if (rc == CAI_OK)
    rc = acquire(pouch, id, &lease, error);
  if (rc == CAI_OK)
    rc = get_session(lease, NULL, id, &session, error);
  if (rc == CAI_OK)
    rc = collection_owner_open(pouch, id, "events", 0, 0, &collection, error);
  if (rc == CAI_OK &&
      collection_retain(collection, days * 86400L, &local) != LC_OK)
    rc = cai_cli_lc_error(error, &local);
  if (rc == CAI_OK) {
    session.retention_override = 1;
    session.retention_days = days;
    rc = update_session(lease, &session, error);
  }
  lc_error_cleanup(&local);
  rc = release(lease, rc, error);
  if (rc == CAI_OK) {
    cai_cli_journal_context *entry = journal_context_find(pouch, id);
    if (entry != NULL) {
      entry->session.retention_override = session.retention_override;
      entry->session.retention_days = session.retention_days;
    }
  }
  pthread_mutex_unlock(&pouch->journal_lock);
  return rc;
}

#include "pouch_journal_context.inc"
#include "pouch_prompt.inc"

/** Index failures affect presentation availability, never acknowledgement of
 * already durable model input. A failed index stays unavailable until reopen.
 */
static void transcript_index_begin(cai_cli_pouch *pouch, const char *id) {
  cai_cli_replay_close(pouch->transcript_index);
  pouch->transcript_index = NULL;
  snprintf(pouch->transcript_index_session,
           sizeof(pouch->transcript_index_session), "%s", id);
  pouch->transcript_index_failed =
      cai_cli_replay_index_open(&pouch->transcript_index, NULL) != CAI_OK;
}
static void transcript_index_event(cai_cli_pouch *pouch, const char *id,
                                   const cai_agent_session_event *event) {
  if (strcmp(pouch->transcript_index_session, id) != 0)
    transcript_index_begin(pouch, id);
  if (pouch->transcript_index_failed)
    return;
  if (event->sequence <= cai_cli_replay_last(pouch->transcript_index))
    return; /* An acknowledged idempotent append is not another turn. */
  if (cai_cli_replay_feed(pouch->transcript_index, event, NULL) != CAI_OK) {
    cai_cli_replay_close(pouch->transcript_index);
    pouch->transcript_index = NULL;
    pouch->transcript_index_failed = 1;
  }
}
#include "pouch_trim.inc"

/** Per-invocation scratch belongs to the heap, including callback reentry. */
typedef struct commit_mode_scratch {
  cai_cli_session session;
  latest_index index;
} commit_mode_scratch;
static int
commit_mode_with_scratch(void *context, const char *scope, const char *id,
                         cai_source *state, unsigned long long sequence,
                         const cai_agent_session_trim_request *cut_request,
                         cai_agent_session_trim_source_fn make_replacement,
                         void *replacement_context, cai_error *error,
                         commit_mode_scratch *scratch) {
  cai_cli_pouch *pouch;

  lc_lease *lease;
  lc_lease *directory;
  lc_collection *collection = NULL;
  char append_id[96];
  unsigned long long position = 0U;

  struct timespec now;
  cli_record record;
  cai_source *source = NULL;
  cai_source *replacement_state = NULL;
  pouch_cut *cut = NULL;
  int rc, durable = 0;
  memset(&record, 0, sizeof(record));
  pouch = (cai_cli_pouch *)context;
  lease = NULL;
  directory = NULL;
  memset(&record, 0, sizeof(record));
  rc = session_claim(pouch, id, error);
  pthread_mutex_lock(&pouch->journal_lock);
  if (rc == CAI_OK)
    rc = journal_write_ready(pouch, error);
  if (rc == CAI_OK)
    rc = collection_owner_open(pouch, id, "events", 1, 0, &collection, error);
  if (rc == CAI_OK && scope_indexed(scope))
    rc = latest_open(pouch, scope, &directory, &scratch->index, error);
  if (rc == CAI_OK && directory != NULL) {
    scratch->index.dirty = 1;
    rc = latest_write(pouch, directory, &scratch->index, error);
  }
  if (rc == CAI_OK)
    rc = acquire(pouch, id, &lease, error);
  if (rc == CAI_OK)
    rc = get_session(lease, scope, id, &scratch->session, error);
  /* Publish the small identity before the first journal append. Recovery must
   * be able to find a durable append even if its later index update fails. */
  if (rc == CAI_OK &&
      (lease->state_etag == NULL || lease->state_etag[0] == '\0')) {
    rc = update_session(lease, &scratch->session, error);
    rc = release(lease, rc, error);
    lease = NULL;
    if (rc == CAI_OK)
      rc = acquire(pouch, id, &lease, error);
  }
  if (rc == CAI_OK && clock_gettime(CLOCK_REALTIME, &now) != 0)
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "read commit timestamp");
  if (rc == CAI_OK && cut_request != NULL) {
    rc = pouch_cut_prepare(collection, cut_request, &cut, error);
    if (rc == CAI_OK)
      rc = make_replacement(replacement_context, cut->plan.revision,
                            (size_t)cut->plan.bytes, &replacement_state, error);
    state = replacement_state;
  }
  if (rc == CAI_OK) {
    scratch->session.commit_ns = (lonejson_uint64)now.tv_sec * 1000000000ULL +
                                 (lonejson_uint64)now.tv_nsec;
    scratch->session.applied_sequence = sequence;
    snprintf(append_id, sizeof(append_id), "commit-%020llu-%020llu",
             (unsigned long long)scratch->session.commit_ns, sequence);
    memset(&record, 0, sizeof(record));
    strcpy(record.record_type, "commit");
    record.commit_created_at_ns = scratch->session.commit_ns;
    record.applied_event_sequence = sequence;
    lonejson_json_value_init(NULL, &record.state);
    if (lonejson_json_value_set_reader(&record.state, cai_cli_json_read, state,
                                       NULL) != LONEJSON_STATUS_OK)
      rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "prepare journal edit");
    if (rc == CAI_OK)
      rc = cai_cli_mapped_source(&commit_map, &record, &source, error);
    if (rc == CAI_OK && cut == NULL)
      rc = journal_append_source(pouch, &scratch->session, collection,
                                 append_id, source, &position, error);
    else if (rc == CAI_OK)
      rc = pouch_cut_publish(pouch, collection, &scratch->session, append_id,
                             source, cut, &position, error);
    durable = rc == CAI_OK;
    scratch->session.commit_position = position;
    if (rc == CAI_OK)
      rc = update_session(lease, &scratch->session, error);
  }
  cai_source_close(source);
  cai_source_close(replacement_state);
  pouch_cut_close(cut);
  lonejson_json_value_cleanup(&record.state);
  rc = release(lease, rc, error);
  if (rc == CAI_OK && directory != NULL) {
    if (scratch->session.published &&
        strcmp(scratch->session.id, scratch->index.latest.id) == 0 &&
        scratch->session.commit_ns < scratch->index.latest.commit_ns)
      rc = latest_rebuild(pouch, &scratch->index, error);
    else if (scratch->session.published &&
             (scratch->index.latest.id[0] == '\0' ||
              strcmp(scratch->session.id, scratch->index.latest.id) == 0 ||
              session_compare(&scratch->session, &scratch->index.latest) < 0))
      scratch->index.latest = scratch->session;
    if (rc == CAI_OK) {
      scratch->index.dirty = 0;
      rc = latest_write(pouch, directory, &scratch->index, error);
    }
  }
  rc = release(directory, rc, error);
  if (durable && rc != CAI_OK) {
    if (pouch->logger != NULL) {
      pslog_field fields[2];
      fields[0] = pslog_str("session_id", id);
      fields[1] = pslog_str("detail", error->message != NULL
                                          ? error->message
                                          : "metadata update failed");
      pouch->logger->warn(
          pouch->logger,
          "Journal commit is durable; session index metadata will be recovered",
          fields, 2U);
    }
    cai_error_cleanup(error);
    cai_error_init(error);
    rc = CAI_OK;
  }
  pthread_mutex_unlock(&pouch->journal_lock);
  return rc;
}

static int commit_mode(void *context, const char *scope, const char *id,
                       cai_source *state, unsigned long long sequence,
                       const cai_agent_session_trim_request *cut_request,
                       cai_agent_session_trim_source_fn make_replacement,
                       void *replacement_context, cai_error *error) {
  commit_mode_scratch *scratch =
      (commit_mode_scratch *)calloc(1U, sizeof(*scratch));
  int rc;
  if (scratch == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate commit_mode scratch");
  rc = commit_mode_with_scratch(context, scope, id, state, sequence,
                                cut_request, make_replacement,
                                replacement_context, error, scratch);
  free(scratch);
  return rc;
}

static int commit(void *context, const char *scope, const char *id,
                  cai_source *state, unsigned long long sequence,
                  cai_error *error) {
  return commit_mode(context, scope, id, state, sequence, NULL, NULL, NULL,
                     error);
}
static int trim_history(void *context, const char *scope, const char *id,
                        const cai_agent_session_trim_request *request,
                        cai_agent_session_trim_source_fn make_replacement,
                        void *replacement_context, unsigned long long sequence,
                        cai_error *error) {
  return commit_mode(context, scope, id, NULL, sequence, request,
                     make_replacement, replacement_context, error);
}

static int load_id_mode(void *context, const char *scope, const char *id,
                        cai_source **out, unsigned long long *sequence,
                        int writable, cai_error *error) {
  cai_cli_pouch *pouch;
  cai_cli_session session;
  int rc, available;
  pouch = (cai_cli_pouch *)context;
  *out = NULL;
  rc = writable ? session_claim(pouch, id, error) : CAI_OK;
  if (rc == CAI_OK)
    rc = history_supported(pouch, error);
  if (rc == CAI_OK)
    rc = cai_cli_pouch_session(pouch, id, &session, error);
  if (rc != CAI_OK)
    return rc;
  if (strcmp(scope, session.workspace) != 0)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "session belongs to another directory");
  if (strcmp(session.history_backend, "collections") != 0)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "Session uses retired attachment-backed history; "
                         "start a new session with --new");
  rc = session_available(pouch, id, &available, error);
  if (rc != CAI_OK)
    return rc;
  if (!available)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "Session journal is no longer available (it may "
                         "have expired); start a new session");
  if (writable) {
    lc_collection *owned = NULL;
    rc = collection_owner_open(pouch, id, "events", 0, 0, &owned, error);
    if (rc != CAI_OK)
      return rc;
  }
  if (session.commit_position == 0U) {
    rc = session_refresh(pouch, &session, error);
    if (rc != CAI_OK)
      return rc;
  }
  *sequence = session.applied_sequence;
  return commit_source(pouch, id, out, error);
}

static int load_id(void *context, const char *scope, const char *id,
                   cai_source **out, unsigned long long *sequence,
                   cai_error *error) {
  return load_id_mode(context, scope, id, out, sequence, 0, error);
}

int cai_cli_pouch_read_id(cai_cli_pouch *pouch, const char *scope,
                          const char *id, cai_source **out,
                          unsigned long long *sequence, cai_error *error) {
  return load_id(pouch, scope, id, out, sequence, error);
}

static int load_owned_id(void *context, const char *scope, const char *id,
                         cai_source **out, unsigned long long *sequence,
                         cai_error *error) {
  return load_id_mode(context, scope, id, out, sequence, 1, error);
}

static int session_compare(const void *left, const void *right) {
  const cai_cli_session *a;
  const cai_cli_session *b;
  a = (const cai_cli_session *)left;
  b = (const cai_cli_session *)right;
  if (a->commit_ns != b->commit_ns)
    return a->commit_ns > b->commit_ns ? -1 : 1;
  return strcmp(b->id, a->id);
}

typedef struct key_page {
  char keys[256][CAI_AGENT_SESSION_ID_MAX];
  size_t count;
  size_t length;
} key_page;
static int key_begin(void *context, lc_error *error) {
  key_page *page;
  (void)error;
  page = (key_page *)context;
  page->length = 0U;
  return page->count < 256U;
}
static int key_chunk(void *context, const char *bytes, size_t length,
                     lc_error *error) {
  key_page *page;
  (void)error;
  page = (key_page *)context;
  if (page->length + length >= CAI_AGENT_SESSION_ID_MAX)
    return 0;
  memcpy(page->keys[page->count] + page->length, bytes, length);
  page->length += length;
  return 1;
}
static int key_end(void *context, lc_error *error) {
  key_page *page;
  (void)error;
  page = (key_page *)context;
  page->keys[page->count++][page->length] = '\0';
  return 1;
}

static int session_list(cai_cli_pouch *pouch, const char *scope, int visible,
                        cai_cli_session **out, size_t *count,
                        cai_error *error) {
  lc_query_req query;
  lc_query_res result;
  lc_query_key_handler handler;
  lc_error local;
  key_page *page;
  cai_cli_session session;
  cai_cli_session *grown;
  char *cursor;
  char *selector;
  lonejson *json;
  lonejson_error json_error;
  size_t i;
  int rc;
  int exists, available;
  *out = NULL;
  *count = 0U;
  rc = history_supported(pouch, error);
  if (rc != CAI_OK)
    return rc;
  cursor = NULL;
  selector = NULL;
  json = lonejson_new(NULL, &json_error);
  if (json == NULL)
    return cai_cli_lj_error(error, &json_error);
  /* LQL literal escaping is delegated to lonejson; selectors never interpolate
   * raw paths. */
  if (scope != NULL) {
    scope_query doc;
    memset(&doc, 0, sizeof(doc));
    strcpy(doc.eq.field, "/workspace");
    strcpy(doc.eq.value, scope);
    selector =
        lonejson_serialize_alloc(json, &scope_map, &doc, NULL, &json_error);
    if (selector == NULL) {
      lonejson_free(json);
      return cai_cli_lj_error(error, &json_error);
    }
  }
  page = (key_page *)calloc(1U, sizeof(*page));
  if (page == NULL) {
    free(selector);
    lonejson_free(json);
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate session query page");
  }
  lc_error_init(&local);
  lc_query_req_init(&query);
  memset(&handler, 0, sizeof(handler));
  handler.begin = key_begin;
  handler.chunk = key_chunk;
  handler.end = key_end;
  query.selector_json = selector;
  query.ns = pouch->sessions_namespace;
  query.selector_lql =
      scope == NULL && visible ? "eq{field=/published,value=1}" : NULL;
  query.engine = "index";
  query.refresh = "wait_for";
  query.limit = 256;
  rc = CAI_OK;
  do {
    memset(page, 0, sizeof(*page));
    memset(&result, 0, sizeof(result));
    query.cursor = cursor;
    if (lc_query_keys(pouch->sessions, &query, &handler, page, &result,
                      &local) != LC_OK)
      rc = cai_cli_lc_error(error, &local);
    free(cursor);
    cursor = result.cursor != NULL ? strdup(result.cursor) : NULL;
    if (result.cursor != NULL && cursor == NULL)
      rc = cai_cli_error(error, CAI_ERR_NOMEM, "allocate query cursor");
    lc_query_res_cleanup(&result);
    for (i = 0U; rc == CAI_OK && i < page->count; i++) {
      /* Removed or uncommitted imports can have indexed keys but no public
       * state body. Skip those; malformed nonempty metadata still fails. */
      rc = session_read(pouch, page->keys[i], &session, &exists, error);
      if (rc == CAI_OK && !exists)
        continue;
      if (rc != CAI_OK)
        break;
      if ((visible && !session.published) ||
          (scope != NULL && strcmp(scope, session.workspace) != 0))
        continue;
      if (strcmp(session.id, page->keys[i]) != 0) {
        rc = cai_cli_error(error, CAI_ERR_INVALID,
                           "session metadata identity does not match its key");
        break;
      }
      if (visible) {
        rc = session_available(pouch, session.id, &available, error);
        if (rc != CAI_OK)
          break;
        if (!available) {
          rc = journal_prune_missing_hint(pouch, &session, error);
          if (rc != CAI_OK)
            break;
          continue;
        }
      }
      if (visible == 2 || (visible && session.commit_position == 0U)) {
        rc = session_refresh(pouch, &session, error);
        if (rc != CAI_OK)
          break;
      }
      if (visible && session.commit_position == 0U)
        continue;
      grown = (cai_cli_session *)realloc(*out, (*count + 1U) * sizeof(**out));
      if (grown == NULL) {
        rc = cai_cli_error(error, CAI_ERR_NOMEM, "allocate session list");
        break;
      }
      *out = grown;
      (*out)[(*count)++] = session;
    }
  } while (rc == CAI_OK && cursor != NULL && cursor[0] != '\0');
  free(cursor);
  free(selector);
  free(page);
  lonejson_free(json);
  lc_error_cleanup(&local);
  if (rc != CAI_OK) {
    free(*out);
    *out = NULL;
    *count = 0U;
  } else if (*count > 1U) {
    qsort(*out, *count, sizeof(**out), session_compare);
  }
  return rc;
}

int cai_cli_pouch_list(cai_cli_pouch *pouch, const char *scope,
                       cai_cli_session **out, size_t *count, cai_error *error) {
  return session_list(pouch, scope, 1, out, count, error);
}

static int latest_write(cai_cli_pouch *pouch, lc_lease *guard,
                        latest_index *index, cai_error *error) {
  lc_lease *lease = NULL;
  cai_source *source = NULL;
  int rc = acquire_ns(pouch, pouch->scopes_namespace, guard->key + 5U, &lease,
                      error);
  if (rc == CAI_OK)
    rc = cai_cli_mapped_source(&latest_map, index, &source, error);
  if (rc == CAI_OK)
    rc = update(lease, source, error);
  cai_source_close(source);
  /* Lease updates are staged. Releasing this separate document lease commits
   * dirty before any session mutation, while guard still serializes writers. */
  return release(lease, rc, error);
}

static int latest_rebuild(cai_cli_pouch *pouch, latest_index *index,
                          cai_error *error) {
  cai_cli_session *sessions = NULL;
  size_t count = 0U;
  int rc = session_list(pouch, index->workspace, 2, &sessions, &count, error);
  if (rc == CAI_OK) {
    memset(&index->latest, 0, sizeof(index->latest));
    if (count != 0U)
      index->latest = sessions[0];
    index->dirty = 0;
  }
  free(sessions);
  return rc;
}

static int latest_key(const char *scope, char key[65], cai_error *error) {
  unsigned char digest[32];
  unsigned int length;
  size_t i;
  if (strlen(scope) >= PATH_MAX)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "directory index scope too long");
  if (EVP_Digest(scope, strlen(scope), digest, &length, EVP_sha256(), NULL) !=
          1 ||
      length != sizeof(digest))
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "hash directory index key");
  for (i = 0U; i < sizeof(digest); i++)
    snprintf(key + i * 2U, 65U - i * 2U, "%02x", digest[i]);
  return CAI_OK;
}

static int latest_open_wait(cai_cli_pouch *pouch, const char *scope,
                            long wait_seconds, lc_lease **out,
                            latest_index *index, cai_error *error) {
  char key[70];
  lc_lease *head = NULL;
  lc_get_res response;
  lc_error local;
  int rc;
  *out = NULL;
  memset(index, 0, sizeof(*index));
  memcpy(key, "lock-", 5U);
  rc = latest_key(scope, key + 5U, error);
  if (rc != CAI_OK)
    return rc;
  rc = acquire_wait(pouch, pouch->scopes_namespace, key, wait_seconds, out,
                    error);
  if (rc != CAI_OK)
    return rc;
  rc = acquire_ns(pouch, pouch->scopes_namespace, key + 5U, &head, error);
  if (rc != CAI_OK)
    return rc;
  lc_error_init(&local);
  memset(&response, 0, sizeof(response));
  if (head->state_etag != NULL && head->state_etag[0] != '\0') {
    rc = head->load(head, &latest_map, index, NULL, &response, &local);
    if (rc != LC_OK)
      rc = cai_cli_lc_error(error, &local);
    else if (strcmp(scope, index->workspace) != 0)
      rc = cai_cli_error(error, CAI_ERR_INVALID,
                         "directory index scope mismatch");
  } else {
    strcpy(index->workspace, scope);
    index->dirty = 1;
  }
  lc_get_res_cleanup(&response);
  lc_error_cleanup(&local);
  rc = release(head, rc, error);
  if (rc == CAI_OK && !index->dirty && index->latest.id[0] != '\0') {
    int available;
    rc = session_available(pouch, index->latest.id, &available, error);
    if (rc == CAI_OK && !available)
      index->dirty = 1;
  }
  if (rc == CAI_OK && index->dirty) {
    pslog_field field = pslog_str("workspace", scope);
    if (pouch->logger != NULL)
      pouch->logger->info(pouch->logger, "Rebuilding directory session index",
                          &field, 1U);
    rc = latest_rebuild(pouch, index, error);
    if (rc == CAI_OK)
      rc = latest_write(pouch, *out, index, error);
  }
  return rc;
}

static int latest_open(cai_cli_pouch *pouch, const char *scope, lc_lease **out,
                       latest_index *index, cai_error *error) {
  return latest_open_wait(pouch, scope, 5L, out, index, error);
}

#include "pouch_approval.inc"

/** Per-invocation scratch belongs to the heap, including callback reentry. */
typedef struct load_latest_mode_scratch {
  latest_index index;
} load_latest_mode_scratch;
static int load_latest_mode_with_scratch(void *context, const char *scope,
                                         char *id, size_t capacity,
                                         cai_source **out,
                                         unsigned long long *sequence,
                                         int writable, cai_error *error,
                                         load_latest_mode_scratch *scratch) {
  cai_cli_pouch *pouch = context;
  lc_lease *directory = NULL;
  cai_source *source = NULL;

  char key[65];
  int has_body = 0;
  int rc;
  *out = NULL;
  memset(&scratch->index, 0, sizeof(scratch->index));
  rc = history_supported(pouch, error);
  if (rc != CAI_OK || !scope_indexed(scope))
    return rc;
  if (rc == CAI_OK)
    rc = latest_key(scope, key, error);
  if (rc == CAI_OK)
    rc = download_source_ns(pouch->sessions, pouch->scopes_namespace, key, NULL,
                            &source, error);
  if (rc == CAI_OK)
    rc = parse_source_optional(source, &latest_map, &scratch->index, &has_body,
                               NULL, error);
  cai_source_close(source);
  if (rc != CAI_OK && error->http_status == 404) {
    cai_error_cleanup(error);
    cai_error_init(error);
    rc = CAI_OK;
    has_body = 0;
  }
  if (rc == CAI_OK && has_body && strcmp(scope, scratch->index.workspace) != 0)
    rc =
        cai_cli_error(error, CAI_ERR_INVALID, "directory index scope mismatch");
  if (rc == CAI_OK && has_body && !scratch->index.dirty &&
      scratch->index.latest.id[0] != '\0') {
    int available;
    rc = session_available(pouch, scratch->index.latest.id, &available, error);
    if (rc == CAI_OK && !available)
      scratch->index.dirty = 1;
  }
  if (rc == CAI_OK && (!has_body || scratch->index.dirty)) {
    cai_error optional;
    cai_error_init(&optional);
    rc = latest_open_wait(pouch, scope, 0L, &directory, &scratch->index,
                          &optional);
    rc = release(directory, rc, &optional);
    /* A concurrent/interrupted writer must not delay prompt readiness. If its
     * guard is held, select authoritative committed metadata without changing
     * that writer's dirty index. The next writer repairs it under the guard. */
    if (rc != CAI_OK) {
      pslog_field fields[2];
      fields[0] = pslog_str("workspace", scope);
      fields[1] =
          pslog_str("reason", optional.message != NULL ? optional.message
                                                       : "index unavailable");
      if (pouch->logger != NULL)
        pouch->logger->debug(pouch->logger,
                             "Selecting newest session from metadata", fields,
                             2U);
      memset(&scratch->index, 0, sizeof(scratch->index));
      strcpy(scratch->index.workspace, scope);
      rc = latest_rebuild(pouch, &scratch->index, error);
    }
    cai_error_cleanup(&optional);
  }
  if (rc == CAI_OK && scratch->index.latest.id[0] != '\0') {
    if (strlen(scratch->index.latest.id) >= capacity)
      rc = cai_cli_error(error, CAI_ERR_INVALID, "session ID output too small");
    else {
      strcpy(id, scratch->index.latest.id);
      rc = load_id_mode(context, scope, id, out, sequence, writable, error);
    }
  }
  return rc;
}

static int load_latest_mode(void *context, const char *scope, char *id,
                            size_t capacity, cai_source **out,
                            unsigned long long *sequence, int writable,
                            cai_error *error) {
  load_latest_mode_scratch *scratch =
      (load_latest_mode_scratch *)calloc(1U, sizeof(*scratch));
  int rc;
  if (scratch == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM,
                         "allocate load_latest_mode scratch");
  rc = load_latest_mode_with_scratch(context, scope, id, capacity, out,
                                     sequence, writable, error, scratch);
  free(scratch);
  return rc;
}

static int load_latest(void *context, const char *scope, char *id,
                       size_t capacity, cai_source **out,
                       unsigned long long *sequence, cai_error *error) {
  return load_latest_mode(context, scope, id, capacity, out, sequence, 1,
                          error);
}

static void preview(char *out, size_t capacity, const char *text) {
  size_t n;
  n = 0U;
  while (text != NULL && *text != '\0' && n + 1U < capacity) {
    out[n++] = (unsigned char)*text < 32U || *text == 127 ? ' ' : *text;
    text++;
  }
  /* Drop any incomplete UTF-8 sequence at the truncation boundary. */
  if (text != NULL && *text != '\0') {
    if (n > 3U)
      n -= 3U;
    while (n > 0U && ((unsigned char)out[n - 1U] & 0xc0U) == 0x80U)
      n--;
    if (n > 0U && (unsigned char)out[n - 1U] >= 0xc0U)
      n--;
    if (n + 4U <= capacity) {
      memcpy(out + n, "...", 3U);
      n += 3U;
    }
  }
  out[n] = '\0';
}

static int append_event(void *context, const char *scope, const char *id,
                        const cai_agent_session_event *event,
                        cai_error *error) {
  cai_cli_pouch *pouch;
  cai_cli_session session;
  cai_cli_journal_context *journal = NULL;
  cai_agent_session_event visible;
  lc_lease *lease;
  cai_source *source;
  char name[64];
  unsigned long long position = 0U;
  int rc;
  rc = cai_agent_session_event_project_input(event, &visible, error);
  if (rc != CAI_OK)
    return rc;
  pouch = (cai_cli_pouch *)context;
  lease = NULL;
  source = NULL;
  snprintf(name, sizeof(name), "event-%020llu", event->sequence);
  rc = session_claim(pouch, id, error);
  pthread_mutex_lock(&pouch->journal_lock);
  if (rc == CAI_OK)
    rc = journal_write_ready(pouch, error);
  if (rc == CAI_OK)
    rc = history_supported(pouch, error);
  if (rc == CAI_OK)
    rc = journal_context_open(pouch, scope, id, &journal, error);
  if (rc == CAI_OK)
    rc = cai_session_event_source(event, &source, error);
  if (rc == CAI_OK)
    rc = journal_append_source(pouch, &journal->session, journal->collection,
                               name, source, &position, error);
  if (rc == CAI_OK && scope_indexed(scope))
    transcript_index_event(pouch, id, event);
  if (rc == CAI_OK && pouch->journal_observer != NULL)
    pouch->journal_observer(pouch->journal_observer_context, id, position,
                            event);
  /* Children already forward tagged events into the parent journal. Publish
   * that presentation stream once, rather than their unscoped private copy. */
  if (rc == CAI_OK && scope_indexed(scope) && pouch->network_observer != NULL)
    pouch->network_observer(pouch->network_observer_context, id, position,
                            event);
  if (rc == CAI_OK && journal->session.first_prompt[0] == '\0' &&
      visible.data != NULL &&
      (strcmp(visible.type, "turn_submitted") == 0 ||
       strcmp(visible.type, "turn_queued") == 0)) {
    rc = acquire(pouch, id, &lease, error);
    if (rc == CAI_OK)
      rc = get_session(lease, scope, id, &session, error);
    if (rc == CAI_OK) {
      if (session.first_prompt[0] == '\0') {
        preview(session.first_prompt, sizeof(session.first_prompt),
                visible.data);
        rc = update_session(lease, &session, error);
      }
      if (rc == CAI_OK)
        memcpy(journal->session.first_prompt, session.first_prompt,
               sizeof(session.first_prompt));
    }
  }
  cai_source_close(source);
  rc = release(lease, rc, error);
  pthread_mutex_unlock(&pouch->journal_lock);
  return rc;
}

static int session_presence_write(lc_sink *sink, const void *bytes,
                                  size_t count, lc_error *error) {
  (void)sink;
  (void)bytes;
  (void)count;
  (void)error;
  return 1;
}
static int session_exists(cai_cli_pouch *pouch, const char *id, int *exists,
                          cai_error *error) {
  lc_get_opts options;
  lc_get_res response;
  lc_sink sink;
  lc_error local;
  int rc;
  *exists = 0;
  lc_get_opts_init(&options);
  options.public_read = 1;
  memset(&sink, 0, sizeof(sink));
  sink.write = session_presence_write;
  memset(&response, 0, sizeof(response));
  lc_error_init(&local);
  /* A private etag can survive body deletion as lease metadata. Inspect the
   * committed public body, streaming only the small identity document into a
   * discard sink: no JSON allocation, download thread or history read. */
  rc = lc_get_in_namespace(pouch->sessions, pouch->sessions_namespace, id,
                           &options, &sink, &response, &local);
  if (rc != LC_OK && local.http_status != 404 &&
      (local.server_code == NULL ||
       strcmp(local.server_code, "not_found") != 0))
    rc = cai_cli_lc_error(error, &local);
  else {
    *exists = rc == LC_OK && !response.no_content;
    rc = CAI_OK;
  }
  lc_get_res_cleanup(&response);
  lc_error_cleanup(&local);
  return rc;
}

typedef struct collection_events {
  cai_cli_pouch *pouch;
  const char *id;
  lonejson *json;
  const lonejson_map *map;
  lonejson_curl_parse parser;
  cli_record record;
  int initialized, limited, rc, observe, discard_data;
  unsigned long long previous;
  unsigned long long after, through;
  int index_positions;
  uint64_t position;
  int (*select_event)(void *, unsigned long long);
  cai_agent_session_event_fn visit;
  void *visitor;
  cai_error *error;
} collection_events;

typedef struct cai_cli_event_position {
  unsigned long long sequence, position;
} cai_cli_event_position;

static int event_position_add(collection_events *read,
                              const lc_collection_read_metadata *metadata) {
  cai_cli_pouch *pouch = read->pouch;
  cai_cli_event_position *points;
  size_t capacity;
  if (!read->index_positions)
    return CAI_OK;
  pouch->event_last_sequence = read->record.sequence;
  pouch->event_last_position = metadata->position;
  if (pouch->event_position_stride++ % 128U != 0U)
    return CAI_OK;
  if (pouch->event_position_count == pouch->event_position_capacity) {
    capacity = pouch->event_position_capacity != 0U
                   ? pouch->event_position_capacity * 2U
                   : 32U;
    if (capacity < pouch->event_position_capacity ||
        capacity > (size_t)-1 / sizeof(*points))
      return cai_cli_error(read->error, CAI_ERR_LIMIT,
                           "replay position index exceeds addressable size");
    points = realloc(pouch->event_positions, capacity * sizeof(*points));
    if (points == NULL)
      return cai_cli_error(read->error, CAI_ERR_NOMEM,
                           "allocate replay position index");
    pouch->event_positions = points;
    pouch->event_position_capacity = capacity;
  }
  points = pouch->event_positions + pouch->event_position_count++;
  points->sequence = read->record.sequence;
  points->position = metadata->position;
  return CAI_OK;
}

static void event_position_window(const cai_cli_pouch *pouch, const char *id,
                                  unsigned long long after,
                                  unsigned long long through,
                                  lc_collection_read_opts *options) {
  size_t i;
  if (through == 0U || strcmp(pouch->event_position_session, id) != 0 ||
      pouch->event_last_sequence == 0U || through > pouch->event_last_sequence)
    return;
  options->has_after = options->has_through = 1;
  options->after = 0U;
  options->through = pouch->event_last_position;
  for (i = 0U; i < pouch->event_position_count; i++) {
    const cai_cli_event_position *point = &pouch->event_positions[i];
    if (point->sequence <= after)
      options->after = point->position;
    if (point->sequence >= through) {
      options->through = point->position;
      break;
    }
  }
}

static lonejson_status discard_json(void *context, const void *bytes,
                                    size_t count, lonejson_error *error) {
  (void)context;
  (void)bytes;
  (void)count;
  (void)error;
  return LONEJSON_STATUS_OK;
}

static void collection_event_cleanup(collection_events *read) {
  lonejson_curl_parse_cleanup(&read->parser);
  if (read->map != NULL)
    lonejson_cleanup(read->map, &read->record, NULL);
  memset(&read->parser, 0, sizeof(read->parser));
  memset(&read->record, 0, sizeof(read->record));
  read->initialized = 0;
}
/* The public push parser accepts arbitrary chunks despite its curl adapter
 * name. Collection events are parsed on the caller's thread without a pull
 * bridge, per-entry producer thread, or complete JSON download buffer. */
static int collection_event_write(lc_sink *sink, const void *bytes,
                                  size_t count, lc_error *error) {
  collection_events *read = sink->impl;
  const unsigned char *input = bytes;
  char chunk[4096];
  size_t amount;
  (void)error;
  if (!read->initialized) {
    read->initialized = 1;
    if (read->discard_data) {
      lonejson_error local;
      lonejson_json_value_init(read->json, &read->record.state);
      if (lonejson_json_value_set_parse_sink(&read->record.state, discard_json,
                                             NULL,
                                             &local) != LONEJSON_STATUS_OK)
        read->rc = cai_cli_lj_error(read->error, &local);
    }
    if (read->rc == CAI_OK &&
        lonejson_curl_parse_init(&read->parser, read->json, read->map,
                                 &read->record) != LONEJSON_STATUS_OK)
      read->rc = cai_cli_lj_error(read->error, &read->parser.error);
  }
  while (count != 0U && read->rc == CAI_OK && !read->limited) {
    amount = count < sizeof(chunk) ? count : sizeof(chunk);
    memcpy(chunk, input, amount);
    if (lonejson_curl_write_callback(chunk, 1U, amount, &read->parser) !=
        amount) {
      read->limited =
          read->parser.error.code == LONEJSON_STATUS_OVERFLOW &&
          strstr(read->parser.error.message,
                 "exceeds configured max dynamic string bytes") != NULL;
      if (!read->limited)
        read->rc = cai_cli_lj_error(read->error, &read->parser.error);
    }
    count -= amount;
    input += amount;
  }
  /* Oversized records are drained only to their collection boundary. The
   * parser's allocation limit remains in force; no whole JSONL buffer. */
  return read->rc == CAI_OK;
}
static int
collection_event_boundary(void *context,
                          const lc_collection_read_metadata *metadata,
                          lc_error *error) {
  collection_events *read = context;
  cai_agent_session_event event;
  char expected[64], *end;
  unsigned long long sequence;
  (void)error;
  if (metadata->kind != LC_COLLECTION_READ_METADATA_ENTRY)
    return LC_OK;
  if (!read->limited && read->rc == CAI_OK &&
      lonejson_curl_parse_finish(&read->parser) != LONEJSON_STATUS_OK)
    read->rc = cai_cli_lj_error(read->error, &read->parser.error);
  if (read->rc == CAI_OK && !read->limited &&
      strcmp(read->record.record_type, "commit") == 0) {
    if (metadata->append_id == NULL ||
        strncmp(metadata->append_id, "commit-", 7U) != 0)
      read->rc = cai_cli_error(read->error, CAI_ERR_INVALID,
                               "invalid stored commit identity");
    collection_event_cleanup(read);
    return read->rc == CAI_OK ? LC_OK : LC_ERR_INVALID;
  }
  sequence = 0U;
  if (metadata->append_id != NULL &&
      strncmp(metadata->append_id, "event-", 6U) == 0) {
    sequence = strtoull(metadata->append_id + 6U, &end, 10);
    snprintf(expected, sizeof(expected), "event-%020llu", sequence);
    if (*end != '\0' || strcmp(expected, metadata->append_id) != 0)
      sequence = 0U;
  }
  if (read->rc == CAI_OK &&
      (sequence == 0U || sequence <= read->previous ||
       (!read->limited &&
        (read->record.sequence != sequence ||
         (read->map == &journal_event_map && read->record.type == NULL) ||
         strcmp(read->record.record_type, "event") != 0))))
    read->rc = cai_cli_error(read->error, CAI_ERR_INVALID,
                             "invalid stored event identity or order");
  if (read->rc == CAI_OK && read->limited && read->pouch->logger != NULL) {
    pslog_field fields[2];
    fields[0] = pslog_str("session_id", read->id);
    fields[1] = pslog_u64("sequence", sequence);
    read->pouch->logger->warn(
        read->pouch->logger,
        "Skipped oversized session event during replay; stored record "
        "preserved, continuing with the remaining session",
        fields, 2U);
  }
  if (read->rc == CAI_OK) {
    /* Oversized events retain their native identity, too. */
    read->record.sequence = sequence;
    read->rc = event_position_add(read, metadata);
  }
  if (read->rc == CAI_OK) {
    read->previous = sequence;
    read->position = metadata->position;
    event.sequence = sequence;
    event.timestamp_ns = read->record.timestamp_ns;
    event.utc_offset_seconds = read->record.utc_offset_seconds;
    event.type = read->limited ? "replay_event_omitted" : read->record.type;
    event.data = read->limited ? NULL : read->record.data;
    if (read->index_positions) {
      cai_agent_session_event indexed = event;
      /* The already parsed type is sufficient for oversized output. Only a
       * delivery whose provenance cannot be decoded makes selection fail. */
      indexed.type = read->record.type != NULL ? read->record.type : event.type;
      transcript_index_event(read->pouch, read->id, &indexed);
    }
    if (sequence > read->after &&
        (read->through == 0U || sequence <= read->through) &&
        (read->select_event == NULL ||
         read->select_event(read->visitor, sequence))) {
      read->rc = read->visit(read->visitor, &event, read->error);
      if (read->rc == CAI_OK && !read->limited && read->observe &&
          read->pouch->replay_event != NULL)
        read->rc = read->pouch->replay_event(read->pouch->replay_context,
                                             &event, read->error);
    }
  }
  collection_event_cleanup(read);
  read->limited = 0;
  return read->rc == CAI_OK ? LC_OK : LC_ERR_INVALID;
}

typedef struct commit_refresh {
  collection_events read;
  cai_cli_session *session;
} commit_refresh;
static int commit_refresh_boundary(void *context,
                                   const lc_collection_read_metadata *metadata,
                                   lc_error *error) {
  commit_refresh *refresh = context;
  collection_events *read = &refresh->read;
  (void)error;
  if (metadata->kind != LC_COLLECTION_READ_METADATA_ENTRY)
    return LC_OK;
  if (lonejson_curl_parse_finish(&read->parser) != LONEJSON_STATUS_OK)
    read->rc = cai_cli_lj_error(read->error, &read->parser.error);
  if (read->rc == CAI_OK && strcmp(read->record.record_type, "commit") != 0)
    read->rc = cai_cli_error(read->error, CAI_ERR_INVALID,
                             "invalid journal commit record");
  if (read->rc == CAI_OK) {
    refresh->session->commit_position = metadata->position;
    refresh->session->applied_sequence = read->record.applied_event_sequence;
    if (read->record.commit_created_at_ns > refresh->session->commit_ns)
      refresh->session->commit_ns = read->record.commit_created_at_ns;
  }
  collection_event_cleanup(read);
  return read->rc == CAI_OK ? LC_OK : LC_ERR_INVALID;
}
/* Only dirty-index recovery scans committed headers. The normal startup/list
 * path keeps its small metadata lookup; payloads are discarded incrementally.
 */
static int session_refresh(cai_cli_pouch *pouch, cai_cli_session *session,
                           cai_error *error) {
  commit_refresh refresh;
  lc_collection *collection = NULL;
  lc_collection_read_opts options;
  lc_collection_read_res result;
  lc_sink sink;
  lc_error detail;
  lonejson_error local;
  lonejson_config config = lonejson_default_config();
  int rc;
  memset(&refresh, 0, sizeof(refresh));
  memset(&sink, 0, sizeof(sink));
  lc_error_init(&detail);
  lc_collection_read_opts_init(&options);
  lc_collection_read_res_init(&result);
  config.clear_destination_by_default = 0;
  refresh.read.map = &commit_map;
  refresh.read.discard_data = 1;
  refresh.read.error = error;
  config.json_value_max_total_bytes = 0U;
  config.json_value_max_string_bytes = 0U;
  refresh.session = session;
  refresh.read.json = lonejson_new(&config, &local);
  rc = refresh.read.json != NULL ? CAI_OK : cai_cli_lj_error(error, &local);
  if (rc == CAI_OK)
    rc =
        history_collection(pouch, session->id, "events", 0, &collection, error);
  if (rc != CAI_OK && error->http_status == 404) {
    cai_error_cleanup(error);
    cai_error_init(error);
    rc = CAI_OK;
  }
  session->commit_position = 0U;
  if (rc == CAI_OK && collection != NULL) {
    options.selector_lql = "eq{field=/record_type,value=\"commit\"}";
    options.on_metadata = commit_refresh_boundary;
    options.metadata_context = &refresh;
    sink.write = collection_event_write;
    sink.impl = &refresh.read;
    if (collection->read(collection, &options, &sink, &result, &detail) !=
        LC_OK)
      rc = refresh.read.rc != CAI_OK ? refresh.read.rc
                                     : cai_cli_lc_error(error, &detail);
  }
  collection_event_cleanup(&refresh.read);
  lonejson_free(refresh.read.json);
  if (collection != NULL)
    collection->close(collection);
  lc_collection_read_res_cleanup(&result);
  lc_error_cleanup(&detail);
  return rc;
}

static int read_events(void *context, const char *scope, const char *id,
                       unsigned long long after, unsigned long long through,
                       int (*select_event)(void *, unsigned long long),
                       cai_agent_session_event_fn visit, void *visitor,
                       int observers, cai_error *error) {
  cai_cli_pouch *pouch = context;
  cai_cli_session session;
  collection_events read;
  lc_collection *collection = NULL;
  lc_collection_read_opts options;
  lc_collection_read_res result;
  lc_sink sink;
  lc_error local;
  lonejson_error json_error;
  int rc, exists;
  memset(&read, 0, sizeof(read));
  memset(&sink, 0, sizeof(sink));
  lc_error_init(&local);
  lc_collection_read_opts_init(&options);
  lc_collection_read_res_init(&result);
  read.pouch = pouch;
  read.map = &journal_event_map;
  read.discard_data = 1;
  read.id = id;
  read.visit = visit;
  read.visitor = visitor;
  read.select_event = select_event;
  read.error = error;
  read.after = after;
  read.through = through;
  read.observe =
      observers && pthread_equal(pthread_self(), pouch->owner_thread) &&
      pouch->replay_scope != NULL && strcmp(scope, pouch->replay_scope) == 0;
  read.index_positions = read.observe && after == 0U;
  if (read.index_positions) {
    transcript_index_begin(pouch, id);
    pouch->event_position_count = pouch->event_position_stride = 0U;
    pouch->event_last_sequence = pouch->event_last_position = 0U;
    snprintf(pouch->event_position_session,
             sizeof(pouch->event_position_session), "%s", id);
  }
  rc = history_supported(pouch, error);
  if (rc == CAI_OK)
    rc = session_exists(pouch, id, &exists, error);
  if (rc != CAI_OK || !exists)
    return rc;
  rc = cai_cli_pouch_session(pouch, id, &session, error);
  if (rc == CAI_OK && strcmp(scope, session.workspace) != 0)
    rc = cai_cli_error(error, CAI_ERR_INVALID,
                       "session belongs to another directory");
  if (rc == CAI_OK)
    rc = history_collection(pouch, id, "events", 0, &collection, error);
  if (rc == CAI_OK && read.observe && pouch->replay_begin != NULL)
    rc = pouch->replay_begin(pouch->replay_context, id, error);
  if (rc == CAI_OK) {
    read.json = lonejson_new(NULL, &json_error);
    if (read.json == NULL)
      rc = cai_cli_lj_error(error, &json_error);
  }
  if (rc == CAI_OK) {
    if (!observers)
      event_position_window(pouch, id, after, through, &options);
    options.on_metadata = collection_event_boundary;
    options.metadata_context = &read;
    sink.write = collection_event_write;
    sink.impl = &read;
    if (collection->read(collection, &options, &sink, &result, &local) != LC_OK)
      rc = read.rc != CAI_OK ? read.rc : cai_cli_lc_error(error, &local);
  }
  collection_event_cleanup(&read);
  lonejson_free(read.json);
  if (collection != NULL)
    collection->close(collection);
  lc_collection_read_res_cleanup(&result);
  lc_error_cleanup(&local);
  if (rc != CAI_OK && read.index_positions)
    pouch->event_position_session[0] = '\0';
  return rc;
}

static int load_events(void *context, const char *scope, const char *id,
                       unsigned long long after,
                       cai_agent_session_event_fn visit, void *visitor,
                       cai_error *error) {
  return read_events(context, scope, id, after, 0U, NULL, visit, visitor, 1,
                     error);
}
int cai_cli_pouch_replay(cai_cli_pouch *pouch, const char *scope,
                         const char *id, unsigned long long after,
                         unsigned long long through,
                         int (*select_event)(void *, unsigned long long),
                         cai_agent_session_event_fn visit, void *context,
                         cai_error *error) {
  return read_events(pouch, scope, id, after, through, select_event, visit,
                     context, 0, error);
}

int cai_cli_pouch_transcript_open(cai_cli_pouch *pouch, const char *scope,
                                  const char *id,
                                  cai_cli_pouch_transcript **out,
                                  cai_error *error) {
  cai_cli_session session;
  cai_cli_pouch_transcript *reader;
  int rc;
  if (out != NULL)
    *out = NULL;
  if (pouch == NULL || scope == NULL || id == NULL || out == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "transcript context and identity required");
  rc = cai_cli_pouch_session(pouch, id, &session, error);
  if (rc == CAI_OK && strcmp(session.workspace, scope) != 0)
    rc = cai_cli_error(error, CAI_ERR_INVALID,
                       "transcript belongs to another workspace");
  if (rc == CAI_OK && strcmp(session.history_backend, "collections") != 0)
    rc = cai_cli_error(error, CAI_ERR_INVALID,
                       "transcript requires collection history");
  if (rc != CAI_OK)
    return rc;
  reader = calloc(1U, sizeof(*reader));
  if (reader == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate transcript context");
  rc = open_history_collection(pouch, id, "events", 0, 0, &reader->collection,
                               error);
  if (rc != CAI_OK) {
    lc_collection_close(reader->collection);
    free(reader);
  } else
    *out = reader;
  return rc;
}
#include "pouch_delete.inc"

static int auth_load(void *context, const char *key, cai_source **out,
                     cai_error *error) {
  cai_cli_pouch *pouch;
  lc_describe_req request;
  lc_describe_res response;
  lc_error local;
  int rc;
  pouch = (cai_cli_pouch *)context;
  *out = NULL;
  lc_describe_req_init(&request);
  memset(&response, 0, sizeof(response));
  lc_error_init(&local);
  request.key = key;
  request.ns = pouch->auth_namespace;
  rc = lc_describe(pouch->sessions, &request, &response, &local);
  if (rc != LC_OK && local.http_status != 404)
    rc = cai_cli_lc_error(error, &local);
  else {
    rc = response.state_etag != NULL && response.state_etag[0] != '\0'
             ? download_source_ns(pouch->sessions, pouch->auth_namespace, key,
                                  NULL, out, error)
             : CAI_OK;
  }
  lc_describe_res_cleanup(&response);
  lc_error_cleanup(&local);
  return rc;
}

static int auth_replace(void *context, const char *key, cai_source *source,
                        cai_error *error) {
  cai_cli_pouch *pouch;
  lc_lease *lease;
  int rc;
  pouch = (cai_cli_pouch *)context;
  lease = NULL;
  rc = acquire_ns(pouch, pouch->auth_namespace, key, &lease, error);
  if (rc == CAI_OK)
    rc = update(lease, source, error);
  return release(lease, rc, error);
}

typedef struct todo_transaction {
  cai_cli_pouch *pouch;
  lc_lease *lease;
  cai_sink *upload;
  unsigned int staged;
  char stage_name[16];
} todo_transaction;
static int todo_initial(void *context, cai_sink *sink, cai_error *error) {
  static const char empty[] =
      "{\"version\":1,\"boards\":[],\"items\":[],\"done\":[]}";
  (void)context;
  return cai_sink_write(sink, empty, sizeof(empty) - 1U, error);
}

static int todo_begin(void *context, void **out, cai_error *error) {
  todo_transaction *txn;
  cai_source *initial = NULL;
  int rc;
  *out = NULL;
  txn = calloc(1U, sizeof(*txn));
  if (txn == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate kanban transaction");
  txn->pouch = context;
  rc = acquire_ns(txn->pouch, txn->pouch->todo_namespace, "kanban", &txn->lease,
                  error);
  if (rc == CAI_OK &&
      (txn->lease->state_etag == NULL || txn->lease->state_etag[0] == '\0')) {
    rc = cai_cli_stream_source(todo_initial, NULL, NULL, &initial, error);
    if (rc == CAI_OK)
      rc = update(txn->lease, initial, error);
    cai_source_close(initial);
  }
  if (rc != CAI_OK) {
    release(txn->lease, rc, error);
    free(txn);
    return rc;
  }
  *out = txn;
  return CAI_OK;
}
static int todo_open_read(void *context, void *transaction,
                          lonejson_reader_fn *reader, void **reader_context,
                          cai_error *error) {
  todo_transaction *txn = transaction;
  cai_source *source = NULL;
  int rc;
  (void)context;
  rc = download_private(txn->lease, txn->staged ? txn->stage_name : NULL,
                        &source, error);
  *reader = cai_cli_json_read;
  *reader_context = source;
  return rc;
}
static void todo_close_read(void *context, void *transaction, void *reader) {
  (void)context;
  (void)transaction;
  cai_source_close(reader);
}
static int todo_upload(void *context, cai_source *source, cai_error *error) {
  todo_transaction *txn = context;
  lc_source *input = NULL;
  lc_attach_req request;
  lc_attach_res response;
  lc_error local;
  int rc;
  lc_attach_req_init(&request);
  memset(&response, 0, sizeof(response));
  lc_error_init(&local);
  request.name = txn->stage_name;
  request.content_type = "application/json";
  rc = cai_cli_lc_source(source, &input, error);
  if (rc == CAI_OK &&
      lc_lease_attach(txn->lease, &request, input, &response, &local) != LC_OK)
    rc = cai_cli_lc_error(error, &local);
  lc_source_close(input);
  lc_attach_res_cleanup(&response);
  lc_error_cleanup(&local);
  return rc;
}
static int todo_open_write(void *context, void *transaction,
                           lonejson_sink_fn *sink, void **sink_context,
                           cai_error *error) {
  todo_transaction *txn = transaction;
  int rc;
  (void)context;
  snprintf(txn->stage_name, sizeof(txn->stage_name), "rewrite-%u",
           txn->staged % 2U);
  rc = cai_cli_stream_sink(todo_upload, txn, &txn->upload, error);
  *sink = cai_cli_json_write;
  *sink_context = txn->upload;
  return rc;
}
static int todo_commit_write(void *context, void *transaction,
                             cai_error *error) {
  todo_transaction *txn = transaction;
  int rc;
  (void)context;
  rc = cai_cli_stream_finish(txn->upload, error);
  cai_sink_close(txn->upload);
  txn->upload = NULL;
  if (rc == CAI_OK)
    txn->staged++;
  return rc;
}
static void todo_cleanup(todo_transaction *txn) {
  lc_error error;
  int deleted;
  cai_sink_close(txn->upload);
  lc_error_init(&error);
  if (lc_lease_delete_all_attachments(txn->lease, &deleted, &error) != LC_OK &&
      txn->pouch->logger != NULL)
    txn->pouch->logger->warnf(
        txn->pouch->logger, "Kanban staging cleanup failed", "error=%s",
        error.message != NULL ? error.message : "lockd error");
  lc_error_cleanup(&error);
}
static int todo_commit(void *context, void *transaction, cai_error *error) {
  todo_transaction *txn = transaction;
  cai_source *source = NULL;
  int rc = CAI_OK;
  (void)context;
  if (txn->staged) {
    rc = download_private(txn->lease, txn->stage_name, &source, error);
    if (rc == CAI_OK)
      rc = update(txn->lease, source, error);
    cai_source_close(source);
  }
  todo_cleanup(txn);
  rc = release_mode(txn->lease, rc != CAI_OK, rc, error);
  free(txn);
  return rc;
}
static void todo_rollback(void *context, void *transaction) {
  todo_transaction *txn = transaction;
  cai_error error;
  (void)context;
  cai_error_init(&error);
  cai_sink_close(txn->upload);
  if (release_mode(txn->lease, 1, CAI_OK, &error) != CAI_OK) {
    if (txn->pouch->logger != NULL)
      txn->pouch->logger->warnf(
          txn->pouch->logger, "Kanban rollback failed", "error=%s",
          error.message != NULL ? error.message : "lockd error");
  }
  cai_error_cleanup(&error);
  free(txn);
}
const cai_todo_store_callbacks *cai_cli_pouch_todo_callbacks(void) {
  static const cai_todo_store_callbacks callbacks = {
      todo_begin,        todo_open_read, todo_close_read, todo_open_write,
      todo_commit_write, todo_commit,    todo_rollback,   NULL};
  return &callbacks;
}

static int private_directory(const char *path, cai_error *error) {
  struct stat status;
  if (mkdir(path, 0700) != 0 && errno != EEXIST)
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "create application state directory");
  if (lstat(path, &status) != 0 || !S_ISDIR(status.st_mode) ||
      status.st_uid != geteuid() || (status.st_mode & 077U) != 0U)
    return cai_cli_error(
        error, CAI_ERR_INVALID,
        "application state directory must be owned by you with mode 0700");
  return CAI_OK;
}

static int ensure_parents(char *path, cai_error *error) {
  char *cursor;
  struct stat status;
  for (cursor = path + 1; *cursor != '\0'; cursor++) {
    if (*cursor != '/')
      continue;
    *cursor = '\0';
    if (mkdir(path, 0700) != 0 && errno != EEXIST) {
      *cursor = '/';
      return cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "create state parent directory");
    }
    if (stat(path, &status) != 0 || !S_ISDIR(status.st_mode)) {
      *cursor = '/';
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "state parent is not a directory");
    }
    *cursor = '/';
  }
  return CAI_OK;
}

/* Auto-resume reads the directory cache and commit directly, so it need
 * not execute a query that repairs a retired/corrupt search index. The public
 * wait flush keeps a healthy index's trusted fast path and repairs derived
 * state when needed. Failure must not prevent direct session replay. */
static void prepare_session_indexes(cai_cli_pouch *pouch) {
  lc_index_flush_req request;
  lc_index_flush_res result;
  lc_error local;
  const char *namespaces[2];
  size_t i;
  namespaces[0] = pouch->sessions_namespace;
  namespaces[1] = pouch->scopes_namespace;
  for (i = 0U; i < 2U; i++) {
    if (i != 0U && strcmp(namespaces[0], namespaces[i]) == 0)
      continue;
    lc_index_flush_req_init(&request);
    memset(&result, 0, sizeof(result));
    lc_error_init(&local);
    request.ns = namespaces[i];
    request.mode = "wait";
    if (lc_flush_index(pouch->sessions, &request, &result, &local) != LC_OK &&
        pouch->logger != NULL)
      pouch->logger->warnf(
          pouch->logger,
          "Session search index recovery failed; session replay remains "
          "available",
          "ns=%s code=%d error=%s detail=%s", request.ns, local.code,
          local.message != NULL ? local.message : "index flush failed",
          local.detail != NULL ? local.detail : "");
    lc_index_flush_res_cleanup(&result);
    lc_error_cleanup(&local);
  }
}

int cai_cli_pouch_open(cai_cli_pouch *pouch, const char *selected_endpoint,
                       const char *client_pem, pslog_logger *logger,
                       cai_error *error) {
  return cai_cli_pouch_open_at(pouch, NULL, selected_endpoint, client_pem,
                               logger, error);
}

int cai_cli_pouch_open_at(cai_cli_pouch *pouch, const char *state_directory,
                          const char *selected_endpoint, const char *client_pem,
                          pslog_logger *logger, cai_error *error) {
  cai_cli_options options;
  memset(&options, 0, sizeof(options));
  options.conversation_retention_days = 30L;
  options.state_dir = state_directory;
  options.lockd = selected_endpoint;
  options.lockd_client_pem = client_pem;
  return cai_cli_pouch_open_options(pouch, &options, logger, error);
}

/** Per-invocation scratch belongs to the heap, including callback reentry. */
typedef struct cai_cli_pouch_open_options_scratch {
  char path[PATH_MAX];
  char root[PATH_MAX];
  char init_path[PATH_MAX];
} cai_cli_pouch_open_options_scratch;
static int cai_cli_pouch_open_options_with_scratch(
    cai_cli_pouch *pouch, const cai_cli_options *options, pslog_logger *logger,
    cai_error *error, cai_cli_pouch_open_options_scratch *scratch) {
  const char *selected_endpoint = options->lockd;
  const char *client_pem = options->lockd_client_pem;
  const char *name = options->program_name != NULL
                         ? options->program_name
                         : CAI_APP_DEFAULT_PROGRAM_NAME;
  const char *prefix =
      options->namespace_prefix != NULL ? options->namespace_prefix : name;

  char *endpoint;
  const char *endpoints[1];
  lc_client_config config;
  lc_pouch_settings settings;
  lc_error local;
  struct stat status;
  int lock_fd;
  int root_exists;
  int local_pouch;
  char *decoded_root;
  int rc;
  memset(pouch, 0, sizeof(*pouch));
  pouch->logger = logger;
  if (options->conversation_retention_days < 0L ||
      options->conversation_retention_days > LONG_MAX / 86400L)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "session retention day count is out of range");
  pouch->conversation_retention_seconds =
      options->conversation_retention_days * 86400L;
  if (options->config_dir != NULL) {
    if (strlen(options->config_dir) >= sizeof(pouch->config_directory))
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "configuration directory too long");
    strcpy(pouch->config_directory, options->config_dir);
  }
  if (!cai_cli_identity_valid(name))
    return cai_cli_error(error, CAI_ERR_INVALID, "invalid program_name");
  strcpy(pouch->program_name, name);
  name = options->queue_prefix != NULL ? options->queue_prefix : name;
  if (!cai_cli_identity_valid(name))
    return cai_cli_error(error, CAI_ERR_INVALID, "invalid queue_prefix");
  strcpy(pouch->queue_prefix, name);
#define NAMESPACE(kind)                                                        \
  rc =                                                                         \
      options->kind##_namespace != NULL                                        \
          ? snprintf(pouch->kind##_namespace, sizeof(pouch->kind##_namespace), \
                     "%s", options->kind##_namespace)                          \
          : snprintf(pouch->kind##_namespace, sizeof(pouch->kind##_namespace), \
                     "%s." #kind, prefix);                                     \
  if (rc <= 0 || (size_t)rc >= sizeof(pouch->kind##_namespace) ||              \
      strspn(pouch->kind##_namespace,                                          \
             "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKL"                          \
             "MNOPQRSTUVWXYZ0123456789._-") != (size_t)rc)                     \
  return cai_cli_error(error, CAI_ERR_INVALID, "invalid " #kind " namespace")
  NAMESPACE(sessions);
  NAMESPACE(auth);
  NAMESPACE(scopes);
  NAMESPACE(todo);
  NAMESPACE(messages);
#undef NAMESPACE
  if (options->state_dir != NULL) {
    rc = snprintf(scratch->path, sizeof(scratch->path), "%s",
                  options->state_dir);
    if (rc <= 0 || (size_t)rc >= sizeof(scratch->path) ||
        scratch->path[0] != '/')
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "state directory must be absolute and fit PATH_MAX");
  } else if (cai_cli_xdg_path(scratch->path, sizeof(scratch->path),
                              "XDG_STATE_HOME", ".local/state",
                              pouch->program_name, error) != CAI_OK)
    return error != NULL ? error->code : CAI_ERR_INVALID;
  rc = ensure_parents(scratch->path, error);
  if (rc == CAI_OK)
    rc = private_directory(scratch->path, error);
  if (rc != CAI_OK)
    return rc;
  if (realpath(scratch->path, pouch->state_directory) == NULL)
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "resolve application state directory");
  rc = options->pouch_key_file != NULL
           ? snprintf(pouch->key_path, sizeof(pouch->key_path), "%s",
                      options->pouch_key_file)
           : snprintf(pouch->key_path, sizeof(pouch->key_path), "%s/pouch.key",
                      pouch->state_directory);
  if (rc <= 0 || (size_t)rc >= sizeof(pouch->key_path) ||
      pouch->key_path[0] != '/' ||
      ensure_parents(pouch->key_path, error) != CAI_OK)
    return cai_cli_error(error, CAI_ERR_INVALID, "invalid pouch key path");
  if (snprintf(scratch->root, sizeof(scratch->root), "%s/pouch",
               pouch->state_directory) >= (int)sizeof(scratch->root) ||
      snprintf(scratch->init_path, sizeof(scratch->init_path),
               "%s/pouch.init.lock",
               pouch->state_directory) >= (int)sizeof(scratch->init_path))
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "application state path too long");
  local_pouch = selected_endpoint == NULL ||
                strncmp(selected_endpoint, "pouch://", 8U) == 0;
  if (selected_endpoint != NULL && local_pouch) {
    /* Unlike HTTP, pouch:// has no authority component. Match the SDK's
     * documented root syntax and use libcurl only for percent-decoding. */
    const char *encoded = selected_endpoint + 8U;
    size_t length = strcspn(encoded, "?#"), i;
    int decoded_length = 0;
    if (length == 0U || length > INT_MAX)
      return cai_cli_error(error, CAI_ERR_INVALID, "invalid pouch root URL");
    for (i = 0U; i < length; i++) {
      if (encoded[i] == '%') {
        if (length - i < 3U ||
            strchr("0123456789abcdefABCDEF", encoded[i + 1U]) == NULL ||
            strchr("0123456789abcdefABCDEF", encoded[i + 2U]) == NULL)
          return cai_cli_error(error, CAI_ERR_INVALID,
                               "invalid pouch root percent escape");
        i += 2U;
      }
    }
    decoded_root =
        curl_easy_unescape(NULL, encoded, (int)length, &decoded_length);
    if (decoded_root == NULL || decoded_length <= 0 || decoded_root[0] != '/' ||
        (size_t)decoded_length >= sizeof(scratch->root) ||
        memchr(decoded_root, '\0', (size_t)decoded_length) != NULL) {
      curl_free(decoded_root);
      return cai_cli_error(error, CAI_ERR_INVALID, "invalid pouch root URL");
    }
    strcpy(scratch->root, decoded_root);
    curl_free(decoded_root);
  }
  lock_fd =
      open(scratch->init_path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (lock_fd < 0 || flock(lock_fd, LOCK_EX) != 0) {
    if (lock_fd >= 0)
      close(lock_fd);
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "lock pouch initialization");
  }
  root_exists = lstat(scratch->root, &status) == 0;
  lc_error_init(&local);
  endpoint = NULL;
  if (selected_endpoint != NULL) {
    endpoint = strdup(selected_endpoint);
    rc = endpoint != NULL ? LC_OK : LC_ERR_NOMEM;
  } else {
    rc = lc_pouch_endpoint_build(scratch->root, NULL, 0U, &endpoint, &local);
  }
  lc_client_config_init(&config);
  config.logger = logger;
  lc_pouch_settings_init(&settings);
  settings.set_mask =
      LC_POUCH_SETTING_SINGLE_WRITER | LC_POUCH_SETTING_DURABLE_SYNC |
      LC_POUCH_SETTING_QUERY_INDEXING | LC_POUCH_SETTING_QUERY_ENGINE |
      LC_POUCH_SETTING_CRYPTO_KEY_FILE |
      LC_POUCH_SETTING_CRYPTO_GENERATE_KEY_FILE | LC_POUCH_SETTING_QUEUE_WATCH |
      LC_POUCH_SETTING_COMPRESSION;
  settings.compression = options->disable_compression ? "none" : "zlib";
  settings.single_writer = 0;
  settings.queue_watch = 1;
  settings.durable_sync = 1;
  settings.query_indexing_enabled = 1;
  settings.query_engine = "index";
  settings.crypto_key_file = pouch->key_path;
  settings.crypto_generate_key_file = !root_exists;
  endpoints[0] = endpoint;
  config.endpoints = endpoints;
  config.endpoint_count = 1U;
  config.default_namespace = pouch->sessions_namespace;
  config.pouch_settings = local_pouch ? &settings : NULL;
  config.client_bundle_path = client_pem;
  config.disable_mtls = client_pem == NULL;
  if (rc == LC_OK)
    rc = shared_client_open(pouch, &config, scratch->root, local_pouch,
                            client_pem, error);
  else
    rc = cai_cli_lc_error(error, &local);
  lc_pouch_endpoint_free(endpoint);
  lc_error_cleanup(&local);
  close(lock_fd);
  if (rc != CAI_OK) {
    cai_cli_pouch_close(pouch);
    return rc;
  }
  if (local_pouch && realpath(scratch->root, pouch->root_directory) == NULL) {
    cai_cli_pouch_close(pouch);
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "resolve shared Pouch root");
  }
  if (pthread_mutex_init(&pouch->history_lock, NULL) != 0) {
    cai_cli_pouch_close(pouch);
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "initialize session collection mutex");
  }
  pouch->history_lock_initialized = 1;
  if (pthread_mutex_init(&pouch->journal_lock, NULL) != 0) {
    cai_cli_pouch_close(pouch);
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "initialize journal context mutex");
  }
  pouch->journal_lock_initialized = 1;
  rc = collection_owners_init(pouch, error);
  if (rc != CAI_OK) {
    cai_cli_pouch_close(pouch);
    return rc;
  }
  pouch->owner_thread = pthread_self();
  if (local_pouch)
    prepare_session_indexes(pouch);
  pouch->store.context = pouch;
  pouch->store.commit = commit;
  pouch->store.trim = trim_history;
  pouch->store.load_latest = load_latest;
  pouch->store.load_id = load_owned_id;
  pouch->store.append_event = append_event;
  pouch->store.load_events_after = load_events;
  pouch->credentials.context = pouch;
  pouch->credentials.load = auth_load;
  pouch->credentials.replace = auth_replace;
  return CAI_OK;
}

int cai_cli_pouch_open_options(cai_cli_pouch *pouch,
                               const cai_cli_options *options,
                               pslog_logger *logger, cai_error *error) {
  cai_cli_pouch_open_options_scratch *scratch =
      (cai_cli_pouch_open_options_scratch *)calloc(1U, sizeof(*scratch));
  int rc;
  if (scratch == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM,
                         "allocate cai_cli_pouch_open_options scratch");
  rc = cai_cli_pouch_open_options_with_scratch(pouch, options, logger, error,
                                               scratch);
  free(scratch);
  return rc;
}

void cai_cli_pouch_close(cai_cli_pouch *pouch) {
  cai_cli_pouch_client **link, *entry;
  cai_cli_pouch_prompt_close(pouch);
  free(pouch->event_positions);
  cai_cli_replay_close(pouch->transcript_index);
  pouch->transcript_index = NULL;
  pouch->event_positions = NULL;
  cai_cli_pouch_end_use(pouch);
  if (pouch->ownership_initialized) {
    if (pouch->ownership_pipe[0] >= 0)
      close(pouch->ownership_pipe[0]);
    if (pouch->ownership_pipe[1] >= 0)
      close(pouch->ownership_pipe[1]);
    pthread_cond_destroy(&pouch->ownership_condition);
    pthread_mutex_destroy(&pouch->ownership_health_lock);
    pthread_mutex_destroy(&pouch->ownership_lock);
    pouch->ownership_initialized = 0;
  }
  if (pouch->journal_lock_initialized) {
    pthread_mutex_destroy(&pouch->journal_lock);
    pouch->journal_lock_initialized = 0;
  }
  if (pouch->history_lock_initialized) {
    if (pouch->history_anchor != NULL)
      pouch->history_anchor->close(pouch->history_anchor);
    pouch->history_anchor = NULL;
    pthread_mutex_destroy(&pouch->history_lock);
    pouch->history_lock_initialized = 0;
  }
  pthread_mutex_lock(&clients_lock);
  entry = pouch->connection;
  if (entry != NULL && --entry->references == 0U) {
    for (link = &clients; *link != entry; link = &(*link)->next)
      ;
    *link = entry->next;
    entry->client->close(entry->client);
    (void)cai_cli_log_release(entry->logging);
    free(entry->endpoint);
    free(entry->client_pem);
    free(entry);
  }
  pthread_mutex_unlock(&clients_lock);
  pouch->connection = NULL;
  pouch->sessions = NULL;
}

typedef struct auth_origin {
  char path[PATH_MAX];
  char digest[65];
} auth_origin;
static const lonejson_field auth_origin_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(auth_origin, path, "path",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED_REQ(auth_origin, digest, "digest",
                                    LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(auth_origin_map, auth_origin, auth_origin_fields);

/** Per-invocation scratch belongs to the heap, including callback reentry. */
typedef struct cai_cli_pouch_seed_auth_scratch {
  auth_origin current;
  auth_origin previous;
} cai_cli_pouch_seed_auth_scratch;
static int
cai_cli_pouch_seed_auth_with_scratch(cai_cli_pouch *pouch, const char *path,
                                     cai_error *error,
                                     cai_cli_pouch_seed_auth_scratch *scratch) {

  unsigned char buffer[4096];
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_length;
  EVP_MD_CTX *hash;
  cai_source *source;
  cai_source *stored;
  FILE *file;
  cai_chatgpt_auth_config auth_config;
  cai_chatgpt_auth *auth;
  size_t count;
  size_t i;
  int rc;
  memset(&scratch->current, 0, sizeof(scratch->current));
  memset(&scratch->previous, 0, sizeof(scratch->previous));
  if (realpath(path, scratch->current.path) == NULL)
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "cannot open authentication source file");
  /* Reject invalid credentials before changing the durable credential store.
   * Opening auth only validates the document; token access performs refresh. */
  auth = NULL;
  cai_chatgpt_auth_config_init(&auth_config);
  auth_config.logger = pouch->logger;
  auth_config.auth_json_path = scratch->current.path;
  rc = cai_chatgpt_auth_open(&auth_config, &auth, error);
  cai_chatgpt_auth_close(auth);
  if (rc != CAI_OK)
    return rc;
  file = fopen(scratch->current.path, "rb");
  if (file == NULL)
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "cannot read authentication source file");
  source = NULL;
  stored = NULL;
  rc = cai_source_file(file, 1, &source, error);
  if (rc != CAI_OK) {
    fclose(file);
    return rc;
  }
  hash = EVP_MD_CTX_new();
  if (hash == NULL || EVP_DigestInit_ex(hash, EVP_sha256(), NULL) != 1)
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                       "initialize auth source fingerprint");
  while (rc == CAI_OK) {
    count = cai_source_read(source, buffer, sizeof(buffer), error);
    if (count == 0U) {
      rc = error->code;
      break;
    }
    if (EVP_DigestUpdate(hash, buffer, count) != 1)
      rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "fingerprint auth source");
  }
  if (rc == CAI_OK && EVP_DigestFinal_ex(hash, digest, &digest_length) != 1)
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                       "finish auth source fingerprint");
  EVP_MD_CTX_free(hash);
  OPENSSL_cleanse(buffer, sizeof(buffer));
  if (rc == CAI_OK) {
    for (i = 0U; i < digest_length; i++)
      snprintf(scratch->current.digest + i * 2U,
               sizeof(scratch->current.digest) - i * 2U, "%02x",
               (unsigned int)digest[i]);
    rc = auth_load(pouch, "source.json", &stored, error);
  }
  if (rc == CAI_OK && stored != NULL)
    rc = parse_source(stored, &auth_origin_map, &scratch->previous, error);
  cai_source_close(stored);
  stored = NULL;
  /* An unchanged Codex file must not replace refreshed tokens in the pouch. */
  if (rc == CAI_OK &&
      (strcmp(scratch->current.path, scratch->previous.path) != 0 ||
       strcmp(scratch->current.digest, scratch->previous.digest) != 0)) {
    rc = cai_source_reset(source, error);
    if (rc == CAI_OK)
      rc = auth_replace(pouch, "auth.json", source, error);
    if (rc == CAI_OK)
      rc = cai_cli_mapped_source(&auth_origin_map, &scratch->current, &stored,
                                 error);
    if (rc == CAI_OK)
      rc = auth_replace(pouch, "source.json", stored, error);
  }
  cai_source_close(stored);
  cai_source_close(source);
  return rc;
}

int cai_cli_pouch_seed_auth(cai_cli_pouch *pouch, const char *path,
                            cai_error *error) {
  cai_cli_pouch_seed_auth_scratch *scratch =
      (cai_cli_pouch_seed_auth_scratch *)calloc(1U, sizeof(*scratch));
  int rc;
  if (scratch == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM,
                         "allocate cai_cli_pouch_seed_auth scratch");
  rc = cai_cli_pouch_seed_auth_with_scratch(pouch, path, error, scratch);
  free(scratch);
  return rc;
}

/** Per-invocation scratch belongs to the heap, including callback reentry. */
typedef struct cai_cli_pouch_publish_scratch {
  cai_cli_session session;
  latest_index index;
} cai_cli_pouch_publish_scratch;
static int
cai_cli_pouch_publish_with_scratch(cai_cli_pouch *pouch, const char *id,
                                   int published, cai_error *error,
                                   cai_cli_pouch_publish_scratch *scratch) {

  lc_lease *lease = NULL;
  lc_lease *directory = NULL;
  int rc = cai_cli_pouch_session(pouch, id, &scratch->session, error);
  memset(&scratch->index, 0, sizeof(scratch->index));
  if (rc == CAI_OK)
    strcpy(scratch->index.workspace, scratch->session.workspace);
  if (rc == CAI_OK && scope_indexed(scratch->session.workspace))
    rc = latest_open(pouch, scratch->session.workspace, &directory,
                     &scratch->index, error);
  if (rc == CAI_OK && directory != NULL) {
    scratch->index.dirty = 1;
    rc = latest_write(pouch, directory, &scratch->index, error);
  }
  if (rc == CAI_OK)
    rc = acquire(pouch, id, &lease, error);
  if (rc == CAI_OK)
    rc = get_session(lease, scratch->index.workspace, id, &scratch->session,
                     error);
  if (rc == CAI_OK) {
    scratch->session.published = published;
    rc = update_session(lease, &scratch->session, error);
  }
  rc = release(lease, rc, error);
  if (rc == CAI_OK && directory != NULL) {
    if (!published && strcmp(scratch->index.latest.id, id) == 0)
      rc = latest_rebuild(pouch, &scratch->index, error);
    else if (published && scratch->session.commit_position != 0U &&
             (scratch->index.latest.id[0] == '\0' ||
              session_compare(&scratch->session, &scratch->index.latest) < 0))
      scratch->index.latest = scratch->session;
    if (rc == CAI_OK) {
      scratch->index.dirty = 0;
      rc = latest_write(pouch, directory, &scratch->index, error);
    }
  }
  return release(directory, rc, error);
}

int cai_cli_pouch_publish(cai_cli_pouch *pouch, const char *id, int published,
                          cai_error *error) {
  cai_cli_pouch_publish_scratch *scratch =
      (cai_cli_pouch_publish_scratch *)calloc(1U, sizeof(*scratch));
  int rc;
  if (scratch == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM,
                         "allocate cai_cli_pouch_publish scratch");
  rc = cai_cli_pouch_publish_with_scratch(pouch, id, published, error, scratch);
  free(scratch);
  return rc;
}

int cai_cli_pouch_lock(cai_cli_pouch *pouch, const char *id, lc_lease **out,
                       cai_error *error) {
  return acquire(pouch, id, out, error);
}

int cai_cli_pouch_unlock(lc_lease *lease, int result, cai_error *error) {
  return release(lease, result, error);
}

#include "pouch_export.inc"

static int validate_commit(cai_cli_pouch *pouch, const char *id,
                           const char *workspace, cai_error *error) {
  cai_client_config client_config;
  cai_agent_config agent_config;
  cai_client *client;
  cai_agent *agent;
  cai_session *session;
  cai_source *state;
  unsigned long long sequence;
  int rc;
  client = NULL;
  agent = NULL;
  session = NULL;
  state = NULL;
  cai_client_config_init(&client_config);
  client_config.logger = pouch->logger;
  client_config.base_url = "http://127.0.0.1:1/v1";
  client_config.api_key = "offline-state-validation";
  cai_agent_config_init(&agent_config);
  agent_config.model = "gpt-6-luna";
  agent_config.enable_local_history = 1;
  rc = cai_client_open(&client_config, &client, error);
  if (rc == CAI_OK)
    rc = cai_client_new_agent(client, &agent_config, &agent, error);
  if (rc == CAI_OK)
    rc = cai_agent_new_session(agent, &session, error);
  if (rc == CAI_OK)
    rc = load_id(pouch, workspace, id, &state, &sequence, error);
  if (rc == CAI_OK)
    rc = cai_session_restore_journal_source(session, state, error);
  cai_source_close(state);
  if (session != NULL)
    session->close(session);
  if (agent != NULL)
    agent->close(agent);
  if (client != NULL)
    client->close(client);
  return rc;
}

#include "pouch_import.inc"
