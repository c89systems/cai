/* Exercise failures before publication, a stale expected head, and a lost
 * acknowledgement after the real SDK publishes its atomic edit. */
static int cut_fault_mode, cut_edit_calls;
static size_t cut_notifications;
static void cut_test_notification(void *context, const char *session,
                                  unsigned long long position,
                                  const cai_agent_session_event *event) {
  (void)context;
  assert((strcmp(session, "atomic-cut") == 0 ||
          strcmp(session, "atomic-cut-success") == 0) &&
         event == NULL && position != 0U);
  ++cut_notifications;
}
static int (*cut_real_edit)(lc_collection *, const lc_collection_edit_req *,
                            lc_source *, lc_collection_edit_res *, lc_error *);
typedef struct cut_test_text {
  char text[512];
  size_t offset;
} cut_test_text;
static size_t cut_test_read(void *context, void *bytes, size_t count,
                            cai_error *error) {
  cut_test_text *input = context;
  size_t remaining = strlen(input->text) - input->offset;
  (void)error;
  if (count > remaining)
    count = remaining;
  memcpy(bytes, input->text + input->offset, count);
  input->offset += count;
  return count;
}
static void cut_test_close(void *context) { free(context); }
static int cut_test_replacement(void *context, unsigned long long revision,
                                size_t bytes, cai_source **out,
                                cai_error *error) {
  cut_test_text *input = calloc(1U, sizeof(*input));
  cai_source_callbacks callbacks;
  int rc;
  (void)context;
  assert(revision == 1U && bytes == 0U && input != NULL);
  assert(snprintf(input->text, sizeof(input->text),
                  "{\"revision\":%llu,\"base_bytes\":0,\"end_bytes\":0,"
                  "\"blocks_keep\":0,\"hidden_keep\":0,\"owned_keep\":0,"
                  "\"metadata\":{\"version\":1},\"history_append\":\"\"}",
                  revision + 1U) > 0);
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.read = cut_test_read;
  callbacks.close = cut_test_close;
  callbacks.context = input;
  rc = cai_source_from_callbacks(&callbacks, out, error);
  if (rc != CAI_OK)
    free(input);
  return rc;
}
static int cut_test_edit(lc_collection *collection,
                         const lc_collection_edit_req *request,
                         lc_source *source, lc_collection_edit_res *result,
                         lc_error *error) {
  int rc;
  cut_edit_calls++;
  if (cut_fault_mode == 2) {
    /* Mutate between head capture and edit, using the same owned receiver.
     * The expected-head precondition must reject the whole cut. */
    cai_agent_session_event event;
    cai_source *record = NULL;
    lc_source *input = NULL;
    cai_error local;
    lc_collection_append_req append;
    lc_collection_append_res appended;
    memset(&event, 0, sizeof(event));
    event.sequence = 3U;
    event.type = "test_event";
    event.data = "RACING event";
    cai_error_init(&local);
    lc_collection_append_req_init(&append);
    append.append_id = "event-00000000000000000003";
    assert(cai_session_event_source(&event, &record, &local) == CAI_OK);
    assert(cai_cli_lc_source(record, &input, &local) == CAI_OK);
    assert(collection->append(collection, &append, input, &appended, error) ==
           LC_OK);
    lc_source_close(input);
    cai_source_close(record);
    cai_error_cleanup(&local);
    return cut_real_edit(collection, request, source, result, error);
  }
  if (cut_fault_mode == 3) {
    rc = cut_real_edit(collection, request, source, result, error);
    assert(rc == LC_OK && result->published && result->has_replacement);
  }
  error->code = LC_ERR_TRANSPORT;
  error->message = strdup("injected atomic cut acknowledgement failure");
  return LC_ERR_TRANSPORT;
}
static int cut_test_observe(void *context, const cai_agent_session_event *event,
                            cai_error *error) {
  size_t *count = context;
  (void)error;
  assert(event->sequence == ++*count);
  return CAI_OK;
}
static void pouch_atomic_cuts(cai_cli_pouch *pouch, cai_error *error) {
  const char *id = "atomic-cut", *scope = "/work/atomic-cut";
  cai_source_callbacks callbacks;
  cai_source *source = NULL;
  lc_collection *collection;
  lc_collection_info before, after;
  lc_error local;
  cai_agent_session_trim_request request;
  cai_agent_session_event event;
  cai_cli_pouch *alias = calloc(1U, sizeof(*alias));
  char bytes[2048];
  size_t offset = 0U, count, events_seen;
  unsigned long long sequence;
  int mode, calls, rc;
  assert(alias != NULL);
  memset(&request, 0, sizeof(request));
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.read = journal_identity_read;
  callbacks.context = &offset;
  assert(cai_source_from_callbacks(&callbacks, &source, error) == CAI_OK);
  assert(pouch->store.commit(pouch, scope, id, source, 0U, error) == CAI_OK);
  cai_source_close(source);
  event.type = "test_event";
  event.sequence = 1U;
  event.data = "KEEP";
  assert(pouch->store.append_event(pouch, scope, id, &event, error) == CAI_OK);
  event.sequence = 2U;
  event.data = "DROP";
  assert(pouch->store.append_event(pouch, scope, id, &event, error) == CAI_OK);
  assert(collection_owner_open(pouch, id, "events", 0, 0, &collection, error) ==
         CAI_OK);
  cut_real_edit = collection->edit;
  collection->edit = cut_test_edit;
  lc_error_init(&local);
  request.first_sequence = request.through_sequence = 2U;
  pouch->journal_observer = cut_test_notification;
  for (mode = 1; mode <= 3; mode++) {
    cut_fault_mode = mode;
    assert(collection->describe(collection, &before, &local) == LC_OK);
    assert(pouch->store.trim(pouch, scope, id, &request, cut_test_replacement,
                             NULL, request.through_sequence, error) != CAI_OK);
    if (mode == 3)
      assert(strstr(error->message, "reopen") != NULL);
    assert(cut_notifications == (size_t)(mode == 3));
    cai_error_cleanup(error);
    cai_error_init(error);
    assert(collection->describe(collection, &after, &local) == LC_OK);
    if (mode < 3) {
      assert(after.live_count == before.live_count + (mode == 2));
      events_seen = 0U;
      rc = pouch->store.load_events_after(
          pouch, scope, id, 0U, cut_test_observe, &events_seen, error);
      if (rc != CAI_OK)
        fprintf(stderr, "cut mode %d read failure: %s (%s)\n", mode,
                error->message, error->detail != NULL ? error->detail : "");
      assert(rc == CAI_OK);
      assert(events_seen == (size_t)mode + 1U);
      request.through_sequence = 3U;
    }
  }
  pouch->journal_observer = NULL;
  /* Every alias shares the uncertain-write fence. No subsequent event may
   * reach the SDK until the owning store context has been reopened. */
  assert(cai_cli_pouch_open(alias, NULL, NULL, NULL, error) == CAI_OK);
  event.sequence = 4U;
  calls = cut_edit_calls;
  assert(alias->store.append_event(alias, scope, id, &event, error) ==
         CAI_ERR_TRANSPORT);
  assert(cut_edit_calls == calls);
  cai_error_cleanup(error);
  cai_error_init(error);
  cai_cli_pouch_close(alias);
  free(alias);
  collection->edit = cut_real_edit;
  cai_cli_pouch_close(pouch);
  assert(cai_cli_pouch_open(pouch, NULL, NULL, NULL, error) == CAI_OK);
  assert(cai_cli_pouch_read_id(pouch, scope, id, &source, &sequence, error) ==
         CAI_OK);
  assert(source != NULL);
  offset = 0U;
  while ((count = cai_source_read(source, bytes + offset,
                                  sizeof(bytes) - 1U - offset, error)) != 0U)
    offset += count;
  bytes[offset] = '\0';
  assert(offset < sizeof(bytes) - 1U && error->code == CAI_OK);
  /* The derived lookup watermark may still predate the lost acknowledgement;
   * the replacement journal commit is authoritative during restoration. */
  assert(strstr(bytes, "\"applied_event_sequence\":3") != NULL);
  assert(strstr(bytes, "KEEP") != NULL && strstr(bytes, "DROP") == NULL &&
         strstr(bytes, "RACING") == NULL &&
         strstr(bytes, "\"revision\":2") != NULL);
  cai_source_close(source);
  assert(pouch->store.append_event(pouch, scope, id, &event, error) == CAI_OK);
  lc_error_cleanup(&local);
}

static void pouch_cut_notification_success(cai_cli_pouch *pouch,
                                           cai_error *error) {
  const char *scope = "/work/atomic-cut-success", *id = "atomic-cut-success";
  cai_source_callbacks callbacks;
  cai_source *source = NULL;
  cai_agent_session_event event;
  cai_agent_session_trim_request request;
  size_t offset = 0U;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.read = journal_identity_read;
  callbacks.context = &offset;
  assert(cai_source_from_callbacks(&callbacks, &source, error) == CAI_OK);
  assert(pouch->store.commit(pouch, scope, id, source, 0U, error) == CAI_OK);
  cai_source_close(source);
  memset(&event, 0, sizeof(event));
  event.type = "test_event";
  event.sequence = 1U;
  event.data = "KEEP";
  assert(pouch->store.append_event(pouch, scope, id, &event, error) == CAI_OK);
  event.sequence = 2U;
  event.data = "DROP";
  assert(pouch->store.append_event(pouch, scope, id, &event, error) == CAI_OK);
  cut_notifications = 0U;
  pouch->journal_observer = cut_test_notification;
  memset(&request, 0, sizeof(request));
  request.first_sequence = request.through_sequence = 2U;
  assert(pouch->store.trim(pouch, scope, id, &request, cut_test_replacement,
                           NULL, 2U, error) == CAI_OK);
  assert(cut_notifications == 1U);
  pouch->journal_observer = NULL;
}
static void *pouch_cut_worker(void *context) {
  cai_cli_pouch *pouch = calloc(1U, sizeof(*pouch));
  cai_error error;
  (void)context;
  assert(pouch != NULL);
  cai_error_init(&error);
  assert(cai_cli_pouch_open(pouch, NULL, NULL, NULL, &error) == CAI_OK);
  pouch_atomic_cuts(pouch, &error);
  pouch_cut_notification_success(pouch, &error);
  cai_cli_pouch_close(pouch);
  cai_error_cleanup(&error);
  free(pouch);
  return NULL;
}
