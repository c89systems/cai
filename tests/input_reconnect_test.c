#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <cai/input_controller.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct journal {
  cai_input_record records[64];
  size_t count;
} journal;
typedef struct gate {
  uint64_t generation;
  size_t calls;
} gate;
typedef struct frontend {
  cai_input_connection *connection;
  unsigned long long conversation, producer, high;
  cai_input_result result, failure;
  size_t snapshots, results, disconnected;
  int snapshot_active;
} frontend;
static int persist(void *context, const cai_input_record *record,
                   unsigned long long *position, cai_error *error) {
  journal *log = context;
  cai_input_record *copy;
  (void)error;
  assert(log->count < 64U && record->input_event_count == 0U);
  copy = &log->records[log->count++];
  *copy = *record;
  if (record->request.text != NULL) {
    copy->request.text = strdup(record->request.text);
    assert(copy->request.text != NULL);
  }
  if (record->entry.text != NULL) {
    copy->entry.text = strdup(record->entry.text);
    assert(copy->entry.text != NULL);
  }
  if (record->type == CAI_INPUT_RECORD_REQUEST &&
      record->result.outcome == CAI_INPUT_REJECTED)
    assert(copy->request.text == NULL &&
           record->before_revision == record->after_revision);
  if (position != NULL)
    *position = log->count;
  return CAI_OK;
}
static int authorize(void *context, uint64_t attachment, uint64_t generation,
                     cai_error *error) {
  gate *scope = context;
  ++scope->calls;
  if (attachment == 42U && generation == scope->generation)
    return CAI_OK;
  if (error != NULL) {
    error->code = CAI_ERR_INVALID;
    error->message = strdup("stale authority");
    assert(error->message != NULL);
  }
  return CAI_ERR_INVALID;
}
static void drain(frontend *f) {
  cai_input_observation *value = NULL;
  for (;;) {
    assert(cai_input_connection_poll(f->connection, &value, NULL) == CAI_OK);
    if (value == NULL)
      return;
    if (value->producer != 0U) {
      f->producer = value->producer;
      f->high = value->last_request;
    }
    if (value->type == CAI_INPUT_OBSERVATION_SNAPSHOT_END) {
      f->conversation = value->conversation;
      ++f->snapshots;
      f->snapshot_active = 0;
    } else if (value->type == CAI_INPUT_OBSERVATION_SNAPSHOT_BEGIN) {
      f->snapshot_active = 1;
    } else if (value->type == CAI_INPUT_OBSERVATION_RESULT) {
      f->result = value->result;
      ++f->results;
    } else if (value->type == CAI_INPUT_OBSERVATION_DISCONNECTED) {
      f->snapshot_active = 0;
      f->failure = value->result;
      ++f->disconnected;
    }
    cai_input_observation_close(value);
  }
}
static void send(frontend *f, unsigned long long id, uint64_t generation,
                 cai_input_operation operation, unsigned long long target) {
  cai_input_request request;
  cai_input_scope scope;
  memset(&request, 0, sizeof(request));
  request.request_id = id;
  request.operation = operation;
  request.text = operation == CAI_INPUT_OFFER ? "one accepted input" : NULL;
  request.target_request_id = target;
  scope.conversation = f->conversation;
  scope.generation = generation;
  assert(cai_input_connection_request(f->connection, &scope, &request, NULL) ==
         CAI_OK);
}
static void pump(cai_input_controller *controller, frontend *f) {
  size_t steps = 0U;
  do {
    assert(cai_input_controller_pump(controller, 32U, NULL) == CAI_OK);
    if (f != NULL)
      drain(f);
    assert(++steps < 128U);
  } while (f != NULL && f->snapshot_active);
}
static void resume(cai_input_controller *controller, frontend *f,
                   uint64_t attachment, unsigned long long conversation,
                   unsigned long long producer) {
  memset(f, 0, sizeof(*f));
  f->conversation = conversation;
  assert(cai_input_controller_resume(controller, attachment, conversation,
                                     producer, &f->connection, NULL) == CAI_OK);
  pump(controller, f);
}
static void close_frontend(cai_input_controller *controller, frontend *f) {
  cai_input_connection_close(f->connection);
  f->connection = NULL;
  pump(controller, NULL);
}
static void count(cai_input_queue *queue, size_t expected) {
  cai_input_state state;
  assert(cai_input_queue_get_state(queue, &state, NULL) == CAI_OK &&
         state.count == expected);
}
static void free_text(const char *text) {
  void *owned;
  memcpy(&owned, &text, sizeof(owned));
  free(owned);
}
static void *run(void *context) {
  cai_input_controller *controller;
  cai_input_controller_config config;
  cai_input_queue_config storage;
  cai_input_queue *queue, *other;
  cai_input_connection *invalid = NULL;
  frontend f, bad, fresh;
  journal *log = calloc(1U, sizeof(*log));
  gate authority;
  size_t saved, i, calls;
  unsigned long long conversation, producer;
  (void)context;
  assert(log != NULL);
  memset(&authority, 0, sizeof(authority));
  authority.generation = 1U;
  memset(&config, 0, sizeof(config));
  config.authorize = authorize;
  config.authorize_context = &authority;
  cai_input_queue_config_init(&storage);
  storage.persist = persist;
  storage.persist_context = log;
  assert(cai_input_queue_open(&storage, &queue, NULL) == CAI_OK);
  assert(cai_input_controller_open(&config, &controller, NULL) == CAI_OK);
  assert(cai_input_controller_select(controller, queue, NULL) == CAI_OK);
  memset(&f, 0, sizeof(f));
  assert(cai_input_controller_connect(controller, 42U, &f.connection, NULL) ==
         CAI_OK);
  pump(controller, &f);
  conversation = f.conversation;
  producer = f.producer;
  assert(producer != 0U && f.high == 0U && log->count == 1U);
  send(&f, 1U, 1U, CAI_INPUT_OFFER, 0U);
  authority.generation =
      2U; /* Revoked after handoff, before owner admission. */
  pump(controller, &f);
  assert(f.result.outcome == CAI_INPUT_REJECTED && f.high == 1U);
  assert(log->count == 2U && authority.calls == 1U);
  count(queue, 0U);
  close_frontend(controller, &f);
  resume(controller, &f, 42U, conversation, producer);
  assert(f.snapshots == 1U && f.producer == producer && f.high == 1U &&
         log->count == 2U);
  send(&f, 1U, 2U, CAI_INPUT_OFFER, 0U);
  pump(controller, &f);
  assert(f.result.outcome == CAI_INPUT_REJECTED && authority.calls == 1U &&
         log->count == 2U);
  send(&f, 2U, 2U, CAI_INPUT_OFFER, 0U);
  pump(controller, NULL); /* Lose the acceptance acknowledgement. */
  assert(log->count == 3U);
  close_frontend(controller, &f);
  resume(controller, &f, 84U, conversation, producer); /* Now an observer. */
  assert(f.producer == producer && f.high == 2U && log->count == 3U);
  calls = authority.calls;
  send(&f, 2U, 2U, CAI_INPUT_OFFER, 0U);
  pump(controller, &f);
  assert(f.result.outcome == CAI_INPUT_ACCEPTED && log->count == 3U &&
         authority.calls == calls);
  count(queue, 1U);
  send(&f, 3U, 2U, CAI_INPUT_OFFER, 0U);
  pump(controller, &f);
  assert(f.result.outcome == CAI_INPUT_REJECTED && log->count == 4U);
  resume(controller, &bad, 42U, conversation, producer);
  assert(bad.disconnected == 1U && bad.failure.code == CAI_ERR_LIMIT);
  close_frontend(controller, &bad);
  resume(controller, &bad, 42U, conversation, 9999U);
  assert(bad.disconnected == 1U && bad.failure.code == CAI_ERR_INVALID &&
         log->count == 4U);
  close_frontend(controller, &bad);
  assert(cai_input_controller_resume(controller, 42U, conversation + 1U,
                                     producer, &invalid,
                                     NULL) == CAI_ERR_INVALID &&
         invalid == NULL);
  send(&f, 4U, 2U, CAI_INPUT_SYNCHRONIZE, 0U);
  pump(controller, &f);
  assert(f.result.outcome == CAI_INPUT_ACCEPTED && f.high == 4U &&
         log->count == 4U);
  close_frontend(controller, &f);
  memset(&bad, 0, sizeof(bad));
  bad.conversation = conversation;
  assert(cai_input_controller_resume(controller, 42U, conversation, producer,
                                     &bad.connection, NULL) == CAI_OK);
  send(&bad, 5U, 2U, CAI_INPUT_OFFER, 0U);
  assert(cai_input_queue_open(NULL, &other, NULL) == CAI_OK);
  assert(cai_input_controller_select(controller, other, NULL) == CAI_OK);
  pump(controller, &bad);
  assert(bad.disconnected == 1U && bad.result.outcome == CAI_INPUT_REJECTED);
  assert(log->count == 4U);
  memset(&fresh, 0, sizeof(fresh));
  assert(cai_input_controller_connect(controller, 42U, &fresh.connection,
                                      NULL) == CAI_OK);
  pump(controller, &fresh);
  assert(fresh.producer ==
         1U); /* Failed resume reserved nothing in the new queue. */
  close_frontend(controller, &bad);
  close_frontend(controller, &fresh);
  cai_input_controller_close(controller);
  cai_input_queue_close(other);
  cai_input_queue_close(queue);
  /* Reconstruct the logical producer/receipts after a server restart. */
  saved = log->count;
  assert(cai_input_queue_open(&storage, &queue, NULL) == CAI_OK);
  for (i = 0U; i < saved; ++i)
    assert(cai_input_queue_restore(queue, &log->records[i], i + 1U, NULL) ==
           CAI_OK);
  assert(cai_input_controller_open(&config, &controller, NULL) == CAI_OK);
  assert(cai_input_controller_select(controller, queue, NULL) == CAI_OK);
  resume(controller, &f, 84U, 1U, producer);
  assert(f.producer == producer && f.high == 3U && log->count == saved);
  calls = authority.calls;
  send(&f, 5U, 2U, CAI_INPUT_RESOLVE, 2U);
  pump(controller, &f);
  assert(f.result.outcome == CAI_INPUT_ACCEPTED && authority.calls == calls &&
         log->count == saved);
  for (i = 6U; i < 26U; ++i) {
    send(&f, i, 2U, CAI_INPUT_OFFER, 0U);
    pump(controller, &f);
    assert(f.result.outcome == CAI_INPUT_REJECTED);
  }
  saved = log->count;
  send(&f, 1U, 2U, CAI_INPUT_OFFER, 0U);
  pump(controller, &f);
  assert(f.result.outcome == CAI_INPUT_UNKNOWN && log->count == saved);
  count(queue, 1U);
  close_frontend(controller, &f);
  for (i = 0U; i < 17U; ++i) {
    memset(&fresh, 0, sizeof(fresh));
    assert(cai_input_controller_connect(controller, 42U, &fresh.connection,
                                        NULL) == CAI_OK);
    pump(controller, &fresh);
    close_frontend(controller, &fresh);
  }
  saved = log->count;
  resume(controller, &bad, 42U, 1U, producer);
  assert(bad.failure.code == CAI_ERR_INVALID && bad.disconnected == 1U &&
         log->count == saved);
  close_frontend(controller, &bad);
  count(queue, 1U);
  cai_input_controller_close(controller);
  cai_input_queue_close(queue);
  for (i = 0U; i < log->count; ++i) {
    free_text(log->records[i].request.text);
    free_text(log->records[i].entry.text);
  }
  free(log);
  return NULL;
}
int main(void) {
  pthread_t worker;
  assert(pthread_create(&worker, NULL, run, NULL) == 0);
  assert(pthread_join(worker, NULL) == 0);
  return 0;
}
