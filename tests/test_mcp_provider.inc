/* Provider credentials are connection-owned, never inferred from the model. */
#include "cai_mcp_search.h"

static int test_mcp_discard_result(void *context, const void *bytes,
                                   size_t length, cai_error *error) {
  (void)context;
  (void)bytes;
  (void)length;
  (void)error;
  return CAI_OK;
}

static void test_mcp_tool_aliases(test_state *state) {
  test_mcp_client_impl fake;
  cai_mcp_tool_registration_config config;
  cai_tool_registry *registry = NULL;
  cai_sink *sink = NULL;
  cai_sink_callbacks callbacks;
  lonejson_spooled args;
  lonejson_error json_error;
  cai_error error;
  char dotted[65], slash[65], long_name[201], second[65];
  cai_error_init(&error);
  expect_int(state, "alias_readable",
             cai_mcp_tool_alias("p_", "echo", dotted, &error), CAI_OK);
  expect_str(state, "alias_readable_name", dotted, "p_echo");
  expect_int(state, "alias_dotted",
             cai_mcp_tool_alias("apps_", "mail.search", dotted, &error),
             CAI_OK);
  expect_int(state, "alias_slash",
             cai_mcp_tool_alias("apps_", "mail/search", slash, &error), CAI_OK);
  expect_int(state, "alias_lossless_domain", strcmp(dotted, slash) != 0, 1L);
  memset(long_name, 'x', sizeof(long_name) - 1U);
  long_name[200] = '\0';
  expect_int(state, "alias_long",
             cai_mcp_tool_alias("apps_", long_name, second, &error), CAI_OK);
  expect_int(state, "alias_bounded", strlen(second) <= 64U, 1L);
  test_mcp_fake_client_init(&fake);
  fake.tools[0].name = "mail.search";
  fake.tools[1].name = "mail/search";
  memset(&config, 0, sizeof(config));
  config.name_prefix = "apps_";
  expect_int(state, "alias_registry", cai_tool_registry_new(&registry, &error),
             CAI_OK);
  if (registry != NULL) {
    expect_int(state, "alias_native_registration",
               cai_mcp_client_register_tools(&fake.public_client, registry,
                                             &config, &error),
               CAI_OK);
    expect_str(state, "alias_native_name",
               cai_tool_registry_name_at(registry, 0U), dotted);
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.write = test_mcp_discard_result;
    expect_int(state, "alias_null_sink",
               cai_sink_from_callbacks(&callbacks, &sink, &error), CAI_OK);
    memset(&args, 0, sizeof(args));
    CAI_LJ->spooled_init(CAI_LJ, &args);
    lonejson_error_init(&json_error);
    expect_int(state, "alias_arguments",
               args.append(&args, "{\"message\":\"hello\"}", 19U, &json_error),
               LONEJSON_STATUS_OK);
    expect_int(
        state, "alias_remote_dispatch",
        cai_tool_registry_run_spooled(registry, dotted, &args, sink, &error),
        CAI_OK);
    expect_str(state, "alias_exact_remote_name", fake.last_name, "mail.search");
    args.cleanup(&args);
    cai_sink_close(sink);
    cai_tool_registry_destroy(registry);
  }
  cai_error_cleanup(&error);
}

static void test_mcp_search_index(test_state *state) {
  cai_mcp_search *search = NULL;
  cai_mcp_search_match matches[32];
  cai_mcp_client_tool tool;
  cai_mcp_client_schema schema;
  const char *properties[] = {"recipientAddress", "invoiceNumber"};
  cai_error error;
  char name[64], *description;
  size_t count, i;
  memset(&tool, 0, sizeof(tool));
  memset(&schema, 0, sizeof(schema));
  cai_error_init(&error);
  expect_int(state, "search_open", cai_mcp_search_new(&search, &error), CAI_OK);
  if (search == NULL)
    return;
  schema.properties = properties;
  schema.property_count = 2U;
  tool.name = "searchInvoices";
  tool.title = "Search invoices";
  tool.description = "Find billing records in email messages";
  tool.input_schema = &schema;
  expect_int(state, "search_add_mail",
             cai_mcp_search_add(search, 17U, &tool, "mcp_mail_search", "Gmail",
                                "Search and send mail.", &error),
             CAI_OK);
  tool.name = "create_event";
  tool.title = "Create event";
  tool.description = "Schedule calendar events and attendees";
  tool.input_schema = NULL;
  expect_int(state, "search_add_calendar",
             cai_mcp_search_add(search, 19U, &tool, "mcp_calendar_create",
                                "Calendar", "Plan meetings.", &error),
             CAI_OK);
  for (i = 0U; i < 1000U; ++i) {
    snprintf(name, sizeof(name), "files_%lu", (unsigned long)i);
    tool.name = name;
    tool.title = "Read files";
    tool.description = "Open stored documents";
    expect_int(state, "search_add_large_catalog",
               cai_mcp_search_add(search, i + 100U, &tool, name, "Drive",
                                  "Work with documents.", &error),
               CAI_OK);
  }
  expect_int(state, "search_catalog_count", (long)cai_mcp_search_count(search),
             1002L);
  count = cai_mcp_search_find(search, "GMAIL invoice", matches, 8U, &error);
  expect_int(state, "search_relevant_count", (long)count, 1L);
  expect_int(state, "search_rank_mail", (long)matches[0].index, 17L);
  count = cai_mcp_search_find(search, "recipient address", matches, 1U, &error);
  expect_int(state, "search_parameter_names", (long)count, 1L);
  expect_int(state, "search_parameter_target", (long)matches[0].index, 17L);
  count = cai_mcp_search_find(search, "calendar meeting", matches, 8U, &error);
  expect_int(state, "search_source_description", (long)count, 1L);
  expect_int(state, "search_calendar_target", (long)matches[0].index, 19L);
  count = cai_mcp_search_find(search, "files", matches, 8U, &error);
  expect_int(state, "search_default_limit", (long)count, 8L);
  expect_int(state, "search_stable_tie", (long)matches[0].index, 100L);
  count = cai_mcp_search_find(search, "files files", matches, 32U, &error);
  expect_int(state, "search_repeated_query_terms", (long)count, 32L);
  expect_int(state, "search_zero_limit",
             (long)cai_mcp_search_find(search, "files", matches, 0U, &error),
             0L);
  expect_int(
      state, "search_no_match",
      (long)cai_mcp_search_find(search, "xyzzynomatch", matches, 8U, &error),
      0L);
  expect_int(state, "search_punctuation_only",
             (long)cai_mcp_search_find(search, "___ !", matches, 8U, &error),
             0L);
  expect_int(state, "search_no_match_is_success", error.code, CAI_OK);
  description = cai_mcp_search_description(search, &error);
  expect_int(state, "search_sources_hint",
             description != NULL &&
                 strstr(description, "- Gmail: Search and send mail.") != NULL,
             1L);
  expect_int(state, "search_sources_deduplicated",
             description != NULL && strstr(description, "files_999") == NULL &&
                 strlen(description) < 1024U,
             1L);
  cai_free_mem(NULL, description);
  cai_mcp_search_destroy(search);
  cai_error_cleanup(&error);
}

typedef struct test_mcp_provider_state {
  int headers, refreshes, version, bad;
} test_mcp_provider_state;
static int test_mcp_provider_headers(void *context, const char *url,
                                     cai_mcp_header_set_fn write,
                                     void *destination, cai_error *error) {
  test_mcp_provider_state *state = context;
  state->headers++;
  (void)url;
  if (state->bad)
    return write(destination,
                 state->bad == 1   ? "Bad\r\nName"
                 : state->bad == 3 ? "Content-Type"
                 : state->bad == 4 ? "Host"
                                   : "Authorization",
                 state->bad == 2 ? "secret\r\nInjected: yes" : "test",
                 error) == 0
               ? CAI_OK
               : CAI_ERR_INVALID;
  return write(destination, "Authorization",
               state->version ? "Bearer provider-new" : "Bearer provider-old",
               error) == 0
             ? CAI_OK
             : CAI_ERR_INVALID;
}
static int test_mcp_provider_refresh(void *context, cai_error *error) {
  test_mcp_provider_state *state = context;
  (void)error;
  state->refreshes++;
  state->version++;
  return CAI_OK;
}
static void test_mcp_provider_credentials(test_state *state) {
  static const char init[] =
      "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{"
      "\"protocolVersion\":\"" CAI_MCP_PROTOCOL_VERSION
      "\",\"capabilities\":{},"
      "\"serverInfo\":{\"name\":\"provider\",\"version\":\"1\"}}}";
  static const char *old[] = {"Authorization: Bearer provider-old",
                              "\"method\":\"initialize\""};
  static const char *fresh[] = {"Authorization: Bearer provider-new",
                                "\"method\":\"initialize\""};
  static const char *notification[] = {"Authorization: Bearer provider-new",
                                       "notifications/initialized"};
  static const char *ping[] = {"Authorization: Bearer provider-new",
                               "\"method\":\"ping\""};
  static const mock_http_expectation script[] = {
      {"POST /v1/mcp HTTP/", old, 2U, NULL, 0U, 401, "Unauthorized",
       "application/json", NULL, "{}"},
      {"POST /v1/mcp HTTP/", fresh, 2U, NULL, 0U, 200, "OK", "application/json",
       NULL, init},
      {"POST /v1/mcp HTTP/", notification, 2U, NULL, 0U, 202, "Accepted",
       "application/json", NULL, ""},
      {"POST /v1/mcp HTTP/", ping, 2U, NULL, 0U, 200, "OK", "application/json",
       NULL, "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{}}"}};
  http_mock_server server;
  cai_mcp_streamable_http_client_config config;
  cai_mcp_client_auth_provider provider;
  test_mcp_provider_state context;
  cai_mcp_client *client = NULL;
  cai_error error;
  char url[192];
  memset(&context, 0, sizeof(context));
  memset(&server, 0, sizeof(server));
  memset(&provider, 0, sizeof(provider));
  cai_error_init(&error);
  if (http_mock_server_open_script(state, "mcp_provider", script, 4U,
                                   &server) != 0)
    return;
  provider.context = &context;
  provider.headers = test_mcp_provider_headers;
  provider.refresh = test_mcp_provider_refresh;
  snprintf(url, sizeof(url), "%s/mcp", server.base_url);
  cai_mcp_streamable_http_client_config_init(&config);
  config.url = url;
  config.timeout_ms = 1000L;
  config.auth_provider = &provider;
  expect_int(state, "provider_open",
             cai_mcp_streamable_http_client_open(&config, &client, &error),
             CAI_OK);
  expect_int(state, "provider_ping", cai_mcp_client_ping(client, &error),
             CAI_OK);
  expect_int(state, "provider_renewed_once", context.refreshes, 1);
  expect_int(state, "provider_fresh_headers_each_request", context.headers, 4);
  cai_mcp_client_destroy(client);
  cai_error_cleanup(&error);
  expect_child_exit(state, "mcp_provider", server.pid, &server.child_status);
}

static void test_mcp_provider_validation(test_state *state) {
  cai_mcp_streamable_http_client_config config;
  cai_mcp_client_auth_provider provider;
  test_mcp_provider_state context;
  cai_mcp_client *client = NULL;
  cai_error error;
  int i;
  memset(&provider, 0, sizeof(provider));
  memset(&context, 0, sizeof(context));
  provider.context = &context;
  provider.headers = test_mcp_provider_headers;
  cai_mcp_streamable_http_client_config_init(&config);
  config.auth_provider = &provider;
  config.url = "http://example.com/mcp";
  cai_error_init(&error);
  expect_int(state, "provider_requires_secure_url",
             cai_mcp_streamable_http_client_open(&config, &client, &error),
             CAI_ERR_INVALID);
  config.url = "http://127.0.0.1:1/mcp";
  config.api_key = "conflicting-key";
  expect_int(state, "provider_exclusive_credentials",
             cai_mcp_streamable_http_client_open(&config, &client, &error),
             CAI_ERR_INVALID);
  config.api_key = NULL;
  provider.headers = NULL;
  expect_int(state, "provider_requires_callback",
             cai_mcp_streamable_http_client_open(&config, &client, &error),
             CAI_ERR_INVALID);
  provider.headers = test_mcp_provider_headers;
  for (i = 1; i <= 4; i++) {
    context.bad = i;
    expect_int(state, "provider_invalid_header_open",
               cai_mcp_streamable_http_client_open(&config, &client, &error),
               CAI_OK);
    expect_int(state, "provider_invalid_header_rejected_before_io",
               cai_mcp_client_ping(client, &error), CAI_ERR_INVALID);
    cai_mcp_client_destroy(client);
    client = NULL;
  }
  expect_int(state, "provider_only_valid_constructions_call_auth",
             context.headers, 4);
  cai_error_cleanup(&error);
}

static void test_mcp_provider_refresh_bounded(test_state *state) {
  static const mock_http_expectation script[] = {
      {"POST /v1/mcp HTTP/", NULL, 0U, NULL, 0U, 401, "Unauthorized",
       "application/json", NULL, "{}"},
      {"POST /v1/mcp HTTP/", NULL, 0U, NULL, 0U, 401, "Unauthorized",
       "application/json", NULL, "{}"}};
  http_mock_server server;
  cai_mcp_streamable_http_client_config config;
  cai_mcp_client_auth_provider provider;
  test_mcp_provider_state context;
  cai_mcp_client *client = NULL;
  cai_error error;
  char url[192];
  memset(&context, 0, sizeof(context));
  memset(&server, 0, sizeof(server));
  memset(&provider, 0, sizeof(provider));
  cai_error_init(&error);
  if (http_mock_server_open_script(state, "mcp_provider_bounded", script, 2U,
                                   &server) != 0)
    return;
  provider.context = &context;
  provider.headers = test_mcp_provider_headers;
  provider.refresh = test_mcp_provider_refresh;
  snprintf(url, sizeof(url), "%s/mcp", server.base_url);
  cai_mcp_streamable_http_client_config_init(&config);
  config.url = url;
  config.timeout_ms = 1000L;
  config.auth_provider = &provider;
  expect_int(state, "bounded_provider_open",
             cai_mcp_streamable_http_client_open(&config, &client, &error),
             CAI_OK);
  expect_int(state, "persistent_unauthorized_is_error",
             cai_mcp_client_ping(client, &error), CAI_ERR_SERVER);
  expect_int(state, "persistent_unauthorized_http", error.http_status, 401);
  expect_int(state, "at_most_two_requests", context.headers, 2);
  cai_mcp_client_destroy(client);
  cai_error_cleanup(&error);
  expect_child_exit(state, "mcp_provider_bounded", server.pid,
                    &server.child_status);
}

static void test_mcp_notification_204_opt_in(test_state *state) {
  static const mock_http_expectation script[] = {
      {"POST /v1/mcp HTTP/", NULL, 0U, NULL, 0U, 200, "OK", "application/json",
       NULL,
       "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":"
       "\"" CAI_MCP_PROTOCOL_VERSION "\",\"capabilities\":{},\"serverInfo\":{"
       "\"name\":\"test\",\"version\":\"1\"}}}"},
      {"POST /v1/mcp HTTP/", NULL, 0U, NULL, 0U, 204, "No Content",
       "application/json", NULL, ""}};
  int allow;
  for (allow = 0; allow <= 1; ++allow) {
    http_mock_server server;
    cai_mcp_streamable_http_client_config config;
    cai_mcp_client *client = NULL;
    cai_error error;
    char url[192];
    memset(&server, 0, sizeof(server));
    cai_error_init(&error);
    if (http_mock_server_open_script(state, "mcp_notification_204", script, 2U,
                                     &server) != 0)
      return;
    snprintf(url, sizeof(url), "%s/mcp", server.base_url);
    cai_mcp_streamable_http_client_config_init(&config);
    config.url = url;
    config.timeout_ms = 1000L;
    config.allow_notification_204 = allow;
    expect_int(state, "notification_204_open",
               cai_mcp_streamable_http_client_open(&config, &client, &error),
               CAI_OK);
    expect_int(state, "notification_204_requires_explicit_opt_in",
               cai_mcp_client_initialize(client, &error),
               allow ? CAI_OK : CAI_ERR_PROTOCOL);
    cai_mcp_client_destroy(client);
    cai_error_cleanup(&error);
    expect_child_exit(state, "mcp_notification_204", server.pid,
                      &server.child_status);
  }
}

static void *test_mcp_large_catalog_worker(void *context) {
  test_state *state = context;
  cai_buffer_builder body;
  mock_http_expectation script[3];
  http_mock_server server;
  cai_mcp_streamable_http_client_config config;
  cai_mcp_client *client = NULL;
  cai_error error;
  char *description;
  char name[160], url[192];
  size_t i;
  int rc;
  memset(&body, 0, sizeof(body));
  memset(script, 0, sizeof(script));
  memset(&server, 0, sizeof(server));
  cai_error_init(&error);
  description = malloc(16385U);
  if (description == NULL) {
    test_fail(state, "large_mcp_description", "allocation failed");
    return NULL;
  }
  memset(description, 'x', 16384U);
  description[16384] = '\0';
  rc = cai_buffer_append_cstr(
      &body, "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":[", &error);
  for (i = 0U; rc == CAI_OK && i < 40U; ++i) {
    snprintf(name, sizeof(name), "%s{\"name\":\"tool%lu\",\"description\":\"",
             i ? "," : "", (unsigned long)i);
    rc = cai_buffer_append_cstr(&body, name, &error);
    if (rc == CAI_OK)
      rc = cai_buffer_append_cstr(&body, description, &error);
    if (rc == CAI_OK)
      rc = cai_buffer_append_cstr(
          &body,
          "\",\"inputSchema\":{\"type\":\"object\"},"
          "\"outputSchema\":{\"anyOf\":[{\"type\":\"object\"}]},"
          "\"_meta\":{\"providerInstructions\":\"",
          &error);
    if (rc == CAI_OK)
      rc = cai_buffer_append_cstr(&body, description, &error);
    if (rc == CAI_OK)
      rc = cai_buffer_append_cstr(&body, "\"}}", &error);
  }
  if (rc == CAI_OK)
    rc = cai_buffer_append_cstr(&body, "]}}", &error);
  expect_int(state, "large_mcp_fixture", rc, CAI_OK);
  if (rc != CAI_OK)
    goto done;
  for (i = 0U; i < 3U; ++i) {
    script[i].request_prefix = "POST /v1/mcp HTTP/";
    script[i].status = 200;
    script[i].status_text = "OK";
    script[i].content_type = "application/json";
  }
  script[0].body = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{"
                   "\"protocolVersion\":\"" CAI_MCP_PROTOCOL_VERSION "\","
                   "\"capabilities\":{},\"serverInfo\":{\"name\":\"test\","
                   "\"version\":\"1\"}}}";
  script[1].status = 202;
  script[1].status_text = "Accepted";
  script[1].body = "";
  script[2].body = body.data;
  if (http_mock_server_open_script(state, "mcp_large_catalog", script, 3U,
                                   &server) != 0)
    goto done;
  snprintf(url, sizeof(url), "%s/mcp", server.base_url);
  cai_mcp_streamable_http_client_config_init(&config);
  config.url = url;
  config.timeout_ms = 3000L;
  expect_int(state, "large_mcp_open",
             cai_mcp_streamable_http_client_open(&config, &client, &error),
             CAI_OK);
  rc = cai_mcp_client_refresh_tools(client, &error);
  expect_int(state, "large_mcp_refresh", rc, CAI_OK);
  if (rc != CAI_OK)
    fprintf(stderr, "large catalog: %s (%s)\n",
            error.message != NULL ? error.message : "failed",
            error.detail != NULL ? error.detail : "no detail");
  expect_int(state, "large_mcp_all_tools", cai_mcp_client_tool_count(client),
             40);
  if (cai_mcp_client_tool_count(client) == 40U) {
    const cai_mcp_client_tool *tool = cai_mcp_client_tool_at(client, 39U);
    cai_mcp_search *search = NULL;
    cai_mcp_search_match matches[8];
    expect_int(state, "large_mcp_long_description", strlen(tool->description),
               16384);
    expect_str(state, "large_mcp_implicit_output_type",
               tool->output_schema->type, "");
    expect_int(state, "large_mcp_no_raw_hint", tool->raw_input == NULL, 1);
    expect_int(state, "large_search_open", cai_mcp_search_new(&search, &error),
               CAI_OK);
    if (search != NULL) {
      for (i = 0U; i < 40U; ++i) {
        tool = cai_mcp_client_tool_at(client, i);
        expect_int(state, "large_search_add",
                   cai_mcp_search_add(search, i, tool, tool->name,
                                      "Large catalog", "", &error),
                   CAI_OK);
      }
      expect_int(
          state, "large_search_default_thread_stack",
          (long)cai_mcp_search_find(search, "tool39", matches, 8U, &error), 1L);
      expect_int(state, "large_search_exact_tool", (long)matches[0].index, 39L);
      cai_mcp_search_destroy(search);
    }
  }
  cai_mcp_client_destroy(client);
  expect_child_exit(state, "mcp_large_catalog", server.pid,
                    &server.child_status);
done:
  free(description);
  cai_free_mem(NULL, body.data);
  cai_error_cleanup(&error);
  return NULL;
}
static void test_mcp_large_catalog(test_state *state) {
  pthread_t worker;
  if (pthread_create(&worker, NULL, test_mcp_large_catalog_worker, state) !=
      0) {
    test_fail(state, "mcp_large_catalog_thread", "create worker failed");
    return;
  }
  pthread_join(worker, NULL);
}

typedef struct test_mcp_refresh_observation {
  int updates, retained;
  size_t local, mcp;
  int renamed;
} test_mcp_refresh_observation;
static int test_mcp_refresh_event(void *context,
                                  const cai_agent_runtime_event *event,
                                  cai_error *error) {
  test_mcp_refresh_observation *seen = context;
  (void)error;
  if (event->type == CAI_AGENT_EVENT_MCP_UPDATED) {
    seen->updates++;
    if (event->data != NULL && event->data_length >= 25U &&
        memcmp(event->data, "MCP catalog update failed;", 25U) == 0)
      seen->retained++;
  }
  return CAI_OK;
}
static int test_mcp_refresh_inventory(void *context,
                                      const cai_agent_tool_info *tool,
                                      cai_error *error) {
  test_mcp_refresh_observation *seen = context;
  (void)error;
  if (tool->source == CAI_AGENT_TOOL_MCP) {
    seen->mcp++;
    if (strcmp(tool->name, "renamed") == 0)
      seen->renamed++;
  } else if (strcmp(tool->name, "tool_search") != 0)
    seen->local++;
  return CAI_OK;
}
static void test_mcp_runtime_refresh(test_state *state) {
  cai_client_config client_config;
  cai_agent_runtime_config config;
  cai_agent_runtime *runtime = NULL;
  cai_client *client = NULL;
  test_mcp_client_impl fake;
  cai_mcp_client *clients[1];
  test_mcp_refresh_observation seen;
  cai_error error;
  size_t retained_local = 0U;
  int pass, i;
  memset(&seen, 0, sizeof(seen));
  cai_error_init(&error);
  cai_client_config_init(&client_config);
  client_config.base_url = "http://127.0.0.1:1/v1";
  client_config.api_key = "fixture";
  expect_int(state, "mcp_refresh_client",
             cai_client_open(&client_config, &client, &error), CAI_OK);
  test_mcp_fake_client_init(&fake);
  clients[0] = &fake.public_client;
  cai_agent_runtime_config_init(&config);
  config.workspace_directory = "/tmp";
  config.disable_default_session_store = 1;
  config.disable_terminal = 1;
  config.disable_chrome = 1;
  config.disable_web_search = 1;
  config.mcp_clients = clients;
  config.mcp_client_count = 1U;
  config.event_callback = test_mcp_refresh_event;
  config.event_context = &seen;
  expect_int(state, "mcp_refresh_runtime_open",
             cai_agent_runtime_open(client, &config, &runtime, &error), CAI_OK);
  if (runtime != NULL) {
    for (pass = 0; pass < 4; ++pass) {
      if (pass == 0)
        fake.tools[0].name = "renamed";
      else if (pass == 1)
        fake.tools[1].name = "renamed"; /* Invalid replacement is atomic. */
      else
        fake.tool_count = pass == 2 ? 0U : 2U;
      if (pass == 3)
        fake.tools[1].name = "restored";
      expect_int(state, "mcp_refresh_schedule",
                 cai_agent_runtime_refresh_mcp(runtime, &error), CAI_OK);
      for (i = 0; i < 100 && seen.updates <= pass; ++i)
        expect_int(state, "mcp_refresh_pump",
                   cai_agent_runtime_pump(runtime, 20L, &error), CAI_OK);
      expect_int(state, "mcp_refresh_completion", seen.updates, pass + 1);
      seen.local = seen.mcp = 0U;
      seen.renamed = 0;
      expect_int(state, "mcp_refresh_inventory",
                 cai_agent_runtime_visit_tools(
                     runtime, test_mcp_refresh_inventory, &seen, &error),
                 CAI_OK);
      expect_int(state, "mcp_refresh_visible_count", (long)seen.mcp,
                 pass == 2 ? 0 : 2);
      expect_int(state, "mcp_refresh_name_published", seen.renamed,
                 pass == 2 ? 0 : 1);
      if (pass == 0)
        retained_local = seen.local;
      expect_int(state, "mcp_refresh_keeps_native_tools", (long)seen.local,
                 (long)retained_local);
    }
    expect_int(state, "mcp_refresh_preserved_failed_replacement", seen.retained,
               1);
    cai_agent_runtime_close(runtime);
  }
  cai_client_close(client);
  cai_error_cleanup(&error);
}
