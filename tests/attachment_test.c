#include <assert.h>
#include <cai/attachment.h>
#include <cai/input_controller.h>
#include <pthread.h>
#include <string.h>

static cai_attachment_state state(cai_attachment_controller *controller) {
  cai_attachment_state out;
  assert(cai_attachment_controller_get_state(controller, &out, NULL) == CAI_OK);
  return out;
}
static void lifetime(void) {
  cai_attachment_controller *controller = NULL;
  cai_attachment_config config;
  uint64_t a, b, c, generation;
  cai_attachment_config_init(&config);
  assert(config.persist == 0 && config.first_attachment_timeout_ms == 0U);
  assert(cai_attachment_controller_open(&config, &controller, NULL) == CAI_OK);
  assert(state(controller).phase == CAI_ATTACHMENT_STARTING);
  assert(cai_attachment_controller_attach(controller, 100U, &a, NULL) ==
             CAI_ERR_INVALID &&
         a == 0U);
  /* Slow restoration does not consume the first-attachment grace period. */
  assert(cai_attachment_controller_advance(controller, 90000U, NULL) == CAI_OK);
  assert(state(controller).phase == CAI_ATTACHMENT_STARTING);
  assert(cai_attachment_controller_ready(controller, 90000U, NULL) == CAI_OK);
  assert(state(controller).deadline_ms == 120000U);
  assert(cai_attachment_controller_ready(controller, 90000U, NULL) ==
         CAI_ERR_INVALID);
  assert(cai_attachment_controller_attach(controller, 119999U, &a, NULL) ==
         CAI_OK);
  assert(a != 0U && state(controller).count == 1U &&
         state(controller).controller == a &&
         state(controller).deadline_ms == 0U);
  generation = state(controller).generation;
  assert(cai_attachment_controller_authorize(controller, a, generation, NULL) ==
         CAI_OK);
  assert(cai_attachment_controller_attach(controller, 120000U, &b, NULL) ==
         CAI_OK);
  assert(a != b && state(controller).count == 2U &&
         state(controller).controller == a);
  assert(cai_attachment_controller_authorize(controller, b, generation, NULL) ==
         CAI_ERR_INVALID);
  assert(cai_attachment_controller_detach(controller, a, NULL) == CAI_OK);
  /* An observer keeps the nonpersistent server alive, without silently
   * becoming its controller. */
  assert(state(controller).phase == CAI_ATTACHMENT_READY &&
         state(controller).count == 1U && state(controller).controller == 0U);
  assert(cai_attachment_controller_authorize(controller, a, generation, NULL) ==
         CAI_ERR_INVALID);
  assert(cai_attachment_controller_authorize(controller, b,
                                             state(controller).generation,
                                             NULL) == CAI_ERR_INVALID);
  assert(cai_attachment_controller_takeover(controller, b, &generation, NULL) ==
         CAI_OK);
  assert(cai_attachment_controller_authorize(controller, b, generation, NULL) ==
         CAI_OK);
  assert(cai_attachment_controller_detach(controller, b, NULL) == CAI_OK);
  assert(state(controller).phase == CAI_ATTACHMENT_STOPPING &&
         state(controller).stop_reason == CAI_ATTACHMENT_LAST_DETACHED &&
         state(controller).count == 0U);
  assert(cai_attachment_controller_attach(controller, 120001U, &c, NULL) ==
             CAI_ERR_INVALID &&
         c == 0U);
  assert(cai_attachment_controller_takeover(controller, b, &generation, NULL) ==
         CAI_ERR_INVALID);
  assert(cai_attachment_controller_stop(controller, NULL) == CAI_OK);
  assert(state(controller).stop_reason == CAI_ATTACHMENT_LAST_DETACHED);
  cai_attachment_controller_close(controller);

  config.persist = 1;
  assert(cai_attachment_controller_open(&config, &controller, NULL) == CAI_OK);
  assert(cai_attachment_controller_ready(controller, 0U, NULL) == CAI_OK);
  assert(state(controller).persist && state(controller).deadline_ms == 0U);
  assert(cai_attachment_controller_advance(controller, 999999U, NULL) ==
         CAI_OK);
  assert(state(controller).phase == CAI_ATTACHMENT_READY);
  assert(cai_attachment_controller_attach(controller, 999999U, &a, NULL) ==
         CAI_OK);
  generation = state(controller).generation;
  assert(cai_attachment_controller_detach(controller, a, NULL) == CAI_OK);
  assert(state(controller).phase == CAI_ATTACHMENT_READY &&
         state(controller).controller == 0U && state(controller).count == 0U);
  assert(cai_attachment_controller_attach(controller, 1000000U, &b, NULL) ==
         CAI_OK);
  assert(b != a && state(controller).generation > generation);
  assert(cai_attachment_controller_stop(controller, NULL) == CAI_OK);
  assert(cai_attachment_controller_authorize(controller, b,
                                             state(controller).generation,
                                             NULL) == CAI_ERR_INVALID);
  assert(cai_attachment_controller_detach(controller, b, NULL) == CAI_OK);
  assert(state(controller).stop_reason == CAI_ATTACHMENT_STOP_REQUESTED);
  cai_attachment_controller_close(controller);
}
static void timing(void) {
  cai_attachment_controller *controller = NULL;
  cai_attachment_config config;
  uint64_t id;
  cai_attachment_config_init(&config);
  config.first_attachment_timeout_ms = 17U;
  assert(cai_attachment_controller_open(&config, &controller, NULL) == CAI_OK);
  assert(cai_attachment_controller_ready(controller, 1000U, NULL) == CAI_OK);
  assert(cai_attachment_controller_advance(controller, 1016U, NULL) == CAI_OK);
  assert(state(controller).phase == CAI_ATTACHMENT_READY);
  assert(cai_attachment_controller_advance(controller, 1015U, NULL) ==
         CAI_ERR_INVALID);
  assert(state(controller).deadline_ms == 1017U);
  /* Admission checks expiry even if the host's timer and accept are ready
   * together. Stopping cannot be reversed by a late first attachment. */
  assert(cai_attachment_controller_attach(controller, 1017U, &id, NULL) ==
             CAI_ERR_INVALID &&
         id == 0U);
  assert(state(controller).phase == CAI_ATTACHMENT_STOPPING &&
         state(controller).stop_reason == CAI_ATTACHMENT_STARTUP_ABANDONED &&
         state(controller).deadline_ms == 0U);
  cai_attachment_controller_close(controller);
  assert(cai_attachment_controller_open(NULL, &controller, NULL) == CAI_OK);
  assert(cai_attachment_controller_ready(controller, UINT64_MAX, NULL) ==
         CAI_ERR_LIMIT);
  assert(state(controller).phase == CAI_ATTACHMENT_STARTING);
  assert(cai_attachment_controller_ready(controller, 0U, NULL) == CAI_OK);
  assert(cai_attachment_controller_advance(controller, 30000U, NULL) == CAI_OK);
  assert(state(controller).stop_reason == CAI_ATTACHMENT_STARTUP_ABANDONED);
  cai_attachment_controller_close(controller);
}
static void capacity_and_roles(void) {
  cai_attachment_controller *controller = NULL;
  cai_attachment_config config = {1, 0U};
  uint64_t ids[CAI_ATTACHMENT_LIMIT], replacement, old, next;
  size_t i;
  assert(cai_attachment_controller_open(&config, &controller, NULL) == CAI_OK);
  assert(cai_attachment_controller_ready(controller, 0U, NULL) == CAI_OK);
  for (i = 0U; i < CAI_ATTACHMENT_LIMIT; ++i)
    assert(cai_attachment_controller_attach(controller, 0U, &ids[i], NULL) ==
           CAI_OK);
  assert(cai_attachment_controller_attach(controller, 0U, &replacement, NULL) ==
             CAI_ERR_LIMIT &&
         replacement == 0U);
  old = state(controller).generation;
  assert(cai_attachment_controller_takeover(controller, ids[0], &next, NULL) ==
             CAI_OK &&
         next == old);
  assert(cai_attachment_controller_takeover(controller, ids[1], &next, NULL) ==
             CAI_OK &&
         next > old);
  assert(cai_attachment_controller_authorize(controller, ids[0], old, NULL) ==
         CAI_ERR_INVALID);
  assert(cai_attachment_controller_authorize(controller, ids[1], next, NULL) ==
         CAI_OK);
  assert(cai_attachment_controller_takeover(controller, ids[0], &next, NULL) ==
             CAI_OK &&
         next > old);
  assert(cai_attachment_controller_authorize(controller, ids[0], old, NULL) ==
         CAI_ERR_INVALID);
  assert(cai_attachment_controller_detach(controller, ids[2], NULL) == CAI_OK);
  assert(cai_attachment_controller_attach(controller, 1U, &replacement, NULL) ==
             CAI_OK &&
         replacement > ids[CAI_ATTACHMENT_LIMIT - 1U]);
  assert(cai_attachment_controller_takeover(controller, ids[2], &next, NULL) ==
         CAI_ERR_INVALID);
  assert(cai_attachment_controller_detach(controller, ids[2], NULL) ==
         CAI_ERR_INVALID);
  assert(state(controller).count == CAI_ATTACHMENT_LIMIT);
  cai_attachment_controller_close(controller);
}
static void *wrong_owner(void *context) {
  cai_attachment_controller *controller = context;
  cai_attachment_state out;
  uint64_t id;
  assert(cai_attachment_controller_ready(controller, 0U, NULL) ==
         CAI_ERR_INVALID);
  assert(cai_attachment_controller_get_state(controller, &out, NULL) ==
         CAI_ERR_INVALID);
  assert(cai_attachment_controller_attach(controller, 0U, &id, NULL) ==
         CAI_ERR_INVALID);
  assert(cai_attachment_controller_stop(controller, NULL) == CAI_ERR_INVALID);
  cai_attachment_controller_close(controller);
  return NULL;
}
static void invalid(void) {
  cai_attachment_controller *controller = NULL;
  cai_attachment_config config = {2, 0U};
  pthread_t thread;
  uint64_t id;
  assert(cai_attachment_controller_open(NULL, NULL, NULL) == CAI_ERR_INVALID);
  assert(cai_attachment_controller_open(&config, &controller, NULL) ==
             CAI_ERR_INVALID &&
         controller == NULL);
  assert(cai_attachment_controller_open(NULL, &controller, NULL) == CAI_OK);
  assert(pthread_create(&thread, NULL, wrong_owner, controller) == 0);
  assert(pthread_join(thread, NULL) == 0);
  assert(cai_attachment_controller_get_state(controller, NULL, NULL) ==
         CAI_ERR_INVALID);
  assert(cai_attachment_controller_attach(controller, 0U, NULL, NULL) ==
         CAI_ERR_INVALID);
  assert(state(controller).phase == CAI_ATTACHMENT_STARTING);
  assert(cai_attachment_controller_stop(controller, NULL) == CAI_OK);
  assert(cai_attachment_controller_ready(controller, 0U, NULL) ==
         CAI_ERR_INVALID);
  assert(cai_attachment_controller_attach(controller, 0U, &id, NULL) ==
         CAI_ERR_INVALID);
  cai_attachment_controller_close(controller);
  cai_attachment_controller_close(NULL);
}

typedef struct client {
  cai_input_connection *connection;
  cai_input_scope scope;
  unsigned long long request;
  cai_input_result result;
} client;
static void drain(client *c) {
  cai_input_observation *event;
  for (;;) {
    assert(cai_input_connection_poll(c->connection, &event, NULL) == CAI_OK);
    if (event == NULL)
      break;
    if (event->type == CAI_INPUT_OBSERVATION_SNAPSHOT)
      c->scope.conversation = event->conversation;
    if (event->type == CAI_INPUT_OBSERVATION_RESULT)
      c->result = event->result;
    cai_input_observation_close(event);
  }
}
static void send(client *c, cai_input_operation operation) {
  cai_input_request request;
  memset(&request, 0, sizeof(request));
  request.operation = operation;
  request.request_id = ++c->request;
  request.text = operation == CAI_INPUT_OFFER ? "admitted once" : NULL;
  assert(cai_input_connection_request(c->connection, &c->scope, &request,
                                      NULL) == CAI_OK);
}
static int authorize(void *context, uint64_t id, uint64_t generation,
                     cai_error *error) {
  return cai_attachment_controller_authorize(context, id, generation, error);
}
typedef struct admission_writes {
  size_t accepted, rejected;
} admission_writes;
static int persist(void *context, const cai_input_record *record,
                   unsigned long long *position, cai_error *error) {
  admission_writes *count = context;
  (void)error;
  *position = 0U;
  if (record->type == CAI_INPUT_RECORD_REQUEST) {
    if (record->result.outcome == CAI_INPUT_ACCEPTED)
      ++count->accepted;
    else {
      assert(record->result.outcome == CAI_INPUT_REJECTED &&
             record->request.text == NULL);
      assert(record->before_revision == record->after_revision);
      ++count->rejected;
    }
  }
  return CAI_OK;
}
static void admission(void) {
  cai_attachment_controller *roles = NULL;
  cai_input_controller *controller = NULL;
  cai_input_controller_config input_config;
  cai_input_queue *queue = NULL;
  cai_input_queue_config queue_config;
  cai_input_state queue_state;
  client a, b;
  uint64_t first, second, generation;
  cai_input_operation operation;
  admission_writes writes;
  memset(&writes, 0, sizeof(writes));
  memset(&a, 0, sizeof(a));
  memset(&b, 0, sizeof(b));
  assert(cai_attachment_controller_open(NULL, &roles, NULL) == CAI_OK);
  assert(cai_attachment_controller_ready(roles, 0U, NULL) == CAI_OK);
  assert(cai_attachment_controller_attach(roles, 0U, &first, NULL) == CAI_OK);
  assert(cai_attachment_controller_attach(roles, 0U, &second, NULL) == CAI_OK);
  input_config.authorize = authorize;
  input_config.authorize_context = roles;
  cai_input_queue_config_init(&queue_config);
  queue_config.persist = persist;
  queue_config.persist_context = &writes;
  assert(cai_input_queue_open(&queue_config, &queue, NULL) == CAI_OK);
  assert(cai_input_controller_open(&input_config, &controller, NULL) == CAI_OK);
  assert(cai_input_controller_select(controller, queue, NULL) == CAI_OK);
  assert(cai_input_controller_connect(controller, first, &a.connection, NULL) ==
         CAI_OK);
  assert(cai_input_controller_connect(controller, second, &b.connection,
                                      NULL) == CAI_OK);
  assert(cai_input_controller_pump(controller, 8U, NULL) == CAI_OK);
  drain(&a);
  drain(&b);
  a.scope.generation = b.scope.generation = state(roles).generation;
  /* A copied handoff cannot survive revocation merely because its sender
   * regains control before actual owner-thread admission. */
  send(&a, CAI_INPUT_OFFER);
  assert(cai_attachment_controller_takeover(roles, second, &generation, NULL) ==
         CAI_OK);
  assert(cai_attachment_controller_takeover(roles, first, &generation, NULL) ==
         CAI_OK);
  send(&b, CAI_INPUT_OFFER);
  assert(cai_input_controller_pump(controller, 8U, NULL) == CAI_OK);
  drain(&a);
  drain(&b);
  assert(a.result.outcome == CAI_INPUT_REJECTED &&
         b.result.outcome == CAI_INPUT_REJECTED && writes.accepted == 0U &&
         writes.rejected == 2U);
  assert(strstr(a.result.reason, "stale") != NULL);
  a.scope.generation = generation;
  send(&a, CAI_INPUT_OFFER);
  assert(cai_input_controller_pump(controller, 8U, NULL) == CAI_OK);
  drain(&a);
  drain(&b);
  assert(a.result.outcome == CAI_INPUT_ACCEPTED && writes.accepted == 1U);
  /* Observers may synchronize/reconcile, but STOP/approval/etc are mutations.
   */
  send(&b, CAI_INPUT_SYNCHRONIZE);
  assert(cai_input_controller_pump(controller, 8U, NULL) == CAI_OK);
  drain(&b);
  assert(b.result.outcome == CAI_INPUT_ACCEPTED);
  for (operation = CAI_INPUT_OFFER; operation <= CAI_INPUT_RESOLVE;
       operation = (cai_input_operation)(operation + 1)) {
    if (operation == CAI_INPUT_SYNCHRONIZE || operation == CAI_INPUT_RESOLVE)
      continue;
    send(&b, operation);
    assert(cai_input_controller_pump(controller, 8U, NULL) == CAI_OK);
    drain(&b);
    assert(b.result.outcome == CAI_INPUT_REJECTED && writes.accepted == 1U);
  }
  assert(cai_input_queue_get_state(queue, &queue_state, NULL) == CAI_OK &&
         queue_state.count == 1U && !queue_state.stopped);
  /* Shutdown fences unadmitted mutations, preserving previously accepted work.
   */
  send(&a, CAI_INPUT_OFFER);
  assert(cai_attachment_controller_stop(roles, NULL) == CAI_OK);
  assert(cai_input_controller_pump(controller, 8U, NULL) == CAI_OK);
  drain(&a);
  assert(a.result.outcome == CAI_INPUT_REJECTED && writes.accepted == 1U);
  assert(cai_input_queue_get_state(queue, &queue_state, NULL) == CAI_OK &&
         queue_state.count == 1U);
  cai_input_connection_close(a.connection);
  cai_input_connection_close(b.connection);
  cai_input_controller_close(controller);
  cai_input_queue_close(queue);
  cai_attachment_controller_close(roles);
}
int main(void) {
  lifetime();
  timing();
  capacity_and_roles();
  invalid();
  admission();
  return 0;
}
