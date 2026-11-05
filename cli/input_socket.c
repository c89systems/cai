#include "input_socket.h"
#include "error.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
struct cai_cli_input_socket {
  cai_cli_input_adapter *adapter;
  cai_app_client *client;
  cai_app_client *paused_client;
  unsigned long long paused_conversation;
  int synchronizing, resumed_snapshot;
  char session[129];
  unsigned long long producer, base, highest, source_high, item_namespace;
  int fixed, detached;
  int idle_announced;
  uint64_t idle_generation;
  unsigned long long idle_after;
  int wakeup;
  void (*submitted)(void *, const cai_input_request *);
  void *submitted_context;
  cai_error error;
};
static int unavailable(cai_error *error, const char *message) {
  return cai_cli_error(error, CAI_ERR_INVALID, message);
}
static void wake(cai_cli_input_socket *s) {
  char byte = 'q';
  ssize_t n;
  if (s->wakeup < 0)
    return;
  do {
    n = write(s->wakeup, &byte, 1U);
  } while (n < 0 && errno == EINTR);
}
static int submit(cai_cli_input_socket *s, const cai_input_scope *scope,
                  const cai_input_request *source, cai_error *error) {
  cai_app_client_state state;
  cai_input_request request = *source;
  cai_input_scope original = *scope;
  unsigned long long maximum = ~(unsigned long long)0;
  int rc;
  if (s->detached || s->client == NULL || !s->fixed)
    return unavailable(error, "Wait for the producer binding snapshot");
  rc = cai_app_client_get_state(s->client, &state, error);
  if (rc != CAI_OK)
    return rc;
  if (!state.bound || strcmp(state.session, s->session) != 0 ||
      state.producer != s->producer)
    return unavailable(error, "Resume the original queue producer first");
  if (source->request_id <= s->source_high)
    return unavailable(error,
                       "Use RESOLVE for an existing editor request identity");
  if (source->request_id > maximum - s->base ||
      source->target_request_id > s->source_high)
    return unavailable(error, "Editor request identity is out of range");
  if (state.last_request > s->highest)
    return unavailable(error,
                       "Queue producer advanced outside this editor; inspect "
                       "accepted work and reopen its view");
  request.request_id = s->base + source->request_id;
  if (request.target_request_id != 0U)
    request.target_request_id += s->base;
  /* Reserve even on an uncertain handoff failure. The editor never upgrades
   * and retries an old mutation using a new authority generation. */
  s->source_high = source->request_id;
  if (request.request_id > s->highest)
    s->highest = request.request_id;
  original.generation = state.transport.generation;
  return cai_app_client_request(s->client, &original, &request, error);
}
static int request(void *context, const cai_input_scope *scope,
                   const cai_input_request *value, cai_error *error) {
  cai_cli_input_socket *s = context;
  int rc;
  s->idle_announced = 0;
  cai_error_cleanup(&s->error);
  if (s->submitted != NULL &&
      (value->operation == CAI_INPUT_OFFER ||
       value->operation == CAI_INPUT_END_EDIT) &&
      value->text != NULL)
    s->submitted(s->submitted_context, value);
  rc = submit(s, scope, value, &s->error);
  if (rc == CAI_OK)
    wake(s);
  if (rc != CAI_OK)
    cai_cli_error(error, rc,
                  s->error.message != NULL ? s->error.message
                                           : "Queue handoff failed");
  return rc;
}
static void detached(void *context) {
  cai_cli_input_socket *s = context;
  s->detached = 1;
}
static unsigned long long item_identity(void *context,
                                        unsigned long long conversation) {
  (void)conversation;
  return ((cai_cli_input_socket *)context)->item_namespace;
}
int cai_cli_input_socket_open(cai_app_client *client,
                              unsigned long long last_issued,
                              cai_cli_input_socket **out, cai_error *error) {
  cai_cli_input_socket *s;
  cai_cli_input_transport transport;
  cai_app_client_state state;
  int rc;
  if (out != NULL)
    *out = NULL;
  if (out == NULL)
    return unavailable(error, "Application client and queue output required");
  memset(&state, 0, sizeof(state));
  rc =
      client != NULL ? cai_app_client_get_state(client, &state, error) : CAI_OK;
  if (rc != CAI_OK)
    return rc;
  if (state.bound || state.binding)
    return unavailable(error,
                       "Construct the editor before binding its producer");
  s = calloc(1U, sizeof(*s));
  if (s == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "Allocate socket queue binding");
  s->client = client;
  s->wakeup = -1;
  s->base = s->highest = last_issued;
  cai_error_init(&s->error);
  memset(&transport, 0, sizeof(transport));
  transport.context = s;
  transport.request = request;
  transport.close = detached;
  transport.identity = item_identity;
  rc = cai_cli_input_adapter_open(&transport, &s->adapter, error);
  if (rc == CAI_OK && client != NULL)
    rc = cai_app_client_manage_delivery(client, error);
  if (rc != CAI_OK) {
    cai_cli_input_adapter_close(s->adapter);
    free(s);
    return rc;
  }
  *out = s;
  return CAI_OK;
}
void cai_cli_input_socket_set_wakeup(cai_cli_input_socket *s, int descriptor) {
  if (s != NULL)
    s->wakeup = descriptor;
}
cai_cli_input_adapter *cai_cli_input_socket_adapter(cai_cli_input_socket *s) {
  return s != NULL ? s->adapter : NULL;
}
void cai_cli_input_socket_prompt(cai_cli_input_socket *s,
                                 void (*fn)(void *, const cai_input_request *),
                                 void *context) {
  if (s != NULL) {
    s->submitted = fn;
    s->submitted_context = context;
  }
}
int cai_cli_input_socket_disconnect(cai_cli_input_socket *s, cai_error *error) {
  cai_input_observation observation;
  int rc;
  if (s == NULL || s->detached)
    return unavailable(error, "Live socket queue binding required");
  s->paused_client = NULL;
  s->idle_announced = 0;
  s->synchronizing = s->resumed_snapshot = 0;
  if (s->client == NULL)
    return CAI_OK;
  memset(&observation, 0, sizeof(observation));
  observation.type = CAI_INPUT_OBSERVATION_DISCONNECTED;
  rc = cai_cli_input_adapter_observe(s->adapter, &observation, error);
  if (rc == CAI_OK)
    s->client = NULL;
  return rc;
}
int cai_cli_input_socket_pause(cai_cli_input_socket *s, cai_error *error) {
  cai_app_client_state state;
  cai_app_client *client;
  int rc;
  if (s == NULL || s->detached || !s->fixed || s->client == NULL ||
      s->paused_client != NULL)
    return unavailable(error, "Live bound editor routing required");
  client = s->client;
  rc = cai_app_client_get_state(client, &state, error);
  if (rc != CAI_OK)
    return rc;
  if (!state.bound || state.producer != s->producer ||
      strcmp(state.session, s->session) != 0)
    return unavailable(error, "The editor producer is not bound");
  rc = cai_cli_input_socket_disconnect(s, error);
  if (rc == CAI_OK) {
    s->paused_client = client;
    s->paused_conversation = state.conversation;
  }
  return rc;
}
int cai_cli_input_socket_resume(cai_cli_input_socket *s, cai_app_client *client,
                                cai_error *error) {
  cai_app_client_state state;
  int rc;
  if (s == NULL || s->detached || client == NULL ||
      client != s->paused_client || s->client != NULL)
    return unavailable(error, "The original paused transport is required");
  rc = cai_app_client_get_state(client, &state, error);
  if (rc != CAI_OK)
    return rc;
  if (state.transport.phase != CAI_WORKSPACE_CONNECTED || !state.bound ||
      state.conversation != s->paused_conversation ||
      state.producer != s->producer || strcmp(state.session, s->session) != 0)
    return unavailable(error, "Paused editor scope changed; rebind its view");
  s->client = client;
  s->paused_client = NULL;
  s->synchronizing = 1;
  s->resumed_snapshot = 0;
  return CAI_OK;
}
static unsigned long long local_request(cai_cli_input_socket *s,
                                        unsigned long long id) {
  return s->fixed && id > s->base && id - s->base <= s->source_high &&
                 id - s->base <= ULONG_MAX
             ? id - s->base
             : 0U;
}
int cai_cli_input_socket_observe(cai_cli_input_socket *s,
                                 const cai_app_client_event *event,
                                 cai_error *error) {
  cai_input_observation observation;
  cai_app_client_state current;
  const cai_app_client_state *state;
  int end, rc;
  if (s == NULL || s->detached || event == NULL || event->state == NULL)
    return unavailable(error, "Socket queue event required");
  if (s->client == NULL)
    return unavailable(error,
                       "Disconnected queue cannot accept old client events");
  state = event->state;
  rc = cai_app_client_get_state(s->client, &current, error);
  if (rc != CAI_OK)
    return rc;
  if (current.transport.attachment != state->transport.attachment ||
      memcmp(current.transport.instance, state->transport.instance,
             sizeof(current.transport.instance)) != 0)
    return unavailable(error,
                       "Queue event belongs to an old client attachment");
  if (event->type == CAI_APP_CLIENT_CONNECTION)
    return state->transport.phase == CAI_WORKSPACE_FAILED ||
                   state->transport.phase == CAI_WORKSPACE_DISCONNECTED
               ? cai_cli_input_socket_disconnect(s, error)
               : CAI_OK;
  if (event->type == CAI_APP_CLIENT_BIND_REJECTED)
    return cai_cli_input_socket_disconnect(s, error);
  if (event->type == CAI_APP_CLIENT_STATUS ||
      event->type == CAI_APP_CLIENT_CONTROL_RESULT ||
      event->type == CAI_APP_CLIENT_HISTORY_RESULT ||
      event->type == CAI_APP_CLIENT_SHELL_RESULT)
    return CAI_OK;
  if (event->type != CAI_APP_CLIENT_INPUT || event->input == NULL)
    return unavailable(error, "Typed input observation required");
  observation = *event->input;
  if (s->synchronizing) {
    if (observation.type == CAI_INPUT_OBSERVATION_SNAPSHOT_BEGIN)
      s->resumed_snapshot = 1;
    else if (observation.type != CAI_INPUT_OBSERVATION_RESULT &&
             ((observation.type != CAI_INPUT_OBSERVATION_SNAPSHOT_ITEM &&
               observation.type != CAI_INPUT_OBSERVATION_SNAPSHOT_END) ||
              !s->resumed_snapshot))
      return CAI_OK;
  }
  end = observation.type == CAI_INPUT_OBSERVATION_SNAPSHOT_END;
  if (end &&
      (!state->ready || state->session[0] == '\0' || state->producer == 0U ||
       observation.conversation != state->conversation ||
       observation.producer != state->producer ||
       (s->fixed && (strcmp(state->session, s->session) != 0 ||
                     state->producer != s->producer))))
    return unavailable(error, "Queue snapshot belongs to another producer");
  if (end && s->item_namespace == 0U)
    s->item_namespace = observation.conversation;
  if (observation.type == CAI_INPUT_OBSERVATION_RESULT) {
    observation.result.request_id =
        local_request(s, observation.result.request_id);
    if (observation.result.request_id == 0U)
      return CAI_OK; /* A receipt from before this editor was constructed. */
  } else if (observation.type == CAI_INPUT_OBSERVATION_CHANGE)
    observation.change.request_id =
        local_request(s, observation.change.request_id);
  rc = cai_cli_input_adapter_observe(s->adapter, &observation, error);
  if (rc == CAI_OK && end)
    s->synchronizing = s->resumed_snapshot = 0;
  if (rc == CAI_OK && end && !s->fixed) {
    strcpy(s->session, state->session);
    s->producer = state->producer;
    if (state->last_request > s->base)
      s->base = state->last_request;
    s->highest = s->base;
    s->fixed = 1;
  }
  return rc;
}
int cai_cli_input_socket_reconnect(cai_cli_input_socket *s,
                                   cai_app_client *client, cai_error *error) {
  cai_app_client_state replacement;
  int rc;
  if (s == NULL || s->detached || client == NULL || client == s->client ||
      s->paused_client != NULL)
    return unavailable(error, "New application client required");
  rc = cai_app_client_get_state(client, &replacement, error);
  if (rc != CAI_OK)
    return rc;
  if (s->client != NULL || replacement.bound || replacement.binding ||
      replacement.transport.phase == CAI_WORKSPACE_FAILED ||
      replacement.transport.phase == CAI_WORKSPACE_DISCONNECTED)
    return unavailable(error,
                       "Disconnect the old transport and explicitly rebind "
                       "the replacement");
  rc = cai_app_client_manage_delivery(client, error);
  if (rc != CAI_OK)
    return rc;
  s->idle_announced = 0;
  s->client = client;
  return CAI_OK;
}
int cai_cli_input_socket_idle(cai_cli_input_socket *s, sl_t *editor,
                              cai_error *error) {
  cai_app_client_state state;
  sl_queue_backend_info_t info;
  cai_input_scope scope;
  size_t i;
  int rc;
  if (s == NULL || editor == NULL)
    return unavailable(error, "editor binding required");
  if (s->client == NULL || s->detached || !s->fixed || s->synchronizing)
    return CAI_OK;
  rc = cai_app_client_get_state(s->client, &state, error);
  if (rc != CAI_OK)
    return rc;
  if (!state.ready || !state.bound ||
      state.transport.controller != state.transport.attachment)
    return CAI_OK;
  if (sl_queue_backend_info(editor, &info) != SL_OK)
    return unavailable(error, "inspect native editor receipts");
  if (!info.synchronized)
    return CAI_OK;
  for (i = 0U; i < info.pending_count; ++i) {
    sl_queue_pending_info_t pending;
    if (sl_queue_backend_pending(editor, i, &pending) != SL_OK)
      return unavailable(error, "inspect native editor receipt");
    if (pending.pending || pending.status == SL_QUEUE_ACCEPTED ||
        pending.status == SL_QUEUE_UNKNOWN)
      return CAI_OK;
  }
  if (s->idle_announced && s->idle_generation == state.transport.generation &&
      s->idle_after == state.last_request)
    return CAI_OK;
  scope.conversation = state.conversation;
  scope.generation = state.transport.generation;
  rc = cai_app_client_settled(s->client, &scope, state.last_request, error);
  if (rc == CAI_OK) {
    s->idle_announced = 1;
    s->idle_generation = scope.generation;
    s->idle_after = state.last_request;
    wake(s);
  }
  return rc;
}
const cai_error *cai_cli_input_socket_error(const cai_cli_input_socket *s) {
  return s != NULL ? &s->error : NULL;
}
unsigned long long
cai_cli_input_socket_last_issued(const cai_cli_input_socket *s) {
  return s != NULL ? s->highest : 0U;
}
void cai_cli_input_socket_close(cai_cli_input_socket *s) {
  if (s != NULL) {
    cai_cli_input_adapter_close(s->adapter);
    cai_error_cleanup(&s->error);
    free(s);
  }
}
