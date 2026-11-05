static int test_queue_retained_event(void *context,
                                     const cai_agent_session_event *event,
                                     cai_error *error) {
  int *found = context;
  (void)error;
  if (event->data != NULL &&
      strstr(event->data, "accepted queue instruction") != NULL)
    *found = 1;
  return CAI_OK;
}
static void test_agent_runtime_pending_input(test_state *state) {
#define QUEUE_REQUIRE(condition)                                               \
  do {                                                                         \
    int valid = (condition);                                                   \
    expect_int(state, #condition, valid, 1L);                                  \
    if (!valid) {                                                              \
      fprintf(stderr, "pending input fixture line %d: code %d: %s (%s)\n",     \
              __LINE__, error.code,                                            \
              error.message != NULL ? error.message : "",                      \
              error.detail != NULL ? error.detail : "");                       \
      goto cleanup;                                                            \
    }                                                                          \
  } while (0)
  static const char response[] =
      "data: {\"type\":\"response.output_text.delta\",\"delta\":\"done\"}\n\n"
      "data: "
      "{\"type\":\"response.completed\",\"response\":{\"id\":\"resp_queue\"}}"
      "\n\n";
  static const char *required[] = {"accepted queue instruction"};
  static const char review_response[] =
      "data: "
      "{\"type\":\"response.output_text.delta\",\"delta\":\"{\\\"findings\\\":["
      "],"
      "\\\"overall_correctness\\\":\\\"patch is "
      "correct\\\",\\\"overall_explanation\\\":"
      "\\\"Queue fixture "
      "review.\\\",\\\"overall_confidence_score\\\":0.9}\"}\n\n"
      "data: "
      "{\"type\":\"response.completed\",\"response\":{\"id\":\"resp_review\"}}"
      "\n\n";
  static const char *recovered[] = {"instruction retained after child exit"};
  static const mock_http_expectation script[] = {
      {"POST /v1/responses HTTP/", required, 1U, NULL, 0U, 200, "OK",
       "text/event-stream", NULL, response},
      {"POST /v1/responses HTTP/", required, 1U, NULL, 0U, 200, "OK",
       "text/event-stream", NULL, response},
      {"POST /v1/responses HTTP/", NULL, 0U, NULL, 0U, 200, "OK",
       "text/event-stream", NULL, review_response},
      {"POST /v1/responses HTTP/", NULL, 0U, NULL, 0U, 200, "OK",
       "text/event-stream", NULL, review_response},
      {"POST /v1/responses HTTP/", recovered, 1U, NULL, 0U, 200, "OK",
       "text/event-stream", NULL, response}};
  char root[] = CAI_TEST_BINARY_DIR "/test-tmp/runtime-queue-XXXXXX";
  cai_agent_runtime_config config;
  cai_agent_local_session_store_config storage;
  cai_agent_session_store store;
  cai_agent_runtime *runtime = NULL;
  cai_agent_runtime *child = NULL;
  cai_agent_review_request review;
  cai_input_queue *queue = NULL;
  cai_input_queue_client *attachment = NULL;
  cai_input_request request;
  cai_input_result result;
  cai_input_state pending;
  cai_input_entry item;
  cai_input_claim claim;
  cai_agent_run_state run_state;
  cai_sink *sink = NULL;
  cai_sink_callbacks callbacks;
  mcp_sink_state *captured = NULL;
  http_mock_client mock;
  cai_error error;
  input_identity_trace identity;
  char input_call[256];
  int i, trimmed_payload = 0;
  if (http_mock_client_open_script(state, "pending_input", script,
                                   sizeof(script) / sizeof(script[0]),
                                   &mock) != 0)
    return;
  cai_error_init(&error);
  memset(&store, 0, sizeof(store));
  QUEUE_REQUIRE(mkdtemp(root) != NULL);
  cai_agent_local_session_store_config_init(&storage);
  storage.root_directory = root;
  QUEUE_REQUIRE(cai_agent_local_session_store_open(&storage, &store, &error) ==
                CAI_OK);
  cai_agent_runtime_config_init(&config);
  config.workspace_directory = "/tmp";
  config.session_scope = "pending-input-fixture";
  config.session_id = "conversation-A";
  config.session_store = &store;
  config.record_transcript = 1;
  config.disable_web_search = config.disable_chrome = config.disable_terminal =
      1;
  QUEUE_REQUIRE(
      cai_agent_runtime_open(mock.client, &config, &runtime, &error) == CAI_OK);
  QUEUE_REQUIRE(cai_agent_runtime_pending_input(runtime, &queue, &error) ==
                CAI_OK);
  QUEUE_REQUIRE(cai_input_queue_client_open(queue, &attachment, &error) ==
                CAI_OK);
  memset(&request, 0, sizeof(request));
  request.request_id = 1U;
  request.operation = CAI_INPUT_OFFER;
  request.text = "accepted queue instruction";
  request.enabled = 1;
  QUEUE_REQUIRE(
      cai_input_queue_request(attachment, &request, &result, &error) == CAI_OK);
  QUEUE_REQUIRE(result.outcome == CAI_INPUT_ACCEPTED);
  cai_input_queue_client_close(attachment);
  attachment = NULL;
  cai_agent_runtime_close(runtime);
  runtime = NULL;
  /* New conversations do not inherit A's accepted text or approval hold. */
  config.session_id = "conversation-B";
  QUEUE_REQUIRE(
      cai_agent_runtime_open(mock.client, &config, &runtime, &error) == CAI_OK);
  QUEUE_REQUIRE(cai_agent_runtime_pending_input(runtime, &queue, &error) ==
                CAI_OK);
  QUEUE_REQUIRE(cai_input_queue_get_state(queue, &pending, &error) == CAI_OK);
  expect_int(state, "pending_input_separate_conversation", (long)pending.count,
             0L);
  cai_agent_runtime_close(runtime);
  runtime = NULL;
  config.session_id = NULL;
  config.resume_session_id = "conversation-A";
  QUEUE_REQUIRE(
      cai_agent_runtime_open(mock.client, &config, &runtime, &error) == CAI_OK);
  QUEUE_REQUIRE(cai_agent_runtime_pending_input(runtime, &queue, &error) ==
                CAI_OK);
  QUEUE_REQUIRE(cai_input_queue_client_open(queue, &attachment, &error) ==
                CAI_OK);
  QUEUE_REQUIRE(cai_input_queue_get_state(queue, &pending, &error) == CAI_OK);
  expect_int(state, "pending_input_restored_count", (long)pending.count, 1L);
  QUEUE_REQUIRE(cai_input_queue_entry_at(attachment, 0U, &item, &error) ==
                CAI_OK);
  expect_int(state, "pending_input_approval_survives_restart", item.held, 1L);
  expect_str(state, "pending_input_restored_text", item.text,
             "accepted queue instruction");
  memset(&request, 0, sizeof(request));
  request.request_id = 1U;
  request.operation = CAI_INPUT_APPROVE;
  request.item_id = item.id;
  request.item_revision = item.revision;
  QUEUE_REQUIRE(
      cai_input_queue_request(attachment, &request, &result, &error) == CAI_OK);
  QUEUE_REQUIRE(result.outcome == CAI_INPUT_ACCEPTED);
  QUEUE_REQUIRE(cai_input_queue_entry_at(attachment, 0U, &item, &error) ==
                CAI_OK);
  QUEUE_REQUIRE(cai_input_queue_set_ready(queue, item.id, 1, &error) == CAI_OK);
  QUEUE_REQUIRE(cai_input_queue_claim(queue, item.id, item.revision, &claim,
                                      &error) == CAI_OK);
  snprintf(input_call, sizeof(input_call), "input:%s:%llu:%llu:%llu",
           "conversation-A", claim.entry.client_id, claim.entry.request_id,
           claim.entry.id);
  cai_input_claim_cleanup(&claim);
  cai_input_queue_client_close(attachment);
  attachment = NULL;
  /* Crash window: durable claim, no model delivery yet. */
  cai_agent_runtime_close(runtime);
  runtime = NULL;
  QUEUE_REQUIRE(
      cai_agent_runtime_open(mock.client, &config, &runtime, &error) == CAI_OK);
  QUEUE_REQUIRE(cai_agent_runtime_pending_input(runtime, &queue, &error) ==
                CAI_OK);
  QUEUE_REQUIRE(cai_input_queue_get_state(queue, &pending, &error) == CAI_OK);
  expect_int(state, "pending_input_claim_not_editable", (long)pending.count,
             0L);
  run_state = CAI_AGENT_IDLE;
  for (i = 0; i < 60 && run_state != CAI_AGENT_COMPLETED &&
              run_state != CAI_AGENT_FAILED;
       ++i) {
    QUEUE_REQUIRE(cai_agent_runtime_pump(runtime, 50L, &error) == CAI_OK);
    QUEUE_REQUIRE(cai_agent_runtime_state(runtime, &run_state, &error) ==
                  CAI_OK);
  }
  expect_int(state, "pending_input_claim_delivered_after_restart", run_state,
             CAI_AGENT_COMPLETED);
  cai_agent_runtime_close(runtime);
  runtime = NULL;
  memset(&identity, 0, sizeof(identity));
  QUEUE_REQUIRE(store.load_events_after(
                    store.context, config.session_scope, "conversation-A", 0U,
                    input_identity_collect, &identity, &error) == CAI_OK);
  expect_int(state, "pending_input_recovered_claim_identity",
             input_identity_count(&identity, "turn.completed", input_call), 1L);
  QUEUE_REQUIRE(input_identity_cut_event(root, config.session_scope,
                                         "input_queue_delivery") == 0);
  QUEUE_REQUIRE(
      cai_agent_runtime_open(mock.client, &config, &runtime, &error) == CAI_OK);
  memset(&identity, 0, sizeof(identity));
  QUEUE_REQUIRE(input_identity_wait(runtime, &store, config.session_scope,
                                    "conversation-A", &identity, 1,
                                    &error) == CAI_OK);
  cai_agent_runtime_close(runtime);
  runtime = NULL;
  expect_int(state, "pending_input_recovered_delivery_identity",
             input_identity_count(&identity, "turn.completed", input_call), 1L);
  config.record_transcript = 0;
  QUEUE_REQUIRE(
      cai_agent_runtime_open(mock.client, &config, &runtime, &error) == CAI_OK);
  QUEUE_REQUIRE(cai_agent_runtime_pump(runtime, 10L, &error) == CAI_OK);
  QUEUE_REQUIRE(cai_agent_runtime_state(runtime, &run_state, &error) == CAI_OK);
  expect_int(state, "pending_input_no_duplicate_delivery", run_state,
             CAI_AGENT_IDLE);
  /* Trim replaces model history atomically while preserving undecided work.
   * Its OFFER/edit records can precede the retained history boundary. */
  QUEUE_REQUIRE(cai_agent_runtime_pending_input(runtime, &queue, &error) ==
                CAI_OK);
  QUEUE_REQUIRE(cai_input_queue_client_open(queue, &attachment, &error) ==
                CAI_OK);
  memset(&request, 0, sizeof(request));
  request.request_id = 1U;
  request.operation = CAI_INPUT_OFFER;
  request.text = "held across atomic history cut";
  request.enabled = 1;
  QUEUE_REQUIRE(
      cai_input_queue_request(attachment, &request, &result, &error) == CAI_OK);
  QUEUE_REQUIRE(result.outcome == CAI_INPUT_ACCEPTED);
  request.request_id++;
  request.text = "draft before cut";
  request.enabled = 0;
  QUEUE_REQUIRE(
      cai_input_queue_request(attachment, &request, &result, &error) == CAI_OK);
  QUEUE_REQUIRE(cai_input_queue_entry_at(attachment, 1U, &item, &error) ==
                CAI_OK);
  request.request_id++;
  request.operation = CAI_INPUT_BEGIN_EDIT;
  request.text = NULL;
  request.item_id = item.id;
  request.item_revision = item.revision;
  QUEUE_REQUIRE(
      cai_input_queue_request(attachment, &request, &result, &error) == CAI_OK);
  QUEUE_REQUIRE(result.outcome == CAI_INPUT_ACCEPTED);
  QUEUE_REQUIRE(cai_input_queue_entry_at(attachment, 1U, &item, &error) ==
                CAI_OK);
  request.request_id++;
  request.operation = CAI_INPUT_END_EDIT;
  request.text = "edited retained input";
  request.item_revision = item.revision;
  request.edit_token = item.edit_token;
  QUEUE_REQUIRE(
      cai_input_queue_request(attachment, &request, &result, &error) == CAI_OK);
  QUEUE_REQUIRE(result.outcome == CAI_INPUT_ACCEPTED);
  QUEUE_REQUIRE(cai_input_queue_entry_at(attachment, 1U, &item, &error) ==
                CAI_OK);
  request.request_id++;
  request.operation = CAI_INPUT_BEGIN_EDIT;
  request.text = NULL;
  request.item_revision = item.revision;
  request.edit_token = 0U;
  QUEUE_REQUIRE(
      cai_input_queue_request(attachment, &request, &result, &error) == CAI_OK);
  QUEUE_REQUIRE(result.outcome == CAI_INPUT_ACCEPTED);
  QUEUE_REQUIRE(cai_agent_runtime_trim_history(runtime, 1U, &error) == CAI_OK);
  QUEUE_REQUIRE(store.load_events_after(store.context, config.session_scope,
                                        "conversation-A", 0U,
                                        test_queue_retained_event,
                                        &trimmed_payload, &error) == CAI_OK);
  QUEUE_REQUIRE(!trimmed_payload);
  captured = calloc(1U, sizeof(*captured));
  QUEUE_REQUIRE(captured != NULL);
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.write = test_mcp_sink_write;
  callbacks.context = captured;
  QUEUE_REQUIRE(cai_sink_from_callbacks(&callbacks, &sink, &error) == CAI_OK);
  QUEUE_REQUIRE(cai_agent_runtime_export_journal_state(runtime, sink, &error) ==
                CAI_OK);
  QUEUE_REQUIRE(strstr(captured->buffer, "accepted queue instruction") == NULL);
  QUEUE_REQUIRE(strstr(captured->buffer, "held across atomic history cut") !=
                NULL);
  QUEUE_REQUIRE(strstr(captured->buffer, "edited retained input") != NULL);
  cai_sink_close(sink);
  sink = NULL;
  cai_input_queue_client_close(attachment);
  attachment = NULL;
  cai_agent_runtime_close(runtime);
  runtime = NULL;
  QUEUE_REQUIRE(
      cai_agent_runtime_open(mock.client, &config, &runtime, &error) == CAI_OK);
  QUEUE_REQUIRE(cai_agent_runtime_pending_input(runtime, &queue, &error) ==
                CAI_OK);
  QUEUE_REQUIRE(cai_input_queue_client_open(queue, &attachment, &error) ==
                CAI_OK);
  QUEUE_REQUIRE(cai_input_queue_get_state(queue, &pending, &error) == CAI_OK);
  expect_int(state, "pending_input_cut_preserves_both_entries",
             (long)pending.count, 2L);
  QUEUE_REQUIRE(cai_input_queue_entry_at(attachment, 0U, &item, &error) ==
                CAI_OK);
  expect_str(state, "pending_input_cut_held_text", item.text,
             "held across atomic history cut");
  expect_int(state, "pending_input_cut_approval", item.held, 1L);
  QUEUE_REQUIRE(cai_input_queue_entry_at(attachment, 1U, &item, &error) ==
                CAI_OK);
  expect_str(state, "pending_input_cut_edited_text", item.text,
             "edited retained input");
  expect_int(state, "pending_input_cut_edit_hold", item.edit_held, 1L);
  expect_int(state, "pending_input_cut_hides_foreign_token",
             item.edit_token != 0U, 0L);
  memset(&request, 0, sizeof(request));
  request.request_id = 1U;
  request.operation = CAI_INPUT_OFFER;
  request.text = "new input after cut";
  request.enabled = 1;
  QUEUE_REQUIRE(
      cai_input_queue_request(attachment, &request, &result, &error) == CAI_OK);
  QUEUE_REQUIRE(result.outcome == CAI_INPUT_ACCEPTED);
  QUEUE_REQUIRE(cai_agent_runtime_trim_history(runtime, 1U, &error) ==
                CAI_ERR_INVALID);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  cai_input_queue_client_close(attachment);
  attachment = NULL;
  cai_agent_runtime_close(runtime);
  runtime = NULL;
  QUEUE_REQUIRE(
      cai_agent_runtime_open(mock.client, &config, &runtime, &error) == CAI_OK);
  QUEUE_REQUIRE(cai_agent_runtime_pending_input(runtime, &queue, &error) ==
                CAI_OK);
  QUEUE_REQUIRE(cai_input_queue_get_state(queue, &pending, &error) == CAI_OK);
  expect_int(state, "pending_input_failed_cut_keeps_followup",
             (long)pending.count, 3L);
  /* A late claim pins an isolated child until delivery or explicit close.
   * Closing that child before pumping must neither deadlock nor lose input. */
  cai_agent_review_request_init(&review);
  review.target = CAI_AGENT_REVIEW_CUSTOM;
  review.instructions = "Review this queue fixture";
  QUEUE_REQUIRE(cai_agent_runtime_start_review(runtime, &review, &child,
                                               &error) == CAI_OK);
  run_state = CAI_AGENT_SAMPLING;
  for (i = 0; i < 60 && run_state != CAI_AGENT_COMPLETED &&
              run_state != CAI_AGENT_FAILED;
       ++i) {
    QUEUE_REQUIRE(cai_agent_runtime_pump(child, 50L, &error) == CAI_OK);
    QUEUE_REQUIRE(cai_agent_runtime_state(child, &run_state, &error) == CAI_OK);
  }
  QUEUE_REQUIRE(run_state == CAI_AGENT_COMPLETED);
  QUEUE_REQUIRE(cai_input_queue_client_open(queue, &attachment, &error) ==
                CAI_OK);
  memset(&request, 0, sizeof(request));
  request.operation = CAI_INPUT_OFFER;
  request.request_id = 1U;
  request.intent = CAI_INPUT_STEERING;
  request.text = "instruction retained after child exit";
  QUEUE_REQUIRE(
      cai_input_queue_request(attachment, &request, &result, &error) == CAI_OK);
  QUEUE_REQUIRE(result.outcome == CAI_INPUT_ACCEPTED);
  QUEUE_REQUIRE(cai_input_queue_lookup(queue, result.item_id, &item, &error) ==
                CAI_OK);
  QUEUE_REQUIRE(cai_input_queue_set_ready(queue, item.id, 1, &error) == CAI_OK);
  QUEUE_REQUIRE(cai_input_queue_claim(queue, item.id, item.revision, &claim,
                                      &error) == CAI_OK);
  cai_input_claim_cleanup(&claim);
  cai_agent_runtime_close(child);
  child = NULL;
  cai_input_queue_client_close(attachment);
  attachment = NULL;
  cai_agent_runtime_close(runtime);
  runtime = NULL;
  QUEUE_REQUIRE(
      cai_agent_runtime_open(mock.client, &config, &runtime, &error) == CAI_OK);
  QUEUE_REQUIRE(cai_agent_runtime_pump(runtime, 0L, &error) == CAI_OK);
  QUEUE_REQUIRE(cai_agent_runtime_start_review(runtime, &review, &child,
                                               &error) == CAI_OK);
  run_state = CAI_AGENT_SAMPLING;
  for (i = 0; i < 60 && run_state != CAI_AGENT_COMPLETED &&
              run_state != CAI_AGENT_FAILED;
       ++i) {
    QUEUE_REQUIRE(cai_agent_runtime_pump(child, 50L, &error) == CAI_OK);
    QUEUE_REQUIRE(cai_agent_runtime_state(child, &run_state, &error) == CAI_OK);
  }
  QUEUE_REQUIRE(run_state == CAI_AGENT_COMPLETED);
  QUEUE_REQUIRE(cai_agent_runtime_finish_review(runtime, child, &error) ==
                CAI_OK);
  cai_agent_runtime_close(child);
  child = NULL;
  run_state = CAI_AGENT_IDLE;
  for (i = 0; i < 60 && run_state != CAI_AGENT_COMPLETED &&
              run_state != CAI_AGENT_FAILED;
       ++i) {
    QUEUE_REQUIRE(cai_agent_runtime_pump(runtime, 50L, &error) == CAI_OK);
    QUEUE_REQUIRE(cai_agent_runtime_state(runtime, &run_state, &error) ==
                  CAI_OK);
  }
  QUEUE_REQUIRE(run_state == CAI_AGENT_COMPLETED);
cleanup:
  cai_sink_close(sink);
  free(captured);
  cai_input_queue_client_close(attachment);
  cai_agent_runtime_close(child);
  cai_agent_runtime_close(runtime);
  cai_agent_local_session_store_close(&store);
  http_mock_client_close(state, "pending_input", &mock);
  cai_error_cleanup(&error);
#undef QUEUE_REQUIRE
}
