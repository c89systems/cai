#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "../src/cai_internal.h"
#include <assert.h>
#include <cai/agent_runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct approval_fixture {
  char root[4096], repo[4096], child[4096], other[4096], global[4096];
  char path[4096], link[4096];
} approval_fixture;
static void path(char *out, size_t capacity, const char *base,
                 const char *leaf) {
  int n = snprintf(out, capacity, "%s/%s", base, leaf);
  assert(n > 0 && (size_t)n < capacity);
}
static void policy(approval_fixture *f, const char *directory,
                   const char *text) {
  FILE *file;
  path(f->path, sizeof(f->path), directory, "AGENTS.md");
  file = fopen(f->path, "w");
  assert(file != NULL && fputs(text, file) >= 0 && fclose(file) == 0);
}
static void agent_status(cai_client *client, const cai_smith_config *config,
                         size_t count, cai_error *error) {
  cai_agent *agent;
  cai_agent_instruction_status status;
  assert(cai_client_new_smith_agent(client, config, &agent, error) == CAI_OK);
  assert(cai_agent_get_instruction_status(agent, &status, error) == CAI_OK);
  assert(status.global_agents_md_loaded && status.codex_agents_md_loaded);
  assert(status.repository_agents_md_count == count);
  assert(strstr(CAI_AGENT_IMPL(agent)->developer_instructions,
                "GLOBAL_POLICY") != NULL);
  assert(strstr(CAI_AGENT_IMPL(agent)->developer_instructions,
                "UNAPPROVED_PARENT") == NULL);
  assert((strstr(CAI_AGENT_IMPL(agent)->developer_instructions,
                 "ROOT_POLICY") != NULL) == (count == 2U));
  assert((strstr(CAI_AGENT_IMPL(agent)->developer_instructions,
                 "CHILD_POLICY") != NULL) == (count != 0U));
  agent->close(agent);
}
int main(int argc, char **argv) {
  approval_fixture *f = calloc(1U, sizeof(*f));
  cai_client_config client_config;
  cai_smith_config smith;
  cai_agent_runtime_config runtime_config;
  cai_agent_runtime *runtime;
  cai_agent *agent;
  cai_agent_instruction_status status;
  cai_client *client;
  cai_error error;
  int mode;
  assert(f != NULL && argc == 2);
  path(f->root, sizeof(f->root), argv[1], "workspace-approval-XXXXXX");
  assert(mkdtemp(f->root) != NULL);
  path(f->repo, sizeof(f->repo), f->root, "repo");
  path(f->child, sizeof(f->child), f->repo, "child");
  path(f->other, sizeof(f->other), f->root, "repo2");
  path(f->global, sizeof(f->global), f->root, "global");
  assert(mkdir(f->repo, 0700) == 0 && mkdir(f->child, 0700) == 0 &&
         mkdir(f->other, 0700) == 0 && mkdir(f->global, 0700) == 0);
  path(f->path, sizeof(f->path), f->repo, ".git");
  assert(mkdir(f->path, 0700) == 0);
  policy(f, f->root, "UNAPPROVED_PARENT");
  policy(f, f->repo, "ROOT_POLICY");
  policy(f, f->child, "CHILD_POLICY");
  policy(f, f->global, "GLOBAL_POLICY");
  assert(setenv("CODEX_HOME", f->global, 1) == 0);
  cai_error_init(&error);
  cai_client_config_init(&client_config);
  client_config.api_key = "fixture";
  client_config.base_url = "http://127.0.0.1:1/v1";
  client_config.logger_disabled = 1;
  assert(cai_client_open(&client_config, &client, &error) == CAI_OK);
  cai_smith_config_init(&smith);
  smith.workspace_directory = f->child;
  smith.agent_config_directory = f->global;
  smith.codex_compat_agents_md = 1;
  smith.disable_terminal = 1;
  agent_status(client, &smith, 0U, &error);
  smith.workspace_instructions_root = f->child;
  agent_status(client, &smith, 1U, &error);
  smith.workspace_instructions_root = f->repo;
  agent_status(client, &smith, 2U, &error);
  smith.workspace_directory = f->other;
  assert(cai_client_new_smith_agent(client, &smith, &agent, &error) ==
         CAI_ERR_INVALID);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  path(f->link, sizeof(f->link), f->repo, "escape");
  assert(symlink(f->other, f->link) == 0);
  smith.workspace_directory = f->link;
  assert(cai_client_new_smith_agent(client, &smith, &agent, &error) ==
         CAI_ERR_INVALID);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  for (mode = 0; mode < 3; mode++) {
    cai_agent_runtime_config_init(&runtime_config);
    runtime_config.workspace_directory = f->child;
    runtime_config.agent_config_directory = f->global;
    runtime_config.codex_compat_agents_md = 1;
    runtime_config.disable_default_session_store = 1;
    runtime_config.disable_terminal = runtime_config.disable_web_search = 1;
    runtime_config.disable_review_subagent = runtime_config.disable_chrome = 1;
    runtime_config.disable_skills = runtime_config.logger_disabled = 1;
    runtime_config.insecure = mode == 1;
    runtime_config.workspace_approval_directory = mode == 2 ? f->repo : NULL;
    assert(cai_agent_runtime_open(client, &runtime_config, &runtime, &error) ==
           CAI_OK);
    assert(cai_agent_runtime_get_instruction_status(runtime, &status, &error) ==
           CAI_OK);
    assert(status.repository_agents_md_count == (size_t)mode);
    cai_agent_runtime_close(runtime);
  }
  cai_client_close(client);
  cai_error_cleanup(&error);
  assert(unlink(f->link) == 0);
  path(f->path, sizeof(f->path), f->repo, ".git");
  assert(rmdir(f->path) == 0);
  path(f->path, sizeof(f->path), f->child, "AGENTS.md");
  assert(unlink(f->path) == 0 && rmdir(f->child) == 0);
  path(f->path, sizeof(f->path), f->repo, "AGENTS.md");
  assert(unlink(f->path) == 0 && rmdir(f->repo) == 0 && rmdir(f->other) == 0);
  path(f->path, sizeof(f->path), f->global, "AGENTS.md");
  assert(unlink(f->path) == 0 && rmdir(f->global) == 0);
  path(f->path, sizeof(f->path), f->root, "AGENTS.md");
  assert(unlink(f->path) == 0 && rmdir(f->root) == 0);
  free(f);
  return 0;
}
