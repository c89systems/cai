/** Exercise durable work correlation through public queue/store boundaries.
 * The trace holds only bounded lifecycle metadata, never a response body. */
typedef struct input_identity_trace {
  unsigned long long through;
  size_t count;
  int terminals;
  size_t settled_count;
  struct {
    char call[256], goal[32];
    long long code;
  } settled[8];
  struct {
    char kind[32], call[256];
    long long code;
  } entries[32];
} input_identity_trace;

static int input_identity_collect(void *context,
                                  const cai_agent_session_event *event,
                                  cai_error *error) {
  input_identity_trace *trace = context;
  cai_execution_record record;
  int rc;
  trace->through = event->sequence;
  if (strcmp(event->type, "execution") != 0)
    return CAI_OK;
  cai_execution_record_init(&record);
  rc = cai_execution_record_parse(event->data, &record, error);
  if (rc == CAI_OK && strcmp(record.kind, "turn.settled") == 0) {
    size_t n = trace->settled_count;
    if (n >= 8U || record.field == NULL ||
        strcmp(record.field, "goal_status") != 0 || !record.final ||
        record.offset != 0U || strlen(record.data) >= 32U ||
        trace->count == 0U ||
        strcmp(trace->entries[trace->count - 1U].kind, "turn.started") == 0)
      rc = cai_set_error(error, CAI_ERR_PROTOCOL, "invalid settled boundary");
    else {
      snprintf(trace->settled[n].call, sizeof(trace->settled[n].call), "%s",
               record.call_id != NULL ? record.call_id : "");
      snprintf(trace->settled[n].goal, sizeof(trace->settled[n].goal), "%s",
               record.data);
      trace->settled[n].code = record.code;
      ++trace->settled_count;
    }
  }
  if (rc == CAI_OK && record.field == NULL &&
      (strncmp(record.kind, "turn.", 5U) == 0 ||
       strcmp(record.kind, "response.completed") == 0)) {
    size_t at = trace->count;
    if (at == sizeof(trace->entries) / sizeof(trace->entries[0]))
      rc = cai_set_error(error, CAI_ERR_LIMIT, "identity trace overflow");
    else {
      snprintf(trace->entries[at].kind, sizeof(trace->entries[at].kind), "%s",
               record.kind);
      snprintf(trace->entries[at].call, sizeof(trace->entries[at].call), "%s",
               record.call_id != NULL ? record.call_id : "");
      trace->entries[at].code = record.code;
      ++trace->count;
      if (strcmp(record.kind, "turn.completed") == 0 ||
          strcmp(record.kind, "turn.cancelled") == 0 ||
          strcmp(record.kind, "turn.failed") == 0)
        ++trace->terminals;
    }
  }
  cai_execution_record_cleanup(&record);
  return rc;
}

static int input_identity_count(const input_identity_trace *trace,
                                const char *kind, const char *call) {
  size_t i;
  int count = 0;
  for (i = 0U; i < trace->count; ++i)
    if (strcmp(trace->entries[i].kind, kind) == 0 &&
        strcmp(trace->entries[i].call, call) == 0)
      ++count;
  return count;
}

/** Simulate process loss immediately after an acknowledged local delivery.
 * Only the fixture's closed local journal is shortened; no live store or real
 * user history is modified. Later commits/results deliberately do not survive.
 */
static int input_identity_cut_delivery(const char *root, const char *scope) {
  static const char hex[] = "0123456789abcdef";
  unsigned char digest[SHA256_DIGEST_LENGTH];
  char hash[65], path[PATH_MAX];
  char *line = NULL;
  size_t i, capacity = 0U;
  FILE *file;
  int rc = -1;
  if (SHA256((const unsigned char *)scope, strlen(scope), digest) == NULL)
    return -1;
  for (i = 0U; i < sizeof(digest); ++i) {
    hash[2U * i] = hex[digest[i] >> 4U];
    hash[2U * i + 1U] = hex[digest[i] & 15U];
  }
  hash[64] = '\0';
  snprintf(path, sizeof(path), "%s/%s/conversation-A.jsonl", root, hash);
  file = fopen(path, "r+");
  if (file == NULL)
    return -1;
  while (getline(&line, &capacity, file) >= 0)
    if (strstr(line, "\"type\":\"input_queue_delivery\"") != NULL) {
      off_t end = ftello(file);
      if (end >= 0)
        rc = ftruncate(fileno(file), end);
      break;
    }
  free(line);
  fclose(file);
  return rc;
}

/** Polls for exact durable terminal records, not a transient idle state. */
static int input_identity_wait(cai_agent_runtime *runtime,
                               cai_agent_session_store *store,
                               const char *scope, const char *session,
                               input_identity_trace *trace, int terminals,
                               cai_error *error) {
  int i, rc = CAI_OK;
  for (i = 0; rc == CAI_OK &&
              (trace->terminals < terminals ||
               trace->settled_count < (size_t)terminals) &&
              i < 80;
       ++i) {
    rc = cai_agent_runtime_pump(runtime, 50L, error);
    if (rc == CAI_OK)
      rc = store->load_events_after(store->context, scope, session,
                                    trace->through, input_identity_collect,
                                    trace, error);
  }
  if (rc == CAI_OK && (trace->terminals != terminals ||
                       trace->settled_count != (size_t)terminals))
    rc = cai_set_error(error, CAI_ERR_LIMIT, "queued outcome did not arrive");
  /* Dispatch terminal observations before another direct submission. */
  if (rc == CAI_OK)
    rc = cai_agent_runtime_pump(runtime, 0L, error);
  if (rc == CAI_OK) {
    int quiet = 0;
    rc = cai_agent_runtime_is_quiescent(runtime, &quiet, error);
    if (rc == CAI_OK && !quiet)
      rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                         "settled runtime is not quiescent");
  }
  return rc;
}

typedef struct input_identity_observer {
  cai_agent_runtime *runtime;
  int cancel, cancelled, steer, steered;
  char steer_call[256];
} input_identity_observer;
static int input_identity_event(void *context,
                                const cai_agent_runtime_event *event,
                                cai_error *error) {
  input_identity_observer *s = context;
  if (event->type == CAI_AGENT_EVENT_INPUT_BOUNDARY && s->cancel &&
      !s->cancelled) {
    s->cancelled = 1;
    return cai_agent_runtime_cancel_turn(s->runtime, error);
  }
  if (event->type == CAI_AGENT_EVENT_INPUT_BOUNDARY && s->steer &&
      !s->steered) {
    cai_input_queue *queue;
    cai_input_queue_client *client = NULL;
    cai_input_request request;
    cai_input_result result;
    cai_input_entry item;
    cai_input_claim claim;
    int rc = cai_agent_runtime_pending_input(s->runtime, &queue, error);
    if (rc == CAI_OK)
      rc = cai_input_queue_client_open(queue, &client, error);
    memset(&request, 0, sizeof(request));
    request.request_id = 1U;
    request.text = "joining steering input";
    request.intent = CAI_INPUT_STEERING;
    if (rc == CAI_OK)
      rc = cai_input_queue_request(client, &request, &result, error);
    if (rc == CAI_OK && result.outcome != CAI_INPUT_ACCEPTED)
      rc = cai_set_error(error, CAI_ERR_INVALID, "fixture steer rejected");
    /* Another queued turn remains visible: locate this producer's own item. */
    if (rc == CAI_OK) {
      cai_input_state pending;
      size_t at;
      rc = cai_input_queue_get_state(queue, &pending, error);
      for (at = 0U; rc == CAI_OK && at < pending.count; ++at) {
        rc = cai_input_queue_entry_at(client, at, &item, error);
        if (rc == CAI_OK && item.id == result.item_id)
          break;
      }
      if (rc == CAI_OK && at == pending.count)
        rc = cai_set_error(error, CAI_ERR_INVALID, "fixture steer missing");
    }
    if (rc == CAI_OK) {
      snprintf(s->steer_call, sizeof(s->steer_call), "input:%s:%llu:%llu:%llu",
               cai_agent_runtime_session_id(s->runtime), item.client_id,
               item.request_id, item.id);
      rc = cai_input_queue_set_ready(queue, item.id, 1, error);
    }
    if (rc == CAI_OK) {
      rc = cai_input_queue_claim(queue, item.id, item.revision, &claim, error);
      if (rc == CAI_OK)
        cai_input_claim_cleanup(&claim);
    }
    cai_input_queue_client_close(client);
    s->steered = rc == CAI_OK;
    return rc;
  }
  return CAI_OK;
}

static void input_identity_cases(test_state *state) {
#define IDENTITY_REQUIRE(condition)                                            \
  do {                                                                         \
    int valid = (condition);                                                   \
    expect_int(state, #condition, valid, 1L);                                  \
    if (!valid) {                                                              \
      fprintf(stderr, "input identity line %d: %d %s (%s)\n", __LINE__,        \
              error.code, error.message != NULL ? error.message : "",          \
              error.detail != NULL ? error.detail : "");                       \
      goto cleanup;                                                            \
    }                                                                          \
  } while (0)
  static const char response[] =
      "data: {\"type\":\"response.output_text.delta\",\"delta\":\"done\"}\n\n"
      "data: "
      "{\"type\":\"response.completed\",\"response\":{\"id\":\"resp_id\"}}\n\n";
  static const char *steering_required[] = {"joining steering input"};
  mock_http_expectation script[5];
  int variant;
  for (variant = 0; variant < 4; ++variant) {
    char root[] = CAI_TEST_BINARY_DIR "/test-tmp/input-identity-XXXXXX";
    cai_agent_local_session_store_config storage;
    cai_agent_session_store store;
    cai_agent_runtime_config config;
    cai_agent_runtime *runtime = NULL;
    cai_input_queue *queue;
    cai_input_queue_client *clients[2] = {NULL, NULL};
    cai_input_request request;
    cai_input_result result;
    cai_input_entry item;
    cai_input_claim claim;
    input_identity_trace *trace = calloc(1, sizeof(*trace));
    input_identity_observer observer;
    http_mock_client mock;
    cai_error error;
    char calls[2][256];
    size_t j;
    int success = variant < 2, requests = success ? (variant == 1 ? 5 : 4) : 1;
    memset(script, 0, sizeof(script));
    for (j = 0U; j < (size_t)requests; ++j) {
      script[j].request_prefix = "POST /v1/responses HTTP/";
      script[j].status = variant == 2 ? 400 : 200;
      script[j].status_text = variant == 2 ? "Bad Request" : "OK";
      script[j].content_type =
          variant == 2 ? "application/json" : "text/event-stream";
      script[j].body = variant == 2
                           ? "{\"error\":{\"message\":\"identity "
                             "failure\",\"type\":\"invalid_request_error\"}}"
                           : response;
    }
    if (variant == 1) {
      script[2].required = steering_required;
      script[2].required_count = 1U;
    }
    cai_error_init(&error);
    memset(&store, 0, sizeof(store));
    memset(&observer, 0, sizeof(observer));
    if (http_mock_client_open_script(state, "input_identity", script,
                                     (size_t)requests, &mock) != 0) {
      free(trace);
      return;
    }
    IDENTITY_REQUIRE(trace != NULL);
    IDENTITY_REQUIRE(mkdtemp(root) != NULL);
    cai_agent_local_session_store_config_init(&storage);
    storage.root_directory = root;
    IDENTITY_REQUIRE(
        cai_agent_local_session_store_open(&storage, &store, &error) == CAI_OK);
    cai_agent_runtime_config_init(&config);
    config.workspace_directory = "/tmp";
    config.session_scope = "input-identity";
    config.session_id = "identity-session";
    config.session_store = &store;
    config.record_transcript = 1;
    config.disable_web_search = config.disable_chrome =
        config.disable_terminal = 1;
    if (variant != 0) {
      config.event_callback = input_identity_event;
      config.event_context = &observer;
    }
    if (variant == 3) {
      observer.cancel = 1;
    }
    config.synchronize_input_boundaries = variant == 1 || variant == 3;
    IDENTITY_REQUIRE(cai_agent_runtime_open(mock.client, &config, &runtime,
                                            &error) == CAI_OK);
    observer.runtime = runtime;
    if (success) {
      int quiet = 1;
      IDENTITY_REQUIRE(cai_agent_runtime_submit(runtime, "same text", &error) ==
                       CAI_OK);
      IDENTITY_REQUIRE(
          cai_agent_runtime_is_quiescent(runtime, &quiet, &error) == CAI_OK &&
          !quiet);
      IDENTITY_REQUIRE(
          input_identity_wait(runtime, &store, config.session_scope,
                              config.session_id, trace, 1, &error) == CAI_OK);
      observer.steer = variant == 1;
    }
    IDENTITY_REQUIRE(cai_agent_runtime_pending_input(runtime, &queue, &error) ==
                     CAI_OK);
    for (j = 0U; j < (success ? 2U : 1U); ++j) {
      IDENTITY_REQUIRE(
          cai_input_queue_client_open(queue, &clients[j], &error) == CAI_OK);
      memset(&request, 0, sizeof(request));
      request.operation = CAI_INPUT_OFFER;
      request.request_id = 1U;
      request.text = "same text";
      IDENTITY_REQUIRE(cai_input_queue_request(clients[j], &request, &result,
                                               &error) == CAI_OK);
      IDENTITY_REQUIRE(result.outcome == CAI_INPUT_ACCEPTED);
      IDENTITY_REQUIRE(
          cai_input_queue_entry_at(clients[j], 0U, &item, &error) == CAI_OK);
      /* Editing replaces text/revision, not the originating OFFER identity. */
      if (success && j == 1U) {
        unsigned long long original_request = item.request_id;
        request.request_id = 2U;
        request.operation = CAI_INPUT_BEGIN_EDIT;
        request.text = NULL;
        request.item_id = item.id;
        request.item_revision = item.revision;
        IDENTITY_REQUIRE(cai_input_queue_request(clients[j], &request, &result,
                                                 &error) == CAI_OK);
        IDENTITY_REQUIRE(result.outcome == CAI_INPUT_ACCEPTED);
        IDENTITY_REQUIRE(
            cai_input_queue_entry_at(clients[j], 0U, &item, &error) == CAI_OK);
        request.request_id = 3U;
        request.operation = CAI_INPUT_END_EDIT;
        request.item_revision = item.revision;
        request.edit_token = item.edit_token;
        request.text = "same text";
        IDENTITY_REQUIRE(cai_input_queue_request(clients[j], &request, &result,
                                                 &error) == CAI_OK);
        IDENTITY_REQUIRE(result.outcome == CAI_INPUT_ACCEPTED);
        IDENTITY_REQUIRE(
            cai_input_queue_entry_at(clients[j], 0U, &item, &error) == CAI_OK);
        IDENTITY_REQUIRE(item.request_id == original_request);
      }
      snprintf(calls[j], sizeof(calls[j]), "input:%s:%llu:%llu:%llu",
               config.session_id, item.client_id, item.request_id, item.id);
      IDENTITY_REQUIRE(cai_input_queue_set_ready(queue, item.id, 1, &error) ==
                       CAI_OK);
      IDENTITY_REQUIRE(cai_input_queue_claim(queue, item.id, item.revision,
                                             &claim, &error) == CAI_OK);
      cai_input_claim_cleanup(&claim);
    }
    IDENTITY_REQUIRE(input_identity_wait(runtime, &store, config.session_scope,
                                         config.session_id, trace,
                                         success ? 3 : 1, &error) == CAI_OK);
    for (j = 0U; j < (success ? 2U : 1U); ++j) {
      expect_int(state, "queued_input_has_own_start",
                 input_identity_count(trace, "turn.started", calls[j]), 1L);
      expect_int(state, "queued_input_has_own_outcome",
                 input_identity_count(trace,
                                      success        ? "turn.completed"
                                      : variant == 2 ? "turn.failed"
                                                     : "turn.cancelled",
                                      calls[j]),
                 1L);
      expect_int(state, "queued_response_owns_identity",
                 input_identity_count(trace, "response.completed", calls[j]),
                 variant == 2              ? 0L
                 : variant == 1 && j == 0U ? 2L
                                           : 1L);
      {
        size_t n = success ? j + 1U : j;
        expect_int(state, "queued_settlement_keeps_identity",
                   n < trace->settled_count &&
                       strcmp(trace->settled[n].call, calls[j]) == 0,
                   1L);
        expect_str(state, "queued_settlement_goal_status",
                   trace->settled[n].goal, "none");
        expect_int(state, "queued_settlement_code", trace->settled[n].code,
                   success        ? CAI_OK
                   : variant == 3 ? CAI_ERR_CANCELLED
                                  : CAI_ERR_SERVER);
      }
    }
    if (success) {
      IDENTITY_REQUIRE(strcmp(calls[0], calls[1]) != 0);
      IDENTITY_REQUIRE(cai_agent_runtime_submit(runtime, "same text", &error) ==
                       CAI_OK);
      IDENTITY_REQUIRE(
          input_identity_wait(runtime, &store, config.session_scope,
                              config.session_id, trace, 4, &error) == CAI_OK);
      expect_int(state, "direct_turns_do_not_inherit_identity",
                 input_identity_count(trace, "turn.completed", ""), 2L);
      expect_int(state, "direct_starts_do_not_inherit_identity",
                 input_identity_count(trace, "turn.started", ""), 2L);
      if (variant == 1) {
        IDENTITY_REQUIRE(observer.steered);
        expect_int(
            state, "joining_steer_cannot_own_turn",
            input_identity_count(trace, "turn.completed", observer.steer_call),
            0L);
      }
    }
  cleanup:
    cai_input_queue_client_close(clients[0]);
    cai_input_queue_client_close(clients[1]);
    cai_agent_runtime_close(runtime);
    cai_agent_local_session_store_close(&store);
    http_mock_client_close(state, "input_identity", &mock);
    cai_error_cleanup(&error);
    free(trace);
  }
#undef IDENTITY_REQUIRE
}

typedef struct input_identity_preflight_state {
  runtime_session_store_state base;
  input_identity_trace trace;
} input_identity_preflight_state;
static int input_identity_preflight_append(void *context, const char *scope,
                                           const char *session,
                                           const cai_agent_session_event *event,
                                           cai_error *error) {
  input_identity_preflight_state *s = context;
  (void)scope;
  (void)session;
  return input_identity_collect(&s->trace, event, error);
}
/** Exhausted goals reject queued work before inference. That rejection still
 * owes its producer a durable outcome, also without an event receiver. */
static void input_identity_preflight(test_state *state) {
  input_identity_preflight_state *s = calloc(1U, sizeof(*s));
  cai_agent_session_store store;
  cai_agent_runtime_config config;
  cai_client_config cc;
  cai_client *client = NULL;
  cai_agent_runtime *runtime = NULL;
  cai_input_queue *queue;
  cai_input_queue_client *attachment = NULL;
  cai_input_request request;
  cai_input_result result;
  cai_input_entry item;
  cai_input_claim claim;
  cai_agent_run_state status = CAI_AGENT_IDLE;
  cai_error error;
  char call[256];
  int i, rc;
  cai_error_init(&error);
  expect_int(state, "input_preflight_fixture", s != NULL, 1L);
  if (s == NULL)
    goto cleanup;
  s->base.commit_json =
      "{\"version\":1,\"model\":\"gpt-5-nano\",\"goal_objective\":\"done\","
      "\"goal_status\":\"budget_limited\",\"goal_token_budget\":1,"
      "\"goal_tokens_used\":1,\"history\":[]}";
  memset(&store, 0, sizeof(store));
  store.context = s;
  store.load_latest = test_runtime_session_store_load;
  store.load_events_after = test_runtime_session_store_load_events_after;
  store.commit = execution_test_commit;
  store.append_event = input_identity_preflight_append;
  cai_client_config_init(&cc);
  cc.api_key = "fixture";
  cc.base_url = "http://127.0.0.1:1/v1";
  cc.timeout_ms = 100L;
  cc.http_2_disabled = cc.logger_disabled = 1;
  rc = cai_client_open(&cc, &client, &error);
  expect_int(state, "input_preflight_client", rc, CAI_OK);
  if (rc != CAI_OK)
    goto cleanup;
  cai_agent_runtime_config_init(&config);
  config.workspace_directory = "/tmp";
  config.session_store = &store;
  config.resume_latest = config.record_transcript = 1;
  config.disable_terminal = config.disable_chrome = config.disable_web_search =
      1;
  rc = cai_agent_runtime_open(client, &config, &runtime, &error);
  expect_int(state, "input_preflight_open", rc, CAI_OK);
  if (rc != CAI_OK)
    goto cleanup;
  if (cai_agent_runtime_pending_input(runtime, &queue, &error) != CAI_OK ||
      cai_input_queue_client_open(queue, &attachment, &error) != CAI_OK)
    goto cleanup;
  memset(&request, 0, sizeof(request));
  request.request_id = 1U;
  request.text = "new queued work";
  rc = cai_input_queue_request(attachment, &request, &result, &error);
  expect_int(state, "input_preflight_offer", rc, CAI_OK);
  if (rc != CAI_OK)
    goto cleanup;
  expect_int(state, "input_preflight_accept", result.outcome,
             CAI_INPUT_ACCEPTED);
  if (result.outcome != CAI_INPUT_ACCEPTED ||
      cai_input_queue_entry_at(attachment, 0U, &item, &error) != CAI_OK)
    goto cleanup;
  snprintf(call, sizeof(call), "input:%s:%llu:%llu:%llu",
           cai_agent_runtime_session_id(runtime), item.client_id,
           item.request_id, item.id);
  if (cai_input_queue_set_ready(queue, item.id, 1, &error) != CAI_OK ||
      cai_input_queue_claim(queue, item.id, item.revision, &claim, &error) !=
          CAI_OK)
    goto cleanup;
  cai_input_claim_cleanup(&claim);
  for (i = 0; i < 80 && status != CAI_AGENT_FAILED; ++i) {
    if (cai_agent_runtime_pump(runtime, 50L, &error) != CAI_OK ||
        cai_agent_runtime_state(runtime, &status, &error) != CAI_OK)
      break;
  }
  cai_input_queue_client_close(attachment);
  attachment = NULL;
  cai_agent_runtime_close(runtime);
  runtime = NULL;
  /* The worker has joined before its captured records are inspected. */
  expect_int(state, "input_preflight_tagged_failure",
             input_identity_count(&s->trace, "turn.failed", call), 1L);
  expect_int(state, "input_preflight_no_false_start",
             input_identity_count(&s->trace, "turn.started", call), 0L);
  expect_int(state, "input_preflight_no_request",
             input_identity_count(&s->trace, "response.completed", call), 0L);
  expect_int(state, "input_preflight_settled", (long)s->trace.settled_count,
             1L);
  expect_str(state, "input_preflight_settled_identity",
             s->trace.settled[0].call, call);
  expect_int(state, "input_preflight_settled_code", s->trace.settled[0].code,
             CAI_ERR_LIMIT);
  expect_int(state, "input_preflight_budget_code",
             s->trace.count == 1U ? s->trace.entries[0].code : -1L,
             CAI_ERR_LIMIT);
cleanup:
  expect_int(state, "input_preflight_no_fixture_error", error.code, CAI_OK);
  cai_input_queue_client_close(attachment);
  cai_agent_runtime_close(runtime);
  cai_client_close(client);
  cai_error_cleanup(&error);
  free(s);
}

static void *input_identity_worker(void *context) {
  input_identity_cases(context);
  input_identity_preflight(context);
  return NULL;
}
static void test_agent_runtime_input_identity(test_state *state) {
  pthread_t worker;
  int rc = pthread_create(&worker, NULL, input_identity_worker, state);
  expect_int(state, "input_identity_default_stack_thread", rc, 0L);
  if (rc == 0)
    expect_int(state, "input_identity_join", pthread_join(worker, NULL), 0L);
}
