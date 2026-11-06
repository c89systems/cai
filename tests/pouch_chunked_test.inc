/* Released SDK push/staged-pull records through CAI's real encrypted journal,
 * native projection and recovery readers. No upstream internals are used. */
#include "../cli/journal_projection.h"

static int chunked_form;
static int chunked_append(lc_collection *collection,
                          const lc_collection_append_req *request,
                          lc_source *source, lc_collection_append_res *result,
                          lc_error *error) {
  lc_collection_writer *writer = NULL;
  unsigned char bytes[317];
  size_t count;
  int rc;
  if (chunked_form != 0)
    return collection->append_staged(collection, request, source, result,
                                     error);
  rc = collection->append_begin(collection, request, &writer, error);
  while (rc == LC_OK &&
         (count = source->read(source, bytes, sizeof(bytes), error)) != 0U)
    rc = writer->write(writer, bytes, count, error);
  if (rc == LC_OK && error->code != LC_OK)
    rc = error->code;
  if (rc == LC_OK)
    rc = writer->commit(writer, result, error);
  if (writer != NULL)
    writer->close(writer);
  return rc;
}
typedef struct chunked_seen {
  const char *text;
  size_t count, bytes, length;
} chunked_seen;
static int chunked_event(void *context, const cai_agent_session_event *event,
                         cai_error *error) {
  chunked_seen *seen = context;
  (void)error;
  assert(event->sequence == ++seen->count);
  if (seen->count <= 2U) {
    /* The bounded runtime recovery API omits oversized bodies; the native
     * projection below must still stream their complete original text. */
    assert(strcmp(event->type, "replay_event_omitted") == 0);
    assert(event->data == NULL);
  } else {
    assert(event->data != NULL && strcmp(event->data, "direct tail") == 0);
  }
  return CAI_OK;
}
static int chunked_span(void *context, const cai_cli_transcript_event *event,
                        cai_error *error) {
  chunked_seen *seen = context;
  const char *expected = seen->count < 2U ? seen->text : "direct tail";
  size_t length = seen->count < 2U ? seen->length : sizeof("direct tail") - 1U;
  (void)error;
  if (event->kind == CAI_CLI_TRANSCRIPT_BEGIN) {
    assert(event->sequence == seen->count + 1U);
    seen->bytes = 0U;
  } else if (event->kind == CAI_CLI_TRANSCRIPT_DATA) {
    assert(seen->bytes <= length && event->length <= length - seen->bytes);
    assert(memcmp(event->data, expected + seen->bytes, event->length) == 0);
    seen->bytes += event->length;
  } else {
    assert(seen->bytes == length);
    ++seen->count;
  }
  return CAI_OK;
}
static void chunked_verify(cai_cli_pouch *pouch, const char *id,
                           const char *text, cai_error *error) {
  lc_collection *reader = NULL;
  cai_cli_journal_view *view = NULL;
  chunked_seen seen;
  memset(&seen, 0, sizeof(seen));
  seen.text = text;
  seen.length = strlen(text);
  assert(pouch->store.load_events_after(pouch, "/work/chunked", id, 0U,
                                        chunked_event, &seen, error) == CAI_OK);
  assert(seen.count == 3U);
  assert(open_history_collection(pouch, id, "events", 0, 0, &reader, error) ==
         CAI_OK);
  assert(cai_cli_journal_view_open(reader, NULL, &view, error) == CAI_OK);
  seen.count = seen.bytes = 0U;
  assert(cai_cli_journal_view_events(view, chunked_span, &seen, error) ==
         CAI_OK);
  assert(seen.count == 3U);
  cai_cli_journal_view_close(view);
  reader->close(reader);
}
static void *pouch_chunked_worker(void *unused) {
  static const char *const ids[] = {"chunked-push", "chunked-pull"};
  cai_cli_pouch *pouch = calloc(1U, sizeof(*pouch));
  cai_cli_options *options = calloc(1U, sizeof(*options));
  char *text = malloc(256U * 1024U + 1U);
  cai_source_callbacks callbacks;
  cai_source *source = NULL;
  cai_agent_session_event event;
  cai_error error;
  lc_error local;
  size_t i, offset;
  unsigned long random = 7UL;
  int form, pass;
  (void)unused;
  assert(pouch && options && text);
  /* Keep compressed output larger than one codec chunk too. */
  for (i = 0U; i < 256U * 1024U; ++i) {
    random = random * 1664525UL + 1013904223UL;
    text[i] = "0123456789abcdef"[(random >> 24U) & 15UL];
  }
  text[2047] = '\n';
  text[4095] = '"';
  text[i] = '\0';
  cai_error_init(&error);
  lc_error_init(&local);
  options->conversation_retention_days = 30L;
  assert(cai_cli_pouch_open_options(pouch, options, NULL, &error) == CAI_OK);
  for (form = 0; form < 2; ++form) {
    cai_cli_journal_context *journal;
    lc_collection_append_req request;
    lc_collection_append_status_res status;
    lc_collection_writer *unfinished = NULL;
    lc_collection_head_res before, after;
    int (*direct)(lc_collection *, const lc_collection_append_req *,
                  lc_source *, lc_collection_append_res *, lc_error *);
    offset = 0U;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.read = journal_identity_read;
    callbacks.context = &offset;
    assert(cai_source_from_callbacks(&callbacks, &source, &error) == CAI_OK);
    assert(pouch->store.commit(pouch, "/work/chunked", ids[form], source, 0U,
                               &error) == CAI_OK);
    cai_source_close(source);
    assert(journal_context_open(pouch, "/work/chunked", ids[form], &journal,
                                &error) == CAI_OK);
    direct = journal->collection->append;
    journal->collection->append = chunked_append;
    chunked_form = form;
    memset(&event, 0, sizeof(event));
    event.type = "assistant_text_delta";
    event.data = text;
    for (event.sequence = 1U; event.sequence <= 2U; ++event.sequence)
      assert(append_event(pouch, "/work/chunked", ids[form], &event, &error) ==
             CAI_OK);
    /* Retry the identical append through the other encoding path. */
    event.sequence = 2U;
    chunked_form = !form;
    assert(append_event(pouch, "/work/chunked", ids[form], &event, &error) ==
           CAI_OK);
    journal->collection->append = direct;
    lc_collection_append_req_init(&request);
    request.append_id = "event-00000000000000000003";
    assert(journal->collection->head(journal->collection, &before, &local) ==
           LC_OK);
    assert(journal->collection->append_begin(journal->collection, &request,
                                             &unfinished, &local) == LC_OK);
    assert(unfinished->write(unfinished, "{\"partial\":\"", 12U, &local) ==
           LC_OK);
    assert(unfinished->write(unfinished, text + 8192U, 128U * 1024U, &local) ==
           LC_OK);
    unfinished->close(unfinished);
    assert(journal->collection->head(journal->collection, &after, &local) ==
           LC_OK);
    assert(after.upper_position == before.upper_position &&
           after.live_count == before.live_count);
    assert(journal->collection->append_status(journal->collection,
                                              request.append_id, &status,
                                              &local) == LC_OK);
    assert(status.state == LC_COLLECTION_APPEND_ABSENT);
    event.sequence = 3U;
    event.data = "direct tail";
    assert(append_event(pouch, "/work/chunked", ids[form], &event, &error) ==
           CAI_OK);
    chunked_verify(pouch, ids[form], text, &error);
  }
  /* Reopen and convert both ways; all readers must preserve exact text/order.
   */
  for (pass = 0; pass < 3; ++pass) {
    cai_cli_pouch_close(pouch);
    options->disable_compression = pass == 1;
    assert(cai_cli_pouch_open_options(pouch, options, NULL, &error) == CAI_OK);
    for (form = 0; form < 2; ++form)
      chunked_verify(pouch, ids[form], text, &error);
  }
  cai_cli_pouch_close(pouch);
  cai_error_cleanup(&error);
  lc_error_cleanup(&local);
  free(text);
  free(options);
  free(pouch);
  return NULL;
}
