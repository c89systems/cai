/* Stored definitions use ordinary state objects. Publish initial metadata and
 * its INI attachment in one lease; readers never own a session collection. */
typedef struct stored_agent_metadata {
  char kind[16], name[16];
} stored_agent_metadata;
static const lonejson_field stored_agent_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(stored_agent_metadata, kind, "kind",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED_REQ(stored_agent_metadata, name, "name",
                                    LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(stored_agent_map, stored_agent_metadata,
                    stored_agent_fields);

static int agent_key(const char *name, char key[72], cai_error *error) {
  size_t n = name != NULL ? strlen(name) : 0U;
  if (n == 0U || n > 64U ||
      strspn(name, "abcdefghijklmnopqrstuvwxyz0123456789_-") != n)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "invalid canonical agent name");
  snprintf(key, 72U, "agent.%s", name);
  return CAI_OK;
}

int cai_cli_pouch_agent_read(cai_cli_pouch *pouch, const char *name,
                             const char *attachment, cai_source **out,
                             cai_error *error) {
  char key[72];
  int rc = agent_key(name, key, error);
  *out = NULL;
  return rc == CAI_OK
             ? download_source_ns(pouch->sessions, pouch->agents_namespace, key,
                                  attachment, out, error)
             : rc;
}

int cai_cli_pouch_agent_private(lc_lease *lease, cai_source **out,
                                cai_error *error) {
  return download_private(lease, NULL, out, error);
}

int cai_cli_pouch_agent_publish(cai_cli_pouch *pouch, const char *name,
                                int replace, cai_cli_agent_publish_fn publish,
                                void *context, cai_error *error) {
  char key[72];
  lc_lease *lease = NULL;
  int rc = agent_key(name, key, error);
  if (rc == CAI_OK)
    rc = acquire_wait(pouch, pouch->agents_namespace, key, 30L, &lease, error);
  if (rc == CAI_OK && !replace && lease->state_etag != NULL &&
      lease->state_etag[0] != '\0')
    rc = cai_cli_error(error, CAI_ERR_INVALID,
                       "agent already exists; use --yes or --force to replace");
  if (rc == CAI_OK)
    rc = publish(context, lease, error);
  return release_mode(lease, rc != CAI_OK, rc, error);
}

int cai_cli_pouch_agents_visit(cai_cli_pouch *pouch,
                               cai_cli_agent_visit_fn visit, void *context,
                               cai_error *error) {
  key_page *page = calloc(1U, sizeof(*page));
  lc_query_req query;
  lc_query_res result;
  lc_query_key_handler handler = {0};
  lc_error local;
  char *cursor = NULL;
  size_t i;
  int rc = CAI_OK;
  if (page == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate agent query page");
  lc_error_init(&local);
  lc_query_req_init(&query);
  handler.begin = key_begin;
  handler.chunk = key_chunk;
  handler.end = key_end;
  query.ns = pouch->agents_namespace;
  query.selector_lql = "eq{field=/kind,value=agent}";
  query.engine = "index";
  query.refresh = "wait_for";
  query.limit = 256;
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
      rc = cai_cli_error(error, CAI_ERR_NOMEM, "copy agent query cursor");
    lc_query_res_cleanup(&result);
    for (i = 0U; rc == CAI_OK && i < page->count; ++i) {
      if (strncmp(page->keys[i], "agent.", 6U) != 0)
        rc =
            cai_cli_error(error, CAI_ERR_INVALID, "invalid agent registry key");
      else
        rc = visit(context, page->keys[i] + 6U, error);
    }
  } while (rc == CAI_OK && cursor != NULL && *cursor != '\0');
  free(cursor);
  free(page);
  lc_error_cleanup(&local);
  return rc;
}

int cai_cli_pouch_ensure_default_agent(cai_cli_pouch *pouch,
                                       cai_cli_producer produce, void *seed,
                                       cai_error *error) {
  lc_describe_req describe;
  lc_describe_res observed = {0};
  lc_error local;
  lc_lease *lease = NULL;
  cai_source *source = NULL;
  lc_source *input = NULL;
  lc_attach_req attachment;
  lc_attach_res attached = {0};
  stored_agent_metadata metadata = {"agent", "default"};
  int rc, exists;
  lc_error_init(&local);
  lc_describe_req_init(&describe);
  describe.ns = pouch->agents_namespace;
  describe.key = "agent.default";
  rc = lc_describe(pouch->sessions, &describe, &observed, &local);
  exists = rc == LC_OK && observed.state_etag != NULL &&
           observed.state_etag[0] != '\0';
  if (rc != LC_OK && local.http_status != 404) {
    rc = cai_cli_lc_error(error, &local);
    goto done;
  }
  /* Missing is expected on first use; do not pass that stale SDK error into
   * attachment streaming callbacks belonging to the initialization lease. */
  lc_error_cleanup(&local);
  lc_error_init(&local);
  rc = CAI_OK;
  if (!exists) {
    rc = acquire_wait(pouch, pouch->agents_namespace, "agent.default", 30L,
                      &lease, error);
    if (rc == CAI_OK &&
        (lease->state_etag == NULL || lease->state_etag[0] == '\0')) {
      /* Attachments require an initialized parent in the private lease. Both
       * changes remain unpublished until the same successful release. */
      rc = cai_cli_mapped_source(&stored_agent_map, &metadata, &source, error);
      if (rc == CAI_OK)
        rc = update(lease, source, error);
      cai_source_close(source);
      source = NULL;
      if (rc == CAI_OK)
        rc = cai_cli_stream_source(produce, seed, NULL, &source, error);
      if (rc == CAI_OK)
        rc = cai_cli_lc_source(source, &input, error);
      lc_attach_req_init(&attachment);
      attachment.name = "config.ini";
      attachment.content_type = "text/plain";
      if (rc == CAI_OK && lc_lease_attach(lease, &attachment, input, &attached,
                                          &local) != LC_OK)
        rc = cai_cli_lc_error(error, &local);
      lc_source_close(input);
      input = NULL;
      cai_source_close(source);
      source = NULL;
    }
    rc = release_mode(lease, rc != CAI_OK, rc, error);
  }
done:
  lc_attach_res_cleanup(&attached);
  lc_describe_res_cleanup(&observed);
  lc_error_cleanup(&local);
  return rc;
}

int cai_cli_pouch_default_agent(cai_cli_pouch *pouch, cai_cli_producer produce,
                                void *seed, cai_source **out,
                                cai_error *error) {
  stored_agent_metadata metadata = {0};
  cai_source *source = NULL;
  int rc = cai_cli_pouch_ensure_default_agent(pouch, produce, seed, error);
  *out = NULL;
  if (rc == CAI_OK)
    rc = cai_cli_pouch_agent_read(pouch, "default", NULL, &source, error);
  if (rc == CAI_OK)
    rc = parse_source(source, &stored_agent_map, &metadata, error);
  cai_source_close(source);
  if (rc == CAI_OK &&
      (strcmp(metadata.kind, "agent") || strcmp(metadata.name, "default")))
    rc = cai_cli_error(error, CAI_ERR_INVALID,
                       "invalid stored default agent metadata");
  return rc == CAI_OK ? cai_cli_pouch_agent_read(pouch, "default", "config.ini",
                                                 out, error)
                      : rc;
}
