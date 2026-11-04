#ifndef CAI_CLI_MCP_H
#define CAI_CLI_MCP_H
#include <cai/config.h>
#include <cai/mcp.h>
#include <pslog.h>
#include <pthread.h>

typedef struct cai_cli_mcp_server cai_cli_mcp_server;
typedef struct cai_cli_mcp {
  cai_cli_mcp_server *servers;
  size_t count;
  cai_mcp_client **clients;
  size_t client_count;
  cai_agent_mcp_options *options;
  cai_environment *environment;
  pthread_mutex_t lock;
  int lock_initialized;
  int catalog_pending;
  int discovery_started;
} cai_cli_mcp;

/* Config/auth/logger outlive this collection, which outlives every borrowing
 * runtime. Initial registration uses an empty snapshot. Start discovery only
 * after the runtime worker is available; subsequent runtimes reuse the cache.
 * Configuration definitions remain unchanged while borrowed.
 * API-key discovery/refresh uses an owned immutable effective config
 * environment, never later process-global getenv values.
 */
int cai_cli_mcp_open(cai_cli_mcp *mcp, const cai_config *config,
                     const char *program_name, int enabled,
                     cai_chatgpt_auth *chatgpt_auth, pslog_logger *logger,
                     cai_error *error);
void cai_cli_mcp_close(cai_cli_mcp *mcp);
void cai_cli_mcp_start(cai_cli_mcp *mcp);
int cai_cli_mcp_ready(cai_cli_mcp *mcp);
void cai_cli_mcp_applied(cai_cli_mcp *mcp);
typedef int (*cai_cli_tools_write)(void *, const char *, size_t);
int cai_cli_mcp_command(cai_cli_mcp *mcp, const char *arguments,
                        cai_agent_runtime *runtime, cai_cli_tools_write write,
                        void *context, cai_error *error);
/* Detailed output is standalone; the summary appends rows to /status's table
 * before ending it and emitting the inventory hints. */
int cai_cli_tools_markdown(cai_cli_mcp *mcp, cai_agent_runtime *runtime,
                           int detailed, cai_cli_tools_write write,
                           void *context, cai_error *error);
#endif
