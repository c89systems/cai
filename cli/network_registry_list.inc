/* Registry listing retains one key page and one fixed row, never the registry.
 */
typedef struct network_key_page {
  char keys[32][129];
  size_t count, length;
} network_key_page;
static int key_begin(void *context, lc_error *error) {
  network_key_page *page = context;
  page->length = 0U;
  if (page->count >= 32U) {
    lc_error_set(error, LC_ERR_INVALID, 0L, "registry page exceeded limit",
                 NULL, NULL, NULL);
    return 0;
  }
  return 1;
}
static int key_chunk(void *context, const char *bytes, size_t count,
                     lc_error *error) {
  network_key_page *page = context;
  if (count > 128U - page->length || memchr(bytes, 0, count) != NULL) {
    lc_error_set(error, LC_ERR_INVALID, 0L, "invalid registry key", NULL, NULL,
                 NULL);
    return 0;
  }
  memcpy(page->keys[page->count] + page->length, bytes, count);
  page->length += count;
  return 1;
}
static int key_end(void *context, lc_error *error) {
  network_key_page *page = context;
  (void)error;
  page->keys[page->count++][page->length] = '\0';
  return 1;
}
typedef struct registry_output {
  cai_sink *sink;
  cai_error *error;
} registry_output;
static lonejson_status registry_write(void *context, const void *bytes,
                                      size_t count, lonejson_error *error) {
  registry_output *out = context;
  (void)error;
  return cai_sink_write(out->sink, bytes, count, out->error) == CAI_OK
             ? LONEJSON_STATUS_OK
             : LONEJSON_STATUS_IO_ERROR;
}
static int print_field(cai_sink *sink, const char *value, cai_error *error) {
  char buffer[512];
  size_t used = 0U;
  while (*value) {
    unsigned char c = (unsigned char)*value++;
    buffer[used++] = c < 32U || c == 127U ? '?' : (char)c;
    if (used == sizeof(buffer)) {
      int rc = cai_sink_write(sink, buffer, used, error);
      if (rc != CAI_OK)
        return rc;
      used = 0U;
    }
  }
  return used != 0U ? cai_sink_write(sink, buffer, used, error) : CAI_OK;
}
static int print_endpoint(cai_sink *sink, const endpoint_record *row,
                          cai_error *error) {
  char detail[256];
  int rc;
  const char *address = row->advertised[0] ? row->advertised : row->bind;
  rc = print_field(sink, row->kind, error);
  if (rc == CAI_OK)
    rc = cai_sink_write(sink, "  ", 2U, error);
  if (rc == CAI_OK)
    rc = print_field(sink, address, error);
  if (rc == CAI_OK)
    rc = cai_sink_write(sink, "  ", 2U, error);
  if (rc == CAI_OK)
    rc = print_field(sink, row->workspace, error);
  snprintf(detail, sizeof(detail), "  attachment=%llu  session=",
           (unsigned long long)row->attachment_id);
  if (rc == CAI_OK)
    rc = cai_sink_write(sink, detail, strlen(detail), error);
  if (rc == CAI_OK)
    rc = print_field(sink, row->session_id[0] ? row->session_id : "--", error);
  if (rc == CAI_OK && !row->available)
    rc = print_field(sink, "  unavailable", error);
  if (rc == CAI_OK)
    rc = cai_sink_write(sink, "\n", 1U, error);
  if (rc == CAI_OK && row->socat[0]) {
    rc = cai_sink_write(sink, "  ", 2U, error);
    if (rc == CAI_OK)
      rc = print_field(sink, row->socat, error);
    if (rc == CAI_OK)
      rc = cai_sink_write(sink, "\n", 1U, error);
  }
  return rc;
}
static int registry_listing(cai_cli_network_registry *r, const char *workspace,
                            const char *after, int plain, cai_sink *sink,
                            cai_error *error) {
  network_key_page *page = calloc(1U, sizeof(*page));
  endpoint_record *row = calloc(1U, sizeof(*row));
  lc_query_req query;
  lc_query_res response;
  lc_query_key_handler handler;
  lc_get_opts get;
  lc_get_res result;
  lc_error local;
  lonejson_error json_error;
  lonejson *json;
  lonejson_writer writer;
  registry_output out;
  lonejson_status status;
  int rc = CAI_OK;
  size_t displayed = 0U, pages = 0U, i;
  char *cursor = after != NULL && *after ? strdup(after) : NULL;
  if (page == NULL || row == NULL) {
    free(page);
    free(row);
    return cai_cli_error(error, CAI_ERR_NOMEM,
                         "allocate registry listing page");
  }
  lc_error_init(&local);
  lonejson_error_init(&json_error);
  memset(&writer, 0, sizeof(writer));
  json = lonejson_new(NULL, &json_error);
  out.sink = sink;
  out.error = error;
  status = json != NULL ? lonejson_writer_init_sink(
                              json, &writer, registry_write, &out, &json_error)
                        : LONEJSON_STATUS_IO_ERROR;
  if (!plain) {
    if (status == LONEJSON_STATUS_OK)
      status = lonejson_writer_begin_object(&writer, &json_error);
    if (status == LONEJSON_STATUS_OK)
      status = lonejson_writer_key(&writer, "endpoints", 9U, &json_error);
    if (status == LONEJSON_STATUS_OK)
      status = lonejson_writer_begin_array(&writer, &json_error);
  }
  handler.begin = key_begin;
  handler.chunk = key_chunk;
  handler.end = key_end;
  do {
    memset(page, 0, sizeof(*page));
    memset(&response, 0, sizeof(response));
    lc_query_req_init(&query);
    query.ns = r->ns;
    query.limit = 32L;
    query.cursor = cursor;
    rc = lc_query_keys(r->pouch->sessions, &query, &handler, page, &response,
                       &local);
    free(cursor);
    cursor = NULL;
    if (rc == LC_OK && response.cursor != NULL && *response.cursor) {
      cursor = strdup(response.cursor);
      if (cursor == NULL)
        rc = LC_ERR_NOMEM;
    }
    lc_query_res_cleanup(&response);
    for (i = 0U; rc == LC_OK && status == LONEJSON_STATUS_OK && i < page->count;
         i++) {
      memset(row, 0, sizeof(*row));
      memset(&result, 0, sizeof(result));
      lc_get_opts_init(&get);
      get.public_read = 1;
      rc = lc_load_in_namespace(r->pouch->sessions, r->ns, page->keys[i],
                                &endpoint_map, row, &get, &result, &local);
      lc_get_res_cleanup(&result);
      if (rc != LC_OK && (local.http_status == 404L ||
                          (local.server_code != NULL &&
                           !strcmp(local.server_code, "not_found")))) {
        lc_error_cleanup(&local);
        rc = LC_OK;
        continue;
      }
      if (rc == LC_OK && row->id[0] &&
          (!workspace || !*workspace || !strcmp(workspace, row->workspace))) {
        if (plain) {
          if (!displayed)
            rc = cai_sink_write(sink, "\nNetwork endpoints\n", 19U, error);
          if (rc == CAI_OK)
            rc = print_endpoint(sink, row, error);
        } else
          status =
              lonejson_writer_mapped(&writer, &endpoint_map, row, &json_error);
        displayed++;
      }
    }
    pages++;
  } while (rc == LC_OK && status == LONEJSON_STATUS_OK && cursor != NULL &&
           (plain || (displayed < 64U && pages < 8U)));
  if (!plain) {
    if (status == LONEJSON_STATUS_OK)
      status = lonejson_writer_end_array(&writer, &json_error);
    if (status == LONEJSON_STATUS_OK)
      status = lonejson_writer_key(&writer, "cursor", 6U, &json_error);
    if (status == LONEJSON_STATUS_OK)
      status = lonejson_writer_string(&writer, cursor != NULL ? cursor : "",
                                      cursor != NULL ? strlen(cursor) : 0U,
                                      &json_error);
    if (status == LONEJSON_STATUS_OK)
      status = lonejson_writer_end_object(&writer, &json_error);
    if (status == LONEJSON_STATUS_OK)
      status = lonejson_writer_finish(&writer, &json_error);
  }
  if (rc != LC_OK)
    rc = cai_cli_lc_error(error, &local);
  else if (status != LONEJSON_STATUS_OK)
    rc = cai_cli_error(error, CAI_ERR_PROTOCOL, json_error.message);
  lonejson_writer_cleanup(&writer);
  lonejson_free(json);
  lc_error_cleanup(&local);
  free(cursor);
  free(page);
  free(row);
  return rc;
}
int cai_cli_network_registry_list(cai_cli_network_registry *r,
                                  const char *workspace, const char *after,
                                  cai_sink *sink, cai_error *error) {
  return registry_listing(r, workspace, after, 0, sink, error);
}
int cai_cli_network_registry_print(cai_cli_pouch *pouch,
                                   const cai_cli_options *options,
                                   const char *workspace, cai_sink *sink,
                                   cai_error *error) {
  cai_cli_network_registry *r = calloc(1U, sizeof(*r));
  int rc;
  if (r == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate endpoint lookup");
  r->pouch = pouch;
  rc = snprintf(r->ns, sizeof(r->ns), "%s.network",
                options->namespace_prefix != NULL ? options->namespace_prefix
                                                  : options->program_name);
  if (rc < 0 || (size_t)rc >= sizeof(r->ns)) {
    free(r);
    return cai_cli_error(error, CAI_ERR_LIMIT,
                         "network namespace exceeds 128 bytes");
  }
  rc = registry_listing(r, workspace, NULL, 1, sink, error);
  free(r);
  return rc;
}
