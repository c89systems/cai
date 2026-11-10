#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include <assert.h>
#include <cai/context.h>
#include <errno.h>
#include <fcntl.h>
#include <lonejson.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct capture {
  char *bytes;
  size_t used, capacity;
} capture;
static int capture_write(void *ctx, const void *b, size_t n, cai_error *e) {
  capture *c = ctx;
  (void)e;
  if (c->used + n + 1U > c->capacity) {
    c->capacity = c->used + n + 65536U;
    c->bytes = realloc(c->bytes, c->capacity);
    assert(c->bytes != NULL);
  }
  memcpy(c->bytes + c->used, b, n);
  c->used += n;
  c->bytes[c->used] = '\0';
  return CAI_OK;
}
static cai_sink *capture_sink(capture *c) {
  cai_sink_callbacks cb;
  cai_sink *out = NULL;
  cai_error e;
  memset(&cb, 0, sizeof(cb));
  cb.context = c;
  cb.write = capture_write;
  cai_error_init(&e);
  assert(cai_sink_from_callbacks(&cb, &out, &e) == CAI_OK);
  cai_error_cleanup(&e);
  return out;
}
typedef struct result {
  char id[21], kind[16];
  lonejson_int64 version;
} result;
static const lonejson_field result_fields[] = {
    LONEJSON_FIELD_STRING_FIXED(result, id, "id", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED(result, kind, "kind", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_I64(result, version, "version")};
LONEJSON_MAP_DEFINE(result_map, result, result_fields);
static result decode(capture *c) {
  result r;
  lonejson_error e;
  lonejson *j;
  memset(&r, 0, sizeof(r));
  lonejson_error_init(&e);
  j = lonejson_new(NULL, &e);
  assert(j);
  assert(lonejson_parse_cstr(j, &result_map, &r, c->bytes, &e) ==
         LONEJSON_STATUS_OK);
  lonejson_free(j);
  return r;
}
static void call(cai_context *ctx, const char *tool, const char *args,
                 capture *c, int success) {
  cai_error e;
  cai_sink *sink = capture_sink(c);
  int rc;
  cai_error_init(&e);
  c->used = 0U;
  rc = cai_context_call(ctx, tool, args, sink, &e);
  if ((rc == CAI_OK) != success)
    fprintf(stderr, "%s: %s => %d %s (%s)\n", tool, args, rc,
            e.message ? e.message : "", e.detail ? e.detail : "");
  assert((rc == CAI_OK) == success);
  cai_error_cleanup(&e);
  cai_sink_close(sink);
}
static result create(cai_context *ctx, const char *text, capture *c) {
  FILE *fp = tmpfile();
  cai_source *source = NULL;
  cai_sink *sink = capture_sink(c);
  cai_error e;
  int rc;
  assert(fp);
  assert(fwrite(text, 1U, strlen(text), fp) == strlen(text));
  rewind(fp);
  cai_error_init(&e);
  assert(cai_source_file(fp, 0, &source, &e) == CAI_OK);
  c->used = 0U;
  rc = cai_context_create(ctx, source, sink, &e);
  if (rc != CAI_OK)
    fprintf(stderr, "create: %s\n", e.message ? e.message : "");
  assert(rc == CAI_OK);
  cai_source_close(source);
  cai_sink_close(sink);
  fclose(fp);
  cai_error_cleanup(&e);
  return decode(c);
}
static void write_file(const char *path, const void *b, size_t n) {
  FILE *fp = fopen(path, "wb");
  assert(fp);
  assert(fwrite(b, 1U, n, fp) == n);
  assert(fclose(fp) == 0);
}
static void check_file(const char *path, const void *b, size_t n) {
  FILE *fp = fopen(path, "rb");
  unsigned char block[512];
  size_t used = 0U, got;
  assert(fp);
  while ((got = fread(block, 1U, sizeof(block), fp)) != 0) {
    assert(used + got <= n);
    assert(!memcmp(block, (const char *)b + used, got));
    used += got;
  }
  assert(used == n);
  fclose(fp);
}
static const char original[] =
    "# Hello\n\n**Markdown** `verbatim` \\ \"quote\" \303\245\n";
static char failed_id[21];
static int (*real_acquire)(lc_client *, const lc_acquire_req *, lc_lease **,
                           lc_error *);
static int (*real_keepalive)(lc_lease *, const lc_keepalive_req *, lc_error *);
static int renewals;
static int counted_keepalive(lc_lease *lease, const lc_keepalive_req *request,
                             lc_error *error) {
  renewals++;
  return real_keepalive(lease, request, error);
}
static int renewing_acquire(lc_client *client, const lc_acquire_req *request,
                            lc_lease **lease, lc_error *error) {
  int rc = real_acquire(client, request, lease, error);
  if (rc == LC_OK) {
    real_keepalive = (*lease)->keepalive;
    (*lease)->keepalive = counted_keepalive;
    (*lease)->lease_expires_at_unix = 0;
  }
  return rc;
}
static int tracked_acquire(lc_client *client, const lc_acquire_req *request,
                           lc_lease **lease, lc_error *error) {
  strcpy(failed_id, request->key);
  return real_acquire(client, request, lease, error);
}
static size_t failing_read(void *ctx, void *b, size_t n, cai_error *error) {
  int *sent = ctx;
  if (!*sent && n >= 7U) {
    memcpy(b, "partial", 7U);
    *sent = 1;
    return 7U;
  }
  error->code = CAI_ERR_TRANSPORT;
  error->message = strdup("injected context source failure");
  return 0U;
}
static void *run(void *arg) {
  const char *root = arg;
  lc_client_config cc;
  lc_client *client = NULL, *second_client = NULL;
  lc_pouch_settings settings;
  const char *endpoints[1];
  lc_error le;
  cai_context_config config;
  cai_context *ctx = NULL, *other = NULL, *isolated = NULL;
  cai_error e;
  char *endpoint, *path, *args, *large;
  char id[21], binary_id[21], empty_id[21], special_id[21];
  result r;
  capture c;
  size_t i;
  lc_version v;
  cai_tool_registry *registry = NULL;
  memset(&c, 0, sizeof(c));
  endpoint = malloc(strlen(root) + 80U);
  path = malloc(strlen(root) + 1024U);
  args = malloc(strlen(root) + 4096U);
  large = malloc(1024U * 1024U + 4U);
  assert(endpoint && path && args && large);
  snprintf(endpoint, strlen(root) + 80U, "pouch://%s/pouch?durable_sync=false",
           root);
  endpoints[0] = endpoint;
  lc_client_config_init(&cc);
  lc_pouch_settings_init(&settings);
  settings.set_mask =
      LC_POUCH_SETTING_SINGLE_WRITER | LC_POUCH_SETTING_CRYPTO_KEY_FILE |
      LC_POUCH_SETTING_CRYPTO_GENERATE_KEY_FILE | LC_POUCH_SETTING_COMPRESSION;
  settings.single_writer = 0;
  snprintf(path, strlen(root) + 1024U, "%s/pouch.key", root);
  settings.crypto_key_file = path;
  settings.crypto_generate_key_file = 1;
  settings.compression = "zlib";
  cc.pouch_settings = &settings;
  cc.endpoints = endpoints;
  cc.endpoint_count = 1U;
  lc_error_init(&le);
  {
    int opened = lc_client_open(&cc, &client, &le);
    if (opened != LC_OK)
      fprintf(stderr, "open context fixture: %s (%s)\n",
              le.message ? le.message : "", le.detail ? le.detail : "");
    assert(opened == LC_OK);
  }
  assert(lc_client_open(&cc, &second_client, &le) == LC_OK);
  cai_error_init(&e);
  cai_context_config_init(&config);
  config.client = client;
  config.root_path = root;
  assert(cai_context_open(&config, &ctx, &e) == CAI_OK);
  config.client = second_client;
  assert(cai_context_open(&config, &other, &e) == CAI_OK);
  config.client = client;
  config.namespace_name = "other.context";
  assert(cai_context_open(&config, &isolated, &e) == CAI_OK);
  r = create(ctx, original, &c);
  strcpy(id, r.id);
  v = r.version;
  assert(lc_xid_is_valid(id) && !strcmp(r.kind, "text"));
  snprintf(args, strlen(root) + 4096U, "{\"id\":\"%s\"}", id);
  call(other, "context_get", args, &c, 1);
  assert(strstr(c.bytes, "Markdown") && strstr(c.bytes, "\\n"));
  call(isolated, "context_get", args, &c, 0);
  snprintf(args, strlen(root) + 4096U,
           "{\"id\":\"%s\",\"expected_version\":%ld,\"title\":\"A "
           "title\",\"filename\":\"notes.md\",\"tags\":[\"ops\",\"x\\\"y\"],"
           "\"date\":\"2026-11-10\"}",
           id, (long)v);
  call(other, "context_set", args, &c, 1);
  r = decode(&c);
  v = r.version;
  call(ctx, "context_set", args, &c, 0);
  call(ctx, "context_query",
       "{\"query\":\"icontains{field=/content,value=\\\"markdown\\\"}\"}", &c,
       1);
  assert(strstr(c.bytes, id) && strstr(c.bytes, "A title") &&
         !strstr(c.bytes, "verbatim"));
  call(ctx, "context_query",
       "{\"query\":\"eq{field=/tags[],value=\\\"ops\\\"}\"}", &c, 1);
  assert(strstr(c.bytes, id));
  snprintf(
      args, strlen(root) + 4096U,
      "{\"id\":\"%s\",\"expected_version\":%ld,\"title\":null,\"tags\":null}",
      id, (long)v);
  call(ctx, "context_set", args, &c, 1);
  r = decode(&c);
  v = r.version;
  snprintf(args, strlen(root) + 4096U,
           "{\"id\":\"%s\",\"expected_version\":%ld,\"title\":null}", id,
           (long)v);
  call(ctx, "context_set", args, &c, 1);
  r = decode(&c);
  v = r.version;
  call(ctx, "context_query", "{\"query\":\"eq{field=/title,value=\\\"\\\"}\"}",
       &c, 1);
  assert(strstr(c.bytes, id));
  snprintf(args, strlen(root) + 4096U, "{\"id\":\"%s\"}", id);
  call(ctx, "context_get", args, &c, 1);
  assert(strstr(c.bytes, "notes.md") && strstr(c.bytes, "2026-11-10") &&
         !strstr(c.bytes, "A title"));
  snprintf(args, strlen(root) + 4096U,
           "{\"id\":\"%s\",\"path\":\"download.md\"}", id);
  call(ctx, "context_download", args, &c, 1);
  call(ctx, "context_download", args, &c, 0);
  snprintf(path, strlen(root) + 1024U, "%s/download.md", root);
  check_file(path, original, strlen(original));
  snprintf(args, strlen(root) + 4096U,
           "{\"id\":\"%s\",\"path\":\"%s/absolute.md\"}", id, root);
  call(ctx, "context_download", args, &c, 1);
  snprintf(path, strlen(root) + 1024U, "%s/absolute.md", root);
  check_file(path, original, strlen(original));
  real_acquire = client->acquire;
  client->acquire = renewing_acquire;
  snprintf(args, strlen(root) + 4096U,
           "{\"id\":\"%s\",\"path\":\"renewed.md\"}", id);
  call(ctx, "context_download", args, &c, 1);
  client->acquire = real_acquire;
  assert(renewals > 0);
  /* Text, empty text, and binary content detection ignores misleading suffixes.
   */
  snprintf(path, strlen(root) + 1024U, "%s/binary.md", root);
  write_file(path, "a\0b\377", 4U);
  call(ctx, "context_upload", "{\"path\":\"binary.md\"}", &c, 1);
  r = decode(&c);
  strcpy(binary_id, r.id);
  assert(!strcmp(r.kind, "file"));
  snprintf(args, strlen(root) + 4096U,
           "{\"id\":\"%s\",\"path\":\"binary.out\"}", binary_id);
  call(ctx, "context_download", args, &c, 1);
  snprintf(path, strlen(root) + 1024U, "%s/binary.out", root);
  check_file(path, "a\0b\377", 4U);
  snprintf(path, strlen(root) + 1024U, "%s/empty.bin", root);
  write_file(path, "", 0U);
  call(ctx, "context_upload", "{\"path\":\"empty.bin\"}", &c, 1);
  r = decode(&c);
  assert(!strcmp(r.kind, "text"));
  strcpy(empty_id, r.id);
  snprintf(args, strlen(root) + 4096U,
           "{\"id\":\"%s\",\"expected_version\":%ld,\"filename\":\"notes.md\"}",
           r.id, (long)r.version);
  call(ctx, "context_set", args, &c, 1);
  memset(large, 'x', 1024U * 1024U);
  large[2047] = '\303';
  large[2048] = '\245';
  large[1024U * 1024U] = '\0';
  snprintf(path, strlen(root) + 1024U, "%s/text.bin", root);
  write_file(path, large, 1024U * 1024U);
  call(ctx, "context_upload", "{\"path\":\"text.bin\"}", &c, 1);
  r = decode(&c);
  assert(!strcmp(r.kind, "text"));
  snprintf(args, strlen(root) + 4096U, "{\"id\":\"%s\"}", r.id);
  call(ctx, "context_get", args, &c, 1);
  assert(strstr(c.bytes, "\"truncated\":true") && c.used < 20000U);
  snprintf(args, strlen(root) + 4096U, "{\"id\":\"%s\",\"path\":\"text.out\"}",
           r.id);
  call(ctx, "context_download", args, &c, 1);
  snprintf(path, strlen(root) + 1024U, "%s/text.out", root);
  check_file(path, large, 1024U * 1024U);
  /* Array metadata updates backpressure a large content read without spooling,
   * and preserve the original file bytes beyond the model's preview limit. */
  snprintf(args, strlen(root) + 4096U,
           "{\"id\":\"%s\",\"expected_version\":%ld,\"tags\":[\"large\"]}",
           r.id, (long)r.version);
  call(ctx, "context_set", args, &c, 1);
  r = decode(&c);
  snprintf(args, strlen(root) + 4096U,
           "{\"id\":\"%s\",\"path\":\"retagged.out\"}", r.id);
  call(ctx, "context_download", args, &c, 1);
  snprintf(path, strlen(root) + 1024U, "%s/retagged.out", root);
  check_file(path, large, 1024U * 1024U);
  /* Invalid UTF-8 late in a large file must not become searchable text. */
  large[1024U * 1024U - 1U] = '\300';
  snprintf(path, strlen(root) + 1024U, "%s/late.txt", root);
  write_file(path, large, 1024U * 1024U);
  call(ctx, "context_upload", "{\"path\":\"late.txt\"}", &c, 1);
  r = decode(&c);
  assert(!strcmp(r.kind, "file"));
  snprintf(path, strlen(root) + 1024U, "%s/.context.json", root);
  write_file(path, "%PDF-1.7\n", 9U);
  call(ctx, "context_upload", "{\"path\":\".context.json\"}", &c, 1);
  r = decode(&c);
  assert(!strcmp(r.kind, "file"));
  strcpy(special_id, r.id);
  /* Workspace confinement, symlinks, non-files, malformed IDs and limits. */
  call(ctx, "context_upload", "{\"path\":\"../outside\"}", &c, 0);
  call(ctx, "context_upload", "{\"path\":\".\"}", &c, 0);
  snprintf(path, strlen(root) + 1024U, "%s/link", root);
  assert(symlink("download.md", path) == 0);
  call(ctx, "context_upload", "{\"path\":\"link\"}", &c, 0);
  call(ctx, "context_get", "{\"id\":\"../no\"}", &c, 0);
  call(ctx, "context_query", "{\"limit\":33}", &c, 0);
  call(ctx, "context_query", "{\"query\":\"broken{\"}", &c, 0);
  /* The public read stays available while a different owner holds a lease. */
  {
    lc_acquire_req request;
    lc_release_req release;
    lc_lease *held = NULL;
    lc_acquire_req_init(&request);
    request.ns = "cai.context";
    request.key = id;
    request.owner = "other-agent";
    request.ttl_seconds = 30;
    assert(client->acquire(client, &request, &held, &le) == LC_OK);
    snprintf(args, strlen(root) + 4096U, "{\"id\":\"%s\"}", id);
    call(other, "context_get", args, &c, 1);
    snprintf(args, strlen(root) + 4096U,
             "{\"id\":\"%s\",\"expected_version\":%ld,\"title\":\"blocked\"}",
             id, (long)v);
    call(ctx, "context_set", args, &c, 0);
    lc_release_req_init(&release);
    assert(held->release(held, &release, &le) == LC_OK);
  }
  {
    cai_source_callbacks cb;
    cai_source *source = NULL;
    cai_sink *sink = capture_sink(&c);
    int sent = 0;
    memset(&cb, 0, sizeof(cb));
    cb.read = failing_read;
    cb.context = &sent;
    assert(cai_source_from_callbacks(&cb, &source, &e) == CAI_OK);
    real_acquire = client->acquire;
    client->acquire = tracked_acquire;
    assert(cai_context_create(ctx, source, sink, &e) == CAI_ERR_TRANSPORT);
    assert(strstr(e.message, "injected context source failure"));
    client->acquire = real_acquire;
    cai_source_close(source);
    cai_sink_close(sink);
    cai_error_cleanup(&e);
    snprintf(args, strlen(root) + 4096U, "{\"id\":\"%s\"}", failed_id);
    call(ctx, "context_get", args, &c, 0);
  }
  snprintf(
      args, strlen(root) + 4096U,
      "{\"id\":\"%s\",\"expected_version\":%ld,\"filename\":\"../bad.md\"}", id,
      (long)v);
  call(ctx, "context_set", args, &c, 0);
  /* Replace content preserves metadata, identity and original creation time. */
  snprintf(args, strlen(root) + 4096U,
           "{\"id\":\"%s\",\"expected_version\":%ld,\"path\":\"download.md\"}",
           id, (long)v);
  call(ctx, "context_replace", args, &c, 1);
  r = decode(&c);
  v = r.version;
  snprintf(args, strlen(root) + 4096U, "{\"id\":\"%s\"}", id);
  call(ctx, "context_get", args, &c, 1);
  assert(strstr(c.bytes, "notes.md") && strstr(c.bytes, "2026-11-10"));
  snprintf(args, strlen(root) + 4096U,
           "{\"id\":\"%s\",\"expected_version\":%ld,\"path\":\"binary.md\"}",
           id, (long)v);
  call(ctx, "context_replace", args, &c, 1);
  r = decode(&c);
  v = r.version;
  assert(!strcmp(r.id, id) && !strcmp(r.kind, "file"));
  snprintf(args, strlen(root) + 4096U,
           "{\"id\":\"%s\",\"path\":\"converted.bin\"}", id);
  call(ctx, "context_download", args, &c, 1);
  snprintf(path, strlen(root) + 1024U, "%s/converted.bin", root);
  check_file(path, "a\0b\377", 4U);
  snprintf(args, strlen(root) + 4096U,
           "{\"id\":\"%s\",\"expected_version\":%ld,\"path\":\"download.md\"}",
           id, (long)v);
  call(ctx, "context_replace", args, &c, 1);
  r = decode(&c);
  v = r.version;
  assert(!strcmp(r.kind, "text"));
  {
    lc_attachment_list_req request;
    lc_attachment_list attachments;
    lc_attachment_list_req_init(&request);
    request.lease.ns = "cai.context";
    request.lease.key = id;
    request.public_read = 1;
    memset(&attachments, 0, sizeof(attachments));
    assert(client->list_attachments(client, &request, &attachments, &le) ==
           LC_OK);
    assert(attachments.count == 0U);
    lc_attachment_list_cleanup(&attachments);
  }
  /* Export includes every page, same-name items and complete binary/text files.
   */
  for (i = 0U; i < 34U; i++) {
    r = create(ctx, "page fixture", &c);
    assert(lc_xid_is_valid(r.id));
  }
  {
    cai_sink *sink = capture_sink(&c);
    int exported;
    c.used = 0U;
    exported = cai_context_export(ctx, "export", sink, &e);
    if (exported != CAI_OK)
      fprintf(stderr, "export: %s (%s)\n", e.message ? e.message : "",
              e.detail ? e.detail : "");
    assert(exported == CAI_OK);
    assert(strstr(c.bytes, "\"items\":40"));
    cai_sink_close(sink);
  }
  snprintf(path, strlen(root) + 1024U, "%s/export/%s/notes.md", root, id);
  check_file(path, original, strlen(original));
  snprintf(path, strlen(root) + 1024U, "%s/export/%s/binary.md", root,
           binary_id);
  check_file(path, "a\0b\377", 4U);
  snprintf(path, strlen(root) + 1024U, "%s/export/%s/notes.md", root, empty_id);
  check_file(path, "", 0U);
  snprintf(path, strlen(root) + 1024U, "%s/export/%s/.context.json", root,
           special_id);
  check_file(path, "%PDF-1.7\n", 9U);
  assert(cai_tool_registry_new(&registry, &e) == CAI_OK);
  assert(cai_context_register_registry(ctx, registry, &e) == CAI_OK);
  {
    cai_sink *sink = capture_sink(&c);
    c.used = 0U;
    assert(cai_tool_registry_run(registry, "context_query", "{\"limit\":1}",
                                 sink, &e) == CAI_OK);
    assert(strstr(c.bytes, "cursor") != NULL);
    cai_sink_close(sink);
  }
  cai_tool_registry_destroy(registry);
  registry = NULL;
  {
    cai_context *storage_only = NULL;
    cai_sink *sink = capture_sink(&c);
    config.namespace_name = NULL;
    config.root_path = NULL;
    assert(cai_context_open(&config, &storage_only, &e) == CAI_OK);
    assert(cai_tool_registry_new(&registry, &e) == CAI_OK);
    assert(cai_context_register_registry(storage_only, registry, &e) == CAI_OK);
    assert(cai_tool_registry_run(registry, "context_upload", "{}", sink, &e) !=
           CAI_OK);
    assert(strstr(e.message, "unknown tool") != NULL ||
           strstr(e.message, "not registered") != NULL);
    cai_error_cleanup(&e);
    cai_tool_registry_destroy(registry);
    cai_context_close(storage_only);
    cai_sink_close(sink);
  }
  snprintf(args, strlen(root) + 4096U,
           "{\"id\":\"%s\",\"expected_version\":%ld}", id, (long)v);
  call(ctx, "context_delete", args, &c, 1);
  snprintf(args, strlen(root) + 4096U, "{\"id\":\"%s\"}", id);
  call(ctx, "context_get", args, &c, 0);
  /* A malformed binary document must not prepend JSON text to its attachment.
   */
  {
    lc_acquire_req request;
    lc_release_req release;
    lc_mutate_req mutation;
    lc_lease *held = NULL;
    const char *expressions[] = {"/content=\"prefix\""};
    lc_acquire_req_init(&request);
    request.ns = "cai.context";
    request.key = binary_id;
    request.owner = "schema-fixture";
    request.ttl_seconds = 30;
    assert(client->acquire(client, &request, &held, &le) == LC_OK);
    lc_mutate_req_init(&mutation);
    mutation.mutations = expressions;
    mutation.mutation_count = 1;
    assert(held->mutate(held, &mutation, &le) == LC_OK);
    lc_release_req_init(&release);
    assert(held->release(held, &release, &le) == LC_OK);
    snprintf(args, strlen(root) + 4096U,
             "{\"id\":\"%s\",\"path\":\"invalid-mixed.bin\"}", binary_id);
    call(ctx, "context_download", args, &c, 0);
    snprintf(path, strlen(root) + 1024U, "%s/invalid-mixed.bin", root);
    assert(access(path, F_OK) != 0);
    snprintf(args, strlen(root) + 4096U, "{\"id\":\"%s\"}", binary_id);
    call(ctx, "context_get", args, &c, 0);
    held = NULL;
    assert(client->acquire(client, &request, &held, &le) == LC_OK);
    expressions[0] = "rm:/content";
    assert(held->mutate(held, &mutation, &le) == LC_OK);
    assert(held->release(held, &release, &le) == LC_OK);
  }
  snprintf(args, strlen(root) + 4096U, "{\"id\":\"%s\"}", binary_id);
  call(ctx, "context_get", args, &c, 1);
  r = decode(&c);
  snprintf(args, strlen(root) + 4096U,
           "{\"id\":\"%s\",\"expected_version\":%ld}", binary_id,
           (long)r.version);
  call(ctx, "context_delete", args, &c, 1);
  snprintf(args, strlen(root) + 4096U,
           "{\"id\":\"%s\",\"path\":\"deleted.bin\"}", binary_id);
  call(ctx, "context_download", args, &c, 0);
  snprintf(path, strlen(root) + 1024U, "%s/deleted.bin", root);
  assert(access(path, F_OK) != 0);
  {
    cai_sink *sink = capture_sink(&c);
    assert(cai_context_call(ctx, "context_get", "{}", sink, &e) != CAI_OK);
    assert(strstr(e.message, "valid XID"));
    snprintf(args, strlen(root) + 4096U, "{\"id\":\"%s\"}", binary_id);
    assert(cai_context_call(ctx, "context_get", args, sink, &e) != CAI_OK);
    assert(strstr(e.message, "valid XID") == NULL);
    cai_error_cleanup(&e);
    cai_sink_close(sink);
  }
  /* Caller may omit its error object; ordinary failures remain recoverable. */
  {
    cai_sink *sink = capture_sink(&c);
    assert(cai_context_call(ctx, "context_get", "{}", sink, NULL) != CAI_OK);
    cai_sink_close(sink);
  }
  cai_context_set_yolo(ctx, 1);
  call(ctx, "context_upload", "{\"path\":\"link\"}", &c, 1);
  cai_context_set_yolo(ctx, 0);
  call(ctx, "context_upload", "{\"path\":\"link\"}", &c, 0);
  cai_context_close(ctx);
  cai_context_close(other);
  cai_context_close(isolated);
  client->close(client);
  second_client->close(second_client);
  lc_error_cleanup(&le);
  cai_error_cleanup(&e);
  free(c.bytes);
  free(endpoint);
  free(path);
  free(args);
  free(large);
  return NULL;
}
int main(int argc, char **argv) {
  pthread_t worker;
  assert(argc == 2);
  assert(pthread_create(&worker, NULL, run, argv[1]) == 0);
  assert(pthread_join(worker, NULL) == 0);
  return 0;
}
