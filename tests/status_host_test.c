#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "../cli/workspace_host.h"
#include <assert.h>
#include <cai/app_client.h>
#include <dirent.h>
#include <lc/lc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
int __wrap_lc_client_open(const lc_client_config *, lc_client **, lc_error *);
int __wrap_lc_client_open(const lc_client_config *config, lc_client **out,
                          lc_error *error) {
  (void)config;
  (void)out;
  (void)error;
  abort(); /* Neither observers nor the metadata host may open SDK storage. */
}
typedef struct observer {
  cai_app_client *client;
  size_t statuses;
} observer;
static uint64_t now_ms(void) {
  struct timespec t;
  assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
  return (uint64_t)t.tv_sec * 1000U + (uint64_t)t.tv_nsec / 1000000U;
}
static int event(void *context, const cai_app_client_event *e,
                 cai_error *error) {
  observer *o = context;
  (void)error;
  if (e->type == CAI_APP_CLIENT_STATUS) {
    assert(e->input == NULL && e->status != NULL && !e->status->stale);
    ++o->statuses;
  }
  return CAI_OK;
}
static const cai_app_view *view(observer *o) {
  const cai_app_view *v = NULL;
  assert(cai_app_client_get_view(o->client, &v, NULL) == CAI_OK);
  return v;
}
static void exchange(cai_cli_workspace_host *host, observer *a, observer *b) {
  size_t i;
  int stopping;
  for (i = 0U; i < 16U; ++i) {
    assert(cai_cli_workspace_host_pump(host, &stopping, NULL) == CAI_OK &&
           !stopping);
    assert(cai_app_client_pump(a->client, now_ms(), NULL) == CAI_OK);
    if (b != NULL)
      assert(cai_app_client_pump(b->client, now_ms(), NULL) == CAI_OK);
  }
}
static void idle(cai_cli_workspace_host *host) {
  struct pollfd fds[CAI_CLI_WORKSPACE_HOST_FDS];
  size_t n;
  long timeout = -1L;
  assert(cai_cli_workspace_host_prepare(host, fds, CAI_CLI_WORKSPACE_HOST_FDS,
                                        &n, &timeout, NULL) == CAI_OK);
  assert(timeout == -1L); /* No elapsed timer or unchanged-view feedback. */
  assert(poll(fds, n, 0) == 0); /* No perpetually writable/ready interest. */
}
static void *run(void *context) {
  char temp[] = "./sv.XXXXXX";
  char *runtime, *workspace, *huge;
  cai_cli_workspace_host *host = NULL;
  cai_app_client_config config;
  cai_attachment_config lifetime;
  cai_app_view source;
  cai_app_client_state state;
  observer a, b;
  uint64_t revision, generation;
  size_t i, statuses;
  DIR *directory;
  struct dirent *entry;
  (void)context;
  assert(mkdtemp(temp) != NULL);
  runtime = realpath(temp, NULL);
  workspace = realpath(".", NULL);
  assert(runtime != NULL && workspace != NULL);
  cai_attachment_config_init(&lifetime);
  lifetime.persist = 1;
  assert(cai_cli_workspace_host_open(runtime, workspace, NULL, &lifetime, NULL,
                                     NULL, NULL, &host, NULL) == CAI_OK);
  memset(&source, 0, sizeof(source));
  source.available = 1;
  source.provider = "custom";
  source.effective.model = "first";
  source.metrics.turn_active = 1;
  source.metrics.turn_elapsed_ms = 123U;
  source.goal.has_goal = 1;
  source.goal.status = "active";
  source.goal.elapsed_seconds = 11LL;
  assert(cai_cli_workspace_host_status(host, &source, NULL) == CAI_OK);
  assert(cai_cli_workspace_host_ready(host, NULL) == CAI_OK);
  memset(&a, 0, sizeof(a));
  memset(&b, 0, sizeof(b));
  cai_app_client_config_init(&config);
  config.runtime_directory = runtime;
  config.workspace = workspace;
  config.event = event;
  config.context = &a;
  assert(cai_app_client_open(&config, now_ms(), &a.client, NULL) == CAI_OK);
  config.context = &b;
  assert(cai_app_client_open(&config, now_ms(), &b.client, NULL) == CAI_OK);
  exchange(host, &a, &b);
  assert(a.statuses == 1U && b.statuses == 1U);
  assert(view(&a)->revision == 1U && view(&b)->revision == 1U);
  assert(cai_app_client_get_state(b.client, &state, NULL) == CAI_OK &&
         !state.bound && state.producer == 0U &&
         state.transport.controller != state.transport.attachment);
  generation = state.transport.generation;
  idle(host);
  for (i = 0U; i < 500U; ++i) {
    ++source.metrics.turn_elapsed_ms;
    ++source.goal.elapsed_seconds;
    assert(cai_cli_workspace_host_status(host, &source, NULL) == CAI_OK);
    exchange(host, &a, &b);
  }
  assert(a.statuses == 1U && b.statuses == 1U);
  idle(host);
  /* Replace many unencoded views. Each observer receives only the newest. */
  source.effective.model = "second";
  assert(cai_cli_workspace_host_status(host, &source, NULL) == CAI_OK);
  source.effective.model = "third";
  source.quota.has_credit_balance = 1;
  source.quota.credit_balance = 58816.38;
  assert(cai_cli_workspace_host_status(host, &source, NULL) == CAI_OK);
  exchange(host, &a, &b);
  assert(a.statuses == 2U && b.statuses == 2U && view(&a)->revision == 3U &&
         view(&b)->quota.credit_balance == 58816.38 &&
         strcmp(view(&a)->effective.model, "third") == 0);
  /* Stop reading one real socket until its send window/backlog are blocked.
   * Healthy readers keep progressing; unsent views coalesce without forcing
   * an immediate idle pass. No partial encoded record is discarded. */
  huge = malloc(40001U);
  assert(huge != NULL);
  memset(huge, 'x', 40000U);
  huge[40000U] = '\0';
  source.effective.model = huge;
  statuses = b.statuses;
  for (i = 0U; i < 200U; ++i) {
    source.quota.credit_balance = (double)i;
    assert(cai_cli_workspace_host_status(host, &source, NULL) == CAI_OK);
    exchange(host, &a, NULL);
  }
  revision = view(&a)->revision;
  assert(view(&a)->quota.credit_balance == 199.0 &&
         view(&b)->revision < revision);
  idle(host);
  for (i = 0U; i < 100U && view(&b)->revision != revision; ++i)
    exchange(host, &a, &b);
  assert(view(&b)->revision == revision &&
         view(&b)->quota.credit_balance == 199.0 &&
         b.statuses - statuses < 200U);
  assert(cai_app_client_get_state(b.client, &state, NULL) == CAI_OK &&
         state.transport.phase == CAI_WORKSPACE_CONNECTED && !state.bound);
  idle(host);
  source.effective.model = "third";
  free(huge);
  /* Arbitrary finite values/strings remain literal. Oversized canonical
   * metadata invalidates just the status, then an ordinary update recovers. */
  huge = malloc(CAI_WIRE_MAX_RECORD + 1U);
  assert(huge != NULL);
  memset(huge, 'x', CAI_WIRE_MAX_RECORD);
  huge[CAI_WIRE_MAX_RECORD] = '\0';
  source.pending.model = huge;
  assert(cai_cli_workspace_host_status(host, &source, NULL) == CAI_OK);
  exchange(host, &a, &b);
  assert(!view(&a)->available && view(&a)->code != CAI_OK);
  revision = view(&a)->revision;
  assert(cai_app_client_get_state(a.client, &state, NULL) == CAI_OK &&
         state.transport.phase == CAI_WORKSPACE_CONNECTED && !state.bound);
  idle(host);
  /* Repeating the unrepresentable source does not flood unavailable views. */
  assert(cai_cli_workspace_host_status(host, &source, NULL) == CAI_OK);
  exchange(host, &a, &b);
  assert(view(&a)->revision == revision);
  free(huge);
  source.pending.model = "next\033[31m**literal**";
  source.pending.present = CAI_AGENT_RUNTIME_SETTING_MODEL;
  source.metrics.turn_active = 0;
  source.goal.status = "paused";
  assert(cai_cli_workspace_host_status(host, &source, NULL) == CAI_OK);
  exchange(host, &a, &b);
  assert(view(&a)->available && view(&a)->revision == revision + 1U &&
         strcmp(view(&b)->pending.model, source.pending.model) == 0 &&
         !view(&a)->metrics.turn_active);
  /* A status revision never changes authority. Explicit takeover still does. */
  statuses = b.statuses;
  assert(cai_app_client_takeover(b.client, 1U, generation, NULL) == CAI_OK);
  exchange(host, &a, &b);
  assert(cai_app_client_get_state(b.client, &state, NULL) == CAI_OK &&
         state.transport.generation == generation + 1U &&
         state.transport.controller == state.transport.attachment &&
         b.statuses == statuses);
  idle(host);
  cai_cli_workspace_host_close(host);
  assert(cai_app_client_pump(a.client, now_ms(), NULL) == CAI_OK);
  assert(view(&a)->stale && view(&a)->available);
  cai_app_client_close(a.client);
  cai_app_client_close(b.client);
  directory = opendir(runtime);
  assert(directory != NULL);
  while ((entry = readdir(directory)) != NULL)
    if (entry->d_name[0] != '.') {
      char file[512];
      assert(snprintf(file, sizeof(file), "%s/%s", runtime, entry->d_name) > 0);
      assert(unlink(file) == 0);
    }
  assert(closedir(directory) == 0 && rmdir(runtime) == 0);
  free(workspace);
  free(runtime);
  return NULL;
}
int main(void) {
  pthread_t worker;
  assert(pthread_create(&worker, NULL, run, NULL) == 0);
  assert(pthread_join(worker, NULL) == 0);
  return 0;
}
