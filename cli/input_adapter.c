#include "input_adapter.h"
#include "pouch_stream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cai_cli_input_adapter {
  cai_input_connection *connection;
  sl_t *editor;
  sl_watch_id_t watch;
  unsigned long long conversation, view;
  char revision[96];
};
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
  unsigned long long conversation = adapter->conversation, view;
  if (adapter->connection == NULL)
    return SL_ERROR_IO;
  memset(&target, 0, sizeof(target));
  if (revision != NULL &&
      (!decimal(&revision, &conversation) || *revision++ != '.' ||
       !decimal(&revision, &view) || *revision++ != '.' ||
       !decimal(&revision, &target.queue_revision) || *revision != '\0'))
    return SL_ERROR_INVALID;
  if (!identity(source->item_id, conversation, &target.item_id) ||
      !identity(source->item_revision, conversation, &target.item_revision) ||
      !identity(source->edit_token, conversation, &target.edit_token))
    return SL_ERROR_INVALID;
  target.request_id = source->request_id;
  target.operation = (cai_input_operation)source->operation;
  target.text = source->text;
  target.intent = source->mode == SL_PROMPT_QUEUE_MODE_STEER
                      ? CAI_INPUT_STEERING
                      : CAI_INPUT_QUEUED;
  target.enabled = source->enabled;
  target.target_request_id = source->target_request_id;
  return cai_input_connection_request(adapter->connection, conversation,
                                      &target, NULL) == CAI_OK
             ? SL_OK
             : SL_ERROR_IO;
}
static void detach(void *context) {
  cai_cli_input_adapter *adapter = context;
  adapter->editor = NULL;
  cai_input_connection_close(adapter->connection);
  adapter->connection = NULL;
}
static const sl_queue_backend_t callbacks = {
    request, detach, (1UL << (SL_QUEUE_RESOLVE + 1)) - 1UL, 0U};
int cai_cli_input_adapter_open(cai_input_controller *controller,
                               cai_cli_input_adapter **out, cai_error *error) {
  cai_cli_input_adapter *adapter;
  int rc;
  if (out == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "input adapter output is required");
  *out = NULL;
  adapter = calloc(1U, sizeof(*adapter));
  if (adapter == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM,
                         "allocate Softline input adapter");
  rc = cai_input_controller_connect(controller, &adapter->connection, error);
  if (rc != CAI_OK) {
    free(adapter);
    return rc;
  }
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
static int receive_one(cai_cli_input_adapter *adapter,
                       const cai_input_observation *observation,
                       cai_error *error) {
  sl_queue_change_t change;
  adapter_entry one, *owned = NULL;
  sl_queue_entry_t *entries = NULL;
  char revision[96], item[48];
  size_t i;
  int rc;
  if (observation->type == CAI_INPUT_OBSERVATION_RESULT) {
    sl_queue_result_t result;
    char item_revision[48], token[48];
    memset(&result, 0, sizeof(result));
    result.request_id = (sl_queue_request_id_t)observation->result.request_id;
    result.status = (sl_queue_result_status_t)observation->result.outcome;
    result.reason = observation->result.reason[0] != '\0'
                        ? observation->result.reason
                        : NULL;
    if (observation->result.item_id != 0U) {
      id_text(item, sizeof(item), observation->conversation,
              observation->result.item_id);
      result.item_id = item;
      id_text(item_revision, sizeof(item_revision), observation->conversation,
              observation->result.item_revision);
      result.item_revision = item_revision;
    }
    if (observation->result.edit_token != 0U) {
      id_text(token, sizeof(token), observation->conversation,
              observation->result.edit_token);
      result.edit_token = token;
    }
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
      if (observation->type == CAI_INPUT_OBSERVATION_SNAPSHOT) {
        change.kind = SL_QUEUE_SNAPSHOT;
        change.count = observation->count;
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
            entry_copy(&owned[i], &observation->entries[i],
                       observation->conversation);
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
          entry_copy(&one, &event->entry, observation->conversation);
          change.entries = &one.entry;
          change.count = 1U;
        } else if (event->type == CAI_INPUT_REMOVE) {
          id_text(item, sizeof(item), observation->conversation,
                  event->entry.id);
          change.item_id = item;
        }
      }
    }
    rc = sl_queue_backend_update(adapter->editor, &change);
    free(entries);
    free(owned);
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
  if (adapter == NULL || adapter->editor == NULL || maximum == 0U)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "bound input adapter is required");
  while (maximum-- != 0U) {
    rc = cai_input_connection_poll(adapter->connection, &observation, error);
    if (rc != CAI_OK || observation == NULL)
      break;
    rc = receive_one(adapter, observation, error);
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
  if (adapter == NULL || editor == NULL || adapter->connection == NULL ||
      adapter->editor != NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "unbound input adapter and editor are required");
  adapter->editor = editor;
  if (sl_watch_add(editor, cai_input_connection_fd(adapter->connection),
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
  if (adapter->connection != NULL)
    cai_input_connection_close(adapter->connection);
  free(adapter);
}
