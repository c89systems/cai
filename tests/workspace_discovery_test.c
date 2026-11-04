#define _GNU_SOURCE
#include <assert.h>
#include <cai/workspace.h>
#include <cai/workspace_discovery.h>
#include <cai/workspace_server.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef CAI_WRAP_READDIR
static int fail_directory;
struct dirent *__real_readdir(DIR *);
struct dirent *__wrap_readdir(DIR *);
struct dirent *__wrap_readdir(DIR *directory) {
  if (fail_directory) {
    fail_directory = 0;
    errno = EIO;
    return NULL;
  }
  return __real_readdir(directory);
}
#endif
typedef struct observer {
  cai_workspace_discovery *query;
  size_t found, errors;
  int reject, reentry;
} observer;
static int observed(void *context, const cai_workspace_discovery_event *event,
                    cai_error *error) {
  observer *out = context;
  (void)error;
  if (out->reentry) {
    assert(cai_workspace_discovery_pump(out->query, 0U, NULL) ==
           CAI_ERR_INVALID);
    cai_workspace_discovery_close(out->query); /* Reentrant close is ignored. */
  }
  if (event->error) {
    assert(event->error->code != CAI_OK);
    ++out->errors;
  } else {
    assert(event->entry && event->entry->record.workspace[0] == '/');
    assert(strlen(event->entry->record.id) == 32U);
    assert(event->entry->phase != CAI_ATTACHMENT_STOPPING);
    ++out->found;
  }
  return out->reject ? CAI_ERR_CANCELLED : CAI_OK;
}
static cai_workspace_discovery *open_query(const char *root,
                                           const char *selector,
                                           const char *base, observer *out) {
  cai_workspace_discovery_config config;
  cai_workspace_discovery_config_init(&config);
  config.runtime_directory = root;
  config.selector = selector;
  config.directory = base;
  config.probe_timeout_ms = 5U;
  config.event = observed;
  config.context = out;
  assert(cai_workspace_discovery_open(&config, 0U, &out->query, NULL) ==
         CAI_OK);
  return out->query;
}
static cai_workspace_discovery_state snapshot(cai_workspace_discovery *q) {
  cai_workspace_discovery_state state;
  assert(cai_workspace_discovery_get_state(q, &state, NULL) == CAI_OK);
  return state;
}
static void terminal(cai_workspace_discovery *q,
                     cai_workspace_discovery_phase phase) {
  struct pollfd fd;
  uint64_t deadline;
  assert(snapshot(q).phase == phase);
  assert(cai_workspace_discovery_prepare(q, &fd, &deadline, NULL) == CAI_OK);
  assert(fd.fd == -1 && fd.events == 0 && deadline == 0U);
  assert((cai_workspace_discovery_get_result(q) != NULL) ==
         (phase == CAI_WORKSPACE_FOUND));
}
static void finish(cai_workspace_discovery *q, cai_workspace_server *a,
                   cai_workspace_server *b, uint64_t now) {
  size_t i;
  for (i = 0U; i < 100U && snapshot(q).phase == CAI_WORKSPACE_DISCOVERING;
       ++i) {
    if (a)
      assert(cai_workspace_server_pump(a, now, NULL) == CAI_OK);
    if (b)
      assert(cai_workspace_server_pump(b, now, NULL) == CAI_OK);
    assert(cai_workspace_discovery_pump(q, now, NULL) == CAI_OK);
  }
  assert(snapshot(q).phase != CAI_WORKSPACE_DISCOVERING);
}
static cai_workspace_server *start(const char *root, const char *workspace,
                                   const char *name, int ready) {
  cai_workspace_server_config config;
  cai_workspace_server *server;
  cai_workspace_server_config_init(&config);
  config.runtime_directory = root;
  config.workspace = workspace;
  config.name = name;
  assert(cai_workspace_server_open(&config, &server, NULL) == CAI_OK);
  if (ready)
    assert(cai_workspace_server_ready(server, 0U, NULL) == CAI_OK);
  return server;
}
static void unchanged(cai_workspace_server *server,
                      const cai_attachment_state *before) {
  cai_attachment_state after;
  assert(cai_workspace_server_get_state(server, &after, NULL) == CAI_OK);
  assert(
      after.count == before->count && after.controller == before->controller &&
      after.generation == before->generation &&
      after.deadline_ms == before->deadline_ms &&
      after.phase == before->phase && after.stop_reason == before->stop_reason);
}
static void select_owner(const char *root, const char *selector,
                         const char *base, cai_workspace_server *a,
                         cai_workspace_server *b) {
  observer out;
  cai_workspace_discovery *q;
  const cai_workspace_discovery_result *result;
  cai_attachment_state before;
  memset(&out, 0, sizeof(out));
  assert(cai_workspace_server_get_state(a, &before, NULL) == CAI_OK);
  q = open_query(root, selector, base, &out);
  finish(q, a, b, 0U);
  terminal(q, CAI_WORKSPACE_FOUND);
  result = cai_workspace_discovery_get_result(q);
  assert(strcmp(result->record.workspace, cai_workspace_server_directory(a)) ==
         0);
  assert(memcmp(result->record.instance, cai_workspace_server_instance(a),
                16U) == 0);
  assert(result->phase == before.phase && !result->persist);
  assert(out.found == 1U && !out.errors);
  cai_workspace_discovery_close(q);
  unchanged(a, &before);
}
static void ordinary(const char *root, const char *a, const char *b) {
  cai_workspace_server *first = start(root, a, "alpha", 0);
  cai_workspace_server *second = start(root, b, "beta", 1);
  cai_workspace_endpoint *endpoint;
  observer out;
  cai_workspace_discovery *q;
  cai_workspace_client *client;
  cai_workspace_client_config client_config;
  cai_workspace_client_state state;
  cai_attachment_state before;
  char alias[256];
  size_t i;
  select_owner(root, "alpha", root, first, second);
  assert(cai_workspace_server_ready(first, 0U, NULL) == CAI_OK);
  assert(cai_workspace_endpoint_open(root, a, &endpoint, NULL) == CAI_OK);
  select_owner(root, cai_workspace_endpoint_id(endpoint), root, first, second);
  cai_workspace_endpoint_close(endpoint);
  select_owner(root, a, NULL, first, second);
  select_owner(root, "./a", root, first, second);
  snprintf(alias, sizeof(alias), "%s/link", root);
  assert(symlink(a, alias) == 0);
  select_owner(root, "./link", root, first, second);
  assert(unlink(alias) == 0);
  memset(&out, 0, sizeof(out));
  out.reentry = 1;
  q = open_query(root, NULL, NULL, &out);
  finish(q, first, second, 0U);
  terminal(q, CAI_WORKSPACE_LISTED);
  assert(out.found == 2U && snapshot(q).matches == 2U && !out.errors);
  cai_workspace_discovery_close(q);
  memset(&out, 0, sizeof(out));
  q = open_query(root, "a", root,
                 &out); /* Bare selectors never invent a cwd path. */
  finish(q, first, second, 0U);
  terminal(q, CAI_WORKSPACE_MISSING);
  assert(!out.found && !out.errors);
  cai_workspace_discovery_close(q);
  cai_workspace_client_config_init(&client_config);
  client_config.runtime_directory = root;
  client_config.workspace = a;
  assert(cai_workspace_client_open(&client_config, 0U, &client, NULL) ==
         CAI_OK);
  for (i = 0U; i < 8U; ++i) {
    assert(cai_workspace_client_pump(client, 0U, NULL) == CAI_OK);
    assert(cai_workspace_server_pump(first, 0U, NULL) == CAI_OK);
  }
  assert(cai_workspace_client_get_state(client, &state, NULL) == CAI_OK);
  assert(state.phase == CAI_WORKSPACE_CONNECTED &&
         state.controller == state.attachment);
  assert(cai_workspace_server_get_state(first, &before, NULL) == CAI_OK);
  select_owner(root, "alpha", root, first, second);
  unchanged(first, &before);
  cai_workspace_client_close(client);
  assert(cai_workspace_server_pump(first, 0U, NULL) == CAI_OK);
  cai_workspace_server_close(first);
  cai_workspace_server_close(second);
}
static void ambiguity(const char *root, const char *a, const char *b) {
  cai_workspace_endpoint *endpoint;
  cai_workspace_server *first, *second;
  cai_workspace_discovery *q;
  cai_workspace_discovery_state state;
  observer out;
  assert(cai_workspace_endpoint_open(root, b, &endpoint, NULL) == CAI_OK);
  first = start(root, a, cai_workspace_endpoint_id(endpoint), 1);
  second = start(root, b, "beta", 1);
  memset(&out, 0, sizeof(out));
  q = open_query(root, cai_workspace_endpoint_id(endpoint), root, &out);
  finish(q, first, second, 0U);
  terminal(q, CAI_WORKSPACE_AMBIGUOUS);
  assert(snapshot(q).matches == 2U && out.found == 2U);
  state = snapshot(q);
  assert(strlen(state.first) == 32U && strlen(state.second) == 32U);
  assert(strcmp(state.first, state.second) != 0);
  cai_workspace_discovery_close(q);
  cai_workspace_endpoint_close(endpoint);
  cai_workspace_server_close(first);
  cai_workspace_server_close(second);
}
static void errors(const char *root, const char *a) {
  static const char bad_name[] = "ws-00000000000000000000000000000000.info";
  cai_workspace_server *server = start(root, a, "alpha", 1);
  cai_workspace_discovery *q;
  observer out;
  char path[256];
  int fd;
  snprintf(path, sizeof(path), "%s/%s", root, bad_name);
  fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  assert(fd >= 0 && write(fd, "bad", 3U) == 3 && close(fd) == 0);
  memset(&out, 0, sizeof(out));
  q = open_query(root, NULL, NULL, &out);
  finish(q, server, NULL, 0U);
  terminal(q, CAI_WORKSPACE_LISTED);
  assert(out.found == 1U && out.errors == 1U);
  cai_workspace_discovery_close(q);
  assert(unlink(path) == 0);
  assert(symlink(a, path) == 0);
  memset(&out, 0, sizeof(out));
  q = open_query(root, "alpha", NULL, &out);
  finish(q, server, NULL, 0U);
  terminal(q, CAI_WORKSPACE_FOUND);
  assert(out.found == 1U && out.errors == 1U);
  cai_workspace_discovery_close(q);
  assert(unlink(path) == 0);
  memset(&out, 0, sizeof(out));
  out.reject = 1;
  q = open_query(root, "alpha", NULL, &out);
  while (snapshot(q).phase == CAI_WORKSPACE_DISCOVERING) {
    int rc = cai_workspace_discovery_pump(q, 0U, NULL);
    assert(rc == CAI_OK || rc == CAI_ERR_CANCELLED);
    assert(cai_workspace_server_pump(server, 0U, NULL) == CAI_OK);
  }
  terminal(q, CAI_WORKSPACE_DISCOVERY_FAILED);
  assert(snapshot(q).error_code == CAI_ERR_CANCELLED);
  cai_workspace_discovery_close(q);
#ifdef CAI_WRAP_READDIR
  memset(&out, 0, sizeof(out));
  q = open_query(root, NULL, NULL, &out);
  fail_directory = 1;
  assert(cai_workspace_discovery_pump(q, 0U, NULL) == CAI_ERR_TRANSPORT);
  terminal(q, CAI_WORKSPACE_DISCOVERY_FAILED);
  assert(cai_workspace_discovery_pump(q, 0U, NULL) == CAI_ERR_TRANSPORT);
  cai_workspace_discovery_close(q);
#endif
  cai_workspace_server_close(server);
}
static void interrupted(const char *root, const char *a) {
  cai_workspace_server *old = start(root, a, "alpha", 1), *current;
  cai_workspace_discovery *q;
  observer out;
  memset(&out, 0, sizeof(out));
  q = open_query(root, "alpha", NULL, &out);
  assert(cai_workspace_discovery_pump(q, 0U, NULL) == CAI_OK);
  cai_workspace_server_close(old);
  current = start(root, a, "alpha", 1);
  finish(q, current, NULL, 0U);
  terminal(q, CAI_WORKSPACE_UNAVAILABLE);
  assert(!out.found && out.errors == 1U);
  cai_workspace_discovery_close(q);
  select_owner(root, "alpha", root, current, NULL);
  cai_workspace_server_close(current);
}
static void timeout_and_cancel(const char *root, const char *a) {
  cai_workspace_endpoint *endpoint;
  cai_workspace_discovery *q;
  cai_workspace_registry *registry;
  const cai_workspace_record *record;
  unsigned char instance[16];
  observer out;
  struct pollfd fd;
  uint64_t deadline;
  memset(instance, 42, sizeof(instance));
  assert(cai_workspace_endpoint_open(root, a, &endpoint, NULL) == CAI_OK);
  assert(cai_workspace_endpoint_listen(endpoint, NULL) == CAI_OK);
  assert(cai_workspace_endpoint_publish(endpoint, "hung", instance, NULL) ==
         CAI_OK);
  memset(&out, 0, sizeof(out));
  q = open_query(root, "hung", NULL, &out);
  assert(cai_workspace_discovery_pump(q, 0U, NULL) == CAI_OK);
  assert(cai_workspace_discovery_pump(q, 0U, NULL) == CAI_OK);
  assert(cai_workspace_discovery_prepare(q, &fd, &deadline, NULL) == CAI_OK);
  assert(fd.fd >= 0 && deadline == 5U && !(fd.events & POLLOUT));
  finish(q, NULL, NULL, 5U);
  terminal(q, CAI_WORKSPACE_UNAVAILABLE);
  assert(out.errors == 1U && snapshot(q).error_code == CAI_ERR_LIMIT);
  cai_workspace_discovery_close(q);
  assert(cai_workspace_registry_open(root, &registry, NULL) == CAI_OK);
  assert(cai_workspace_registry_next(registry, &record, NULL) == CAI_OK &&
         record);
  cai_workspace_registry_close(registry);
  memset(&out, 0, sizeof(out));
  q = open_query(root, "hung", NULL, &out);
  assert(cai_workspace_discovery_pump(q, 0U, NULL) == CAI_OK);
  cai_workspace_discovery_close(
      q); /* Does not remove the live endpoint/hint. */
  assert(cai_workspace_endpoint_fd(endpoint) >= 0);
  cai_workspace_endpoint_close(endpoint);
}
static void stale_hint(const char *root, const char *a) {
  cai_workspace_server *server = start(root, a, "stale", 1);
  cai_workspace_endpoint *endpoint;
  cai_workspace_discovery *q;
  observer out;
  char path[256], backup[256], lock[256], chunk[512];
  int in, saved;
  ssize_t n;
  assert(cai_workspace_endpoint_open(root, a, &endpoint, NULL) == CAI_OK);
  snprintf(path, sizeof(path), "%s/ws-%s.info", root,
           cai_workspace_endpoint_id(endpoint));
  snprintf(lock, sizeof(lock), "%s/ws-%s.lock", root,
           cai_workspace_endpoint_id(endpoint));
  snprintf(backup, sizeof(backup), "%s/backup", root);
  cai_workspace_endpoint_close(endpoint);
  in = open(path, O_RDONLY);
  saved = open(backup, O_WRONLY | O_CREAT | O_EXCL, 0600);
  assert(in >= 0 && saved >= 0);
  while ((n = read(in, chunk, sizeof(chunk))) > 0)
    assert(write(saved, chunk, (size_t)n) == n);
  assert(n == 0 && close(in) == 0 && close(saved) == 0);
  cai_workspace_server_close(server);
  assert(rename(backup, path) == 0);
  memset(&out, 0, sizeof(out));
  q = open_query(root, "stale", NULL, &out);
  finish(q, NULL, NULL, 0U);
  terminal(q, CAI_WORKSPACE_MISSING);
  assert(!out.found && out.errors == 1U && access(path, F_OK) != 0);
  assert(access(lock, F_OK) == 0);
  cai_workspace_discovery_close(q);
}
static void *run(void *unused) {
  char root[] = "/tmp/cai-discovery-XXXXXX", a[256], b[256], path[512];
  cai_workspace_discovery_config config;
  cai_workspace_discovery *q;
  DIR *directory;
  struct dirent *entry;
  (void)unused;
  assert(mkdtemp(root));
  snprintf(a, sizeof(a), "%s/a", root);
  snprintf(b, sizeof(b), "%s/b", root);
  assert(mkdir(a, 0700) == 0 && mkdir(b, 0700) == 0);
  ordinary(root, a, b);
  ambiguity(root, a, b);
  errors(root, a);
  interrupted(root, a);
  timeout_and_cancel(root, a);
  stale_hint(root, a);
  cai_workspace_discovery_config_init(&config);
  config.runtime_directory = root;
  config.selector = "./a";
  assert(cai_workspace_discovery_open(&config, 0U, &q, NULL) ==
             CAI_ERR_INVALID &&
         !q);
  assert(cai_workspace_discovery_open(NULL, 0U, &q, NULL) == CAI_ERR_INVALID &&
         !q);
  assert(rmdir(a) == 0 && rmdir(b) == 0);
  directory = opendir(root);
  assert(directory);
  while ((entry = readdir(directory)) != NULL) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
      continue;
    snprintf(path, sizeof(path), "%s/%s", root, entry->d_name);
    assert(unlink(path) == 0);
  }
  closedir(directory);
  assert(rmdir(root) == 0);
  return NULL;
}
int main(void) {
  pthread_t worker;
  assert(pthread_create(&worker, NULL, run, NULL) == 0);
  assert(pthread_join(worker, NULL) == 0);
  return 0;
}
