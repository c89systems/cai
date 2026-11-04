#define _POSIX_C_SOURCE 200809L
#include "transcript_feed.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PEERS CAI_ATTACHMENT_LIMIT
#define FRAMES 32U
#define WINDOW 32U
typedef struct ring {
  cai_cli_transcript_frame frames[FRAMES];
  size_t start, count;
} ring;
typedef struct subscriber {
  uint64_t attachment, stream, token, after, sequence, watermark, progress;
  char expected[21], *cursor;
  ring *history, *live;
  int pending, prepared, active, replaying, error;
  unsigned int limit;
  cai_transcript_failure failure;
  int code;
  char reason[192];
} subscriber;
struct cai_cli_transcript_publisher {
  cai_cli_pouch *pouch;
  cai_cli_pouch_transcript *reader;
  lc_collection *prompt_reader;
  unsigned int prompt_limit;
  char session[129];
  char workspace[PATH_MAX];
  uint64_t conversation;
  pthread_mutex_t lock;
  pthread_cond_t condition;
  pthread_t worker;
  int descriptors[2], stopping;
  size_t next;
  subscriber peers[2U * PEERS];
};
typedef struct job {
  cai_cli_transcript_publisher *feed;
  subscriber *peer;
  uint64_t attachment, token, stream, after, sequence, watermark, offset,
      progress;
  char *cursor;
  int prepared;
  int prompts;
  unsigned int limit;
} job;
static void wake(cai_cli_transcript_publisher *f) {
  char byte = 't';
  while (write(f->descriptors[1], &byte, 1U) < 0 && errno == EINTR) {
  }
}
static void drain(cai_cli_transcript_publisher *f) {
  char bytes[64];
  ssize_t n;
  do {
    n = read(f->descriptors[0], bytes, sizeof(bytes));
  } while (n > 0 || (n < 0 && errno == EINTR));
}
static subscriber *find_lane(cai_cli_transcript_publisher *f,
                             uint64_t attachment, int prompts) {
  size_t i;
  for (i = prompts ? PEERS : 0U; i < (prompts ? 2U * PEERS : PEERS); ++i)
    if (f->peers[i].attachment == attachment)
      return &f->peers[i];
  return NULL;
}
static subscriber *find(cai_cli_transcript_publisher *f, uint64_t attachment) {
  return find_lane(f, attachment, 0);
}
void cai_cli_transcript_frame_borrow(cai_cli_transcript_frame *frame) {
  frame->event.type = frame->type;
  frame->event.session = frame->session;
  frame->event.collection = frame->collection;
  frame->event.reason = frame->reason;
  frame->event.data = frame->data;
}
static void frame_copy(cai_cli_transcript_frame *out,
                       const cai_transcript_event *e) {
  memset(out, 0, sizeof(*out));
  out->event = *e;
#define COPY(member)                                                           \
  if (e->member != NULL)                                                       \
  snprintf(out->member, sizeof(out->member), "%s", e->member)
  COPY(type);
  COPY(session);
  COPY(collection);
  COPY(reason);
#undef COPY
  if (e->length != 0U)
    memcpy(out->data, e->data, e->length);
}
static void push(ring *r, const cai_transcript_event *e) {
  frame_copy(&r->frames[(r->start + r->count) % FRAMES], e);
  ++r->count;
}
static void failure(cai_cli_transcript_publisher *f, subscriber *s,
                    cai_transcript_failure why, int code, const char *message) {
  s->active = s->pending = 0;
  s->error = 1;
  if (s->token != UINT64_MAX)
    ++s->token;
  s->failure = why;
  s->code = code;
  snprintf(s->reason, sizeof(s->reason), "%s", message);
  if (s->history != NULL)
    s->history->count = s->history->start = 0U;
  if (s->live != NULL)
    s->live->count = s->live->start = 0U;
  free(s->cursor);
  s->cursor = NULL;
  pthread_cond_broadcast(&f->condition);
  wake(f);
}
static int current(const job *j) {
  return !j->feed->stopping && j->peer->active &&
         j->peer->attachment == j->attachment && j->peer->token == j->token &&
         j->peer->stream == j->stream;
}
static int cancelled(void *context) {
  job *j = context;
  int stop;
  pthread_mutex_lock(&j->feed->lock);
  stop = !current(j);
  pthread_mutex_unlock(&j->feed->lock);
  return stop;
}
static int add(job *j, cai_transcript_event *e, cai_error *error) {
  cai_cli_transcript_publisher *f = j->feed;
  subscriber *s = j->peer;
  struct timespec deadline;
  int rc = 0;
  (void)error;
  if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0)
    return CAI_ERR_TRANSPORT;
  ++deadline.tv_sec;
  pthread_mutex_lock(&f->lock);
  while (current(j) && s->history->count == FRAMES && rc == 0)
    rc = pthread_cond_timedwait(&f->condition, &f->lock, &deadline);
  if (current(j) && rc != 0)
    failure(f, s, CAI_TRANSCRIPT_SLOW, CAI_ERR_LIMIT,
            "Transcript subscriber is not draining output; resynchronize");
  if (current(j)) {
    e->stream = j->stream;
    e->conversation = f->conversation;
    push(s->history, e);
    wake(f);
  } else
    rc = -1;
  pthread_mutex_unlock(&f->lock);
  return rc == 0 ? CAI_OK : CAI_ERR_CANCELLED;
}
static int read_event(void *context, const cai_cli_transcript_event *source,
                      cai_error *error) {
  job *j = context;
  cai_transcript_event e;
  memset(&e, 0, sizeof(e));
  e.sequence = source->sequence;
  e.timestamp_ns = source->timestamp_ns;
  e.utc_offset_seconds = source->utc_offset_seconds;
  if (source->kind == CAI_CLI_TRANSCRIPT_BEGIN) {
    if (source->sequence <= j->sequence)
      return cai_cli_error(
          error, CAI_ERR_PROTOCOL,
          "transcript cursor sequence does not match position");
    j->offset = 0U;
    e.kind = CAI_TRANSCRIPT_EVENT_BEGIN;
    e.type = source->type;
    e.has_data = source->has_data;
  } else if (source->kind == CAI_CLI_TRANSCRIPT_DATA) {
    if (source->length == 0U)
      return CAI_OK;
    if (source->length > CAI_CLI_TRANSCRIPT_CHUNK ||
        source->length > UINT64_MAX - j->offset)
      return CAI_ERR_LIMIT;
    e.kind = CAI_TRANSCRIPT_DATA;
    e.offset = j->offset;
    e.data = source->data;
    e.length = source->length;
    j->offset += source->length;
  } else {
    e.kind = CAI_TRANSCRIPT_EVENT_END;
    e.position = source->position;
    e.offset = j->offset;
    j->sequence = source->sequence;
  }
  return add(j, &e, error);
}
static void observe(void *context, const char *session,
                    unsigned long long position,
                    const cai_agent_session_event *source) {
  cai_cli_transcript_publisher *f = context;
  size_t i, length = 0U;
  int have_length = 0;
  if (strcmp(session, f->session) != 0)
    return;
  pthread_mutex_lock(&f->lock);
  for (i = 0U; i < PEERS; ++i) {
    subscriber *s = &f->peers[i];
    cai_transcript_event e;
    size_t offset = 0U;
    if (!s->active)
      continue;
    if (source == NULL) {
      failure(f, s, CAI_TRANSCRIPT_HISTORY_CHANGED, CAI_ERR_INVALID,
              "Conversation history changed; resynchronize");
      continue;
    }
    if (!s->prepared || position <= s->watermark)
      continue;
    if (!have_length) {
      length = source->data != NULL ? strlen(source->data) : 0U;
      have_length = 1;
    }
    memset(&e, 0, sizeof(e));
    e.stream = s->stream;
    e.conversation = f->conversation;
    e.sequence = source->sequence;
    e.timestamp_ns = source->timestamp_ns;
    e.utc_offset_seconds = source->utc_offset_seconds;
    e.kind = CAI_TRANSCRIPT_EVENT_BEGIN;
    e.type = source->type;
    e.has_data = source->data != NULL;
    if (source->type == NULL || strlen(source->type) > 128U ||
        s->live->count == FRAMES)
      goto overflow;
    push(s->live, &e);
    while (offset < length) {
      size_t n = length - offset;
      if (n > CAI_CLI_TRANSCRIPT_CHUNK)
        n = CAI_CLI_TRANSCRIPT_CHUNK;
      if (s->live->count == FRAMES)
        goto overflow;
      e.kind = CAI_TRANSCRIPT_DATA;
      e.offset = offset;
      e.data = source->data + offset;
      e.length = n;
      push(s->live, &e);
      offset += n;
    }
    if (s->live->count == FRAMES)
      goto overflow;
    e.kind = CAI_TRANSCRIPT_EVENT_END;
    e.position = position;
    e.offset = offset;
    e.data = NULL;
    e.length = 0U;
    push(s->live, &e);
    wake(f);
    continue;
  overflow:
    failure(f, s, CAI_TRANSCRIPT_SLOW, CAI_ERR_LIMIT,
            "Transcript subscriber buffer is full; resynchronize");
  }
  pthread_mutex_unlock(&f->lock);
}
static int prepare(job *j, cai_error *error) {
  cai_cli_transcript_publisher *f = j->feed;
  lc_collection_head_res head;
  lc_error detail;
  cai_transcript_event e;
  int rc;
  lc_error_init(&detail);
  /* Lock order is journal -> feed, as in observe. Capturing W and installing
   * live delivery happen at one append seam. Read callbacks hold neither. */
  pthread_mutex_lock(&f->pouch->journal_lock);
  rc = f->reader->collection->head(f->reader->collection, &head, &detail);
  if (rc != LC_OK)
    rc = cai_cli_lc_error(error, &detail);
  pthread_mutex_lock(&f->lock);
  if (rc == CAI_OK && current(j)) {
    if (j->after > head.upper_position ||
        (j->peer->expected[0] != '\0' &&
         strcmp(j->peer->expected, head.collection_id) != 0))
      rc = cai_cli_error(error, CAI_ERR_INVALID,
                         "Transcript collection or cursor no longer matches");
    else {
      j->watermark = j->peer->watermark = head.upper_position;
      j->peer->prepared = 1;
    }
  } else if (!current(j))
    rc = CAI_ERR_CANCELLED;
  pthread_mutex_unlock(&f->lock);
  pthread_mutex_unlock(&f->pouch->journal_lock);
  lc_error_cleanup(&detail);
  if (rc != CAI_OK)
    return rc;
  memset(&e, 0, sizeof(e));
  e.kind = CAI_TRANSCRIPT_REPLAY_BEGIN;
  e.session = f->session;
  e.collection = head.collection_id;
  e.watermark = head.upper_position;
  e.visibility = head.recovery_revision;
  return add(j, &e, error);
}
static int prompt_event(void *context, uint64_t position, const char *prompt,
                        cai_error *error) {
  job *j = context;
  cai_transcript_event e;
  size_t offset = 0U, length = strlen(prompt);
  int rc;
  memset(&e, 0, sizeof(e));
  e.kind = CAI_TRANSCRIPT_EVENT_BEGIN;
  e.type = "prompt_history";
  e.sequence = position;
  e.has_data = 1;
  rc = add(j, &e, error);
  while (rc == CAI_OK && offset < length) {
    size_t n = length - offset;
    if (n > CAI_CLI_TRANSCRIPT_CHUNK)
      n = CAI_CLI_TRANSCRIPT_CHUNK;
    e.kind = CAI_TRANSCRIPT_DATA;
    e.offset = offset;
    e.data = prompt + offset;
    e.length = n;
    rc = add(j, &e, error);
    offset += n;
  }
  if (rc == CAI_OK) {
    e.kind = CAI_TRANSCRIPT_EVENT_END;
    e.position = position;
    e.offset = offset;
    e.data = NULL;
    e.length = 0U;
    j->sequence = position;
    rc = add(j, &e, error);
  }
  return rc;
}
static int prepare_prompts(job *j, cai_error *error) {
  cai_cli_transcript_publisher *f = j->feed;
  lc_collection_info info;
  lc_error local;
  cai_transcript_event begin;
  int rc = CAI_OK;
  memset(&info, 0, sizeof(info));
  if (j->limit != 0U && f->prompt_reader == NULL)
    rc = cai_cli_pouch_prompt_reader(f->pouch, f->session, &f->prompt_reader,
                                     error);
  lc_error_init(&local);
  if (rc == CAI_OK && j->limit != 0U && f->prompt_reader != NULL &&
      f->prompt_reader->describe(f->prompt_reader, &info, &local) != LC_OK)
    rc = cai_cli_lc_error(error, &local);
  lc_error_cleanup(&local);
  if (rc != CAI_OK)
    return rc;
  j->watermark = info.upper_position;
  j->after = j->progress =
      info.upper_position > j->limit ? info.upper_position - j->limit : 0U;
  memset(&begin, 0, sizeof(begin));
  begin.kind = CAI_TRANSCRIPT_REPLAY_BEGIN;
  begin.session = f->session;
  begin.collection = info.collection_id;
  begin.watermark = j->watermark;
  begin.visibility = j->limit;
  return add(j, &begin, error);
}
static void *worker(void *context) {
  cai_cli_transcript_publisher *f = context;
  for (;;) {
    job j;
    cai_error error;
    lc_collection_read_opts options;
    lc_collection_read_res result;
    size_t i;
    int rc, found = 0;
    memset(&j, 0, sizeof(j));
    j.feed = f;
    pthread_mutex_lock(&f->lock);
    while (!f->stopping && !found) {
      for (i = 0U; i < 2U * PEERS; ++i) {
        size_t index = (f->next + i) % (2U * PEERS);
        subscriber *s = &f->peers[index];
        if (!s->active || !s->pending)
          continue;
        j.peer = s;
        j.attachment = s->attachment;
        j.token = s->token;
        j.stream = s->stream;
        j.after = s->after;
        j.sequence = s->sequence;
        j.watermark = s->watermark;
        j.progress = s->progress;
        j.prepared = s->prepared;
        j.prompts = index >= PEERS;
        j.limit = s->limit;
        j.cursor = s->cursor;
        s->cursor = NULL;
        s->pending = 0;
        f->next = (index + 1U) % (2U * PEERS);
        found = 1;
        break;
      }
      if (!found)
        pthread_cond_wait(&f->condition, &f->lock);
    }
    if (f->stopping) {
      pthread_mutex_unlock(&f->lock);
      break;
    }
    pthread_mutex_unlock(&f->lock);
    cai_error_init(&error);
    lc_collection_read_opts_init(&options);
    lc_collection_read_res_init(&result);
    rc = !j.prompts && f->reader == NULL
             ? cai_cli_pouch_transcript_open(f->pouch, f->workspace, f->session,
                                             &f->reader, &error)
             : CAI_OK;
    if (rc == CAI_OK && !j.prepared)
      rc = j.prompts ? prepare_prompts(&j, &error) : prepare(&j, &error);
    options.has_after = options.has_through = options.has_first_n = 1;
    options.after = j.after;
    options.through = j.watermark;
    options.first_n = WINDOW;
    options.cursor = j.cursor;
    options.cancelled = cancelled;
    options.cancel_context = &j;
    if (rc == CAI_OK && j.prompts && j.after >= j.watermark)
      result.last_examined_position = j.watermark;
    else if (rc == CAI_OK)
      rc = j.prompts
               ? cai_cli_pouch_prompt_scan(f->prompt_reader, &options, &result,
                                           prompt_event, &j, &error)
               : cai_cli_pouch_transcript_read(f->reader, &options, &result,
                                               read_event, &j, &error);
    if (rc == CAI_OK && result.last_examined_position < j.watermark &&
        (result.last_examined_position <= j.progress ||
         result.checkpoint_cursor == NULL))
      rc = cai_cli_error(&error, CAI_ERR_PROTOCOL,
                         "Transcript read made no resumable progress");
    if (rc == CAI_OK && result.last_examined_position >= j.watermark) {
      cai_transcript_event end;
      memset(&end, 0, sizeof(end));
      end.kind = CAI_TRANSCRIPT_REPLAY_END;
      end.watermark = j.watermark;
      rc = add(&j, &end, &error);
    }
    pthread_mutex_lock(&f->lock);
    if (current(&j)) {
      subscriber *s = j.peer;
      if (rc != CAI_OK)
        failure(f, s, CAI_TRANSCRIPT_FAILED, rc,
                error.message != NULL ? error.message
                                      : "Transcript read failed");
      else if (result.last_examined_position >= j.watermark) {
        s->replaying = 0;
        wake(f);
      } else {
        s->prepared = 1;
        s->watermark = j.watermark;
        s->after = j.after;
        s->cursor = result.checkpoint_cursor;
        result.checkpoint_cursor = NULL;
        s->sequence = j.sequence;
        s->progress = result.last_examined_position;
        s->pending = 1;
      }
    }
    pthread_mutex_unlock(&f->lock);
    free(j.cursor);
    lc_collection_read_res_cleanup(&result);
    cai_error_cleanup(&error);
  }
  cai_cli_pouch_transcript_close(f->reader);
  lc_collection_close(f->prompt_reader);
  f->reader = NULL;
  return NULL;
}
int cai_cli_transcript_feed_open(cai_cli_pouch *pouch, const char *workspace,
                                 const char *session, uint64_t conversation,
                                 cai_cli_transcript_publisher **out,
                                 cai_error *error) {
  cai_cli_transcript_publisher *f;
  pthread_condattr_t attr;
  int rc, i, result = CAI_ERR_TRANSPORT;
  if (out != NULL)
    *out = NULL;
  if (pouch == NULL || workspace == NULL || strlen(workspace) >= PATH_MAX ||
      session == NULL || session[0] == '\0' || strlen(session) > 128U ||
      conversation == 0U || out == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID, "Transcript context required");
  f = calloc(1U, sizeof(*f));
  if (f == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "Allocate transcript feed");
  f->pouch = pouch;
  f->conversation = conversation;
  strcpy(f->session, session);
  strcpy(f->workspace, workspace);
  if (pipe(f->descriptors) != 0) {
    free(f);
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "Open transcript wakeup");
  }
  for (i = 0; i < 2; ++i)
    if (fcntl(f->descriptors[i], F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(f->descriptors[i], F_SETFL, O_NONBLOCK) != 0)
      goto fail_pipe;
  if (pthread_mutex_init(&f->lock, NULL) != 0)
    goto fail_pipe;
  rc = pthread_condattr_init(&attr);
  if (rc != 0)
    goto fail_mutex;
  rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
  if (rc == 0)
    rc = pthread_cond_init(&f->condition, &attr);
  pthread_condattr_destroy(&attr);
  if (rc != 0)
    goto fail_mutex;
  pthread_mutex_lock(&pouch->journal_lock);
  if (pouch->journal_observer != NULL) {
    pthread_mutex_unlock(&pouch->journal_lock);
    result = CAI_ERR_INVALID;
    goto fail_condition;
  }
  pouch->journal_observer = observe;
  pouch->journal_observer_context = f;
  pthread_mutex_unlock(&pouch->journal_lock);
  if (pthread_create(&f->worker, NULL, worker, f) != 0) {
    pthread_mutex_lock(&pouch->journal_lock);
    pouch->journal_observer = NULL;
    pouch->journal_observer_context = NULL;
    pthread_mutex_unlock(&pouch->journal_lock);
    goto fail_condition;
  }
  *out = f;
  return CAI_OK;
fail_condition:
  pthread_cond_destroy(&f->condition);
fail_mutex:
  pthread_mutex_destroy(&f->lock);
fail_pipe:
  close(f->descriptors[0]);
  close(f->descriptors[1]);
  free(f);
  return cai_cli_error(error, result,
                       result == CAI_ERR_INVALID
                           ? "Transcript publisher already bound"
                           : "Initialize transcript feed");
}
int cai_cli_transcript_feed_request(cai_cli_transcript_publisher *f,
                                    uint64_t attachment, uint64_t stream,
                                    const char *collection, uint64_t after,
                                    uint64_t sequence, cai_error *error) {
  subscriber *s;
  size_t i;
  if (f == NULL || attachment == 0U || stream == 0U ||
      (collection != NULL && strlen(collection) > 20U) ||
      ((after != 0U || sequence != 0U) &&
       (collection == NULL || collection[0] == '\0')) ||
      (after == 0U && sequence != 0U))
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "Verified transcript cursor required");
  pthread_mutex_lock(&f->lock);
  s = find(f, attachment);
  if (s == NULL)
    for (i = 0U; i < PEERS; ++i)
      if (f->peers[i].attachment == 0U) {
        s = &f->peers[i];
        break;
      }
  if (s == NULL || f->stopping || s->token == UINT64_MAX) {
    pthread_mutex_unlock(&f->lock);
    return cai_cli_error(error, CAI_ERR_LIMIT,
                         "Transcript subscriber limit reached");
  }
  if (s->history == NULL)
    s->history = calloc(1U, sizeof(*s->history));
  if (s->live == NULL)
    s->live = calloc(1U, sizeof(*s->live));
  if (s->history == NULL || s->live == NULL) {
    free(s->history);
    free(s->live);
    s->history = s->live = NULL;
    pthread_mutex_unlock(&f->lock);
    return cai_cli_error(error, CAI_ERR_NOMEM,
                         "Allocate bounded subscriber queues");
  }
  ++s->token;
  s->attachment = attachment;
  s->stream = stream;
  s->after = after;
  s->progress = after;
  s->sequence = sequence;
  s->active = s->pending = s->replaying = 1;
  s->prepared = s->error = 0;
  s->history->count = s->history->start = s->live->count = s->live->start = 0U;
  snprintf(s->expected, sizeof(s->expected), "%s",
           collection != NULL ? collection : "");
  free(s->cursor);
  s->cursor = NULL;
  pthread_cond_broadcast(&f->condition);
  pthread_mutex_unlock(&f->lock);
  return CAI_OK;
}
void cai_cli_transcript_feed_cancel(cai_cli_transcript_publisher *f,
                                    uint64_t attachment, uint64_t stream) {
  subscriber *s;
  if (f == NULL)
    return;
  pthread_mutex_lock(&f->lock);
  s = find(f, attachment);
  if (s != NULL && s->stream == stream)
    failure(f, s, CAI_TRANSCRIPT_CONTEXT_CLOSED, CAI_ERR_CANCELLED,
            "Transcript subscription ended");
  pthread_mutex_unlock(&f->lock);
}
void cai_cli_transcript_feed_detach(cai_cli_transcript_publisher *f,
                                    uint64_t attachment) {
  subscriber *s;
  if (f == NULL)
    return;
  pthread_mutex_lock(&f->lock);
  s = find(f, attachment);
  if (s != NULL) {
    uint64_t token = s->token;
    free(s->history);
    free(s->live);
    free(s->cursor);
    memset(s, 0, sizeof(*s));
    s->token = token;
    pthread_cond_broadcast(&f->condition);
  }
  s = find_lane(f, attachment, 1);
  if (s != NULL) {
    uint64_t token = s->token;
    free(s->history);
    free(s->live);
    free(s->cursor);
    memset(s, 0, sizeof(*s));
    s->token = token;
    pthread_cond_broadcast(&f->condition);
  }
  pthread_mutex_unlock(&f->lock);
}
int cai_cli_transcript_feed_fd(cai_cli_transcript_publisher *f) {
  return f != NULL ? f->descriptors[0] : -1;
}
void cai_cli_transcript_feed_drain(cai_cli_transcript_publisher *f) {
  if (f == NULL)
    return;
  pthread_mutex_lock(&f->lock);
  drain(f);
  pthread_mutex_unlock(&f->lock);
}
static int lane_ready(cai_cli_transcript_publisher *f, uint64_t attachment,
                      int prompts, int *failed) {
  subscriber *s;
  int ready = 0;
  *failed = 0;
  if (f == NULL)
    return 0;
  pthread_mutex_lock(&f->lock);
  s = find_lane(f, attachment, prompts);
  if (s != NULL) {
    *failed = s->error;
    ready = s->error || (s->history != NULL && s->history->count != 0U) ||
            (s->live != NULL && !s->replaying && s->live->count != 0U);
  }
  pthread_mutex_unlock(&f->lock);
  return ready;
}
static int lane_next(cai_cli_transcript_publisher *f, uint64_t attachment,
                     int prompts, cai_cli_transcript_frame *out) {
  subscriber *s;
  ring *r = NULL;
  int found = 0;
  if (f == NULL)
    return 0;
  pthread_mutex_lock(&f->lock);
  s = find_lane(f, attachment, prompts);
  if (s != NULL && s->error) {
    memset(out, 0, sizeof(*out));
    out->event.kind = CAI_TRANSCRIPT_ERROR;
    out->event.stream = s->stream;
    out->event.conversation = f->conversation;
    out->event.code = s->code;
    out->event.failure = s->failure;
    strcpy(out->reason, s->reason);
    s->error = 0;
    found = 1;
  } else if (s != NULL && s->history != NULL) {
    r = s->history->count != 0U ? s->history : !s->replaying ? s->live : NULL;
    if (r != NULL && r->count != 0U) {
      *out = r->frames[r->start];
      r->start = (r->start + 1U) % FRAMES;
      --r->count;
      found = 1;
      pthread_cond_broadcast(&f->condition);
    }
  }
  if (found)
    cai_cli_transcript_frame_borrow(out);
  pthread_mutex_unlock(&f->lock);
  return found;
}
void cai_cli_transcript_feed_close(cai_cli_transcript_publisher *f) {
  size_t i;
  if (f == NULL)
    return;
  pthread_mutex_lock(&f->pouch->journal_lock);
  if (f->pouch->journal_observer_context == f) {
    f->pouch->journal_observer = NULL;
    f->pouch->journal_observer_context = NULL;
  }
  pthread_mutex_unlock(&f->pouch->journal_lock);
  pthread_mutex_lock(&f->lock);
  f->stopping = 1;
  pthread_cond_broadcast(&f->condition);
  pthread_mutex_unlock(&f->lock);
  pthread_join(f->worker, NULL);
  for (i = 0U; i < 2U * PEERS; ++i) {
    free(f->peers[i].history);
    free(f->peers[i].live);
    free(f->peers[i].cursor);
  }
  close(f->descriptors[0]);
  close(f->descriptors[1]);
  pthread_cond_destroy(&f->condition);
  pthread_mutex_destroy(&f->lock);
  free(f);
}
int cai_cli_transcript_feed_ready(cai_cli_transcript_publisher *f, uint64_t a,
                                  int *failed) {
  return lane_ready(f, a, 0, failed);
}
int cai_cli_transcript_feed_next(cai_cli_transcript_publisher *f, uint64_t a,
                                 cai_cli_transcript_frame *frame) {
  return lane_next(f, a, 0, frame);
}
int cai_cli_transcript_feed_prompt_ready(cai_cli_transcript_publisher *f,
                                         uint64_t a, int *failed) {
  return lane_ready(f, a, 1, failed);
}
int cai_cli_transcript_feed_prompt_next(cai_cli_transcript_publisher *f,
                                        uint64_t a,
                                        cai_cli_transcript_frame *frame) {
  return lane_next(f, a, 1, frame);
}
void cai_cli_transcript_feed_prompt_limit(cai_cli_transcript_publisher *f,
                                          unsigned int limit) {
  if (f != NULL) {
    pthread_mutex_lock(&f->lock);
    f->prompt_limit = limit;
    pthread_mutex_unlock(&f->lock);
  }
}
int cai_cli_transcript_feed_prompts(cai_cli_transcript_publisher *f,
                                    uint64_t attachment, uint64_t stream,
                                    unsigned int limit, cai_error *error) {
  subscriber *s;
  size_t i;
  if (f == NULL || attachment == 0U || stream == 0U)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "prompt-history context required");
  pthread_mutex_lock(&f->lock);
  s = find_lane(f, attachment, 1);
  if (s == NULL)
    for (i = PEERS; i < 2U * PEERS; ++i)
      if (f->peers[i].attachment == 0U) {
        s = &f->peers[i];
        break;
      }
  if (s == NULL || f->stopping || s->token == UINT64_MAX) {
    pthread_mutex_unlock(&f->lock);
    return cai_cli_error(error, CAI_ERR_LIMIT,
                         "prompt-history subscriber limit reached");
  }
  if (s->history == NULL)
    s->history = calloc(1U, sizeof(*s->history));
  if (s->history == NULL) {
    pthread_mutex_unlock(&f->lock);
    return cai_cli_error(error, CAI_ERR_NOMEM,
                         "allocate prompt-history channel");
  }
  ++s->token;
  s->attachment = attachment;
  s->stream = stream;
  s->limit = limit != 0U && limit < f->prompt_limit ? limit : f->prompt_limit;
  s->after = s->sequence = s->progress = s->watermark = 0U;
  s->active = s->pending = s->replaying = 1;
  s->prepared = s->error = 0;
  s->history->count = s->history->start = 0U;
  free(s->cursor);
  s->cursor = NULL;
  pthread_cond_broadcast(&f->condition);
  pthread_mutex_unlock(&f->lock);
  return CAI_OK;
}
