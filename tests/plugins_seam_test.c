#include <assert.h>
#include <cai/plugins.h>
#include <stdio.h>
#include <string.h>
typedef struct fixture {
  cai_plugins *plugins;
  int boundaries, old_calls, new_calls;
} fixture;
static int old_handler(void *context, cai_source *input, cai_sink *output,
                       const cai_plugin_call *call, cai_error *error) {
  fixture *f = context;
  (void)input;
  (void)call;
  f->old_calls++;
  return cai_sink_write(output, "OLD_HANDLER", 11U, error);
}
static int new_handler(void *context, cai_source *input, cai_sink *output,
                       const cai_plugin_call *call, cai_error *error) {
  fixture *f = context;
  (void)input;
  (void)call;
  f->new_calls++;
  return cai_sink_write(output, "NEW_HANDLER", 11U, error);
}
static int define(fixture *f, int updated, cai_error *error) {
  cai_plugin_definition d;
  cai_plugin_handler h;
  memset(&d, 0, sizeof(d));
  memset(&h, 0, sizeof(h));
  d.name = "replace_me";
  d.description = updated ? "NEW_SCHEMA" : "OLD_SCHEMA";
  d.parameters_json = "{\"type\":\"object\"}";
  h.context = f;
  h.invoke = updated ? new_handler : old_handler;
  return cai_plugins_define(f->plugins, &d, &h, error);
}
static int late_executor(void *context, const cai_plugin_definition *definition,
                         cai_plugin_handler *handler, cai_error *error) {
  (void)context;
  (void)definition;
  (void)handler;
  (void)error;
  return CAI_ERR_INVALID;
}
static int completed(void *context, cai_error *error) {
  fixture *f = context;
  cai_plugin_executor executor;
  f->boundaries++;
  if (f->boundaries == 1)
    return define(f, 1, error);
  if (f->boundaries == 2) {
    memset(&executor, 0, sizeof(executor));
    executor.prepare = late_executor;
    return cai_plugins_register_executor(f->plugins, "late_executor", &executor,
                                         error);
  }
  return CAI_OK;
}
int main(int argc, char **argv) {
  cai_client_config config;
  cai_client *client = NULL;
  cai_agent *agent = NULL;
  cai_session *session = NULL;
  cai_agent_config agent_config;
  cai_plugins_config plugins;
  cai_plugin_call call;
  cai_stream_sinks sinks;
  cai_error error;
  fixture f;
  int rc;
  assert(argc == 3);
  memset(&f, 0, sizeof(f));
  memset(&sinks, 0, sizeof(sinks));
  cai_error_init(&error);
  cai_client_config_init(&config);
  config.api_key = "fixture";
  config.base_url = argv[1];
  config.api_protocol = strcmp(argv[2], "chat_completions") == 0
                            ? CAI_API_PROTOCOL_CHAT_COMPLETIONS
                            : CAI_API_PROTOCOL_RESPONSES;
  cai_plugins_config_init(&plugins);
  cai_plugin_call_init(&call);
  call.workspace = ".";
  assert(cai_plugins_open(&plugins, &f.plugins, &error) == CAI_OK);
  assert(define(&f, 0, &error) == CAI_OK);
  assert(cai_client_open(&config, &client, &error) == CAI_OK);
  cai_agent_config_init(&agent_config);
  agent_config.model = "fixture";
  agent_config.disable_auto_compaction = 1;
  agent_config.session_continuity = CAI_SESSION_CONTINUITY_CLIENT_HISTORY;
  assert(cai_client_new_agent(client, &agent_config, &agent, &error) == CAI_OK);
  assert(cai_agent_use_plugins(agent, f.plugins, &call, &error) == CAI_OK);
  /* The retained agent reference owns the catalog after the host releases it.
   */
  cai_plugins_close(f.plugins);
  assert(cai_agent_new_session(agent, &session, &error) == CAI_OK);
  assert(session->add_user_text(session, "Run the tool", &error) == CAI_OK);
  sinks.response_completed = completed;
  sinks.response_completed_context = &f;
  rc = session->stream_auto(session, NULL, &sinks, &error);
  if (rc != CAI_OK)
    fprintf(stderr, "%s\n",
            error.message != NULL ? error.message : "plugin seam failed");
  assert(rc == CAI_OK && f.old_calls == 1 && f.new_calls == 1 &&
         f.boundaries == 3);
  session->close(session);
  agent->close(agent);
  client->close(client);
  cai_error_cleanup(&error);
  return 0;
}
