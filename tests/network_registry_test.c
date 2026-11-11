/* Native storage and sockets; fault injection exercises ownership failure. */
#define _DEFAULT_SOURCE
#include "../cli/network_registry.c"
#include <assert.h>
#include <poll.h>
#include <sys/socket.h>
static int drop_newline;
int __real_cai_net_send(cai_net *, const void *, size_t, size_t *, cai_error *);
int __wrap_cai_net_send(cai_net *, const void *, size_t, size_t *, cai_error *);
int __wrap_cai_net_send(cai_net *net, const void *bytes, size_t count,
                        size_t *written, cai_error *error) {
  if (drop_newline && count == 1U && *(const char *)bytes == '\n') {
    *written = 0U;
    return CAI_OK; /* Same public result as a nonblocking EAGAIN. */
  }
  return __real_cai_net_send(net, bytes, count, written, error);
}
typedef struct capture {
  char bytes[16384];
  size_t count;
} capture;
static int collect(void *context, const void *bytes, size_t count,
                   cai_error *error) {
  capture *out = context;
  (void)error;
  assert(count < sizeof(out->bytes) - out->count);
  memcpy(out->bytes + out->count, bytes, count);
  out->count += count;
  out->bytes[out->count] = '\0';
  return CAI_OK;
}
static int fail_renew(lc_collection *c, long seconds,
                      lc_collection_lease_info *info, lc_error *error) {
  (void)c;
  (void)seconds;
  (void)info;
  return lc_error_set(error, LC_ERR_TRANSPORT, 0L,
                      "injected lease renewal failure", NULL, NULL, NULL);
}
typedef struct test_state {
  const char *root;
} test_state;
typedef struct allocation_race {
  cai_cli_pouch *pouch;
  const cai_cli_options *options;
  const char *instance;
  pthread_barrier_t *barrier;
  cai_cli_network_registry *registry;
  int result;
} allocation_race;
static void *race_open(void *context) {
  allocation_race *race = context;
  int rc = pthread_barrier_wait(race->barrier);
  assert(rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD);
  race->result =
      cai_cli_network_registry_open(race->pouch, race->options, "/network-race",
                                    race->instance, &race->registry, NULL);
  return NULL;
}
static void allocation_races(cai_cli_pouch *pouch, cai_cli_options *options) {
  allocation_race a, b;
  pthread_barrier_t barrier;
  pthread_t first, second;
  cai_cli_options explicit_options = *options;
  unsigned int round;
  explicit_options.event_multicast = "239.193.89.211:49189";
  for (round = 0U; round < 2U; round++) {
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.pouch = b.pouch = pouch;
    a.options = options;
    b.options = round == 0U ? options : &explicit_options;
    a.instance = "00000000000000000000000000000004";
    b.instance = "00000000000000000000000000000005";
    assert(pthread_barrier_init(&barrier, NULL, 2U) == 0);
    a.barrier = b.barrier = &barrier;
    assert(pthread_create(&first, NULL, race_open, &a) == 0);
    assert(pthread_create(&second, NULL, race_open, &b) == 0);
    assert(pthread_join(first, NULL) == 0 && pthread_join(second, NULL) == 0);
    assert(a.result == CAI_OK);
    if (round == 0U)
      assert(b.result == CAI_OK);
    if (b.result == CAI_OK)
      assert(strcmp(a.registry->destination, b.registry->destination));
    else
      assert(round == 1U && b.registry == NULL);
    cai_cli_network_registry_close(a.registry);
    cai_cli_network_registry_close(b.registry);
    assert(pthread_barrier_destroy(&barrier) == 0);
  }
}
static void *run(void *context) {
  test_state *test = context;
  cai_cli_pouch *pouch = calloc(1U, sizeof(*pouch));
  cai_cli_options options, automatic;
  cai_cli_network_registry *a = NULL, *b = NULL, *c = NULL;
  cai_error error;
  cai_net_config socket_config;
  cai_net *receiver = NULL;
  cai_agent_session_event event;
  cai_source *source = NULL;
  capture *expected = calloc(1U, sizeof(*expected)),
          *actual = calloc(1U, sizeof(*actual));
  cai_sink_callbacks callbacks;
  cai_sink *sink = NULL;
  int (*real_renew)(lc_collection *, long, lc_collection_lease_info *,
                    lc_error *);
  char *long_text = malloc(6001U);
  size_t i, got;
  int rc;
  assert(pouch && expected && actual && long_text);
  cai_error_init(&error);
  assert(cai_cli_pouch_open_at(pouch, test->root, NULL, NULL, NULL, &error) ==
         CAI_OK);
  cai_cli_options_init(&options);
  options.event_multicast = "239.193.89.211:49189";
  options.event_multicast_base = "239.193.89.211";
  options.event_multicast_count = 2;
  options.ip_multicast_if = "127.0.0.1";
  rc = cai_cli_network_registry_open(pouch, &options, "/network-a",
                                     "00000000000000000000000000000001", &a,
                                     &error);
  if (rc != CAI_OK)
    fprintf(stderr, "registry: %s %s\n", error.message,
            error.detail ? error.detail : "");
  assert(rc == CAI_OK);
  {
    lc_collection_info info;
    lc_error local;
    lc_error_init(&local);
    assert(a->owner->describe(a->owner, &info, &local) == LC_OK);
    assert(info.has_retention_expires_at_unix &&
           info.retention_expires_at_unix > info.lease.expires_at_unix);
    assert(a->allocation->describe(a->allocation, &info, &local) == LC_OK);
    assert(info.has_retention_expires_at_unix &&
           info.retention_expires_at_unix > info.lease.expires_at_unix);
    a->renew_at = 0U;
    assert(cai_cli_network_registry_renew(a, &error) == CAI_OK);
    lc_error_cleanup(&local);
  }
  automatic = options;
  automatic.event_multicast = "auto";
  rc = cai_cli_network_registry_open(pouch, &automatic, "/network-b",
                                     "00000000000000000000000000000002", &b,
                                     &error);
  if (rc != CAI_OK)
    fprintf(stderr, "registry b: %s\n", error.message);
  assert(rc == CAI_OK);
  assert(!strcmp(cai_cli_network_registry_publisher(b)->address,
                 "239.193.89.212:49189"));
  assert(cai_cli_network_registry_open(pouch, &automatic, "/network-c",
                                       "00000000000000000000000000000003", &c,
                                       &error) == CAI_ERR_LIMIT &&
         c == NULL);
  cai_error_cleanup(&error);
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.write = collect;
  callbacks.context = actual;
  assert(cai_sink_from_callbacks(&callbacks, &sink, &error) == CAI_OK);
  assert(cai_cli_network_registry_list(a, NULL, NULL, sink, &error) == CAI_OK);
  assert(strstr(actual->bytes, "/network-a") &&
         strstr(actual->bytes, "/network-b"));
  actual->count = 0U;
  actual->bytes[0] = '\0';
  cai_net_config_init(&socket_config);
  socket_config.mode = CAI_NET_MULTICAST_RECEIVE;
  socket_config.address = options.event_multicast;
  socket_config.interface_name = "127.0.0.1";
  assert(cai_net_open(&socket_config, &receiver, &error) == CAI_OK);
  for (i = 0U; i < 6000U; i++)
    long_text[i] = i % 71U ? 'a' : '\n';
  long_text[6000] = '\0';
  memset(&event, 0, sizeof(event));
  event.type = "assistant_text_delta";
  event.data = long_text;
  event.sequence = 7U;
  event.timestamp_ns = 123456U;
  event.utc_offset_seconds = 3600;
  assert(cai_session_event_source(&event, &source, &error) == CAI_OK);
  while ((got = cai_source_read(source, expected->bytes + expected->count,
                                sizeof(expected->bytes) - expected->count - 1U,
                                &error)) != 0U)
    expected->count += got;
  expected->bytes[expected->count++] = '\n';
  cai_source_close(source);
  /* Independent registries share one store; this direct observer invocation
   * exercises A's publication without stealing the store's installed hook. */
  pthread_mutex_lock(&pouch->journal_lock);
  observe(a, "session-a", 7U, &event);
  pthread_mutex_unlock(&pouch->journal_lock);
  while (actual->count < expected->count) {
    struct pollfd fd;
    ssize_t count;
    fd.fd = cai_net_fd(receiver);
    fd.events = POLLIN;
    assert(poll(&fd, 1U, 1000) == 1);
    count = recv(fd.fd, actual->bytes + actual->count,
                 sizeof(actual->bytes) - actual->count, 0);
    assert(count > 0 && count <= 1200);
    actual->count += (size_t)count;
  }
  assert(actual->count == expected->count &&
         !memcmp(actual->bytes, expected->bytes, expected->count));
  drop_newline = 1;
  pthread_mutex_lock(&pouch->journal_lock);
  observe(a, "session-a", 7U, &event);
  pthread_mutex_unlock(&pouch->journal_lock);
  assert(a->dropped == 1U);
  drop_newline = 0;
  cai_net_close(receiver, 0);
  real_renew = a->owner->keepalive;
  a->owner->keepalive = fail_renew;
  a->renew_at = 0U;
  assert(cai_cli_network_registry_renew(a, &error) != CAI_OK);
  assert(cai_cli_network_registry_publisher(a) == NULL);
  a->owner->keepalive = real_renew;
  cai_cli_network_registry_close(a);
  assert(cai_cli_network_registry_open(pouch, &automatic, "/network-c",
                                       "00000000000000000000000000000003", &c,
                                       &error) == CAI_OK);
  assert(!strcmp(cai_cli_network_registry_publisher(c)->address,
                 options.event_multicast));
  cai_cli_network_registry_close(c);
  cai_cli_network_registry_close(b);
  allocation_races(pouch, &automatic);
  cai_sink_close(sink);
  cai_cli_pouch_close(pouch);
  cai_error_cleanup(&error);
  free(pouch);
  free(expected);
  free(actual);
  free(long_text);
  return NULL;
}
int main(int argc, char **argv) {
  test_state test;
  pthread_t worker;
  char *root;
  assert(argc == 2);
  root = malloc(strlen(argv[1]) + 32U);
  assert(root != NULL);
  sprintf(root, "%s/network-registry.XXXXXX", argv[1]);
  assert(mkdtemp(root) != NULL);
  test.root = root;
  assert(pthread_create(&worker, NULL, run, &test) == 0);
  assert(pthread_join(worker, NULL) == 0);
  free(root);
  return 0;
}
