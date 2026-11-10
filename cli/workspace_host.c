#define _POSIX_C_SOURCE 200809L
#include "workspace_host.h"
#include "catalog_wire.h"
#include "commands.h"
#include "identity.h"
#include "input_wire.h"
#include "log.h"
#include "pouch_stream.h"
#include "status_wire.h"
#include "transcript_feed.h"
#include <cai/workspace_server.h>
#include <limits.h>
#include <openssl/sha.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct host_peer {
  cai_cli_workspace_host *host;
  uint64_t attachment;
  cai_cli_input_wire_decoder *decoder;
  cai_cli_transcript_decoder *transcript_decoder;
  cai_input_connection *input;
  unsigned long long binding_request, snapshot_request;
  uint64_t stream, stream_high, status_revision;
  uint64_t prompt_stream, prompt_request, prompt_generation,
      prompt_conversation;
  unsigned char prompt_digest[SHA256_DIGEST_LENGTH];
  int prompt_code;
  unsigned long long prompt_position;
  char prompt_reason[192];
  int detached, binding, managed_delivery;
  uint64_t cancel_request, cancel_generation, cancel_conversation;
  int cancel_code;
  char cancel_reason[192];
  int selecting;
  uint64_t selection_request, selection_generation;
  int selection_managed;
  char selection[129];
  cai_agent_runtime *shell_runtime;
  uint64_t shell_id, shell_sequence, shell_high, shell_generation,
      shell_conversation, shell_started;
  uint64_t catalog_revision, catalog_index;
  int catalog_phase;
} host_peer;
struct cai_cli_workspace_host {
  cai_workspace_server *server;
  cai_app_bridge *bridge;
  pslog_logger *logger;
  cai_input_controller_config admission;
  cai_input_controller *input;
  cai_cli_transcript_publisher *history;
  cai_cli_pouch *pouch;
  unsigned int prompt_limit;
  cai_agent_runtime *catalog_runtime;
  uint64_t catalog_revision;
  char session[129];
  uint64_t generation, controller;
  cai_app_view status;
  int (*cancel)(void *, cai_error *);
  void *cancel_context;
  int (*select)(void *, const char *, cai_error *);
  void *select_context;
  host_peer peers[CAI_ATTACHMENT_LIMIT];
};
static int output_begin(void *context, const cai_protocol_header *header,
                        uint32_t size, cai_error *error) {
  host_peer *peer = context;
  return cai_workspace_server_begin(peer->host->server, peer->attachment,
                                    header, size, error);
}
static int output_write(void *context, const cai_cbor_token *token,
                        cai_error *error) {
  host_peer *peer = context;
  return cai_workspace_server_write(peer->host->server, peer->attachment, token,
                                    error);
}
static int output_end(void *context, cai_error *error) {
  host_peer *peer = context;
  return cai_workspace_server_end(peer->host->server, peer->attachment, error);
}
static int send_message(host_peer *peer, const cai_cli_input_wire_message *m,
                        cai_error *error) {
  cai_cli_input_wire_output output;
  output.context = peer;
  output.begin = output_begin;
  output.write = output_write;
  output.end = output_end;
  return cai_cli_input_wire_write(&output, m, error);
}
static int send_status(host_peer *peer, uint64_t now, cai_error *error) {
  cai_cli_input_wire_output output;
  cai_app_view view = peer->host->status;
  output.context = peer;
  output.begin = output_begin;
  output.write = output_write;
  output.end = output_end;
  cai_app_view_elapsed(&view, now, &view.metrics.turn_elapsed_ms,
                       &view.goal.elapsed_seconds);
  return cai_cli_status_write(&output, &view, error);
}

/** The runtime's immutable skills catalog has at most 256 entries. Visiting
 * one indexed row with an early stop keeps socket backpressure bounded without
 * copying that catalog or adding another public runtime/catalog API. */
typedef struct catalog_row {
  host_peer *peer;
  size_t skip;
  int found, rc;
} catalog_row;
static int send_catalog(host_peer *peer, cai_app_catalog_phase phase,
                        const char *text, int code, cai_error *error) {
  cai_cli_input_wire_output output;
  cai_app_catalog_event e;
  unsigned long long epoch;
  int rc = cai_input_controller_conversation(peer->host->input, &epoch, error);
  if (rc != CAI_OK)
    return rc;
  memset(&e, 0, sizeof(e));
  e.phase = phase;
  e.conversation = epoch;
  e.revision = peer->catalog_revision;
  e.index = peer->catalog_index;
  e.text = text;
  e.code = code;
  output.context = peer;
  output.begin = output_begin;
  output.write = output_write;
  output.end = output_end;
  return cai_cli_catalog_write(&output, &e, error);
}
static int catalog_item(void *context, const cai_skill_info *skill,
                        cai_error *error) {
  catalog_row *row = context;
  if (row->skip != 0U) {
    --row->skip;
    return CAI_OK;
  }
  row->found = 1;
  row->rc =
      send_catalog(row->peer, CAI_APP_CATALOG_ITEM, skill->name, CAI_OK, error);
  return row->rc == CAI_OK ? CAI_ERR_CANCELLED : row->rc;
}
static int catalog_pending(const host_peer *peer) {
  return peer->host->catalog_runtime != NULL &&
         (peer->catalog_revision != peer->host->catalog_revision ||
          peer->catalog_phase != CAI_APP_CATALOG_END);
}
static int output_catalog(host_peer *peer, cai_error *error) {
  catalog_row row;
  int rc;
  if (peer->catalog_revision != peer->host->catalog_revision) {
    peer->catalog_revision = peer->host->catalog_revision;
    peer->catalog_index = 0U;
    peer->catalog_phase = CAI_APP_CATALOG_ITEM;
    return send_catalog(peer, CAI_APP_CATALOG_BEGIN, "", CAI_OK, error);
  }
  memset(&row, 0, sizeof(row));
  row.peer = peer;
  row.skip = (size_t)peer->catalog_index;
  rc = cai_agent_runtime_visit_skills(peer->host->catalog_runtime, catalog_item,
                                      &row, error);
  if (row.found) {
    if (row.rc == CAI_OK && rc == CAI_ERR_CANCELLED) {
      ++peer->catalog_index;
      return CAI_OK;
    }
    return row.rc != CAI_OK ? row.rc : rc;
  }
  if (rc != CAI_OK)
    return rc;
  peer->catalog_phase = CAI_APP_CATALOG_END;
  return send_catalog(peer, CAI_APP_CATALOG_END, "", CAI_OK, error);
}
int cai_cli_workspace_host_catalog(cai_cli_workspace_host *host,
                                   cai_agent_runtime *runtime,
                                   cai_error *error) {
  if (host == NULL)
    return CAI_OK;
  if (runtime == NULL || host->input == NULL ||
      host->catalog_revision == UINT64_MAX)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "open catalog context required");
  host->catalog_runtime = runtime;
  ++host->catalog_revision;
  return CAI_OK;
}
static int send_transcript(host_peer *peer, const cai_transcript_event *event,
                           cai_error *error) {
  static const cai_protocol_kind kinds[] = {
      CAI_PROTOCOL_REPLAY_BEGIN,   CAI_PROTOCOL_STREAM_BEGIN,
      CAI_PROTOCOL_STREAM_CHUNK,   CAI_PROTOCOL_STREAM_END,
      CAI_PROTOCOL_REPLAY_END,     CAI_PROTOCOL_SUBSCRIPTION_ERROR,
      CAI_PROTOCOL_SHELL_OUTPUT,   CAI_PROTOCOL_SHELL_OUTPUT_END,
      CAI_PROTOCOL_TERMINAL_OUTPUT};
  cai_cli_transcript_message m;
  cai_cli_input_wire_output output;
  memset(&m, 0, sizeof(m));
  m.event = *event;
  m.header.kind = kinds[event->kind];
  m.header.request = m.header.stream = event->stream;
  m.header.conversation = event->conversation;
  if ((event->kind >= CAI_TRANSCRIPT_EVENT_BEGIN &&
       event->kind <= CAI_TRANSCRIPT_EVENT_END) ||
      event->kind == CAI_TRANSCRIPT_SHELL_OUTPUT ||
      event->kind == CAI_TRANSCRIPT_SHELL_OUTPUT_END ||
      event->kind == CAI_TRANSCRIPT_TERMINAL_OUTPUT)
    m.header.sequence = event->sequence;
  output.context = peer;
  output.begin = output_begin;
  output.write = output_write;
  output.end = output_end;
  return cai_cli_transcript_write(&output, &m, error);
}
static int transcript_error(host_peer *peer, uint64_t stream,
                            cai_transcript_failure failure, int code,
                            const char *reason, cai_error *error) {
  cai_transcript_event e;
  memset(&e, 0, sizeof(e));
  e.kind = CAI_TRANSCRIPT_ERROR;
  e.stream = stream;
  e.failure = failure;
  e.code = code;
  e.reason = reason;
  return send_transcript(peer, &e, error);
}
static int transcript_request(host_peer *peer, const cai_protocol_event *event,
                              cai_error *error) {
  const cai_cli_transcript_message *m = NULL;
  unsigned long long epoch;
  cai_error local;
  int rc;
  if (peer->transcript_decoder == NULL) {
    rc = cai_cli_transcript_decoder_open(&peer->transcript_decoder, error);
    if (rc != CAI_OK)
      return rc;
  }
  rc = cai_cli_transcript_decode(peer->transcript_decoder, event, &m, error);
  if (rc != CAI_OK || m == NULL)
    return rc;
  if (m->header.kind == CAI_PROTOCOL_UNSUBSCRIBE) {
    if (m->header.stream == peer->stream)
      cai_cli_transcript_feed_cancel(peer->host->history, peer->attachment,
                                     peer->stream);
    else
      return transcript_error(peer, m->header.stream,
                              CAI_TRANSCRIPT_CONTEXT_CLOSED, CAI_ERR_CANCELLED,
                              "Transcript subscription is not active", error);
    return CAI_OK;
  }
  if (m->header.kind == CAI_PROTOCOL_HISTORY_GET) {
    cai_error_init(&local);
    rc = peer->host->history != NULL && m->event.session != NULL &&
                 strcmp(m->event.session, peer->host->session) == 0 &&
                 m->header.request > peer->stream_high &&
                 m->event.position <= UINT_MAX && m->event.sequence == 0U &&
                 (m->event.collection == NULL || m->event.collection[0] == '\0')
             ? cai_input_controller_conversation(peer->host->input, &epoch,
                                                 &local)
             : cai_cli_error(
                   &local, CAI_ERR_INVALID,
                   "active session and fresh history request required");
    if (rc == CAI_OK && m->header.conversation != 0U &&
        m->header.conversation != epoch)
      rc = cai_cli_error(&local, CAI_ERR_INVALID,
                         "prompt-history session changed");
    if (m->header.request > peer->stream_high)
      peer->stream_high = m->header.request;
    if (rc == CAI_OK)
      rc = cai_cli_transcript_feed_prompts(
          peer->host->history, peer->attachment, m->header.request,
          (unsigned int)m->event.position, &local);
    if (rc == CAI_OK)
      peer->prompt_stream = m->header.request;
    else
      rc = transcript_error(
          peer, m->header.request, CAI_TRANSCRIPT_FAILED, rc,
          local.message != NULL ? local.message : "prompt-history unavailable",
          error);
    cai_error_cleanup(&local);
    return rc;
  }
  if (m->header.request <= peer->stream_high)
    return transcript_error(peer, m->header.request, CAI_TRANSCRIPT_FAILED,
                            CAI_ERR_INVALID, "Fresh subscription ID required",
                            error);
  cai_error_init(&local);
  if (peer->stream != 0U)
    cai_cli_transcript_feed_cancel(peer->host->history, peer->attachment,
                                   peer->stream);
  peer->stream = 0U;
  rc =
      peer->host->history != NULL && m->event.session != NULL &&
              strcmp(m->event.session, peer->host->session) == 0
          ? cai_input_controller_conversation(peer->host->input, &epoch, &local)
          : cai_cli_error(&local, CAI_ERR_INVALID,
                          "Active session and fresh subscription ID required");
  if (m->header.request > peer->stream_high)
    peer->stream_high = m->header.request;
  if (rc == CAI_OK && m->header.conversation != 0U &&
      m->header.conversation != epoch)
    rc = cai_cli_error(&local, CAI_ERR_INVALID, "Transcript session changed");
  if (rc == CAI_OK)
    rc = cai_cli_transcript_feed_request(peer->host->history, peer->attachment,
                                         m->header.request, m->event.collection,
                                         m->event.position, m->event.sequence,
                                         m->replay_turns, &local);
  if (rc == CAI_OK)
    peer->stream = peer->stream_high = m->header.request;
  else
    rc = transcript_error(peer, m->header.request, CAI_TRANSCRIPT_FAILED, rc,
                          local.message != NULL ? local.message
                                                : "Transcript request failed",
                          error);
  cai_error_cleanup(&local);
  return rc;
}
static int clock_ms(uint64_t *out, cai_error *error) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    (void)cai_cli_error(error, CAI_ERR_TRANSPORT, "read workspace clock");
    return CAI_ERR_TRANSPORT;
  }
  *out = (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
  return CAI_OK;
}
static int publish(cai_cli_workspace_host *host, cai_error *error) {
  cai_attachment_state snapshot;
  int rc = cai_workspace_server_get_state(host->server, &snapshot, error);
  if (rc == CAI_OK)
    cai_app_bridge_server(host->bridge, &snapshot,
                          cai_workspace_server_instance(host->server));
  return rc;
}
int cai_cli_workspace_host_status(cai_cli_workspace_host *host,
                                  const cai_app_view *source,
                                  cai_error *error) {
  cai_app_view view;
  uint64_t now;
  int rc;
  if (host == NULL)
    return CAI_OK;
  if (source == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID, "status source required");
  view = *source;
  rc = cai_cli_status_representable(&view);
  if (rc != CAI_OK) {
    memset(&view, 0, sizeof(view));
    view.conversation = source->conversation;
    view.code = rc;
    strcpy(view.reason, "Status metadata exceeds the wire profile");
  }
  if (host->status.revision != 0U && cai_cli_status_equal(&host->status, &view))
    return CAI_OK;
  if (host->status.revision == UINT64_MAX)
    return cai_cli_error(error, CAI_ERR_LIMIT, "status revision exhausted");
  rc = clock_ms(&now, error);
  if (rc != CAI_OK)
    return rc;
  view.received_ms = now;
  view.revision = host->status.revision + 1U;
  return cai_cli_status_copy(&host->status, &view, error);
}
/** Observer permissions apply to a current request and its trusted existing
 * target. Editing a read-only item cannot turn it into a model submission or
 * reach another producer's work. Global queue transitions remain privileged. */
static int observer_request(const cai_input_admission *a) {
  const cai_input_request *r = a->request;
  if (r == NULL)
    return 1; /* This producer's delivery availability, never queue mutation. */
  if (r->origin[0] != '\0')
    return 0;
  if (r->operation == CAI_INPUT_OFFER)
    return !r->enabled && cai_cli_command_observer(r->text);
  if (a->entry == NULL || a->entry->client_id != a->producer ||
      a->entry->origin[0] != '\0' || !cai_cli_command_observer(a->entry->text))
    return 0;
  switch (r->operation) {
  case CAI_INPUT_END_EDIT:
    return cai_cli_command_observer(r->text);
  case CAI_INPUT_BEGIN_EDIT:
  case CAI_INPUT_ABORT_EDIT:
  case CAI_INPUT_WITHDRAW:
  case CAI_INPUT_SET_INTENT:
    return 1;
  default:
    return 0;
  }
}
static int authorize(void *context, const cai_input_admission *a,
                     cai_error *error) {
  cai_cli_workspace_host *host = context;
  cai_attachment_state state;
  size_t i;
  int rc = cai_workspace_server_get_state(host->server, &state, error);
  if (rc != CAI_OK)
    return rc;
  for (i = 0U; i < CAI_ATTACHMENT_LIMIT; ++i)
    if (host->peers[i].attachment == a->attachment && !host->peers[i].detached)
      break;
  if (a->attachment == 0U || i == CAI_ATTACHMENT_LIMIT ||
      state.phase != CAI_ATTACHMENT_READY || a->generation != state.generation)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "attachment is stale or stopping");
  if (state.controller != a->attachment && !observer_request(a))
    return cai_cli_error(
        error, CAI_ERR_INVALID,
        "attachment is observing; use /takeover to change shared state");
  if (rc == CAI_OK && host->admission.authorize != NULL)
    rc = host->admission.authorize(host->admission.authorize_context, a, error);
  return rc;
}
/** Non-queue controls never acquire observer exemptions from command text. */
static int authorize_control(cai_cli_workspace_host *host, uint64_t attachment,
                             uint64_t generation, cai_error *error) {
  cai_input_admission a;
  int rc = cai_workspace_server_authorize(host->server, attachment, generation,
                                          error);
  if (rc == CAI_OK && host->admission.authorize != NULL) {
    memset(&a, 0, sizeof(a));
    a.attachment = attachment;
    a.generation = generation;
    rc =
        host->admission.authorize(host->admission.authorize_context, &a, error);
  }
  return rc;
}
/** A recording belongs to this attachment/context, never to a replacement
 * controller. Preserve its bounded tail on abandonment so it cannot hold
 * newly submitted turns indefinitely. The runtime is still alive at unbind. */
static int shell_abandon(host_peer *peer, cai_error *error) {
  uint64_t now;
  int rc;
  if (peer->shell_runtime == NULL)
    return CAI_OK;
  rc = clock_ms(&now, error);
  if (rc == CAI_OK)
    rc = cai_agent_runtime_user_shell_finish(peer->shell_runtime, 129,
                                             now - peer->shell_started, error);
  if (rc == CAI_OK) {
    peer->shell_runtime = NULL;
    peer->shell_id = peer->shell_sequence = 0U;
  }
  return rc;
}
/** Each raw chunk is acknowledged after owner-thread capture. The sender
 * waits before reading another PTY chunk: bounded end-to-end backpressure,
 * with no second output journal and no raw bytes retained by the transport. */
static int shell_request(host_peer *peer, const cai_cli_input_wire_message *m,
                         cai_error *error) {
  const cai_app_shell_request *s = &m->shell;
  cai_cli_workspace_host *host = peer->host;
  cai_cli_input_wire_message reply;
  cai_agent_runtime *recording_runtime = peer->shell_runtime;
  cai_error local;
  unsigned long long epoch = 0U;
  unsigned long long first = 0U, last = 0U;
  uint64_t now = 0U;
  int rc;
  cai_error_init(&local);
  rc = authorize_control(host, peer->attachment, m->header.generation, &local);
  if (rc == CAI_OK)
    rc = host->input != NULL && peer->input != NULL && !peer->binding
             ? cai_input_controller_conversation(host->input, &epoch, &local)
             : cai_cli_error(&local, CAI_ERR_INVALID,
                             "bound shell recording context required");
  if (rc == CAI_OK && (epoch != m->header.conversation ||
                       m->header.request <= peer->shell_high))
    rc = cai_cli_error(&local, CAI_ERR_INVALID,
                       "shell recording context or request changed");
  if (rc == CAI_OK) {
    peer->shell_high = m->header.request;
    if (s->operation == CAI_APP_SHELL_BEGIN) {
      if (peer->shell_runtime != NULL || host->catalog_runtime == NULL ||
          s->recording != 0U || s->sequence != 0U || s->length == 0U ||
          s->length > 8192U || memchr(s->data, '\0', s->length) != NULL)
        rc = cai_cli_error(&local, CAI_ERR_INVALID,
                           "a new bounded shell command is required");
      else {
        rc = clock_ms(&now, &local);
        if (rc == CAI_OK)
          rc = cai_agent_runtime_user_shell_begin(host->catalog_runtime,
                                                  s->data, 1, &local);
        if (rc == CAI_OK) {
          peer->shell_runtime = host->catalog_runtime;
          recording_runtime = peer->shell_runtime;
          peer->shell_id = m->header.request;
          peer->shell_sequence = 0U;
          peer->shell_generation = m->header.generation;
          peer->shell_conversation = epoch;
          peer->shell_started = now;
        }
      }
    } else if (peer->shell_runtime == NULL || s->recording != peer->shell_id ||
               peer->shell_generation != m->header.generation ||
               peer->shell_conversation != epoch ||
               peer->shell_sequence == UINT64_MAX ||
               s->sequence != peer->shell_sequence + 1U ||
               (s->operation == CAI_APP_SHELL_WRITE ? s->length > 4096U
                                                    : s->length != 0U))
      rc = cai_cli_error(&local, CAI_ERR_INVALID,
                         "shell recording identity or sequence changed");
    else {
      rc = s->operation == CAI_APP_SHELL_WRITE
               ? cai_agent_runtime_user_shell_write(peer->shell_runtime,
                                                    s->data, s->length, &local)
               : cai_agent_runtime_user_shell_finish(peer->shell_runtime,
                                                     (int)s->exit_code,
                                                     s->duration_ms, &local);
      if (rc == CAI_OK) {
        peer->shell_sequence = s->sequence;
        if (s->operation == CAI_APP_SHELL_FINISH) {
          peer->shell_runtime = NULL;
          peer->shell_id = peer->shell_sequence = 0U;
        }
      }
    }
  }
  if (rc == CAI_OK && recording_runtime != NULL)
    rc = cai_agent_runtime_user_shell_range(recording_runtime, &first, &last,
                                            &local);
  memset(&reply, 0, sizeof(reply));
  reply.header.kind = CAI_PROTOCOL_RESULT;
  reply.header.request = m->header.request;
  reply.header.conversation = m->header.conversation;
  reply.category = CAI_CLI_INPUT_SHELL_RESULT;
  reply.observation.result.item_revision = first;
  reply.observation.result.edit_token = last;
  reply.observation.result.code = rc;
  reply.observation.result.outcome =
      rc == CAI_OK ? CAI_INPUT_ACCEPTED : CAI_INPUT_REJECTED;
  reply.observation.result.item_id =
      s->operation == CAI_APP_SHELL_BEGIN ? m->header.request : s->recording;
  snprintf(reply.observation.result.reason,
           sizeof(reply.observation.result.reason), "%s",
           local.message != NULL ? local.message : "");
  cai_error_cleanup(&local);
  return send_message(peer, &reply, error);
}
static int append_prompt(host_peer *peer, const cai_cli_input_wire_message *m,
                         cai_error *error) {
  cai_cli_workspace_host *host = peer->host;
  cai_cli_input_wire_message result;
  cai_error local;
  unsigned long long epoch = 0U;
  unsigned char digest[SHA256_DIGEST_LENGTH];
  char identity[112], instance[33];
  size_t i;
  int rc;
  cai_error_init(&local);
  SHA256((const unsigned char *)m->request.text, strlen(m->request.text),
         digest);
  if (m->header.request < peer->prompt_request ||
      (m->header.request == peer->prompt_request &&
       (m->header.generation != peer->prompt_generation ||
        m->header.conversation != peer->prompt_conversation ||
        memcmp(digest, peer->prompt_digest, sizeof(digest)) != 0)))
    rc = cai_cli_error(&local, CAI_ERR_INVALID,
                       "prompt save identity is stale or changed");
  else if (m->header.request == peer->prompt_request) {
    rc = peer->prompt_code;
    if (rc != CAI_OK)
      cai_cli_error(&local, rc, peer->prompt_reason);
  } else {
    rc =
        authorize_control(host, peer->attachment, m->header.generation, &local);
    if (rc == CAI_OK)
      rc = host->input != NULL && peer->input != NULL && !peer->binding
               ? cai_input_controller_conversation(host->input, &epoch, &local)
               : cai_cli_error(&local, CAI_ERR_INVALID,
                               "bound prompt-history context required");
    if (rc == CAI_OK && epoch != m->header.conversation)
      rc = cai_cli_error(&local, CAI_ERR_INVALID,
                         "prompt-history session changed");
    if (rc == CAI_OK && (host->pouch == NULL || host->prompt_limit == 0U))
      rc = cai_cli_error(&local, CAI_ERR_INVALID, "prompt history is disabled");
    if (rc == CAI_OK) {
      const unsigned char *bytes = cai_workspace_server_instance(host->server);
      for (i = 0U; i < 16U; ++i)
        snprintf(instance + 2U * i, 3U, "%02x", (unsigned int)bytes[i]);
      snprintf(identity, sizeof(identity), "saved-%s-%llu-%llu", instance,
               (unsigned long long)peer->attachment,
               (unsigned long long)m->header.request);
      peer->prompt_position = 0U;
      rc = cai_cli_pouch_prompt_save(host->pouch, identity, m->request.text,
                                     &peer->prompt_position, &local);
    }
    peer->prompt_request = m->header.request;
    peer->prompt_generation = m->header.generation;
    peer->prompt_conversation = m->header.conversation;
    memcpy(peer->prompt_digest, digest, sizeof(digest));
    peer->prompt_code = rc;
    snprintf(peer->prompt_reason, sizeof(peer->prompt_reason), "%s",
             local.message != NULL ? local.message : "");
  }
  memset(&result, 0, sizeof(result));
  result.header.kind = CAI_PROTOCOL_RESULT;
  result.header.request = m->header.request;
  result.header.conversation = m->header.conversation;
  result.category = CAI_CLI_INPUT_HISTORY_RESULT;
  result.observation.result.code = rc;
  result.observation.result.item_id = rc == CAI_OK ? peer->prompt_position : 0U;
  result.observation.result.outcome =
      rc == CAI_OK ? CAI_INPUT_ACCEPTED : CAI_INPUT_REJECTED;
  snprintf(result.observation.result.reason,
           sizeof(result.observation.result.reason), "%s",
           local.message != NULL ? local.message : "");
  cai_error_cleanup(&local);
  return send_message(peer, &result, error);
}
/** Complete an explicit startup selection after the transport pump returns.
 * Runtime replacement may close readers and producers; it must never reenter
 * the socket decoder that borrowed the original request. */
static int select_peer(host_peer *peer, cai_error *error) {
  cai_cli_workspace_host *host = peer->host;
  cai_cli_input_wire_message reply;
  cai_error local;
  unsigned long long epoch = 0U;
  int rc;
  peer->selecting = 0;
  cai_error_init(&local);
  rc = authorize_control(host, peer->attachment, peer->selection_generation,
                         &local);
  if (rc == CAI_OK && peer->input != NULL)
    rc = cai_cli_error(&local, CAI_ERR_INVALID,
                       "session selection requires an unbound producer");
  if (rc == CAI_OK)
    rc = host->select != NULL
             ? host->select(host->select_context,
                            peer->selection[0] != '\0' ? peer->selection : NULL,
                            &local)
             : cai_cli_error(&local, CAI_ERR_INVALID,
                             "session selection unavailable");
  if (rc == CAI_OK)
    rc = cai_input_controller_conversation(host->input, &epoch, &local);
  if (rc == CAI_OK)
    rc = cai_input_controller_connect(host->input, peer->attachment,
                                      &peer->input, &local);
  if (rc == CAI_OK && peer->selection_managed) {
    cai_input_scope scope;
    scope.conversation = epoch;
    scope.generation = peer->selection_generation;
    rc = cai_input_connection_manage_delivery(peer->input, &scope, &local);
  }
  if (rc == CAI_OK) {
    peer->managed_delivery = peer->selection_managed;
    peer->binding_request = peer->selection_request;
    peer->binding = 1;
  } else {
    if (peer->input != NULL) {
      cai_input_connection_close(peer->input);
      peer->input = NULL;
    }
    memset(&reply, 0, sizeof(reply));
    reply.header.kind = CAI_PROTOCOL_RESULT;
    reply.header.request = peer->selection_request;
    reply.category = CAI_CLI_INPUT_BIND_REJECTED;
    reply.observation.result.outcome = CAI_INPUT_REJECTED;
    reply.observation.result.code = rc;
    snprintf(reply.observation.result.reason,
             sizeof(reply.observation.result.reason), "%s",
             local.message != NULL ? local.message
                                   : "session selection failed");
    rc = send_message(peer, &reply, error);
  }
  cai_error_cleanup(&local);
  return rc;
}
static int message(void *context, const cai_workspace_server_event *event,
                   cai_error *error) {
  cai_cli_workspace_host *host = context;
  host_peer *peer = NULL;
  const cai_cli_input_wire_message *m;
  size_t i;
  int rc;
  for (i = 0U; i < CAI_ATTACHMENT_LIMIT; ++i)
    if (host->peers[i].attachment == event->attachment &&
        event->attachment != 0U)
      peer = &host->peers[i];
  if (event->type == CAI_WORKSPACE_ATTACHED) {
    if (peer != NULL)
      return cai_cli_error(error, CAI_ERR_PROTOCOL,
                           "duplicate workspace attachment");
    for (i = 0U; i < CAI_ATTACHMENT_LIMIT; ++i)
      if (host->peers[i].attachment == 0U) {
        peer = &host->peers[i];
        break;
      }
    if (peer == NULL)
      return cai_cli_error(error, CAI_ERR_LIMIT,
                           "workspace input attachment limit reached");
    peer->host = host;
    peer->attachment = event->attachment;
    return CAI_OK;
  }
  if (event->type == CAI_WORKSPACE_DETACHED) {
    if (peer != NULL)
      peer->detached = 1;
    return CAI_OK;
  }
  if (event->type == CAI_WORKSPACE_PEER_ERROR && event->error != NULL)
    cai_cli_log_error(host->logger, "workspace attachment", event->error);
  if (event->type != CAI_WORKSPACE_MESSAGE)
    return CAI_OK;
  if (peer == NULL || peer->detached)
    return cai_cli_error(error, CAI_ERR_PROTOCOL,
                         "workspace input attachment is unavailable");
  if (event->message->header->kind == CAI_PROTOCOL_HISTORY_GET ||
      event->message->header->kind == CAI_PROTOCOL_SUBSCRIBE ||
      event->message->header->kind == CAI_PROTOCOL_UNSUBSCRIBE)
    return transcript_request(peer, event->message, error);
  if (event->message->header->kind == CAI_PROTOCOL_CANCEL) {
    const cai_protocol_header *h = event->message->header;
    cai_cli_input_wire_message result;
    cai_error local;
    unsigned long long epoch = 0U;
    if (event->message->type != CAI_PROTOCOL_COMPLETE)
      return CAI_OK;
    memset(&result, 0, sizeof(result));
    cai_error_init(&local);
    if (h->request == 0U || h->request < peer->cancel_request ||
        (h->request == peer->cancel_request &&
         (h->generation != peer->cancel_generation ||
          h->conversation != peer->cancel_conversation)))
      rc = cai_cli_error(&local, CAI_ERR_INVALID,
                         "cancellation identity is stale");
    else if (h->request == peer->cancel_request) {
      rc = peer->cancel_code;
      if (rc != CAI_OK)
        cai_cli_error(&local, rc, peer->cancel_reason);
    } else {
      rc = authorize_control(host, peer->attachment, h->generation, &local);
      if (rc == CAI_OK)
        rc =
            host->input != NULL
                ? cai_input_controller_conversation(host->input, &epoch, &local)
                : cai_cli_error(&local, CAI_ERR_INVALID,
                                "foreground context unavailable");
      if (rc == CAI_OK && epoch != h->conversation)
        rc = cai_cli_error(&local, CAI_ERR_INVALID,
                           "cancellation session changed");
      if (rc == CAI_OK)
        rc = host->cancel != NULL
                 ? host->cancel(host->cancel_context, &local)
                 : cai_cli_error(&local, CAI_ERR_INVALID,
                                 "foreground context unavailable");
      peer->cancel_request = h->request;
      peer->cancel_generation = h->generation;
      peer->cancel_conversation = h->conversation;
      peer->cancel_code = rc;
      snprintf(peer->cancel_reason, sizeof(peer->cancel_reason), "%s",
               local.message != NULL ? local.message : "");
    }
    result.header.kind = CAI_PROTOCOL_RESULT;
    result.header.request = h->request;
    result.category = CAI_CLI_INPUT_CONTROL_RESULT;
    result.observation.result.code = rc;
    result.observation.result.outcome =
        rc == CAI_OK ? CAI_INPUT_ACCEPTED : CAI_INPUT_REJECTED;
    snprintf(result.observation.result.reason,
             sizeof(result.observation.result.reason), "%s",
             local.message != NULL ? local.message : "");
    cai_error_cleanup(&local);
    return send_message(peer, &result, error);
  }
  if (peer->decoder == NULL) {
    rc = cai_cli_input_wire_decoder_open(&peer->decoder, error);
    if (rc != CAI_OK)
      return rc;
  }
  rc = cai_cli_input_wire_decode(peer->decoder, event->message, &m, error);
  if (rc != CAI_OK || m == NULL)
    return rc;
  if (m->header.kind == CAI_PROTOCOL_HISTORY_APPEND)
    return append_prompt(peer, m, error);
  if (m->header.kind == CAI_PROTOCOL_USER_SHELL)
    return shell_request(peer, m, error);
  if (m->header.kind == CAI_PROTOCOL_SYNCHRONIZE) {
    unsigned long long epoch = 0U;
    cai_error binding;
    if (peer->selecting)
      return cai_cli_error(error, CAI_ERR_PROTOCOL,
                           "session selection is already pending");
    if (m->select_session) {
      if (peer->input != NULL)
        return cai_cli_error(error, CAI_ERR_PROTOCOL,
                             "duplicate session selection");
      peer->selecting = 1;
      peer->selection_request = m->header.request;
      peer->selection_generation = m->header.generation;
      peer->selection_managed = m->managed_delivery;
      snprintf(peer->selection, sizeof(peer->selection), "%s",
               m->session != NULL ? m->session : "");
      return CAI_OK;
    }
    cai_error_init(&binding);
    rc = host->input != NULL
             ? cai_input_controller_conversation(host->input, &epoch, &binding)
             : cai_cli_error(&binding, CAI_ERR_INVALID, "no active session");
    if (rc == CAI_OK &&
        (peer->input != NULL ||
         (m->header.conversation != 0U && m->header.conversation != epoch) ||
         (m->producer != 0U && (m->session == NULL || m->session[0] == '\0')) ||
         (m->session != NULL && m->session[0] != '\0' &&
          strcmp(m->session, host->session) != 0)))
      rc = cai_cli_error(&binding, CAI_ERR_INVALID,
                         "producer binding or session does not match");
    if (rc == CAI_OK)
      rc = m->producer != 0U
               ? cai_input_controller_resume(host->input, peer->attachment,
                                             epoch, m->producer, &peer->input,
                                             &binding)
               : cai_input_controller_connect(host->input, peer->attachment,
                                              &peer->input, &binding);
    if (rc == CAI_OK) {
      if (m->managed_delivery) {
        cai_input_scope scope;
        cai_attachment_state role;
        scope.conversation = epoch;
        rc = cai_workspace_server_get_state(host->server, &role, error);
        if (rc != CAI_OK)
          return rc;
        /* Binding is read-only. Gate the producer under the live role;
         * subsequent mutation requests retain their original generation. */
        scope.generation = role.generation;
        rc = cai_input_connection_manage_delivery(peer->input, &scope, error);
        if (rc != CAI_OK)
          return rc;
      }
      peer->managed_delivery = m->managed_delivery;
      peer->binding_request = m->header.request;
      peer->binding = 1;
    } else {
      cai_cli_input_wire_message reply;
      memset(&reply, 0, sizeof(reply));
      reply.header.kind = CAI_PROTOCOL_RESULT;
      reply.header.request = m->header.request;
      reply.header.conversation = m->header.conversation;
      reply.category = CAI_CLI_INPUT_BIND_REJECTED;
      reply.observation.result.outcome = CAI_INPUT_REJECTED;
      reply.observation.result.code = rc;
      snprintf(reply.observation.result.reason,
               sizeof(reply.observation.result.reason), "%s",
               binding.message != NULL ? binding.message
                                       : "input binding failed");
      rc = send_message(peer, &reply, error);
    }
    cai_error_cleanup(&binding);
    return rc;
  }
  /* A readiness observation can have been sent before the peer received the
   * context-closed notification. It cannot release a departed/new producer. */
  if ((peer->input == NULL || peer->binding) &&
      m->header.kind == CAI_PROTOCOL_INPUT_SETTLED)
    return CAI_OK;
  if (peer->input == NULL || peer->binding)
    return cai_cli_error(error, CAI_ERR_PROTOCOL,
                         "bind the input producer before submitting work");
  {
    cai_input_scope scope;
    scope.conversation = m->header.conversation;
    scope.generation = m->header.generation;
    if (m->header.kind == CAI_PROTOCOL_INPUT_SETTLED)
      return cai_input_connection_settled(peer->input, &scope,
                                          m->delivery_after, error);
    rc = cai_input_connection_request(peer->input, &scope, &m->request, error);
    return rc;
  }
}
static int release_peer(host_peer *peer, cai_error *error) {
  int rc = shell_abandon(peer, error);
  if (rc != CAI_OK)
    return rc;
  rc = peer->host != NULL && peer->host->input != NULL
           ? cai_input_controller_release_edits(peer->host->input,
                                                peer->attachment, error)
           : CAI_OK;
  if (peer->host != NULL)
    cai_cli_transcript_feed_detach(peer->host->history, peer->attachment);
  if (peer->input != NULL) {
    cai_input_connection_close(peer->input);
    peer->input = NULL;
  }
  return rc;
}
static int output_one(host_peer *peer, int *produced, cai_error *error) {
  cai_input_observation *o;
  cai_cli_input_wire_message m;
  int rc;
  *produced = 0;
  if (peer->input == NULL)
    return CAI_OK;
  rc = cai_input_connection_poll(peer->input, &o, error);
  if (rc != CAI_OK || o == NULL)
    return rc;
  *produced = 1;
  memset(&m, 0, sizeof(m));
  m.observation = *o;
  m.header.conversation = o->conversation;
  m.header.sequence = o->state.view_revision;
  switch (o->type) {
  case CAI_INPUT_OBSERVATION_SNAPSHOT_BEGIN:
    m.header.kind = CAI_PROTOCOL_QUEUE_BEGIN;
    m.session = peer->host->session;
    peer->snapshot_request = peer->binding ? peer->binding_request : 0U;
    m.header.request = peer->snapshot_request;
    break;
  case CAI_INPUT_OBSERVATION_SNAPSHOT_ITEM:
    m.header.kind = CAI_PROTOCOL_QUEUE_ITEM;
    m.header.request = peer->snapshot_request;
    break;
  case CAI_INPUT_OBSERVATION_SNAPSHOT_END:
    m.header.kind = CAI_PROTOCOL_QUEUE_END;
    m.header.request = peer->snapshot_request;
    peer->binding = 0;
    break;
  case CAI_INPUT_OBSERVATION_CHANGE:
    m.header.kind = CAI_PROTOCOL_QUEUE_CHANGE;
    break;
  case CAI_INPUT_OBSERVATION_RESULT:
    m.header.kind = CAI_PROTOCOL_RESULT;
    m.header.sequence = 0U;
    m.header.request = o->result.request_id;
    break;
  case CAI_INPUT_OBSERVATION_DISCONNECTED:
    m.header.kind = CAI_PROTOCOL_RESULT;
    m.header.sequence = 0U;
    m.category = peer->binding && o->result.code != CAI_OK
                     ? CAI_CLI_INPUT_BIND_REJECTED
                     : CAI_CLI_INPUT_RESET;
    m.header.request =
        m.category == CAI_CLI_INPUT_BIND_REJECTED ? peer->binding_request : 0U;
    if (o->result.code != CAI_OK) {
      cai_input_connection_close(peer->input);
      peer->input = NULL;
      peer->binding = 0;
    }
    break;
  }
  rc = send_message(peer, &m, error);
  cai_input_observation_close(o);
  return rc;
}
int cai_cli_workspace_host_input(cai_cli_workspace_host *host,
                                 cai_input_controller *input,
                                 const char *session, cai_error *error) {
  size_t i;
  int result = CAI_OK;
  if (host == NULL)
    return CAI_OK;
  if (input != NULL) {
    if (host->input != NULL || session == NULL || strlen(session) > 128U)
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "workspace input binding is invalid");
    host->input = input;
    strcpy(host->session, session);
    return CAI_OK;
  }
  for (i = 0U; i < CAI_ATTACHMENT_LIMIT; ++i) {
    host_peer *peer = &host->peers[i];
    if (!peer->detached && host->catalog_runtime != NULL &&
        peer->catalog_revision == host->catalog_revision)
      (void)send_catalog(peer, CAI_APP_CATALOG_ERROR, "Session closed",
                         CAI_ERR_CANCELLED, NULL);
  }
  host->catalog_runtime = NULL;
  for (i = 0U; i < CAI_ATTACHMENT_LIMIT; ++i)
    if (host->peers[i].stream != 0U) {
      (void)transcript_error(&host->peers[i], host->peers[i].stream,
                             CAI_TRANSCRIPT_CONTEXT_CLOSED, CAI_ERR_CANCELLED,
                             "Session closed", NULL);
      host->peers[i].stream = 0U;
    }
  for (i = 0U; i < CAI_ATTACHMENT_LIMIT; ++i)
    if (host->peers[i].prompt_stream != 0U) {
      (void)transcript_error(&host->peers[i], host->peers[i].prompt_stream,
                             CAI_TRANSCRIPT_CONTEXT_CLOSED, CAI_ERR_CANCELLED,
                             "Session closed", NULL);
      host->peers[i].prompt_stream = 0U;
    }
  cai_cli_transcript_feed_close(host->history);
  host->history = NULL;
  host->pouch = NULL;
  for (i = 0U; i < CAI_ATTACHMENT_LIMIT; ++i) {
    host_peer *peer = &host->peers[i];
    cai_error local;
    int rc;
    cai_error_init(&local);
    if (peer->input != NULL && !peer->detached) {
      cai_cli_input_wire_message reset;
      memset(&reset, 0, sizeof(reset));
      reset.header.kind = CAI_PROTOCOL_RESULT;
      reset.category = CAI_CLI_INPUT_RESET;
      (void)send_message(peer, &reset, NULL);
    }
    rc = release_peer(peer, &local);
    if (rc != CAI_OK && result == CAI_OK) {
      result = rc;
      cai_cli_error(error, rc,
                    local.message != NULL ? local.message
                                          : "release workspace editor");
    }
    cai_error_cleanup(&local);
  }
  host->input = NULL;
  host->session[0] = '\0';
  return result;
}
int cai_cli_workspace_host_history(cai_cli_workspace_host *host,
                                   cai_cli_pouch *pouch, cai_error *error) {
  unsigned long long epoch;
  int rc;
  if (host == NULL)
    return CAI_OK;
  if (host->history != NULL || host->input == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "Open session required for transcript");
  rc = cai_input_controller_conversation(host->input, &epoch, error);
  if (rc == CAI_OK)
    rc = cai_cli_transcript_feed_open(
        pouch, cai_workspace_server_directory(host->server), host->session,
        epoch, &host->history, error);
  if (rc == CAI_OK) {
    host->pouch = pouch;
    cai_cli_transcript_feed_prompt_limit(host->history, host->prompt_limit);
  }
  return rc;
}
void cai_cli_workspace_host_prompt_limit(cai_cli_workspace_host *host,
                                         unsigned int limit) {
  if (host != NULL) {
    host->prompt_limit = limit;
    cai_cli_transcript_feed_prompt_limit(host->history, limit);
  }
}
int cai_cli_workspace_host_open(
    const char *runtime_directory, const char *workspace, const char *name,
    const cai_workspace_claim *claims, size_t claim_count,
    const cai_attachment_config *lifetime,
    const cai_input_controller_config *admission, cai_app_bridge *bridge,
    pslog_logger *logger, cai_cli_workspace_host **out, cai_error *error) {
  cai_workspace_server_config config;
  cai_cli_workspace_host *host;
  int rc;
  *out = NULL;
  host = calloc(1U, sizeof(*host));
  if (host == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate workspace host");
  host->bridge = bridge;
  host->logger = logger;
  if (admission != NULL)
    host->admission = *admission;
  cai_workspace_server_config_init(&config);
  config.runtime_directory = runtime_directory;
  config.workspace = workspace;
  config.name = name;
  config.claims = claims;
  config.claim_count = claim_count;
  config.lifetime = *lifetime;
  config.event = message;
  config.context = host;
  rc = cai_cli_private_directory(runtime_directory, error);
  if (rc == CAI_OK)
    rc = cai_workspace_server_open(&config, &host->server, error);
  if (rc == CAI_OK)
    rc = publish(host, error);
  if (rc != CAI_OK)
    cai_cli_workspace_host_close(host);
  else
    *out = host;
  return rc;
}
void cai_cli_workspace_host_selection(cai_cli_workspace_host *host,
                                      int (*select)(void *, const char *,
                                                    cai_error *),
                                      void *context) {
  if (host != NULL) {
    host->select = select;
    host->select_context = context;
  }
}
void cai_cli_workspace_host_cancel(cai_cli_workspace_host *host,
                                   int (*cancel)(void *, cai_error *),
                                   void *context) {
  if (host != NULL) {
    host->cancel = cancel;
    host->cancel_context = context;
  }
}
int cai_cli_workspace_host_ready(cai_cli_workspace_host *host,
                                 cai_error *error) {
  cai_attachment_state snapshot;
  uint64_t now;
  int rc;
  if (host == NULL)
    return CAI_OK;
  rc = cai_workspace_server_get_state(host->server, &snapshot, error);
  if (rc == CAI_OK && snapshot.phase == CAI_ATTACHMENT_STARTING) {
    rc = clock_ms(&now, error);
    if (rc == CAI_OK)
      rc = cai_workspace_server_ready(host->server, now, error);
  }
  return rc == CAI_OK ? publish(host, error) : rc;
}
/** A single bounded transcript span, shared by ordinary dispatch and output
 * continuation. Readiness is advisory; next() owns the atomic dequeue. */
static int output_transcript(host_peer *peer, int *produced, cai_error *error) {
  cai_cli_transcript_frame frame;
  size_t pending;
  int failed, rc;
  *produced = 0;
  if (peer->stream == 0U || !cai_cli_transcript_feed_ready(
                                peer->host->history, peer->attachment, &failed))
    return CAI_OK;
  rc = cai_workspace_server_pending(peer->host->server, peer->attachment,
                                    &pending, error);
  if (rc != CAI_OK || (!failed && pending > 65536U))
    return rc;
  if (!cai_cli_transcript_feed_next(peer->host->history, peer->attachment,
                                    &frame))
    return CAI_OK;
  *produced = 1;
  rc = send_transcript(peer, &frame.event, error);
  if (frame.event.kind == CAI_TRANSCRIPT_ERROR)
    peer->stream = 0U;
  return rc;
}
int cai_cli_workspace_host_shell(cai_cli_workspace_host *host,
                                 uint64_t sequence, uint64_t offset,
                                 const void *bytes, size_t length, int final,
                                 int *accepted, cai_error *error) {
  cai_attachment_state state;
  uint64_t guard = 0U;
  if (host != NULL) {
    int rc = cai_workspace_server_get_state(host->server, &state, error);
    if (rc != CAI_OK)
      return rc;
    guard = state.controller;
  }
  return cai_cli_transcript_feed_shell(host != NULL ? host->history : NULL,
                                       guard, sequence, offset, bytes, length,
                                       final, accepted, error);
}
int cai_cli_workspace_host_terminal(cai_cli_workspace_host *host,
                                    const cai_agent_runtime_event *event,
                                    cai_error *error) {
  return cai_cli_transcript_feed_terminal(host != NULL ? host->history : NULL,
                                          event, error);
}
int cai_cli_workspace_host_progress(cai_cli_workspace_host *host,
                                    cai_error *error) {
  size_t i;
  int rc;
  if (host == NULL)
    return CAI_OK;
  rc = cai_workspace_server_flush(host->server, error);
  for (i = 0U; rc == CAI_OK && i < CAI_ATTACHMENT_LIMIT; ++i) {
    host_peer *peer = &host->peers[i];
    cai_error local;
    unsigned int spans;
    if (peer->attachment == 0U || peer->detached)
      continue;
    cai_error_init(&local);
    /* Never wait for storage, readers or socket credit. At most 64 existing
     * bounded spans per subscriber; ordinary dispatch handles any remainder. */
    for (spans = 0U; spans < 64U; ++spans) {
      int produced;
      size_t pending;
      if (cai_workspace_server_pending(host->server, peer->attachment, &pending,
                                       &local) != CAI_OK ||
          pending > 65536U || output_one(peer, &produced, &local) != CAI_OK)
        break;
      /* Claim/removal receipts precede the accepted command presentation;
       * the editor must stop offering an already executing item for edits. */
      if (!produced &&
          (output_transcript(peer, &produced, &local) != CAI_OK || !produced))
        break;
    }
    cai_error_cleanup(&local);
  }
  return rc == CAI_OK ? cai_workspace_server_flush(host->server, error) : rc;
}
int cai_cli_workspace_host_prepare(cai_cli_workspace_host *host,
                                   struct pollfd *fds, size_t capacity,
                                   size_t *count, long *timeout,
                                   cai_error *error) {
  uint64_t deadline, now, remaining;
  size_t i;
  int rc;
  *count = 0U;
  if (host == NULL)
    return CAI_OK;
  if (capacity < CAI_CLI_WORKSPACE_HOST_FDS)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "workspace poll capacity is insufficient");
  rc = cai_workspace_server_prepare(host->server, fds, CAI_WORKSPACE_SERVER_FDS,
                                    count, &deadline, error);
  if (rc != CAI_OK)
    return rc;
  for (i = 0U; i < CAI_ATTACHMENT_LIMIT; ++i) {
    host_peer *peer = &host->peers[i];
    size_t pending;
    if (peer->input != NULL && !peer->detached &&
        cai_workspace_server_pending(host->server, peer->attachment, &pending,
                                     NULL) == CAI_OK &&
        pending <= 65536U) {
      fds[*count].fd = cai_input_connection_fd(peer->input);
      fds[(*count)++].events = POLLIN;
    }
    if (peer->detached)
      *timeout = 0L;
    if (peer->attachment != 0U && !peer->detached &&
        (peer->status_revision < host->status.revision ||
         catalog_pending(peer)) &&
        cai_workspace_server_pending(host->server, peer->attachment, &pending,
                                     NULL) == CAI_OK &&
        pending <= 65536U)
      *timeout = 0L;
  }
  for (i = 0U; i < CAI_ATTACHMENT_LIMIT; ++i) {
    size_t pending;
    int failed;
    host_peer *peer = &host->peers[i];
    if (!peer->detached && peer->stream != 0U &&
        cai_cli_transcript_feed_ready(host->history, peer->attachment,
                                      &failed) &&
        cai_workspace_server_pending(host->server, peer->attachment, &pending,
                                     NULL) == CAI_OK &&
        (failed || pending <= 65536U)) {
      *timeout = 0L;
      break;
    }
  }
  if (host->history != NULL) {
    for (i = 0U; i < CAI_ATTACHMENT_LIMIT; ++i) {
      host_peer *peer = &host->peers[i];
      size_t pending;
      int failed;
      if (!peer->detached && peer->prompt_stream != 0U &&
          cai_cli_transcript_feed_prompt_ready(host->history, peer->attachment,
                                               &failed) &&
          cai_workspace_server_pending(host->server, peer->attachment, &pending,
                                       NULL) == CAI_OK &&
          (failed || pending <= 65536U))
        *timeout = 0L;
    }
    fds[*count].fd = cai_cli_transcript_feed_fd(host->history);
    fds[(*count)++].events = POLLIN;
  }
  if (rc != CAI_OK || deadline == 0U)
    return rc;
  rc = clock_ms(&now, error);
  if (rc != CAI_OK)
    return rc;
  remaining = deadline > now ? deadline - now : 0U;
  if (remaining > INT_MAX)
    remaining = INT_MAX;
  if (*timeout < 0L || (uint64_t)*timeout > remaining)
    *timeout = (long)remaining;
  return CAI_OK;
}
int cai_cli_workspace_host_pump(cai_cli_workspace_host *host, int *stopping,
                                cai_error *error) {
  cai_attachment_state snapshot;
  uint64_t now;
  int rc;
  size_t i;
  *stopping = 0;
  if (host == NULL)
    return CAI_OK;
  cai_cli_transcript_feed_drain(host->history);
  rc = clock_ms(&now, error);
  if (rc == CAI_OK)
    rc = cai_workspace_server_pump(host->server, now, error);
  if (rc != CAI_OK)
    return rc;
  rc = cai_workspace_server_get_state(host->server, &snapshot, error);
  if (rc != CAI_OK)
    return rc;
  if (host->input != NULL && host->generation != 0U &&
      host->generation != snapshot.generation)
    for (i = 0U; i < CAI_ATTACHMENT_LIMIT; ++i)
      if (host->peers[i].attachment != 0U) {
        if (host->peers[i].attachment == host->controller) {
          rc = cai_input_controller_release_edits(
              host->input, host->peers[i].attachment, error);
          if (rc != CAI_OK)
            return rc;
        }
        if (host->peers[i].shell_runtime != NULL &&
            host->peers[i].shell_generation != snapshot.generation) {
          rc = shell_abandon(&host->peers[i], error);
          if (rc != CAI_OK)
            return rc;
        }
        if (host->peers[i].input != NULL && host->peers[i].managed_delivery) {
          cai_input_scope scope;
          rc = cai_input_controller_conversation(host->input,
                                                 &scope.conversation, error);
          scope.generation = snapshot.generation;
          if (rc == CAI_OK)
            rc = cai_input_connection_manage_delivery(host->peers[i].input,
                                                      &scope, error);
          if (rc != CAI_OK)
            return rc;
        }
      }
  host->generation = snapshot.generation;
  host->controller = snapshot.controller;
  for (i = 0U; i < CAI_ATTACHMENT_LIMIT; ++i) {
    host_peer *peer = &host->peers[i];
    size_t pending;
    cai_error local;
    if (peer->attachment == 0U)
      continue;
    if (peer->detached) {
      rc = release_peer(peer, error);
      cai_cli_input_wire_decoder_close(peer->decoder);
      cai_cli_transcript_decoder_close(peer->transcript_decoder);
      memset(peer, 0, sizeof(*peer));
      if (rc != CAI_OK)
        return rc;
      continue;
    }
    cai_error_init(&local);
    if (peer->selecting) {
      rc = select_peer(peer, &local);
      if (rc != CAI_OK) {
        cai_error_cleanup(&local);
        return rc;
      }
    }
    rc = cai_workspace_server_pending(host->server, peer->attachment, &pending,
                                      &local);
    if (rc == CAI_OK && pending <= 65536U &&
        peer->status_revision < host->status.revision) {
      rc = send_status(peer, now, &local);
      if (rc == CAI_OK) {
        peer->status_revision = host->status.revision;
        rc = cai_workspace_server_pending(host->server, peer->attachment,
                                          &pending, &local);
      }
    }
    if (rc == CAI_OK && pending <= 65536U) {
      int produced;
      rc = output_one(peer, &produced, &local);
    }
    if (rc == CAI_OK && catalog_pending(peer) &&
        cai_workspace_server_pending(host->server, peer->attachment, &pending,
                                     &local) == CAI_OK &&
        pending <= 65536U)
      rc = output_catalog(peer, &local);
    if (rc == CAI_OK) {
      int produced;
      rc = output_transcript(peer, &produced, &local);
    }
    if (rc != CAI_OK) {
      cai_cli_log_error(host->logger, "workspace input output", &local);
      (void)cai_workspace_server_disconnect(host->server, peer->attachment,
                                            NULL);
    }
    if (rc == CAI_OK && peer->prompt_stream != 0U) {
      cai_cli_transcript_frame frame;
      int failed;
      if (cai_workspace_server_pending(host->server, peer->attachment, &pending,
                                       &local) == CAI_OK &&
          cai_cli_transcript_feed_prompt_ready(host->history, peer->attachment,
                                               &failed) &&
          (failed || pending <= 65536U) &&
          cai_cli_transcript_feed_prompt_next(host->history, peer->attachment,
                                              &frame)) {
        rc = send_transcript(peer, &frame.event, &local);
        if (frame.event.kind == CAI_TRANSCRIPT_ERROR ||
            frame.event.kind == CAI_TRANSCRIPT_REPLAY_END)
          peer->prompt_stream = 0U;
      }
      if (rc != CAI_OK)
        (void)cai_workspace_server_disconnect(host->server, peer->attachment,
                                              NULL);
    }
    cai_error_cleanup(&local);
  }
  rc = CAI_OK;
  if (rc == CAI_OK)
    rc = cai_workspace_server_get_state(host->server, &snapshot, error);
  if (rc == CAI_OK) {
    cai_app_bridge_server(host->bridge, &snapshot,
                          cai_workspace_server_instance(host->server));
    *stopping = snapshot.phase == CAI_ATTACHMENT_STOPPING;
  }
  return rc;
}
void cai_cli_workspace_host_admission(cai_cli_workspace_host *host,
                                      cai_input_controller_config *out) {
  out->authorize = authorize;
  out->authorize_context = host;
}
void cai_cli_workspace_host_stop(cai_cli_workspace_host *host) {
  if (host != NULL) {
    (void)cai_workspace_server_stop(host->server, NULL);
    (void)publish(host, NULL);
  }
}
void cai_cli_workspace_host_close(cai_cli_workspace_host *host) {
  size_t i;
  if (host != NULL) {
    cai_cli_transcript_feed_close(host->history);
    for (i = 0U; i < CAI_ATTACHMENT_LIMIT; ++i) {
      cai_input_connection_close(host->peers[i].input);
      cai_cli_input_wire_decoder_close(host->peers[i].decoder);
      cai_cli_transcript_decoder_close(host->peers[i].transcript_decoder);
    }
    cai_workspace_server_close(host->server);
    cai_cli_status_cleanup(&host->status);
    free(host);
  }
}
