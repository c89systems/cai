/** Exercise frontend action correlation without opening outbound backend TCP.
 */
#define _POSIX_C_SOURCE 200809L
#include "../cli/network_host.c"
#include <assert.h>
#include <poll.h>
typedef struct fixture {
  cai_cli_network_host *host;
  cai_cli_network_scope scope;
  cai_app_network_event sent;
  pthread_t worker;
  cai_sink *sink;
  char output[4096];
  size_t length, sends, cancels;
  const char *arguments;
  int result;
} fixture;
static int get_scope(void *context, cai_cli_network_scope *scope,
                     cai_error *error) {
  (void)error;
  *scope = ((fixture *)context)->scope;
  return CAI_OK;
}
static int send_action(void *context, uint64_t id,
                       const cai_app_network_event *event, cai_error *error) {
  fixture *f = context;
  (void)error;
  assert(id == event->header.attachment);
  if (event->operation == CAI_APP_NETWORK_SEND) {
    f->sends++;
    f->sent = *event;
    assert(!strcmp(event->prompt, "hello") && event->timeout_ms == 10000U);
  } else if (event->operation == CAI_APP_NETWORK_CANCEL)
    f->cancels++;
  else
    assert(0);
  return CAI_OK;
}
static int capture(void *context, const void *bytes, size_t length,
                   cai_error *error) {
  fixture *f = context;
  (void)error;
  assert(length < sizeof(f->output) - f->length);
  memcpy(f->output + f->length, bytes, length);
  f->length += length;
  f->output[f->length] = '\0';
  return CAI_OK;
}
static void *call(void *context) {
  fixture *f = context;
  f->result = cai_cli_network_host_call(f->host, "prompt_send", f->arguments,
                                        f->sink, NULL);
  return NULL;
}
static void start(fixture *f) {
  struct pollfd fd;
  f->length = 0U;
  f->output[0] = '\0';
  assert(pthread_create(&f->worker, NULL, call, f) == 0);
  fd.fd = cai_cli_network_host_fd(f->host);
  fd.events = POLLIN;
  assert(poll(&fd, 1U, 1000) == 1);
  assert(cai_cli_network_host_pump(f->host, NULL) == CAI_OK);
}
static void join(fixture *f) { assert(pthread_join(f->worker, NULL) == 0); }
static void *run(void *root) {
  fixture *f = calloc(1U, sizeof(*f));
  cai_cli_pouch *pouch = calloc(1U, sizeof(*pouch));
  cai_cli_options options;
  cai_cli_network_host_config config;
  cai_app_network_event event;
  cai_sink_callbacks callbacks;
  unsigned int mode;
  assert(f != NULL && pouch != NULL);
  assert(cai_cli_pouch_open_at(pouch, root, NULL, NULL, NULL, NULL) == CAI_OK);
  cai_cli_options_init(&options);
  options.network_tools = 1;
  memset(&config, 0, sizeof(config));
  config.options = &options;
  config.pouch = pouch;
  config.instance = "11111111111111111111111111111111";
  config.workspace = "/fixture";
  config.scope = get_scope;
  config.send = send_action;
  config.context = f;
  f->scope.controller = 1U;
  f->scope.generation = 2U;
  f->scope.epoch = 3U;
  strcpy(f->scope.session, "session");
  assert(cai_cli_network_host_open(&config, &f->host, NULL) == CAI_OK);
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.write = capture;
  callbacks.context = f;
  assert(cai_sink_from_callbacks(&callbacks, &f->sink, NULL) == CAI_OK);
  f->arguments = "{\"address\":\"127.0.0.1:1234\",\"prompt\":\"hello\"}";
  /* A missing frontend fails before any action can be sent. */
  start(f);
  join(f);
  assert(f->result == CAI_ERR_INVALID && f->sends == 0U);
  memset(&event, 0, sizeof(event));
  event.operation = CAI_APP_NETWORK_CAPABILITY;
  event.header.attachment = 1U;
  event.header.generation = 2U;
  event.header.conversation = 3U;
  event.flags = CAI_APP_NETWORK_CAN_SEND;
  event.address = event.advertise = "";
  assert(cai_cli_network_host_receive(f->host, 1U, &event, NULL) == CAI_OK);
  for (mode = 0U; mode < 5U; mode++) {
    size_t before = f->sends;
    start(f);
    assert(f->sends == before + 1U && f->host->active != NULL);
    event = f->sent;
    event.operation = CAI_APP_NETWORK_RECEIPT;
    event.code = CAI_OK;
    event.transferred = 5U;
    event.reason = "locally transmitted";
    if (mode == 0U) {
      event.header.request++;
      assert(cai_cli_network_host_receive(f->host, 1U, &event, NULL) == CAI_OK);
      assert(f->host->active !=
             NULL); /* An unrelated receipt cannot settle it. */
      event.header.request--;
    } else if (mode == 1U)
      f->scope.generation++;
    else if (mode == 2U)
      f->host->active->deadline = 0U;
    else if (mode == 3U)
      cai_cli_network_host_detach(f->host, 1U);
    else
      cai_cli_network_host_cancel(f->host);
    if (mode <= 1U)
      assert(cai_cli_network_host_receive(f->host, 1U, &event, NULL) == CAI_OK);
    else if (mode != 4U)
      assert(cai_cli_network_host_pump(f->host, NULL) == CAI_OK);
    join(f);
    assert(f->result == CAI_OK);
    assert(strstr(f->output, mode == 0U ? "\"status\":\"transmitted\""
                                        : "\"uncertain\":true"));
    assert(f->host->active == NULL && f->sends == before + 1U);
    f->scope.generation = 2U;
    if (mode == 3U) {
      memset(&event, 0, sizeof(event));
      event.operation = CAI_APP_NETWORK_CAPABILITY;
      event.header.attachment = 1U;
      event.header.generation = 2U;
      event.header.conversation = 3U;
      event.flags = CAI_APP_NETWORK_CAN_SEND;
      event.address = event.advertise = "";
      assert(cai_cli_network_host_receive(f->host, 1U, &event, NULL) == CAI_OK);
    }
  }
  assert(f->cancels == 3U);
  cai_cli_network_host_stop(f->host);
  assert(cai_cli_network_host_call(f->host, "prompt_send", f->arguments,
                                   f->sink, NULL) == CAI_ERR_INVALID);
  cai_cli_network_host_close(f->host);
  cai_sink_close(f->sink);
  free(f);
  cai_cli_pouch_close(pouch);
  free(pouch);
  return NULL;
}
int main(int argc, char **argv) {
  pthread_t worker;
  char *root;
  assert(argc == 2);
  root = malloc(strlen(argv[1]) + 32U);
  assert(root != NULL);
  sprintf(root, "%s/network-host.XXXXXX", argv[1]);
  assert(mkdtemp(root) != NULL);
  assert(pthread_create(&worker, NULL, run, root) == 0);
  assert(pthread_join(worker, NULL) == 0);
  free(root);
  return 0;
}
