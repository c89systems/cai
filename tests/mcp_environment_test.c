#define _POSIX_C_SOURCE 200809L
#include "../cli/mcp.h"
#include <assert.h>
#include <cai/environment.h>
#include <stdlib.h>
#include <string.h>

static const char *expected;
static size_t opens;
int __wrap_cai_mcp_streamable_http_client_open(
    const cai_mcp_streamable_http_client_config *, cai_mcp_client **,
    cai_error *);
int __wrap_cai_mcp_streamable_http_client_open(
    const cai_mcp_streamable_http_client_config *config, cai_mcp_client **out,
    cai_error *error) {
  assert(expected != NULL && config->api_key != NULL);
  assert(strcmp(config->api_key, expected) == 0);
  ++opens;
  *out = NULL;
  error->code = CAI_ERR_CANCELLED;
  error->message = strdup("test transport boundary");
  assert(error->message != NULL);
  return CAI_ERR_CANCELLED;
}
static cai_config *configuration(const char *directory, const char *captured,
                                 const char *configured, int override) {
  const char *entries[2];
  cai_environment *environment;
  cai_config_context context;
  cai_config *config;
  entries[0] = captured;
  entries[1] = NULL;
  assert(cai_environment_open(entries, &environment, NULL) == CAI_OK);
  context.directory = directory;
  context.environment = environment;
  assert(cai_config_open(NULL, 0U, &context, &config, NULL) == CAI_OK);
  cai_environment_close(environment);
  assert(cai_config_set(config, "mcp.fixture", "endpoint",
                        "http://127.0.0.1:1/mcp", NULL) == CAI_OK);
  assert(cai_config_set(config, "mcp.fixture", "api_key_env",
                        "CAI_TEST_MCP_SCOPE", NULL) == CAI_OK);
  if (configured != NULL)
    assert(cai_config_set(config, "env", "CAI_TEST_MCP_SCOPE", configured,
                          NULL) == CAI_OK);
  if (override)
    assert(cai_config_set(config, "", "config_env_has_precedence", "true",
                          NULL) == CAI_OK);
  return config;
}
static void refresh(cai_cli_mcp *mcp, const char *key) {
  size_t before = opens;
  expected = key;
  cai_cli_mcp_start(mcp);
  assert(mcp->client_count == 1U);
  /* The optional unavailable server contributes no tools. Inspect the actual
   * transport creation boundary, without network, inference or private state.
   */
  assert(cai_mcp_client_refresh_tools(mcp->clients[0], NULL) == CAI_OK);
  assert(opens == before + (key != NULL ? 1U : 0U));
  assert(cai_mcp_client_tool_count(mcp->clients[0]) == 0U);
}
int main(int argc, char **argv) {
  cai_config *first, *second;
  cai_cli_mcp a, b;
  const char *original = getenv("CAI_TEST_MCP_SCOPE");
  char *saved = original != NULL ? strdup(original) : NULL;
  assert(argc == 2 && (original == NULL || saved != NULL));
  first = configuration(argv[1], "CAI_TEST_MCP_SCOPE=first", "ignored", 0);
  second = configuration(argv[1], "CAI_TEST_MCP_SCOPE=second", "configured", 1);
  assert(cai_cli_mcp_open(&a, first, "cai", 1, NULL, NULL, NULL) == CAI_OK);
  assert(cai_cli_mcp_open(&b, second, "cai", 1, NULL, NULL, NULL) == CAI_OK);
  assert(setenv("CAI_TEST_MCP_SCOPE", "changed-process", 1) == 0);
  refresh(&b, "configured");
  refresh(&a, "first");
  cai_cli_mcp_close(&a);
  cai_cli_mcp_close(&b);
  assert(cai_config_set(second, "env", "CAI_TEST_MCP_SCOPE", "later", NULL) ==
         CAI_OK);
  /* A new context sees subsequent config changes; an old one never reads the
   * changed process environment. Missing/empty keys never open a transport. */
  assert(cai_cli_mcp_open(&a, second, "cai", 1, NULL, NULL, NULL) == CAI_OK);
  refresh(&a, "later");
  cai_cli_mcp_close(&a);
  cai_config_close(first);
  cai_config_close(second);
  first = configuration(argv[1], NULL, NULL, 0);
  assert(cai_cli_mcp_open(&a, first, "cai", 1, NULL, NULL, NULL) == CAI_OK);
  refresh(&a, NULL);
  cai_cli_mcp_close(&a);
  cai_config_close(first);
  first = configuration(argv[1], "CAI_TEST_MCP_SCOPE=", NULL, 0);
  assert(cai_cli_mcp_open(&a, first, "cai", 1, NULL, NULL, NULL) == CAI_OK);
  refresh(&a, NULL);
  cai_cli_mcp_close(&a);
  cai_config_close(first);
  assert((saved != NULL ? setenv("CAI_TEST_MCP_SCOPE", saved, 1)
                        : unsetenv("CAI_TEST_MCP_SCOPE")) == 0);
  free(saved);
  return 0;
}
