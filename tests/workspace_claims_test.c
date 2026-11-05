/** Startup compatibility must be decided before authority/lifetime changes. */
#define _GNU_SOURCE
#include "../src/cai_workspace_claims.h"
#include <assert.h>
#include <cai/workspace_client.h>
#include <cai/workspace_server.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static cai_workspace_client *client(const char *runtime, const char *workspace,
                                    const cai_workspace_claim *requirements,
                                    size_t n, int probe) {
  cai_workspace_client_config config;
  cai_workspace_client *out;
  cai_workspace_client_config_init(&config);
  config.runtime_directory = runtime;
  config.workspace = workspace;
  config.requirements = requirements;
  config.requirement_count = n;
  config.probe = probe;
  assert(cai_workspace_client_open(&config, 0U, &out, NULL) == CAI_OK);
  return out;
}
static void connect(cai_workspace_server *server, cai_workspace_client *c,
                    int reject, const char *field) {
  cai_workspace_client_state state;
  cai_error error;
  size_t i;
  int rc = CAI_OK;
  cai_error_init(&error);
  for (i = 0U; i < 100U; ++i) {
    assert(cai_workspace_server_pump(server, 0U, NULL) == CAI_OK);
    rc = cai_workspace_client_pump(c, 0U, &error);
    assert(cai_workspace_client_get_state(c, &state, NULL) == CAI_OK);
    if (rc != CAI_OK || state.phase == CAI_WORKSPACE_CONNECTED ||
        state.phase == CAI_WORKSPACE_IDENTIFIED)
      break;
  }
  assert(i < 100U);
  if (reject) {
    assert(rc == CAI_ERR_INVALID && state.phase == CAI_WORKSPACE_FAILED);
    assert(!state.has_welcome && !state.has_identity && !state.attachment &&
           !state.generation && strstr(error.message, field) != NULL);
  } else
    assert(rc == CAI_OK);
  cai_error_cleanup(&error);
}
static void unchanged(cai_workspace_server *server,
                      const cai_attachment_state *before) {
  cai_attachment_state after;
  assert(cai_workspace_server_get_state(server, &after, NULL) == CAI_OK);
  assert(after.count == before->count && after.phase == before->phase &&
         after.generation == before->generation &&
         after.controller == before->controller &&
         after.deadline_ms == before->deadline_ms);
}
static int feed(cai_workspace_claim_reader *r, cai_cbor_type type,
                uint64_t value, const void *p, size_t n) {
  cai_cbor_token t;
  memset(&t, 0, sizeof(t));
  t.type = type;
  t.argument = value;
  t.data = p;
  t.length = n;
  return cai_workspace_claim_read(r, &t);
}
static void entry(cai_workspace_claim_reader *r, const cai_workspace_claim *c) {
  size_t i;
  assert(feed(r, CAI_CBOR_ARRAY, 2U, NULL, 0U) == 0);
  assert(feed(r, CAI_CBOR_TEXT, strlen(c->name), NULL, 0U) == 0);
  for (i = 0U; c->name[i]; ++i)
    assert(feed(r, CAI_CBOR_DATA, 0U, c->name + i, 1U) == 0);
  assert(feed(r, CAI_CBOR_END, CAI_CBOR_TEXT, NULL, 0U) == 0);
  assert(feed(r, CAI_CBOR_BYTES, 32U, NULL, 0U) == 0);
  for (i = 0U; i < 32U; ++i)
    assert(feed(r, CAI_CBOR_DATA, 0U, c->digest + i, 1U) == 0);
  assert(feed(r, CAI_CBOR_END, CAI_CBOR_BYTES, NULL, 0U) == 0);
  assert(feed(r, CAI_CBOR_END, CAI_CBOR_ARRAY, NULL, 0U) == 0);
}
static void codec(void) {
  cai_workspace_claim_reader r;
  cai_workspace_claim c, *copy = NULL;
  memset(&c, 0, sizeof(c));
  c.name = "storage.sessions_store";
  memset(&r, 0, sizeof(r));
  r.claims = &c;
  r.count = 1U;
  assert(feed(&r, CAI_CBOR_ARRAY, 1U, NULL, 0U) == 0);
  entry(&r, &c);
  assert(!r.mismatch[0]);
  assert(feed(&r, CAI_CBOR_END, CAI_CBOR_ARRAY, NULL, 0U) == 1);
  assert(feed(&r, CAI_CBOR_END, CAI_CBOR_ARRAY, NULL, 0U) < 0);
  memset(&r, 0, sizeof(r));
  assert(feed(&r, CAI_CBOR_ARRAY, CAI_WORKSPACE_CLAIM_LIMIT + 1U, NULL, 0U) <
         0);
  assert(feed(&r, CAI_CBOR_ARRAY, 2U, NULL, 0U) == 0);
  entry(&r, &c);
  assert(strcmp(r.mismatch, c.name) == 0);
  assert(feed(&r, CAI_CBOR_ARRAY, 2U, NULL, 0U) == 0);
  assert(feed(&r, CAI_CBOR_TEXT, strlen(c.name), NULL, 0U) == 0);
  assert(feed(&r, CAI_CBOR_DATA, 0U, c.name, strlen(c.name)) == 0);
  assert(feed(&r, CAI_CBOR_END, CAI_CBOR_TEXT, NULL, 0U) < 0); /* duplicate */
  memset(&r, 0, sizeof(r));
  assert(feed(&r, CAI_CBOR_ARRAY, 1U, NULL, 0U) == 0);
  assert(feed(&r, CAI_CBOR_ARRAY, 2U, NULL, 0U) == 0);
  assert(feed(&r, CAI_CBOR_TEXT, 1U, NULL, 0U) == 0);
  assert(feed(&r, CAI_CBOR_DATA, 0U, "\033", 1U) < 0);
  assert(cai_workspace_claim_copy(&c, 1U, &copy, NULL) == CAI_OK);
  assert(copy != &c && copy->name != c.name && !strcmp(copy->name, c.name));
  cai_workspace_claim_free(copy, 1U);
  c.name = "bad\nname";
  assert(cai_workspace_claim_copy(&c, 1U, &copy, NULL) == CAI_ERR_INVALID);
  assert(copy == NULL);
}
int main(void) {
  char temporary[] = "cai-claims-XXXXXX", workspace[4096];
  char *root;
  cai_workspace_claim claims[CAI_WORKSPACE_CLAIM_LIMIT],
      requested[CAI_WORKSPACE_CLAIM_LIMIT];
  char names[CAI_WORKSPACE_CLAIM_LIMIT][32];
  cai_workspace_server_config config;
  cai_workspace_server *server;
  cai_workspace_client *c, *controller;
  cai_attachment_state before;
  DIR *directory;
  struct dirent *entry_file;
  size_t i;
  codec();
  assert(mkdtemp(temporary));
  root = realpath(temporary, NULL);
  assert(root);
  snprintf(workspace, sizeof(workspace), "%s/..", root);
  for (i = 0U; i < CAI_WORKSPACE_CLAIM_LIMIT; ++i) {
    snprintf(names[i], sizeof(names[i]), "setting.%03u", (unsigned int)i);
    claims[i].name = names[i];
    memset(claims[i].digest, (int)i, 32U);
  }
  cai_workspace_server_config_init(&config);
  config.runtime_directory = root;
  config.workspace = workspace;
  config.claims = claims;
  config.claim_count = CAI_WORKSPACE_CLAIM_LIMIT;
  assert(cai_workspace_server_open(&config, &server, NULL) == CAI_OK);
  memcpy(requested, claims, sizeof(claims));
  claims[0].digest[0] ^= 1U; /* Owner must retain its original copy. */
  assert(cai_workspace_server_get_state(server, &before, NULL) == CAI_OK);
  for (i = 0U; i < 2U; ++i) {
    requested[CAI_WORKSPACE_CLAIM_LIMIT - 1U].digest[31] ^= 1U;
    c = client(root, workspace, requested, CAI_WORKSPACE_CLAIM_LIMIT, (int)i);
    connect(server, c, 1, "setting.127");
    unchanged(server, &before);
    cai_workspace_client_close(c);
    requested[CAI_WORKSPACE_CLAIM_LIMIT - 1U].digest[31] ^= 1U;
  }
  requested[0].name = "missing";
  c = client(root, workspace, requested, 1U, 0);
  connect(server, c, 1, "missing");
  unchanged(server, &before);
  cai_workspace_client_close(c);
  requested[0].name = names[0];
  assert(cai_workspace_server_ready(server, 0U, NULL) == CAI_OK);
  assert(cai_workspace_server_get_state(server, &before, NULL) == CAI_OK);
  requested[0].digest[0] ^= 1U;
  c = client(root, workspace, requested, 1U, 0);
  connect(server, c, 1, names[0]);
  unchanged(server, &before); /* Nonpersistent first-attach grace unchanged. */
  cai_workspace_client_close(c);
  requested[0].digest[0] ^= 1U;
  controller = client(root, workspace, requested, CAI_WORKSPACE_CLAIM_LIMIT, 0);
  requested[0].digest[0] ^= 1U; /* Client must retain its original copy too. */
  connect(server, controller, 0, NULL);
  assert(cai_workspace_server_get_state(server, &before, NULL) == CAI_OK);
  assert(before.count == 1U && before.controller != 0U);
  c = client(root, workspace, requested, 1U, 0);
  connect(server, c, 1, names[0]);
  unchanged(server, &before);
  cai_workspace_client_close(c);
  cai_workspace_client_close(controller);
  cai_workspace_server_close(server);
  directory = opendir(root);
  assert(directory);
  while ((entry_file = readdir(directory)) != NULL)
    if (entry_file->d_name[0] != '.') {
      char path[4608];
      snprintf(path, sizeof(path), "%s/%s", root, entry_file->d_name);
      assert(unlink(path) == 0);
    }
  closedir(directory);
  assert(rmdir(root) == 0);
  free(root);
  return 0;
}
