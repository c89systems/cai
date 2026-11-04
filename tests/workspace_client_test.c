#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <cai/workspace.h>
#include <cai/workspace_client.h>
#include <cai/workspace_server.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct observer {
  size_t welcomes, roles, identities, headers, complete, bytes;
  int reject, fail_output;
  cai_workspace_client *client;
  const unsigned char *expected;
  uint64_t requests[16];
  size_t request_count;
} observer;
typedef struct admissions {
  cai_workspace_server *server;
  size_t completed, applied;
  uint64_t generation;
  size_t attached, detached;
} admissions;
static int admitted(void *context, const cai_workspace_server_event *event,
                    cai_error *error) {
  admissions *state = context;
  (void)error;
  if (event->type == CAI_WORKSPACE_ATTACHED)
    ++state->attached;
  if (event->type == CAI_WORKSPACE_DETACHED)
    ++state->detached;
  if (event->type == CAI_WORKSPACE_MESSAGE &&
      event->message->type == CAI_PROTOCOL_COMPLETE) {
    assert(event->attachment == event->message->header->attachment);
    ++state->completed;
    state->generation = event->message->header->generation;
    if (cai_workspace_server_authorize(state->server, event->attachment,
                                       state->generation, NULL) == CAI_OK)
      ++state->applied;
  }
  return CAI_OK;
}
static int observed(void *context, const cai_protocol_event *event,
                    cai_error *error) {
  observer *out = context;
  (void)error;
  if (out->reject)
    return CAI_ERR_CANCELLED;
  if (out->fail_output) {
    cai_protocol_header header;
    memset(&header, 0, sizeof(header));
    header.kind = CAI_PROTOCOL_REQUEST;
    assert(cai_workspace_client_begin(out->client, &header, 0U, 0, NULL) ==
           CAI_ERR_INVALID);
    return CAI_OK; /* A nested output failure is still terminal to pump. */
  }
  if (event->header->kind == CAI_PROTOCOL_WELCOME ||
      event->header->kind == CAI_PROTOCOL_ROLE ||
      event->header->kind == CAI_PROTOCOL_IDENTITY)
    assert(event->type == CAI_PROTOCOL_COMPLETE);
  if (event->type == CAI_PROTOCOL_HEADER)
    ++out->headers;
  if (event->type == CAI_PROTOCOL_PAYLOAD &&
      event->token->type == CAI_CBOR_DATA)
    out->bytes += event->token->length;
  if (event->type == CAI_PROTOCOL_COMPLETE) {
    if (event->header->kind == CAI_PROTOCOL_WELCOME)
      ++out->welcomes;
    else if (event->header->kind == CAI_PROTOCOL_IDENTITY)
      ++out->identities;
    else if (event->header->kind == CAI_PROTOCOL_ROLE) {
      ++out->roles;
      if (event->header->request != 0U) {
        assert(out->request_count < 16U);
        out->requests[out->request_count++] = event->header->request;
      }
    } else
      ++out->complete;
  }
  return CAI_OK;
}
static cai_workspace_client *open_client_mode(const char *runtime,
                                              const char *workspace,
                                              uint64_t timeout, observer *out,
                                              int probe) {
  cai_workspace_client_config config;
  cai_workspace_client *client = NULL;
  cai_workspace_client_config_init(&config);
  config.runtime_directory = runtime;
  config.workspace = workspace;
  config.handshake_timeout_ms = timeout;
  config.event = observed;
  config.context = out;
  config.probe = probe;
  config.expected_instance = out->expected;
  assert(cai_workspace_client_open(&config, 0U, &client, NULL) == CAI_OK);
  out->client = client;
  return client;
}
static cai_workspace_client *open_client(const char *runtime,
                                         const char *workspace,
                                         uint64_t timeout, observer *out) {
  return open_client_mode(runtime, workspace, timeout, out, 0);
}
static cai_workspace_client_state snapshot(cai_workspace_client *client) {
  cai_workspace_client_state state;
  assert(cai_workspace_client_get_state(client, &state, NULL) == CAI_OK);
  return state;
}
static void exchange(cai_workspace_server *server, cai_workspace_client *first,
                     cai_workspace_client *second, uint64_t now) {
  size_t i;
  for (i = 0U; i < 6U; ++i) {
    assert(cai_workspace_client_pump(first, now, NULL) == CAI_OK);
    if (second != NULL)
      assert(cai_workspace_client_pump(second, now, NULL) == CAI_OK);
    assert(cai_workspace_server_pump(server, now, NULL) == CAI_OK);
  }
}
static void request(cai_workspace_client *client, uint64_t generation,
                    int mutation, int expected) {
  cai_protocol_header header;
  cai_cbor_token value;
  int rc;
  memset(&header, 0, sizeof(header));
  header.kind = CAI_PROTOCOL_REQUEST;
  header.request = 7U;
  header.generation = generation;
  header.attachment = UINT64_MAX;
  rc = cai_workspace_client_begin(client, &header, 1U, mutation, NULL);
  assert(rc == expected);
  if (rc != CAI_OK)
    return;
  memset(&value, 0, sizeof(value));
  value.type = CAI_CBOR_NULL;
  assert(cai_workspace_client_write(client, &value, NULL) == CAI_OK);
  assert(cai_workspace_client_end(client, NULL) == CAI_OK);
}
static void probe(cai_workspace_server *server, const char *root, uint64_t now,
                  cai_attachment_phase phase, int persist) {
  observer out;
  cai_workspace_client *client;
  cai_workspace_client_state state;
  cai_attachment_state before, after;
  struct pollfd interest;
  uint64_t deadline;
  memset(&out, 0, sizeof(out));
  assert(cai_workspace_server_get_state(server, &before, NULL) == CAI_OK);
  client = open_client_mode(root, root, 1000000U, &out, 1);
  exchange(server, client, NULL, now);
  state = snapshot(client);
  assert(state.phase == CAI_WORKSPACE_IDENTIFIED && state.has_identity);
  assert(!state.has_welcome && !state.attachment && !state.generation &&
         !state.controller && !state.error_code);
  assert(state.persist == persist && state.server_phase == phase);
  assert(memcmp(state.instance, cai_workspace_server_instance(server),
                sizeof(state.instance)) == 0);
  assert(out.identities == 1U && !out.welcomes && !out.roles && !out.headers &&
         !out.bytes && !out.complete);
  assert(cai_workspace_server_get_state(server, &after, NULL) == CAI_OK);
  assert(after.phase == before.phase && after.count == before.count &&
         after.generation == before.generation &&
         after.controller == before.controller &&
         after.deadline_ms == before.deadline_ms &&
         after.stop_reason == before.stop_reason);
  assert(cai_workspace_client_prepare(client, &interest, &deadline, NULL) ==
         CAI_OK);
  assert(interest.fd == -1 && !interest.events && !deadline);
  request(client, 0U, 0, CAI_ERR_INVALID);
  assert(cai_workspace_client_takeover(client, 1U, 0U, NULL) ==
         CAI_ERR_INVALID);
  assert(cai_workspace_client_shutdown(client, 1U, 0U, NULL) ==
         CAI_ERR_INVALID);
  assert(cai_workspace_client_pump(client, now + 1U, NULL) == CAI_OK);
  assert(cai_workspace_client_detach(client, NULL) == CAI_OK);
  assert(snapshot(client).phase == CAI_WORKSPACE_IDENTIFIED);
  cai_workspace_client_close(client);
}
static void discovery(const char *root, int persist) {
  cai_workspace_server_config config;
  cai_workspace_server *server;
  cai_workspace_client *a, *b;
  cai_attachment_state state;
  admissions admission;
  observer oa, ob;
  memset(&admission, 0, sizeof(admission));
  memset(&oa, 0, sizeof(oa));
  memset(&ob, 0, sizeof(ob));
  cai_workspace_server_config_init(&config);
  config.runtime_directory = config.workspace = root;
  config.lifetime.persist = persist;
  config.lifetime.first_attachment_timeout_ms = 50U;
  config.event = admitted;
  config.context = &admission;
  assert(cai_workspace_server_open(&config, &server, NULL) == CAI_OK);
  admission.server = server;
  probe(server, root, 0U, CAI_ATTACHMENT_STARTING, persist);
  assert(cai_workspace_server_ready(server, 2U, NULL) == CAI_OK);
  probe(server, root, 3U, CAI_ATTACHMENT_READY, persist);
  assert(!admission.attached && !admission.detached && !admission.completed);
  a = open_client(root, root, 0U, &oa);
  b = open_client(root, root, 0U, &ob);
  exchange(server, a, b, 4U);
  assert(admission.attached == 2U);
  probe(server, root, 5U, CAI_ATTACHMENT_READY, persist);
  assert(cai_workspace_server_authorize(server, snapshot(a).attachment,
                                        snapshot(a).generation,
                                        NULL) == CAI_OK);
  assert(cai_workspace_server_authorize(server, snapshot(b).attachment,
                                        snapshot(b).generation,
                                        NULL) != CAI_OK);
  assert(admission.attached == 2U && !admission.detached);
  cai_workspace_client_close(a);
  cai_workspace_client_close(b);
  assert(cai_workspace_server_pump(server, 6U, NULL) == CAI_OK);
  assert(cai_workspace_server_get_state(server, &state, NULL) == CAI_OK);
  assert(!state.count && admission.detached == 2U);
  assert(state.phase ==
         (persist ? CAI_ATTACHMENT_READY : CAI_ATTACHMENT_STOPPING));
  cai_workspace_server_close(server);

  /* Probing READY must not consume or renew the first-attachment grace. */
  assert(cai_workspace_server_open(&config, &server, NULL) == CAI_OK);
  admission.server = server;
  assert(cai_workspace_server_ready(server, 0U, NULL) == CAI_OK);
  probe(server, root, 49U, CAI_ATTACHMENT_READY, persist);
  assert(cai_workspace_server_pump(server, 50U, NULL) == CAI_OK);
  assert(cai_workspace_server_get_state(server, &state, NULL) == CAI_OK);
  assert(state.phase ==
         (persist ? CAI_ATTACHMENT_READY : CAI_ATTACHMENT_STOPPING));
  assert(persist || state.stop_reason == CAI_ATTACHMENT_STARTUP_ABANDONED);
  cai_workspace_server_close(server);

  /* A connection accepted before STOPPING may still identify the owner;
   * stopping servers accept no new connections. No attach is admitted. */
  assert(cai_workspace_server_open(&config, &server, NULL) == CAI_OK);
  admission.server = server;
  memset(&oa, 0, sizeof(oa));
  a = open_client_mode(root, root, 0U, &oa, 1);
  assert(cai_workspace_server_pump(server, 0U, NULL) == CAI_OK);
  assert(cai_workspace_server_stop(server, NULL) == CAI_OK);
  exchange(server, a, NULL, 0U);
  assert(snapshot(a).phase == CAI_WORKSPACE_IDENTIFIED &&
         snapshot(a).server_phase == CAI_ATTACHMENT_STOPPING);
  cai_workspace_client_close(a);
  cai_workspace_server_close(server);
}
static void integration(const char *runtime, const char *workspace,
                        int persist) {
  cai_workspace_server_config config;
  cai_workspace_server *server;
  cai_workspace_client *a, *b;
  cai_workspace_client_state sa, sb;
  cai_attachment_state server_state;
  struct pollfd fd;
  uint64_t deadline, old;
  observer oa, ob;
  admissions admission;
  memset(&admission, 0, sizeof(admission));
  memset(&oa, 0, sizeof(oa));
  memset(&ob, 0, sizeof(ob));
  cai_workspace_server_config_init(&config);
  config.runtime_directory = runtime;
  config.workspace = workspace;
  config.lifetime.persist = persist;
  config.event = admitted;
  config.context = &admission;
  assert(cai_workspace_server_open(&config, &server, NULL) == CAI_OK);
  admission.server = server;
  a = open_client(runtime, workspace, 0U, &oa);
  request(a, 0U, 0, CAI_ERR_INVALID);
  exchange(server, a, NULL,
           100000U); /* Default does not time out slow restoration. */
  assert(!snapshot(a).has_welcome && oa.welcomes == 0U);
  assert(cai_workspace_server_ready(server, 100000U, NULL) == CAI_OK);
  exchange(server, a, NULL, 100000U);
  sa = snapshot(a);
  old = sa.generation;
  assert(sa.phase == CAI_WORKSPACE_CONNECTED && sa.attachment == sa.controller);
  assert(sa.persist == persist && oa.welcomes == 1U && oa.roles == 1U);
  assert(memcmp(sa.instance, cai_workspace_server_instance(server),
                sizeof(sa.instance)) == 0);
  assert(cai_workspace_client_prepare(a, &fd, &deadline, NULL) == CAI_OK);
  assert(fd.fd >= 0 && fd.events == POLLIN && deadline == 0U);
  b = open_client(runtime, workspace, 0U, &ob);
  exchange(server, a, b, 100001U);
  sb = snapshot(b);
  assert(memcmp(sa.instance, sb.instance, sizeof(sa.instance)) == 0);
  assert(sb.phase == CAI_WORKSPACE_CONNECTED && sb.controller == sa.attachment);
  {
    cai_protocol_header header;
    cai_cbor_token value;
    unsigned char bytes[256];
    size_t part;
    memset(bytes, 'o', sizeof(bytes));
    memset(&header, 0, sizeof(header));
    header.kind = CAI_PROTOCOL_STREAM_CHUNK;
    header.stream = 8U;
    assert(cai_workspace_server_begin(server, sa.attachment, &header, 8195U,
                                      NULL) == CAI_OK);
    memset(&value, 0, sizeof(value));
    value.type = CAI_CBOR_BYTES;
    value.argument = 8192U;
    assert(cai_workspace_server_write(server, sa.attachment, &value, NULL) ==
           CAI_OK);
    value.type = CAI_CBOR_DATA;
    value.argument = 0U;
    value.data = bytes;
    value.length = sizeof(bytes);
    assert(cai_workspace_server_write(server, sa.attachment, &value, NULL) ==
           CAI_OK);
    exchange(server, a, b, 100001U);
    assert(oa.bytes == sizeof(bytes) && oa.complete == 0U);
    for (part = 0U; part < 4U; ++part) {
      assert(cai_workspace_client_takeover(
                 a, 20U + part, snapshot(a).generation, NULL) == CAI_OK);
      exchange(server, a, b, 100001U);
    }
    assert(cai_workspace_client_takeover(b, 10U, sb.generation, NULL) ==
           CAI_OK);
    exchange(server, a, b, 100001U);
    for (part = 1U; part < 32U; ++part)
      assert(cai_workspace_server_write(server, sa.attachment, &value, NULL) ==
             CAI_OK);
    value.type = CAI_CBOR_END;
    value.argument = CAI_CBOR_BYTES;
    value.data = NULL;
    value.length = 0U;
    assert(cai_workspace_server_write(server, sa.attachment, &value, NULL) ==
           CAI_OK);
    assert(cai_workspace_server_end(server, sa.attachment, NULL) == CAI_OK);
    exchange(server, a, b, 100001U);
    assert(oa.complete == 1U && oa.bytes == 8192U);
    assert(oa.request_count == 4U);
    for (part = 0U; part < 4U; ++part)
      assert(oa.requests[part] == 20U + part);
    assert(snapshot(a).controller == sb.attachment);
    assert(cai_workspace_client_takeover(a, 11U, snapshot(a).generation,
                                         NULL) == CAI_OK);
    exchange(server, a, b, 100001U);
    sb = snapshot(b);
  }
  request(b, sb.generation, 1, CAI_ERR_INVALID);
  request(b, sb.generation, 0, CAI_OK);
  assert(cai_workspace_client_shutdown(b, 1U, sb.generation, NULL) ==
         CAI_ERR_INVALID);
  assert(cai_workspace_client_takeover(b, 2U, sb.generation, NULL) == CAI_OK);
  exchange(server, a, b, 100002U);
  assert(snapshot(a).controller == sb.attachment &&
         snapshot(b).controller == sb.attachment);
  assert(cai_workspace_client_takeover(a, 3U, snapshot(a).generation, NULL) ==
         CAI_OK);
  exchange(server, a, b, 100003U);
  assert(snapshot(a).controller == sa.attachment &&
         snapshot(a).generation > old);
  request(a, old, 1, CAI_ERR_INVALID); /* Losing then regaining control cannot
                                          revive old input. */
  request(a, snapshot(a).generation, 1, CAI_OK);
  assert(cai_workspace_client_takeover(a, 4U, old, NULL) == CAI_ERR_INVALID);
  exchange(server, a, b, 100004U);
  assert(admission.completed == 2U && admission.applied == 1U);
  {
    cai_protocol_header header;
    cai_cbor_token value;
    unsigned char bytes[256];
    size_t part;
    uint64_t original = snapshot(a).generation;
    memset(bytes, 'x', sizeof(bytes));
    memset(&header, 0, sizeof(header));
    header.kind = CAI_PROTOCOL_REQUEST;
    header.generation = original;
    assert(cai_workspace_client_begin(a, &header, 8195U, 1, NULL) == CAI_OK);
    memset(&value, 0, sizeof(value));
    value.type = CAI_CBOR_BYTES;
    value.argument = 8192U;
    assert(cai_workspace_client_write(a, &value, NULL) == CAI_OK);
    value.type = CAI_CBOR_DATA;
    value.argument = 0U;
    value.data = bytes;
    value.length = sizeof(bytes);
    assert(cai_workspace_client_write(a, &value, NULL) == CAI_OK);
    exchange(server, a, b, 100004U);
    assert(admission.completed == 2U); /* No admission of partial input. */
    assert(cai_workspace_client_takeover(b, 5U, snapshot(b).generation, NULL) ==
           CAI_OK);
    exchange(server, a, b, 100004U);
    for (part = 1U; part < 32U; ++part)
      assert(cai_workspace_client_write(a, &value, NULL) == CAI_OK);
    value.type = CAI_CBOR_END;
    value.argument = CAI_CBOR_BYTES;
    value.data = NULL;
    value.length = 0U;
    assert(cai_workspace_client_write(a, &value, NULL) == CAI_OK);
    assert(cai_workspace_client_end(a, NULL) == CAI_OK);
    exchange(server, a, b, 100004U);
    assert(admission.completed == 3U && admission.applied == 1U);
    assert(admission.generation == original); /* No rewriting at flush/end. */
    assert(cai_workspace_client_takeover(a, 6U, snapshot(a).generation, NULL) ==
           CAI_OK);
    exchange(server, a, b, 100004U);
  }
  assert(cai_workspace_client_detach(a, NULL) == CAI_OK);
  exchange(server, a, b, 100005U);
  assert(snapshot(a).phase == CAI_WORKSPACE_DISCONNECTED);
  assert(snapshot(b).controller == 0U &&
         snapshot(b).phase == CAI_WORKSPACE_CONNECTED);
  assert(cai_workspace_client_detach(b, NULL) == CAI_OK);
  exchange(server, a, b, 100006U);
  assert(snapshot(b).phase == CAI_WORKSPACE_DISCONNECTED);
  assert(cai_workspace_server_get_state(server, &server_state, NULL) == CAI_OK);
  assert(server_state.count == 0U &&
         server_state.phase ==
             (persist ? CAI_ATTACHMENT_READY : CAI_ATTACHMENT_STOPPING));
  assert(cai_workspace_client_prepare(b, &fd, &deadline, NULL) == CAI_OK &&
         fd.fd == -1);
  cai_workspace_client_close(a);
  cai_workspace_client_close(b);
  cai_workspace_server_close(server);
}
static void guarded_rejection(cai_workspace_server *server, const char *root,
                              const unsigned char *expected, int probe_mode,
                              uint64_t now) {
  cai_attachment_state before, after;
  observer out;
  cai_workspace_client *client;
  unsigned char input[16];
  int rc = CAI_OK;
  size_t i;
  memcpy(input, expected, sizeof(input));
  memset(&out, 0, sizeof(out));
  out.expected = input;
  assert(cai_workspace_server_get_state(server, &before, NULL) == CAI_OK);
  client = open_client_mode(root, root, 0U, &out, probe_mode);
  /* Changing the caller's buffer must not change the selected owner. */
  memcpy(input, cai_workspace_server_instance(server), sizeof(input));
  for (i = 0U; i < 8U && rc == CAI_OK; ++i) {
    rc = cai_workspace_client_pump(client, now, NULL);
    assert(cai_workspace_server_pump(server, now, NULL) == CAI_OK);
  }
  assert(rc == (probe_mode ? CAI_ERR_PROTOCOL : CAI_ERR_TRANSPORT));
  assert(snapshot(client).phase == CAI_WORKSPACE_FAILED);
  assert(!snapshot(client).has_welcome && !snapshot(client).has_identity);
  assert(!out.welcomes && !out.roles && !out.identities);
  assert(cai_workspace_server_get_state(server, &after, NULL) == CAI_OK);
  assert(after.phase == before.phase && after.count == before.count &&
         after.generation == before.generation &&
         after.controller == before.controller &&
         after.deadline_ms == before.deadline_ms &&
         after.stop_reason == before.stop_reason);
  cai_workspace_client_close(client);
}
static void instance_guard(const char *root, int persist) {
  cai_workspace_server_config config;
  cai_workspace_server *server;
  cai_workspace_client *a, *b, *query;
  cai_attachment_state state;
  observer oa, ob, oq;
  admissions admission;
  unsigned char old[16], expected[16];
  memset(&admission, 0, sizeof(admission));
  memset(&oa, 0, sizeof(oa));
  memset(&ob, 0, sizeof(ob));
  memset(&oq, 0, sizeof(oq));
  cai_workspace_server_config_init(&config);
  config.runtime_directory = config.workspace = root;
  config.lifetime.persist = persist;
  config.lifetime.first_attachment_timeout_ms = 1000U;
  config.event = admitted;
  config.context = &admission;
  assert(cai_workspace_server_open(&config, &server, NULL) == CAI_OK);
  memcpy(old, cai_workspace_server_instance(server), sizeof(old));
  cai_workspace_server_close(server);
  assert(cai_workspace_server_open(&config, &server, NULL) == CAI_OK);
  admission.server = server;
  assert(memcmp(old, cai_workspace_server_instance(server), sizeof(old)) != 0);
  guarded_rejection(server, root, old, 0, 0U);
  assert(cai_workspace_server_ready(server, 5U, NULL) == CAI_OK);
  guarded_rejection(server, root, old, 0, 5U);
  guarded_rejection(server, root, old, 1, 5U);
  assert(!admission.attached && !admission.detached && !admission.completed);
  oq.expected = cai_workspace_server_instance(server);
  query = open_client_mode(root, root, 0U, &oq, 1);
  exchange(server, query, NULL, 5U);
  assert(snapshot(query).phase == CAI_WORKSPACE_IDENTIFIED &&
         oq.identities == 1U);
  cai_workspace_client_close(query);
  memcpy(expected, cai_workspace_server_instance(server), sizeof(expected));
  oa.expected = expected;
  a = open_client(root, root, 0U, &oa);
  memset(expected, 0, sizeof(expected));
  exchange(server, a, NULL, 5U);
  assert(snapshot(a).phase == CAI_WORKSPACE_CONNECTED && oa.welcomes == 1U);
  b = open_client(root, root, 0U, &ob);
  exchange(server, a, b, 5U);
  guarded_rejection(server, root, old, 0, 6U);
  assert(admission.attached == 2U && !admission.detached &&
         !admission.completed);
  assert(cai_workspace_server_authorize(server, snapshot(a).attachment,
                                        snapshot(a).generation,
                                        NULL) == CAI_OK);
  cai_workspace_client_close(a);
  cai_workspace_client_close(b);
  assert(cai_workspace_server_pump(server, 7U, NULL) == CAI_OK);
  assert(cai_workspace_server_get_state(server, &state, NULL) == CAI_OK);
  assert(state.count == 0U && admission.detached == 2U);
  assert(state.phase ==
         (persist ? CAI_ATTACHMENT_READY : CAI_ATTACHMENT_STOPPING));
  cai_workspace_server_close(server);
}
static void stale_control(const char *runtime, const char *workspace) {
  cai_workspace_server_config config;
  cai_workspace_server *server;
  cai_workspace_client *a, *b;
  cai_attachment_state state;
  observer oa, ob;
  uint64_t generation;
  memset(&oa, 0, sizeof(oa));
  memset(&ob, 0, sizeof(ob));
  cai_workspace_server_config_init(&config);
  config.runtime_directory = runtime;
  config.workspace = workspace;
  config.lifetime.persist = 1;
  assert(cai_workspace_server_open(&config, &server, NULL) == CAI_OK);
  assert(cai_workspace_server_ready(server, 0U, NULL) == CAI_OK);
  a = open_client(runtime, workspace, 0U, &oa);
  b = open_client(runtime, workspace, 0U, &ob);
  exchange(server, a, b, 1U);
  generation = snapshot(a).generation;
  assert(cai_workspace_client_takeover(b, 1U, snapshot(b).generation, NULL) ==
         CAI_OK);
  assert(cai_workspace_client_pump(b, 2U, NULL) == CAI_OK);
  assert(cai_workspace_server_pump(server, 2U, NULL) == CAI_OK);
  /* A has not received B's new role yet. Its locally valid request must still
   * be rejected at server admission rather than taking control back. */
  assert(snapshot(a).generation == generation);
  assert(cai_workspace_client_takeover(a, 2U, generation, NULL) == CAI_OK);
  assert(cai_workspace_client_pump(a, 2U, NULL) == CAI_OK);
  assert(cai_workspace_server_pump(server, 2U, NULL) == CAI_OK);
  exchange(server, a, b, 2U);
  assert(snapshot(a).phase == CAI_WORKSPACE_DISCONNECTED);
  assert(cai_workspace_server_get_state(server, &state, NULL) == CAI_OK);
  assert(state.count == 1U && state.controller == snapshot(b).attachment);
  cai_workspace_client_close(a);
  cai_workspace_client_close(b);
  cai_workspace_server_close(server);
}
static void control_backpressure(const char *runtime, const char *workspace) {
  cai_workspace_server_config config;
  cai_workspace_server *server;
  cai_workspace_client *a, *b;
  cai_protocol_header header;
  cai_cbor_token value;
  cai_attachment_state state;
  observer oa, ob;
  size_t i;
  memset(&oa, 0, sizeof(oa));
  memset(&ob, 0, sizeof(ob));
  cai_workspace_server_config_init(&config);
  config.runtime_directory = runtime;
  config.workspace = workspace;
  config.lifetime.persist = 1;
  assert(cai_workspace_server_open(&config, &server, NULL) == CAI_OK);
  assert(cai_workspace_server_ready(server, 0U, NULL) == CAI_OK);
  a = open_client(runtime, workspace, 0U, &oa);
  b = open_client(runtime, workspace, 0U, &ob);
  exchange(server, a, b, 1U);
  memset(&header, 0, sizeof(header));
  header.kind = CAI_PROTOCOL_STREAM_CHUNK;
  assert(cai_workspace_server_begin(server, snapshot(a).attachment, &header,
                                    8195U, NULL) == CAI_OK);
  memset(&value, 0, sizeof(value));
  value.type = CAI_CBOR_BYTES;
  value.argument = 8192U;
  assert(cai_workspace_server_write(server, snapshot(a).attachment, &value,
                                    NULL) == CAI_OK);
  for (i = 0U; i < 16U; ++i) {
    assert(cai_workspace_client_takeover(a, 100U + i, snapshot(a).generation,
                                         NULL) == CAI_OK);
    exchange(server, a, b, 1U);
  }
  /* Seventeen deferred receipts cannot grow memory without bound or block
   * another attachment. Only the subscriber with the unfinished frame fails. */
  assert(cai_workspace_client_takeover(b, 200U, snapshot(b).generation, NULL) ==
         CAI_OK);
  assert(cai_workspace_client_pump(b, 2U, NULL) == CAI_OK);
  assert(cai_workspace_server_pump(server, 2U, NULL) == CAI_OK);
  assert(cai_workspace_server_pump(server, 2U, NULL) == CAI_OK);
  assert(cai_workspace_client_pump(a, 2U, NULL) == CAI_ERR_PROTOCOL);
  assert(cai_workspace_client_pump(b, 2U, NULL) == CAI_OK);
  assert(cai_workspace_server_get_state(server, &state, NULL) == CAI_OK);
  assert(state.count == 1U && state.controller == snapshot(b).attachment);
  assert(cai_workspace_server_authorize(server, snapshot(b).attachment,
                                        snapshot(b).generation,
                                        NULL) == CAI_OK);
  cai_workspace_client_close(a);
  cai_workspace_client_close(b);
  cai_workspace_server_close(server);
}
static int ignore(void *context, const cai_wire_event *event,
                  cai_error *error) {
  (void)context;
  (void)event;
  (void)error;
  return CAI_OK;
}
static void wt(cai_wire_peer *peer, cai_cbor_type type, uint64_t argument,
               const void *bytes, size_t length) {
  cai_cbor_token token;
  memset(&token, 0, sizeof(token));
  token.type = type;
  token.argument = argument;
  token.data = bytes;
  token.length = length;
  assert(cai_wire_peer_write(peer, &token, NULL) == CAI_OK);
}
static void ws(cai_wire_peer *peer, const char *value, size_t length) {
  wt(peer, CAI_CBOR_BYTES, length, NULL, 0U);
  wt(peer, CAI_CBOR_DATA, 0U, value, length);
  wt(peer, CAI_CBOR_END, CAI_CBOR_BYTES, NULL, 0U);
}
static void send_identity(cai_wire_peer *peer, const char *workspace,
                          int defect, cai_attachment_phase phase) {
  cai_protocol_header h;
  const char *schema = defect == 2 ? "bad.workspace" : CAI_WORKSPACE_SCHEMA;
  static const char instance[] = "0123456789abcdef";
  size_t n = strlen(workspace), s = strlen(schema), m = defect == 3 ? 15U : 16U;
  memset(&h, 0, sizeof(h));
  h.kind = defect == 8 ? CAI_PROTOCOL_WELCOME : CAI_PROTOCOL_IDENTITY;
  h.attachment = defect == 5;
  h.generation = defect == 6;
  h.request = defect == 7;
  assert(n < 256U && s < 24U);
  assert(cai_protocol_peer_begin(peer, &h,
                                 (uint32_t)(n + s + m + (n < 24U ? 6U : 7U)),
                                 NULL) == CAI_OK);
  wt(peer, CAI_CBOR_ARRAY, defect == 14 ? 4U : 5U, NULL, 0U);
  if (defect == 1) {
    wt(peer, CAI_CBOR_BYTES, n, NULL, 0U);
    wt(peer, CAI_CBOR_DATA, 0U, "!", 1U);
    wt(peer, CAI_CBOR_DATA, 0U, workspace + 1, n - 1U);
    wt(peer, CAI_CBOR_END, CAI_CBOR_BYTES, NULL, 0U);
  } else
    ws(peer, workspace, n);
  wt(peer, CAI_CBOR_TEXT, s, NULL, 0U);
  wt(peer, CAI_CBOR_DATA, 0U, schema, s);
  wt(peer, CAI_CBOR_END, CAI_CBOR_TEXT, NULL, 0U);
  ws(peer, instance, m);
  wt(peer, defect == 9 ? CAI_CBOR_UINT : CAI_CBOR_BOOL, 1U, NULL, 0U);
  if (defect == 14)
    return; /* Invalid arity is rejected at the first token. */
  wt(peer, CAI_CBOR_UINT, defect == 4 ? 3U : (uint64_t)phase, NULL, 0U);
  wt(peer, CAI_CBOR_END, CAI_CBOR_ARRAY, NULL, 0U);
  assert(cai_protocol_peer_end(peer, NULL) == CAI_OK);
}
static void discovery_replies(const char *root) {
  int defect;
  for (defect = 0; defect < 18; ++defect) {
    cai_workspace_endpoint *endpoint;
    cai_workspace_client *client;
    cai_wire_peer *peer;
    observer out;
    int fd = -1, rc = CAI_OK;
    size_t pending;
    memset(&out, 0, sizeof(out));
    assert(cai_workspace_endpoint_open(root, root, &endpoint, NULL) == CAI_OK);
    assert(cai_workspace_endpoint_listen(endpoint, NULL) == CAI_OK);
    client = open_client_mode(root, root, 50U, &out, 1);
    assert(cai_workspace_client_pump(client, 0U, NULL) == CAI_OK);
    assert(cai_workspace_endpoint_accept(endpoint, &fd, NULL) == CAI_OK &&
           fd >= 0);
    assert(cai_wire_peer_open(fd, 0U, ignore, NULL, &peer, NULL) == CAI_OK);
    if (defect == 16) {
      assert(shutdown(fd, SHUT_WR) == 0);
      assert(cai_workspace_client_pump(client, 1U, NULL) == CAI_ERR_TRANSPORT);
    } else if (defect == 17) {
      assert(cai_workspace_client_pump(client, 49U, NULL) == CAI_OK);
      assert(cai_workspace_client_pump(client, 50U, NULL) == CAI_ERR_LIMIT);
    } else {
      cai_attachment_phase phase = defect == 15
                                       ? CAI_ATTACHMENT_STOPPING
                                       : (cai_attachment_phase)(defect % 3);
      out.reject = defect == 12;
      send_identity(peer, root, defect, phase);
      if (defect == 10)
        send_identity(peer, root, 0, CAI_ATTACHMENT_READY);
      if (defect == 10) {
        assert(cai_wire_peer_flush(peer, 0U, NULL) == CAI_OK);
        assert(cai_workspace_client_pump(client, 0U, NULL) == CAI_ERR_PROTOCOL);
        assert(out.identities == 1U &&
               snapshot(client).phase == CAI_WORKSPACE_FAILED);
      } else if (defect == 11) {
        assert(cai_wire_peer_flush(peer, 17U, NULL) == CAI_OK);
        assert(cai_workspace_client_pump(client, 0U, NULL) == CAI_OK);
        assert(shutdown(fd, SHUT_WR) == 0);
        assert(cai_workspace_client_pump(client, 1U, NULL) == CAI_ERR_PROTOCOL);
      } else {
        do {
          assert(cai_wire_peer_flush(peer, 1U, NULL) == CAI_OK);
          pending = cai_wire_peer_pending(peer);
          rc = cai_workspace_client_pump(client, 0U, NULL);
          if (rc != CAI_OK)
            break;
          if (pending)
            assert(!out.identities && !snapshot(client).has_identity);
        } while (pending);
        if (defect == 0 || defect == 13 || defect == 15) {
          assert(rc == CAI_OK && out.identities == 1U);
          assert(snapshot(client).phase == CAI_WORKSPACE_IDENTIFIED);
          assert(snapshot(client).server_phase == phase);
        } else {
          assert(rc == (defect == 12 ? CAI_ERR_CANCELLED : CAI_ERR_PROTOCOL));
          assert(snapshot(client).phase == CAI_WORKSPACE_FAILED);
          assert(!out.identities &&
                 snapshot(client).has_identity == (defect == 12));
        }
      }
    }
    cai_workspace_client_close(client);
    cai_wire_peer_close(peer);
    cai_workspace_endpoint_close(endpoint);
  }
}
static void send_welcome(cai_wire_peer *peer, const char *workspace,
                         int defect) {
  cai_protocol_header h;
  static const char instance[] = "0123456789abcdef";
  size_t n = strlen(workspace), m = defect == 2 ? 15U : 16U;
  memset(&h, 0, sizeof(h));
  h.kind = CAI_PROTOCOL_WELCOME;
  h.attachment = defect == 1 ? 0U : 7U;
  h.generation = 1U;
  assert(n < 256U);
  assert(cai_protocol_peer_begin(peer, &h,
                                 (uint32_t)(n + m + (n < 24U ? 4U : 5U)),
                                 NULL) == CAI_OK);
  wt(peer, CAI_CBOR_ARRAY, 3U, NULL, 0U);
  ws(peer, workspace, n);
  ws(peer, instance, m);
  wt(peer, CAI_CBOR_BOOL, 1U, NULL, 0U);
  wt(peer, CAI_CBOR_END, CAI_CBOR_ARRAY, NULL, 0U);
  assert(cai_protocol_peer_end(peer, NULL) == CAI_OK);
}
static void send_role(cai_wire_peer *peer, uint64_t attachment,
                      uint64_t generation, uint64_t controller, int malformed) {
  cai_protocol_header h;
  memset(&h, 0, sizeof(h));
  h.kind = CAI_PROTOCOL_ROLE;
  h.attachment = attachment;
  h.generation = generation;
  assert(cai_protocol_peer_begin(peer, &h, 1U, NULL) == CAI_OK);
  wt(peer, malformed ? CAI_CBOR_NULL : CAI_CBOR_UINT, controller, NULL, 0U);
  assert(cai_protocol_peer_end(peer, NULL) == CAI_OK);
}
static void streamed(cai_wire_peer *peer, cai_workspace_client *client,
                     observer *out) {
  cai_protocol_header header;
  unsigned char bytes[256];
  size_t chunk, part;
  int eof;
  memset(bytes, 'x', sizeof(bytes));
  send_role(peer, 7U, 1U, 7U, 0);
  assert(cai_wire_peer_flush(peer, 0U, NULL) == CAI_OK);
  assert(cai_workspace_client_pump(client, 1U, NULL) == CAI_OK);
  assert(snapshot(client).phase == CAI_WORKSPACE_CONNECTED);
  /* More than a default transport backlog in total, produced/consumed as
   * bounded chunks, with DATA observable before each record completes. */
  for (chunk = 0U; chunk < 40U; ++chunk) {
    memset(&header, 0, sizeof(header));
    header.kind = CAI_PROTOCOL_STREAM_CHUNK;
    header.attachment = 7U;
    header.generation = 1U;
    header.stream = 42U;
    header.sequence = chunk + 1U;
    assert(cai_protocol_peer_begin(peer, &header, 32771U, NULL) == CAI_OK);
    wt(peer, CAI_CBOR_BYTES, 32768U, NULL, 0U);
    for (part = 0U; part < 128U; ++part)
      wt(peer, CAI_CBOR_DATA, 0U, bytes, sizeof(bytes));
    wt(peer, CAI_CBOR_END, CAI_CBOR_BYTES, NULL, 0U);
    assert(cai_protocol_peer_end(peer, NULL) == CAI_OK);
    assert(cai_wire_peer_flush(peer, 512U, NULL) == CAI_OK);
    assert(cai_workspace_client_pump(client, 1U, NULL) == CAI_OK);
    assert(out->bytes > chunk * 32768U && out->bytes < (chunk + 1U) * 32768U);
    assert(out->complete == chunk);
    assert(cai_wire_peer_flush(peer, 0U, NULL) == CAI_OK);
    assert(cai_workspace_client_pump(client, 1U, NULL) == CAI_OK);
    assert(out->complete == chunk + 1U);
    assert(out->bytes == (chunk + 1U) * 32768U);
  }
  assert(out->headers == 40U);
  /* Drain the peer's greeting so closing it produces clean EOF, not an
   * operating-system reset for discarded unread input. */
  assert(cai_wire_peer_read(peer, 0U, &eof, NULL) == CAI_OK && !eof);
}
static void adversarial(const char *runtime, const char *workspace) {
  cai_workspace_endpoint *endpoint;
  cai_workspace_client *client;
  cai_wire_peer *peer;
  observer observed_state;
  int fd, rc, variant;
  size_t bytes;
  for (variant = 0; variant < 16; ++variant) {
    memset(&observed_state, 0, sizeof(observed_state));
    assert(cai_workspace_endpoint_open(runtime, workspace, &endpoint, NULL) ==
           CAI_OK);
    assert(cai_workspace_endpoint_listen(endpoint, NULL) == CAI_OK);
    client = open_client(runtime, workspace, variant == 8 ? 10U : 0U,
                         &observed_state);
    assert(cai_workspace_client_pump(client, 0U, NULL) == CAI_OK);
    assert(cai_workspace_endpoint_accept(endpoint, &fd, NULL) == CAI_OK &&
           fd >= 0);
    assert(cai_wire_peer_open(fd, 0U, ignore, NULL, &peer, NULL) == CAI_OK);
    if (variant == 8) {
      assert(cai_workspace_client_pump(client, 10U, NULL) == CAI_ERR_LIMIT);
      assert(snapshot(client).phase == CAI_WORKSPACE_FAILED);
    } else if (variant == 9) {
      assert(cai_workspace_client_detach(client, NULL) == CAI_OK);
      assert(snapshot(client).phase == CAI_WORKSPACE_DISCONNECTED);
    } else {
      send_welcome(peer, variant == 0 ? "/wrong" : workspace, variant);
      if (variant == 13) {
        assert(cai_wire_peer_flush(peer, 17U, NULL) == CAI_OK);
        assert(cai_workspace_client_pump(client, 0U, NULL) == CAI_OK);
        assert(observed_state.welcomes == 0U && !snapshot(client).has_welcome);
        assert(shutdown(cai_wire_peer_fd(peer), SHUT_WR) == 0);
        assert(cai_workspace_client_pump(client, 1U, NULL) == CAI_ERR_PROTOCOL);
        cai_wire_peer_close(peer);
        cai_workspace_client_close(client);
        cai_workspace_endpoint_close(endpoint);
        continue;
      }
      /* Fragment every byte and prove there is no provisional welcome or
       * authority visible to consumers before the record is complete. */
      while (cai_wire_peer_pending(peer) != 0U) {
        assert(cai_wire_peer_flush(peer, 1U, NULL) == CAI_OK);
        bytes = cai_wire_peer_pending(peer);
        rc = cai_workspace_client_pump(client, 0U, NULL);
        if (variant < 3) {
          if (rc != CAI_OK)
            break;
        } else
          assert(rc == CAI_OK);
        if (bytes != 0U)
          assert(observed_state.welcomes == 0U);
      }
      if (variant < 3) {
        assert(snapshot(client).phase == CAI_WORKSPACE_FAILED &&
               observed_state.welcomes == 0U);
      } else if (variant == 3) {
        cai_wire_peer_close(peer);
        peer = NULL;
        assert(cai_workspace_client_pump(client, 1U, NULL) ==
               CAI_ERR_TRANSPORT);
      } else if (variant == 7) {
        cai_error error;
        cai_error_init(&error);
        observed_state.reject = 1;
        send_role(peer, 7U, 1U, 7U, 0);
        assert(cai_wire_peer_flush(peer, 0U, NULL) == CAI_OK);
        assert(cai_workspace_client_pump(client, 1U, &error) ==
               CAI_ERR_CANCELLED);
        assert(error.code == CAI_ERR_CANCELLED && error.message != NULL);
        cai_error_cleanup(&error);
      } else if (variant == 11) {
        send_welcome(peer, workspace, 0);
        assert(cai_wire_peer_flush(peer, 0U, NULL) == CAI_OK);
        assert(cai_workspace_client_pump(client, 1U, NULL) == CAI_ERR_PROTOCOL);
        assert(observed_state.welcomes == 1U);
      } else if (variant == 12) {
        send_role(peer, 7U, 0U, 7U, 0);
        assert(cai_wire_peer_flush(peer, 0U, NULL) == CAI_OK);
        assert(cai_workspace_client_pump(client, 1U, NULL) == CAI_ERR_PROTOCOL);
      } else if (variant == 14) {
        struct pollfd interest;
        uint64_t deadline;
        observed_state.fail_output = 1;
        send_role(peer, 7U, 1U, 7U, 0);
        assert(cai_wire_peer_flush(peer, 0U, NULL) == CAI_OK);
        assert(cai_workspace_client_pump(client, 1U, NULL) == CAI_ERR_INVALID);
        assert(snapshot(client).phase == CAI_WORKSPACE_FAILED);
        assert(cai_workspace_client_prepare(client, &interest, &deadline,
                                            NULL) == CAI_OK);
        assert(interest.fd == -1 && deadline == 0U);
      } else if (variant == 15) {
        cai_protocol_header header;
        cai_cbor_token value;
        send_role(peer, 7U, 1U, 7U, 0);
        assert(cai_wire_peer_flush(peer, 0U, NULL) == CAI_OK);
        assert(cai_workspace_client_pump(client, 1U, NULL) == CAI_OK);
        memset(&header, 0, sizeof(header));
        header.kind = CAI_PROTOCOL_REQUEST;
        header.generation = 1U;
        assert(cai_workspace_client_begin(client, &header, 4U, 1, NULL) ==
               CAI_OK);
        memset(&value, 0, sizeof(value));
        value.type = CAI_CBOR_BYTES;
        value.argument = 3U;
        assert(cai_workspace_client_write(client, &value, NULL) == CAI_OK);
        assert(cai_workspace_client_pump(client, 1U, NULL) == CAI_OK);
        assert(cai_workspace_client_detach(client, NULL) == CAI_OK);
        assert(cai_workspace_client_write(client, &value, NULL) ==
               CAI_ERR_INVALID);
        assert(snapshot(client).phase == CAI_WORKSPACE_DISCONNECTED);
      } else if (variant == 10) {
        streamed(peer, client, &observed_state);
        cai_wire_peer_close(peer);
        peer = NULL;
        assert(cai_workspace_client_pump(client, 2U, NULL) == CAI_OK);
        assert(snapshot(client).phase == CAI_WORKSPACE_DISCONNECTED);
      } else {
        send_role(peer, variant == 4 ? 8U : 7U, 1U, 7U, variant == 5);
        assert(cai_wire_peer_flush(peer, 0U, NULL) == CAI_OK);
        rc = cai_workspace_client_pump(client, 1U, NULL);
        if (variant == 6) {
          assert(rc == CAI_OK);
          send_role(peer, 7U, 1U, 0U,
                    0); /* Authority changed without a new generation. */
          assert(cai_wire_peer_flush(peer, 0U, NULL) == CAI_OK);
          rc = cai_workspace_client_pump(client, 2U, NULL);
        }
        assert(rc == CAI_ERR_PROTOCOL);
      }
    }
    cai_wire_peer_close(peer);
    cai_workspace_client_close(client);
    cai_workspace_endpoint_close(endpoint);
  }
}
static void *run(void *unused) {
  char root[] = "/tmp/cai-client-XXXXXX", path[256];
  DIR *directory;
  struct dirent *entry;
  (void)unused;
  assert(mkdtemp(root) != NULL);
  integration(root, root, 0);
  integration(root, root, 1);
  instance_guard(root, 0);
  instance_guard(root, 1);
  discovery(root, 0);
  discovery(root, 1);
  discovery_replies(root);
  stale_control(root, root);
  control_backpressure(root, root);
  adversarial(root, root);
  directory = opendir(root);
  assert(directory != NULL);
  while ((entry = readdir(directory)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
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
