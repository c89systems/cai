/** @file Reusable backend application adapter, independent of terminal editors.
 */
#define _POSIX_C_SOURCE 200809L
#include "network_host.h"
#include "input_wire.h"
#include "log.h"
#include <cai/network_tools.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>
typedef struct network_arguments {
  char address[1025], prompt[CAI_CLI_INPUT_TEXT_MAX + 1U], workspace[4096];
  char cursor[2048];
  lonejson_uint64 attachment_id;
  lonejson_int64 timeout_ms;
  int include_reply, has_timeout;
} network_arguments;
static const lonejson_field argument_fields[] = {
    LONEJSON_FIELD_STRING_FIXED(network_arguments, address, "address",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED(network_arguments, prompt, "prompt",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED(network_arguments, workspace, "workspace",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED(network_arguments, cursor, "cursor",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_U64(network_arguments, attachment_id, "attachment_id"),
    LONEJSON_FIELD_I64_PRESENT(network_arguments, timeout_ms, has_timeout,
                               "timeout_ms"),
    LONEJSON_FIELD_BOOL(network_arguments, include_reply, "include_reply")};
LONEJSON_MAP_DEFINE(argument_map, network_arguments, argument_fields);
typedef struct network_job {
  struct network_job *next;
  const char *operation;
  const char *server_instance;
  char session[129];
  network_arguments *args;
  cai_sink *sink;
  cai_error error;
  cai_app_network_event request;
  uint64_t deadline;
  int done, cancelled, rc;
} network_job;
typedef struct network_attachment {
  uint64_t id;
  unsigned int flags;
  char address[256], advertise[256];
  int publisher_sent;
  int warning_sent;
} network_attachment;
struct cai_cli_network_host {
  cai_cli_network_host_config config;
  cai_cli_network_registry *registry;
  pthread_mutex_t lock;
  pthread_cond_t changed;
  int fd, stopped;
  size_t count;
  network_job *first, *last, *active;
  uint64_t next_request;
  char warning[256];
  network_attachment attachments[CAI_ATTACHMENT_LIMIT];
};
static uint64_t now_ms(void) {
  struct timespec now;
  (void)clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}
static void wake(cai_cli_network_host *host) {
  uint64_t one = 1U;
  while (write(host->fd, &one, sizeof(one)) < 0 && errno == EINTR) {
  }
}
static int registry(cai_cli_network_host *host, cai_error *error) {
  return host->registry != NULL
             ? CAI_OK
             : cai_cli_network_registry_open(
                   host->config.pouch, host->config.options,
                   host->config.workspace, host->config.instance,
                   &host->registry, error);
}
static void warning(cai_cli_network_host *host, const cai_error *error) {
  size_t i;
  if (host->warning[0])
    return;
  snprintf(host->warning, sizeof(host->warning),
           "Workspace network service: %s",
           error != NULL && error->message != NULL
               ? error->message
               : "registration or publication failed");
  if (host->config.pouch->logger != NULL)
    cai_cli_log_error(host->config.pouch->logger, "workspace networking",
                      error);
  for (i = 0U; i < CAI_ATTACHMENT_LIMIT; i++)
    if (host->attachments[i].id) {
      cai_app_network_event notice;
      memset(&notice, 0, sizeof(notice));
      notice.header.kind = CAI_PROTOCOL_NETWORK;
      notice.header.attachment = host->attachments[i].id;
      notice.operation = CAI_APP_NETWORK_NOTICE;
      notice.reason = host->warning;
      if (host->config.send(host->config.context, notice.header.attachment,
                            &notice, NULL) == CAI_OK)
        host->attachments[i].warning_sent = 1;
    }
}
int cai_cli_network_host_open(const cai_cli_network_host_config *config,
                              cai_cli_network_host **out, cai_error *error) {
  cai_cli_network_host *host = calloc(1U, sizeof(*host));
  int rc = CAI_OK;
  *out = NULL;
  if (host == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate network host");
  host->config = *config;
  host->fd = -1;
  if (pthread_mutex_init(&host->lock, NULL) != 0) {
    free(host);
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "initialize network host lock");
  }
  if (pthread_cond_init(&host->changed, NULL) != 0) {
    pthread_mutex_destroy(&host->lock);
    free(host);
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "initialize network host condition");
  }
  host->fd = eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK);
  if (host->fd < 0)
    rc =
        cai_cli_error(error, CAI_ERR_TRANSPORT, "create network action wakeup");
  if (rc == CAI_OK && (config->options->network_tools ||
                       (config->options->event_multicast != NULL &&
                        *config->options->event_multicast)))
    rc = registry(host, error);
  if (rc != CAI_OK)
    cai_cli_network_host_close(host);
  else
    *out = host;
  return rc;
}
static network_attachment *attachment(cai_cli_network_host *host, uint64_t id,
                                      int create) {
  size_t i;
  network_attachment *empty = NULL;
  for (i = 0U; i < CAI_ATTACHMENT_LIMIT; i++) {
    if (host->attachments[i].id == id)
      return &host->attachments[i];
    if (!host->attachments[i].id && empty == NULL)
      empty = &host->attachments[i];
  }
  if (create && empty != NULL) {
    empty->id = id;
    return empty;
  }
  return NULL;
}
typedef struct receipt_output {
  cai_sink *sink;
  cai_error *error;
} receipt_output;
static lonejson_status receipt_write(void *context, const void *bytes,
                                     size_t count, lonejson_error *error) {
  receipt_output *out = context;
  (void)error;
  return cai_sink_write(out->sink, bytes, count, out->error) == CAI_OK
             ? LONEJSON_STATUS_OK
             : LONEJSON_STATUS_IO_ERROR;
}
static int receipt(network_job *job, const cai_app_network_event *event) {
  lonejson *json;
  lonejson_writer writer;
  lonejson_error error;
  lonejson_status rc;
  receipt_output out;
  lonejson_error_init(&error);
  json = lonejson_new(NULL, &error);
  if (json == NULL)
    return cai_cli_error(&job->error, CAI_ERR_NOMEM,
                         "allocate network receipt writer");
  memset(&writer, 0, sizeof(writer));
  out.sink = job->sink;
  out.error = &job->error;
  rc = lonejson_writer_init_sink(json, &writer, receipt_write, &out, &error);
#define EMIT(call)                                                             \
  do {                                                                         \
    if (rc == LONEJSON_STATUS_OK)                                              \
      rc = (call);                                                             \
  } while (0)
#define KEY(name)                                                              \
  EMIT(lonejson_writer_key(&writer, name, sizeof(name) - 1U, &error))
#define TEXT(name, value)                                                      \
  do {                                                                         \
    KEY(name);                                                                 \
    EMIT(lonejson_writer_string(&writer, value, strlen(value), &error));       \
  } while (0)
  EMIT(lonejson_writer_begin_object(&writer, &error));
  TEXT("status", event->code != CAI_OK ? "failed"
                 : !strcmp(job->operation, "prompt_listener_info")
                     ? "available"
                     : "transmitted");
  TEXT("operation", job->operation);
  TEXT("server_instance", job->server_instance);
  TEXT("session_id", job->session);
  TEXT("message",
       event->reason != NULL ? event->reason : "TCP transfer failed");
  TEXT("address", job->args->address);
  KEY("attachment_id");
  EMIT(lonejson_writer_u64(&writer, event->header.attachment, &error));
  KEY("request_id");
  EMIT(lonejson_writer_u64(&writer, event->header.request, &error));
  KEY("bytes");
  EMIT(lonejson_writer_u64(&writer, event->transferred, &error));
  KEY("uncertain");
  EMIT(lonejson_writer_bool(
      &writer, (event->flags & CAI_APP_NETWORK_UNCERTAIN) != 0U, &error));
  if (!strcmp(job->operation, "prompt_listener_info")) {
    KEY("reply_tool");
    EMIT(lonejson_writer_begin_object(&writer, &error));
    TEXT("name", "prompt_send");
    KEY("arguments");
    EMIT(lonejson_writer_begin_object(&writer, &error));
    TEXT("address", job->args->address);
    TEXT("prompt", "YOUR REPLY");
    EMIT(lonejson_writer_end_object(&writer, &error));
    EMIT(lonejson_writer_end_object(&writer, &error));
  }
  EMIT(lonejson_writer_end_object(&writer, &error));
  EMIT(lonejson_writer_finish(&writer, &error));
#undef TEXT
#undef KEY
#undef EMIT
  lonejson_writer_cleanup(&writer);
  lonejson_free(json);
  return rc == LONEJSON_STATUS_OK
             ? CAI_OK
             : cai_cli_error(&job->error, CAI_ERR_PROTOCOL, error.message);
}
static void settled(cai_cli_network_host *host, network_job *job, int rc) {
  job->rc = rc;
  job->done = 1;
  if (host->active == job)
    host->active = NULL;
  pthread_cond_broadcast(&host->changed);
}
int cai_cli_network_host_receive(cai_cli_network_host *host, uint64_t id,
                                 const cai_app_network_event *event,
                                 cai_error *error) {
  network_attachment *peer;
  cai_cli_network_scope scope;
  const cai_app_network_event *publisher;
  int rc;
  if (host == NULL)
    return CAI_OK;
  if (event->header.attachment != id)
    return cai_cli_error(error, CAI_ERR_PROTOCOL,
                         "network attachment identity mismatch");
  memset(&scope, 0, sizeof(scope));
  rc = host->config.scope(host->config.context, &scope, error);
  if (rc != CAI_OK)
    return rc;
  if (event->operation == CAI_APP_NETWORK_NOTICE) {
    if (host->config.pouch->logger != NULL && event->reason != NULL)
      cai_cli_log_message(host->config.pouch->logger, PSLOG_LEVEL_WARN,
                          "network_notice", "diagnostic", event->reason,
                          strlen(event->reason), NULL, 0U);
    return CAI_OK;
  }
  if (event->operation == CAI_APP_NETWORK_CAPABILITY) {
    peer = attachment(host, id, 1);
    if (peer == NULL || strlen(event->address) >= 256U ||
        strlen(event->advertise) >= 256U)
      return cai_cli_error(error, CAI_ERR_LIMIT,
                           "network attachment metadata limit");
    peer->flags = event->flags;
    strcpy(peer->address, event->address);
    strcpy(peer->advertise, event->advertise);
    if (event->header.generation != scope.generation ||
        event->header.conversation != scope.epoch)
      peer->flags &= ~CAI_APP_NETWORK_AVAILABLE;
    if (peer->address[0] && (rc = registry(host, error)) != CAI_OK) {
      warning(host, error);
      cai_error_cleanup(error);
      return CAI_OK;
    }
    {
      cai_app_network_event current = *event;
      current.flags = peer->flags;
      rc = cai_cli_network_registry_listener(host->registry, id, &current,
                                             scope.session,
                                             id == scope.controller, error);
    }
    if (rc != CAI_OK) {
      warning(host, error);
      cai_error_cleanup(error);
      rc = CAI_OK;
    }
    publisher = cai_cli_network_registry_publisher(host->registry);
    if (rc == CAI_OK && !peer->publisher_sent && publisher != NULL) {
      cai_app_network_event notice = *publisher;
      notice.header.attachment = id;
      notice.header.generation = scope.generation;
      notice.header.conversation = scope.epoch;
      rc = host->config.send(host->config.context, id, &notice, error);
      if (rc == CAI_OK)
        peer->publisher_sent = 1;
    }
    if (rc == CAI_OK && host->warning[0] && !peer->warning_sent) {
      cai_app_network_event notice;
      memset(&notice, 0, sizeof(notice));
      notice.header.kind = CAI_PROTOCOL_NETWORK;
      notice.header.attachment = id;
      notice.operation = CAI_APP_NETWORK_NOTICE;
      notice.reason = host->warning;
      rc = host->config.send(host->config.context, id, &notice, error);
      if (rc == CAI_OK)
        peer->warning_sent = 1;
    }
    return rc;
  }
  if (event->operation != CAI_APP_NETWORK_RECEIPT)
    return cai_cli_error(error, CAI_ERR_PROTOCOL,
                         "unexpected frontend network operation");
  pthread_mutex_lock(&host->lock);
  if (host->active != NULL &&
      host->active->request.header.request == event->header.request &&
      host->active->request.header.attachment == id &&
      host->active->request.header.generation == event->header.generation &&
      host->active->request.header.conversation == event->header.conversation) {
    network_job *job = host->active;
    cai_app_network_event checked = *event;
    int result;
    if (scope.controller != id ||
        scope.generation != event->header.generation ||
        scope.epoch != event->header.conversation) {
      checked.code = CAI_ERR_CANCELLED;
      checked.flags |= CAI_APP_NETWORK_UNCERTAIN;
      checked.reason = "TCP receipt arrived after attachment authority "
                       "changed; delivery is uncertain.";
    }
    result = receipt(job, &checked);
    settled(host, job, result);
  }
  pthread_mutex_unlock(&host->lock);
  return CAI_OK;
}
void cai_cli_network_host_detach(cai_cli_network_host *host, uint64_t id) {
  network_attachment *peer;
  if (host == NULL)
    return;
  peer = attachment(host, id, 0);
  if (peer != NULL)
    memset(peer, 0, sizeof(*peer));
  cai_cli_network_registry_remove(host->registry, id);
}
int cai_cli_network_host_fd(cai_cli_network_host *host) {
  return host != NULL ? host->fd : -1;
}
uint64_t cai_cli_network_host_deadline(cai_cli_network_host *host) {
  uint64_t deadline;
  if (host == NULL)
    return 0U;
  deadline = cai_cli_network_registry_deadline(host->registry);
  pthread_mutex_lock(&host->lock);
  if (host->active != NULL && (!deadline || host->active->deadline < deadline))
    deadline = host->active->deadline;
  pthread_mutex_unlock(&host->lock);
  return deadline;
}
static int listener_info(cai_cli_network_host *host, network_job *job,
                         network_attachment *peer,
                         const cai_cli_network_scope *scope) {
  const char *address = peer->advertise[0] ? peer->advertise : peer->address;
  int rc;
  if (peer->id != scope->controller ||
      !(peer->flags & CAI_APP_NETWORK_AVAILABLE))
    return cai_cli_error(&job->error, CAI_ERR_INVALID,
                         "selected listener cannot currently admit input; it "
                         "must be a ready controller");
  if (!*address || (!peer->advertise[0] && (!strncmp(address, "0.0.0.0:", 8U) ||
                                            !strncmp(address, "[::]:", 5U))))
    return cai_cli_error(&job->error, CAI_ERR_INVALID,
                         "selected attachment has no usable reply endpoint");
  /* A mapped row keeps endpoint strings escaped; no shell command is executed.
   */
  {
    cai_app_network_event info;
    memset(&info, 0, sizeof(info));
    info.header.attachment = peer->id;
    info.reason =
        "Reply with prompt_send using this address and the desired prompt. "
        "Endpoint targets the attachment's currently active session.";
    snprintf(job->args->address, sizeof(job->args->address), "%s", address);
    rc = receipt(job, &info);
  }
  (void)host;
  (void)scope;
  return rc;
}
typedef struct footer_buffer {
  char *text;
  size_t used;
} footer_buffer;
static lonejson_status footer_write(void *context, const void *bytes,
                                    size_t count, lonejson_error *error) {
  footer_buffer *buffer = context;
  (void)error;
  if (count > CAI_CLI_INPUT_TEXT_MAX - buffer->used)
    return LONEJSON_STATUS_IO_ERROR;
  memcpy(buffer->text + buffer->used, bytes, count);
  buffer->used += count;
  buffer->text[buffer->used] = '\0';
  return LONEJSON_STATUS_OK;
}
static int reply_footer(network_job *job, const network_attachment *peer) {
  const char *address = peer->advertise[0] ? peer->advertise : peer->address;
  static const char prefix[] = "\n\nReply requested: use prompt_send with "
                               "these arguments, replacing YOUR REPLY: ";
  footer_buffer buffer;
  lonejson *json;
  lonejson_writer writer;
  lonejson_error error;
  lonejson_status status;
  if (!*address || (!peer->advertise[0] && (!strncmp(address, "0.0.0.0:", 8U) ||
                                            !strncmp(address, "[::]:", 5U))))
    return cai_cli_error(&job->error, CAI_ERR_INVALID,
                         "include_reply requires this sending attachment's "
                         "usable prompt listener");
  buffer.text = job->args->prompt;
  buffer.used = strlen(buffer.text);
  lonejson_error_init(&error);
  json = lonejson_new(NULL, &error);
  if (json == NULL)
    return cai_cli_error(&job->error, CAI_ERR_NOMEM,
                         "allocate reply instruction writer");
  memset(&writer, 0, sizeof(writer));
  status = footer_write(&buffer, prefix, sizeof(prefix) - 1U, &error);
#define EMIT(call)                                                             \
  do {                                                                         \
    if (status == LONEJSON_STATUS_OK)                                          \
      status = (call);                                                         \
  } while (0)
  EMIT(lonejson_writer_init_sink(json, &writer, footer_write, &buffer, &error));
  EMIT(lonejson_writer_begin_object(&writer, &error));
  EMIT(lonejson_writer_key(&writer, "address", 7U, &error));
  EMIT(lonejson_writer_string(&writer, address, strlen(address), &error));
  EMIT(lonejson_writer_key(&writer, "prompt", 6U, &error));
  EMIT(lonejson_writer_string(&writer, "YOUR REPLY", 10U, &error));
  EMIT(lonejson_writer_end_object(&writer, &error));
  EMIT(lonejson_writer_finish(&writer, &error));
#undef EMIT
  lonejson_writer_cleanup(&writer);
  lonejson_free(json);
  return status == LONEJSON_STATUS_OK
             ? CAI_OK
             : cai_cli_error(
                   &job->error, CAI_ERR_LIMIT,
                   "prompt plus reply instructions exceeds prompt size limit");
}
static int run_job(cai_cli_network_host *host, network_job *job,
                   const cai_cli_network_scope *scope) {
  uint64_t id =
      job->args->attachment_id ? job->args->attachment_id : scope->controller;
  network_attachment *peer = attachment(host, id, 0);
  int rc;
  snprintf(job->session, sizeof(job->session), "%s", scope->session);
  if (!strcmp(job->operation, "network_list")) {
    rc = registry(host, &job->error);
    if (rc == CAI_OK)
      rc = cai_cli_network_registry_list(host->registry, job->args->workspace,
                                         job->args->cursor, job->sink,
                                         &job->error);
    return rc;
  }
  if (peer == NULL || !id)
    return cai_cli_error(&job->error, CAI_ERR_INVALID,
                         "select an available frontend attachment; no "
                         "unambiguous controller exists");
  if (!strcmp(job->operation, "prompt_listener_info"))
    return listener_info(host, job, peer, scope);
  if (id != scope->controller || !(peer->flags & CAI_APP_NETWORK_CAN_SEND) ||
      host->active != NULL)
    return cai_cli_error(
        &job->error, CAI_ERR_INVALID,
        "selected frontend cannot send: observer, unsupported or busy");
  if (!job->args->address[0] || !job->args->prompt[0] ||
      job->args->timeout_ms < 1 || job->args->timeout_ms > 60000)
    return cai_cli_error(
        &job->error, CAI_ERR_INVALID,
        "prompt_send needs address, nonempty prompt and timeout_ms 1..60000");
  if (job->args->include_reply && (rc = reply_footer(job, peer)) != CAI_OK)
    return rc;
  memset(&job->request, 0, sizeof(job->request));
  if (host->next_request == UINT64_MAX)
    return cai_cli_error(&job->error, CAI_ERR_LIMIT,
                         "frontend network request identities exhausted");
  job->request.operation = CAI_APP_NETWORK_SEND;
  job->request.header.kind = CAI_PROTOCOL_NETWORK;
  job->request.header.request = ++host->next_request;
  job->request.header.attachment = id;
  job->request.header.generation = scope->generation;
  job->request.header.conversation = scope->epoch;
  job->request.address = job->args->address;
  job->request.prompt = job->args->prompt;
  job->request.timeout_ms = (uint64_t)job->args->timeout_ms;
  job->deadline = now_ms() + job->request.timeout_ms + 250U;
  rc = host->config.send(host->config.context, id, &job->request, &job->error);
  if (rc == CAI_OK)
    host->active = job;
  return rc;
}
int cai_cli_network_host_pump(cai_cli_network_host *host, cai_error *error) {
  uint64_t signals;
  cai_cli_network_scope scope;
  int rc;
  if (host == NULL)
    return CAI_OK;
  while (read(host->fd, &signals, sizeof(signals)) < 0 && errno == EINTR) {
  }
  memset(&scope, 0, sizeof(scope));
  rc = host->config.scope(host->config.context, &scope, error);
  if (rc != CAI_OK)
    return rc;
  rc = cai_cli_network_registry_renew(host->registry, error);
  if (rc == CAI_OK)
    rc = cai_cli_network_registry_session(host->registry, scope.session, error);
  if (rc != CAI_OK) {
    warning(host, error);
    cai_error_cleanup(error);
  }
  pthread_mutex_lock(&host->lock);
  if (host->active != NULL) {
    network_job *job = host->active;
    if (job->cancelled || job->deadline <= now_ms() ||
        scope.controller != job->request.header.attachment ||
        scope.generation != job->request.header.generation ||
        scope.epoch != job->request.header.conversation ||
        attachment(host, scope.controller, 0) == NULL) {
      cai_app_network_event cancel = job->request;
      cancel.operation = CAI_APP_NETWORK_CANCEL;
      cancel.prompt = NULL;
      (void)host->config.send(host->config.context, cancel.header.attachment,
                              &cancel, NULL);
      cancel.code = CAI_ERR_CANCELLED;
      cancel.flags = CAI_APP_NETWORK_UNCERTAIN;
      cancel.reason =
          "TCP action cancelled, authority changed or receipt deadline "
          "elapsed; delivery is uncertain. Do not retry automatically.";
      rc = receipt(job, &cancel);
      settled(host, job, rc);
    }
  }
  while (host->first != NULL) {
    network_job *job = host->first;
    host->first = job->next;
    if (host->first == NULL)
      host->last = NULL;
    host->count--;
    rc = job->cancelled
             ? cai_cli_error(&job->error, CAI_ERR_CANCELLED,
                             "network request cancelled before dispatch")
             : run_job(host, job, &scope);
    if (host->active != job)
      settled(host, job, rc);
  }
  pthread_mutex_unlock(&host->lock);
  return CAI_OK;
}
int cai_cli_network_host_call(void *context, const char *operation,
                              const char *arguments, cai_sink *sink,
                              cai_error *error) {
  cai_cli_network_host *host = context;
  network_job *job;
  lonejson *json;
  lonejson_error local;
  int rc;
  if (host == NULL || !host->config.options->network_tools)
    return cai_cli_error(error, CAI_ERR_INVALID, "network tools are disabled");
  job = calloc(1U, sizeof(*job));
  if (job == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate network request");
  job->args = calloc(1U, sizeof(*job->args));
  if (job->args == NULL) {
    free(job);
    return cai_cli_error(error, CAI_ERR_NOMEM,
                         "allocate bounded network input");
  }
  job->operation = operation;
  job->server_instance = host->config.instance;
  job->sink = sink;
  job->args->timeout_ms = 10000;
  lonejson_error_init(&local);
  json = lonejson_new(NULL, &local);
  rc =
      json != NULL && arguments != NULL &&
              lonejson_parse_buffer(json, &argument_map, job->args, arguments,
                                    strlen(arguments),
                                    &local) == LONEJSON_STATUS_OK
          ? CAI_OK
          : cai_cli_error(error, CAI_ERR_INVALID, "invalid network tool input");
  lonejson_free(json);
  if (rc == CAI_OK && !job->args->has_timeout)
    job->args->timeout_ms = 10000;
  if (rc == CAI_OK) {
    pthread_mutex_lock(&host->lock);
    if (host->stopped || host->count >= 32U)
      rc = cai_cli_error(error, CAI_ERR_INVALID,
                         "network host is stopped or busy");
    else {
      if (host->last != NULL)
        host->last->next = job;
      else
        host->first = job;
      host->last = job;
      host->count++;
      wake(host);
      while (!job->done) {
        struct timespec until;
        (void)clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += 50000000L;
        if (until.tv_nsec >= 1000000000L) {
          until.tv_sec++;
          until.tv_nsec -= 1000000000L;
        }
        (void)pthread_cond_timedwait(&host->changed, &host->lock, &until);
        if (cai_network_tool_cancel_requested()) {
          job->cancelled = 1;
          wake(host);
        }
      }
      rc = job->rc;
      if (rc != CAI_OK)
        cai_cli_error(error, rc, job->error.message);
    }
    pthread_mutex_unlock(&host->lock);
  }
  cai_error_cleanup(&job->error);
  free(job->args);
  free(job);
  return rc;
}
void cai_cli_network_host_cancel(cai_cli_network_host *host) {
  if (host == NULL)
    return;
  pthread_mutex_lock(&host->lock);
  if (host->active != NULL) {
    network_job *job = host->active;
    cai_app_network_event cancel = job->request;
    cancel.operation = CAI_APP_NETWORK_CANCEL;
    cancel.prompt = NULL;
    (void)host->config.send(host->config.context, cancel.header.attachment,
                            &cancel, NULL);
    cancel.code = CAI_ERR_CANCELLED;
    cancel.flags = CAI_APP_NETWORK_UNCERTAIN;
    cancel.reason = "Network host stopped; TCP delivery is uncertain. Do not "
                    "retry automatically.";
    settled(host, job, receipt(job, &cancel));
  }
  while (host->first != NULL) {
    network_job *job = host->first;
    host->first = job->next;
    settled(host, job,
            cai_cli_error(&job->error, CAI_ERR_CANCELLED,
                          "network host stopped before dispatch"));
  }
  host->last = NULL;
  host->count = 0U;
  pthread_mutex_unlock(&host->lock);
}
void cai_cli_network_host_stop(cai_cli_network_host *host) {
  if (host == NULL)
    return;
  pthread_mutex_lock(&host->lock);
  host->stopped = 1;
  pthread_mutex_unlock(&host->lock);
  cai_cli_network_host_cancel(host);
}
void cai_cli_network_host_close(cai_cli_network_host *host) {
  if (host == NULL)
    return;
  cai_cli_network_host_stop(host);
  cai_cli_network_registry_close(host->registry);
  if (host->fd >= 0)
    close(host->fd);
  pthread_cond_destroy(&host->changed);
  pthread_mutex_destroy(&host->lock);
  free(host);
}
