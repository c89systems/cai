#define CAI_CUT_FAIL(e, m) cai_cli_error((e), CAI_ERR_INVALID, (m))
#include "../src/cai_session_cut.h"
#undef CAI_CUT_FAIL

typedef struct pouch_cut {
  cai_cut_plan plan;
  lc_collection_head_res head;
  lc_collection_edit_range ranges[CAI_CUT_MAX_RANGES];
  cai_error *error;
  int rc;
} pouch_cut;

static int pouch_cut_write(lc_sink *sink, const void *bytes, size_t length,
                           lc_error *error) {
  pouch_cut *cut = sink->impl;
  (void)error;
  cut->rc = cai_cut_feed(&cut->plan, bytes, length, cut->error);
  return cut->rc == CAI_OK;
}
static int pouch_cut_boundary(void *context,
                              const lc_collection_read_metadata *metadata,
                              lc_error *error) {
  pouch_cut *cut = context;
  (void)error;
  if (metadata->kind != LC_COLLECTION_READ_METADATA_ENTRY)
    return LC_OK;
  cut->rc = cai_cut_end(&cut->plan, metadata->position, cut->error);
  return cut->rc == CAI_OK ? LC_OK : LC_ERR_INVALID;
}
static void pouch_cut_close(pouch_cut *cut) {
  if (cut == NULL)
    return;
  cai_cut_record_close(&cut->plan);
  lonejson_free(cut->plan.json);
  free(cut);
}
static int pouch_cut_prepare(lc_collection *collection,
                             const cai_agent_session_trim_request *request,
                             pouch_cut **out, cai_error *error) {
  pouch_cut *cut = calloc(1U, sizeof(*cut));
  lc_collection_read_opts options;
  lc_collection_read_res result;
  lc_error local;
  lonejson_error json_error;
  lc_sink sink;
  size_t i;
  int rc;
  *out = NULL;
  if (cut == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "prepare atomic history cut");
  cut->error = error;
  cut->plan.request = request;
  cut->plan.json = cai_cut_json(&json_error);
  lc_error_init(&local);
  lc_collection_read_opts_init(&options);
  lc_collection_read_res_init(&result);
  memset(&sink, 0, sizeof(sink));
  sink.write = pouch_cut_write;
  sink.impl = cut;
  rc = cut->plan.json != NULL ? CAI_OK : cai_cli_lj_error(error, &json_error);
  if (rc == CAI_OK && collection->head(collection, &cut->head, &local) != LC_OK)
    rc = cai_cli_lc_error(error, &local);
  options.expected_head = cut->head.etag;
  options.has_through = 1;
  options.through = cut->head.upper_position;
  options.on_metadata = pouch_cut_boundary;
  options.metadata_context = cut;
  if (rc == CAI_OK &&
      collection->read(collection, &options, &sink, &result, &local) != LC_OK)
    rc = cut->rc != CAI_OK ? cut->rc : cai_cli_lc_error(error, &local);
  if (rc == CAI_OK)
    rc = cai_cut_finish(&cut->plan, error);
  if (rc == CAI_OK && cut->plan.anchor != 0U &&
      cai_cut_removed(&cut->plan, cut->plan.anchor))
    rc =
        cai_cli_error(error, CAI_ERR_INVALID, "history cut removes its prefix");
  if (rc == CAI_OK) {
    for (i = 0U; i < cut->plan.count; i++) {
      cut->ranges[i].after = cut->plan.ranges[i].after;
      cut->ranges[i].through = cut->plan.ranges[i].through;
    }
    *out = cut;
  } else
    pouch_cut_close(cut);
  lc_collection_read_res_cleanup(&result);
  lc_error_cleanup(&local);
  return rc;
}
static int pouch_cut_publish(cai_cli_pouch *pouch, lc_collection *collection,
                             const cai_cli_session *session,
                             const char *append_id, cai_source *source,
                             pouch_cut *cut, unsigned long long *position,
                             cai_error *error) {
  lc_collection_edit_req request;
  lc_collection_edit_res result;
  lc_collection_append_req replacement;
  lc_source *input = NULL;
  lc_error local;
  long seconds = 0L;
  int rc = session_retention(pouch, session, &seconds, error);
  lc_collection_edit_req_init(&request);
  lc_collection_append_req_init(&replacement);
  memset(&result, 0, sizeof(result));
  lc_error_init(&local);
  request.operation_id = append_id;
  request.expected_head = cut->head.etag;
  request.ranges = cut->ranges;
  request.range_count = cut->plan.count;
  request.replacement = &replacement;
  replacement.append_id = append_id;
  replacement.has_collection_retention_seconds = 1;
  replacement.collection_retention_seconds = seconds;
  if (rc == CAI_OK)
    rc = cai_cli_lc_source(source, &input, error);
  if (rc == CAI_OK) {
    int written =
        collection->edit(collection, &request, input, &result, &local);
    if (written != LC_OK) {
      rc = cai_cli_lc_error(error, &local);
      if (result.published) {
        /* A receipt proves logical publication, not recovery from a failed
         * durable-sync acknowledgement. Never roll forward into more writes. */
        pthread_mutex_lock(&clients_lock);
        if (pouch->connection != NULL)
          pouch->connection->write_uncertain = 1;
        pthread_mutex_unlock(&clients_lock);
        rc = cai_cli_error(
            error, CAI_ERR_TRANSPORT,
            "atomic history cut was published but not acknowledged; "
            "reopen the store before writing again");
      }
    } else if (!result.has_replacement || result.replacement.deleted)
      rc = cai_cli_error(error, CAI_ERR_INVALID,
                         "atomic history cut has no live replacement");
    else
      *position = result.replacement.position;
  }
  lc_source_close(input);
  lc_error_cleanup(&local);
  return rc;
}
