#define _POSIX_C_SOURCE 200809L
#include "bounded_allocator.h"
#include <assert.h>
#include <cai/agent_runtime.h>
#include <cai/smith.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct output {
  size_t bytes, calls, fail_at;
  int objective;
  char text[128];
} output;
static int receive(void *context, const void *bytes, size_t count,
                   cai_error *error) {
  output *out = context;
  size_t i;
  (void)error;
  assert(count <= 4096U);
  if (++out->calls == out->fail_at)
    return CAI_ERR_CANCELLED;
  if (out->objective) {
    assert(out->bytes + count < sizeof(out->text));
    memcpy(out->text + out->bytes, bytes, count);
    out->text[out->bytes + count] = '\0';
  } else
    for (i = 0U; i < count; ++i)
      assert(((const char *)bytes)[i] == '~');
  out->bytes += count;
  return CAI_OK;
}
static void *read_capture(void *context) {
  const cai_handover_config *config = cai_agent_handover_config(context);
  cai_sink sink;
  output out;
  memset(&sink, 0, sizeof(sink));
  sink.callbacks.context = &out;
  sink.callbacks.write = receive;
  memset(&out, 0, sizeof(out));
  out.fail_at = 4U;
  bounded = 1;
  assert(config->developer_instructions.write(
             config->developer_instructions.context, &sink, NULL) ==
         CAI_ERR_CANCELLED);
  memset(&out, 0, sizeof(out));
  assert(config->developer_instructions.write(
             config->developer_instructions.context, &sink, NULL) == CAI_OK);
  assert(out.bytes == 3U * 1024U * 1024U && out.calls == 768U);
  memset(&out, 0, sizeof(out));
  out.objective = 1;
  assert(config->goal_objective.write(config->goal_objective.context, &sink,
                                      NULL) == CAI_OK);
  assert(strcmp(out.text, "ORIGINAL GOAL") == 0);
  bounded = 0;
  return NULL;
}
static void *run(void *unused) {
  cai_client_config cc;
  cai_agent_runtime_config rc;
  cai_agent_preset preset;
  cai_client *client = NULL;
  cai_agent_runtime *runtime = NULL;
  cai_agent_handover *capture = NULL, *failed = NULL;
  cai_error error;
  char directory[] = "cai-capture-XXXXXX";
  char *text = malloc(3U * 1024U * 1024U + 1U);
  size_t i, attempts, retained;
  int status = CAI_ERR_INVALID;
  pthread_t reader;
  (void)unused;
  assert(text && mkdtemp(directory));
  memset(text, '~', 3U * 1024U * 1024U);
  text[3U * 1024U * 1024U] = '\0';
  cai_error_init(&error);
  cai_client_config_init(&cc);
  cc.api_key = "offline-capture";
  cc.base_url = "http://127.0.0.1:1/v1";
  cc.logger_disabled = 1;
  assert(cai_client_open(&cc, &client, &error) == CAI_OK);
  cai_agent_preset_from_smith(&preset);
  preset.developer_instructions = text;
  preset.tool_capabilities = 0UL;
  preset.supports_review = 0;
  cai_agent_runtime_config_init(&rc);
  rc.workspace_directory = directory;
  rc.agent_config_directory = directory;
  rc.preset_descriptor = &preset;
  rc.disable_terminal = rc.disable_default_session_store = 1;
  assert(cai_agent_runtime_open(client, &rc, &runtime, &error) == CAI_OK);
  free(text);
  assert(cai_agent_runtime_replace_goal(runtime, "ORIGINAL GOAL", &error) ==
         CAI_OK);
  for (i = 0U; i < 40U && capture == NULL; ++i) {
    assert(cai_agent_runtime_pump(runtime, 100L, &error) == CAI_OK);
    allocation_attempts = 0U;
    bounded = 1;
    status = cai_agent_runtime_prepare_handover(runtime, &capture, &error);
    bounded = 0;
    if (status == CAI_ERR_INVALID)
      cai_error_cleanup(&error);
  }
  assert(status == CAI_OK && capture);
  assert(cai_agent_handover_config(capture)->history.write == NULL);
  attempts = allocation_attempts;
  retained = live_bytes;
  assert(retained > 0U && retained < 65536U);
  for (i = 0U; i < attempts; ++i) {
    allocation_fail_at = i;
    allocation_attempts = 0U;
    bounded = 1;
    assert(cai_agent_runtime_prepare_handover(runtime, &failed, &error) ==
           CAI_ERR_NOMEM);
    bounded = 0;
    cai_error_cleanup(&error);
    assert(failed == NULL && live_bytes == retained);
  }
  allocation_fail_at = SIZE_MAX;
  assert(cai_agent_runtime_prepare_handover(runtime, NULL, &error) ==
         CAI_ERR_INVALID);
  assert(cai_agent_runtime_clear_goal(runtime, &error) == CAI_OK);
  for (i = 0U; i < 40U; ++i) {
    int quiet = 0;
    assert(cai_agent_runtime_pump(runtime, 100L, &error) == CAI_OK);
    assert(cai_agent_runtime_is_quiescent(runtime, &quiet, &error) == CAI_OK);
    if (quiet)
      break;
  }
  assert(i < 40U);
  cai_agent_runtime_close(runtime);
  client->close(client);
  assert(strcmp(cai_agent_handover_config(capture)->goal_status, "active") ==
         0);
  assert(pthread_create(&reader, NULL, read_capture, capture) == 0);
  assert(pthread_join(reader, NULL) == 0);
  cai_agent_handover_close(capture);
  assert(live_bytes == 0U && peak_bytes < 1048576U);
  /* Header values remain explicitly bounded; unlike text, they are copied. */
  text = malloc(8193U);
  assert(text);
  memset(text, 's', 8192U);
  text[8192U] = '\0';
  preset.developer_instructions = "small policy";
  rc.session_scope = text;
  assert(cai_client_open(&cc, &client, &error) == CAI_OK);
  assert(cai_agent_runtime_open(client, &rc, &runtime, &error) == CAI_OK);
  failed = NULL;
  bounded = 1;
  assert(cai_agent_runtime_prepare_handover(runtime, &failed, &error) ==
         CAI_ERR_LIMIT);
  bounded = 0;
  cai_error_cleanup(&error);
  assert(failed == NULL && live_bytes == 0U);
  assert(cai_agent_handover_config(NULL) == NULL);
  cai_agent_handover_close(NULL);
  assert(cai_agent_runtime_prepare_handover(NULL, &failed, &error) ==
         CAI_ERR_INVALID);
  cai_agent_runtime_close(runtime);
  client->close(client);
  free(text);
  cai_error_cleanup(&error);
  assert(rmdir(directory) == 0);
  return NULL;
}
int main(void) {
  pthread_t owner;
  assert(pthread_create(&owner, NULL, run, NULL) == 0);
  assert(pthread_join(owner, NULL) == 0);
  return 0;
}
