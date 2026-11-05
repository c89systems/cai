/** Real local journal and HTTP path, run on the platform's default pthread
 * stack. Large fixtures live on the heap, including their lifecycle trace. */
typedef struct goal_input_fixture {
  cai_agent_runtime_config config;
  cai_agent_local_session_store_config storage;
  cai_agent_session_store store;
  cai_agent_runtime *runtime;
  http_mock_client mock;
  input_identity_trace trace;
  input_identity_observer observer;
  cai_agent_goal_snapshot goal;
  cai_error error;
  char *large;
  int queued;
} goal_input_fixture;
/** Run inside the first turn's real owner boundary. Both offers see the
 * worker paused, making capacity and noninterference assertions deterministic.
 */
static int goal_input_queue_event(void *context,
                                  const cai_agent_runtime_event *event,
                                  cai_error *error) {
  goal_input_fixture *f = context;
  cai_agent_goal_snapshot goal;
  cai_error rejected;
  int rc;
  if (event->type != CAI_AGENT_EVENT_INPUT_BOUNDARY || f->queued)
    return CAI_OK;
  f->queued = 1;
  rc = cai_agent_runtime_start_goal(f->runtime, "Fresh objective",
                                    "Initial user task",
                                    "command:conversation-A:7:8:9", error);
  if (rc != CAI_OK)
    return rc;
  cai_error_init(&rejected);
  rc = cai_agent_runtime_start_goal(f->runtime, "MUST NOT REPLACE",
                                    "MUST NOT RUN", "overflow", &rejected);
  cai_error_cleanup(&rejected);
  if (rc != CAI_ERR_LIMIT ||
      cai_agent_runtime_get_goal(f->runtime, &goal, error) != CAI_OK ||
      goal.has_goal)
    return cai_set_error(
        error, CAI_ERR_PROTOCOL,
        "queued goal changed the active turn or exceeded capacity");
  return CAI_OK;
}
static int goal_input_wait(goal_input_fixture *f, int terminal) {
  int i, rc = CAI_OK;
  for (i = 0; rc == CAI_OK &&
              (f->trace.terminals < terminal ||
               f->trace.settled_count < (size_t)terminal) &&
              i < 100;
       ++i) {
    rc = cai_agent_runtime_pump(f->runtime, 50L, &f->error);
    if (rc == CAI_OK)
      rc = f->store.load_events_after(
          f->store.context, f->config.session_scope, f->config.session_id,
          f->trace.through, input_identity_collect, &f->trace, &f->error);
  }
  if (rc == CAI_OK && f->trace.terminals != terminal)
    rc = cai_set_error(&f->error, CAI_ERR_LIMIT, "goal input did not settle");
  if (rc == CAI_OK)
    rc = cai_agent_runtime_pump(f->runtime, 0L, &f->error);
  return rc;
}
/** Corrupt length prefixes must fail restoration before a worker can infer
 * or read outside the bounded accepted action. No network peer is needed. */
static void goal_input_invalid(test_state *state) {
  static const char *invalid[] = {"",
                                  "0\n0\nab",
                                  "-1\n0\nab",
                                  "18446744073709551616\n0\nab",
                                  "66049\n0\nab",
                                  "1\n256\nab",
                                  "1\n0\na",
                                  "1\n1\nab",
                                  "1x\n0\nabc",
                                  "1\n0x\nabc"};
  runtime_session_store_state *saved = calloc(1U, sizeof(*saved));
  cai_agent_session_store store;
  cai_agent_runtime_config config;
  cai_client_config client_config;
  cai_client *client = NULL;
  cai_error error;
  size_t i;
  int rc;
  cai_error_init(&error);
  if (saved == NULL) {
    test_fail(state, "goal_input_invalid_fixture", "allocation failed");
    return;
  }
  memset(&store, 0, sizeof(store));
  store.context = saved;
  store.load_latest = test_runtime_session_store_load;
  store.load_events_after = test_runtime_session_store_load_events_after;
  store.append_event = test_runtime_session_store_append_event;
  store.commit = execution_test_commit;
  cai_client_config_init(&client_config);
  client_config.api_key = "fixture";
  client_config.base_url = "http://127.0.0.1:1/v1";
  client_config.logger_disabled = client_config.http_2_disabled = 1;
  rc = cai_client_open(&client_config, &client, &error);
  expect_int(state, "goal_input_invalid_client", rc, CAI_OK);
  if (rc == CAI_OK) {
    cai_agent_runtime_config_init(&config);
    config.workspace_directory = "/tmp";
    config.session_store = &store;
    config.resume_latest = 1;
    config.disable_terminal = config.disable_chrome =
        config.disable_web_search = 1;
    for (i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
      cai_agent_runtime *runtime = NULL;
      memset(saved, 0, sizeof(*saved));
      saved->commit_json =
          "{\"version\":1,\"model\":\"gpt-5-nano\",\"history\":[]}";
      saved->replay_event_count = 2U;
      saved->replay_event_sequences[0] = 1U;
      saved->replay_event_sequences[1] = 2U;
      strcpy(saved->replay_event_types[0], "input_journal_v2");
      strcpy(saved->replay_event_types[1], "goal_start");
      strcpy(saved->replay_event_data_items[1], invalid[i]);
      rc = cai_agent_runtime_open(client, &config, &runtime, &error);
      expect_int(state, "goal_input_invalid_rejected", rc != CAI_OK, 1L);
      expect_int(state, "goal_input_invalid_no_runtime", runtime == NULL, 1L);
      cai_agent_runtime_close(runtime);
      cai_error_cleanup(&error);
      cai_error_init(&error);
    }
  }
  cai_client_close(client);
  cai_error_cleanup(&error);
  free(saved);
}
/** A compaction checkpoint may persist the replaced goal before its prompt
 * is consumed. Restore that exact accepted input with an application marker
 * produced by the real runtime, retaining later host changes and accounting.
 * Moving the same saved state to another scope/session must apply it anew. */
typedef struct goal_input_recovery {
  runtime_session_store_state saved;
  unsigned int changes;
} goal_input_recovery;
static int goal_input_recovery_event(void *context,
                                     const cai_agent_runtime_event *event,
                                     cai_error *error) {
  goal_input_recovery *s = context;
  (void)error;
  if (event->type == CAI_AGENT_EVENT_GOAL_CHANGED)
    ++s->changes;
  return CAI_OK;
}
static int goal_input_recovery_wait(cai_agent_runtime *runtime,
                                    cai_error *error) {
  int i, quiet = 0, rc = CAI_OK;
  for (i = 0; i < 100 && !quiet && rc == CAI_OK; ++i) {
    rc = cai_agent_runtime_pump(runtime, 50L, error);
    if (rc == CAI_OK)
      rc = cai_agent_runtime_is_quiescent(runtime, &quiet, error);
  }
  return rc != CAI_OK ? rc : quiet ? CAI_OK : CAI_ERR_LIMIT;
}
static void goal_input_checkpoint(test_state *state) {
  static const char response[] =
      "data: {\"type\":\"response.output_text.delta\",\"delta\":\"done\"}\n\n"
      "data: "
      "{\"type\":\"response.completed\",\"response\":{\"id\":\"restored\"}}"
      "\n\n";
  static const char *required[] = {"Initial user task"};
  goal_input_recovery *s = calloc(1U, sizeof(*s));
  cai_agent_session_store store;
  cai_agent_runtime_config config;
  cai_agent_runtime *runtime = NULL;
  cai_agent_goal_snapshot goal;
  cai_error error;
  http_mock_client mock;
  mock_http_expectation script[7];
  char marker[65], event[256], checkpoint[1024];
  unsigned long long sequence = 0U;
  size_t i;
  int variant, opened = 0;
  cai_error_init(&error);
#define GOAL_CHECKPOINT_REQUIRE(condition)                                     \
  do {                                                                         \
    int valid = (condition);                                                   \
    expect_int(state, #condition, valid, 1L);                                  \
    if (!valid)                                                                \
      goto done;                                                               \
  } while (0)
  GOAL_CHECKPOINT_REQUIRE(s != NULL);
  memset(script, 0, sizeof(script));
  for (i = 0U; i < 7U; ++i) {
    script[i].request_prefix = "POST /v1/responses HTTP/";
    script[i].status = 200;
    script[i].status_text = "OK";
    script[i].content_type = "text/event-stream";
    script[i].body = response;
    script[i].required = required;
    script[i].required_count = 1U;
  }
  GOAL_CHECKPOINT_REQUIRE(http_mock_client_open_script(state, "goal_checkpoint",
                                                       script, 7U, &mock) == 0);
  opened = 1;
  memset(&store, 0, sizeof(store));
  store.context = s;
  store.load_latest = test_runtime_session_store_load;
  store.load_events_after = test_runtime_session_store_load_events_after;
  store.append_event = test_runtime_session_store_append_event;
  store.commit = test_runtime_session_store_commit;
  cai_agent_runtime_config_init(&config);
  config.workspace_directory = "/tmp";
  config.session_scope = "checkpoint-A";
  config.session_id = "conversation-A";
  config.session_store = &store;
  config.disable_terminal = config.disable_chrome = config.disable_web_search =
      1;
  config.event_callback = goal_input_recovery_event;
  config.event_context = s;
  GOAL_CHECKPOINT_REQUIRE(
      cai_agent_runtime_open(mock.client, &config, &runtime, &error) == CAI_OK);
  GOAL_CHECKPOINT_REQUIRE(cai_agent_runtime_start_goal(
                              runtime, "Fresh objective", "Initial user task",
                              "checkpoint-work", &error) == CAI_OK);
  GOAL_CHECKPOINT_REQUIRE(goal_input_recovery_wait(runtime, &error) == CAI_OK);
  cai_agent_runtime_close(runtime);
  runtime = NULL;
  {
    const char *p = strstr(s->saved.saved_commit, "\"goal_input_applied\":\"");
    GOAL_CHECKPOINT_REQUIRE(p != NULL);
    p += strlen("\"goal_input_applied\":\"");
    GOAL_CHECKPOINT_REQUIRE(strlen(p) > 64U && p[64] == '"');
    memcpy(marker, p, 64U);
    marker[64] = '\0';
  }
  for (i = 0U; i < s->saved.replay_event_count; ++i)
    if (strcmp(s->saved.replay_event_types[i], "goal_start") == 0) {
      sequence = s->saved.replay_event_sequences[i];
      strcpy(event, s->saved.replay_event_data_items[i]);
      break;
    }
  GOAL_CHECKPOINT_REQUIRE(sequence > 1U);
  for (variant = 0; variant < 4; ++variant) {
    memset(s, 0, sizeof(*s));
    snprintf(checkpoint, sizeof(checkpoint),
             "{\"version\":1,\"model\":\"gpt-5-nano\","
             "\"goal_input_applied\":\"%s\",%s\"history\":[]}",
             marker,
             variant == 1 ? ""
                          : "\"goal_objective\":\"Host superseded\","
                            "\"goal_status\":\"active\","
                            "\"goal_tokens_used\":37,"
                            "\"goal_elapsed_seconds\":91,"
                            "\"goal_created_at\":123,");
    s->saved.commit_json = checkpoint;
    s->saved.load_applied_event_sequence = sequence;
    s->saved.replay_event_count = 2U;
    s->saved.replay_event_sequences[0] = 1U;
    s->saved.replay_event_sequences[1] = sequence;
    strcpy(s->saved.replay_event_types[0], "input_journal_v2");
    strcpy(s->saved.replay_event_types[1], "goal_start");
    strcpy(s->saved.replay_event_data_items[1], event);
    strcpy(s->saved.loaded_session_id,
           variant == 3 ? "conversation-B" : "conversation-A");
    config.session_scope = variant == 2 ? "checkpoint-B" : "checkpoint-A";
    config.session_id = NULL;
    config.resume_latest = 1;
    GOAL_CHECKPOINT_REQUIRE(cai_agent_runtime_open(mock.client, &config,
                                                   &runtime, &error) == CAI_OK);
    GOAL_CHECKPOINT_REQUIRE(goal_input_recovery_wait(runtime, &error) ==
                            CAI_OK);
    GOAL_CHECKPOINT_REQUIRE(
        cai_agent_runtime_get_goal(runtime, &goal, &error) == CAI_OK);
    GOAL_CHECKPOINT_REQUIRE(s->changes == (variant < 2 ? 0U : 1U));
    if (variant == 1)
      GOAL_CHECKPOINT_REQUIRE(!goal.has_goal);
    else if (variant == 0) {
      GOAL_CHECKPOINT_REQUIRE(goal.has_goal && goal.tokens_used == 37LL);
      GOAL_CHECKPOINT_REQUIRE(goal.elapsed_seconds >= 91LL);
      GOAL_CHECKPOINT_REQUIRE(strcmp(goal.objective, "Host superseded") == 0);
    } else
      GOAL_CHECKPOINT_REQUIRE(goal.has_goal && goal.tokens_used == 0LL &&
                              strcmp(goal.objective, "Fresh objective") == 0);
    cai_agent_runtime_close(runtime);
    runtime = NULL;
    GOAL_CHECKPOINT_REQUIRE(
        strstr(s->saved.saved_commit, "\"goal_input_applied\":") != NULL);
  }
  /* No durable sequence exists without storage. Each explicit start must
   * still replace the goal instead of sharing an all-zero identity. */
  config.session_store = NULL;
  config.resume_latest = 0;
  GOAL_CHECKPOINT_REQUIRE(
      cai_agent_runtime_open(mock.client, &config, &runtime, &error) == CAI_OK);
  for (variant = 0; variant < 2; ++variant) {
    const char *objective =
        variant == 0 ? "First ephemeral" : "Second ephemeral";
    GOAL_CHECKPOINT_REQUIRE(
        cai_agent_runtime_start_goal(runtime, objective, "Initial user task",
                                     NULL, &error) == CAI_OK);
    GOAL_CHECKPOINT_REQUIRE(goal_input_recovery_wait(runtime, &error) ==
                            CAI_OK);
    GOAL_CHECKPOINT_REQUIRE(
        cai_agent_runtime_get_goal(runtime, &goal, &error) == CAI_OK);
    GOAL_CHECKPOINT_REQUIRE(goal.has_goal &&
                            strcmp(goal.objective, objective) == 0);
  }
done:
  cai_agent_runtime_close(runtime);
  if (opened)
    http_mock_client_close(state, "goal_checkpoint", &mock);
  cai_error_cleanup(&error);
  free(s);
#undef GOAL_CHECKPOINT_REQUIRE
}
static void *goal_input_worker(void *context) {
  test_state *state = context;
  static const char response[] =
      "data: {\"type\":\"response.output_text.delta\",\"delta\":\"done\"}\n\n"
      "data: "
      "{\"type\":\"response.completed\",\"response\":{\"id\":\"goal_input\"}}"
      "\n\n";
  static const char *required[] = {"Fresh objective", "Initial user task"};
  int mode;
#define GOAL_INPUT_REQUIRE(condition)                                          \
  do {                                                                         \
    int valid = (condition);                                                   \
    expect_int(state, #condition, valid, 1L);                                  \
    if (!valid) {                                                              \
      fprintf(stderr, "goal input mode %d line %d: %s\n", mode, __LINE__,      \
              f->error.message != NULL ? f->error.message : "");               \
      goto cleanup;                                                            \
    }                                                                          \
  } while (0)
  for (mode = 0; mode < 6; ++mode) {
    goal_input_fixture *f = calloc(1U, sizeof(*f));
    mock_http_expectation script[2];
    char root[] = CAI_TEST_BINARY_DIR "/test-tmp/goal-input-XXXXXX";
    char long_call[257];
    size_t i;
    int mock_open = 0;
    if (f == NULL) {
      test_fail(state, "goal_input_fixture", "allocation failed");
      break;
    }
    cai_error_init(&f->error);
    memset(script, 0, sizeof(script));
    for (i = 0U; i < 2U; ++i) {
      script[i].request_prefix = "POST /v1/responses HTTP/";
      script[i].required = required;
      script[i].required_count = 2U;
      script[i].status = mode == 1 ? 400 : 200;
      script[i].status_text = mode == 1 ? "Bad Request" : "OK";
      script[i].content_type =
          mode == 1 ? "application/json" : "text/event-stream";
      script[i].body = mode == 1
                           ? "{\"error\":{\"message\":\"goal input "
                             "rejected\",\"type\":\"invalid_request_error\"}}"
                           : response;
    }
    if (mode == 5) {
      script[0].required = NULL;
      script[0].required_count = 0U;
      script[0].forbidden = required;
      script[0].forbidden_count = 2U;
    }
    GOAL_INPUT_REQUIRE(http_mock_client_open_script(
                           state, "goal_input", script,
                           mode == 4 || mode == 5 ? 2U : 1U, &f->mock) == 0);
    mock_open = 1;
    GOAL_INPUT_REQUIRE(mkdtemp(root) != NULL);
    cai_agent_local_session_store_config_init(&f->storage);
    f->storage.root_directory = root;
    GOAL_INPUT_REQUIRE(cai_agent_local_session_store_open(
                           &f->storage, &f->store, &f->error) == CAI_OK);
    cai_agent_runtime_config_init(&f->config);
    f->config.workspace_directory = "/tmp";
    f->config.session_scope = "goal-input";
    f->config.session_id = "conversation-A";
    f->config.session_store = &f->store;
    f->config.record_transcript = 1;
    f->config.disable_terminal = f->config.disable_chrome =
        f->config.disable_web_search = 1;
    if (mode == 2) {
      f->observer.cancel = 1;
      f->config.event_callback = input_identity_event;
      f->config.event_context = &f->observer;
      f->config.synchronize_input_boundaries = 1;
    }
    if (mode == 5) {
      f->config.event_callback = goal_input_queue_event;
      f->config.event_context = f;
      f->config.synchronize_input_boundaries = 1;
      f->config.turn_queue_limit = 1U;
    }
    GOAL_INPUT_REQUIRE(cai_agent_runtime_open(f->mock.client, &f->config,
                                              &f->runtime,
                                              &f->error) == CAI_OK);
    f->observer.runtime = f->runtime;
    GOAL_INPUT_REQUIRE(
        cai_agent_runtime_start_goal(f->runtime, "", "Initial user task", NULL,
                                     &f->error) == CAI_ERR_INVALID);
    cai_error_cleanup(&f->error);
    cai_error_init(&f->error);
    f->large = malloc(66050U);
    GOAL_INPUT_REQUIRE(f->large != NULL);
    memset(f->large, 'x', 66049U);
    f->large[66049U] = '\0';
    GOAL_INPUT_REQUIRE(
        cai_agent_runtime_start_goal(f->runtime, f->large, "Initial user task",
                                     NULL, &f->error) == CAI_ERR_LIMIT);
    cai_error_cleanup(&f->error);
    cai_error_init(&f->error);
    GOAL_INPUT_REQUIRE(
        cai_agent_runtime_start_goal(f->runtime, "Fresh objective", "", NULL,
                                     &f->error) == CAI_ERR_INVALID);
    cai_error_cleanup(&f->error);
    cai_error_init(&f->error);
    memset(long_call, 'x', sizeof(long_call) - 1U);
    long_call[sizeof(long_call) - 1U] = '\0';
    GOAL_INPUT_REQUIRE(cai_agent_runtime_start_goal(
                           f->runtime, "Fresh objective", "Initial user task",
                           long_call, &f->error) == CAI_ERR_LIMIT);
    cai_error_cleanup(&f->error);
    cai_error_init(&f->error);
    if (mode == 3)
      cai_agent_runtime_test_set_fail_goal_status_replace(1);
    if (mode == 5)
      GOAL_INPUT_REQUIRE(cai_agent_runtime_submit(f->runtime, "Seed turn",
                                                  &f->error) == CAI_OK);
    else
      GOAL_INPUT_REQUIRE(cai_agent_runtime_start_goal(
                             f->runtime, "Fresh objective", "Initial user task",
                             "command:conversation-A:7:8:9",
                             &f->error) == CAI_OK);
    GOAL_INPUT_REQUIRE(goal_input_wait(f, mode == 5 ? 2 : 1) == CAI_OK);
    if (mode == 5)
      GOAL_INPUT_REQUIRE(f->queued);
    GOAL_INPUT_REQUIRE(
        input_identity_count(&f->trace,
                             mode == 2                ? "turn.cancelled"
                             : mode == 1 || mode == 3 ? "turn.failed"
                                                      : "turn.completed",
                             "command:conversation-A:7:8:9") == 1);
    GOAL_INPUT_REQUIRE(
        cai_agent_runtime_get_goal(f->runtime, &f->goal, &f->error) == CAI_OK);
    if (mode != 3) {
      GOAL_INPUT_REQUIRE(f->goal.has_goal && !f->goal.has_token_budget);
      GOAL_INPUT_REQUIRE(strcmp(f->goal.objective, "Fresh objective") == 0);
      GOAL_INPUT_REQUIRE(
          strcmp(f->goal.status, mode == 2 ? "paused" : "active") == 0);
    } else {
      GOAL_INPUT_REQUIRE(!f->goal.has_goal && f->trace.presented == 0);
      cai_agent_runtime_test_set_fail_goal_status_replace(0);
      GOAL_INPUT_REQUIRE(cai_agent_runtime_start_goal(
                             f->runtime, "Fresh objective", "Initial user task",
                             "recovered-goal-operation", &f->error) == CAI_OK);
      GOAL_INPUT_REQUIRE(goal_input_wait(f, 2) == CAI_OK);
      GOAL_INPUT_REQUIRE(input_identity_count(&f->trace, "turn.completed",
                                              "recovered-goal-operation") == 1);
    }
    cai_agent_runtime_close(f->runtime);
    f->runtime = NULL;
    if (mode == 0 || mode == 4) {
      if (mode == 4)
        GOAL_INPUT_REQUIRE(
            input_identity_cut_event(root, "goal-input", "goal_start") == 0);
      memset(&f->trace, 0, sizeof(f->trace));
      f->config.resume_latest = 1;
      GOAL_INPUT_REQUIRE(cai_agent_runtime_open(f->mock.client, &f->config,
                                                &f->runtime,
                                                &f->error) == CAI_OK);
      if (mode == 4) {
        GOAL_INPUT_REQUIRE(goal_input_wait(f, 1) == CAI_OK);
        GOAL_INPUT_REQUIRE(
            input_identity_count(&f->trace, "turn.completed",
                                 "command:conversation-A:7:8:9") == 1);
      } else {
        int quiet = 0;
        GOAL_INPUT_REQUIRE(cai_agent_runtime_is_quiescent(
                               f->runtime, &quiet, &f->error) == CAI_OK &&
                           quiet);
      }
    }
  cleanup:
    cai_agent_runtime_test_set_fail_goal_status_replace(0);
    cai_agent_runtime_close(f->runtime);
    cai_agent_local_session_store_close(&f->store);
    if (mock_open)
      http_mock_client_close(state, "goal_input", &f->mock);
    cai_error_cleanup(&f->error);
    free(f->large);
    free(f);
  }
#undef GOAL_INPUT_REQUIRE
  goal_input_invalid(state);
  goal_input_checkpoint(state);
  return NULL;
}
static void test_agent_runtime_goal_input(test_state *state) {
  pthread_t worker;
  int rc = pthread_create(&worker, NULL, goal_input_worker, state);
  expect_int(state, "goal_input_default_worker", rc, 0L);
  if (rc == 0)
    expect_int(state, "goal_input_join", pthread_join(worker, NULL), 0L);
}
