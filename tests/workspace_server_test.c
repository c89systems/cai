#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <cai/workspace.h>
#include <cai/workspace_server.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct test_client {
  cai_wire_peer *wire;
  cai_protocol_decoder *protocol;
  uint64_t attachment, generation, controller;
  size_t welcomed, roles, chunks, bytes;
  char workspace[128];
  unsigned char instance[CAI_WORKSPACE_INSTANCE_BYTES];
  size_t part, offset;
  int persist;
} test_client;
typedef struct host {
  cai_workspace_server *server;
  size_t attached, detached, errors, completed, tokens;
  uint64_t admitted;
  int reject;
} host;
static int observed(void *context, const cai_protocol_event *event,
                    cai_error *error) {
  test_client *client = context;
  (void)error;
  if (event->type == CAI_PROTOCOL_HEADER &&
      event->header->kind == CAI_PROTOCOL_WELCOME)
    client->part = client->offset = 0U;
  if (event->type == CAI_PROTOCOL_COMPLETE) {
    client->attachment = event->header->attachment;
    client->generation = event->header->generation;
    if (event->header->kind == CAI_PROTOCOL_WELCOME)
      ++client->welcomed;
    if (event->header->kind == CAI_PROTOCOL_ROLE)
      ++client->roles;
    if (event->header->kind == CAI_PROTOCOL_STREAM_CHUNK)
      ++client->chunks;
  }
  if (event->type == CAI_PROTOCOL_PAYLOAD) {
    if (event->header->kind == CAI_PROTOCOL_WELCOME) {
      const cai_cbor_token *value = event->token;
      if (value->type == CAI_CBOR_BYTES) {
        ++client->part;
        client->offset = 0U;
        assert(value->argument < sizeof(client->workspace));
        if (client->part == 2U)
          assert(value->argument == sizeof(client->instance));
      }
      if (value->type == CAI_CBOR_DATA) {
        void *out = client->part == 1U
                        ? (void *)(client->workspace + client->offset)
                        : (void *)(client->instance + client->offset);
        size_t limit = client->part == 1U ? sizeof(client->workspace) - 1U
                                          : sizeof(client->instance);
        assert(client->offset + value->length <= limit);
        memcpy(out, value->data, value->length);
        client->offset += value->length;
      }
      if (value->type == CAI_CBOR_BOOL)
        client->persist = (int)value->argument;
    }
    if (event->header->kind == CAI_PROTOCOL_ROLE) {
      assert(event->token->type == CAI_CBOR_UINT);
      client->controller = event->token->argument;
    }
    if (event->header->kind == CAI_PROTOCOL_STREAM_CHUNK &&
        event->token->type == CAI_CBOR_DATA)
      client->bytes += event->token->length;
  }
  return CAI_OK;
}
static int host_event(void *context, const cai_workspace_server_event *event,
                      cai_error *error) {
  host *state = context;
  (void)error;
  if (event->type == CAI_WORKSPACE_ATTACHED)
    ++state->attached;
  if (event->type == CAI_WORKSPACE_DETACHED)
    ++state->detached;
  if (event->type == CAI_WORKSPACE_PEER_ERROR) {
    assert(event->error != NULL && event->error->code != CAI_OK);
    ++state->errors;
  }
  if (event->type == CAI_WORKSPACE_MESSAGE) {
    assert(cai_workspace_server_disconnect(state->server, event->attachment,
                                           NULL) == CAI_ERR_INVALID);
    if (event->message->type == CAI_PROTOCOL_PAYLOAD)
      ++state->tokens;
    if (event->message->type == CAI_PROTOCOL_COMPLETE) {
      if (state->reject)
        return CAI_ERR_CANCELLED;
      ++state->completed;
      if (cai_workspace_server_authorize(state->server, event->attachment,
                                         event->message->header->generation,
                                         NULL) == CAI_OK)
        state->admitted = event->attachment;
    }
  }
  return CAI_OK;
}
static void token(test_client *client, cai_cbor_type type, uint64_t argument,
                  const void *bytes, size_t length) {
  cai_cbor_token value;
  memset(&value, 0, sizeof(value));
  value.type = type;
  value.argument = argument;
  value.data = bytes;
  value.length = length;
  assert(cai_wire_peer_write(client->wire, &value, NULL) == CAI_OK);
}
static void greeting_kind(test_client *client, const char *workspace,
                          const char *schema, cai_protocol_kind kind) {
  cai_protocol_header header;
  size_t n = strlen(workspace), s = strlen(schema);
  uint32_t length = 1U + (uint32_t)n +
                    (n < 24U    ? 1U
                     : n < 256U ? 2U
                                : 3U) +
                    (uint32_t)s + (s < 24U ? 1U : 2U);
  memset(&header, 0, sizeof(header));
  header.kind = kind;
  assert(cai_protocol_peer_begin(client->wire, &header, length, NULL) ==
         CAI_OK);
  token(client, CAI_CBOR_ARRAY, 2U, NULL, 0U);
  token(client, CAI_CBOR_BYTES, n, NULL, 0U);
  token(client, CAI_CBOR_DATA, 0U, workspace, n);
  token(client, CAI_CBOR_END, CAI_CBOR_BYTES, NULL, 0U);
  token(client, CAI_CBOR_TEXT, s, NULL, 0U);
  token(client, CAI_CBOR_DATA, 0U, schema, s);
  token(client, CAI_CBOR_END, CAI_CBOR_TEXT, NULL, 0U);
  token(client, CAI_CBOR_END, CAI_CBOR_ARRAY, NULL, 0U);
  assert(cai_protocol_peer_end(client->wire, NULL) == CAI_OK);
}
static void greeting(test_client *client, const char *workspace,
                     const char *schema) {
  greeting_kind(client, workspace, schema, CAI_PROTOCOL_HELLO);
}
static void connect_client(cai_workspace_endpoint *endpoint,
                           test_client *client) {
  int fd, connecting;
  memset(client, 0, sizeof(*client));
  assert(cai_workspace_endpoint_connect(endpoint, &fd, &connecting, NULL) ==
         CAI_OK);
  if (connecting)
    assert(cai_workspace_endpoint_connect_finish(fd, NULL) == CAI_OK);
  assert(cai_protocol_decoder_open(observed, client, &client->protocol, NULL) ==
         CAI_OK);
  assert(cai_wire_peer_open(fd, 0U, cai_protocol_decoder_event,
                            client->protocol, &client->wire, NULL) == CAI_OK);
}
static void close_client(test_client *client) {
  cai_wire_peer_close(client->wire);
  cai_protocol_decoder_close(client->protocol);
  memset(client, 0, sizeof(*client));
}
static void exchange(host *state, test_client *client, uint64_t now) {
  int eof;
  size_t i;
  for (i = 0U; i < 4U; ++i) {
    if (client != NULL)
      assert(cai_wire_peer_flush(client->wire, 0U, NULL) == CAI_OK);
    assert(cai_workspace_server_pump(state->server, now, NULL) == CAI_OK);
    if (client != NULL)
      assert(cai_wire_peer_read(client->wire, 0U, &eof, NULL) == CAI_OK);
  }
}
static cai_attachment_state snapshot(host *state) {
  cai_attachment_state value;
  assert(cai_workspace_server_get_state(state->server, &value, NULL) == CAI_OK);
  return value;
}
static void command(test_client *client, cai_protocol_kind kind,
                    uint64_t generation) {
  cai_protocol_header header;
  memset(&header, 0, sizeof(header));
  header.kind = kind;
  header.attachment = client->attachment;
  header.generation = generation;
  header.request = 1U;
  assert(cai_protocol_peer_begin(client->wire, &header, 1U, NULL) == CAI_OK);
  token(client, CAI_CBOR_NULL, 0U, NULL, 0U);
  assert(cai_protocol_peer_end(client->wire, NULL) == CAI_OK);
}
static void run(const char *runtime, const char *workspace, int persist) {
  cai_workspace_server_config config;
  cai_workspace_endpoint *endpoint;
  cai_workspace_server *duplicate = NULL;
  test_client first, second;
  host state;
  struct pollfd fds[CAI_WORKSPACE_SERVER_FDS];
  uint64_t deadline, old_generation;
  size_t count;
  memset(&state, 0, sizeof(state));
  cai_workspace_server_config_init(&config);
  config.workspace = workspace;
  config.runtime_directory = runtime;
  config.lifetime.persist = persist;
  config.event = host_event;
  config.context = &state;
  assert(cai_workspace_server_open(&config, &state.server, NULL) == CAI_OK);
  assert(cai_workspace_server_open(&config, &duplicate, NULL) == CAI_ERR_LIMIT);
  assert(duplicate == NULL);
  assert(cai_workspace_endpoint_open(runtime, workspace, &endpoint, NULL) ==
         CAI_OK);
  connect_client(endpoint, &first);
  greeting(&first, workspace, CAI_WORKSPACE_SCHEMA);
  while (cai_wire_peer_pending(first.wire) != 0U) {
    assert(cai_wire_peer_flush(first.wire, 1U, NULL) == CAI_OK);
    assert(cai_workspace_server_pump(state.server, 10U, NULL) == CAI_OK);
    assert(state.attached == 0U);
  }
  exchange(&state, &first, 10U);
  assert(first.welcomed == 0U && state.attached == 0U);
  assert(cai_workspace_server_ready(state.server, 100U, NULL) == CAI_OK);
  exchange(&state, &first, 100U);
  assert(first.welcomed == 1U && first.attachment != 0U &&
         first.controller == first.attachment);
  assert(strcmp(first.workspace, workspace) == 0 && first.persist == persist);
  assert(memcmp(first.instance, cai_workspace_server_instance(state.server),
                sizeof(first.instance)) == 0);
  old_generation = first.generation;
  connect_client(endpoint, &second);
  greeting(&second, workspace, CAI_WORKSPACE_SCHEMA);
  exchange(&state, &second, 101U);
  assert(second.welcomed == 1U && second.attachment != first.attachment &&
         second.controller == first.attachment);
  command(&second, CAI_PROTOCOL_REQUEST, second.generation);
  exchange(&state, &second, 102U);
  assert(state.completed == 1U && state.admitted == 0U);
  command(&second, CAI_PROTOCOL_TAKEOVER, second.generation);
  exchange(&state, &second, 103U);
  assert(second.controller == second.attachment &&
         second.generation > old_generation);
  command(&first, CAI_PROTOCOL_REQUEST, old_generation);
  exchange(&state, &first, 104U);
  assert(state.completed == 2U && state.admitted == 0U);
  command(&second, CAI_PROTOCOL_REQUEST, second.generation);
  exchange(&state, &second, 105U);
  assert(state.completed == 3U && state.admitted == second.attachment);
  assert(cai_workspace_server_prepare(state.server, fds,
                                      CAI_WORKSPACE_SERVER_FDS, &count,
                                      &deadline, NULL) == CAI_OK);
  assert(count == 3U && deadline == 0U);
  /* Observer EOF does not stop a server with a controller attached. */
  close_client(&first);
  exchange(&state, &second, 106U);
  assert(snapshot(&state).count == 1U);
  assert(snapshot(&state).phase == CAI_ATTACHMENT_READY);
  command(&second, CAI_PROTOCOL_DETACH, second.generation);
  assert(cai_wire_peer_flush(second.wire, 0U, NULL) == CAI_OK);
  exchange(&state, NULL, 107U);
  assert(snapshot(&state).count == 0U && state.detached == 2U);
  assert(snapshot(&state).phase ==
         (persist ? CAI_ATTACHMENT_READY : CAI_ATTACHMENT_STOPPING));
  close_client(&second);
  cai_workspace_endpoint_close(endpoint);
  cai_workspace_server_close(state.server);
}
static void failures(const char *runtime, const char *workspace) {
  cai_workspace_server_config config;
  cai_workspace_endpoint *endpoint;
  test_client clients[CAI_ATTACHMENT_LIMIT];
  host state;
  struct pollfd fds[CAI_WORKSPACE_SERVER_FDS];
  uint64_t deadline;
  size_t count, i;
  memset(&state, 0, sizeof(state));
  cai_workspace_server_config_init(&config);
  config.workspace = workspace;
  config.runtime_directory = runtime;
  config.lifetime.persist = 1;
  config.event = host_event;
  config.context = &state;
  assert(cai_workspace_server_open(&config, &state.server, NULL) == CAI_OK);
  assert(cai_workspace_server_ready(state.server, 0U, NULL) == CAI_OK);
  assert(cai_workspace_endpoint_open(runtime, workspace, &endpoint, NULL) ==
         CAI_OK);
  /* Invalid identity/schema never gains an attachment or changes lifetime. */
  for (i = 0U; i < 4U; ++i) {
    connect_client(endpoint, &clients[0]);
    greeting_kind(&clients[0], i % 2U == 0U ? "/wrong" : workspace,
                  i % 2U == 0U ? CAI_WORKSPACE_SCHEMA : "wrong",
                  i < 2U ? CAI_PROTOCOL_HELLO : CAI_PROTOCOL_PROBE);
    assert(cai_wire_peer_flush(clients[0].wire, 0U, NULL) == CAI_OK);
    exchange(&state, NULL, 1U);
    close_client(&clients[0]);
  }
  assert(state.errors == 4U && state.attached == 0U);
  /* Discovery is one-shot. Pipelined controls or another greeting never gain
   * authority, trigger application callbacks, or change server lifetime. */
  for (i = 0U; i < 2U; ++i) {
    connect_client(endpoint, &clients[0]);
    greeting_kind(&clients[0], workspace, CAI_WORKSPACE_SCHEMA,
                  CAI_PROTOCOL_PROBE);
    if (i == 0U)
      command(&clients[0], CAI_PROTOCOL_REQUEST, 0U);
    else
      greeting_kind(&clients[0], workspace, CAI_WORKSPACE_SCHEMA,
                    CAI_PROTOCOL_PROBE);
    assert(cai_wire_peer_flush(clients[0].wire, 0U, NULL) == CAI_OK);
    exchange(&state, NULL, 1U);
    close_client(&clients[0]);
  }
  assert(state.errors == 6U && state.attached == 0U && state.completed == 0U);
  /* Full pre-handshake slots must not spin on the readable listener. */
  for (i = 0U; i < CAI_ATTACHMENT_LIMIT; ++i) {
    connect_client(endpoint, &clients[i]);
    exchange(&state, NULL, 2U);
  }
  assert(cai_workspace_server_prepare(state.server, fds,
                                      CAI_WORKSPACE_SERVER_FDS, &count,
                                      &deadline, NULL) == CAI_OK);
  assert(count == CAI_ATTACHMENT_LIMIT && deadline == 10002U);
  exchange(&state, NULL, 10002U);
  assert(state.errors == 6U + CAI_ATTACHMENT_LIMIT);
  for (i = 0U; i < CAI_ATTACHMENT_LIMIT; ++i)
    close_client(&clients[i]);
  assert(snapshot(&state).phase == CAI_ATTACHMENT_READY);
  assert(cai_workspace_server_pump(state.server, 10001U, NULL) ==
         CAI_ERR_INVALID);
  cai_workspace_endpoint_close(endpoint);
  cai_workspace_server_close(state.server);
}
static void output_chunk(host *state, test_client *client, size_t length,
                         int expected) {
  cai_protocol_header header;
  cai_cbor_token value;
  unsigned char *bytes = malloc(length);
  int rc;
  assert(bytes != NULL);
  memset(bytes, 'a', length);
  memset(&header, 0, sizeof(header));
  header.kind = CAI_PROTOCOL_STREAM_CHUNK;
  header.stream = 42U;
  header.attachment =
      UINT64_MAX; /* Host overwrites untrusted producer values. */
  header.generation = UINT64_MAX;
  rc = cai_workspace_server_begin(state->server, client->attachment, &header,
                                  (uint32_t)length + 3U, NULL);
  memset(&value, 0, sizeof(value));
  value.type = CAI_CBOR_BYTES;
  value.argument = length;
  if (rc == CAI_OK)
    rc = cai_workspace_server_write(state->server, client->attachment, &value,
                                    NULL);
  value.type = CAI_CBOR_DATA;
  value.argument = 0U;
  value.data = bytes;
  value.length = length;
  if (rc == CAI_OK)
    rc = cai_workspace_server_write(state->server, client->attachment, &value,
                                    NULL);
  value.type = CAI_CBOR_END;
  value.argument = CAI_CBOR_BYTES;
  value.data = NULL;
  value.length = 0U;
  if (rc == CAI_OK)
    rc = cai_workspace_server_write(state->server, client->attachment, &value,
                                    NULL);
  if (rc == CAI_OK)
    rc = cai_workspace_server_end(state->server, client->attachment, NULL);
  free(bytes);
  assert(rc == expected);
}
static void transport_failures(const char *runtime, const char *workspace) {
  cai_workspace_server_config config;
  cai_workspace_endpoint *endpoint;
  cai_protocol_header header;
  test_client owner, observer;
  host state;
  size_t i, errors;
  memset(&state, 0, sizeof(state));
  cai_workspace_server_config_init(&config);
  config.workspace = workspace;
  config.runtime_directory = runtime;
  config.lifetime.persist = 1;
  config.event = host_event;
  config.context = &state;
  config.backlog_limit = 2048U;
  assert(cai_workspace_server_open(&config, &state.server, NULL) == CAI_OK);
  assert(cai_workspace_server_ready(state.server, 0U, NULL) == CAI_OK);
  assert(cai_workspace_endpoint_open(runtime, workspace, &endpoint, NULL) ==
         CAI_OK);
  connect_client(endpoint, &owner);
  greeting(&owner, workspace, CAI_WORKSPACE_SCHEMA);
  exchange(&state, &owner, 1U);
  for (i = 0U; i < 8U; ++i) {
    output_chunk(&state, &owner, 1024U, CAI_OK);
    exchange(&state, &owner, 2U);
    assert(owner.chunks == i + 1U && owner.bytes == (i + 1U) * 1024U);
  }
  assert(owner.attachment == snapshot(&state).controller);
  connect_client(endpoint, &observer);
  greeting(&observer, workspace, CAI_WORKSPACE_SCHEMA);
  exchange(&state, &observer, 3U);
  /* A slow subscriber fails independently; output never blocks its owner. */
  output_chunk(&state, &observer, 32768U, CAI_ERR_LIMIT);
  assert(state.errors == 1U);
  exchange(&state, &owner, 4U);
  close_client(&observer);
  assert(snapshot(&state).count == 1U);
  command(&owner, CAI_PROTOCOL_REQUEST, owner.generation);
  exchange(&state, &owner, 5U);
  assert(state.completed == 1U && state.admitted == owner.attachment);
  connect_client(endpoint, &observer);
  greeting(&observer, workspace, CAI_WORKSPACE_SCHEMA);
  exchange(&state, &observer, 6U);
  /* Matching metadata is not enough: a partial body must never be admitted. */
  memset(&header, 0, sizeof(header));
  header.kind = CAI_PROTOCOL_REQUEST;
  header.attachment = observer.attachment;
  header.generation = observer.generation;
  assert(cai_protocol_peer_begin(observer.wire, &header, 1U, NULL) == CAI_OK);
  assert(cai_wire_peer_flush(observer.wire, 0U, NULL) == CAI_OK);
  assert(shutdown(cai_wire_peer_fd(observer.wire), SHUT_WR) == 0);
  exchange(&state, &owner, 7U);
  assert(state.completed == 1U && state.errors == 2U);
  close_client(&observer);
  connect_client(endpoint, &observer);
  greeting(&observer, workspace, CAI_WORKSPACE_SCHEMA);
  exchange(&state, &observer, 8U);
  observer.attachment =
      owner.attachment; /* Cannot impersonate the controller. */
  command(&observer, CAI_PROTOCOL_REQUEST, owner.generation);
  assert(cai_wire_peer_flush(observer.wire, 0U, NULL) == CAI_OK);
  exchange(&state, &owner, 9U);
  assert(state.completed == 1U && state.errors == 3U);
  close_client(&observer);
  assert(snapshot(&state).count == 1U);
  connect_client(endpoint, &observer);
  greeting(&observer, workspace, CAI_WORKSPACE_SCHEMA);
  exchange(&state, &observer, 10U);
  command(&observer, CAI_PROTOCOL_SHUTDOWN, observer.generation);
  assert(cai_wire_peer_flush(observer.wire, 0U, NULL) == CAI_OK);
  exchange(&state, &owner, 11U);
  assert(state.errors == 4U && snapshot(&state).count == 1U &&
         snapshot(&state).phase == CAI_ATTACHMENT_READY);
  close_client(&observer);
  connect_client(endpoint, &observer);
  greeting(&observer, workspace, CAI_WORKSPACE_SCHEMA);
  exchange(&state, &observer, 12U);
  close_client(&owner);
  exchange(&state, &observer, 13U);
  assert(snapshot(&state).count == 1U && snapshot(&state).controller == 0U);
  assert(observer.controller == 0U); /* No implicit observer promotion. */
  command(&observer, CAI_PROTOCOL_REQUEST, observer.generation);
  exchange(&state, &observer, 14U);
  assert(state.completed == 2U && state.admitted != observer.attachment);
  command(&observer, CAI_PROTOCOL_TAKEOVER, observer.generation);
  exchange(&state, &observer, 15U);
  command(&observer, CAI_PROTOCOL_REQUEST, observer.generation);
  exchange(&state, &observer, 16U);
  assert(state.completed == 3U && state.admitted == observer.attachment);
  errors = state.errors;
  state.reject = 1;
  command(&observer, CAI_PROTOCOL_REQUEST, observer.generation);
  assert(cai_wire_peer_flush(observer.wire, 0U, NULL) == CAI_OK);
  exchange(&state, NULL, 17U);
  assert(state.errors == errors + 1U && snapshot(&state).count == 0U);
  close_client(&observer);
  state.reject = 0;
  connect_client(endpoint, &owner);
  greeting(&owner, workspace, CAI_WORKSPACE_SCHEMA);
  exchange(&state, &owner, 18U);
  command(&owner, CAI_PROTOCOL_SHUTDOWN, owner.generation);
  assert(cai_wire_peer_flush(owner.wire, 0U, NULL) == CAI_OK);
  exchange(&state, NULL, 19U);
  assert(snapshot(&state).stop_reason == CAI_ATTACHMENT_STOP_REQUESTED);
  close_client(&owner);
  cai_workspace_endpoint_close(endpoint);
  cai_workspace_server_close(state.server);
}
static void producer_backlog(const char *runtime, const char *workspace) {
  cai_workspace_server_config config;
  cai_workspace_endpoint *endpoint;
  host state;
  test_client a, b;
  cai_protocol_header header;
  size_t pending;
  memset(&state, 0, sizeof(state));
  cai_workspace_server_config_init(&config);
  config.runtime_directory = runtime;
  config.workspace = workspace;
  config.lifetime.persist = 1;
  config.event = host_event;
  config.context = &state;
  assert(cai_workspace_server_open(&config, &state.server, NULL) == CAI_OK);
  assert(cai_workspace_server_ready(state.server, 1U, NULL) == CAI_OK);
  assert(cai_workspace_endpoint_open(runtime, workspace, &endpoint, NULL) ==
         CAI_OK);
  connect_client(endpoint, &a);
  greeting(&a, workspace, CAI_WORKSPACE_SCHEMA);
  exchange(&state, &a, 2U);
  connect_client(endpoint, &b);
  greeting(&b, workspace, CAI_WORKSPACE_SCHEMA);
  exchange(&state, &b, 3U);
  assert(cai_workspace_server_pending(state.server, a.attachment, &pending,
                                      NULL) == CAI_OK &&
         pending == 0U);
  assert(cai_workspace_server_pending(state.server, 0U, &pending, NULL) ==
         CAI_ERR_INVALID);
  assert(cai_workspace_server_pending(state.server, a.attachment, NULL, NULL) ==
         CAI_ERR_INVALID);
  memset(&header, 0, sizeof(header));
  header.kind = CAI_PROTOCOL_STREAM_CHUNK;
  assert(cai_workspace_server_begin(state.server, a.attachment, &header, 4U,
                                    NULL) == CAI_OK);
  assert(cai_workspace_server_pending(state.server, a.attachment, &pending,
                                      NULL) == CAI_OK &&
         pending > 0U);
  /* Abort one unfinished subscriber frame outside dispatch. Authority must
   * already be revoked before the application next admits any work. */
  assert(cai_workspace_server_disconnect(state.server, a.attachment, NULL) ==
         CAI_OK);
  assert(snapshot(&state).count == 1U && snapshot(&state).controller == 0U &&
         snapshot(&state).phase == CAI_ATTACHMENT_READY);
  assert(cai_workspace_server_authorize(state.server, a.attachment,
                                        a.generation, NULL) == CAI_ERR_INVALID);
  assert(cai_workspace_server_pending(state.server, a.attachment, &pending,
                                      NULL) == CAI_ERR_INVALID);
  assert(cai_workspace_server_disconnect(state.server, a.attachment, NULL) ==
         CAI_ERR_INVALID);
  exchange(&state, &b, 4U);
  assert(b.controller == 0U && b.generation > a.generation);
  assert(cai_workspace_server_disconnect(state.server, b.attachment, NULL) ==
             CAI_OK &&
         snapshot(&state).count == 0U &&
         snapshot(&state).phase == CAI_ATTACHMENT_READY);
  close_client(&a);
  close_client(&b);
  cai_workspace_endpoint_close(endpoint);
  cai_workspace_server_close(state.server);
}
static void *worker(void *unused) {
  char root[] = "/tmp/cai-host-XXXXXX", runtime[128], workspace[128], path[256];
  DIR *dir;
  struct dirent *entry;
  (void)unused;
  assert(mkdtemp(root) != NULL);
  snprintf(runtime, sizeof(runtime), "%s/run", root);
  snprintf(workspace, sizeof(workspace), "%s/work", root);
  assert(mkdir(runtime, 0700) == 0 && mkdir(workspace, 0700) == 0);
  run(runtime, workspace, 0);
  run(runtime, workspace, 1);
  failures(runtime, workspace);
  transport_failures(runtime, workspace);
  producer_backlog(runtime, workspace);
  dir = opendir(runtime);
  assert(dir != NULL);
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    snprintf(path, sizeof(path), "%s/%s", runtime, entry->d_name);
    assert(unlink(path) == 0);
  }
  closedir(dir);
  assert(rmdir(runtime) == 0 && rmdir(workspace) == 0 && rmdir(root) == 0);
  return NULL;
}
int main(void) {
  pthread_t thread;
  assert(pthread_create(&thread, NULL, worker, NULL) == 0);
  assert(pthread_join(thread, NULL) == 0);
  return 0;
}
