/** Real local journal with a failure seam at the atomic publication boundary.
 */
typedef struct activation_storage {
  cai_agent_session_store base;
  int loads, commits, fail;
} activation_storage;
static int activation_commit(void *context, const char *scope, const char *id,
                             cai_source *source, unsigned long long sequence,
                             cai_error *error) {
  activation_storage *s = context;
  ++s->commits;
  if (s->fail)
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "injected activation publication failure");
  return s->base.commit(s->base.context, scope, id, source, sequence, error);
}
static int activation_latest(void *context, const char *scope, char *id,
                             size_t capacity, cai_source **source,
                             unsigned long long *sequence, cai_error *error) {
  activation_storage *s = context;
  ++s->loads;
  return s->base.load_latest(s->base.context, scope, id, capacity, source,
                             sequence, error);
}
static int activation_id(void *context, const char *scope, const char *id,
                         cai_source **source, unsigned long long *sequence,
                         cai_error *error) {
  activation_storage *s = context;
  ++s->loads;
  return s->base.load_id(s->base.context, scope, id, source, sequence, error);
}
static int activation_append(void *context, const char *scope, const char *id,
                             const cai_agent_session_event *event,
                             cai_error *error) {
  activation_storage *s = context;
  return s->base.append_event(s->base.context, scope, id, event, error);
}
static int activation_events(void *context, const char *scope, const char *id,
                             unsigned long long after,
                             cai_agent_session_event_fn visit, void *receiver,
                             cai_error *error) {
  activation_storage *s = context;
  return s->base.load_events_after(s->base.context, scope, id, after, visit,
                                   receiver, error);
}
static int activation_wait(cai_agent_runtime *runtime, cai_error *error) {
  int i, quiet = 0, rc = CAI_OK;
  for (i = 0; rc == CAI_OK && !quiet && i < 100; ++i) {
    rc = cai_agent_runtime_pump(runtime, 50L, error);
    if (rc == CAI_OK)
      rc = cai_agent_runtime_is_quiescent(runtime, &quiet, error);
  }
  return rc != CAI_OK ? rc : quiet ? CAI_OK : CAI_ERR_LIMIT;
}
static int activation_registration_failure(void *context, cai_agent *agent,
                                           cai_error *error) {
  (void)context;
  (void)agent;
  return cai_set_error(error, CAI_ERR_INVALID,
                       "injected capability preparation failure");
}
static void test_agent_runtime_activation(test_state *state) {
#define ACT_REQUIRE(expression)                                                \
  do {                                                                         \
    int valid = (expression);                                                  \
    expect_int(state, #expression, valid, 1L);                                 \
    if (!valid) {                                                              \
      fprintf(stderr, "activation line %d: %s (%s)\n", __LINE__,               \
              error.message != NULL ? error.message : "",                      \
              error.detail != NULL ? error.detail : "");                       \
      goto done;                                                               \
    }                                                                          \
  } while (0)
  static const char response[] =
      "data: "
      "{\"type\":\"response.output_text.delta\",\"delta\":\"SESSION_RETAINED\"}"
      "\n\n"
      "data: "
      "{\"type\":\"response.completed\",\"response\":{\"id\":\"resp_"
      "activation\",\"usage\":{\"input_tokens\":12,\"output_tokens\":3,\"total_"
      "tokens\":15}}}\n\n";
  static const char *first[] = {"FIRST_AGENT_BASE", "original history input"};
  static const char *next[] = {"SECOND_AGENT_BASE", "original history input",
                               "continue under second agent",
                               "agent-two-model"};
  static const char *last[] = {"SECOND_AGENT_BASE", "original history input",
                               "resume after activation", "agent-two-model"};
  static const char *third[] = {"THIRD_AGENT_BASE", "original history input",
                                "resume under a different definition",
                                "agent-three-model"};
  static const char *old_base[] = {"FIRST_AGENT_BASE"};
  static const char *second_base[] = {"SECOND_AGENT_BASE"};
  static const mock_http_expectation script[] = {
      {"POST /v1/responses HTTP/", first, 2U, NULL, 0U, 200, "OK",
       "text/event-stream", NULL, response},
      {"POST /v1/responses HTTP/", next, 4U, old_base, 1U, 200, "OK",
       "text/event-stream", NULL, response},
      {"POST /v1/responses HTTP/", last, 4U, old_base, 1U, 200, "OK",
       "text/event-stream", NULL, response},
      {"POST /v1/responses HTTP/", third, 4U, second_base, 1U, 200, "OK",
       "text/event-stream", NULL, response}};
  cai_agent_runtime_config *config = calloc(1U, sizeof(*config));
  cai_agent_runtime *runtime = NULL, *candidate = NULL;
  cai_agent_session_store store;
  cai_agent_local_session_store_config local;
  activation_storage storage;
  cai_agent_runtime_metrics before, after;
  cai_input_queue *queue, *same;
  cai_input_queue_client *attachment = NULL;
  cai_input_request request;
  cai_input_result receipt;
  cai_input_entry entry;
  cai_agent_runtime_binding binding;
  cai_agent_preset preset;
  cai_agent_goal_request goal;
  cai_agent_goal_snapshot goal_view;
  cai_client *other_client = NULL;
  cai_error error;
  http_mock_client mock;
  char root[] = CAI_TEST_BINARY_DIR "/test-tmp/runtime-activation-XXXXXX";
  unsigned long long item_id;
  int loads, commits;
  memset(&storage, 0, sizeof(storage));
  memset(&mock, 0, sizeof(mock));
  cai_error_init(&error);
  ACT_REQUIRE(config != NULL);
  if (http_mock_client_open_script(state, "agent_activation", script, 4U,
                                   &mock) != 0)
    goto done;
  ACT_REQUIRE(mkdtemp(root) != NULL);
  cai_agent_local_session_store_config_init(&local);
  local.root_directory = root;
  ACT_REQUIRE(cai_agent_local_session_store_open(&local, &storage.base,
                                                 &error) == CAI_OK);
  memset(&store, 0, sizeof(store));
  store.context = &storage;
  store.commit = activation_commit;
  store.load_latest = activation_latest;
  store.load_id = activation_id;
  store.append_event = activation_append;
  store.load_events_after = activation_events;
  cai_agent_runtime_config_init(config);
  config->workspace_directory = "/tmp";
  config->session_scope = "activation-fixture";
  config->session_id = "same-session";
  config->session_store = &store;
  config->record_transcript = 1;
  config->disable_web_search = config->disable_chrome =
      config->disable_terminal = 1;
  config->developer_instructions_override = "FIRST_AGENT_BASE";
  ACT_REQUIRE(cai_agent_runtime_open_agent(mock.client, config, "first",
                                           "definition-1", &runtime,
                                           &error) == CAI_OK);
  ACT_REQUIRE(cai_agent_runtime_submit_interactive(
                  runtime, "original history input", &error) == CAI_OK);
  ACT_REQUIRE(activation_wait(runtime, &error) == CAI_OK);
  ACT_REQUIRE(cai_agent_runtime_get_metrics(runtime, &before, &error) ==
              CAI_OK);
  cai_agent_goal_request_init(&goal);
  goal.objective = "preserve this goal across agent activation";
  ACT_REQUIRE(cai_agent_runtime_create_goal(runtime, &goal, &error) == CAI_OK);
  ACT_REQUIRE(activation_wait(runtime, &error) == CAI_OK);
  ACT_REQUIRE(cai_agent_runtime_pending_input(runtime, &queue, &error) ==
              CAI_OK);
  ACT_REQUIRE(cai_input_queue_client_open(queue, &attachment, &error) ==
              CAI_OK);
  memset(&request, 0, sizeof(request));
  request.operation = CAI_INPUT_OFFER;
  request.request_id = 1U;
  request.enabled = 1;
  request.text = "pending input must remain held";
  ACT_REQUIRE(cai_input_queue_request(attachment, &request, &receipt, &error) ==
              CAI_OK);
  ACT_REQUIRE(receipt.outcome == CAI_INPUT_ACCEPTED);
  item_id = receipt.item_id;
  loads = storage.loads;
  ACT_REQUIRE(cai_agent_runtime_get_agent(runtime, &binding, &error) == CAI_OK);
  config->developer_instructions_override = "SECOND_AGENT_BASE";
  cai_agent_preset_from_smith(&preset);
  preset.default_model = "agent-two-model";
  config->preset_descriptor = &preset;
  config->register_agent_tools = activation_registration_failure;
  ACT_REQUIRE(cai_agent_runtime_activate(runtime, mock.client, config, "second",
                                         "definition-2", &candidate,
                                         &error) == CAI_ERR_INVALID);
  ACT_REQUIRE(candidate == NULL);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  config->register_agent_tools = NULL;
  mock.config.organization_id = "a-different-account";
  ACT_REQUIRE(cai_client_open(&mock.config, &other_client, &error) == CAI_OK);
  ACT_REQUIRE(cai_agent_runtime_activate(runtime, other_client, config,
                                         "second", "definition-2", &candidate,
                                         &error) == CAI_ERR_INVALID);
  ACT_REQUIRE(candidate == NULL);
  cai_client_close(other_client);
  other_client = NULL;
  mock.config.organization_id = NULL;
  cai_error_cleanup(&error);
  cai_error_init(&error);
  storage.fail = 1;
  ACT_REQUIRE(cai_agent_runtime_activate(runtime, mock.client, config, "second",
                                         "definition-2", &candidate,
                                         &error) == CAI_ERR_TRANSPORT);
  ACT_REQUIRE(candidate == NULL);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  ACT_REQUIRE(cai_agent_runtime_get_agent(runtime, &binding, &error) == CAI_OK);
  ACT_REQUIRE(strcmp(binding.name, "first") == 0);
  storage.fail = 0;
  ACT_REQUIRE(cai_agent_runtime_activate(runtime, mock.client, config, "second",
                                         "definition-2", &candidate,
                                         &error) == CAI_OK);
  ACT_REQUIRE(storage.loads == loads);
  ACT_REQUIRE(cai_agent_runtime_get_agent(candidate, &binding, &error) ==
              CAI_OK);
  ACT_REQUIRE(strcmp(binding.name, "second") == 0);
  ACT_REQUIRE(strcmp(cai_agent_runtime_session_id(candidate), "same-session") ==
              0);
  ACT_REQUIRE(cai_agent_runtime_pending_input(candidate, &same, &error) ==
                  CAI_OK &&
              same == queue);
  ACT_REQUIRE(cai_input_queue_entry_at(attachment, 0U, &entry, &error) ==
              CAI_OK);
  ACT_REQUIRE(entry.id == item_id && entry.held);
  ACT_REQUIRE(cai_agent_runtime_get_metrics(candidate, &after, &error) ==
              CAI_OK);
  ACT_REQUIRE(after.session_usage.usage.total_tokens ==
              before.session_usage.usage.total_tokens);
  ACT_REQUIRE(cai_agent_runtime_get_goal(candidate, &goal_view, &error) ==
              CAI_OK);
  ACT_REQUIRE(goal_view.has_goal &&
              strcmp(goal_view.objective, goal.objective) == 0 &&
              strcmp(goal_view.status, "active") == 0);
  cai_agent_runtime_close(runtime);
  runtime = candidate;
  candidate = NULL;
  request.operation = CAI_INPUT_WITHDRAW;
  request.request_id = 2U;
  request.item_id = entry.id;
  request.item_revision = entry.revision;
  ACT_REQUIRE(cai_input_queue_request(attachment, &request, &receipt, &error) ==
              CAI_OK);
  ACT_REQUIRE(receipt.outcome == CAI_INPUT_ACCEPTED);
  ACT_REQUIRE(cai_agent_runtime_submit_interactive(
                  runtime, "continue under second agent", &error) == CAI_OK);
  ACT_REQUIRE(activation_wait(runtime, &error) == CAI_OK);
  cai_input_queue_client_close(attachment);
  attachment = NULL;
  cai_agent_runtime_close(runtime);
  runtime = NULL;
  config->session_id = NULL;
  config->resume_session_id = "same-session";
  preset.default_model = "ignored-host-default";
  commits = storage.commits;
  ACT_REQUIRE(cai_agent_runtime_open_agent(mock.client, config, "second",
                                           "definition-2", &runtime,
                                           &error) == CAI_OK);
  ACT_REQUIRE(storage.commits == commits);
  ACT_REQUIRE(cai_agent_runtime_get_agent(runtime, &binding, &error) == CAI_OK);
  ACT_REQUIRE(strcmp(binding.name, "second") == 0);
  ACT_REQUIRE(cai_agent_runtime_bind_agent(runtime, "second", "definition-2",
                                           &error) == CAI_OK);
  ACT_REQUIRE(cai_agent_runtime_submit_interactive(
                  runtime, "resume after activation", &error) == CAI_OK);
  ACT_REQUIRE(activation_wait(runtime, &error) == CAI_OK);
  ACT_REQUIRE(cai_agent_runtime_get_agent(runtime, &binding, &error) ==
                  CAI_OK &&
              binding.sequence != 0U);
  cai_agent_runtime_close(runtime);
  runtime = NULL;
  preset.default_model = "agent-three-model";
  config->developer_instructions_override = "THIRD_AGENT_BASE";
  ACT_REQUIRE(cai_agent_runtime_open_agent(mock.client, config, "third",
                                           "definition-3", &runtime,
                                           &error) == CAI_OK);
  ACT_REQUIRE(cai_agent_runtime_submit_interactive(
                  runtime, "resume under a different definition", &error) ==
              CAI_OK);
  ACT_REQUIRE(activation_wait(runtime, &error) == CAI_OK);
  mock.config.organization_id = "a-different-account";
  ACT_REQUIRE(cai_client_open(&mock.config, &other_client, &error) == CAI_OK);
  ACT_REQUIRE(cai_agent_runtime_open(other_client, config, &candidate,
                                     &error) == CAI_ERR_INVALID);
  ACT_REQUIRE(candidate == NULL);
  cai_client_close(other_client);
  other_client = NULL;
done:
  cai_client_close(other_client);
  cai_input_queue_client_close(attachment);
  cai_agent_runtime_close(candidate);
  cai_agent_runtime_close(runtime);
  cai_agent_local_session_store_close(&storage.base);
  if (mock.client != NULL)
    http_mock_client_close(state, "agent_activation", &mock);
  free(config);
  cai_error_cleanup(&error);
#undef ACT_REQUIRE
}
