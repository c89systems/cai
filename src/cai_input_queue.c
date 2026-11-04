#include "cai_input_queue_codec.h"
#include "cai_input_queue_internal.h"
#include "cai_internal.h"
#include <cai/input_queue.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define INPUT_RESULTS 16U
#define INPUT_DETACHED_CLIENTS 16U
typedef struct input_node {
  struct input_node *next;
  cai_input_entry value;
  char *text;
  cai_input_origin_status origin_status;
  unsigned long long *input_events;
  size_t input_event_count, input_event_capacity;
} input_node;
struct cai_input_queue_client {
  struct cai_input_queue_client *next;
  cai_input_queue *queue;
  unsigned long long id, high;
  cai_input_result results[INPUT_RESULTS];
  size_t next_result;
  int attached;
};
struct cai_input_queue {
  cai_input_queue_config config;
  pthread_t owner;
  input_node *head, *tail;
  input_node *receipts;
  cai_input_queue_client *clients;
  size_t count, references, detached_clients;
  size_t origin_count;
  unsigned long long revision, view_revision, serial, client_serial, token;
  unsigned long long uncertain_client, uncertain_request;
  int stopped, auto_approval, uncertain, notifying, closed;
};

static int queue_valid(const cai_input_queue *queue, cai_error *error) {
  if (queue == NULL || queue->closed || queue->notifying ||
      !pthread_equal(queue->owner, pthread_self()))
    return cai_set_error(error, CAI_ERR_INVALID,
                         "input queue requires its controller owner thread");
  return CAI_OK;
}
static void node_free(input_node *node) {
  cai_free_mem(NULL, node->text);
  cai_free_mem(NULL, node->input_events);
  cai_free_mem(NULL, node);
}
static void queue_release(cai_input_queue *queue) {
  input_node *node;
  cai_input_queue_client *client;
  if (--queue->references != 0U)
    return;
  while ((node = queue->head) != NULL) {
    queue->head = node->next;
    node_free(node);
  }
  while ((node = queue->receipts) != NULL) {
    queue->receipts = node->next;
    node_free(node);
  }
  while ((client = queue->clients) != NULL) {
    queue->clients = client->next;
    cai_free_mem(NULL, client);
  }
  cai_free_mem(NULL, queue);
}
static input_node *find_node(cai_input_queue *queue, unsigned long long id,
                             input_node **previous, size_t *index) {
  input_node *node = queue->head;
  if (previous != NULL)
    *previous = NULL;
  if (index != NULL)
    *index = 0U;
  for (; node != NULL; node = node->next) {
    if (node->value.id == id)
      return node;
    if (previous != NULL)
      *previous = node;
    if (index != NULL)
      ++*index;
  }
  return NULL;
}
static cai_input_queue_client *find_client(cai_input_queue *queue,
                                           unsigned long long id) {
  cai_input_queue_client *client;
  for (client = queue->clients; client != NULL; client = client->next)
    if (client->id == id)
      return client;
  return NULL;
}
static int client_attached(cai_input_queue *queue, unsigned long long id) {
  cai_input_queue_client *client = find_client(queue, id);
  return client != NULL && client->attached;
}
static void client_touch(cai_input_queue_client *client) {
  cai_input_queue_client **at;
  cai_input_queue *queue = client->queue;
  for (at = &queue->clients; *at != NULL; at = &(*at)->next)
    if (*at == client) {
      *at = client->next;
      client->next = queue->clients;
      queue->clients = client;
      return;
    }
}
static void clients_prune(cai_input_queue *queue) {
  while (queue->detached_clients > INPUT_DETACHED_CLIENTS) {
    cai_input_queue_client **at, **oldest = NULL, *client;
    for (at = &queue->clients; *at != NULL; at = &(*at)->next)
      if (!(*at)->attached)
        oldest = at;
    client = *oldest;
    *oldest = client->next;
    cai_free_mem(NULL, client);
    --queue->detached_clients;
  }
}
static int client_cached(cai_input_queue *queue, unsigned long long id,
                         cai_input_queue_client **out, cai_error *error) {
  cai_input_queue_client *client = find_client(queue, id);
  if (client == NULL) {
    client = cai_alloc(NULL, sizeof(*client));
    if (client == NULL)
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "restore input producer receipts");
    memset(client, 0, sizeof(*client));
    client->queue = queue;
    client->id = id;
    client->next = queue->clients;
    queue->clients = client;
    ++queue->detached_clients;
    clients_prune(queue);
  } else
    client_touch(client);
  *out = client;
  return CAI_OK;
}
static input_node *find_origin(cai_input_queue *queue, const char *origin) {
  input_node *node;
  if (origin[0] == '\0')
    return NULL;
  for (node = queue->head; node != NULL; node = node->next)
    if (strcmp(node->value.origin, origin) == 0)
      return node;
  for (node = queue->receipts; node != NULL; node = node->next)
    if (strcmp(node->value.origin, origin) == 0)
      return node;
  return NULL;
}
static void state_copy(const cai_input_queue *queue, cai_input_state *out) {
  out->count = queue->count;
  out->revision = queue->revision;
  out->view_revision = queue->view_revision;
  out->stopped = queue->stopped;
  out->auto_approval = queue->auto_approval;
  out->uncertain = queue->uncertain;
}
static cai_input_entry exposed(const cai_input_queue *queue,
                               const input_node *node) {
  cai_input_entry out = node->value;
  out.text = node->text;
  out.ready = out.ready && !out.held && !out.edit_held && !out.stopped &&
              !queue->uncertain;
  return out;
}
static void changed(cai_input_queue *queue, cai_input_change_type type,
                    size_t index, const input_node *node,
                    unsigned long long client, unsigned long long request) {
  cai_input_change event;
  memset(&event, 0, sizeof(event));
  event.type = type;
  event.index = index;
  event.client_id = client;
  event.request_id = request;
  event.previous_view_revision = queue->view_revision++;
  if (node != NULL)
    event.entry = exposed(queue, node);
  state_copy(queue, &event.state);
  if (queue->config.changed != NULL) {
    queue->notifying = 1;
    queue->config.changed(queue->config.changed_context, &event);
    queue->notifying = 0;
  }
}
static int reserve_changes(cai_input_queue *queue, cai_error *error) {
  unsigned long long maximum = ~(unsigned long long)0;
  if (queue->count > maximum - 2U ||
      maximum - queue->view_revision < queue->count + 2U)
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "input queue revision exhausted");
  return CAI_OK;
}
static void result_set(cai_input_result *result, unsigned long long request,
                       cai_input_outcome outcome, int code,
                       const char *reason) {
  memset(result, 0, sizeof(*result));
  result->request_id = request;
  result->outcome = outcome;
  result->code = code;
  if (reason != NULL)
    snprintf(result->reason, sizeof(result->reason), "%s", reason);
}
static cai_input_result *result_find(cai_input_queue_client *client,
                                     unsigned long long request) {
  size_t i;
  for (i = 0U; i < INPUT_RESULTS; ++i)
    if (client->results[i].request_id == request)
      return &client->results[i];
  return NULL;
}
static void result_save(cai_input_queue_client *client,
                        const cai_input_result *result) {
  cai_input_result *saved = result_find(client, result->request_id);
  client_touch(client);
  if (saved != NULL)
    *saved = *result;
  else {
    client->results[client->next_result] = *result;
    client->next_result = (client->next_result + 1U) % INPUT_RESULTS;
  }
  if (result->request_id > client->high)
    client->high = result->request_id;
}
static int persist(cai_input_queue *queue, const cai_input_record *record,
                   unsigned long long *position, cai_error *error) {
  cai_error local;
  unsigned long long local_position = 0U;
  int rc;
  if (position != NULL)
    *position = 0U;
  if (queue->config.persist == NULL)
    return CAI_OK;
  cai_error_init(&local);
  queue->notifying = 1;
  rc = queue->config.persist(queue->config.persist_context, record,
                             position != NULL ? position : &local_position,
                             error != NULL ? error : &local);
  queue->notifying = 0;
  cai_error_cleanup(&local);
  if (rc != CAI_OK) {
    queue->uncertain = 1;
    queue->uncertain_client = record->client_id;
    queue->uncertain_request = record->request.request_id;
    changed(queue, CAI_INPUT_STATE, 0U, NULL, record->client_id,
            record->request.request_id);
  }
  return rc;
}

typedef struct input_prepared {
  cai_input_record record;
  input_node *node, *previous, *append;
  char *replacement;
  size_t index;
} input_prepared;
static void prepared_clear(input_prepared *prepared) {
  if (prepared->append != NULL)
    node_free(prepared->append);
  cai_free_mem(NULL, prepared->replacement);
}
static int entry_equal(const cai_input_entry *a, const cai_input_entry *b) {
  return strcmp(a->origin, b->origin) == 0 && a->id == b->id &&
         a->revision == b->revision && a->client_id == b->client_id &&
         a->request_id == b->request_id && a->edit_owner == b->edit_owner &&
         a->edit_token == b->edit_token && a->intent == b->intent &&
         a->held == b->held && a->edit_held == b->edit_held &&
         a->action == b->action && a->independent == b->independent &&
         a->stopped == b->stopped &&
         (b->text == NULL ||
          (a->text != NULL && strcmp(a->text, b->text) == 0));
}
static void deny(input_prepared *prepared, int code, const char *reason) {
  prepared->record.result.code = code;
  snprintf(prepared->record.result.reason,
           sizeof(prepared->record.result.reason), "%s", reason);
}
static int text_valid(cai_input_queue *queue, const char *text) {
  size_t n;
  if (text == NULL || text[0] == '\0')
    return 0;
  for (n = 0U; n < queue->config.max_text_bytes && text[n] != '\0'; ++n) {
  }
  return text[n] == '\0';
}
static int reserve_input_event(input_node *node, cai_error *error) {
  unsigned long long *events;
  size_t capacity;
  if (node->input_event_count < node->input_event_capacity)
    return CAI_OK;
  capacity =
      node->input_event_capacity == 0U ? 4U : node->input_event_capacity * 2U;
  if (capacity < node->input_event_capacity ||
      capacity > (size_t)-1 / sizeof(*events))
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "input provenance capacity exhausted");
  events =
      cai_realloc_mem(NULL, node->input_events, capacity * sizeof(*events));
  if (events == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "retain input provenance");
  node->input_events = events;
  node->input_event_capacity = capacity;
  return CAI_OK;
}
static int policy_for(cai_input_queue *queue, const cai_input_request *request,
                      const char *text, const cai_input_policy *restored,
                      cai_input_policy *out, cai_error *error) {
  int rc = CAI_OK;
  memset(out, 0, sizeof(*out));
  out->intent = request->intent;
  if (out->intent != CAI_INPUT_QUEUED && out->intent != CAI_INPUT_STEERING)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "invalid input delivery intent");
  if (restored != NULL)
    *out = *restored;
  else if (queue->config.classify != NULL) {
    queue->notifying = 1;
    rc = queue->config.classify(queue->config.classify_context, text, out,
                                error);
    queue->notifying = 0;
  }
  if (rc == CAI_OK && out->intent != CAI_INPUT_QUEUED &&
      out->intent != CAI_INPUT_STEERING)
    rc = cai_set_error(error, CAI_ERR_INVALID, "invalid input delivery policy");
  out->action = out->action != 0;
  out->independent = out->independent != 0;
  out->resume_backlog = out->resume_backlog != 0;
  return rc;
}
static int prepare_request(cai_input_queue *queue, unsigned long long client,
                           const cai_input_request *request,
                           const cai_input_policy *restored,
                           input_prepared *prepared, cai_error *error) {
  cai_input_record *record = &prepared->record;
  input_node *before;
  int rc;
  memset(prepared, 0, sizeof(*prepared));
  record->type = CAI_INPUT_RECORD_REQUEST;
  record->client_id = client;
  record->before_revision = record->after_revision = queue->revision;
  record->request = *request;
  record->request.enabled = request->enabled != 0;
  result_set(&record->result, request->request_id, CAI_INPUT_REJECTED,
             CAI_ERR_INVALID, "invalid input queue request");
  if ((unsigned int)request->operation >
          (unsigned int)CAI_INPUT_FORGET_ORIGIN ||
      memchr(request->origin, '\0', sizeof(request->origin)) == NULL)
    return CAI_OK;
  prepared->node =
      find_node(queue, request->item_id, &prepared->previous, &prepared->index);
  if (request->operation >= CAI_INPUT_BEGIN_EDIT &&
      request->operation <= CAI_INPUT_SET_INTENT) {
    if (prepared->node == NULL || request->item_revision == 0U ||
        prepared->node->value.revision != request->item_revision) {
      deny(prepared, CAI_ERR_INVALID, "item changed or was already claimed");
      return CAI_OK;
    }
    record->entry = prepared->node->value;
    record->entry.text = prepared->node->text;
    if (record->entry.edit_held && request->operation != CAI_INPUT_BEGIN_EDIT &&
        (record->entry.edit_owner != client ||
         record->entry.edit_token != request->edit_token)) {
      deny(prepared, CAI_ERR_INVALID, "edit hold belongs to another client");
      return CAI_OK;
    }
    if ((request->operation == CAI_INPUT_END_EDIT ||
         request->operation == CAI_INPUT_ABORT_EDIT) &&
        !record->entry.edit_held) {
      deny(prepared, CAI_ERR_INVALID, "item has no owned edit hold");
      return CAI_OK;
    }
    if (record->entry.held && (request->operation == CAI_INPUT_BEGIN_EDIT ||
                               request->operation == CAI_INPUT_APPROVE ||
                               request->operation == CAI_INPUT_WITHDRAW)) {
      for (before = queue->head; before != prepared->node;
           before = before->next)
        if (before->value.held) {
          deny(prepared, CAI_ERR_INVALID, "approval decisions are FIFO");
          return CAI_OK;
        }
    }
  }
  /* STOP cannot be defeated by a racing readiness/state update. Resuming or
   * clearing work deliberately compares the durable queue revision. */
  if (request->operation >= CAI_INPUT_RESUME &&
      request->operation <= CAI_INPUT_AUTO_APPROVAL &&
      request->queue_revision != queue->revision) {
    deny(prepared, CAI_ERR_INVALID, "queue changed; inspect before retrying");
    return CAI_OK;
  }
  if (queue->revision == ~(unsigned long long)0) {
    deny(prepared, CAI_ERR_LIMIT, "input queue revision exhausted");
    return CAI_OK;
  }
  switch (request->operation) {
  case CAI_INPUT_OFFER:
    before = find_origin(queue, request->origin);
    if (before != NULL) {
      record->entry = before->value;
      record->entry.text = NULL;
      record->request.text = NULL;
      record->result.outcome = CAI_INPUT_ACCEPTED;
      record->result.code = CAI_OK;
      record->result.reason[0] = '\0';
      record->result.item_id = before->value.id;
      record->result.item_revision = before->value.revision;
      return CAI_OK;
    }
    if (!text_valid(queue, request->text)) {
      deny(prepared, CAI_ERR_LIMIT,
           "input must be nonempty and within its limit");
      return CAI_OK;
    }
    if (queue->count >= queue->config.max_entries ||
        (request->origin[0] != '\0' &&
         queue->origin_count >= queue->config.max_entries) ||
        queue->serial == ~(unsigned long long)0) {
      deny(prepared, CAI_ERR_LIMIT, "input queue is full");
      return CAI_OK;
    }
    rc = policy_for(queue, request, request->text, restored, &record->policy,
                    error);
    if (rc != CAI_OK)
      return rc;
    prepared->append = cai_alloc(NULL, sizeof(*prepared->append));
    if (prepared->append == NULL)
      return cai_set_error(error, CAI_ERR_NOMEM, "allocate pending input");
    memset(prepared->append, 0, sizeof(*prepared->append));
    prepared->append->text = cai_strdup(NULL, request->text);
    if (prepared->append->text == NULL)
      return cai_set_error(error, CAI_ERR_NOMEM, "copy pending input");
    memset(&record->entry, 0, sizeof(record->entry));
    record->entry.id = queue->serial + 1U;
    record->entry.client_id = client;
    record->entry.request_id = request->request_id;
    record->entry.text = prepared->append->text;
    record->entry.intent = record->policy.intent;
    record->entry.action = record->policy.action;
    record->entry.independent = record->policy.independent;
    record->entry.held = request->enabled && !queue->auto_approval;
    record->entry.stopped =
        queue->stopped &&
        !(!request->enabled &&
          (record->policy.resume_backlog || record->policy.action));
    memcpy(record->entry.origin, request->origin, sizeof(record->entry.origin));
    break;
  case CAI_INPUT_FORGET_ORIGIN:
    before = find_origin(queue, request->origin);
    if (request->origin[0] == '\0' ||
        (before != NULL && before->origin_status == CAI_INPUT_ORIGIN_PENDING))
      return CAI_OK;
    if (before == NULL) {
      record->result.outcome = CAI_INPUT_ACCEPTED;
      record->result.code = CAI_OK;
      record->result.reason[0] = '\0';
      return CAI_OK;
    }
    break;
  case CAI_INPUT_BEGIN_EDIT:
    if (record->entry.edit_held && record->entry.edit_owner == client) {
      record->result.outcome = CAI_INPUT_ACCEPTED;
      record->result.code = CAI_OK;
      record->result.reason[0] = '\0';
      record->result.item_id = record->entry.id;
      record->result.item_revision = record->entry.revision;
      record->result.edit_token = record->entry.edit_token;
      return CAI_OK;
    }
    if (record->entry.edit_held &&
        client_attached(queue, record->entry.edit_owner)) {
      deny(prepared, CAI_ERR_INVALID, "item is already being edited");
      return CAI_OK;
    }
    if (queue->token == ~(unsigned long long)0) {
      deny(prepared, CAI_ERR_LIMIT, "input edit token exhausted");
      return CAI_OK;
    }
    record->entry.edit_held = 1;
    record->entry.edit_owner = client;
    record->entry.edit_token = queue->token + 1U;
    break;
  case CAI_INPUT_END_EDIT:
    if (!text_valid(queue, request->text)) {
      deny(prepared, CAI_ERR_LIMIT,
           "replacement must be nonempty and within its limit");
      return CAI_OK;
    }
    rc = policy_for(queue, request, request->text, restored, &record->policy,
                    error);
    if (rc != CAI_OK)
      return rc;
    prepared->replacement = cai_strdup(NULL, request->text);
    if (prepared->replacement == NULL)
      return cai_set_error(error, CAI_ERR_NOMEM, "copy input replacement");
    record->entry.text = prepared->replacement;
    record->entry.intent = record->policy.intent;
    record->entry.action = record->policy.action;
    record->entry.independent = record->policy.independent;
    record->entry.ready = 0;
    if (request->intent == CAI_INPUT_STEERING && !record->entry.held)
      record->entry.stopped = 0;
    /* fall through */
  case CAI_INPUT_ABORT_EDIT:
    record->entry.edit_held = 0;
    record->entry.edit_owner = 0U;
    record->entry.edit_token = 0U;
    break;
  case CAI_INPUT_APPROVE:
    record->entry.held = 0;
    break;
  case CAI_INPUT_WITHDRAW:
    break;
  case CAI_INPUT_SET_INTENT:
    rc = policy_for(queue, request, prepared->node->text, restored,
                    &record->policy, error);
    if (rc != CAI_OK)
      return rc;
    record->entry.intent = record->policy.intent;
    record->entry.action = record->policy.action;
    record->entry.independent = record->policy.independent;
    record->entry.ready = 0;
    if (request->intent == CAI_INPUT_STEERING && !record->entry.held &&
        !record->entry.edit_held)
      record->entry.stopped = 0;
    break;
  case CAI_INPUT_STOP:
  case CAI_INPUT_RESUME:
  case CAI_INPUT_CLEAR:
  case CAI_INPUT_AUTO_APPROVAL:
    break;
  default:
    return CAI_OK;
  }
  record->after_revision = queue->revision + 1U;
  if (record->entry.id != 0U)
    record->entry.revision = record->after_revision;
  record->result.outcome = CAI_INPUT_ACCEPTED;
  record->result.code = CAI_OK;
  record->result.reason[0] = '\0';
  record->result.item_id = record->entry.id;
  record->result.item_revision = record->entry.revision;
  record->result.edit_token = record->entry.edit_token;
  return CAI_OK;
}
static void emit_change(cai_input_queue *queue, cai_input_change_type type,
                        size_t index, input_node *node,
                        const cai_input_record *record, int live) {
  if (live)
    changed(queue, type, index, node, record->client_id,
            record->request.request_id);
}
static void remove_node(cai_input_queue *queue, input_node *node,
                        input_node *previous, size_t index,
                        const cai_input_record *record, int live,
                        cai_input_claim *claim) {
  if (previous == NULL)
    queue->head = node->next;
  else
    previous->next = node->next;
  if (queue->tail == node)
    queue->tail = previous;
  --queue->count;
  emit_change(queue, CAI_INPUT_REMOVE, index, node, record, live);
  if (claim != NULL) {
    claim->entry = exposed(queue, node);
    claim->text = node->text;
    claim->entry.text = claim->text;
    claim->entry.ready = 0;
    node->text = NULL;
    claim->input_events = node->input_events;
    claim->input_event_count = node->input_event_count;
    node->input_events = NULL;
  }
  cai_free_mem(NULL, node->input_events);
  node->input_events = NULL;
  node->input_event_count = node->input_event_capacity = 0U;
  if (node->value.origin[0] != '\0') {
    cai_free_mem(NULL, node->text);
    node->text = NULL;
    node->value.text = NULL;
    node->value.held = node->value.edit_held = node->value.ready =
        node->value.stopped = 0;
    node->value.edit_owner = node->value.edit_token = 0U;
    node->origin_status = record->type == CAI_INPUT_RECORD_CLAIM
                              ? CAI_INPUT_ORIGIN_CLAIMED
                              : CAI_INPUT_ORIGIN_WITHDRAWN;
    node->next = queue->receipts;
    queue->receipts = node;
  } else
    node_free(node);
}
static void apply_request(cai_input_queue *queue, input_prepared *prepared,
                          int live) {
  const cai_input_record *record = &prepared->record;
  input_node *node, *already_changed = NULL;
  size_t index;
  int backlog = -1;
  if (record->after_revision == queue->revision)
    return;
  queue->revision = record->after_revision;
  switch (record->request.operation) {
  case CAI_INPUT_OFFER:
    node = prepared->append;
    prepared->append = NULL;
    node->value = record->entry;
    if (queue->tail == NULL)
      queue->head = node;
    else
      queue->tail->next = node;
    queue->tail = node;
    ++queue->count;
    if (node->value.origin[0] != '\0')
      ++queue->origin_count;
    queue->serial = node->value.id;
    if (record->policy.resume_backlog && !record->request.enabled) {
      queue->stopped = 0;
      backlog = 0;
    }
    already_changed = node;
    emit_change(queue, CAI_INPUT_UPSERT, queue->count - 1U, node, record, live);
    break;
  case CAI_INPUT_BEGIN_EDIT:
  case CAI_INPUT_END_EDIT:
  case CAI_INPUT_ABORT_EDIT:
  case CAI_INPUT_APPROVE:
  case CAI_INPUT_SET_INTENT:
    node = prepared->node;
    if (prepared->replacement != NULL) {
      cai_free_mem(NULL, node->text);
      node->text = prepared->replacement;
      prepared->replacement = NULL;
    }
    node->value = record->entry;
    node->value.text = node->text;
    if (node->value.edit_token > queue->token)
      queue->token = node->value.edit_token;
    already_changed = node;
    emit_change(queue, CAI_INPUT_UPSERT, prepared->index, node, record, live);
    break;
  case CAI_INPUT_WITHDRAW:
    remove_node(queue, prepared->node, prepared->previous, prepared->index,
                record, live, NULL);
    break;
  case CAI_INPUT_STOP:
    queue->stopped = 1;
    backlog = 1;
    break;
  case CAI_INPUT_RESUME:
    queue->stopped = 0;
    backlog = 0;
    break;
  case CAI_INPUT_CLEAR:
    while (queue->head != NULL)
      remove_node(queue, queue->head, NULL, 0U, record, live, NULL);
    break;
  case CAI_INPUT_AUTO_APPROVAL:
    queue->auto_approval = record->request.enabled;
    for (node = queue->head, index = 0U; node != NULL;
         node = node->next, ++index)
      if (queue->auto_approval && node->value.held) {
        node->value.held = 0;
        node->value.revision = queue->revision;
        emit_change(queue, CAI_INPUT_UPSERT, index, node, record, live);
      }
    break;
  case CAI_INPUT_FORGET_ORIGIN: {
    input_node **at;
    for (at = &queue->receipts; *at != NULL; at = &(*at)->next)
      if (strcmp((*at)->value.origin, record->request.origin) == 0) {
        node = *at;
        *at = node->next;
        node_free(node);
        --queue->origin_count;
        break;
      }
    break;
  }
  default:
    break;
  }
  if (backlog >= 0)
    for (node = queue->head, index = 0U; node != NULL;
         node = node->next, ++index)
      if (node != already_changed) {
        node->value.stopped = backlog;
        emit_change(queue, CAI_INPUT_UPSERT, index, node, record, live);
      }
  emit_change(queue, CAI_INPUT_STATE, 0U, NULL, record, live);
}

void cai_input_queue_config_init(cai_input_queue_config *config) {
  if (config != NULL)
    memset(config, 0, sizeof(*config));
}
static int has_edits(cai_input_queue *queue, unsigned long long client) {
  input_node *node;
  for (node = queue->head; node != NULL; node = node->next)
    if (node->value.edit_held && node->value.edit_owner == client)
      return 1;
  return 0;
}
static void release_edits(cai_input_queue *queue,
                          const cai_input_record *record, int live) {
  input_node *node;
  size_t index;
  queue->revision = record->after_revision;
  for (node = queue->head, index = 0U; node != NULL; node = node->next, ++index)
    if (node->value.edit_held && node->value.edit_owner == record->client_id) {
      node->value.edit_held = 0;
      node->value.edit_owner = node->value.edit_token = 0U;
      node->value.revision = queue->revision;
      emit_change(queue, CAI_INPUT_UPSERT, index, node, record, live);
    }
  emit_change(queue, CAI_INPUT_STATE, 0U, NULL, record, live);
}
static int release_client(cai_input_queue *queue, unsigned long long client,
                          cai_error *error) {
  cai_input_record record;
  int rc;
  rc = queue_valid(queue, error);
  if (rc != CAI_OK)
    return rc;
  if (!has_edits(queue, client))
    return CAI_OK;
  if (queue->uncertain)
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "input queue requires recovery before edit release");
  if (queue->revision == ~(unsigned long long)0)
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "input queue revision exhausted");
  rc = reserve_changes(queue, error);
  if (rc != CAI_OK)
    return rc;
  memset(&record, 0, sizeof(record));
  record.type = CAI_INPUT_RECORD_RELEASE;
  record.client_id = client;
  record.before_revision = queue->revision;
  record.after_revision = queue->revision + 1U;
  rc = persist(queue, &record, NULL, error);
  if (rc == CAI_OK)
    release_edits(queue, &record, 1);
  return rc;
}
int cai_input_queue_client_release_edits(cai_input_queue_client *client,
                                         cai_error *error) {
  if (client == NULL || !client->attached)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "attached input producer required");
  return release_client(client->queue, client->id, error);
}
int cai_input_queue_release_orphans(cai_input_queue *queue, cai_error *error) {
  input_node *node;
  int rc = queue_valid(queue, error);
  if (rc != CAI_OK)
    return rc;
  for (;;) {
    for (node = queue->head; node != NULL; node = node->next)
      if (node->value.edit_held &&
          !client_attached(queue, node->value.edit_owner))
        break;
    if (node == NULL)
      return CAI_OK;
    rc = release_client(queue, node->value.edit_owner, error);
    if (rc != CAI_OK)
      return rc;
  }
}
int cai_input_queue_open(const cai_input_queue_config *config,
                         cai_input_queue **out, cai_error *error) {
  cai_input_queue *queue;
  if (out == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "input queue output is required");
  *out = NULL;
  queue = cai_alloc(NULL, sizeof(*queue));
  if (queue == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "allocate input queue");
  memset(queue, 0, sizeof(*queue));
  if (config != NULL)
    queue->config = *config;
  if (queue->config.max_entries == 0U)
    queue->config.max_entries = 128U;
  if (queue->config.max_text_bytes == 0U)
    queue->config.max_text_bytes = 66048U;
  queue->owner = pthread_self();
  queue->references = 1U;
  *out = queue;
  return CAI_OK;
}
void cai_input_queue_close(cai_input_queue *queue) {
  if (queue != NULL) {
    queue->closed = 1;
    queue_release(queue);
  }
}
int cai_input_queue_retain(cai_input_queue *queue, cai_error *error) {
  int rc = queue_valid(queue, error);
  if (rc != CAI_OK)
    return rc;
  if (queue->references == (size_t)-1)
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "input queue reference capacity exhausted");
  ++queue->references;
  return CAI_OK;
}
void cai_input_queue_release(cai_input_queue *queue) {
  if (queue != NULL)
    queue_release(queue);
}
int cai_input_queue_client_open(cai_input_queue *queue,
                                cai_input_queue_client **out,
                                cai_error *error) {
  cai_input_queue_client *client;
  cai_input_record record;
  int rc;
  if (out != NULL)
    *out = NULL;
  rc = queue_valid(queue, error);
  if (rc != CAI_OK)
    return rc;
  if (out == NULL || queue->uncertain ||
      queue->client_serial == ~(unsigned long long)0)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "input queue cannot attach a client");
  client = cai_alloc(NULL, sizeof(*client));
  if (client == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "allocate input queue client");
  memset(client, 0, sizeof(*client));
  memset(&record, 0, sizeof(record));
  record.type = CAI_INPUT_RECORD_CLIENT;
  record.before_revision = record.after_revision = queue->revision;
  record.client_id = queue->client_serial + 1U;
  rc = reserve_changes(queue, error);
  if (rc == CAI_OK)
    rc = persist(queue, &record, NULL, error);
  if (rc != CAI_OK) {
    cai_free_mem(NULL, client);
    return rc;
  }
  client->id = ++queue->client_serial;
  client->attached = 1;
  client->queue = queue;
  client->next = queue->clients;
  queue->clients = client;
  ++queue->references;
  *out = client;
  return CAI_OK;
}
int cai_input_queue_client_resume(cai_input_queue *queue, unsigned long long id,
                                  cai_input_queue_client **out,
                                  cai_error *error) {
  cai_input_queue_client *client;
  int rc = queue_valid(queue, error);
  if (out != NULL)
    *out = NULL;
  if (rc != CAI_OK)
    return rc;
  if (out == NULL || id == 0U || (client = find_client(queue, id)) == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "producer receipts unavailable; reconcile uncertain "
                         "input without automatically reoffering it");
  if (client->attached)
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "input producer is already attached");
  client->attached = 1;
  --queue->detached_clients;
  ++queue->references;
  client_touch(client);
  *out = client;
  return CAI_OK;
}
void cai_input_queue_client_close(cai_input_queue_client *client) {
  cai_input_queue *queue;
  if (client == NULL || !client->attached)
    return;
  queue = client->queue;
  client->attached = 0;
  ++queue->detached_clients;
  client_touch(client);
  clients_prune(queue);
  queue_release(queue);
}
unsigned long long
cai_input_queue_client_id(const cai_input_queue_client *client) {
  return client != NULL && client->attached ? client->id : 0U;
}
unsigned long long
cai_input_queue_client_last_request(const cai_input_queue_client *client) {
  return client != NULL && client->attached ? client->high : 0U;
}
int cai_input_queue_get_state(const cai_input_queue *queue,
                              cai_input_state *out, cai_error *error) {
  if (out == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "input queue state is unavailable");
  if (queue_valid(queue, error) != CAI_OK)
    return CAI_ERR_INVALID;
  state_copy(queue, out);
  return CAI_OK;
}
int cai_input_queue_set_policy(cai_input_queue *queue,
                               cai_input_classify_fn classify, void *context,
                               cai_error *error) {
  int rc = queue_valid(queue, error);
  if (rc == CAI_OK) {
    queue->config.classify = classify;
    queue->config.classify_context = context;
  }
  return rc;
}
int cai_input_queue_set_observer(cai_input_queue *queue,
                                 cai_input_changed_fn observer, void *context,
                                 cai_error *error) {
  int rc;
  if (queue != NULL && queue->closed && observer == NULL &&
      pthread_equal(queue->owner, pthread_self()) && !queue->notifying) {
    queue->config.changed = NULL;
    queue->config.changed_context = NULL;
    return CAI_OK;
  }
  rc = queue_valid(queue, error);
  if (rc == CAI_OK) {
    queue->config.changed = observer;
    queue->config.changed_context = context;
  }
  return rc;
}
int cai_input_queue_lookup(cai_input_queue *queue, unsigned long long id,
                           cai_input_entry *out, cai_error *error) {
  input_node *node;
  int rc = queue_valid(queue, error);
  if (rc != CAI_OK)
    return rc;
  node = find_node(queue, id, NULL, NULL);
  if (node == NULL || out == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "input queue entry was not found");
  *out = exposed(queue, node);
  return CAI_OK;
}
int cai_input_queue_find_origin(cai_input_queue *queue, const char *origin,
                                cai_input_entry *out,
                                cai_input_origin_status *status,
                                cai_error *error) {
  input_node *node;
  int rc = queue_valid(queue, error);
  if (rc != CAI_OK)
    return rc;
  if (origin == NULL || out == NULL || status == NULL ||
      (node = find_origin(queue, origin)) == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "input origin receipt was not found");
  *out = exposed(queue, node);
  *status = node->origin_status;
  return CAI_OK;
}
int cai_input_queue_input_events(cai_input_queue *queue, unsigned long long id,
                                 const unsigned long long **events,
                                 size_t *count, cai_error *error) {
  input_node *node;
  int rc = queue_valid(queue, error);
  if (rc != CAI_OK)
    return rc;
  if (events == NULL || count == NULL ||
      (node = find_node(queue, id, NULL, NULL)) == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "pending input provenance was not found");
  *events = node->input_events;
  *count = node->input_event_count;
  return CAI_OK;
}
int cai_input_queue_entry_at(cai_input_queue_client *client, size_t index,
                             cai_input_entry *out, cai_error *error) {
  input_node *node;
  int rc;
  if (client == NULL || !client->attached || out == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "input queue entry is unavailable");
  rc = queue_valid(client->queue, error);
  if (rc != CAI_OK)
    return rc;
  for (node = client->queue->head; node != NULL && index > 0U;
       node = node->next)
    --index;
  if (node == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "input queue entry was not found");
  *out = exposed(client->queue, node);
  if (out->edit_owner != client->id)
    out->edit_token = 0U;
  return CAI_OK;
}
int cai_input_queue_set_ready(cai_input_queue *queue, unsigned long long id,
                              int ready, cai_error *error) {
  input_node *node;
  size_t index;
  int rc = queue_valid(queue, error);
  if (rc != CAI_OK)
    return rc;
  node = find_node(queue, id, NULL, &index);
  if (node == NULL || queue->uncertain)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "input queue entry is unavailable");
  if (node->value.ready == (ready != 0))
    return CAI_OK;
  rc = reserve_changes(queue, error);
  if (rc == CAI_OK) {
    node->value.ready = ready != 0;
    changed(queue, CAI_INPUT_UPSERT, index, node, 0U, 0U);
  }
  return rc;
}

int cai_input_queue_admit(cai_input_queue_client *client,
                          const cai_input_request *request,
                          int (*authorize)(void *, cai_error *), void *context,
                          cai_input_result *result, cai_error *error) {
  cai_input_queue *queue;
  cai_input_result *saved;
  input_prepared prepared;
  cai_error local;
  unsigned long long position = 0U;
  int rc;
  if (client == NULL || !client->attached || request == NULL ||
      result == NULL || request->request_id == 0U ||
      request->operation < CAI_INPUT_OFFER ||
      request->operation > CAI_INPUT_FORGET_ORIGIN ||
      memchr(request->origin, '\0', sizeof(request->origin)) == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "input queue request is incomplete");
  queue = client->queue;
  rc = queue_valid(queue, error);
  if (rc != CAI_OK)
    return rc;
  if (request->request_id <= client->high) {
    saved = result_find(client, request->request_id);
    if (saved != NULL)
      *result = *saved;
    else
      result_set(result, request->request_id, CAI_INPUT_UNKNOWN,
                 CAI_ERR_TRANSPORT,
                 "result expired; request is never readmitted");
    return CAI_OK;
  }
  if (request->operation == CAI_INPUT_RESOLVE) {
    saved = request->target_request_id != 0U &&
                    request->target_request_id < request->request_id
                ? result_find(client, request->target_request_id)
                : NULL;
    if (saved != NULL) {
      *result = *saved;
      result->request_id = request->request_id;
    } else
      result_set(result, request->request_id, CAI_INPUT_UNKNOWN,
                 CAI_ERR_TRANSPORT, "request outcome is unavailable");
    result_save(client, result);
    return CAI_OK;
  }
  if (request->operation == CAI_INPUT_SYNCHRONIZE) {
    result_set(result, request->request_id,
               queue->uncertain ? CAI_INPUT_UNKNOWN : CAI_INPUT_ACCEPTED,
               queue->uncertain ? CAI_ERR_TRANSPORT : CAI_OK,
               queue->uncertain ? "queue recovery is pending" : NULL);
    result_save(client, result);
    return CAI_OK;
  }
  if (queue->uncertain) {
    result_set(result, request->request_id, CAI_INPUT_REJECTED,
               CAI_ERR_TRANSPORT,
               "queue recovery is pending; input was not accepted");
    result_save(client, result);
    return CAI_OK;
  }
  cai_error_init(&local);
  memset(&prepared, 0, sizeof(prepared));
  rc = authorize != NULL ? authorize(context, &local) : CAI_OK;
  if (rc != CAI_OK) {
    prepared.record.type = CAI_INPUT_RECORD_REQUEST;
    prepared.record.client_id = client->id;
    prepared.record.before_revision = prepared.record.after_revision =
        queue->revision;
    prepared.record.request = *request;
    result_set(
        &prepared.record.result, request->request_id, CAI_INPUT_REJECTED, rc,
        local.message != NULL ? local.message : "input authority was revoked");
  } else {
    rc = reserve_changes(queue, &local);
    if (rc != CAI_OK) {
      result_set(result, request->request_id, CAI_INPUT_REJECTED, rc,
                 "input queue observation limit reached");
      result_save(client, result);
      cai_error_cleanup(&local);
      return CAI_OK;
    }
    rc = prepare_request(queue, client->id, request, NULL, &prepared, &local);
    if (rc == CAI_OK && prepared.record.result.outcome == CAI_INPUT_ACCEPTED &&
        prepared.record.request.text != NULL &&
        (request->operation == CAI_INPUT_OFFER ||
         request->operation == CAI_INPUT_END_EDIT))
      rc = reserve_input_event(
          prepared.append != NULL ? prepared.append : prepared.node, &local);
    if (rc != CAI_OK) {
      prepared_clear(&prepared);
      memset(&prepared, 0, sizeof(prepared));
      prepared.record.type = CAI_INPUT_RECORD_REQUEST;
      prepared.record.client_id = client->id;
      prepared.record.before_revision = prepared.record.after_revision =
          queue->revision;
      prepared.record.request = *request;
      result_set(&prepared.record.result, request->request_id,
                 CAI_INPUT_REJECTED, rc,
                 local.message != NULL
                     ? local.message
                     : "input could not be prepared; nothing was accepted");
    }
  }
  /* Rejected requests keep an outcome, not an unbounded rejected payload. */
  if (prepared.record.result.outcome == CAI_INPUT_REJECTED) {
    prepared.record.request.text = NULL;
    if (prepared.record.request.intent != CAI_INPUT_QUEUED &&
        prepared.record.request.intent != CAI_INPUT_STEERING)
      prepared.record.request.intent = CAI_INPUT_QUEUED;
    memset(&prepared.record.entry, 0, sizeof(prepared.record.entry));
  }
  cai_error_cleanup(&local);
  cai_error_init(&local);
  rc = persist(queue, &prepared.record, &position, &local);
  if (rc == CAI_OK) {
    apply_request(queue, &prepared, 1);
    if (position != 0U &&
        prepared.record.result.outcome == CAI_INPUT_ACCEPTED &&
        prepared.record.request.text != NULL &&
        (request->operation == CAI_INPUT_OFFER ||
         request->operation == CAI_INPUT_END_EDIT)) {
      input_node *node = find_node(queue, prepared.record.entry.id, NULL, NULL);
      node->input_events[node->input_event_count++] = position;
    }
    *result = prepared.record.result;
  } else
    result_set(
        result, request->request_id, CAI_INPUT_UNKNOWN, rc,
        "input queue journal outcome is unknown; recover before retrying");
  result_save(client, result);
  prepared_clear(&prepared);
  cai_error_cleanup(&local);
  return CAI_OK;
}
int cai_input_queue_request(cai_input_queue_client *client,
                            const cai_input_request *request,
                            cai_input_result *result, cai_error *error) {
  return cai_input_queue_admit(client, request, NULL, NULL, result, error);
}
int cai_input_queue_claim(cai_input_queue *queue, unsigned long long id,
                          unsigned long long revision, cai_input_claim *out,
                          cai_error *error) {
  cai_input_record record;
  input_node *node, *previous, *before;
  size_t index;
  unsigned long long position = 0U;
  int rc = queue_valid(queue, error);
  if (rc != CAI_OK)
    return rc;
  if (out == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "input claim output is required");
  memset(out, 0, sizeof(*out));
  node = find_node(queue, id, &previous, &index);
  if (node == NULL || node->value.revision != revision ||
      !exposed(queue, node).ready)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "input is held, changed or not ready");
  if (!node->value.independent) {
    for (before = queue->head; before != node; before = before->next)
      if (!before->value.independent && !before->value.held &&
          !before->value.edit_held && !before->value.stopped &&
          before->value.intent == node->value.intent)
        return cai_set_error(error, CAI_ERR_INVALID,
                             "earlier input owns this delivery lane");
    if (node->value.intent == CAI_INPUT_QUEUED)
      for (before = queue->head; before != NULL; before = before->next)
        if (!before->value.independent &&
            before->value.intent == CAI_INPUT_STEERING &&
            exposed(queue, before).ready)
          return cai_set_error(error, CAI_ERR_INVALID,
                               "steering input has priority");
  }
  if (queue->revision == ~(unsigned long long)0)
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "input queue revision exhausted");
  rc = reserve_changes(queue, error);
  if (rc != CAI_OK)
    return rc;
  memset(&record, 0, sizeof(record));
  record.type = CAI_INPUT_RECORD_CLAIM;
  record.before_revision = queue->revision;
  record.after_revision = queue->revision + 1U;
  record.entry = exposed(queue, node);
  record.input_events = node->input_events;
  record.input_event_count = node->input_event_count;
  record.request.item_id = id;
  record.request.item_revision = revision;
  rc = persist(queue, &record, &position, error);
  if (rc == CAI_OK) {
    queue->revision = record.after_revision;
    remove_node(queue, node, previous, index, &record, 1, out);
    out->position = position;
    emit_change(queue, CAI_INPUT_STATE, 0U, NULL, &record, 1);
  }
  return rc;
}
void cai_input_claim_cleanup(cai_input_claim *claim) {
  if (claim != NULL) {
    cai_free_mem(NULL, claim->text);
    cai_free_mem(NULL, claim->input_events);
    memset(claim, 0, sizeof(*claim));
  }
}
int cai_input_queue_restore(cai_input_queue *queue,
                            const cai_input_record *record,
                            unsigned long long position, cai_error *error) {
  input_prepared prepared;
  input_node *node, *previous;
  cai_input_queue_client *client;
  size_t index;
  int rc = queue_valid(queue, error);
  if (rc != CAI_OK)
    return rc;
  if (record == NULL ||
      memchr(record->request.origin, '\0', sizeof(record->request.origin)) ==
          NULL ||
      memchr(record->entry.origin, '\0', sizeof(record->entry.origin)) ==
          NULL ||
      record->before_revision != queue->revision ||
      (queue->uncertain &&
       (record->client_id != queue->uncertain_client ||
        record->request.request_id != queue->uncertain_request)))
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "input queue journal revision mismatch");
  if (record->type == CAI_INPUT_RECORD_CLIENT) {
    if (record->after_revision != queue->revision || record->client_id == 0U ||
        queue->client_serial == ~(unsigned long long)0 ||
        record->client_id != queue->client_serial + 1U)
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "invalid input queue client record");
    rc = client_cached(queue, record->client_id, &client, error);
    if (rc != CAI_OK)
      return rc;
    queue->client_serial = record->client_id;
  } else if (record->type == CAI_INPUT_RECORD_CLAIM) {
    cai_input_entry claimed = record->entry;
    node = find_node(queue, record->request.item_id, &previous, &index);
    if (node != NULL && claimed.text == NULL)
      claimed.text = node->text;
    if (node == NULL || node->value.revision != record->request.item_revision ||
        queue->revision == ~(unsigned long long)0 ||
        record->after_revision != queue->revision + 1U ||
        !entry_equal(&node->value, &claimed))
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "invalid input queue claim record");
    queue->revision = record->after_revision;
    remove_node(queue, node, previous, index, record, 0, NULL);
  } else if (record->type == CAI_INPUT_RECORD_RELEASE) {
    if (record->client_id == 0U || record->client_id > queue->client_serial ||
        record->request.request_id != 0U || record->request.text != NULL ||
        queue->revision == ~(unsigned long long)0 ||
        record->after_revision != queue->revision + 1U ||
        !has_edits(queue, record->client_id))
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "invalid input edit release record");
    release_edits(queue, record, 0);
  } else if (record->type == CAI_INPUT_RECORD_REQUEST) {
    if (record->client_id == 0U || record->client_id > queue->client_serial ||
        record->request.request_id == 0U ||
        record->result.request_id != record->request.request_id ||
        memchr(record->result.reason, '\0', sizeof(record->result.reason)) ==
            NULL ||
        (record->result.outcome == CAI_INPUT_ACCEPTED &&
         record->result.code != CAI_OK) ||
        (record->result.outcome != CAI_INPUT_ACCEPTED &&
         record->result.outcome != CAI_INPUT_REJECTED))
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "invalid input queue request record");
    rc = client_cached(queue, record->client_id, &client, error);
    if (rc != CAI_OK)
      return rc;
    if (record->result.outcome == CAI_INPUT_REJECTED) {
      if (record->after_revision != queue->revision)
        return cai_set_error(error, CAI_ERR_PROTOCOL,
                             "rejected input changed queue revision");
    } else {
      rc = prepare_request(queue, record->client_id, &record->request,
                           &record->policy, &prepared, error);
      if (rc == CAI_OK &&
          prepared.record.result.outcome == CAI_INPUT_ACCEPTED &&
          prepared.record.request.text != NULL &&
          (record->request.operation == CAI_INPUT_OFFER ||
           record->request.operation == CAI_INPUT_END_EDIT))
        rc = reserve_input_event(
            prepared.append != NULL ? prepared.append : prepared.node, error);
      if (rc != CAI_OK) {
        prepared_clear(&prepared);
        return rc;
      }
      if (prepared.record.result.outcome != CAI_INPUT_ACCEPTED ||
          prepared.record.after_revision != record->after_revision ||
          prepared.record.result.item_id != record->result.item_id ||
          prepared.record.result.item_revision !=
              record->result.item_revision ||
          prepared.record.result.edit_token != record->result.edit_token ||
          !entry_equal(&prepared.record.entry, &record->entry)) {
        prepared_clear(&prepared);
        return cai_set_error(error, CAI_ERR_PROTOCOL,
                             "input queue journal transition mismatch");
      }
      apply_request(queue, &prepared, 0);
      if (position != 0U && prepared.record.request.text != NULL &&
          (record->request.operation == CAI_INPUT_OFFER ||
           record->request.operation == CAI_INPUT_END_EDIT)) {
        node = find_node(queue, prepared.record.entry.id, NULL, NULL);
        node->input_events[node->input_event_count++] = position;
      }
      prepared_clear(&prepared);
    }
    client = find_client(queue, record->client_id);
    if (client != NULL)
      result_save(client, &record->result);
  } else
    return cai_set_error(error, CAI_ERR_PROTOCOL, "unknown input queue record");
  queue->uncertain = 0;
  queue->uncertain_client = queue->uncertain_request = 0U;
  return CAI_OK;
}
int cai_input_queue_confirm_absent(cai_input_queue *queue, cai_error *error) {
  cai_input_queue_client *client;
  cai_input_result result;
  int rc = queue_valid(queue, error);
  if (rc != CAI_OK)
    return rc;
  if (!queue->uncertain)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "input queue has no uncertain append");
  rc = reserve_changes(queue, error);
  if (rc != CAI_OK)
    return rc;
  client = find_client(queue, queue->uncertain_client);
  if (client != NULL && queue->uncertain_request != 0U) {
    result_set(&result, queue->uncertain_request, CAI_INPUT_REJECTED,
               CAI_ERR_TRANSPORT,
               "journal confirms the request was not accepted");
    result_save(client, &result);
  }
  queue->uncertain = 0;
  queue->uncertain_client = queue->uncertain_request = 0U;
  changed(queue, CAI_INPUT_STATE, 0U, NULL, 0U, 0U);
  return CAI_OK;
}

#include "cai_input_queue_checkpoint.inc"
