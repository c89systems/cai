#include "../cli/app_internal.h"
#include "../cli/config.h"
#include "../cli/goal_input.h"
#include "../cli/input_wire.h"
#include "../cli/startup.h"
#include "../cli/workspace_claims.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void copy_args(char **out, const char *const *source, size_t count) {
  size_t i;
  for (i = 0U; i < count; ++i) {
    out[i] = malloc(strlen(source[i]) + 1U);
    assert(out[i] != NULL);
    strcpy(out[i], source[i]);
  }
}
static void free_args(char **args, size_t count) {
  size_t i;
  for (i = 0U; i < count; ++i)
    free(args[i]);
}

static void expect(cai_cli_startup *cursor, const cai_cli_options *options,
                   const char *session, const char *expected, int generated) {
  cai_error error;
  char *text;
  int control;
  cai_error_init(&error);
  assert(cai_cli_startup_next(cursor, options, session, &text, &control,
                              &error) == CAI_OK);
  assert((expected == NULL && text == NULL) ||
         (expected != NULL && text != NULL && strcmp(text, expected) == 0));
  assert(control == generated);
  free(text);
  cai_error_cleanup(&error);
}

/** Every fixed owner setting must constrain attachment independently, including
 * fields beyond a machine-word boundary and both numeric storage types. */
static void owner_inventory(void) {
  static const struct {
    size_t offset;
    char type;
  } fields[] = {
#define FIELD(s, k, m, t) {offsetof(cai_cli_options, m), t},
      CAI_CLI_OWNER_FIELDS(FIELD)
#undef FIELD
  };
  cai_cli_workspace_claims owner = {0}, requested = {0};
  cai_cli_options defaults, selected;
  cai_config *cfg;
  size_t i, j;
  cai_cli_options_init(&defaults);
  defaults.no_config = 1;
  assert(cai_config_open(NULL, 0U, NULL, &cfg, NULL) == CAI_OK);
  assert(cai_cli_workspace_claims_build(&defaults, cfg, 0, &owner, NULL) ==
         CAI_OK);
  assert(owner.count >= CAI_CLI_OWNER_COUNT && owner.count > 64U);
  assert(cai_cli_workspace_claims_build(&defaults, cfg, 1, &requested, NULL) ==
         CAI_OK);
  assert(requested.count == 0U);
  for (i = 0U; i < CAI_CLI_OWNER_COUNT; ++i) {
    unsigned char *member;
    selected = defaults;
    cai_cli_options_mark_owner(&selected, fields[i].offset);
    assert(selected.owner_explicit[i]);
    assert(cai_cli_workspace_claims_build(&selected, cfg, 1, &requested,
                                          NULL) == CAI_OK);
    assert(requested.count == 1U);
    for (j = 0U; j < owner.count; ++j)
      if (!strcmp(owner.values[j].name, requested.values[0].name))
        break;
    assert(j < owner.count);
    assert(!memcmp(owner.values[j].digest, requested.values[0].digest,
                   CAI_WORKSPACE_CLAIM_DIGEST));
    /* Config origin is independently covered by the file/native-host probes. */
    if (i == CAI_CLI_OWNER_config_file)
      continue;
    member = (unsigned char *)&selected + fields[i].offset;
    if (fields[i].type == 's') {
      const char *value = "changed";
      memcpy(member, &value, sizeof(value));
    } else if (fields[i].type == 'z') {
      size_t value = (size_t)-1;
      memcpy(member, &value, sizeof(value));
    } else if (fields[i].type == 'l') {
      long value = 1L;
      memcpy(member, &value, sizeof(value));
    } else {
      int value;
      memcpy(&value, member, sizeof(value));
      value = !value;
      memcpy(member, &value, sizeof(value));
    }
    assert(cai_cli_workspace_claims_build(&selected, cfg, 1, &requested,
                                          NULL) == CAI_OK);
    assert(memcmp(owner.values[j].digest, requested.values[0].digest,
                  CAI_WORKSPACE_CLAIM_DIGEST));
  }
  cfg->close(cfg);
  cai_cli_workspace_claims_cleanup(&owner);
  cai_cli_workspace_claims_cleanup(&requested);
}
static const cai_workspace_claim *find_claim(const cai_cli_workspace_claims *c,
                                             const char *name) {
  size_t i;
  for (i = 0U; i < c->count; ++i)
    if (!strcmp(c->values[i].name, name))
      return c->values + i;
  return NULL;
}
static void supplemental_inventory(void) {
  cai_cli_workspace_claims owner = {0}, requested = {0};
  cai_cli_options options;
  cai_config *a, *b;
  const cai_workspace_claim *x, *y;
  const char *const names[] = {"client.timeout_ms", "client.ca_bundle_path",
                               "mcp.team.endpoint", "review.allowed_models",
                               "client_limits.max_spend_usd"};
  size_t i;
  cai_cli_options_init(&options);
  options.no_config = 1;
  assert(cai_cli_app_schema(&a, NULL) == 1 &&
         cai_cli_app_schema(&b, NULL) == 1);
  assert(cai_config_set(a, "client", "timeout_ms", "00042", NULL) == CAI_OK);
  assert(cai_config_set(a, "client", "ca_bundle_path", "/selected/ca.pem",
                        NULL) == CAI_OK);
  assert(cai_config_set(a, "mcp.team", "endpoint", "https://mcp.invalid",
                        NULL) == CAI_OK);
  assert(cai_config_set(a, "review", "allowed_models", "first", NULL) ==
         CAI_OK);
  assert(cai_config_set(a, "review", "allowed_models", "second", NULL) ==
         CAI_OK);
  assert(cai_config_set(a, "client_limits", "max_spend_usd", "1.50", NULL) ==
         CAI_OK);
  assert(cai_cli_workspace_claims_build(&options, a, 0, &owner, NULL) ==
         CAI_OK);
  /* Only the requested subset matters, including numeric equivalence. */
  assert(cai_config_set(b, "client", "timeout_ms", "42", NULL) == CAI_OK);
  assert(cai_config_set(b, "client", "ca_bundle_path", "/selected/ca.pem",
                        NULL) == CAI_OK);
  assert(cai_config_set(b, "mcp.team", "endpoint", "https://mcp.invalid",
                        NULL) == CAI_OK);
  assert(cai_config_set(b, "review", "allowed_models", "first", NULL) ==
         CAI_OK);
  assert(cai_config_set(b, "review", "allowed_models", "second", NULL) ==
         CAI_OK);
  assert(cai_config_set(b, "client_limits", "max_spend_usd", "1.5", NULL) ==
         CAI_OK);
  assert(cai_config_set(b, "ui", "theme", "dracula", NULL) == CAI_OK);
  assert(cai_config_set(b, "agent", "model", "another", NULL) == CAI_OK);
  assert(cai_cli_workspace_claims_build(&options, b, 1, &requested, NULL) ==
         CAI_OK);
  assert(requested.count == sizeof(names) / sizeof(names[0]));
  for (i = 0U; i < requested.count; ++i) {
    x = find_claim(&owner, names[i]);
    y = find_claim(&requested, names[i]);
    assert(x != NULL && y != NULL &&
           !memcmp(x->digest, y->digest, sizeof(x->digest)));
  }
  assert(cai_config_set(b, "client", "timeout_ms", "43", NULL) == CAI_OK);
  assert(cai_cli_workspace_claims_build(&options, b, 1, &requested, NULL) ==
         CAI_OK);
  x = find_claim(&owner, "client.timeout_ms");
  y = find_claim(&requested, "client.timeout_ms");
  assert(memcmp(x->digest, y->digest, sizeof(x->digest)));
  /* List order is semantic and named-section defaults are published. */
  assert(cai_config_set(b, "mcp.team", "timeout_ms", "30000", NULL) == CAI_OK);
  assert(cai_cli_workspace_claims_build(&options, b, 1, &requested, NULL) ==
         CAI_OK);
  x = find_claim(&owner, "mcp.team.timeout_ms");
  y = find_claim(&requested, "mcp.team.timeout_ms");
  assert(x != NULL && y != NULL &&
         !memcmp(x->digest, y->digest, sizeof(x->digest)));
  assert(cai_config_set(b, "review", "allowed_models", "third", NULL) ==
         CAI_OK);
  assert(cai_cli_workspace_claims_build(&options, b, 1, &requested, NULL) ==
         CAI_OK);
  x = find_claim(&owner, "review.allowed_models");
  y = find_claim(&requested, "review.allowed_models");
  assert(memcmp(x->digest, y->digest, sizeof(x->digest)));
  cai_cli_workspace_claims_cleanup(&owner);
  cai_cli_workspace_claims_cleanup(&requested);
  assert(owner.values == NULL && owner.count == 0U && owner.capacity == 0U);
  cai_config_close(a);
  cai_config_close(b);
}
static void inventory_limit(void) {
  cai_cli_workspace_claims claims = {0};
  cai_cli_options options;
  cai_config *cfg;
  char key[48];
  size_t i;
  cai_cli_options_init(&options);
  assert(cai_config_open(NULL, 0U, NULL, &cfg, NULL) == CAI_OK);
  for (i = 0U; i < CAI_WORKSPACE_CLAIM_LIMIT; ++i) {
    snprintf(key, sizeof(key), "CAI_LIMIT_%04u", (unsigned int)i);
    assert(cai_config_set(cfg, "env", key, "value", NULL) == CAI_OK);
  }
  assert(cai_cli_workspace_claims_build(&options, cfg, 1, &claims, NULL) ==
         CAI_OK);
  assert(claims.count == CAI_WORKSPACE_CLAIM_LIMIT);
  assert(cai_config_set(cfg, "env", "CAI_LIMIT_OVERFLOW", "value", NULL) ==
         CAI_OK);
  assert(cai_cli_workspace_claims_build(&options, cfg, 1, &claims, NULL) ==
         CAI_ERR_LIMIT);
  assert(claims.values == NULL && claims.count == 0U && claims.capacity == 0U);
  cai_cli_workspace_claims_cleanup(&claims);
  cai_config_close(cfg);
}

static void environment_inventory(void) {
  static const char *const entries[] = {"CAI_CLAIM_ENV=effective", NULL};
  cai_environment *environment;
  cai_config_context context;
  cai_config *a, *b;
  cai_cli_options options;
  cai_cli_workspace_claims owner = {0}, requested = {0};
  const cai_workspace_claim *x, *y;
  cai_cli_options_init(&options);
  options.no_config = 1;
  options.api_key_env = "CAI_CLAIM_ENV";
  options.owner_explicit[CAI_CLI_OWNER_api_key_env] = 1U;
  assert(cai_environment_open(entries, &environment, NULL) == CAI_OK);
  context.directory = NULL;
  context.environment = environment;
  assert(cai_config_open(NULL, 0U, &context, &a, NULL) == CAI_OK);
  assert(cai_config_open(NULL, 0U, &context, &b, NULL) == CAI_OK);
  cai_environment_close(environment);
  assert(cai_config_set(b, "env", "CAI_CLAIM_ENV", "ignored", NULL) == CAI_OK);
  assert(cai_cli_workspace_claims_build(&options, a, 0, &owner, NULL) ==
         CAI_OK);
  assert(cai_cli_workspace_claims_build(&options, b, 1, &requested, NULL) ==
         CAI_OK);
  x = find_claim(&owner, "env.CAI_CLAIM_ENV");
  y = find_claim(&requested, "env.CAI_CLAIM_ENV");
  assert(x != NULL && y != NULL &&
         !memcmp(x->digest, y->digest, sizeof(x->digest)));
  assert(cai_config_set(b, "", "config_env_has_precedence", "true", NULL) ==
         CAI_OK);
  assert(cai_cli_workspace_claims_build(&options, b, 1, &requested, NULL) ==
         CAI_OK);
  y = find_claim(&requested, "env.CAI_CLAIM_ENV");
  assert(y != NULL && memcmp(x->digest, y->digest, sizeof(x->digest)));
  x = find_claim(&owner, "client.api_key_env");
  y = find_claim(&requested, "client.api_key_env");
  assert(x != NULL && y != NULL &&
         memcmp(x->digest, y->digest, sizeof(x->digest)));
  cai_cli_workspace_claims_cleanup(&owner);
  cai_cli_workspace_claims_cleanup(&requested);
  cai_config_close(a);
  cai_config_close(b);
}
static void provider_ini_inventory(void) {
  cai_cli_options options;
  cai_config *cfg;
  cai_cli_workspace_claims first = {0}, second = {0};
  const cai_workspace_claim *x, *y;
  cai_cli_options_init(&options);
  options.no_config = 1;
  assert(options.endpoint == NULL && options.api_key_env == NULL);
  assert(cai_cli_app_schema(&cfg, NULL) == 1);
  assert(cai_cli_workspace_claims_build(&options, cfg, 0, &first, NULL) ==
         CAI_OK);
  assert(cai_config_set(cfg, "client", "base_url", "", NULL) == CAI_OK);
  options.owner_explicit[CAI_CLI_OWNER_endpoint] = 1U;
  assert(cai_cli_workspace_claims_build(&options, cfg, 1, &second, NULL) ==
         CAI_OK);
  x = find_claim(&first, "client.base_url");
  y = find_claim(&second, "client.base_url");
  assert(x != NULL && y != NULL &&
         !memcmp(x->digest, y->digest, sizeof(x->digest)));
  assert(cai_config_set(cfg, "client", "base_url", "https://first.invalid",
                        NULL) == CAI_OK);
  assert(cai_cli_workspace_claims_build(&options, cfg, 0, &first, NULL) ==
         CAI_OK);
  assert(cai_config_set(cfg, "client", "base_url", "https://second.invalid",
                        NULL) == CAI_OK);
  options.owner_explicit[CAI_CLI_OWNER_endpoint] = 1U;
  assert(cai_cli_workspace_claims_build(&options, cfg, 1, &second, NULL) ==
         CAI_OK);
  x = find_claim(&first, "client.base_url");
  y = find_claim(&second, "client.base_url");
  assert(x != NULL && y != NULL &&
         memcmp(x->digest, y->digest, sizeof(x->digest)));
  cai_cli_workspace_claims_cleanup(&first);
  cai_cli_workspace_claims_cleanup(&second);
  cai_config_close(cfg);
}
int main(void) {
  cai_cli_options options, defaults;
  cai_cli_startup cursor = {0};
  cai_error error;
  char *text, *large;
  int control;
  const char *input[] = {"cai",
                         "--resume",
                         "other",
                         "-m",
                         "selected",
                         "-r",
                         "low",
                         "--reasoning-summary",
                         "concise",
                         "--service-tier",
                         "flex",
                         "--yield-time-ms",
                         "17",
                         "--yolo",
                         "--reasoning-tool",
                         "--shell-context",
                         "--prompt-history-limit",
                         "3",
                         "--conversation-retention-days",
                         "0",
                         "--autoqueue",
                         "-i",
                         "--yolo",
                         "-i",
                         "actual prompt"};
  const char *disabled_input[] = {"cai",
                                  "--no-yolo",
                                  "--foreground",
                                  "--no-reasoning-tool",
                                  "--no-shell-context",
                                  "--prompt-history-limit",
                                  "0",
                                  "--conversation-retention-days",
                                  "30",
                                  "--no-autoqueue",
                                  "--service-tier",
                                  "default",
                                  "--reasoning-summary",
                                  "none"};
  const char *background_input[] = {"cai", "--foreground", "--background"};
  const char *no_foreground_input[] = {"cai", "--no-foreground"};
  const char *mapping_input[] = {"cai", "--normal-reasoning-effort", "low",
                                 "--high-reasoning-effort", "xhigh"};
  char *args[sizeof(input) / sizeof(input[0])];
  char *disabled[sizeof(disabled_input) / sizeof(disabled_input[0])];
  char *background[3], *no_foreground[2];
  char *mappings[5];
  owner_inventory();
  supplemental_inventory();
  inventory_limit();
  environment_inventory();
  provider_ini_inventory();
  copy_args(args, input, sizeof(args) / sizeof(args[0]));
  copy_args(disabled, disabled_input, sizeof(disabled) / sizeof(disabled[0]));
  copy_args(background, background_input, 3U);
  copy_args(no_foreground, no_foreground_input, 2U);
  copy_args(mappings, mapping_input, 5U);
  assert(cai_cli_parse_options((int)(sizeof(args) / sizeof(args[0])), args,
                               &options) == 1);
  expect(&cursor, &options, "current", "/resume other", 1);
  expect(&cursor, &options, "other", "/model selected", 1);
  expect(&cursor, &options, "other", "/reasoning low", 1);
  expect(&cursor, &options, "other", "/reasoning-summary concise", 1);
  expect(&cursor, &options, "other", "/tier flex", 1);
  expect(&cursor, &options, "other", "/yield_time_ms 17", 1);
  expect(&cursor, &options, "other", "/yolo on", 1);
  expect(&cursor, &options, "other", "/reasoning-tool on", 1);
  expect(&cursor, &options, "other", "/shell-context on", 1);
  expect(&cursor, &options, "other", "/history 3", 1);
  expect(&cursor, &options, "other", "/retention 0", 1);
  expect(&cursor, &options, "other", "/autoqueue on", 1);
  expect(&cursor, &options, "other", "--yolo", 0);
  expect(&cursor, &options, "other", "actual prompt", 0);
  expect(&cursor, &options, "other", NULL, 0);
  expect(&cursor, &options, "other", NULL, 0);

  assert(cai_cli_parse_options((int)(sizeof(disabled) / sizeof(disabled[0])),
                               disabled, &options) == 1);
  memset(&cursor, 0, sizeof(cursor));
  expect(&cursor, &options, "current", "/reasoning-summary none", 1);
  expect(&cursor, &options, "current", "/tier default", 1);
  expect(&cursor, &options, "current", "/yield_time_ms 0", 1);
  expect(&cursor, &options, "current", "/yolo off", 1);
  expect(&cursor, &options, "current", "/reasoning-tool off", 1);
  expect(&cursor, &options, "current", "/shell-context off", 1);
  expect(&cursor, &options, "current", "/history 0", 1);
  expect(&cursor, &options, "current", "/retention 30", 1);
  expect(&cursor, &options, "current", "/autoqueue off", 1);
  expect(&cursor, &options, "current", NULL, 0);

  /* Inherited configuration is not an instruction to reset an existing owner.
   * Only actual value-consuming argv branches may set the explicit mask. */
  cai_cli_options_init(&defaults);
  defaults.yolo = 1;
  defaults.foreground = 1;
  defaults.service_tier = "priority";
  defaults.reasoning_summary = "detailed";
  defaults.reasoning_tool = 1;
  assert(cai_cli_read_options(1, args, &defaults, &options) == 1);
  memset(&cursor, 0, sizeof(cursor));
  expect(&cursor, &options, "current", NULL, 0);
  assert(cai_cli_read_options(3, background, &defaults, &options) == 1);
  memset(&cursor, 0, sizeof(cursor));
  expect(&cursor, &options, "current", "/yield_time_ms 10000", 1);

  assert(cai_cli_parse_options(5, mappings, &options) == 1);
  memset(&cursor, 0, sizeof(cursor));
  expect(&cursor, &options, "current", "/reasoning-tool normal low", 1);
  expect(&cursor, &options, "current", "/reasoning-tool high xhigh", 1);
  expect(&cursor, &options, "current", NULL, 0);
  {
    const char *board_input[] = {"cai",     "--todo",        "--board",
                                 "a board", "--todo-layout", "list"};
    char *board_args[6];
    copy_args(board_args, board_input, 6U);
    assert(cai_cli_parse_options(6, board_args, &options) == 1);
    memset(&cursor, 0, sizeof(cursor));
    expect(&cursor, &options, "current", "/todo layout list", 1);
    expect(&cursor, &options, "current", "/todo new a board", 1);
    expect(&cursor, &options, "current", NULL, 0);
    /* Defaults do not describe an existing owner's tool policy. An explicit
     * disabled policy, however, retains --board's existing ignored behavior. */
    options.todo = 0;
    options.owner_explicit[CAI_CLI_OWNER_todo] = 0U;
    memset(&cursor, 0, sizeof(cursor));
    expect(&cursor, &options, "current", "/todo layout list", 1);
    expect(&cursor, &options, "current", "/todo new a board", 1);
    expect(&cursor, &options, "current", NULL, 0);
    options.owner_explicit[CAI_CLI_OWNER_todo] = 1U;
    memset(&cursor, 0, sizeof(cursor));
    expect(&cursor, &options, "current", "/todo layout list", 1);
    expect(&cursor, &options, "current", NULL, 0);
    free_args(board_args, 6U);
  }
  assert(cai_cli_read_options(2, no_foreground, &defaults, &options) == 1);
  memset(&cursor, 0, sizeof(cursor));
  expect(&cursor, &options, "current", "/yield_time_ms 10000", 1);

  cai_cli_options_init(&options);
  options.resume_id = "current";
  options.goal = "finish it";
  memset(&cursor, 0, sizeof(cursor));
  expect(&cursor, &options, "current", "/goal new finish it", 1);
  options.new_session = 1;
  memset(&cursor, 0, sizeof(cursor));
  expect(&cursor, &options, "current", "/new", 1);
  {
    cai_cli_goal_input decoded;
    const char *combined[] = {
        "cai", "-ng",  "goal with \"quotes\"\n€", "-i", "prompt \\u0000\n€",
        "-i",  "later"};
    size_t count = sizeof(combined) / sizeof(combined[0]);
    char *combined_args[7];
    copy_args(combined_args, combined, count);
    assert(cai_cli_parse_options((int)count, combined_args, &options) == 1);
    memset(&cursor, 0, sizeof(cursor));
    cai_error_init(&error);
    assert(cai_cli_startup_next(&cursor, &options, "current", &text, &control,
                                &error) == CAI_OK);
    assert(control && strncmp(text, "/goal start ", 12U) == 0);
    assert(cai_cli_goal_input_parse(text + 12U, &decoded, &error) == CAI_OK);
    assert(strcmp(decoded.objective, combined[2]) == 0 &&
           strcmp(decoded.prompt, combined[4]) == 0);
    cai_cli_goal_input_close(&decoded);
    free(text);
    expect(&cursor, &options, "current", "later", 0);
    expect(&cursor, &options, "current", NULL, 0);
    free_args(combined_args, count);
    {
      static const char *invalid[] = {
          "{}",
          "[]",
          "{\"objective\":\"a\"}",
          "{\"objective\":\"\",\"prompt\":\"b\"}",
          "{\"objective\":\"a\",\"prompt\":null}",
          "{\"objective\":\"a\",\"prompt\":\"x\\u0000suffix\"}",
          "{\"objective\":\"a\",\"objective\":\"b\",\"prompt\":\"c\"}",
          "{\"objective\":\"a\",\"prompt\":\"\\ud800\"}"};
      size_t at;
      for (at = 0U; at < sizeof(invalid) / sizeof(invalid[0]); ++at) {
        assert(cai_cli_goal_input_parse(invalid[at], &decoded, &error) !=
               CAI_OK);
        cai_cli_goal_input_close(&decoded);
        cai_error_cleanup(&error);
        cai_error_init(&error);
      }
    }
    cai_error_cleanup(&error);
  }

  cai_cli_options_init(&options);
  options.review_and_fix = 1;
  memset(&cursor, 0, sizeof(cursor));
  expect(&cursor, &options, "current", "/review-and-fix", 1);
  expect(&cursor, &options, "current", NULL, 0);
  options.review_uncommitted = 1;
  memset(&cursor, 0, sizeof(cursor));
  expect(&cursor, &options, "current", "/review-and-fix uncommitted", 1);
  options.review_uncommitted = 0;
  options.base = "HEAD";
  memset(&cursor, 0, sizeof(cursor));
  expect(&cursor, &options, "current", "/review-and-fix base HEAD", 1);
  options.review_and_fix = 0;
  options.base = NULL;
  options.review = 1;
  memset(&cursor, 0, sizeof(cursor));
  expect(&cursor, &options, "current", "/review", 1);
  expect(&cursor, &options, "current", NULL, 0);
  options.base = "HEAD";
  memset(&cursor, 0, sizeof(cursor));
  expect(&cursor, &options, "current", "/review base HEAD", 1);
  options.base = NULL;
  options.review_uncommitted = 1;
  memset(&cursor, 0, sizeof(cursor));
  expect(&cursor, &options, "current", "/review uncommitted", 1);
  {
    const char *review_input[] = {"cai", "--review", "-i", "Inspect only this"};
    char *review_args[4];
    copy_args(review_args, review_input, 4U);
    assert(cai_cli_parse_options(4, review_args, &options) == 1);
    memset(&cursor, 0, sizeof(cursor));
    expect(&cursor, &options, "current", "/review custom Inspect only this", 1);
    expect(&cursor, &options, "current", NULL, 0);
    free_args(review_args, 4U);
  }
  cai_cli_options_init(&options);

  /* No truncation or oversized allocation for generated controls. */
  large = malloc(CAI_CLI_INPUT_TEXT_MAX + 1U);
  assert(large != NULL);
  memset(large, 'x', CAI_CLI_INPUT_TEXT_MAX);
  large[CAI_CLI_INPUT_TEXT_MAX] = '\0';
  options.new_session = 0;
  options.goal = large;
  cai_error_init(&error);
  memset(&cursor, 0, sizeof(cursor));
  assert(cai_cli_startup_next(&cursor, &options, "current", &text, &control,
                              &error) == CAI_ERR_LIMIT);
  assert(text == NULL);
  cai_error_cleanup(&error);
  free(large);
  free_args(args, sizeof(args) / sizeof(args[0]));
  free_args(disabled, sizeof(disabled) / sizeof(disabled[0]));
  free_args(background, 3U);
  free_args(no_foreground, 2U);
  free_args(mappings, 5U);
  return 0;
}
