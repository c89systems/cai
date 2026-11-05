#define _POSIX_C_SOURCE 200809L
#include "../cli/pouch.h"
#include "../cli/transcript_feed.h"
#include <assert.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

unsigned int cai_test_storage_open_count(void);
typedef struct gate {
  pthread_mutex_t lock;
  pthread_cond_t condition;
  int entered, released, written, finished;
} gate;
typedef struct observed {
  cai_cli_pouch_transcript *reader;
  size_t bytes, events, cancel_after;
  unsigned long long first, last, position;
  int cancelled, reject;
  gate *barrier;
} observed;
typedef struct reading {
  cai_cli_pouch_transcript *reader;
  lc_collection_read_opts options;
  lc_collection_read_res result;
  observed seen;
  gate *completion;
  cai_error error;
  int rc;
} reading;
typedef struct writing {
  cai_cli_pouch *pouch;
  gate *barrier;
} writing;
static int visit(void *context, const cai_cli_transcript_event *event,
                 cai_error *error) {
  observed *s = context;
  lc_collection_read_res other;
  lc_collection_read_res_init(&other);
  assert(cai_cli_pouch_transcript_read(s->reader, NULL, &other, visit, s,
                                       NULL) == CAI_ERR_INVALID);
  cai_cli_pouch_transcript_close(s->reader); /* Active callback cannot close. */
  lc_collection_read_res_cleanup(&other);
  if (event->kind == CAI_CLI_TRANSCRIPT_BEGIN) {
    s->bytes = 0U;
    if (s->first == 0U)
      s->first = event->sequence;
  } else if (event->kind == CAI_CLI_TRANSCRIPT_DATA) {
    s->bytes += event->length;
    if (s->cancel_after != 0U && s->bytes >= s->cancel_after) {
      s->cancelled = 1;
      if (s->reject)
        return cai_cli_error(error, CAI_ERR_CANCELLED,
                             "frontend stopped transcript");
    }
    if (s->barrier != NULL) {
      gate *b = s->barrier;
      pthread_mutex_lock(&b->lock);
      b->entered = 1;
      pthread_cond_broadcast(&b->condition);
      while (!b->released)
        pthread_cond_wait(&b->condition, &b->lock);
      pthread_mutex_unlock(&b->lock);
      s->barrier = NULL;
    }
  } else {
    ++s->events;
    s->last = event->sequence;
    s->position = event->position;
  }
  return CAI_OK;
}
static void append(cai_cli_pouch *pouch, unsigned long long sequence,
                   const char *text) {
  cai_agent_session_event event;
  cai_error error;
  memset(&event, 0, sizeof(event));
  event.type = "tool_result";
  event.sequence = sequence;
  event.timestamp_ns = 1793780000000000000ULL + sequence;
  event.utc_offset_seconds = 3600;
  event.data = text;
  cai_error_init(&error);
  assert(pouch->store.append_event(pouch, "/transcript", "stream", &event,
                                   &error) == CAI_OK);
  cai_error_cleanup(&error);
}
static size_t source_read(void *context, void *bytes, size_t capacity,
                          cai_error *error) {
  size_t *offset = context, count = 2U - *offset;
  (void)error;
  if (count > capacity)
    count = capacity;
  memcpy(bytes, "{}" + *offset, count);
  *offset += count;
  return count;
}
static void initialize(cai_cli_pouch *pouch) {
  cai_source_callbacks callbacks;
  cai_source *source = NULL;
  size_t offset = 0U;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.read = source_read;
  callbacks.context = &offset;
  assert(cai_source_from_callbacks(&callbacks, &source, NULL) == CAI_OK);
  assert(pouch->store.commit(pouch, "/transcript", "stream", source, 0U,
                             NULL) == CAI_OK);
  cai_source_close(source);
}
static void *read_worker(void *context) {
  reading *r = context;
  r->rc = cai_cli_pouch_transcript_read(r->reader, &r->options, &r->result,
                                        visit, &r->seen, &r->error);
  pthread_mutex_lock(&r->completion->lock);
  r->completion->finished = 1;
  pthread_cond_broadcast(&r->completion->condition);
  pthread_mutex_unlock(&r->completion->lock);
  return NULL;
}
static int unexpected_metadata(void *context,
                               const lc_collection_read_metadata *metadata,
                               lc_error *error) {
  (void)context;
  (void)metadata;
  (void)error;
  abort();
}
static void *write_worker(void *context) {
  writing *w = context;
  append(w->pouch, 3U, "new live event");
  pthread_mutex_lock(&w->barrier->lock);
  w->barrier->written = 1;
  pthread_cond_broadcast(&w->barrier->condition);
  pthread_mutex_unlock(&w->barrier->lock);
  return NULL;
}
static int cancelled(void *context) { return ((observed *)context)->cancelled; }
static void reset(reading *r) {
  lc_collection_read_res_cleanup(&r->result);
  lc_collection_read_res_init(&r->result);
  cai_error_cleanup(&r->error);
  cai_error_init(&r->error);
  memset(&r->seen, 0, sizeof(r->seen));
  r->seen.reader = r->reader;
}
static void publisher(cai_cli_pouch *pouch) {
  cai_cli_transcript_publisher *feed = NULL;
  cai_cli_transcript_publisher *duplicate = NULL;
  cai_cli_transcript_frame frame;
  struct pollfd fd;
  struct timespec now;
  uint64_t until, position = 0U, sequence = 0U;
  char collection[21] = "";
  size_t replay = 0U, events = 0U, live = 0U;
  unsigned int opens = cai_test_storage_open_count();
  int failed, slow = 0, appended = 0;
  unsigned long long i;
  for (i = 4U; i <= 42U; ++i)
    append(pouch, i, "window event");
  assert(cai_cli_transcript_feed_open(pouch, "/transcript", "stream", 1U, &feed,
                                      NULL) == CAI_OK);
  assert(cai_cli_transcript_feed_open(pouch, "/transcript", "stream", 1U,
                                      &duplicate, NULL) == CAI_ERR_INVALID &&
         duplicate == NULL);
  assert(cai_cli_transcript_feed_request(feed, 1U, 1U, NULL, 0U, 0U, -1LL,
                                         NULL) == CAI_OK);
  assert(cai_cli_transcript_feed_request(feed, 2U, 2U, NULL, 0U, 0U, -1LL,
                                         NULL) == CAI_OK);
  fd.fd = cai_cli_transcript_feed_fd(feed);
  fd.events = POLLIN;
  assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  until = (uint64_t)now.tv_sec + 10U;
  while (!slow || live == 0U) {
    cai_cli_transcript_feed_drain(feed);
    if (cai_cli_transcript_feed_ready(feed, 1U, &failed) && failed) {
      assert(cai_cli_transcript_feed_next(feed, 1U, &frame));
      assert(frame.event.kind == CAI_TRANSCRIPT_ERROR &&
             frame.event.failure == CAI_TRANSCRIPT_SLOW);
      slow = 1;
    }
    while (cai_cli_transcript_feed_next(feed, 2U, &frame)) {
      const cai_transcript_event *e = &frame.event;
      assert(e->kind != CAI_TRANSCRIPT_ERROR && e->stream == 2U);
      if (e->kind == CAI_TRANSCRIPT_REPLAY_BEGIN) {
        strcpy(collection, e->collection);
        if (!appended) {
          append(pouch, 43U, "live after replay");
          appended = 1;
        }
      } else if (e->kind == CAI_TRANSCRIPT_REPLAY_END) {
        ++replay;
        assert(events == 42U);
      } else if (e->kind == CAI_TRANSCRIPT_EVENT_END) {
        assert(e->sequence > sequence && e->position > position);
        sequence = e->sequence;
        position = e->position;
        if (replay != 0U) {
          assert(e->sequence == 43U);
          ++live;
        } else
          ++events;
      }
    }
    if (slow && live != 0U)
      break;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0 &&
           (uint64_t)now.tv_sec < until);
    assert(poll(&fd, 1U, 1000) >= 0);
  }
  assert(cai_cli_transcript_feed_request(feed, 1U, 3U, collection, position,
                                         sequence, -1LL, NULL) == CAI_OK);
  replay = 0U;
  while (replay == 0U) {
    cai_cli_transcript_feed_drain(feed);
    while (cai_cli_transcript_feed_next(feed, 1U, &frame)) {
      assert(frame.event.stream == 3U &&
             frame.event.kind != CAI_TRANSCRIPT_ERROR);
      if (frame.event.kind == CAI_TRANSCRIPT_REPLAY_END)
        ++replay;
      else
        assert(frame.event.kind == CAI_TRANSCRIPT_REPLAY_BEGIN);
    }
    if (replay == 0U)
      assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0 &&
             (uint64_t)now.tv_sec < until && poll(&fd, 1U, 1000) >= 0);
  }
  /* The real durable-cut hook invalidates all subscribed views. Exercising
   * its consumer contract here avoids inventing a model-context fixture. */
  {
    size_t received[2] = {0U, 0U}, bytes[2] = {0U, 0U};
    unsigned int peer;
    int extended = 0;
    char *large = malloc(192U * 1024U + 1U);
    assert(large != NULL);
    memset(large, 'b', 192U * 1024U);
    large[192U * 1024U] = '\0';
    /* Burst beyond both live rings, including one event larger than a ring.
     * Catch-up must preserve accepted prefixes and remain a live stream. */
    for (i = 44U; i <= 123U; ++i)
      append(pouch, i, "burst output");
    append(pouch, 124U, large);
    free(large);
    while (received[0] != 82U || received[1] != 82U) {
      cai_cli_transcript_feed_drain(feed);
      for (peer = 0U; peer < 2U; ++peer)
        while (cai_cli_transcript_feed_next(feed, peer + 1U, &frame)) {
          const cai_transcript_event *e = &frame.event;
          assert(e->kind == CAI_TRANSCRIPT_EVENT_BEGIN ||
                 e->kind == CAI_TRANSCRIPT_DATA ||
                 e->kind == CAI_TRANSCRIPT_EVENT_END);
          assert(e->sequence == 44U + received[peer]);
          if (e->kind == CAI_TRANSCRIPT_EVENT_BEGIN)
            bytes[peer] = 0U;
          else if (e->kind == CAI_TRANSCRIPT_DATA)
            bytes[peer] += e->length;
          else {
            if (e->sequence == 124U)
              assert(bytes[peer] == 192U * 1024U);
            ++received[peer];
            if (!extended && received[peer] >= 20U) {
              /* Extend the target while a native catch-up read is active. */
              append(pouch, 125U, "after captured catch-up window");
              extended = 1;
            }
          }
        }
      if (received[0] != 82U || received[1] != 82U)
        assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0 &&
               (uint64_t)now.tv_sec < until && poll(&fd, 1U, 1000) >= 0);
    }
  }
  pthread_mutex_lock(&pouch->journal_lock);
  pouch->journal_observer(pouch->journal_observer_context, "stream", 0U, NULL);
  pthread_mutex_unlock(&pouch->journal_lock);
  assert(cai_cli_transcript_feed_next(feed, 1U, &frame) &&
         frame.event.failure == CAI_TRANSCRIPT_HISTORY_CHANGED);
  assert(cai_cli_transcript_feed_next(feed, 2U, &frame) &&
         frame.event.failure == CAI_TRANSCRIPT_HISTORY_CHANGED);
  cai_cli_transcript_feed_drain(feed);
  assert(poll(&fd, 1U, 0) ==
         0); /* Idle is quiescent, including after errors. */
  /* Closing a context cancels a producer even when its history ring is full.
   * No drain, frontend acknowledgement or one-second timeout is required. */
  assert(cai_cli_transcript_feed_request(feed, 1U, 4U, NULL, 0U, 0U, -1LL,
                                         NULL) == CAI_OK);
  while (!cai_cli_transcript_feed_ready(feed, 1U, &failed))
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0 &&
           (uint64_t)now.tv_sec < until && poll(&fd, 1U, 1000) >= 0);
  cai_cli_transcript_feed_close(feed);
  assert(pouch->journal_observer == NULL &&
         cai_test_storage_open_count() == opens);
}
int main(int argc, char **argv) {
  cai_cli_pouch *pouch = calloc(1U, sizeof(*pouch));
  cai_cli_pouch_transcript *reader = NULL, *bad = NULL;
  reading r;
  writing w;
  gate b;
  pthread_t reader_thread, writer_thread;
  struct timespec deadline;
  char *text = malloc(2U * 1024U * 1024U + 1U), *cursor;
  unsigned long long first_position, large_position;
  int wrote_while_blocked;
  unsigned int opens;
  assert(argc == 2 && pouch != NULL && text != NULL);
  memset(&r, 0, sizeof(r));
  lc_collection_read_opts_init(&r.options);
  lc_collection_read_res_init(&r.result);
  cai_error_init(&r.error);
  assert(cai_cli_pouch_open_at(pouch, argv[1], NULL, NULL, NULL, &r.error) ==
         CAI_OK);
  initialize(pouch);
  append(pouch, 1U, "first event");
  memset(text, 'x', 2U * 1024U * 1024U);
  text[2U * 1024U * 1024U] = '\0';
  append(pouch, 2U, text);
  opens = cai_test_storage_open_count();
  assert(opens == 1U);
  assert(cai_cli_pouch_transcript_open(pouch, "/wrong", "stream", &bad, NULL) ==
             CAI_ERR_INVALID &&
         bad == NULL);
  assert(cai_cli_pouch_transcript_open(pouch, "/transcript", "stream", &reader,
                                       &r.error) == CAI_OK);
  r.reader = reader;
  r.seen.reader = reader;
  r.options.selector_lql = "record_type == 'event'";
  assert(cai_cli_pouch_transcript_read(reader, &r.options, &r.result, visit,
                                       &r.seen, NULL) == CAI_ERR_INVALID);
  r.options.selector_lql = NULL;
  r.options.on_metadata = unexpected_metadata;
  assert(cai_cli_pouch_transcript_read(reader, &r.options, &r.result, visit,
                                       &r.seen, NULL) == CAI_ERR_INVALID);
  r.options.on_metadata = NULL;
  r.options.has_first_n = 1;
  r.options.first_n = 2U; /* Initial commit plus the first event. */
  assert(cai_cli_pouch_transcript_read(reader, &r.options, &r.result, visit,
                                       &r.seen, &r.error) == CAI_OK);
  assert(r.seen.events == 1U && r.seen.last == 1U && r.result.complete &&
         r.result.checkpoint_cursor != NULL);
  first_position = r.seen.position;
  reset(&r);
  lc_collection_read_opts_init(&r.options);
  r.options.has_after = 1;
  r.options.after = first_position;
  /* Pausing a public read must not hold append authority. Its captured EOD
   * excludes the append made while that read is blocked in its consumer. */
  memset(&b, 0, sizeof(b));
  assert(pthread_mutex_init(&b.lock, NULL) == 0);
  assert(pthread_cond_init(&b.condition, NULL) == 0);
  r.seen.barrier = &b;
  r.completion = &b;
  assert(pthread_create(&reader_thread, NULL, read_worker, &r) == 0);
  pthread_mutex_lock(&b.lock);
  while (!b.entered && !b.finished)
    pthread_cond_wait(&b.condition, &b.lock);
  pthread_mutex_unlock(&b.lock);
  assert(b.entered);
  w.pouch = pouch;
  w.barrier = &b;
  assert(pthread_create(&writer_thread, NULL, write_worker, &w) == 0);
  assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
  deadline.tv_sec += 10;
  pthread_mutex_lock(&b.lock);
  while (!b.written &&
         pthread_cond_timedwait(&b.condition, &b.lock, &deadline) == 0) {
  }
  wrote_while_blocked = b.written;
  b.released = 1;
  pthread_cond_broadcast(&b.condition);
  pthread_mutex_unlock(&b.lock);
  assert(pthread_join(reader_thread, NULL) == 0);
  assert(pthread_join(writer_thread, NULL) == 0);
  assert(wrote_while_blocked && r.rc == CAI_OK && r.seen.events == 1U &&
         r.seen.last == 2U && r.seen.bytes == 2U * 1024U * 1024U);
  large_position = r.seen.position;
  assert(r.result.upper_position == large_position);
  cursor = strdup(r.result.checkpoint_cursor);
  assert(cursor != NULL);
  reset(&r);
  r.options.cursor = cursor;
  assert(cai_cli_pouch_transcript_read(reader, &r.options, &r.result, visit,
                                       &r.seen, &r.error) == CAI_OK);
  assert(r.seen.events == 1U && r.seen.first == 3U && r.seen.last == 3U);
  free(cursor);
  reset(&r);
  lc_collection_read_opts_init(&r.options);
  r.options.has_after = r.options.has_through = 1;
  r.options.after = first_position;
  r.options.through = large_position;
  r.seen.cancel_after = 8192U;
  r.seen.reject = 1;
  assert(cai_cli_pouch_transcript_read(reader, &r.options, &r.result, visit,
                                       &r.seen, &r.error) == CAI_ERR_CANCELLED);
  assert(r.seen.events == 0U && r.result.completed_records == 0U &&
         strstr(r.error.message, "frontend stopped") != NULL);
  reset(&r);
  r.seen.cancel_after = 8192U;
  r.options.cancelled = cancelled;
  r.options.cancel_context = &r.seen;
  assert(cai_cli_pouch_transcript_read(reader, &r.options, &r.result, visit,
                                       &r.seen, &r.error) == CAI_ERR_CANCELLED);
  assert(r.seen.events == 0U &&
         r.result.stop_reason == LC_COLLECTION_READ_CANCELLED);
  reset(&r);
  r.options.cancelled = NULL;
  r.options.cancel_context = NULL;
  assert(cai_cli_pouch_transcript_read(reader, &r.options, &r.result, visit,
                                       &r.seen, &r.error) == CAI_OK);
  assert(r.seen.events == 1U && r.seen.last == 2U &&
         cai_test_storage_open_count() == opens);
  publisher(pouch);
  {
    lc_collection_head_res head;
    lc_error local;
    lc_error_init(&local);
    assert(reader->collection->head(reader->collection, &head, &local) ==
           LC_OK);
    head.etag[0] = head.etag[0] == '0' ? '1' : '0';
    reset(&r);
    r.options.expected_head = head.etag;
    assert(cai_cli_pouch_transcript_read(reader, &r.options, &r.result, visit,
                                         &r.seen, &r.error) != CAI_OK);
    assert(r.seen.events == 0U && r.seen.bytes == 0U);
    lc_error_cleanup(&local);
  }
  {
    lc_collection_lease_req lease;
    lc_collection_config config;
    lc_collection *owned = NULL, *replacement = NULL;
    lc_error local;
    char original[LC_XID_STRING_SIZE];
    lc_error_init(&local);
    assert(strlen(reader->collection->collection_id) < sizeof(original));
    strcpy(original, reader->collection->collection_id);
    /* End writer ownership, retaining the Pouch and its public read receiver.
     * An explicit drop/recreate must not redirect that existing receiver. */
    cai_cli_pouch_end_use(pouch);
    lc_collection_lease_req_init(&lease);
    lease.owner = "transcript-replacement-test";
    assert(reader->collection->acquire(reader->collection, &lease, &owned,
                                       &local) == LC_OK);
    assert(owned->drop(owned, &local) == LC_OK);
    lc_collection_close(owned);
    lc_collection_config_init(&config);
    config.ns = pouch->sessions_namespace;
    config.name = "events.stream";
    assert(lc_client_create_collection(pouch->sessions, &config, &replacement,
                                       &local) == LC_OK);
    assert(strcmp(original, replacement->collection_id) != 0);
    reset(&r);
    lc_collection_read_opts_init(&r.options);
    assert(cai_cli_pouch_transcript_read(reader, &r.options, &r.result, visit,
                                         &r.seen, &r.error) != CAI_OK);
    assert(r.seen.events == 0U && r.seen.bytes == 0U &&
           cai_test_storage_open_count() == opens);
    lc_collection_close(replacement);
    lc_error_cleanup(&local);
  }
  cai_cli_pouch_transcript_close(reader);
  cai_cli_pouch_close(pouch);
  lc_collection_read_res_cleanup(&r.result);
  cai_error_cleanup(&r.error);
  pthread_cond_destroy(&b.condition);
  pthread_mutex_destroy(&b.lock);
  free(text);
  free(pouch);
  return 0;
}
