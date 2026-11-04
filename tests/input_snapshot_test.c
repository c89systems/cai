#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <cai/input_controller.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct view {
  unsigned long long conversation, revision;
  size_t rows, expected, begins, ends, resets;
  int active;
} view;
#ifdef CAI_TEST_ALLOCATION_FAILURE
static int fail_alloc;
void *__real_malloc(size_t);
void *__wrap_malloc(size_t);
void *__wrap_malloc(size_t size) {
  if (fail_alloc != 0 && --fail_alloc == 0)
    return NULL;
  return __real_malloc(size);
}
static void failures(cai_input_controller *controller, cai_input_queue *queue,
                     cai_input_queue_client *producer) {
  cai_input_connection *connection;
  cai_input_observation *event;
  cai_input_entry item;
  int fault;
  for (fault = 0; fault < 3; ++fault) {
    int failed = 0;
    int ended = 0;
    size_t steps, rows = 0U;
    cai_input_connection *healthy;
    assert(cai_input_controller_connect(controller, 0U, &connection, NULL) ==
           CAI_OK);
    assert(cai_input_controller_pump(controller, 32U, NULL) == CAI_OK);
    while (1) {
      assert(cai_input_connection_poll(connection, &event, NULL) == CAI_OK);
      if (event == NULL)
        break;
      cai_input_observation_close(event);
    }
    if (fault == 0) {
      assert(cai_input_queue_entry_at(producer, 0U, &item, NULL) == CAI_OK);
      fail_alloc = 1;
      assert(cai_input_queue_set_ready(queue, item.id, !item.ready, NULL) ==
             CAI_OK);
    } else {
      fail_alloc = fault;
      assert(cai_input_controller_pump(controller, 32U, NULL) == CAI_OK);
    }
    assert(fail_alloc == 0);
    while (1) {
      assert(cai_input_connection_poll(connection, &event, NULL) == CAI_OK);
      if (event == NULL)
        break;
      if (event->type == CAI_INPUT_OBSERVATION_DISCONNECTED) {
        assert(event->result.code == CAI_ERR_NOMEM);
        ++failed;
      }
      cai_input_observation_close(event);
    }
    assert(failed == 1);
    assert(cai_input_controller_connect(controller, 0U, &healthy, NULL) ==
           CAI_OK);
    for (steps = 0U; steps < 32U && !ended; ++steps) {
      assert(cai_input_controller_pump(controller, 32U, NULL) == CAI_OK);
      while (1) {
        assert(cai_input_connection_poll(healthy, &event, NULL) == CAI_OK);
        if (event == NULL)
          break;
        if (event->type == CAI_INPUT_OBSERVATION_SNAPSHOT_ITEM)
          ++rows;
        if (event->type == CAI_INPUT_OBSERVATION_SNAPSHOT_END)
          ended = 1;
        cai_input_observation_close(event);
      }
    }
    assert(ended && rows == 13U);
    cai_input_connection_close(healthy);
    cai_input_connection_close(connection);
    assert(cai_input_controller_pump(controller, 32U, NULL) == CAI_OK);
  }
}
#endif
static size_t receive(cai_input_connection *connection, view *v) {
  cai_input_observation *event;
  size_t messages = 0U, bytes = 0U;
  for (;;) {
    assert(cai_input_connection_poll(connection, &event, NULL) == CAI_OK);
    if (event == NULL)
      break;
    ++messages;
    switch (event->type) {
    case CAI_INPUT_OBSERVATION_SNAPSHOT_BEGIN:
      assert(!v->active);
      v->active = 1;
      v->rows = 0U;
      v->expected = event->state.count;
      v->conversation = event->conversation;
      v->revision = event->state.view_revision;
      ++v->begins;
      break;
    case CAI_INPUT_OBSERVATION_SNAPSHOT_ITEM:
      assert(v->active && event->index == v->rows &&
             event->state.view_revision == v->revision &&
             event->conversation == v->conversation &&
             event->state.count == v->expected && event->entry.text != NULL);
      assert(strlen(event->entry.text) == 66048U);
      bytes += strlen(event->entry.text);
      ++v->rows;
      break;
    case CAI_INPUT_OBSERVATION_SNAPSHOT_END:
      assert(v->active && v->rows == v->expected && event->index == v->rows &&
             event->state.view_revision == v->revision);
      ++v->ends;
      v->active = 0;
      break;
    case CAI_INPUT_OBSERVATION_DISCONNECTED:
      v->active = 0;
      ++v->resets;
      break;
    default:
      assert(event->type == CAI_INPUT_OBSERVATION_CHANGE);
      break;
    }
    cai_input_observation_close(event);
  }
  assert(messages <= 4U && bytes <= 2U * 66048U);
  return messages;
}
static void offer(cai_input_queue_client *client, unsigned long long id,
                  const char *text) {
  cai_input_request request;
  cai_input_result result;
  memset(&request, 0, sizeof(request));
  request.request_id = id;
  request.operation = CAI_INPUT_OFFER;
  request.text = text;
  assert(cai_input_queue_request(client, &request, &result, NULL) == CAI_OK &&
         result.outcome == CAI_INPUT_ACCEPTED);
}
static void *run(void *unused) {
  cai_input_queue *queue;
  cai_input_queue_client *producer;
  cai_input_controller *controller;
  cai_input_connection *slow, *fast;
  char *text = malloc(66049U);
  struct pollfd fd;
  view a, b;
  size_t i;
  (void)unused;
  assert(text != NULL);
  memset(text, 'x', 66048U);
  text[66048U] = '\0';
  memset(&a, 0, sizeof(a));
  memset(&b, 0, sizeof(b));
  assert(cai_input_queue_open(NULL, &queue, NULL) == CAI_OK);
  assert(cai_input_queue_client_open(queue, &producer, NULL) == CAI_OK);
  for (i = 1U; i <= 12U; ++i)
    offer(producer, i, text);
  assert(cai_input_controller_open(NULL, &controller, NULL) == CAI_OK);
  assert(cai_input_controller_select(controller, queue, NULL) == CAI_OK);
  assert(cai_input_controller_connect(controller, 0U, &slow, NULL) == CAI_OK);
  assert(cai_input_controller_pump(controller, 32U, NULL) == CAI_OK);
  /* A nonreading receiver cannot make the producer gather the whole value
   * or keep its owner spinning. Only a bounded row window is retained. */
  for (i = 0U; i < 100U; ++i)
    assert(cai_input_controller_pump(controller, 32U, NULL) == CAI_OK);
  fd.fd = cai_input_controller_fd(controller);
  fd.events = POLLIN;
  assert(poll(&fd, 1U, 0) == 0);
  assert(receive(slow, &a) > 1U && a.begins == 1U && a.ends == 0U &&
         a.rows < a.expected);
  assert(poll(&fd, 1U, 0) == 1); /* Consumption releases producer credit. */
  offer(producer, 13U, text);    /* Invalidate the partial baseline. */
  assert(cai_input_controller_connect(controller, 0U, &fast, NULL) == CAI_OK);
  for (i = 0U; i < 32U && (a.ends == 0U || b.ends == 0U); ++i) {
    assert(cai_input_controller_pump(controller, 32U, NULL) == CAI_OK);
    (void)receive(slow, &a);
    (void)receive(fast, &b);
  }
  assert(a.resets == 1U && a.begins == 2U && a.ends == 1U && a.rows == 13U &&
         b.rows == 13U && b.ends == 1U && a.revision == b.revision &&
         a.conversation == b.conversation);
  /* Teardown halfway through another snapshot leaves no borrowed rows. */
  cai_input_connection_close(slow);
  cai_input_connection_close(fast);
  assert(cai_input_controller_pump(controller, 32U, NULL) == CAI_OK);
#ifdef CAI_TEST_ALLOCATION_FAILURE
  failures(controller, queue, producer);
#endif
  {
    cai_input_queue *other;
    cai_input_queue_client *check;
    cai_input_request request;
    cai_input_scope scope;
    cai_input_state state;
    assert(cai_input_controller_connect(controller, 0U, &slow, NULL) == CAI_OK);
    assert(cai_input_controller_conversation(controller, &scope.conversation,
                                             NULL) == CAI_OK);
    scope.generation = 0U;
    memset(&request, 0, sizeof(request));
    request.request_id = 1U;
    request.operation = CAI_INPUT_OFFER;
    request.text = "must stay in the original conversation";
    assert(cai_input_connection_request(slow, &scope, &request, NULL) ==
           CAI_OK);
    cai_input_connection_close(slow);
    assert(cai_input_queue_open(NULL, &other, NULL) == CAI_OK);
    assert(cai_input_controller_select(controller, other, NULL) == CAI_OK);
    assert(cai_input_controller_pump(controller, 32U, NULL) == CAI_OK);
    assert(cai_input_queue_client_open(other, &check, NULL) == CAI_OK &&
           cai_input_queue_client_id(check) == 1U);
    assert(cai_input_queue_get_state(other, &state, NULL) == CAI_OK &&
           state.count == 0U);
    cai_input_queue_client_close(check);
    assert(cai_input_controller_select(controller, queue, NULL) == CAI_OK);
    cai_input_queue_close(other);
  }
  assert(cai_input_controller_connect(controller, 0U, &slow, NULL) == CAI_OK);
  assert(cai_input_controller_pump(controller, 32U, NULL) == CAI_OK);
  cai_input_controller_close(controller);
  while (1) {
    cai_input_observation *event;
    assert(cai_input_connection_poll(slow, &event, NULL) == CAI_OK);
    if (event == NULL)
      break;
    cai_input_observation_close(event);
  }
  cai_input_connection_close(slow);
  cai_input_queue_client_close(producer);
  cai_input_queue_close(queue);
  free(text);
  return NULL;
}
int main(void) {
  pthread_t worker;
  assert(pthread_create(&worker, NULL, run, NULL) == 0);
  assert(pthread_join(worker, NULL) == 0);
  return 0;
}
