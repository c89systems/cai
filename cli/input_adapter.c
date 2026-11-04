#include "input_adapter.h"
#include "error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cai_cli_input_adapter {
  cai_cli_input_transport transport;
  int detached;
  sl_t *editor;
  sl_watch_id_t watch;
  unsigned long long conversation, view;
  char revision[96];
  /* Explicit bounded editor mirror under construction. The producer/transport
   * sends one row at a time; Softline publishes its snapshot atomically. */
  cai_input_entry *snapshot;
  cai_input_state snapshot_state;
  unsigned long long snapshot_conversation;
  size_t snapshot_count;
  int snapshot_active;
};
static void snapshot_clear(cai_cli_input_adapter *adapter) {
  size_t i;
  for (i = 0U; i < adapter->snapshot_count; ++i) {
    void *text;
    memcpy(&text, &adapter->snapshot[i].text, sizeof(text));
    free(text);
  }
  free(adapter->snapshot);
  adapter->snapshot = NULL;
  adapter->snapshot_count = 0U;
  adapter->snapshot_active = 0;
}
static int decimal(const char **text, unsigned long long *out) {
  const char *at = *text;
  unsigned long long value = 0U, maximum = ~(unsigned long long)0;
  if (*at < '0' || *at > '9')
    return 0;
  do {
    unsigned int digit = (unsigned int)(*at - '0');
    if (value > (maximum - digit) / 10U)
      return 0;
    value = value * 10U + digit;
    ++at;
  } while (*at >= '0' && *at <= '9');
  *text = at;
  *out = value;
  return 1;
}
static int identity(const char *text, unsigned long long conversation,
                    unsigned long long *out) {
  unsigned long long epoch;
  *out = 0U;
  if (text == NULL)
    return 1;
  return decimal(&text, &epoch) && *text++ == '-' && epoch == conversation &&
         decimal(&text, out) && *text == '\0';
}
static int request(void *context, const sl_queue_request_t *source) {
  cai_cli_input_adapter *adapter = context;
  cai_input_request target;
  const char *revision = source->queue_revision;
  unsigned long long conversation = adapter->conversation, view, key;
  cai_input_scope scope;
  if (adapter->detached)
    return SL_ERROR_IO;
  memset(&target, 0, sizeof(target));
  if (revision != NULL &&
      (!decimal(&revision, &conversation) || *revision++ != '.' ||
       !decimal(&revision, &view) || *revision++ != '.' ||
       !decimal(&revision, &target.queue_revision) || *revision != '\0'))
    return SL_ERROR_INVALID;
  key = adapter->transport.identity(adapter->transport.context, conversation);
  if (!identity(source->item_id, key, &target.item_id) ||
      !identity(source->item_revision, key, &target.item_revision) ||
      !identity(source->edit_token, key, &target.edit_token))
    return SL_ERROR_INVALID;
  target.request_id = source->request_id;
  target.operation = (cai_input_operation)source->operation;
  target.text = source->text;
  target.intent = source->mode == SL_PROMPT_QUEUE_MODE_STEER
                      ? CAI_INPUT_STEERING
                      : CAI_INPUT_QUEUED;
  target.enabled = source->enabled;
  target.target_request_id = source->target_request_id;
  scope.conversation = conversation;
  scope.generation = 0U;
  return adapter->transport.request(adapter->transport.context, &scope, &target,
                                    NULL) == CAI_OK
             ? SL_OK
             : SL_ERROR_IO;
}
static void detach(void *context) {
  cai_cli_input_adapter *adapter = context;
  adapter->editor = NULL;
  if (!adapter->detached && adapter->transport.close != NULL)
    adapter->transport.close(adapter->transport.context);
  adapter->detached = 1;
  snapshot_clear(adapter);
}
static const sl_queue_backend_t callbacks = {
    request, detach, (1UL << (SL_QUEUE_RESOLVE + 1)) - 1UL, 0U};
int cai_cli_input_adapter_open(const cai_cli_input_transport *transport,
                               cai_cli_input_adapter **out, cai_error *error) {
  cai_cli_input_adapter *adapter;
  if (out != NULL)
    *out = NULL;
  if (out == NULL || transport == NULL || transport->request == NULL ||
      transport->identity == NULL ||
      ((transport->poll == NULL) != (transport->fd == NULL)))
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "input transport and adapter output are required");
  adapter = calloc(1U, sizeof(*adapter));
  if (adapter == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM,
                         "allocate Softline input adapter");
  adapter->transport = *transport;
  *out = adapter;
  return CAI_OK;
}
void cai_cli_input_adapter_configure(cai_cli_input_adapter *adapter,
                                     sl_config_t *config) {
  config->prompt_queue = 1;
  config->prompt_queue_max_entries = 128U;
  config->queue_backend = &callbacks;
  config->queue_backend_context = adapter;
}
typedef struct adapter_entry {
  sl_queue_entry_t entry;
  char id[48], revision[48], token[48];
} adapter_entry;
static void id_text(char *out, size_t size, unsigned long long conversation,
                    unsigned long long id) {
  snprintf(out, size, "%llu-%llu", conversation, id);
}
static void entry_copy(adapter_entry *target, const cai_input_entry *source,
                       unsigned long long conversation) {
  memset(target, 0, sizeof(*target));
  id_text(target->id, sizeof(target->id), conversation, source->id);
  id_text(target->revision, sizeof(target->revision), conversation,
          source->revision);
  target->entry.id = target->id;
  target->entry.revision = target->revision;
  target->entry.text = source->text;
  target->entry.mode = source->intent == CAI_INPUT_STEERING
                           ? SL_PROMPT_QUEUE_MODE_STEER
                           : SL_PROMPT_QUEUE_MODE_QUEUED;
  target->entry.held = source->held;
  target->entry.edit_held = source->edit_held;
  target->entry.ready = source->ready;
  target->entry.stopped = source->stopped;
  target->entry.action = source->action;
  if (source->edit_token != 0U) {
    id_text(target->token, sizeof(target->token), conversation,
            source->edit_token);
    target->entry.edit_token = target->token;
  }
}
static sl_queue_request_id_t resolved_request(cai_cli_input_adapter *adapter,
                                              sl_queue_request_id_t id) {
  sl_queue_backend_info_t info;
  sl_queue_pending_info_t pending;
  size_t i;
  if (sl_queue_backend_info(adapter->editor, &info) != SL_OK)
    return 0U;
  for (i = 0U; i < info.pending_count; ++i)
    if (sl_queue_backend_pending(adapter->editor, i, &pending) == SL_OK &&
        pending.request.request_id == id &&
        pending.request.operation == SL_QUEUE_RESOLVE)
      return pending.request.target_request_id;
  return 0U;
}
int cai_cli_input_adapter_observe(cai_cli_input_adapter *adapter,
                                  const cai_input_observation *observation,
                                  cai_error *error) {
  sl_queue_change_t change;
  adapter_entry one, *owned = NULL;
  sl_queue_entry_t *entries = NULL;
  char revision[96], item[48];
  unsigned long long key;
  size_t i;
  int rc;
  if (adapter == NULL || adapter->editor == NULL || adapter->detached ||
      observation == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "bound input adapter and observation required");
  key = adapter->transport.identity(adapter->transport.context,
                                    observation->conversation);
  if (observation->type == CAI_INPUT_OBSERVATION_SNAPSHOT_BEGIN) {
    snapshot_clear(adapter);
    if (observation->state.count > 128U)
      return cai_cli_error(error, CAI_ERR_LIMIT,
                           "editor queue exceeds 128 entries");
    if (observation->state.count != 0U) {
      adapter->snapshot =
          calloc(observation->state.count, sizeof(*adapter->snapshot));
      if (adapter->snapshot == NULL)
        return cai_cli_error(error, CAI_ERR_NOMEM,
                             "prepare editor queue mirror");
    }
    adapter->snapshot_state = observation->state;
    adapter->snapshot_conversation = observation->conversation;
    adapter->snapshot_active = 1;
    return CAI_OK;
  }
  if (observation->type == CAI_INPUT_OBSERVATION_SNAPSHOT_ITEM ||
      observation->type == CAI_INPUT_OBSERVATION_SNAPSHOT_END) {
    if (!adapter->snapshot_active ||
        observation->conversation != adapter->snapshot_conversation ||
        observation->state.view_revision !=
            adapter->snapshot_state.view_revision ||
        observation->state.count != adapter->snapshot_state.count ||
        observation->index != adapter->snapshot_count)
      return cai_cli_error(error, CAI_ERR_PROTOCOL,
                           "editor snapshot sequence mismatch");
    if (observation->type == CAI_INPUT_OBSERVATION_SNAPSHOT_ITEM) {
      char *text;
      if (adapter->snapshot_count >= adapter->snapshot_state.count ||
          observation->entry.text == NULL)
        return cai_cli_error(error, CAI_ERR_PROTOCOL,
                             "editor snapshot item is invalid");
      text = strdup(observation->entry.text);
      if (text == NULL)
        return cai_cli_error(error, CAI_ERR_NOMEM, "retain editor queue entry");
      adapter->snapshot[adapter->snapshot_count] = observation->entry;
      adapter->snapshot[adapter->snapshot_count++].text = text;
      return CAI_OK;
    }
    if (adapter->snapshot_count != adapter->snapshot_state.count)
      return cai_cli_error(error, CAI_ERR_PROTOCOL,
                           "editor snapshot ended early");
  }
  if (observation->type == CAI_INPUT_OBSERVATION_DISCONNECTED)
    snapshot_clear(adapter);
  if (observation->type == CAI_INPUT_OBSERVATION_RESULT) {
    sl_queue_result_t result;
    sl_queue_request_id_t target;
    char item_revision[48], token[48];
    memset(&result, 0, sizeof(result));
    result.request_id = (sl_queue_request_id_t)observation->result.request_id;
    result.status = (sl_queue_result_status_t)observation->result.outcome;
    result.reason = observation->result.reason[0] != '\0'
                        ? observation->result.reason
                        : NULL;
    if (observation->result.item_id != 0U) {
      id_text(item, sizeof(item), key, observation->result.item_id);
      result.item_id = item;
      id_text(item_revision, sizeof(item_revision), key,
              observation->result.item_revision);
      result.item_revision = item_revision;
    }
    if (observation->result.edit_token != 0U) {
      id_text(token, sizeof(token), key, observation->result.edit_token);
      result.edit_token = token;
    }
    target = resolved_request(adapter, result.request_id);
    rc = SL_OK;
    if (target != 0U) {
      sl_queue_request_id_t resolver = result.request_id;
      /* The backend receipt acknowledges the RESOLVE operation. Its outcome
       * also settles the original unknown proposal, without another offer. */
      result.request_id = target;
      rc = sl_queue_backend_result(adapter->editor, &result);
      result.request_id = resolver;
    }
    if (rc == SL_OK)
      rc = sl_queue_backend_result(adapter->editor, &result);
  } else {
    memset(&change, 0, sizeof(change));
    change.kind = SL_QUEUE_DISCONNECTED;
    if (observation->type != CAI_INPUT_OBSERVATION_DISCONNECTED) {
      const cai_input_state *state = &observation->state;
      snprintf(revision, sizeof(revision), "%llu.%llu.%llu",
               observation->conversation, state->view_revision,
               state->revision);
      change.revision = revision;
      change.total_count = state->count;
      change.stopped = state->stopped;
      change.auto_approval = state->auto_approval;
      if (observation->type == CAI_INPUT_OBSERVATION_SNAPSHOT_END) {
        change.kind = SL_QUEUE_SNAPSHOT;
        change.count = adapter->snapshot_count;
        if (change.count != 0U) {
          owned = calloc(change.count, sizeof(*owned));
          entries = calloc(change.count, sizeof(*entries));
          if (owned == NULL || entries == NULL) {
            free(owned);
            free(entries);
            return cai_cli_error(error, CAI_ERR_NOMEM,
                                 "copy frontend queue snapshot");
          }
          for (i = 0U; i < change.count; ++i) {
            entry_copy(&owned[i], &adapter->snapshot[i], key);
            entries[i] = owned[i].entry;
          }
          change.entries = entries;
        }
      } else {
        const cai_input_change *event = &observation->change;
        if (observation->conversation != adapter->conversation ||
            event->previous_view_revision != adapter->view)
          return cai_cli_error(error, CAI_ERR_PROTOCOL,
                               "frontend queue observation gap");
        change.kind = event->type == CAI_INPUT_UPSERT   ? SL_QUEUE_UPSERT
                      : event->type == CAI_INPUT_REMOVE ? SL_QUEUE_REMOVE
                                                        : SL_QUEUE_STATE;
        change.previous_revision = adapter->revision;
        change.index = event->index;
        change.request_id = (sl_queue_request_id_t)event->request_id;
        if (event->type == CAI_INPUT_UPSERT) {
          entry_copy(&one, &event->entry, key);
          change.entries = &one.entry;
          change.count = 1U;
        } else if (event->type == CAI_INPUT_REMOVE) {
          id_text(item, sizeof(item), key, event->entry.id);
          change.item_id = item;
        }
      }
    }
    rc = sl_queue_backend_update(adapter->editor, &change);
    free(entries);
    free(owned);
    if (observation->type == CAI_INPUT_OBSERVATION_SNAPSHOT_END)
      snapshot_clear(adapter);
    if (rc == SL_OK && change.kind != SL_QUEUE_DISCONNECTED) {
      adapter->conversation = observation->conversation;
      adapter->view = observation->state.view_revision;
      snprintf(adapter->revision, sizeof(adapter->revision), "%s", revision);
    }
  }
  return rc == SL_OK ? CAI_OK
                     : cai_cli_error(error, CAI_ERR_TRANSPORT,
                                     "apply Softline queue observation");
}
int cai_cli_input_adapter_receive(cai_cli_input_adapter *adapter,
                                  size_t maximum, cai_error *error) {
  cai_input_observation *observation = NULL;
  int rc = CAI_OK;
  if (adapter == NULL || adapter->editor == NULL || maximum == 0U ||
      adapter->detached || adapter->transport.poll == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "bound input adapter is required");
  while (maximum-- != 0U) {
    rc = adapter->transport.poll(adapter->transport.context, &observation,
                                 error);
    if (rc != CAI_OK || observation == NULL)
      break;
    rc = cai_cli_input_adapter_observe(adapter, observation, error);
    cai_input_observation_close(observation);
    if (rc != CAI_OK)
      break;
  }
  return rc;
}
int cai_cli_input_adapter_settling(cai_cli_input_adapter *adapter) {
  sl_queue_backend_info_t info;
  sl_queue_pending_info_t pending;
  size_t i;
  if (adapter == NULL || adapter->editor == NULL ||
      sl_queue_backend_info(adapter->editor, &info) != SL_OK)
    return 0;
  for (i = 0U; i < info.pending_count; ++i)
    if (sl_queue_backend_pending(adapter->editor, i, &pending) == SL_OK &&
        !pending.pending && pending.status == SL_QUEUE_ACCEPTED)
      return 1;
  return 0;
}
static int wakeup(sl_t *editor, const sl_watch_event_t *event, void *context) {
  (void)editor;
  (void)event;
  return cai_cli_input_adapter_receive(context, 64U, NULL) == CAI_OK
             ? SL_OK
             : SL_ERROR_IO;
}
int cai_cli_input_adapter_bind(cai_cli_input_adapter *adapter, sl_t *editor,
                               cai_error *error) {
  if (adapter == NULL || editor == NULL || adapter->detached ||
      adapter->editor != NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "unbound input adapter and editor are required");
  adapter->editor = editor;
  if (adapter->transport.fd == NULL)
    return CAI_OK;
  if (sl_watch_add(editor, adapter->transport.fd(adapter->transport.context),
                   SL_WATCH_READ | SL_WATCH_ERROR | SL_WATCH_HANGUP, wakeup,
                   adapter, &adapter->watch) != SL_OK) {
    adapter->editor = NULL;
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "watch frontend input observations");
  }
  return CAI_OK;
}
void cai_cli_input_adapter_close(cai_cli_input_adapter *adapter) {
  if (adapter == NULL)
    return;
  if (!adapter->detached && adapter->transport.close != NULL)
    adapter->transport.close(adapter->transport.context);
  snapshot_clear(adapter);
  free(adapter);
}
