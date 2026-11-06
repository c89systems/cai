#define _POSIX_C_SOURCE 200809L
#include "../src/cai_internal.h"
#include <assert.h>
#include <cai/agent_runtime.h>
#include <cai/handover.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef CAI_HANDOVER_WRAP
#include "bounded_allocator.h"
int __wrap_cai_client_open(const cai_client_config *, cai_client **,
                           cai_error *);
int __wrap_cai_agent_runtime_open(cai_client *,
                                  const cai_agent_runtime_config *,
                                  cai_agent_runtime **, cai_error *);
int __wrap_cai_client_open(const cai_client_config *config, cai_client **out,
                           cai_error *error) {
  (void)config;
  (void)out;
  (void)error;
  abort();
}
int __wrap_cai_agent_runtime_open(cai_client *client,
                                  const cai_agent_runtime_config *config,
                                  cai_agent_runtime **out, cai_error *error) {
  (void)client;
  (void)config;
  (void)out;
  (void)error;
  abort();
}
#endif

typedef struct fixture {
  const char **records;
  size_t count, chunk, calls, source_bytes, output_bytes, payload_bytes;
  char output[8192];
  size_t length, large_bytes, repetitions, expected_flow_call;
  int large, producing, observed_flow, raw, fail_source, fail_sink;
  int change, cancel, cancel_on_input, discard;
} fixture;

static int emit(fixture *f, cai_sink *sink, const char *text, size_t length,
                cai_error *error) {
  int rc;
  while (length != 0U) {
    size_t take = length < f->chunk ? length : f->chunk;
    f->source_bytes += take;
    if (f->cancel_on_input)
      f->cancel = 1;
    rc = cai_sink_write(sink, text, take, error);
    if (rc != CAI_OK)
      return rc;
    text += take;
    length -= take;
  }
  return CAI_OK;
}
static int history(void *context, cai_sink *sink, cai_error *error) {
  fixture *f = context;
  size_t i, repeat, count = f->count;
  char header[64];
  int rc = CAI_OK;
  f->calls++;
  f->producing = 1;
  f->source_bytes = 0U;
  if (f->fail_source)
    return cai_set_error(error, CAI_ERR_TRANSPORT, "history source failed");
  if (f->raw)
    return emit(f, sink, f->records[0], strlen(f->records[0]), error);
  if (f->calls == 2U && f->change == 1)
    count = 0U;
  if (f->calls == 2U && f->change == 2)
    count++;
  if (f->large) {
    const char *prefix = "[{\"content\":[{\"text\":\"";
    const char *suffix = "\",\"type\":\"output_text\"}],\"role\":\"assistant\","
                         "\"type\":\"message\"}]";
    char bytes[2048];
    size_t remaining = f->large_bytes;
    snprintf(header, sizeof(header), "%lu\n",
             (unsigned long)(strlen(prefix) + f->large_bytes + strlen(suffix)));
    rc = emit(f, sink, header, strlen(header), error);
    if (rc == CAI_OK)
      rc = emit(f, sink, prefix, strlen(prefix), error);
    memset(bytes, '~', sizeof(bytes));
    while (rc == CAI_OK && remaining != 0U) {
      size_t take = remaining < sizeof(bytes) ? remaining : sizeof(bytes);
      rc = emit(f, sink, bytes, take, error);
      remaining -= take;
    }
    if (rc == CAI_OK)
      rc = emit(f, sink, suffix, strlen(suffix), error);
    if (rc == CAI_OK)
      rc = emit(f, sink, "\n", 1U, error);
  } else
    for (repeat = 0U; rc == CAI_OK && repeat < f->repetitions; repeat++) {
      for (i = 0U; rc == CAI_OK && i < count; i++) {
        const char *record = f->records[i % f->count];
        size_t length = strlen(record);
        if (f->calls == 2U && f->change == 3)
          length++;
        snprintf(header, sizeof(header), "%lu\n", (unsigned long)length);
        rc = emit(f, sink, header, strlen(header), error);
        if (rc == CAI_OK)
          rc = emit(f, sink, record, strlen(record), error);
        if (rc == CAI_OK)
          rc = emit(f, sink, "\n", 1U, error);
      }
    }
  f->producing = 0;
  return rc;
}
static int output(void *context, const void *bytes, size_t length,
                  cai_error *error) {
  fixture *f = context;
  const char *data = bytes;
  size_t i;
  assert(length <= 4096U);
  if (f->fail_sink)
    return cai_set_error(error, CAI_ERR_CANCELLED, "handover sink stopped");
  f->output_bytes += length;
  if (f->large || f->discard) {
    for (i = 0U; i < length; i++)
      if (data[i] == '~') {
        if (!f->observed_flow) {
          assert(f->calls == f->expected_flow_call && f->producing);
          assert(f->source_bytes < f->large_bytes);
          f->observed_flow = 1;
        }
        f->payload_bytes++;
      }
  } else {
    assert(length < sizeof(f->output) - f->length);
    memcpy(f->output + f->length, bytes, length);
    f->length += length;
    f->output[f->length] = '\0';
  }
  return CAI_OK;
}
static int text(void *context, cai_sink *sink, cai_error *error) {
  const char *value = context;
  while (*value != '\0') {
    int rc = cai_sink_write(sink, value++, 1U, error);
    if (rc != CAI_OK)
      return rc;
  }
  return CAI_OK;
}
static int cancelled(void *context) { return ((fixture *)context)->cancel; }

static int generated_text(void *context, cai_sink *sink, cai_error *error) {
  fixture *f = context;
  char bytes[2048];
  size_t remaining = f->large_bytes;
  int rc = CAI_OK;
  f->calls++;
  f->producing = 1;
  f->source_bytes = 0U;
  memset(bytes, '~', sizeof(bytes));
  while (rc == CAI_OK && remaining != 0U) {
    size_t take = remaining > sizeof(bytes) ? sizeof(bytes) : remaining;
    rc = emit(f, sink, bytes, take, error);
    remaining -= take;
  }
  f->producing = 0;
  return rc;
}

static void reset(fixture *f) {
  memset(f, 0, sizeof(*f));
  f->chunk = 1U;
  f->repetitions = 1U;
  f->expected_flow_call = 2U;
}

static void *run(void *context) {
  static const char *records[] = {
      "[{\"content\":[{\"text\":\"hello \\u00e5 raw "
      "\303\245\\n**bold**\\u0000!\\u007f\","
      "\"type\":\"output_text\"}],\"role\":\"assistant\",\"type\":\"message\"}"
      "]",
      "[{\"summary\":[{\"text\":\"reason\",\"type\":\"summary_text\"}],"
      "\"type\":\"reasoning\"}]",
      "[{\"arguments\":\"{\\\"x\\\":1}\\nnext\",\"name\":\"read_file\","
      "\"type\":\"function_call\"}]",
      "[{\"output\":\"result\",\"type\":\"function_call_output\"}]",
      "[{\"content\":[{\"type\":\"input_image\",\"image_url\":\"omitted-"
      "binary\"}],"
      "\"role\":\"user\",\"type\":\"message\"}]",
      "[{\"type\":\"compaction\",\"encrypted_content\":\"opaque-value\"}]",
      "[{\"name\":\"silent\",\"type\":\"function_call\"}]",
      "[{\"content\":[{\"text\":\"dev\"}],\"role\":\"developer\"}]",
      "[{\"content\":[{\"text\":\"sys\"}],\"role\":\"system\"}]",
      "[{\"content\":[{\"text\":\"other\"}],\"role\":\"custom\"}]"};
  static const char expected[] =
      "## Assistant\n\nhello \303\245 raw \303\245\n**bold**!\n\n"
      "## Reasoning\n\nreason\n\n"
      "## Activity\n\n### Tool call: read_file\n\n    {\"x\":1}\n    next\n\n"
      "## Activity\n\n### Tool output\n\n    result\n\n"
      "## User\n\n*[non-text attachment omitted]*\n\n"
      "## Activity\n\n### Tool call: silent\n\n\n"
      "## Developer\n\ndev\n\n## System\n\nsys\n\n## Message\n\nother\n\n";
  static const char *bad[] = {"0\n",
                              "18446744073709551616\n",
                              "1",
                              "1\nx\n",
                              "2\n[]",
                              "4\n[{}]x",
                              "5\n[{},]\n",
                              "3\n[1]\n",
                              "6\n[{},1]\n",
                              "10\n[{},false]\n",
                              "8\n[{},\"x\"]\n",
                              "7\n[{},[]]\n",
                              "2\n{}\n",
                              "7\n[{},{}]\n",
                              "2\n[]\n"};
  fixture *f = calloc(1U, sizeof(*f));
  cai_handover_config config;
  cai_sink sink;
  cai_error error;
  const char *body;
  char objective[] = "goal\001\177";
  char instructions[] = "**policy**\nline\033";
  char empty[] = "";
  size_t i;
#ifdef CAI_HANDOVER_WRAP
  size_t fault_count;
#endif
  (void)context;
  assert(f != NULL);
  reset(f);
  cai_handover_config_init(&config);
  config.preset_name = "smith";
  config.active_model = "active-model";
  config.recorded_model = "recorded-model";
  config.session_id = "session";
  config.runtime_state = "idle";
  config.goal_status = "active";
  config.goal_has_token_budget = 1;
  config.goal_token_budget = 100;
  config.goal_tokens_used = 40;
  config.goal_elapsed_seconds = 12;
  config.goal_objective.write = text;
  config.goal_objective.context = objective;
  config.developer_instructions.write = text;
  config.developer_instructions.context = instructions;
  config.history.write = history;
  config.history.context = f;
  config.cancelled = cancelled;
  config.cancel_context = f;
  memset(&sink, 0, sizeof(sink));
  sink.callbacks.write = output;
  sink.callbacks.context = f;
  cai_error_init(&error);
  for (i = 1U; i <= 17U; i++) {
    reset(f);
    f->chunk = i;
    f->records = records;
    f->count = sizeof(records) / sizeof(records[0]);
    assert(cai_handover_write(&config, &sink, &error) == CAI_OK);
    assert(f->calls == 2U);
    assert(strstr(f->output, "- Recorded history model:\n    recorded-model"));
    assert(strstr(f->output, "- Tokens remaining:\n    60"));
    assert(strstr(f->output, "### Objective\n\ngoal\n\n"));
    assert(strstr(f->output, "**policy**\nline\n\n"));
    body = strstr(f->output, "## Assistant\n");
    assert(body != NULL && strcmp(body, expected) == 0);
    assert(strstr(f->output, "omitted-binary") == NULL);
    assert(strstr(f->output, "opaque-value") == NULL);
  }
  for (i = 0U; i < sizeof(bad) / sizeof(bad[0]); i++) {
    reset(f);
    f->raw = 1;
    f->records = &bad[i];
    assert(cai_handover_write(&config, &sink, NULL) == CAI_ERR_PROTOCOL);
    assert(f->calls == 1U);
  }
  for (i = 1U; i <= 3U; i++) {
    reset(f);
    f->records = records;
    f->count = 1U;
    f->change = (int)i;
    assert(cai_handover_write(&config, &sink, NULL) == CAI_ERR_PROTOCOL);
    assert(f->calls == 2U);
  }
  reset(f);
  f->fail_source = 1;
  assert(cai_handover_write(&config, &sink, &error) == CAI_ERR_TRANSPORT);
  assert(strstr(error.message, "history source failed"));
  assert(f->calls == 1U);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  reset(f);
  f->fail_sink = 1;
  assert(cai_handover_write(&config, &sink, &error) == CAI_ERR_CANCELLED);
  assert(f->calls == 0U && strstr(error.message, "handover sink stopped"));
  cai_error_cleanup(&error);
  cai_error_init(&error);
  reset(f);
  f->cancel_on_input = 1;
  f->records = records;
  f->count = 1U;
  assert(cai_handover_write(&config, &sink, &error) == CAI_ERR_CANCELLED);
  assert(f->calls == 1U && f->source_bytes == 1U);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  assert(cai_handover_write(NULL, &sink, NULL) == CAI_ERR_INVALID);
  assert(cai_handover_write(&config, NULL, NULL) == CAI_ERR_INVALID);
  config.goal_tokens_used = -1;
  assert(cai_handover_write(&config, &sink, NULL) == CAI_ERR_INVALID);
  config.goal_tokens_used = 0;
  config.goal_objective.write = NULL;
  config.developer_instructions.context = empty;
  reset(f);
  assert(cai_handover_write(&config, &sink, NULL) == CAI_OK);
  assert(f->calls == 2U);
  assert(strstr(f->output, "No active durable goal."));
  assert(strstr(f->output, "*[no developer instructions configured]*"));
  reset(f);
  f->discard = 1;
  f->records = records;
  f->count = 1U;
  f->repetitions = 1000U;
  assert(cai_handover_write(&config, &sink, &error) == CAI_OK);
  assert(f->calls == 2U && f->output_bytes > 30000U);
  reset(f);
  f->large = 1;
  f->expected_flow_call = 1U;
  f->chunk = 2048U;
  f->large_bytes = 8U * 1024U * 1024U;
  config.history.write = NULL;
  config.goal_objective.write = generated_text;
  config.goal_objective.context = f;
  config.developer_instructions.write = generated_text;
  config.developer_instructions.context = f;
#ifdef CAI_HANDOVER_WRAP
  bounded = 1;
  peak_bytes = 0U;
#endif
  assert(cai_handover_write(&config, &sink, &error) == CAI_OK);
  assert(f->calls == 2U && f->observed_flow);
  assert(f->payload_bytes == 2U * f->large_bytes);
#ifdef CAI_HANDOVER_WRAP
  assert(live_bytes == 0U && peak_bytes < 1048576U);
#endif
  config.goal_objective.write = NULL;
  config.developer_instructions.write = text;
  config.developer_instructions.context = empty;
  config.history.write = history;
  reset(f);
  f->large = 1;
  f->chunk = 2048U;
  f->large_bytes = 32U * 1024U * 1024U;
#ifdef CAI_HANDOVER_WRAP
  bounded = 1;
  allocation_attempts = 0U;
  peak_bytes = 0U;
#endif
  assert(cai_handover_write(&config, &sink, &error) == CAI_OK);
  assert(f->calls == 2U && f->observed_flow);
  assert(f->payload_bytes == f->large_bytes);
#ifdef CAI_HANDOVER_WRAP
  cai_error_cleanup(&error);
  assert(live_bytes == 0U && peak_bytes < 1048576U);
  reset(f);
  f->records = records;
  f->count = 1U;
  allocation_attempts = 0U;
  cai_error_init(&error);
  assert(cai_handover_write(&config, &sink, &error) == CAI_OK);
  fault_count = allocation_attempts;
  cai_error_cleanup(&error);
  assert(fault_count != 0U && live_bytes == 0U);
  for (i = 0U; i < fault_count; i++) {
    reset(f);
    f->records = records;
    f->count = 1U;
    allocation_attempts = 0U;
    allocation_fail_at = i;
    cai_error_init(&error);
    assert(cai_handover_write(&config, &sink, &error) == CAI_ERR_NOMEM);
    cai_error_cleanup(&error);
    assert(live_bytes == 0U);
  }
  allocation_fail_at = SIZE_MAX;
  bounded = 0;
#endif
  cai_error_cleanup(&error);
  free(f);
  return NULL;
}

int main(void) {
  pthread_t worker;
  assert(pthread_create(&worker, NULL, run, NULL) == 0);
  assert(pthread_join(worker, NULL) == 0);
  return 0;
}
