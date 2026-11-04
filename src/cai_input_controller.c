#define _POSIX_C_SOURCE 200809L
#include "cai_input_queue_internal.h"
#include "cai_internal.h"
#include <cai/input_controller.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CONNECTIONS 16U
#define OBSERVATIONS 256U
#define OBSERVATION_BYTES (16U * 1024U * 1024U)
typedef struct input_message {
  struct input_message *next;
  cai_input_observation value;
  size_t bytes;
  int reservation;
} input_message;
typedef struct input_submission {
  struct input_submission *next;
  cai_input_scope scope;
  cai_input_request value;
  char *text;
} input_submission;
struct cai_input_connection {
  struct cai_input_connection *next;
  cai_input_controller *controller;
  cai_input_queue_client *client;
  input_submission *head, *tail;
  input_message *messages, *messages_tail;
  input_message *binding_notice;
  size_t submitted, ordinary, reserved[3], message_count, message_bytes;
  unsigned long long conversation, resume_producer;
  cai_input_state snapshot_state;
  size_t snapshot_index;
  uint64_t attachment;
  cai_input_result binding_error;
  int fd, wake_fd, detached, synchronized, needs_snapshot, snapshot_started,
      binding_failed;
};
struct cai_input_controller {
  cai_input_controller_config config;
  pthread_mutex_t lock;
  pthread_t owner;
  cai_input_queue *queue;
  cai_input_connection *connections;
  unsigned long long conversation;
  size_t count, references;
  int fd, wake_fd, closed;
};
static int wakeup_open(int *read_fd, int *write_fd) {
  int descriptors[2], i;
  if (pipe(descriptors) != 0)
    return -1;
  for (i = 0; i < 2; ++i)
    if (fcntl(descriptors[i], F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(descriptors[i], F_SETFL, O_NONBLOCK) != 0) {
      close(descriptors[0]);
      close(descriptors[1]);
      return -1;
    }
  *read_fd = descriptors[0];
  *write_fd = descriptors[1];
  return 0;
}
static void signal_fd(int fd) {
  char one = 'i';
  while (write(fd, &one, sizeof(one)) < 0 && errno == EINTR) {
  }
}
static void drain_fd(int fd) {
  char value[64];
  while (read(fd, value, sizeof(value)) > 0 || errno == EINTR) {
  }
}
static void controller_release(cai_input_controller *controller) {
  int release;
  pthread_mutex_lock(&controller->lock);
  release = --controller->references == 0U;
  pthread_mutex_unlock(&controller->lock);
  if (release) {
    close(controller->fd);
    close(controller->wake_fd);
    pthread_mutex_destroy(&controller->lock);
    cai_free_mem(NULL, controller);
  }
}
static int owner(cai_input_controller *controller, cai_error *error) {
  if (controller == NULL || controller->closed ||
      !pthread_equal(controller->owner, pthread_self()))
    return cai_set_error(error, CAI_ERR_INVALID,
                         "input controller requires its owner thread");
  return CAI_OK;
}
static void free_text(const char *text) {
  void *owned;
  memcpy(&owned, &text, sizeof(owned));
  cai_free_mem(NULL, owned);
}
void cai_input_observation_close(cai_input_observation *observation) {
  if (observation == NULL)
    return;
  free_text(observation->entry.text);
  free_text(observation->change.entry.text);
  /* value is the first member after the private link, not separately owned. */
  cai_free_mem(NULL, (char *)observation - offsetof(input_message, value));
}
static input_message *message_new(cai_input_observation_type type) {
  input_message *message = cai_alloc(NULL, sizeof(*message));
  if (message != NULL) {
    memset(message, 0, sizeof(*message));
    message->value.type = type;
    message->bytes = sizeof(*message);
  }
  return message;
}
static void append_message(cai_input_connection *connection,
                           input_message *message) {
  if (connection->messages_tail != NULL)
    connection->messages_tail->next = message;
  else
    connection->messages = message;
  connection->messages_tail = message;
  ++connection->message_count;
  connection->message_bytes += message->bytes;
  signal_fd(connection->wake_fd);
}
static void producer_metadata(cai_input_connection *connection,
                              cai_input_observation *value) {
  if (value->conversation == connection->conversation &&
      connection->client != NULL) {
    value->producer = cai_input_queue_client_id(connection->client);
    value->last_request =
        cai_input_queue_client_last_request(connection->client);
  }
}
/* Caller holds the controller lock. No fallback to a new logical producer. */
static void binding_failed(cai_input_connection *connection, int code,
                           const char *reason) {
  input_message *message;
  if (connection->binding_failed)
    return;
  message = connection->binding_notice;
  connection->binding_notice = NULL;
  connection->binding_failed = 1;
  connection->needs_snapshot = connection->synchronized =
      connection->snapshot_started = 0;
  connection->binding_error.outcome = CAI_INPUT_REJECTED;
  connection->binding_error.code = code;
  snprintf(connection->binding_error.reason,
           sizeof(connection->binding_error.reason), "%s", reason);
  message->value.conversation = connection->conversation;
  message->value.result = connection->binding_error;
  append_message(connection, message);
}
static void disconnect_locked(cai_input_connection *connection) {
  input_message *message;
  if (!connection->synchronized && !connection->snapshot_started)
    return;
  connection->synchronized = connection->snapshot_started = 0;
  message = message_new(CAI_INPUT_OBSERVATION_DISCONNECTED);
  if (message != NULL) {
    message->value.conversation = connection->conversation;
    producer_metadata(connection, &message->value);
    append_message(connection, message);
  } else
    binding_failed(connection, CAI_ERR_NOMEM,
                   "input observation allocation failed");
}
static void observe(void *context, const cai_input_change *change) {
  cai_input_controller *controller = context;
  cai_input_connection *connection;
  pthread_mutex_lock(&controller->lock);
  for (connection = controller->connections; connection != NULL;
       connection = connection->next) {
    input_message *message;
    if (connection->detached || connection->client == NULL)
      continue;
    if (!connection->synchronized) {
      if (connection->snapshot_started) {
        disconnect_locked(connection);
        connection->needs_snapshot = !connection->binding_failed;
      }
      continue;
    }
    if (connection->message_count >= OBSERVATIONS ||
        connection->message_bytes >=
            OBSERVATION_BYTES - sizeof(input_message) - 66049U) {
      disconnect_locked(connection);
      continue;
    }
    message = message_new(CAI_INPUT_OBSERVATION_CHANGE);
    if (message == NULL) {
      disconnect_locked(connection);
      continue;
    }
    message->value.conversation = connection->conversation;
    producer_metadata(connection, &message->value);
    message->value.change = *change;
    message->value.state = change->state;
    message->value.change.entry.text = NULL;
    if (change->entry.text != NULL) {
      message->value.change.entry.text = cai_strdup(NULL, change->entry.text);
      if (message->value.change.entry.text == NULL) {
        cai_input_observation_close(&message->value);
        disconnect_locked(connection);
        continue;
      }
      message->bytes += strlen(change->entry.text) + 1U;
    }
    if (change->entry.edit_owner !=
        cai_input_queue_client_id(connection->client))
      message->value.change.entry.edit_token = 0U;
    if (change->client_id != cai_input_queue_client_id(connection->client))
      message->value.change.request_id = 0U;
    append_message(connection, message);
  }
  pthread_mutex_unlock(&controller->lock);
}
static int snapshot(cai_input_connection *connection, cai_error *error) {
  cai_input_controller *controller = connection->controller;
  size_t budget = 4U;
  while (budget-- != 0U) {
    input_message *message;
    cai_input_state current;
    cai_input_observation_type type;
    int full, rc;
    pthread_mutex_lock(&controller->lock);
    full = connection->detached || connection->binding_failed ||
           !connection->needs_snapshot || connection->message_count >= 4U ||
           connection->message_bytes >= 2U * 66049U;
    pthread_mutex_unlock(&controller->lock);
    if (full)
      return CAI_OK;
    rc = cai_input_queue_get_state(controller->queue, &current, error);
    if (rc != CAI_OK)
      return rc;
    /* One owner serializes row reads and queue mutation. A mutation between
     * steps aborts the old baseline through observe(), never mixing versions.
     */
    type = !connection->snapshot_started ? CAI_INPUT_OBSERVATION_SNAPSHOT_BEGIN
           : connection->snapshot_index < connection->snapshot_state.count
               ? CAI_INPUT_OBSERVATION_SNAPSHOT_ITEM
               : CAI_INPUT_OBSERVATION_SNAPSHOT_END;
    message = message_new(type);
    if (message == NULL) {
      pthread_mutex_lock(&controller->lock);
      binding_failed(connection, CAI_ERR_NOMEM,
                     "input snapshot allocation failed");
      pthread_mutex_unlock(&controller->lock);
      return CAI_OK;
    }
    message->value.conversation = connection->conversation;
    producer_metadata(connection, &message->value);
    message->value.state = current;
    message->value.index = type == CAI_INPUT_OBSERVATION_SNAPSHOT_BEGIN
                               ? 0U
                               : connection->snapshot_index;
    if (type == CAI_INPUT_OBSERVATION_SNAPSHOT_ITEM) {
      const char *text;
      rc = cai_input_queue_entry_at(connection->client,
                                    connection->snapshot_index,
                                    &message->value.entry, error);
      text = message->value.entry.text;
      message->value.entry.text = NULL;
      if (rc == CAI_OK) {
        message->value.entry.text = cai_strdup(NULL, text);
        if (message->value.entry.text == NULL)
          rc = cai_set_error(error, CAI_ERR_NOMEM, "copy one input entry");
        else
          message->bytes += strlen(text) + 1U;
      }
      if (rc != CAI_OK) {
        cai_input_observation_close(&message->value);
        if (rc == CAI_ERR_NOMEM) {
          pthread_mutex_lock(&controller->lock);
          binding_failed(connection, rc, "input snapshot allocation failed");
          pthread_mutex_unlock(&controller->lock);
          cai_error_cleanup(error);
          return CAI_OK;
        }
        return rc;
      }
    }
    pthread_mutex_lock(&controller->lock);
    if (type == CAI_INPUT_OBSERVATION_SNAPSHOT_BEGIN) {
      connection->snapshot_state = current;
      connection->snapshot_index = 0U;
      connection->snapshot_started = 1;
      connection->synchronized = 0;
    } else if (type == CAI_INPUT_OBSERVATION_SNAPSHOT_ITEM)
      ++connection->snapshot_index;
    else {
      connection->synchronized = 1;
      connection->needs_snapshot = connection->snapshot_started = 0;
    }
    append_message(connection, message);
    pthread_mutex_unlock(&controller->lock);
  }
  return CAI_OK;
}
int cai_input_controller_open(const cai_input_controller_config *config,
                              cai_input_controller **out, cai_error *error) {
  cai_input_controller *controller;
  if (out == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "input controller output is required");
  *out = NULL;
  controller = cai_alloc(NULL, sizeof(*controller));
  if (controller == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "allocate input controller");
  memset(controller, 0, sizeof(*controller));
  if (config != NULL)
    controller->config = *config;
  if (pthread_mutex_init(&controller->lock, NULL) != 0) {
    cai_free_mem(NULL, controller);
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "initialize input controller");
  }
  if (wakeup_open(&controller->fd, &controller->wake_fd) != 0) {
    pthread_mutex_destroy(&controller->lock);
    cai_free_mem(NULL, controller);
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "open input controller wakeup");
  }
  controller->owner = pthread_self();
  controller->references = 1U;
  *out = controller;
  return CAI_OK;
}
static void connection_free(cai_input_connection *connection) {
  input_submission *submission;
  input_message *message;
  while ((submission = connection->head) != NULL) {
    connection->head = submission->next;
    cai_free_mem(NULL, submission->text);
    cai_free_mem(NULL, submission);
  }
  while ((message = connection->messages) != NULL) {
    connection->messages = message->next;
    cai_input_observation_close(&message->value);
  }
  if (connection->binding_notice != NULL)
    cai_input_observation_close(&connection->binding_notice->value);
  if (connection->fd >= 0)
    close(connection->fd);
  if (connection->wake_fd >= 0)
    close(connection->wake_fd);
  cai_free_mem(NULL, connection);
}
int cai_input_controller_select(cai_input_controller *controller,
                                cai_input_queue *queue, cai_error *error) {
  cai_input_connection *connection;
  cai_input_queue *previous;
  cai_input_state state;
  int rc = owner(controller, error);
  if (rc != CAI_OK || controller->queue == queue)
    return rc;
  if (controller->conversation == ~(unsigned long long)0)
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "input conversation identities exhausted");
  if (queue != NULL &&
      cai_input_queue_get_state(queue, &state, error) != CAI_OK)
    return CAI_ERR_INVALID;
  if (queue != NULL) {
    rc = cai_input_queue_release_orphans(queue, error);
    if (rc != CAI_OK)
      return rc;
  }
  if (queue != NULL && cai_input_queue_retain(queue, error) != CAI_OK)
    return CAI_ERR_INVALID;
  if (controller->queue != NULL &&
      cai_input_queue_set_observer(controller->queue, NULL, NULL, error) !=
          CAI_OK) {
    cai_input_queue_release(queue);
    return CAI_ERR_INVALID;
  }
  if (queue != NULL) {
    rc = cai_input_queue_set_observer(queue, observe, controller, error);
    if (rc != CAI_OK) {
      if (controller->queue != NULL)
        cai_input_queue_set_observer(controller->queue, observe, controller,
                                     NULL);
      cai_input_queue_release(queue);
      return rc;
    }
  }
  previous = controller->queue;
  pthread_mutex_lock(&controller->lock);
  controller->queue = queue;
  ++controller->conversation;
  for (connection = controller->connections; connection != NULL;
       connection = connection->next) {
    disconnect_locked(connection);
    cai_input_queue_client_close(connection->client);
    connection->client = NULL;
    if ((connection->resume_producer != 0U || connection->detached) &&
        !connection->binding_failed) {
      binding_failed(connection, CAI_ERR_INVALID,
                     "conversation changed before input binding");
    }
    connection->conversation = controller->conversation;
    connection->needs_snapshot = !connection->binding_failed;
  }
  pthread_mutex_unlock(&controller->lock);
  cai_input_queue_release(previous);
  signal_fd(controller->wake_fd);
  return rc;
}
static int connect_input(cai_input_controller *controller, uint64_t attachment,
                         unsigned long long conversation,
                         unsigned long long producer,
                         cai_input_connection **out, cai_error *error) {
  cai_input_connection *connection;
  if (controller == NULL || out == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "input connection output is required");
  *out = NULL;
  connection = cai_alloc(NULL, sizeof(*connection));
  if (connection == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "allocate input connection");
  memset(connection, 0, sizeof(*connection));
  connection->fd = connection->wake_fd = -1;
  connection->binding_notice = message_new(CAI_INPUT_OBSERVATION_DISCONNECTED);
  if (connection->binding_notice == NULL) {
    connection_free(connection);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "reserve producer binding receipt");
  }
  if (wakeup_open(&connection->fd, &connection->wake_fd) != 0) {
    connection_free(connection);
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "open input observation wakeup");
  }
  pthread_mutex_lock(&controller->lock);
  if (controller->closed || controller->count >= CONNECTIONS) {
    pthread_mutex_unlock(&controller->lock);
    connection_free(connection);
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "input controller is closed or full");
  }
  connection->controller = controller;
  connection->attachment = attachment;
  if (producer != 0U && conversation != controller->conversation) {
    pthread_mutex_unlock(&controller->lock);
    connection_free(connection);
    return cai_set_error(
        error, CAI_ERR_INVALID,
        "conversation changed; producer resume was not accepted");
  }
  connection->resume_producer = producer;
  connection->conversation = controller->conversation;
  connection->needs_snapshot = 1;
  connection->next = controller->connections;
  controller->connections = connection;
  ++controller->count;
  ++controller->references;
  pthread_mutex_unlock(&controller->lock);
  signal_fd(controller->wake_fd);
  *out = connection;
  return CAI_OK;
}
int cai_input_controller_connect(cai_input_controller *controller,
                                 uint64_t attachment,
                                 cai_input_connection **out, cai_error *error) {
  return connect_input(controller, attachment, 0U, 0U, out, error);
}
int cai_input_controller_conversation(cai_input_controller *controller,
                                      unsigned long long *out,
                                      cai_error *error) {
  int available;
  if (out != NULL)
    *out = 0U;
  if (controller == NULL || out == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "conversation output required");
  pthread_mutex_lock(&controller->lock);
  available = !controller->closed && controller->queue != NULL;
  if (available)
    *out = controller->conversation;
  pthread_mutex_unlock(&controller->lock);
  return available ? CAI_OK
                   : cai_set_error(error, CAI_ERR_INVALID,
                                   "no input conversation selected");
}
int cai_input_connection_release_edits(cai_input_connection *connection,
                                       cai_error *error) {
  int rc;
  if (connection == NULL)
    return cai_set_error(error, CAI_ERR_INVALID, "input connection required");
  rc = owner(connection->controller, error);
  if (rc != CAI_OK || connection->client == NULL)
    return rc;
  return cai_input_queue_client_release_edits(connection->client, error);
}
int cai_input_controller_release_edits(cai_input_controller *controller,
                                       uint64_t attachment, cai_error *error) {
  cai_input_connection *connection;
  int rc = owner(controller, error);
  if (rc != CAI_OK)
    return rc;
  pthread_mutex_lock(&controller->lock);
  connection = controller->connections;
  pthread_mutex_unlock(&controller->lock);
  /* Only this owner removes links. Frontend connect may prepend but cannot
   * change these links, and callbacks never reenter this controller. */
  for (; connection != NULL; connection = connection->next)
    if (connection->attachment == attachment && connection->client != NULL) {
      rc = cai_input_queue_client_release_edits(connection->client, error);
      if (rc != CAI_OK)
        return rc;
    }
  return CAI_OK;
}
int cai_input_controller_resume(cai_input_controller *controller,
                                uint64_t attachment,
                                unsigned long long conversation,
                                unsigned long long producer,
                                cai_input_connection **out, cai_error *error) {
  if (out != NULL)
    *out = NULL;
  if (conversation == 0U || producer == 0U)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "verified conversation and producer are required");
  return connect_input(controller, attachment, conversation, producer, out,
                       error);
}
static int reserved(cai_input_operation operation) {
  return operation == CAI_INPUT_SYNCHRONIZE ? 0
         : operation == CAI_INPUT_RESOLVE   ? 1
         : operation == CAI_INPUT_STOP      ? 2
                                            : -1;
}
int cai_input_connection_request(cai_input_connection *connection,
                                 const cai_input_scope *scope,
                                 const cai_input_request *request,
                                 cai_error *error) {
  cai_input_controller *controller;
  input_submission *submission;
  size_t n = 0U;
  int slot;
  if (connection == NULL || scope == NULL || request == NULL ||
      request->request_id == 0U || request->operation < CAI_INPUT_OFFER ||
      request->operation > CAI_INPUT_RESOLVE)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "input request identity and operation are required");
  if (request->text != NULL) {
    while (n <= 66048U && request->text[n] != '\0')
      ++n;
    if (n > 66048U)
      return cai_set_error(error, CAI_ERR_LIMIT, "input exceeds 66048 bytes");
  }
  submission = cai_alloc(NULL, sizeof(*submission));
  if (submission == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "copy input submission");
  memset(submission, 0, sizeof(*submission));
  submission->value = *request;
  submission->scope = *scope;
  if (request->text != NULL) {
    submission->text = cai_strdup(NULL, request->text);
    if (submission->text == NULL) {
      cai_free_mem(NULL, submission);
      return cai_set_error(error, CAI_ERR_NOMEM, "copy input submission text");
    }
    submission->value.text = submission->text;
  }
  slot = reserved(request->operation);
  controller = connection->controller;
  pthread_mutex_lock(&controller->lock);
  if (controller->closed || connection->detached ||
      connection->binding_failed ||
      (slot >= 0 ? connection->reserved[slot] != 0U
                 : connection->ordinary >= 16U)) {
    pthread_mutex_unlock(&controller->lock);
    cai_free_mem(NULL, submission->text);
    cai_free_mem(NULL, submission);
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "input handoff is disconnected or full");
  }
  if (slot >= 0)
    ++connection->reserved[slot];
  else
    ++connection->ordinary;
  if (connection->tail != NULL)
    connection->tail->next = submission;
  else
    connection->head = submission;
  connection->tail = submission;
  ++connection->submitted;
  pthread_mutex_unlock(&controller->lock);
  signal_fd(controller->wake_fd);
  return CAI_OK;
}
typedef struct input_admission {
  cai_input_controller *controller;
  cai_input_connection *connection;
  input_submission *submission;
} input_admission;
static int authorize_submission(void *context, cai_error *error) {
  input_admission *admission = context;
  cai_input_controller *controller = admission->controller;
  return controller->config.authorize != NULL
             ? controller->config.authorize(
                   controller->config.authorize_context,
                   admission->connection->attachment,
                   admission->submission->scope.generation, error)
             : CAI_OK;
}
int cai_input_controller_pump(cai_input_controller *controller,
                              size_t max_requests, cai_error *error) {
  cai_input_connection **at, *connection;
  int rc = owner(controller, error);
  if (rc != CAI_OK || max_requests == 0U)
    return rc != CAI_OK ? rc
                        : cai_set_error(error, CAI_ERR_INVALID,
                                        "positive dispatch bound required");
  drain_fd(controller->fd);
  /* Only the owner removes connections. Frontend close just marks/detaches. */
  pthread_mutex_lock(&controller->lock);
  at = &controller->connections;
  while ((connection = *at) != NULL) {
    if (connection->detached && connection->head == NULL) {
      *at = connection->next;
      --controller->count;
      cai_input_queue_client_close(connection->client);
      connection_free(connection);
      --controller->references;
    } else
      at = &connection->next;
  }
  connection = controller->connections;
  pthread_mutex_unlock(&controller->lock);
  for (; connection != NULL; connection = connection->next) {
    if (connection->client == NULL && controller->queue != NULL &&
        !connection->binding_failed) {
      if (connection->resume_producer != 0U) {
        cai_error binding;
        cai_error_init(&binding);
        rc = cai_input_queue_client_resume(controller->queue,
                                           connection->resume_producer,
                                           &connection->client, &binding);
        if (rc == CAI_OK)
          connection->resume_producer = 0U;
        else if (rc == CAI_ERR_INVALID || rc == CAI_ERR_LIMIT) {
          pthread_mutex_lock(&controller->lock);
          binding_failed(connection, rc,
                         binding.message != NULL
                             ? binding.message
                             : "producer resume was not accepted");
          pthread_mutex_unlock(&controller->lock);
          rc = CAI_OK;
        } else
          cai_set_error(error, rc,
                        binding.message != NULL ? binding.message
                                                : "producer resume failed");
        cai_error_cleanup(&binding);
      } else
        rc = cai_input_queue_client_open(controller->queue, &connection->client,
                                         error);
      if (rc != CAI_OK)
        return rc;
    }
    while (max_requests != 0U) {
      input_submission *submission;
      input_message *message;
      int slot;
      pthread_mutex_lock(&controller->lock);
      submission = connection->head;
      if (submission != NULL) {
        connection->head = submission->next;
        if (connection->head == NULL)
          connection->tail = NULL;
        --connection->submitted;
      }
      pthread_mutex_unlock(&controller->lock);
      if (submission == NULL)
        break;
      --max_requests;
      message = message_new(CAI_INPUT_OBSERVATION_RESULT);
      if (message == NULL) {
        /* No operation has been admitted. Restore transport ownership. */
        pthread_mutex_lock(&controller->lock);
        submission->next = connection->head;
        connection->head = submission;
        if (connection->tail == NULL)
          connection->tail = submission;
        ++connection->submitted;
        pthread_mutex_unlock(&controller->lock);
        signal_fd(controller->wake_fd);
        return cai_set_error(error, CAI_ERR_NOMEM,
                             "prepare input result receipt");
      }
      message->value.conversation = submission->scope.conversation;
      message->value.result.request_id = submission->value.request_id;
      if (connection->binding_failed) {
        message->value.result = connection->binding_error;
        message->value.result.request_id = submission->value.request_id;
      } else if (connection->client == NULL ||
                 (submission->scope.conversation != connection->conversation &&
                  !(submission->scope.conversation == 0U &&
                    submission->value.operation == CAI_INPUT_SYNCHRONIZE))) {
        message->value.result.outcome = CAI_INPUT_REJECTED;
        message->value.result.code = CAI_ERR_INVALID;
        strcpy(message->value.result.reason,
               "conversation changed; request was not accepted");
      } else {
        input_admission admission;
        admission.controller = controller;
        admission.connection = connection;
        admission.submission = submission;
        rc = cai_input_queue_admit(connection->client, &submission->value,
                                   authorize_submission, &admission,
                                   &message->value.result, error);
        if (rc != CAI_OK) {
          message->value.result.outcome = CAI_INPUT_REJECTED;
          message->value.result.code = rc;
          strcpy(message->value.result.reason,
                 "input queue is unavailable; request was not accepted");
          cai_error_cleanup(error);
          rc = CAI_OK;
        }
        if (rc == CAI_OK &&
            submission->value.operation == CAI_INPUT_SYNCHRONIZE) {
          pthread_mutex_lock(&controller->lock);
          disconnect_locked(connection);
          connection->needs_snapshot = !connection->binding_failed;
          pthread_mutex_unlock(&controller->lock);
        }
      }
      slot = reserved(submission->value.operation);
      producer_metadata(connection, &message->value);
      /* The result owns this request's bounded receipt reservation until poll.
       */
      message->reservation = slot;
      pthread_mutex_lock(&controller->lock);
      append_message(connection, message);
      pthread_mutex_unlock(&controller->lock);
      cai_free_mem(NULL, submission->text);
      cai_free_mem(NULL, submission);
      if (rc != CAI_OK)
        return rc;
    }
    if (connection->needs_snapshot && connection->client != NULL) {
      rc = snapshot(connection, error);
      if (rc != CAI_OK)
        return rc;
    }
    pthread_mutex_lock(&controller->lock);
    if (connection->head != NULL ||
        (connection->needs_snapshot && !connection->detached &&
         !connection->binding_failed && connection->client != NULL &&
         connection->message_count < 4U &&
         connection->message_bytes < 2U * 66049U))
      signal_fd(controller->wake_fd);
    pthread_mutex_unlock(&controller->lock);
  }
  pthread_mutex_lock(&controller->lock);
  at = &controller->connections;
  while ((connection = *at) != NULL) {
    if (connection->detached && connection->head == NULL) {
      *at = connection->next;
      --controller->count;
      cai_input_queue_client_close(connection->client);
      connection_free(connection);
      --controller->references;
    } else
      at = &connection->next;
  }
  pthread_mutex_unlock(&controller->lock);
  return CAI_OK;
}
int cai_input_connection_poll(cai_input_connection *connection,
                              cai_input_observation **out, cai_error *error) {
  input_message *message;
  char one;
  if (connection == NULL || out == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "input observation output is required");
  *out = NULL;
  pthread_mutex_lock(&connection->controller->lock);
  message = connection->messages;
  if (message != NULL) {
    connection->messages = message->next;
    if (connection->messages == NULL)
      connection->messages_tail = NULL;
    --connection->message_count;
    connection->message_bytes -= message->bytes;
    while (read(connection->fd, &one, sizeof(one)) < 0 && errno == EINTR) {
    }
    if (message->value.type == CAI_INPUT_OBSERVATION_RESULT) {
      int slot = message->reservation;
      if (slot >= 0)
        --connection->reserved[slot];
      else
        --connection->ordinary;
    }
    *out = &message->value;
    if (!connection->controller->closed && connection->needs_snapshot)
      signal_fd(connection->controller->wake_fd);
  }
  pthread_mutex_unlock(&connection->controller->lock);
  return CAI_OK;
}
int cai_input_controller_fd(const cai_input_controller *controller) {
  return controller != NULL ? controller->fd : -1;
}
int cai_input_connection_fd(const cai_input_connection *connection) {
  return connection != NULL ? connection->fd : -1;
}
void cai_input_connection_close(cai_input_connection *connection) {
  cai_input_controller *controller;
  cai_input_connection **at;
  int closed;
  if (connection == NULL)
    return;
  controller = connection->controller;
  pthread_mutex_lock(&controller->lock);
  closed = controller->closed;
  connection->detached = 1;
  if (closed)
    for (at = &controller->connections; *at != NULL; at = &(*at)->next)
      if (*at == connection) {
        *at = connection->next;
        break;
      }
  pthread_mutex_unlock(&controller->lock);
  if (closed) {
    connection_free(connection);
    controller_release(controller);
  } else
    signal_fd(controller->wake_fd);
}
void cai_input_controller_close(cai_input_controller *controller) {
  cai_input_connection **at, *connection;
  if (owner(controller, NULL) != CAI_OK)
    return;
  cai_input_controller_select(controller, NULL, NULL);
  pthread_mutex_lock(&controller->lock);
  controller->closed = 1;
  for (connection = controller->connections; connection != NULL;
       connection = connection->next) {
    input_submission *submission;
    while ((submission = connection->head) != NULL) {
      input_message *message = message_new(CAI_INPUT_OBSERVATION_RESULT);
      connection->head = submission->next;
      if (message != NULL) {
        message->value.conversation = submission->scope.conversation;
        message->value.result.request_id = submission->value.request_id;
        message->value.result.outcome = CAI_INPUT_REJECTED;
        message->value.result.code = CAI_ERR_CANCELLED;
        strcpy(message->value.result.reason,
               "controller stopped before acceptance");
        message->reservation = reserved(submission->value.operation);
        append_message(connection, message);
      }
      cai_free_mem(NULL, submission->text);
      cai_free_mem(NULL, submission);
    }
    connection->tail = NULL;
    connection->submitted = 0U;
  }
  at = &controller->connections;
  while ((connection = *at) != NULL) {
    if (connection->detached) {
      *at = connection->next;
      connection_free(connection);
      --controller->references;
    } else
      at = &connection->next;
  }
  pthread_mutex_unlock(&controller->lock);
  controller_release(controller);
}
