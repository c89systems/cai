/* Backend queue/lifecycle, event logging and shared authentication. */
#include "../cli/app_run.c"
typedef struct event_log_capture {
  char bytes[16384];
  size_t length;
} event_log_capture;

static int test_command_queue_modes(void) {
  static const char *const turns[] = {"/new",
                                      "/resume 1",
                                      "/review",
                                      "/review-and-fix",
                                      "/shell",
                                      "/shellshare",
                                      "/compact",
                                      "/trim 1",
                                      "/queue on",
                                      "/todo new board",
                                      "/reasoning-tool off",
                                      "/export output.md",
                                      "/retention 30",
                                      "/yolo on",
                                      "/yolo off",
                                      "/trim",
                                      "/trim list 20",
                                      "/todo use other",
                                      "/shellshare",
                                      "!sh",
                                      "!sh+",
                                      "!ls"};
  static const char *const requests[] = {"/fast",
                                         "/mcp enable chatgpt",
                                         "/mcp disable all",
                                         "/mcp refresh",
                                         "/mcp\trefresh chatgpt  ",
                                         "/theme-offset -300",
                                         "/subagent-theme-offset 1234",
                                         "/review-theme-offset 0",
                                         "/foreground",
                                         "/fg",
                                         "/background",
                                         "/bg",
                                         "/yield_time_ms 0",
                                         "/tier",
                                         "/tier flex",
                                         "/tier\tpriority  ",
                                         "/theme gruvbox:rose-pine-dawn",
                                         "/theme\tplain",
                                         "/model example",
                                         "/model high",
                                         "/model example high",
                                         "/reasoning high",
                                         "/goal pause",
                                         "/goal resume",
                                         "/goal clear",
                                         "/goal new objective",
                                         "/m example high",
                                         "/m\tmedium  ",
                                         "/r high",
                                         "/t plain"};
  static const char *const feed[] = {"/help",
                                     "/theme-offset",
                                     "/subagent-theme-offset",
                                     "/review-theme-offset",
                                     "/yield_time_ms",
                                     "/status",
                                     "/tools",
                                     "/mcp",
                                     "/mcp list",
                                     "/mcp help",
                                     "/mcp chatgpt",
                                     "/skills",
                                     "/quit",
                                     "/exit",
                                     "/resume",
                                     "/model",
                                     "/reasoning",
                                     "/theme",
                                     "/yolo",
                                     "/reasoning-tool",
                                     "/queue",
                                     "/todo",
                                     "/goal",
                                     "/goal status",
                                     "/goal help",
                                     "/tier list",
                                     "/tier\tlist  ",
                                     "/todo list",
                                     "/todo help",
                                     "/todo see DEF-001",
                                     "/todo see",
                                     "/todo layout",
                                     "/todo layout list",
                                     "/todo layout\tboard",
                                     "/autoqueue",
                                     "/autoqueue on",
                                     "/output",
                                     "/output full",
                                     "/history",
                                     "/history 20",
                                     "/retention",
                                     "/shell-context",
                                     "/shell-context off",
                                     "/unknown-command",
                                     "/fastest",
                                     "/tiers",
                                     "/m",
                                     "/r",
                                     "/t",
                                     "/mm",
                                     "/rr",
                                     "/tt"};
  cli_state state;
  cai_input_policy policy;
  size_t i;
  memset(&state, 0, sizeof(state));
  state.was_busy = 1;
  state.options.subagent_steering = 1;
  for (i = 0U; i < sizeof(turns) / sizeof(turns[0]); i++) {
    memset(&policy, 0, sizeof(policy));
    policy.intent = CAI_INPUT_STEERING;
    if (cli_prompt_classify(&state, turns[i], &policy, NULL) != CAI_OK ||
        !policy.action || policy.independent ||
        policy.intent != CAI_INPUT_QUEUED ||
        cli_prompt_mode(&state, turns[i], CAI_INPUT_STEERING) !=
            CAI_INPUT_QUEUED)
      return 1;
  }
  for (i = 0U; i < sizeof(feed) / sizeof(feed[0]); i++) {
    memset(&policy, 0, sizeof(policy));
    if (cli_prompt_classify(&state, feed[i], &policy, NULL) != CAI_OK ||
        !policy.action || !policy.independent ||
        !cli_command_at_feed_boundary(feed[i]))
      return 1;
    memset(&policy, 0, sizeof(policy));
    policy.intent = CAI_INPUT_STEERING;
    if (cli_prompt_classify(&state, feed[i], &policy, NULL) != CAI_OK ||
        !policy.action || !policy.independent ||
        policy.intent != CAI_INPUT_STEERING)
      return 1;
  }
  for (i = 0U; i < sizeof(requests) / sizeof(requests[0]); i++) {
    cai_input_intent modes[2];
    size_t m;
    modes[0] = CAI_INPUT_QUEUED;
    modes[1] = CAI_INPUT_STEERING;
    for (m = 0U; m < 2U; m++) {
      memset(&policy, 0, sizeof(policy));
      policy.intent = modes[m] == CAI_INPUT_STEERING ? CAI_INPUT_STEERING
                                                     : CAI_INPUT_QUEUED;
      if (cli_prompt_classify(&state, requests[i], &policy, NULL) != CAI_OK ||
          !policy.action || !policy.independent ||
          cli_command_at_feed_boundary(requests[i]) ||
          cli_command_at_turn_boundary(requests[i]) ||
          cli_prompt_mode(&state, requests[i], modes[m]) != modes[m])
        return 1;
    }
  }
  if (cli_command_at_request_boundary("/fastest") ||
      cli_command_at_request_boundary("/tiers") ||
      cli_command_at_request_boundary("ordinary prompt") ||
      !cai_cli_command_is_theme("/t plain") ||
      cai_cli_command_is_theme("/tier") ||
      !cai_cli_command_is_theme("/theme-offset -300") ||
      !cai_cli_command_is_theme("/subagent-theme-offset 1234") ||
      !cai_cli_command_is_theme("/review-theme-offset -1"))
    return 1;
  state.was_busy = 0;
  for (i = 0U; i < sizeof(turns) / sizeof(turns[0]); i++) {
    memset(&policy, 0, sizeof(policy));
    policy.intent = CAI_INPUT_STEERING;
    if (cli_prompt_classify(&state, turns[i], &policy, NULL) != CAI_OK ||
        !policy.action || policy.intent != CAI_INPUT_QUEUED)
      return 1;
  }
  state.foreground_subagent = 1;
  if (cai_cli_command_delivery("/goal start {}") != CLI_COMMAND_TURN ||
      cli_prompt_mode(&state, "/goal start {}", CAI_INPUT_STEERING) !=
          CAI_INPUT_QUEUED)
    return 1;
  state.options.subagent_steering = 0;
  memset(&policy, 0, sizeof(policy));
  if (cli_prompt_classify(&state, "ordinary prompt", &policy, NULL) != CAI_OK ||
      policy.action || policy.independent ||
      cli_prompt_mode(&state, "ordinary prompt", CAI_INPUT_QUEUED) !=
          CAI_INPUT_QUEUED)
    return 1;
  return cli_prompt_mode(&state, "child steer", CAI_INPUT_STEERING) ==
                     CAI_INPUT_QUEUED &&
                 cli_prompt_mode(&state, "/theme plain", CAI_INPUT_STEERING) ==
                     CAI_INPUT_STEERING &&
                 cli_prompt_mode(&state, "/t plain", CAI_INPUT_STEERING) ==
                     CAI_INPUT_STEERING
             ? 0
             : 1;
}

static int test_goal_handoff(void) {
  cli_state state;
  cai_agent_runtime_event event;
  memset(&state, 0, sizeof(state));
  memset(&event, 0, sizeof(event));
  state.awaiting_turn = 1;
  state.goal_starts_pending = 2U;
  event.type = CAI_AGENT_EVENT_RUN_COMPLETED;
  if (cli_event_lifecycle(&state, &event, NULL) != CAI_OK ||
      state.awaiting_turn || cli_advance_automation(&state, NULL) != CAI_OK ||
      state.automation_done)
    return 1;
  /* An older turn completing must not consult an obsolete goal projection.
   * Unrelated and child goal events cannot acknowledge these host starts. */
  event.type = CAI_AGENT_EVENT_GOAL_CHANGED;
  event.data = "paused";
  event.data_length = 6U;
  if (cli_event_lifecycle(&state, &event, NULL) != CAI_OK ||
      state.goal_starts_pending != 2U)
    return 1;
  event.data = "replaced";
  event.data_length = 8U;
  event.parent_tool_call_id = "child";
  if (cli_event_lifecycle(&state, &event, NULL) != CAI_OK ||
      state.goal_starts_pending != 2U)
    return 1;
  event.parent_tool_call_id = NULL;
  if (cli_event_lifecycle(&state, &event, NULL) != CAI_OK ||
      state.goal_starts_pending != 1U ||
      cli_advance_automation(&state, NULL) != CAI_OK || state.automation_done)
    return 1;
  event.data = "resumed";
  event.data_length = 7U;
  if (cli_event_lifecycle(&state, &event, NULL) != CAI_OK ||
      state.goal_starts_pending != 0U ||
      cli_event_lifecycle(&state, &event, NULL) != CAI_OK ||
      state.goal_starts_pending != 0U)
    return 1;
  state.goal_starts_pending = 1U;
  state.awaiting_turn = 1;
  event.type = CAI_AGENT_EVENT_RUN_FAILED;
  if (cli_event_lifecycle(&state, &event, NULL) != CAI_OK ||
      state.goal_starts_pending != 0U || state.awaiting_turn ||
      cli_advance_automation(&state, NULL) != CAI_OK || !state.automation_done)
    return 1;
  return 0;
}

static int event_log_write(void *context, const char *bytes, size_t count,
                           size_t *written) {
  event_log_capture *capture = (event_log_capture *)context;
  if (count >= sizeof(capture->bytes) - capture->length)
    return EIO;
  memcpy(capture->bytes + capture->length, bytes, count);
  capture->length += count;
  capture->bytes[capture->length] = '\0';
  *written = count;
  return 0;
}

static int test_event_logging(void) {
  cli_state state;
  cai_agent_runtime_event event;
  cai_error error;
  pslog_config config;
  event_log_capture capture;
  int rc;
  memset(&state, 0, sizeof(state));
  memset(&event, 0, sizeof(event));
  memset(&capture, 0, sizeof(capture));
  strcpy(state.pouch.state_directory, ".");
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.output.write = event_log_write;
  config.output.userdata = &capture;
  config.output.close = NULL;
  config.output.isatty = NULL;
  config.output.owned = 0;
  state.log = calloc(1U, sizeof(*state.log));
  if (state.log == NULL)
    return 1;
  state.log->logger = pslog_new(&config);
  if (state.log->logger == NULL) {
    free(state.log);
    return 1;
  }
  cai_error_init(&error);
  event.type = CAI_AGENT_EVENT_RUN_STARTED;
  event.data = "normal prompt";
  event.data_length = strlen(event.data);
  cli_log_event(&state, &event);
  rc = CAI_OK;
  event.type = CAI_AGENT_EVENT_STEERING_QUEUED;
  event.data = "steer prompt";
  event.data_length = strlen(event.data);
  if (rc == CAI_OK)
    cli_log_event(&state, &event);
  rc = CAI_OK;
  event.type = CAI_AGENT_EVENT_STEERING_DELIVERED;
  if (rc == CAI_OK)
    cli_log_event(&state, &event);
  rc = CAI_OK;
  event.type = CAI_AGENT_EVENT_TURN_QUEUED;
  event.data = "queued prompt";
  event.data_length = strlen(event.data);
  if (rc == CAI_OK)
    cli_log_event(&state, &event);
  rc = CAI_OK;
  event.type = CAI_AGENT_EVENT_TEXT_DELTA;
  event.data = "parent answer";
  event.data_length = strlen(event.data);
  if (rc == CAI_OK)
    cli_log_event(&state, &event);
  rc = CAI_OK;
  event.parent_tool_call_id = "child-invocation";
  event.data = "child answer";
  event.data_length = strlen(event.data);
  if (rc == CAI_OK)
    cli_log_event(&state, &event);
  rc = CAI_OK;
  event.type = CAI_AGENT_EVENT_REVIEW_REPORT;
  if (rc == CAI_OK)
    cli_log_event(&state, &event);
  rc = CAI_OK;
  event.type = CAI_AGENT_EVENT_RESPONSE_COMPLETED;
  event.data = NULL;
  event.data_length = 0U;
  if (rc == CAI_OK)
    cli_log_event(&state, &event);
  rc = CAI_OK;
  event.parent_tool_call_id = NULL;
  event.type = CAI_AGENT_EVENT_TEXT_DELTA;
  event.data = " continues";
  event.data_length = strlen(event.data);
  if (rc == CAI_OK)
    cli_log_event(&state, &event);
  rc = CAI_OK;
  state.log->logger->destroy(state.log->logger);
  free(state.log);
  cai_error_cleanup(&error);
  if (rc != CAI_OK || strstr(capture.bytes, "\"lvl\":\"info\"") == NULL ||
      strstr(capture.bytes, "\"role\":\"user\"") == NULL ||
      strstr(capture.bytes, "\"role\":\"assistant\"") == NULL ||
      strstr(capture.bytes, "\"prompt_kind\":\"normal\"") == NULL ||
      strstr(capture.bytes, "\"prompt_kind\":\"steering\"") == NULL ||
      strstr(capture.bytes, "\"prompt_kind\":\"queued\"") == NULL ||
      strstr(capture.bytes, "child answer") == NULL ||
      strstr(capture.bytes, "\"parent_tool_call_id\":\"child-invocation\"") ==
          NULL ||
      strstr(capture.bytes, "\"app\"") != NULL) {
    fprintf(stderr, "event logging failed: %s\n", capture.bytes);
    return 1;
  }
  return 0;
}

typedef struct auth_fixture {
  pthread_mutex_t lock;
  pthread_cond_t condition;
  int calls;
  int released;
  int second_started;
} auth_fixture;

static int test_auth_call(cai_chatgpt_auth *auth, cai_error *error) {
  auth_fixture *fixture = auth->impl;
  int call;
  (void)error;
  pthread_mutex_lock(&fixture->lock);
  call = ++fixture->calls;
  pthread_cond_broadcast(&fixture->condition);
  while (fixture->released < call)
    pthread_cond_wait(&fixture->condition, &fixture->lock);
  pthread_mutex_unlock(&fixture->lock);
  return CAI_OK;
}

static int test_auth_token(cai_chatgpt_auth *auth, char **out,
                           cai_error *error) {
  *out = NULL;
  return test_auth_call(auth, error);
}

static void *test_token_thread(void *context) {
  cli_state *state = context;
  cai_error error;
  char *token;
  cai_error_init(&error);
  (void)cli_auth_token(&state->shared_auth, &token, &error);
  cai_error_cleanup(&error);
  return NULL;
}

static void *test_refresh_thread(void *context) {
  cli_state *state = context;
  auth_fixture *fixture = state->auth->impl;
  cai_error error;
  pthread_mutex_lock(&fixture->lock);
  fixture->second_started = 1;
  pthread_cond_broadcast(&fixture->condition);
  pthread_mutex_unlock(&fixture->lock);
  cai_error_init(&error);
  (void)cli_auth_refresh(&state->shared_auth, &error);
  cai_error_cleanup(&error);
  return NULL;
}

static int test_shared_auth(void) {
  cli_state state;
  cai_chatgpt_auth auth;
  auth_fixture fixture;
  pthread_t token;
  pthread_t refresh;
  memset(&state, 0, sizeof(state));
  memset(&auth, 0, sizeof(auth));
  memset(&fixture, 0, sizeof(fixture));
  if (pthread_mutex_init(&fixture.lock, NULL) != 0 ||
      pthread_cond_init(&fixture.condition, NULL) != 0 ||
      pthread_mutex_init(&state.auth_lock, NULL) != 0)
    return 1;
  auth.impl = &fixture;
  auth.access_token = test_auth_token;
  auth.refresh = test_auth_call;
  state.auth = &auth;
  state.shared_auth.impl = &state;
  if (pthread_create(&token, NULL, test_token_thread, &state) != 0)
    return 1;
  pthread_mutex_lock(&fixture.lock);
  while (fixture.calls == 0)
    pthread_cond_wait(&fixture.condition, &fixture.lock);
  pthread_mutex_unlock(&fixture.lock);
  if (pthread_create(&refresh, NULL, test_refresh_thread, &state) != 0)
    return 1;
  pthread_mutex_lock(&fixture.lock);
  while (!fixture.second_started)
    pthread_cond_wait(&fixture.condition, &fixture.lock);
  if (fixture.calls != 1)
    return 1;
  fixture.released = 2;
  pthread_cond_broadcast(&fixture.condition);
  pthread_mutex_unlock(&fixture.lock);
  pthread_join(token, NULL);
  pthread_join(refresh, NULL);
  pthread_cond_destroy(&fixture.condition);
  pthread_mutex_destroy(&fixture.lock);
  pthread_mutex_destroy(&state.auth_lock);
  return fixture.calls == 2 ? 0 : 1;
}

int main(void) {
  return test_command_queue_modes() || test_goal_handoff() ||
         test_shared_auth() || test_event_logging();
}
