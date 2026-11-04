#include <assert.h>
#include <cai/input_controller.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct frontend {
  cai_input_connection *connection;
  unsigned long long conversation, request, item, revision, token;
  size_t results, changes, snapshots, disconnected;
  cai_input_result result;
} frontend;
static void drain(frontend *client) {
  cai_input_observation *observation = NULL;
  while (1) {
    assert(cai_input_connection_poll(client->connection, &observation, NULL) ==
           CAI_OK);
    if (observation == NULL)
      return;
    if (observation->type == CAI_INPUT_OBSERVATION_RESULT) {
      ++client->results;
      client->result = observation->result;
    } else if (observation->type == CAI_INPUT_OBSERVATION_SNAPSHOT) {
      ++client->snapshots;
      client->conversation = observation->conversation;
    } else if (observation->type == CAI_INPUT_OBSERVATION_DISCONNECTED)
      ++client->disconnected;
    else {
      ++client->changes;
      if (observation->change.type == CAI_INPUT_UPSERT) {
        client->item = observation->change.entry.id;
        client->revision = observation->change.entry.revision;
        client->token = observation->change.entry.edit_token;
      }
    }
    cai_input_observation_close(observation);
  }
}
static int send(frontend *client, cai_input_operation operation,
                const char *text) {
  cai_input_request request;
  cai_input_scope scope;
  scope.conversation = client->conversation;
  scope.generation = 0U;
  memset(&request, 0, sizeof(request));
  request.request_id = ++client->request;
  request.operation = operation;
  request.text = text;
  request.item_id = client->item;
  request.item_revision = client->revision;
  request.edit_token = client->token;
  return cai_input_connection_request(client->connection, &scope, &request,
                                      NULL);
}
static void *submit_thread(void *context) {
  frontend *client = context;
  assert(send(client, CAI_INPUT_OFFER, "from another frontend thread") ==
         CAI_OK);
  /* Handoff returns before any owner dispatch or durable acceptance. */
  drain(client);
  assert(client->results == 0U);
  return NULL;
}
typedef struct concurrent_fixture {
  cai_input_controller *controller;
  pthread_mutex_t lock;
  size_t accepted, rejected;
  int done;
} concurrent_fixture;
static void *concurrent_frontend(void *context) {
  concurrent_fixture *fixture = context;
  cai_input_connection *connection = NULL;
  cai_input_request request;
  struct pollfd fd;
  unsigned long long conversation = 0U;
  cai_input_scope scope;
  size_t i;
  assert(cai_input_controller_connect(fixture->controller, 0U, &connection,
                                      NULL) == CAI_OK);
  fd.fd = cai_input_connection_fd(connection);
  fd.events = POLLIN;
  for (i = 0U; i <= 80U; ++i) {
    int received = 0;
    memset(&request, 0, sizeof(request));
    request.request_id = (unsigned long long)i + 1U;
    request.operation = i == 0U ? CAI_INPUT_SYNCHRONIZE : CAI_INPUT_OFFER;
    request.text = i == 0U ? NULL : "concurrent frontend input";
    scope.conversation = conversation;
    scope.generation = 0U;
    assert(cai_input_connection_request(connection, &scope, &request, NULL) ==
           CAI_OK);
    while (!received) {
      cai_input_observation *event = NULL;
      assert(poll(&fd, 1U, 5000) > 0);
      for (;;) {
        assert(cai_input_connection_poll(connection, &event, NULL) == CAI_OK);
        if (event == NULL)
          break;
        if (event->type == CAI_INPUT_OBSERVATION_SNAPSHOT)
          conversation = event->conversation;
        if (event->type == CAI_INPUT_OBSERVATION_RESULT &&
            event->result.request_id == request.request_id) {
          received = 1;
          if (i != 0U) {
            assert(event->result.outcome != CAI_INPUT_UNKNOWN);
            if (event->result.outcome == CAI_INPUT_ACCEPTED)
              ++fixture->accepted;
            else
              ++fixture->rejected;
          }
        }
        cai_input_observation_close(event);
      }
    }
  }
  cai_input_connection_close(connection);
  pthread_mutex_lock(&fixture->lock);
  fixture->done = 1;
  pthread_mutex_unlock(&fixture->lock);
  return NULL;
}
static void concurrent_selection(void) {
  concurrent_fixture fixture;
  cai_input_queue *a = NULL, *b = NULL;
  cai_input_state first, second;
  pthread_t thread;
  struct pollfd fd;
  size_t i;
  int done = 0;
  memset(&fixture, 0, sizeof(fixture));
  assert(pthread_mutex_init(&fixture.lock, NULL) == 0);
  assert(cai_input_queue_open(NULL, &a, NULL) == CAI_OK);
  assert(cai_input_queue_open(NULL, &b, NULL) == CAI_OK);
  assert(cai_input_controller_open(NULL, &fixture.controller, NULL) == CAI_OK);
  assert(cai_input_controller_select(fixture.controller, a, NULL) == CAI_OK);
  assert(pthread_create(&thread, NULL, concurrent_frontend, &fixture) == 0);
  fd.fd = cai_input_controller_fd(fixture.controller);
  fd.events = POLLIN;
  for (i = 0U; i < 2000U && !done; ++i) {
    assert(cai_input_controller_pump(fixture.controller, 4U, NULL) == CAI_OK);
    if (i % 8U == 0U)
      assert(cai_input_controller_select(
                 fixture.controller, i % 16U == 0U ? b : a, NULL) == CAI_OK);
    pthread_mutex_lock(&fixture.lock);
    done = fixture.done;
    pthread_mutex_unlock(&fixture.lock);
    if (!done)
      assert(poll(&fd, 1U, 5) >= 0);
  }
  assert(done && pthread_join(thread, NULL) == 0);
  assert(cai_input_controller_pump(fixture.controller, 4U, NULL) == CAI_OK);
  assert(cai_input_queue_get_state(a, &first, NULL) == CAI_OK);
  assert(cai_input_queue_get_state(b, &second, NULL) == CAI_OK);
  assert(fixture.accepted + fixture.rejected == 80U &&
         first.count + second.count == fixture.accepted);
  cai_input_controller_close(fixture.controller);
  cai_input_queue_close(a);
  cai_input_queue_close(b);
  pthread_mutex_destroy(&fixture.lock);
}
int main(void) {
  cai_input_queue_config config;
  cai_input_queue *first = NULL, *second = NULL;
  cai_input_controller *controller = NULL;
  frontend a, b;
  cai_input_state state;
  cai_input_entry item;
  cai_input_claim claim;
  pthread_t thread;
  size_t i;
  unsigned long long old_conversation;
  concurrent_selection();
  memset(&a, 0, sizeof(a));
  memset(&b, 0, sizeof(b));
  cai_input_queue_config_init(&config);
  assert(cai_input_queue_open(&config, &first, NULL) == CAI_OK);
  assert(cai_input_queue_open(&config, &second, NULL) == CAI_OK);
  assert(cai_input_controller_open(NULL, &controller, NULL) == CAI_OK);
  assert(cai_input_controller_select(controller, first, NULL) == CAI_OK);
  assert(cai_input_controller_connect(controller, 0U, &a.connection, NULL) ==
         CAI_OK);
  assert(cai_input_controller_connect(controller, 0U, &b.connection, NULL) ==
         CAI_OK);
  assert(cai_input_controller_pump(controller, 8U, NULL) == CAI_OK);
  drain(&a);
  drain(&b);
  assert(a.snapshots == 1U && b.snapshots == 1U &&
         a.conversation == b.conversation);
  assert(cai_input_controller_fd(controller) >= 0 &&
         cai_input_connection_fd(a.connection) >= 0);
  assert(pthread_create(&thread, NULL, submit_thread, &a) == 0);
  assert(pthread_join(thread, NULL) == 0);
  assert(cai_input_queue_get_state(first, &state, NULL) == CAI_OK &&
         state.count == 0U);
  assert(cai_input_controller_pump(controller, 8U, NULL) == CAI_OK);
  drain(&a);
  drain(&b);
  assert(a.results == 1U && b.results == 0U &&
         a.result.outcome == CAI_INPUT_ACCEPTED);
  assert(a.item == b.item && a.revision == b.revision);
  assert(send(&a, CAI_INPUT_BEGIN_EDIT, NULL) == CAI_OK);
  assert(cai_input_controller_pump(controller, 8U, NULL) == CAI_OK);
  drain(&a);
  drain(&b);
  assert(a.token != 0U && b.token == 0U);
  assert(cai_input_queue_set_ready(first, a.item, 1, NULL) == CAI_OK);
  assert(cai_input_queue_claim(first, a.item, a.revision, &claim, NULL) ==
         CAI_ERR_INVALID);
  cai_input_connection_close(a.connection);
  a.connection = NULL;
  assert(cai_input_controller_pump(controller, 8U, NULL) == CAI_OK);
  assert(cai_input_queue_lookup(first, b.item, &item, NULL) == CAI_OK);
  assert(item.edit_held && item.edit_owner != 0U);
  assert(send(&b, CAI_INPUT_BEGIN_EDIT, NULL) == CAI_OK);
  assert(cai_input_controller_pump(controller, 8U, NULL) == CAI_OK);
  drain(&b);
  assert(b.result.outcome == CAI_INPUT_ACCEPTED && b.token != 0U);
  assert(send(&b, CAI_INPUT_END_EDIT, "accepted edited text") == CAI_OK);
  assert(cai_input_controller_pump(controller, 8U, NULL) == CAI_OK);
  drain(&b);
  assert(b.result.outcome == CAI_INPUT_ACCEPTED);
  /* A queued handoff remains bound to the original conversation. */
  old_conversation = b.conversation;
  b.item = b.revision = b.token = 0U;
  assert(send(&b, CAI_INPUT_OFFER, "late request to old context") == CAI_OK);
  assert(cai_input_controller_select(controller, second, NULL) == CAI_OK);
  assert(cai_input_controller_pump(controller, 8U, NULL) == CAI_OK);
  drain(&b);
  assert(b.conversation != old_conversation &&
         b.result.outcome == CAI_INPUT_REJECTED);
  assert(cai_input_queue_get_state(second, &state, NULL) == CAI_OK &&
         state.count == 0U);
  assert(cai_input_queue_lookup(first, item.id, &item, NULL) == CAI_OK);
  assert(strcmp(item.text, "accepted edited text") == 0);
  /* Receipt pressure reserves separate synchronization/resolve/stop slots. */
  for (i = 0U; i < 16U; ++i)
    assert(send(&b, CAI_INPUT_OFFER, "bounded request") == CAI_OK);
  assert(send(&b, CAI_INPUT_OFFER, "over capacity") == CAI_ERR_LIMIT);
  assert(send(&b, CAI_INPUT_SYNCHRONIZE, NULL) == CAI_OK);
  assert(send(&b, CAI_INPUT_RESOLVE, NULL) == CAI_OK);
  assert(send(&b, CAI_INPUT_STOP, NULL) == CAI_OK);
  assert(send(&b, CAI_INPUT_STOP, NULL) == CAI_ERR_LIMIT);
  assert(cai_input_controller_pump(controller, 2U, NULL) == CAI_OK);
  assert(send(&b, CAI_INPUT_OFFER, "receipt still retained") == CAI_ERR_LIMIT);
  assert(cai_input_controller_pump(controller, 32U, NULL) == CAI_OK);
  drain(&b);
  assert(send(&b, CAI_INPUT_OFFER, "receipt was drained") == CAI_OK);
  assert(cai_input_controller_pump(controller, 8U, NULL) == CAI_OK);
  drain(&b);
  assert(cai_input_queue_get_state(second, &state, NULL) == CAI_OK);
  assert(state.count == 17U && state.stopped);
  /* A slow frontend loses its subscription, not accepted work. */
  assert(cai_input_queue_lookup(second, b.item, &item, NULL) == CAI_OK);
  for (i = 0U; i < 400U; ++i)
    assert(cai_input_queue_set_ready(second, item.id, (int)(i & 1U), NULL) ==
           CAI_OK);
  drain(&b);
  assert(b.disconnected >= 2U);
  assert(send(&b, CAI_INPUT_SYNCHRONIZE, NULL) == CAI_OK);
  assert(cai_input_controller_pump(controller, 8U, NULL) == CAI_OK);
  drain(&b);
  assert(b.result.outcome == CAI_INPUT_ACCEPTED);
  /* Handoff remains backend-owned even if its frontend disconnects before
   * admission. Owner dispatch still accepts the approved input exactly once. */
  memset(&a, 0, sizeof(a));
  assert(cai_input_controller_connect(controller, 0U, &a.connection, NULL) ==
         CAI_OK);
  assert(cai_input_controller_pump(controller, 8U, NULL) == CAI_OK);
  drain(&a);
  assert(send(&a, CAI_INPUT_OFFER, "survives frontend detach") == CAI_OK);
  cai_input_connection_close(a.connection);
  a.connection = NULL;
  assert(cai_input_controller_pump(controller, 8U, NULL) == CAI_OK);
  assert(cai_input_queue_get_state(second, &state, NULL) == CAI_OK &&
         state.count == 18U);
  /* Explicit controller shutdown rejects unadmitted handoffs, with a result
   * retained for frontends that outlive the owner. It does not execute them. */
  assert(send(&b, CAI_INPUT_OFFER, "not accepted during shutdown") == CAI_OK);
  /* Controller teardown never clears either conversation's pending work. */
  cai_input_controller_close(controller);
  assert(cai_input_queue_get_state(first, &state, NULL) == CAI_OK &&
         state.count == 1U);
  assert(cai_input_queue_get_state(second, &state, NULL) == CAI_OK &&
         state.count == 18U);
  drain(&b);
  assert(b.result.outcome == CAI_INPUT_REJECTED &&
         b.result.code == CAI_ERR_CANCELLED);
  cai_input_connection_close(b.connection);
  cai_input_queue_close(first);
  cai_input_queue_close(second);
  /* A receiver reference retains allocation without retaining storage access.
   */
  assert(cai_input_controller_open(NULL, &controller, NULL) == CAI_OK);
  assert(cai_input_queue_open(NULL, &first, NULL) == CAI_OK);
  assert(cai_input_controller_select(controller, first, NULL) == CAI_OK);
  cai_input_queue_close(first);
  assert(cai_input_controller_select(controller, NULL, NULL) == CAI_OK);
  cai_input_controller_close(controller);
  return 0;
}
