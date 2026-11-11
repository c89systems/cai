/** @file Bounded TCP input, strict UTF-8, event-driven typing and EOF
 * admission. */
#define _POSIX_C_SOURCE 200809L
#include "prompt_listener.h"
#include "error.h"
#include "input_wire.h"
#include <cai/network.h>
#include <cai/utf8.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

struct cai_cli_prompt_listener {
  cai_net *listener, *peer;
  cai_cli_prompt_scope_fn scope;
  cai_cli_prompt_notice_fn notice;
  cai_cli_prompt_changed_fn changed;
  void *context;
  cai_cli_prompt_scope original;
  cai_utf8_state utf8;
  sl_t *watch_editor;
  sl_watch_id_t listen_watch, peer_watch, timer_watch;
  int timer, peer_watched, separate, eof, awaiting_submit, submit_idle,
      offering;
  unsigned long delay;
  uint64_t next_character;
  size_t length, pending_length;
  unsigned char pending[4097];
  char *queued;
};
static int same_scope(const cai_cli_prompt_scope *a,
                      const cai_cli_prompt_scope *b) {
  return b->ready && a->attachment == b->attachment &&
         a->generation == b->generation && a->epoch == b->epoch &&
         a->editor == b->editor && !memcmp(a->instance, b->instance, 16U);
}
static void disarm(cai_cli_prompt_listener *p) {
  struct itimerspec timer;
  memset(&timer, 0, sizeof(timer));
  (void)timerfd_settime(p->timer, 0, &timer, NULL);
}
static void finish(cai_cli_prompt_listener *p, int aborting) {
  if (p->peer_watched) {
    (void)sl_watch_remove(p->watch_editor, p->peer_watch);
    p->peer_watched = 0;
  }
  if (aborting && p->peer != NULL && !p->separate && p->original.editor != NULL)
    (void)sl_set_buffer(p->original.editor, "");
  cai_net_close(p->peer, aborting);
  p->peer = NULL;
  free(p->queued);
  p->queued = NULL;
  p->length = p->pending_length = 0U;
  p->eof = p->separate = 0;
  p->awaiting_submit = 0;
  p->offering = 0;
  p->submit_idle = 0;
  p->next_character = 0U;
  memset(&p->utf8, 0, sizeof(p->utf8));
  disarm(p);
  if (p->changed != NULL)
    p->changed(p->context);
}
static void reject(cai_cli_prompt_listener *p, const char *message) {
  finish(p, 1);
  if (p->notice != NULL)
    p->notice(p->context, message);
}
static int watch_peer(cai_cli_prompt_listener *p);
static int active_scope(cai_cli_prompt_listener *p) {
  cai_cli_prompt_scope scope;
  memset(&scope, 0, sizeof(scope));
  p->scope(p->context, &scope);
  if (!same_scope(&p->original, &scope)) {
    reject(p, "TCP prompt discarded: attachment authority or active session "
              "changed.");
    return 0;
  }
  return 1;
}
static void admit(cai_cli_prompt_listener *p) {
  int rc;
  cai_cli_prompt_scope current;
  if (!active_scope(p))
    return;
  if (p->length == 0U) {
    finish(p, 0);
    return;
  }
  memset(&current, 0, sizeof(current));
  p->scope(p->context, &current);
  if (p->separate || current.parked) {
    sl_queue_request_t request;
    sl_queue_request_id_t id;
    memset(&request, 0, sizeof(request));
    request.operation = SL_QUEUE_OFFER;
    request.mode = SL_PROMPT_QUEUE_MODE_QUEUED;
    request.text = p->separate ? p->queued : sl_buffer(p->original.editor);
    p->offering = 1;
    rc = sl_queue_backend_request(p->original.editor, &request, &id);
    p->offering = 0;
    if (rc == SL_OK && !p->separate)
      (void)sl_set_buffer(p->original.editor, "");
  } else if (!*sl_buffer(p->original.editor)) {
    finish(p, 0);
    return;
  } else {
    rc = sl_submit(p->original.editor);
    if (rc == SL_OK) {
      p->awaiting_submit = 1;
      return;
    }
  }
  if (rc != SL_OK)
    reject(p,
           "TCP prompt handoff failed; no partial queue entry was submitted.");
  else
    finish(p, 0);
}
static uint64_t now_ms(void) {
  struct timespec now;
  (void)clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}
static void arm(cai_cli_prompt_listener *p) {
  struct itimerspec timer;
  uint64_t now = now_ms();
  uint64_t remaining = p->next_character > now ? p->next_character - now : 1U;
  memset(&timer, 0, sizeof(timer));
  timer.it_value.tv_sec = (time_t)(remaining / 1000U);
  timer.it_value.tv_nsec = (long)(remaining % 1000U) * 1000000L;
  (void)timerfd_settime(p->timer, 0, &timer, NULL);
}
static void playback(cai_cli_prompt_listener *p) {
  size_t offset = 0U;
  if (p->delay && p->next_character > now_ms()) {
    arm(p);
    return;
  }
  if (!p->delay) {
    while (offset < p->pending_length) {
      unsigned char lead = p->pending[offset];
      size_t width = lead < 0x80U   ? 1U
                     : lead < 0xe0U ? 2U
                     : lead < 0xf0U ? 3U
                                    : 4U;
      if (width > p->pending_length - offset)
        break;
      offset += width;
    }
    if (offset) {
      unsigned char saved = p->pending[offset];
      int rc;
      p->pending[offset] = 0U;
      rc = sl_insert(p->original.editor, (char *)p->pending);
      p->pending[offset] = saved;
      if (rc != SL_OK) {
        reject(p, "TCP prompt insertion failed; incoming draft cleared.");
        return;
      }
    }
  }
  while (p->delay && offset < p->pending_length) {
    unsigned char lead = p->pending[offset];
    size_t width = lead < 0x80U   ? 1U
                   : lead < 0xe0U ? 2U
                   : lead < 0xf0U ? 3U
                                  : 4U;
    char scalar[5];
    if (width > p->pending_length - offset)
      break;
    memcpy(scalar, p->pending + offset, width);
    scalar[width] = '\0';
    if (sl_insert(p->original.editor, scalar) != SL_OK) {
      reject(p, "TCP prompt insertion failed; incoming draft cleared.");
      return;
    }
    offset += width;
    if (p->delay != 0UL) {
      p->next_character = now_ms() + p->delay;
      break;
    }
  }
  p->pending_length -= offset;
  memmove(p->pending, p->pending + offset, p->pending_length);
  if (p->eof && p->pending_length == 0U)
    admit(p);
  else if (p->peer != NULL) {
    if (offset != 0U && p->delay != 0UL && p->pending_length != 0U)
      arm(p);
    (void)watch_peer(p);
  }
}
static int ready(sl_t *editor, const sl_watch_event_t *event, void *context) {
  cai_cli_prompt_listener *p = context;
  cai_error error;
  (void)editor;
  cai_error_init(&error);
  if (event->fd == cai_net_fd(p->listener)) {
    unsigned int accepted;
    int refused = 0;
    for (accepted = 0U; accepted < 8U; accepted++) {
      cai_net *peer = NULL;
      cai_cli_prompt_scope scope;
      sl_prompt_queue_entry_info_t draft;
      if (cai_net_accept(p->listener, &peer, &error) != CAI_OK || peer == NULL)
        break;
      memset(&scope, 0, sizeof(scope));
      p->scope(p->context, &scope);
      if (p->peer != NULL || !scope.ready) {
        cai_net_close(peer, 1);
        refused = 1;
        continue;
      }
      p->peer = peer;
      p->original = scope;
      memset(&draft, 0, sizeof(draft));
      if (sl_prompt_queue_draft_info(scope.editor, &draft) != SL_OK) {
        reject(p, "TCP prompt rejected: editor state unavailable.");
        continue;
      }
      p->separate =
          scope.parked || draft.id != 0U || *sl_buffer(scope.editor) != '\0';
      if (p->separate) {
        p->queued = malloc(CAI_CLI_INPUT_TEXT_MAX + 1U);
        if (p->queued == NULL) {
          reject(p, "TCP prompt rejected: cannot allocate bounded prompt.");
          continue;
        }
        p->queued[0] = '\0';
      }
      if (watch_peer(p) != SL_OK)
        reject(p, "TCP prompt rejected: cannot watch incoming socket.");
      else if (p->changed != NULL)
        p->changed(p->context);
    }
    if (refused && p->notice != NULL)
      p->notice(p->context, "TCP prompt connection rejected: listener is busy "
                            "or attachment cannot admit input.");
  } else if (p->peer != NULL && active_scope(p)) {
    if (event->fd == p->timer) {
      uint64_t ticks;
      (void)read(p->timer, &ticks, sizeof(ticks));
      if (!p->awaiting_submit)
        playback(p);
    } else {
      unsigned char bytes[2048];
      size_t capacity = p->separate
                            ? sizeof(bytes)
                            : sizeof(p->pending) - 1U - p->pending_length;
      ssize_t count;
      if (capacity > sizeof(bytes))
        capacity = sizeof(bytes);
      count = recv(cai_net_fd(p->peer), bytes, capacity, 0);
      if (count < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
          reject(p, "TCP prompt receive failed; partial input discarded.");
      } else if (count == 0) {
        if (!cai_utf8_complete(&p->utf8))
          reject(p, "TCP prompt rejected: incomplete UTF-8 character.");
        else {
          p->eof = 1;
          if (p->peer_watched) {
            (void)sl_watch_remove(p->watch_editor, p->peer_watch);
            p->peer_watched = 0;
          }
          if (p->separate || p->pending_length == 0U)
            admit(p);
        }
      } else if ((size_t)count > CAI_CLI_INPUT_TEXT_MAX - p->length ||
                 memchr(bytes, 0, (size_t)count) != NULL ||
                 !cai_utf8_feed(&p->utf8, bytes, (size_t)count))
        reject(p,
               "TCP prompt rejected: invalid UTF-8, NUL or prompt size limit.");
      else {
        p->length += (size_t)count;
        if (p->separate) {
          memcpy(p->queued + p->length - (size_t)count, bytes, (size_t)count);
          p->queued[p->length] = '\0';
        } else {
          memcpy(p->pending + p->pending_length, bytes, (size_t)count);
          p->pending_length += (size_t)count;
          playback(p);
        }
        if (p->peer != NULL && !p->eof)
          (void)watch_peer(p);
      }
    }
  }
  cai_error_cleanup(&error);
  return SL_OK;
}
static int watch_peer(cai_cli_prompt_listener *p) {
  unsigned int flags = SL_WATCH_READ;
  int rc;
  /* Full pending chunks apply kernel TCP backpressure during paced playback. */
  if (!p->separate && p->pending_length == sizeof(p->pending) - 1U) {
    if (p->peer_watched)
      (void)sl_watch_remove(p->watch_editor, p->peer_watch);
    p->peer_watched = 0;
    return SL_OK;
  }
  if (p->eof)
    return SL_OK;
  if (p->peer_watched)
    return sl_watch_modify(p->watch_editor, p->peer_watch, flags);
  rc = sl_watch_add(p->watch_editor, cai_net_fd(p->peer), flags, ready, p,
                    &p->peer_watch);
  p->peer_watched = rc == SL_OK;
  return rc;
}
int cai_cli_prompt_listener_open(const char *address, int ipv6,
                                 unsigned long delay,
                                 cai_cli_prompt_scope_fn scope,
                                 cai_cli_prompt_notice_fn notice,
                                 cai_cli_prompt_changed_fn changed,
                                 void *context, cai_cli_prompt_listener **out,
                                 cai_error *error) {
  cai_net_config config;
  cai_cli_prompt_listener *p;
  int rc;
  *out = NULL;
  p = calloc(1U, sizeof(*p));
  if (p == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate TCP prompt listener");
  p->timer = -1;
  p->scope = scope;
  p->notice = notice;
  p->changed = changed;
  p->context = context;
  p->delay = delay;
  cai_net_config_init(&config);
  config.address = address;
  config.ipv6 = ipv6;
  rc = cai_net_open(&config, &p->listener, error);
  if (rc == CAI_OK) {
    p->timer = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (p->timer < 0)
      rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "create TCP typing timer");
  }
  if (rc != CAI_OK)
    cai_cli_prompt_listener_close(p);
  else
    *out = p;
  return rc;
}
int cai_cli_prompt_listener_watch(cai_cli_prompt_listener *p, sl_t *editor,
                                  cai_error *error) {
  if (p == NULL)
    return CAI_OK;
  if (p->peer != NULL)
    (void)active_scope(p);
  if (p->watch_editor == editor)
    return CAI_OK;
  if (p->watch_editor != NULL) {
    (void)sl_watch_remove(p->watch_editor, p->listen_watch);
    (void)sl_watch_remove(p->watch_editor, p->timer_watch);
    if (p->peer_watched)
      (void)sl_watch_remove(p->watch_editor, p->peer_watch);
    p->peer_watched = 0;
  }
  p->watch_editor = editor;
  if (editor != NULL &&
      (sl_watch_add(editor, cai_net_fd(p->listener), SL_WATCH_READ, ready, p,
                    &p->listen_watch) != SL_OK ||
       sl_watch_add(editor, p->timer, SL_WATCH_READ, ready, p,
                    &p->timer_watch) != SL_OK ||
       (p->peer != NULL && watch_peer(p) != SL_OK)))
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "watch TCP prompt listener");
  return CAI_OK;
}
const char *cai_cli_prompt_listener_address(const cai_cli_prompt_listener *p) {
  return p != NULL ? cai_net_local(p->listener) : "";
}
int cai_cli_prompt_listener_draft(const cai_cli_prompt_listener *p) {
  return p != NULL && p->peer != NULL && !p->separate;
}
int cai_cli_prompt_listener_active(const cai_cli_prompt_listener *p) {
  return p != NULL && p->peer != NULL;
}
int cai_cli_prompt_listener_submission(cai_cli_prompt_listener *p,
                                       cai_cli_prompt_scope *scope) {
  if (p == NULL || (!p->awaiting_submit && !p->offering))
    return 0;
  *scope = p->original;
  /* A synchronous queue OFFER still borrows queued text until its callback
   * returns. Deferred sl_submit owns its copied editor text independently. */
  if (!p->offering)
    finish(p, 0);
  return 1;
}
void cai_cli_prompt_listener_idle(cai_cli_prompt_listener *p) {
  if (p != NULL && p->awaiting_submit) {
    if (p->submit_idle++)
      reject(p, "TCP prompt handoff was not accepted by the native queue.");
    else {
      struct itimerspec timer;
      /* The first idle precedes Softline's callback submit. Wake the next
       * iteration to release a failed handoff even if all other input is idle.
       */
      memset(&timer, 0, sizeof(timer));
      timer.it_value.tv_nsec = 1L;
      (void)timerfd_settime(p->timer, 0, &timer, NULL);
    }
  }
}
void cai_cli_prompt_listener_cancel(cai_cli_prompt_listener *p) {
  if (p != NULL && p->peer != NULL)
    reject(p, "TCP prompt receipt cancelled; partial input discarded.");
}
void cai_cli_prompt_listener_close(cai_cli_prompt_listener *p) {
  if (p != NULL) {
    if (p->timer >= 0)
      finish(p, 1);
    (void)cai_cli_prompt_listener_watch(p, NULL, NULL);
    cai_net_close(p->listener, 0);
    if (p->timer >= 0)
      close(p->timer);
    free(p);
  }
}
