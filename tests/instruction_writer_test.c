#define _POSIX_C_SOURCE 200809L
#include "../src/cai_instruction_writer.h"
#include "../src/cai_internal.h"
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* A runtime-free export must not obtain a client/agent or allocate a complete
 * rendered instruction value. Large input is already owned by the caller. */
static int bounded;
#ifdef CAI_INSTRUCTION_WRAP
static int reject_all;
void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *pointer, size_t size);
void *__wrap_malloc(size_t size);
void *__wrap_calloc(size_t count, size_t size);
void *__wrap_realloc(void *pointer, size_t size);
int __wrap_cai_client_open(const cai_client_config *config, cai_client **out,
                           cai_error *error);
int __wrap_cai_client_new_agent(cai_client *client,
                                const cai_agent_config *config, cai_agent **out,
                                cai_error *error);
void *__wrap_malloc(size_t size) {
  return bounded && (reject_all || size > 65536U) ? NULL : __real_malloc(size);
}
void *__wrap_calloc(size_t count, size_t size) {
  return bounded && (reject_all || (size != 0U && count > 65536U / size))
             ? NULL
             : __real_calloc(count, size);
}
void *__wrap_realloc(void *pointer, size_t size) {
  return bounded && (reject_all || size > 65536U)
             ? NULL
             : __real_realloc(pointer, size);
}
int __wrap_cai_client_open(const cai_client_config *config, cai_client **out,
                           cai_error *error) {
  (void)config;
  (void)out;
  (void)error;
  abort();
}
int __wrap_cai_client_new_agent(cai_client *client,
                                const cai_agent_config *config, cai_agent **out,
                                cai_error *error) {
  (void)client;
  (void)config;
  (void)out;
  (void)error;
  abort();
}
#endif

typedef struct captured {
  char small[2048];
  size_t length, calls, fail_at;
  int large;
} captured;

static int capture(void *context, const void *bytes, size_t length,
                   cai_error *error) {
  captured *out = context;
  const char *data = bytes;
  size_t i;
  assert(length <= 4096U);
  out->calls++;
  if (out->fail_at == out->calls)
    return cai_set_error(error, CAI_ERR_CANCELLED, "instruction sink stopped");
  if (out->large) {
    for (i = 0U; i < length; i++) {
      size_t at = out->length + i;
      assert(data[i] == (at < 6U ? "BASE\n\n"[at] : 'x'));
    }
  } else {
    assert(length < sizeof(out->small) - out->length);
    memcpy(out->small + out->length, bytes, length);
    out->small[out->length + length] = '\0';
  }
  out->length += length;
  return CAI_OK;
}

static int empty_policy(void *context, const char *key, cai_source **out,
                        cai_error *error) {
  (void)context;
  (void)error;
  assert(strcmp(key, "AGENTS.md") == 0);
  *out = NULL;
  return CAI_OK;
}

typedef struct preparation_source {
  const char *text;
  size_t offset;
  unsigned int listed, read, closed, warnings;
  int fail;
} preparation_source;

static size_t preparation_read(void *context, void *bytes, size_t capacity,
                               cai_error *error) {
  preparation_source *source = context;
  size_t count = strlen(source->text) - source->offset;
  (void)error;
  if (count > capacity)
    count = capacity;
  if (count > 7U)
    count = 7U;
  memcpy(bytes, source->text + source->offset, count);
  source->offset += count;
  return count;
}
static void preparation_close(void *context) {
  ((preparation_source *)context)->closed++;
}
static int preparation_open(void *context, const char *key, cai_source **out,
                            cai_error *error) {
  preparation_source *source = context;
  cai_source_callbacks callbacks;
  (void)key;
  source->read++;
  *out = NULL;
  if (source->fail)
    return cai_set_error(error, CAI_ERR_TRANSPORT, "preparation source failed");
  source->offset = 0U;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.read = preparation_read;
  callbacks.close = preparation_close;
  callbacks.context = source;
  return cai_source_from_callbacks(&callbacks, out, error);
}
static int preparation_list(void *context, cai_skill_provider_visit_fn visit,
                            void *visit_context, cai_error *error) {
  preparation_source *source = context;
  source->listed++;
  return visit(visit_context, "fixture-skill", error);
}
static int preparation_skill(void *context, const char *id,
                             const char *resource, cai_source **out,
                             cai_error *error) {
  assert(strcmp(id, "fixture-skill") == 0 && resource == NULL);
  return preparation_open(context, id, out, error);
}
static void preparation_warning(void *context, const char *message) {
  assert(message != NULL && message[0] != '\0');
  ((preparation_source *)context)->warnings++;
}

static void preparation_cases(cai_agent_preset *preset,
                              cai_agent_preset_config *config, cai_sink *sink,
                              captured *out, cai_blob_store *store) {
  preparation_source source;
  cai_skill_provider provider;
  cai_skill_config skills;
  cai_agent_instruction_status status;
  cai_error error;
  memset(&source, 0, sizeof(source));
  source.text = "GLOBAL_POLICY";
  store->load = preparation_open;
  store->context = &source;
  cai_error_init(&error);
  memset(out, 0, sizeof(*out));
  assert(cai_agent_preset_write_instructions(preset, config, NULL, sink,
                                             &status, NULL) == CAI_OK);
  assert(status.global_agents_md_loaded);
  assert(strstr(out->small,
                "# AGENTS.md instructions from global instruction store"));
  assert(strstr(out->small, "GLOBAL_POLICY") != NULL);
  assert(source.read == 1U && source.closed == 1U);
  source.fail = 1;
  memset(out, 0, sizeof(*out));
  assert(cai_agent_preset_write_instructions(
             preset, config, NULL, sink, &status, &error) == CAI_ERR_TRANSPORT);
  assert(out->calls == 0U && !status.global_agents_md_loaded);
  assert(strstr(error.message, "preparation source failed") != NULL);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  store->load = empty_policy;
  store->context = NULL;
  memset(&source, 0, sizeof(source));
  source.text = "---\nname: useful\ndescription: Example skill\n---\n"
                "BODY_MUST_NOT_BE_IN_INSTRUCTIONS";
  memset(&provider, 0, sizeof(provider));
  provider.list = preparation_list;
  provider.read = preparation_skill;
  provider.context = &source;
  cai_skill_config_init(&skills);
  skills.skill_provider = &provider;
  skills.warning_callback = preparation_warning;
  skills.warning_context = &source;
  config->skills = &skills;
  preset->tool_capabilities = CAI_AGENT_PRESET_TOOL_SKILLS;
  memset(out, 0, sizeof(*out));
  assert(cai_agent_preset_write_instructions(preset, config, NULL, sink, NULL,
                                             NULL) == CAI_OK);
  assert(strstr(out->small, "useful") != NULL);
  assert(strstr(out->small, "BODY_MUST_NOT_BE_IN_INSTRUCTIONS") == NULL);
  assert(source.listed == 1U && source.read == 1U && source.closed == 1U);
  assert(source.warnings == 0U);
  source.fail = 1;
  memset(out, 0, sizeof(*out));
  assert(cai_agent_preset_write_instructions(preset, config, NULL, sink, NULL,
                                             &error) == CAI_OK);
  assert(strcmp(out->small, "BASE") == 0 && source.warnings != 0U);
  config->skills = NULL;
  preset->tool_capabilities = 0UL;
  cai_error_cleanup(&error);
}

static void check_template(cai_instruction_template *input,
                           const char *expected, size_t prefix) {
  captured out;
  cai_sink sink;
  size_t length, actual_prefix;
  memset(&out, 0, sizeof(out));
  memset(&sink, 0, sizeof(sink));
  sink.callbacks.write = capture;
  sink.callbacks.context = &out;
  assert(cai_instruction_template_write(input, NULL, &length, &actual_prefix,
                                        NULL) == CAI_OK);
  assert(length == strlen(expected) && actual_prefix == prefix);
  assert(cai_instruction_template_write(input, &sink, &length, &actual_prefix,
                                        NULL) == CAI_OK);
  assert(length == strlen(expected) && actual_prefix == prefix);
  assert(strcmp(out.small, expected) == 0);
}

static void template_cases(void) {
  cai_instruction_template input;
  assert(cai_agent_identity_valid("My Agent (Rev).2_-"));
  assert(!cai_agent_identity_valid(NULL));
  assert(!cai_agent_identity_valid(""));
  assert(!cai_agent_identity_valid(" ._-()"));
  assert(!cai_agent_identity_valid("Two  Words"));
  assert(!cai_agent_identity_valid(" Leading"));
  assert(!cai_agent_identity_valid("Trailing "));
  assert(!cai_agent_identity_valid("Agent\nInjected"));
  assert(!cai_agent_identity_valid("Agent\033[0m"));
  assert(!cai_agent_identity_valid("../Agent"));
  memset(&input, 0, sizeof(input));
  input.text = "A{{agent_identity}}B{{agent_tools}}C{{agent_identity}}";
  input.identity = "ID";
  input.tools = "TOOLS";
  input.repository = "repo";
  input.skills = "skill";
  input.extension = "extension";
  check_template(&input, "AIDBextension\n\nTOOLSCID\n\nrepo\n\nskill", 4U);
  input.tools = NULL;
  check_template(&input, "AIDB{{agent_tools}}CID\n\nextension\n\nrepo\n\nskill",
                 0U);
  input.repository = input.skills = input.extension = NULL;
  input.text = "{{agent_identity}}{{agent_identity}}{{agent_tools}}";
  input.identity = "";
  input.tools = "";
  check_template(&input, "", 0U);
  input.text = "fragment candidate Use `apply_patch`!{{agent_tools}}";
  check_template(&input, "fragment candidate Use `apply_patch`!",
                 strlen("fragment candidate Use `apply_patch`!"));
  /* Match spans a substitution, remove only the first occurrence, and do not
   * suppress a disabled-tool fragment in repository instructions or extension.
   */
  input.text = "Use {{agent_identity}}{{agent_tools}}";
  input.identity =
      "`apply_patch` for local file edits. Do not create or edit files "
      "with `cat` or other shell write tricks. Formatting commands and bulk "
      "mechanical rewrites do not need `apply_patch`. Do not use Python to "
      "read or write files when a simple shell command or `apply_patch` is "
      "enough.\n";
  input.tools = "TOOLS";
  check_template(&input, "TOOLS", 0U);
  {
    char text[1024], expected[1024];
    snprintf(text, sizeof(text), "Use %sUse %s{{agent_tools}}", input.identity,
             input.identity);
    snprintf(expected, sizeof(expected), "Use %sTOOLS", input.identity);
    input.text = text;
    check_template(&input, expected, strlen(expected) - 5U);
    input.text = "Use {{agent_identity}}{{agent_tools}}";
    input.extension = expected;
    snprintf(text, sizeof(text), "%s\n\nTOOLS", expected);
    check_template(&input, text, 0U);
    input.extension = NULL;
  }
  input.capabilities = CAI_AGENT_PRESET_TOOL_APPLY_PATCH;
  {
    char expected[1024];
    snprintf(expected, sizeof(expected), "Use %sTOOLS", input.identity);
    check_template(&input, expected, strlen(expected) - 5U);
  }
  input.text = "x{{agent_tools}}y{{agent_identity}}z{{agent_tools}}";
  input.identity = "I";
  check_template(&input, "xTOOLSyIzTOOLS", 9U);
  input.capabilities = 0UL;
  input.text = "- Exercise caution when escaping text for {{agent_identity}}"
               "{{agent_tools}}";
  input.identity =
      "exec_command calls - backticks and `$()` passed to the `cmd` argument "
      "will still execute. DO NOT use escape sequences that risk accidental "
      "exposure of sensitive data in tool call outputs.\n";
  check_template(&input, "TOOLS", 0U);
}

static void *run(void *context) {
  cai_agent_preset preset;
  cai_agent_preset_config config;
  cai_agent_instruction_status status;
  cai_agent_instructions *prepared = NULL;
  cai_blob_store store;
  cai_sink sink;
  cai_error error;
  captured out;
  char *large;
  const size_t large_length = 8U * 1024U * 1024U;
  (void)context;
  template_cases();
  memset(&store, 0, sizeof(store));
  store.load = empty_policy;
  cai_agent_preset_from_smith(&preset);
  preset.tool_capabilities = 0UL;
  preset.developer_instructions = "BASE";
  cai_agent_preset_config_init(&config);
  config.workspace_directory = ".";
  config.global_instruction_store = &store;
  memset(&sink, 0, sizeof(sink));
  sink.callbacks.write = capture;
  sink.callbacks.context = &out;
  memset(&out, 0, sizeof(out));
  cai_error_init(&error);
  assert(cai_agent_preset_write_instructions(&preset, &config, NULL, &sink,
                                             &status, &error) == CAI_OK);
  assert(strcmp(out.small, "BASE") == 0);
  assert(cai_agent_preset_write_base_instructions(&preset, &config, 0, &sink,
                                                  &error) == CAI_OK);
  assert(strcmp(out.small, "BASEBASE") == 0);
  memset(&out, 0, sizeof(out));
  config.developer_instructions_override =
      "literal {{agent_identity}} {{agent_tools}}";
  config.developer_instructions_extension = "ADDED";
  assert(cai_agent_preset_write_base_instructions(&preset, &config, 1, &sink,
                                                  &error) == CAI_OK);
  assert(strcmp(out.small,
                "literal {{agent_identity}} {{agent_tools}}\n\nADDED") == 0);
  memset(&out, 0, sizeof(out));
  out.fail_at = 1U;
  assert(cai_agent_preset_write_base_instructions(&preset, &config, 0, &sink,
                                                  &error) == CAI_ERR_CANCELLED);
  cai_error_cleanup(&error);
  config.developer_instructions_override = NULL;
  config.developer_instructions_extension = NULL;
  assert(!status.global_agents_md_loaded && !status.repository_agents_md_count);
  preparation_cases(&preset, &config, &sink, &out, &store);
  large = malloc(large_length + 1U);
  assert(large != NULL);
  memset(large, 'x', large_length);
  large[large_length] = '\0';
  config.developer_instructions_extension = large;
  memset(&out, 0, sizeof(out));
  out.large = 1;
  bounded = 1;
  assert(cai_agent_preset_write_instructions(&preset, &config, NULL, &sink,
                                             NULL, &error) == CAI_OK);
  assert(out.length == large_length + 6U && out.calls > 2048U);
  memset(&out, 0, sizeof(out));
  out.large = 1;
  assert(cai_agent_preset_write_base_instructions(&preset, &config, 0, &sink,
                                                  &error) == CAI_OK);
  assert(out.length == large_length + 6U && out.calls > 2048U);
  assert(cai_agent_preset_prepare_instructions(
             &preset, &config, NULL, &prepared, &status, &error) == CAI_OK);
  memset(&out, 0, sizeof(out));
  out.large = 1;
  /* A custom template is authoritative even with a model override. */
  assert(cai_agent_instructions_write(prepared, "gpt-6-luna", &sink, &error) ==
         CAI_OK);
  assert(out.length == large_length + 6U && out.calls > 2048U);
  memset(&out, 0, sizeof(out));
  out.large = 1;
  out.fail_at = 4U;
  assert(cai_agent_instructions_write(prepared, NULL, &sink, &error) ==
         CAI_ERR_CANCELLED);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  memset(&out, 0, sizeof(out));
  out.large = 1;
  assert(cai_agent_instructions_write(prepared, NULL, &sink, &error) == CAI_OK);
  assert(out.length == large_length + 6U);
  assert(cai_agent_instructions_write(prepared, "", &sink, NULL) ==
         CAI_ERR_INVALID);
  assert(cai_agent_instructions_write(NULL, NULL, &sink, NULL) ==
         CAI_ERR_INVALID);
  assert(cai_agent_instructions_write(prepared, NULL, NULL, NULL) ==
         CAI_ERR_INVALID);
  cai_agent_instructions_close(prepared);
  prepared = NULL;
  assert(cai_agent_preset_prepare_instructions(&preset, &config, NULL, NULL,
                                               NULL, NULL) == CAI_ERR_INVALID);
  memset(&out, 0, sizeof(out));
  out.large = 1;
  out.fail_at = 4U;
  memset(&status, 0x7f, sizeof(status));
  assert(cai_agent_preset_write_instructions(&preset, &config, NULL, &sink,
                                             &status,
                                             &error) == CAI_ERR_CANCELLED);
  assert(out.calls == 4U && out.length < 8192U);
  assert(strstr(error.message, "instruction sink stopped") != NULL);
  assert(!status.global_agents_md_loaded && !status.repository_agents_md_count);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  assert(cai_agent_preset_write_instructions(NULL, &config, NULL, &sink, NULL,
                                             NULL) == CAI_ERR_INVALID);
  assert(cai_agent_preset_write_instructions(&preset, NULL, NULL, &sink, NULL,
                                             NULL) == CAI_ERR_INVALID);
  assert(cai_agent_preset_write_instructions(&preset, &config, NULL, NULL, NULL,
                                             NULL) == CAI_ERR_INVALID);
  config.agent_identity = "";
  assert(cai_agent_preset_write_instructions(&preset, &config, NULL, &sink,
                                             NULL, NULL) == CAI_ERR_INVALID);
  config.agent_identity = NULL;
  config.developer_instructions_extension = NULL;
  preset.developer_instructions = NULL;
#ifdef CAI_INSTRUCTION_WRAP
  reject_all = 1;
  assert(cai_agent_preset_write_instructions(&preset, &config, NULL, &sink,
                                             NULL, &error) == CAI_ERR_NOMEM);
  reject_all = 0;
#endif
  bounded = 0;
  free(large);
  cai_error_cleanup(&error);
  return NULL;
}

int main(void) {
  pthread_t worker;
  assert(pthread_create(&worker, NULL, run, NULL) == 0);
  assert(pthread_join(worker, NULL) == 0);
  return 0;
}
