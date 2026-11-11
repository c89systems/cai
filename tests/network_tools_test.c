/** Exercise default capabilities, real multicast, UTF-8 fragments and
 * cancellation. */
#define _POSIX_C_SOURCE 200809L
#include "../src/cai_internal.h"
#include "../src/cai_network_jsonl.h"
#include <assert.h>
#include <cai/network.h>
#include <cai/network_tools.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct fixture {
  pthread_mutex_t lock;
  pthread_cond_t ready;
  int listening, calls;
  char output[4096];
  size_t length;
} fixture;
static fixture *current;
int __real_cai_net_open(const cai_net_config *, cai_net **, cai_error *);
int __wrap_cai_net_open(const cai_net_config *, cai_net **, cai_error *);
int __wrap_cai_net_open(const cai_net_config *config, cai_net **out,
                        cai_error *error) {
  int rc = __real_cai_net_open(config, out, error);
  if (rc == CAI_OK && config->mode == CAI_NET_MULTICAST_RECEIVE &&
      current != NULL) {
    pthread_mutex_lock(&current->lock);
    current->listening = 1;
    pthread_cond_broadcast(&current->ready);
    pthread_mutex_unlock(&current->lock);
  }
  return rc;
}
static int capture(void *context, const void *bytes, size_t count,
                   cai_error *error) {
  fixture *f = context;
  (void)error;
  assert(count < sizeof(f->output) - f->length);
  memcpy(f->output + f->length, bytes, count);
  f->length += count;
  f->output[f->length] = '\0';
  return CAI_OK;
}
static int host(void *context, const char *operation, const char *arguments,
                cai_sink *sink, cai_error *error) {
  fixture *f = context;
  assert(!strcmp(operation, "network_list"));
  assert(!strcmp(arguments, "{}"));
  f->calls++;
  return cai_sink_write(sink, "{}", 2U, error);
}
static void *sender(void *context) {
  fixture *f = context;
  cai_net_config config;
  cai_net *net = NULL;
  size_t sent;
  pthread_mutex_lock(&f->lock);
  while (!f->listening)
    pthread_cond_wait(&f->ready, &f->lock);
  pthread_mutex_unlock(&f->lock);
  cai_net_config_init(&config);
  config.mode = CAI_NET_MULTICAST_SEND;
  config.address = "239.193.89.229:49229";
  config.interface_name = "127.0.0.1";
  assert(cai_net_open(&config, &net, NULL) == CAI_OK);
  assert(cai_net_send(net, "hello \342", 7U, &sent, NULL) == CAI_OK &&
         sent == 7U);
  assert(cai_net_send(net, "\202\254\n", 3U, &sent, NULL) == CAI_OK &&
         sent == 3U);
  cai_net_close(net, 0);
  return NULL;
}
static int cancelled(void *context) {
  (void)context;
  return 1;
}
static void jsonl(void) {
  cai_network_jsonl *reader = NULL;
  cai_network_jsonl_stats stats;
  char prefix[] = "{\"record_type\":\"event\",\"sequence\":1,\"type\":"
                  "\"assistant_text_delta\",\"data\":\"";
  char suffix[] = "\"}\n", partial[] = "orphaned tail", newline[] = "\n";
  char chunk[1024];
  size_t i;
  assert(cai_network_jsonl_open(&reader, NULL) == CAI_OK);
  cai_network_jsonl_feed(reader, prefix, strlen(prefix));
  memset(chunk, 'x', sizeof(chunk));
  /* A 200 KiB record succeeds with only a 64 KiB parser budget and a 1 KiB
   * maximum spool. Unknown event bodies are skipped incrementally. */
  for (i = 0U; i < 200U; i++)
    cai_network_jsonl_feed(reader, chunk, sizeof(chunk));
  cai_network_jsonl_feed(reader, suffix, strlen(suffix));
  cai_network_jsonl_get(reader, &stats);
  assert(stats.events == 1U && stats.invalid_lines == 0U &&
         !stats.partial_line);
  cai_network_jsonl_feed(reader, partial, strlen(partial));
  cai_network_jsonl_get(reader, &stats);
  assert(stats.partial_line);
  cai_network_jsonl_feed(reader, newline, 1U);
  cai_network_jsonl_get(reader, &stats);
  assert(!stats.partial_line && stats.invalid_lines == 1U &&
         stats.events == 1U);
  cai_network_jsonl_close(reader);
}
static void *run(void *unused) {
  fixture *f = calloc(1U, sizeof(*f));
  cai_tool_registry *registry = NULL;
  cai_network_tools_config config;
  cai_sink_callbacks callbacks;
  cai_sink *sink = NULL;
  cai_error error;
  cai_stream_cancel_scope cancel;
  pthread_t thread;
  (void)unused;
  assert(f != NULL);
  jsonl();
  assert(pthread_mutex_init(&f->lock, NULL) == 0);
  assert(pthread_cond_init(&f->ready, NULL) == 0);
  current = f;
  cai_error_init(&error);
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.context = f;
  callbacks.write = capture;
  assert(cai_sink_from_callbacks(&callbacks, &sink, &error) == CAI_OK);
  assert(cai_tool_registry_new(&registry, &error) == CAI_OK);
  assert(cai_network_tools_register_registry(registry, NULL, &error) == CAI_OK);
  assert(cai_tool_registry_run(registry, "network_list", "{}", sink, &error) !=
         CAI_OK);
  cai_error_cleanup(&error);
  assert(cai_tool_registry_run(
             registry, "event_stream_listen",
             "{\"address\":\"239.193.89.229:49229\",\"duration_ms\":0}", sink,
             &error) == CAI_ERR_INVALID);
  cai_error_cleanup(&error);
  assert(pthread_create(&thread, NULL, sender, f) == 0);
  assert(cai_tool_registry_run(
             registry, "event_stream_listen",
             "{\"address\":\"239.193.89.229:49229\",\"interface\":\"127.0.0."
             "1\",\"duration_ms\":1000,\"max_bytes\":10}",
             sink, &error) == CAI_OK);
  assert(pthread_join(thread, NULL) == 0);
  assert(strstr(f->output, "hello \342\202\254\\n") &&
         strstr(f->output, "\"invalid_utf8\":false"));
  assert(strstr(f->output, "\"bytes\":10") &&
         strstr(f->output, "\"external_data\":true"));
  f->length = 0U;
  f->output[0] = '\0';
  /* Omitted max_bytes uses its real default; explicit zero remains invalid. */
  assert(cai_tool_registry_run(registry, "event_stream_listen",
                               "{\"address\":\"239.193.89.229:49229\","
                               "\"interface\":\"127.0.0.1\",\"duration_ms\":1}",
                               sink, &error) == CAI_OK);
  assert(strstr(f->output, "\"bytes\":0"));
  f->length = 0U;
  cancel.check = cancelled;
  cancel.context = NULL;
  assert(cai_stream_set_thread_cancel_scope(&cancel) == CAI_OK);
  assert(cai_tool_registry_run(
             registry, "event_stream_listen",
             "{\"address\":\"239.193.89.229:49229\",\"duration_ms\":60000}",
             sink, &error) == CAI_ERR_CANCELLED);
  assert(cai_stream_set_thread_cancel_scope(NULL) == CAI_OK);
  cai_error_cleanup(&error);
  cai_tool_registry_destroy(registry);
  assert(cai_tool_registry_new(&registry, &error) == CAI_OK);
  memset(&config, 0, sizeof(config));
  config.disable_event_stream_listener = 1;
  config.network_tools = 1;
  config.call = host;
  config.context = f;
  assert(cai_network_tools_register_registry(registry, &config, &error) ==
         CAI_OK);
  f->length = 0U;
  assert(cai_tool_registry_run(registry, "network_list", "{}", sink, &error) ==
             CAI_OK &&
         f->calls == 1);
  assert(cai_tool_registry_run(registry, "event_stream_listen", "{}", sink,
                               &error) != CAI_OK);
  cai_error_cleanup(&error);
  cai_tool_registry_destroy(registry);
  cai_sink_close(sink);
  current = NULL;
  pthread_cond_destroy(&f->ready);
  pthread_mutex_destroy(&f->lock);
  free(f);
  return NULL;
}
int main(void) {
  pthread_t worker;
  assert(pthread_create(&worker, NULL, run, NULL) == 0);
  assert(pthread_join(worker, NULL) == 0);
  return 0;
}
