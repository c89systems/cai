#include <cai/environment.h>

static void test_client_environment(test_state *state) {
  static const char *const first_values[] = {"CAI_TEST_INSTANCE_KEY=first-key",
                                             NULL};
  static const char *const second_values[] = {
      "CAI_TEST_INSTANCE_KEY=second-key", NULL};
  static const char *first_required[] = {"Authorization: Bearer first-key"};
  static const char *second_required[] = {"Authorization: Bearer second-key"};
  static const char response_body[] = "{\"id\":\"env-response\",\"object\":"
                                      "\"response\",\"status\":\"completed\","
                                      "\"model\":\"fixture\",\"output\":[]}";
  static const mock_http_expectation script[] = {
      {"POST /v1/responses HTTP/", first_required, 1U, NULL, 0U, 200, "OK",
       "application/json", NULL, response_body},
      {"POST /v1/responses HTTP/", second_required, 1U, NULL, 0U, 200, "OK",
       "application/json", NULL, response_body},
      {"POST /v1/responses HTTP/", first_required, 1U, NULL, 0U, 200, "OK",
       "application/json", NULL, response_body}};
  cai_environment *first = NULL, *second = NULL;
  cai_client *clients[2] = {NULL, NULL};
  cai_client_config config;
  cai_response_create_params *params = NULL;
  cai_response *response = NULL;
  cai_error error;
  test_env_snapshot saved;
  http_mock_server server;
  size_t i;
  cai_error_init(&error);
  test_env_capture(&saved, "CAI_TEST_INSTANCE_KEY");
  setenv("CAI_TEST_INSTANCE_KEY", "wrong-process-key", 1);
  if (http_mock_server_open_script(state, "client_environment_wire", script, 3U,
                                   &server) != 0)
    goto cleanup;
  expect_int(state, "client_environment_first",
             cai_environment_open(first_values, &first, &error), CAI_OK);
  expect_int(state, "client_environment_second",
             cai_environment_open(second_values, &second, &error), CAI_OK);
  cai_client_config_init(&config);
  config.api_key_env = "CAI_TEST_INSTANCE_KEY";
  config.base_url = server.base_url;
  config.http_2_disabled = 1;
  config.timeout_ms = 1000L;
  for (i = 0U; i < 2U; ++i) {
    config.environment = i == 0U ? first : second;
    expect_int(state, "client_environment_open",
               cai_client_open(&config, &clients[i], &error), CAI_OK);
  }
  cai_environment_close(first);
  cai_environment_close(second);
  first = second = NULL;
  /* Clients retain their views independently of caller handles and later
   * process mutations. Requests alternate between two coexisting clients. */
  setenv("CAI_TEST_INSTANCE_KEY", "changed-process-key", 1);
  expect_int(state, "client_environment_params",
             cai_response_create_params_new(&params, &error), CAI_OK);
  if (params != NULL) {
    expect_int(state, "client_environment_model",
               params->set_model(params, "fixture", &error), CAI_OK);
    expect_int(state, "client_environment_input",
               params->add_text(params, "user", "probe", &error), CAI_OK);
    for (i = 0U; i < 3U && clients[0] != NULL && clients[1] != NULL; ++i) {
      expect_int(state, "client_environment_request",
                 cai_client_create_response(clients[i % 2U], params, &response,
                                            &error),
                 CAI_OK);
      cai_response_destroy(response);
      response = NULL;
    }
  }
  expect_str(state, "client_environment_process_untouched",
             getenv("CAI_TEST_INSTANCE_KEY"), "changed-process-key");
  expect_child_exit(state, "client_environment_wire", server.pid,
                    &server.child_status);
cleanup:
  cai_response_destroy(response);
  cai_response_create_params_destroy(params);
  cai_client_close(clients[0]);
  cai_client_close(clients[1]);
  cai_environment_close(first);
  cai_environment_close(second);
  test_env_restore(&saved);
  cai_error_cleanup(&error);
}

static void test_environment_tools(test_state *state) {
  static const char *const entries[] = {
      "HOME=/configured home", "XDG_CONFIG_HOME=relative-xdg",
      "PATH=/usr/bin:/bin", "CAI_TEST_CONTEXT_SECRET=owned", NULL};
  static const char command[] =
      "{\"cmd\":\"printf '%s|%s|%s' \\\"$HOME\\\" "
      "\\\"$XDG_CONFIG_HOME\\\" \\\"${CAI_TEST_CONTEXT_SECRET-unset}\\\"\"}";
  cai_environment *environment = NULL;
  cai_exec_tool_config exec;
  cai_terminal_tool_config terminal;
  cai_tool_registry *registry = NULL;
  cai_sink *sink = NULL;
  cai_sink_callbacks callbacks;
  cai_error error;
  write_state output;
  test_env_snapshot saved;
  int mode;
  cai_error_init(&error);
  memset(&exec, 0, sizeof(exec));
  memset(&terminal, 0, sizeof(terminal));
  memset(&callbacks, 0, sizeof(callbacks));
  memset(&output, 0, sizeof(output));
  test_env_capture(&saved, "CAI_TEST_CONTEXT_SECRET");
  setenv("CAI_TEST_CONTEXT_SECRET", "wrong-host-value", 1);
  expect_int(state, "tool_environment_open",
             cai_environment_open(entries, &environment, &error), CAI_OK);
  exec.root_path = CAI_TEST_BINARY_DIR;
  exec.yolo = 1;
  exec.environment = environment;
  if (run_exec_tool_case(state, "tool_environment_exec", &exec, command, CAI_OK,
                         &output, &error) == CAI_OK)
    expect_substr(state, "tool_environment_exec_output", output.buffer,
                  "/configured home|relative-xdg|owned");
  terminal.root_path = CAI_TEST_BINARY_DIR;
  terminal.environment = environment;
  terminal.foreground = 1;
  expect_int(state, "tool_environment_registry",
             cai_tool_registry_new(&registry, &error), CAI_OK);
  if (registry == NULL)
    goto cleanup;
  if (cai_tool_registry_register_terminal_tools(registry, &terminal, &error) !=
      CAI_OK) {
    test_fail(state, "tool_environment_terminal", "registration failed");
    goto cleanup;
  }
  cai_environment_close(environment);
  environment = NULL;
  callbacks.write = test_write;
  callbacks.context = &output;
  expect_int(state, "tool_environment_sink",
             cai_sink_from_callbacks(&callbacks, &sink, &error), CAI_OK);
  if (sink == NULL)
    goto cleanup;
  for (mode = 0; mode < 3; ++mode) {
    int yolo = mode == 1;
    memset(&output, 0, sizeof(output));
    expect_int(state, "tool_environment_policy",
               cai_terminal_tools_yolo_internal(registry, CAI_TEST_BINARY_DIR,
                                                yolo, &error),
               CAI_OK);
    expect_int(
        state, "tool_environment_terminal_run",
        cai_tool_registry_run(registry, "exec_command", command, sink, &error),
        CAI_OK);
    expect_substr(state, "tool_environment_terminal_output", output.buffer,
                  yolo ? "/configured home|relative-xdg|owned"
                       : "/configured home|relative-xdg|unset");
  }
  expect_str(state, "tool_environment_host_unchanged",
             getenv("CAI_TEST_CONTEXT_SECRET"), "wrong-host-value");
cleanup:
  cai_environment_close(environment);
  cai_sink_close(sink);
  cai_tool_registry_destroy(registry);
  test_env_restore(&saved);
  cai_error_cleanup(&error);
}

static int environment_plugin(void *context, cai_source *input,
                              cai_sink *output, const cai_plugin_call *call,
                              cai_error *error) {
  const char *home = cai_environment_get(call->environment, "HOME");
  (void)context;
  (void)input;
  return home != NULL ? cai_sink_write(output, home, strlen(home), error)
                      : CAI_ERR_INVALID;
}
static void test_agent_environment(test_state *state) {
  static const char *const entries[] = {"HOME=/agent home",
                                        "PATH=/usr/bin:/bin", NULL};
  cai_environment *environment = NULL;
  cai_client *client = NULL;
  cai_agent *agents[2] = {NULL, NULL};
  cai_agent_runtime *runtime = NULL;
  cai_client_config cc;
  cai_agent_config ac;
  cai_agent_runtime_config rc;
  cai_exec_tool_config exec;
  cai_terminal_tool_config terminal;
  cai_plugins *plugins = NULL;
  cai_plugins_config pc;
  cai_plugin_call call;
  cai_plugin_definition definition;
  cai_plugin_handler handler;
  cai_sink *sink = NULL;
  cai_sink_callbacks callbacks;
  cai_error error;
  write_state output;
  size_t i;
  cai_error_init(&error);
  memset(&callbacks, 0, sizeof(callbacks));
  memset(&output, 0, sizeof(output));
  memset(&exec, 0, sizeof(exec));
  memset(&terminal, 0, sizeof(terminal));
  memset(&definition, 0, sizeof(definition));
  memset(&handler, 0, sizeof(handler));
  expect_int(state, "agent_environment_open",
             cai_environment_open(entries, &environment, &error), CAI_OK);
  cai_client_config_init(&cc);
  cc.api_key = "fixture";
  cc.environment = environment;
  expect_int(state, "agent_environment_client",
             cai_client_open(&cc, &client, &error), CAI_OK);
  if (client == NULL)
    goto cleanup;
  cai_agent_config_init(&ac);
  ac.model = "gpt-6-luna";
  for (i = 0U; i < 2U; ++i)
    expect_int(state, "agent_environment_agent",
               cai_client_new_agent(client, &ac, &agents[i], &error), CAI_OK);
  if (agents[0] == NULL || agents[1] == NULL)
    goto cleanup;
  exec.root_path = CAI_TEST_BINARY_DIR;
  exec.yolo = 1;
  terminal.root_path = CAI_TEST_BINARY_DIR;
  terminal.yolo = 1;
  terminal.foreground = 1;
  expect_int(state, "agent_environment_exec",
             cai_agent_register_exec_tool(agents[0], &exec, &error), CAI_OK);
  expect_int(state, "agent_environment_terminal",
             cai_agent_register_terminal_tools(agents[1], &terminal, &error),
             CAI_OK);
  cai_plugins_config_init(&pc);
  cai_plugin_call_init(&call);
  call.workspace = CAI_TEST_BINARY_DIR;
  definition.name = "environment_probe";
  definition.description = "Read the invocation environment";
  definition.parameters_json = "{\"type\":\"object\"}";
  handler.invoke = environment_plugin;
  expect_int(state, "agent_environment_plugins",
             cai_plugins_open(&pc, &plugins, &error), CAI_OK);
  if (plugins == NULL)
    goto cleanup;
  expect_int(state, "agent_environment_define",
             cai_plugins_define(plugins, &definition, &handler, &error),
             CAI_OK);
  expect_int(state, "agent_environment_bind",
             cai_agent_use_plugins(agents[1], plugins, &call, &error), CAI_OK);
  callbacks.write = test_write;
  callbacks.context = &output;
  expect_int(state, "agent_environment_sink",
             cai_sink_from_callbacks(&callbacks, &sink, &error), CAI_OK);
  if (sink == NULL)
    goto cleanup;
  for (i = 0U; i < 2U; ++i) {
    memset(&output, 0, sizeof(output));
    expect_int(state, "agent_environment_inherited_command",
               cai_tool_registry_run(
                   CAI_AGENT_IMPL(agents[i])->tools, "exec_command",
                   "{\"cmd\":\"printf '%s' \\\"$HOME\\\"\"}", sink, &error),
               CAI_OK);
    expect_substr(state, "agent_environment_command_home", output.buffer,
                  "/agent home");
  }
  memset(&output, 0, sizeof(output));
  expect_int(state, "agent_environment_inherited_plugin",
             cai_tool_registry_run(CAI_AGENT_IMPL(agents[1])->tools,
                                   "environment_probe", "{}", sink, &error),
             CAI_OK);
  expect_str(state, "agent_environment_plugin_home", output.buffer,
             "/agent home");
  /* A deferred runtime profile owns an explicit environment even when there
   * is no terminal manager. Its caller may release that view immediately. */
  cai_agent_runtime_config_init(&rc);
  rc.workspace_directory = CAI_TEST_BINARY_DIR;
  rc.model = "gpt-6-luna";
  rc.disable_terminal = rc.disable_chrome = rc.disable_web_search = 1;
  rc.disable_skills = rc.disable_default_session_store = 1;
  terminal.environment = environment;
  rc.terminal_tool_config = &terminal;
  expect_int(state, "agent_environment_profile",
             cai_agent_runtime_open(client, &rc, &runtime, &error), CAI_OK);
  cai_environment_close(environment);
  environment = NULL;
cleanup:
  cai_agent_runtime_close(runtime);
  cai_sink_close(sink);
  cai_agent_destroy(agents[0]);
  cai_agent_destroy(agents[1]);
  cai_plugins_close(plugins);
  cai_client_close(client);
  cai_environment_close(environment);
  cai_error_cleanup(&error);
}
