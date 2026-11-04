/** Included after journal primitives. Pending edits live in the conversation
 * journal; claimed model work uses an idempotent delivery receipt. A child
 * collection cannot be committed atomically with its parent's collection. */
static int cai_runtime_enqueue_input_origin(cai_agent_runtime *, const char *,
                                            cai_runtime_input_kind, int *,
                                            const char *, unsigned long long,
                                            const unsigned long long *, size_t,
                                            cai_error *);
static int cai_runtime_replay_journal_event(void *,
                                            const cai_agent_session_event *,
                                            cai_error *);

static int cai_runtime_own_input_events(cai_agent_runtime *runtime,
                                        cai_runtime_input_node *input,
                                        cai_error *error) {
  int rc = CAI_OK;
  while (rc == CAI_OK && input->input_event_cursor < input->input_event_count) {
    rc = cai_session_history_own_event(
        runtime->session, input->input_events[input->input_event_cursor],
        error);
    if (rc == CAI_OK)
      ++input->input_event_cursor;
  }
  return rc;
}

static void cai_runtime_dispatch_free(cai_runtime_input_dispatch *entry) {
  cai_free_mem(NULL, entry->text);
  cai_free_mem(NULL, entry->target);
  cai_free_mem(NULL, entry->input_events);
  cai_free_mem(NULL, entry);
}
static void cai_runtime_queue_cleanup(cai_agent_runtime *runtime) {
  cai_runtime_input_dispatch *dispatch;
  cai_runtime_input_origin *origin;
  cai_input_checkpoint_close(runtime->input_checkpoint_reader);
  runtime->input_checkpoint_reader = NULL;
  while ((dispatch = runtime->input_dispatch) != NULL) {
    runtime->input_dispatch = dispatch->next;
    cai_runtime_dispatch_free(dispatch);
  }
  while ((dispatch = runtime->input_history_pending) != NULL) {
    runtime->input_history_pending = dispatch->next;
    cai_runtime_dispatch_free(dispatch);
  }
  while ((origin = runtime->input_origins) != NULL) {
    runtime->input_origins = origin->next;
    cai_free_mem(NULL, origin->session);
    cai_free_mem(NULL, origin);
  }
  cai_input_queue_close(runtime->input_queue);
  runtime->input_queue = NULL;
}
static cai_runtime_input_dispatch *
cai_runtime_dispatch_new(const char *text, const char *target,
                         cai_runtime_input_kind kind,
                         const unsigned long long *input_events,
                         size_t input_event_count, cai_error *error) {
  cai_runtime_input_dispatch *entry = cai_alloc(NULL, sizeof(*entry));
  if (entry != NULL) {
    memset(entry, 0, sizeof(*entry));
    entry->text = cai_strdup(NULL, text);
    entry->target = cai_strdup(NULL, target);
    entry->kind = kind;
    if (input_event_count != 0U && input_events != NULL &&
        input_event_count <= (size_t)-1 / sizeof(*input_events)) {
      entry->input_events =
          cai_alloc(NULL, input_event_count * sizeof(*input_events));
      if (entry->input_events != NULL) {
        memcpy(entry->input_events, input_events,
               input_event_count * sizeof(*input_events));
        entry->input_event_count = input_event_count;
      }
    }
    if (entry->text != NULL && entry->target != NULL &&
        (input_event_count == 0U || entry->input_events != NULL))
      return entry;
    cai_runtime_dispatch_free(entry);
  }
  cai_set_error(error, CAI_ERR_NOMEM, "prepare accepted input delivery");
  return NULL;
}
static void cai_runtime_dispatch_add_locked(cai_agent_runtime *runtime,
                                            cai_runtime_input_dispatch *entry) {
  cai_runtime_input_dispatch **at = &runtime->input_dispatch;
  while (*at != NULL)
    at = &(*at)->next;
  *at = entry;
}
static void cai_runtime_dispatch_remove_locked(cai_agent_runtime *runtime,
                                               unsigned long long sequence,
                                               int retain_history) {
  cai_runtime_input_dispatch **at;
  for (at = &runtime->input_dispatch; *at != NULL; at = &(*at)->next) {
    cai_runtime_input_dispatch *entry = *at;
    if (entry->sequence == sequence) {
      *at = entry->next;
      if (entry->pinned_child != NULL) {
        --runtime->child_input_users;
        pthread_cond_broadcast(&runtime->condition);
      }
      if (retain_history && entry->input_event_count != 0U) {
        entry->pinned_child = NULL;
        cai_free_mem(NULL, entry->text);
        cai_free_mem(NULL, entry->target);
        entry->text = entry->target = NULL;
        entry->next = runtime->input_history_pending;
        runtime->input_history_pending = entry;
      } else
        cai_runtime_dispatch_free(entry);
      return;
    }
  }
}
static int cai_runtime_queue_persist(void *context,
                                     const cai_input_record *record,
                                     unsigned long long *position,
                                     cai_error *error) {
  cai_agent_runtime *runtime = context;
  cai_runtime_input_dispatch *dispatch = NULL;
  cai_buffer_builder payload;
  char *json = NULL;
  char kind[40];
  int rc = cai_input_record_json(record, &json, error);
  memset(&payload, 0, sizeof(payload));
  if (rc != CAI_OK)
    return rc;
  pthread_mutex_lock(&runtime->lock);
  if (record->type == CAI_INPUT_RECORD_CLAIM && !record->entry.action) {
    cai_agent_runtime *child = NULL;
    cai_runtime_input_kind delivery = record->entry.intent == CAI_INPUT_STEERING
                                          ? CAI_RUNTIME_INPUT_INTERACTIVE
                                          : CAI_RUNTIME_INPUT_QUEUED_TURN;
    if (record->entry.intent == CAI_INPUT_STEERING &&
        !runtime->disable_subagent_steering && !runtime->child_finishing)
      child = runtime->active_subagent != NULL ? runtime->active_subagent
                                               : runtime->active_review;
    dispatch = cai_runtime_dispatch_new(
        record->entry.text,
        child != NULL ? child->session_id : runtime->session_id, delivery,
        record->input_events, record->input_event_count, error);
    if (dispatch == NULL)
      rc = CAI_ERR_NOMEM;
    else {
      dispatch->pinned_child = child;
      if (child != NULL)
        ++runtime->child_input_users;
      snprintf(kind, sizeof(kind), "%d\n%lu\n", (int)delivery,
               (unsigned long)strlen(dispatch->target));
      rc = cai_buffer_append(&payload, kind, strlen(kind), error);
      if (rc == CAI_OK)
        rc = cai_buffer_append(&payload, dispatch->target,
                               strlen(dispatch->target), error);
      if (rc == CAI_OK)
        rc = cai_buffer_append(&payload, "\n", 1U, error);
      if (rc == CAI_OK)
        rc = cai_buffer_append(&payload, json, strlen(json), error);
    }
  }
  if (rc == CAI_OK)
    rc = cai_runtime_append_journal_event_locked(
        runtime, dispatch != NULL ? "input_queue_claim" : "input_queue",
        dispatch != NULL ? payload.data : json, position, error);
  if (rc == CAI_OK && dispatch != NULL) {
    dispatch->sequence = *position != 0U ? *position : record->after_revision;
    cai_runtime_dispatch_add_locked(runtime, dispatch);
    cai_runtime_wakeup_notify_locked(runtime);
  } else if (dispatch != NULL) {
    if (dispatch->pinned_child != NULL) {
      --runtime->child_input_users;
      pthread_cond_broadcast(&runtime->condition);
    }
    cai_runtime_dispatch_free(dispatch);
  }
  pthread_mutex_unlock(&runtime->lock);
  cai_free_mem(NULL, payload.data);
  cai_free_mem(NULL, json);
  return rc;
}
static int cai_runtime_queue_ensure(cai_agent_runtime *runtime,
                                    cai_error *error) {
  cai_input_queue_config config;
  if (runtime->input_queue != NULL)
    return CAI_OK;
  cai_input_queue_config_init(&config);
  config.persist = cai_runtime_queue_persist;
  config.persist_context = runtime;
  return cai_input_queue_open(&config, &runtime->input_queue, error);
}
static int cai_runtime_input_checkpoint_source(void *context, cai_source **out,
                                               cai_error *error) {
  cai_agent_runtime *runtime = context;
  *out = NULL;
  if (runtime->input_queue == NULL)
    return CAI_OK;
  return cai_input_checkpoint_source(runtime->input_queue,
                                     runtime->input_checkpoint_through != 0U
                                         ? runtime->input_checkpoint_through
                                         : runtime->next_event_sequence,
                                     out, error);
}
static int cai_runtime_input_checkpoint_chunk(void *context, const void *bytes,
                                              size_t count, int final,
                                              cai_error *error) {
  cai_agent_runtime *runtime = context;
  cai_input_queue_config config;
  cai_input_queue *restored = NULL;
  unsigned long long through;
  int rc;
  if (runtime->input_checkpoint_reader == NULL) {
    cai_input_queue_config_init(&config);
    config.persist = cai_runtime_queue_persist;
    config.persist_context = runtime;
    rc = cai_input_checkpoint_begin(&config, &runtime->input_checkpoint_reader,
                                    error);
    if (rc != CAI_OK)
      return rc;
  }
  if (!final)
    return cai_input_checkpoint_feed(runtime->input_checkpoint_reader, bytes,
                                     count, error);
  rc = cai_input_checkpoint_finish(runtime->input_checkpoint_reader, &restored,
                                   &through, error);
  if (rc == CAI_OK && through < runtime->input_checkpoint_after) {
    cai_input_queue_close(restored);
    rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                       "input checkpoint boundary moved backwards");
  }
  if (rc == CAI_OK) {
    cai_input_queue_close(runtime->input_queue);
    runtime->input_queue = restored;
    runtime->input_checkpoint_after = through;
    if (runtime->next_event_sequence < through)
      runtime->next_event_sequence = through;
  }
  cai_input_checkpoint_close(runtime->input_checkpoint_reader);
  runtime->input_checkpoint_reader = NULL;
  return rc;
}

/** These receipts remain for the lifetime of the destination conversation.
 * They prevent a lost parent acknowledgement from duplicating child input. */
static int cai_runtime_origin_seen_locked(cai_agent_runtime *runtime,
                                          const char *session,
                                          unsigned long long sequence) {
  cai_runtime_input_origin *at;
  for (at = runtime->input_origins; at != NULL; at = at->next)
    if (at->sequence == sequence && strcmp(at->session, session) == 0)
      return 1;
  return 0;
}
static cai_runtime_input_origin *
cai_runtime_origin_new(const char *session, unsigned long long sequence,
                       cai_error *error) {
  cai_runtime_input_origin *origin = cai_alloc(NULL, sizeof(*origin));
  if (origin != NULL) {
    memset(origin, 0, sizeof(*origin));
    origin->session = cai_strdup(NULL, session);
    origin->sequence = sequence;
    if (origin->session != NULL)
      return origin;
    cai_free_mem(NULL, origin);
  }
  cai_set_error(error, CAI_ERR_NOMEM, "prepare input delivery receipt");
  return NULL;
}
static int cai_runtime_delivery_data(const char *session,
                                     unsigned long long sequence,
                                     cai_runtime_input_kind kind, int local,
                                     const char *text, char **out,
                                     cai_error *error) {
  cai_buffer_builder buffer;
  char header[192];
  int n, rc;
  memset(&buffer, 0, sizeof(buffer));
  *out = NULL;
  n = snprintf(header, sizeof(header), "%c\n%lu\n%s\n%llu\n%d\n",
               local ? 'L' : 'C', (unsigned long)strlen(session), session,
               sequence, (int)kind);
  if (n < 0 || (size_t)n >= sizeof(header))
    return cai_set_error(error, CAI_ERR_INVALID,
                         "input delivery identity exceeds limit");
  rc = cai_buffer_append(&buffer, header, (size_t)n, error);
  if (rc == CAI_OK)
    rc = cai_buffer_append(&buffer, text, strlen(text), error);
  if (rc == CAI_OK)
    *out = buffer.data;
  else
    cai_free_mem(NULL, buffer.data);
  return rc;
}
static int cai_runtime_queue_replay(cai_agent_runtime *runtime,
                                    const cai_agent_session_event *event,
                                    cai_error *error) {
  cai_input_record record;
  cai_runtime_input_dispatch *dispatch = NULL;
  const char *json = event->data, *end;
  unsigned long long kind, sequence;
  char target[CAI_AGENT_SESSION_ID_MAX];
  int rc;
  if (strcmp(event->type, "input_queue_delivery") == 0) {
    cai_input_delivery delivery;
    cai_runtime_input_origin *origin;
    rc = cai_input_delivery_parse(event, &delivery, error);
    if (rc != CAI_OK)
      return rc;
    if (delivery.local) {
      cai_runtime_input_dispatch *entry;
      cai_runtime_input_node *input;
      /* A local delivery is its claim's acknowledgement in the same atomic
       * append. No separate marker or growing local receipt index is needed. */
      for (entry = runtime->input_dispatch; entry != NULL; entry = entry->next)
        if (entry->sequence == delivery.sequence)
          break;
      if (entry == NULL && event->sequence > runtime->input_checkpoint_after)
        return cai_set_error(error, CAI_ERR_PROTOCOL,
                             "model input delivery has no accepted claim");
      rc = cai_runtime_replay_journal_event(runtime, &delivery.event, error);
      if (rc == CAI_OK && entry != NULL) {
        input = strcmp(delivery.event.type, "steering_queued") == 0
                    ? runtime->steering_head
                    : runtime->turn_head;
        for (; input != NULL; input = input->next)
          if (input->journal_sequence == event->sequence) {
            input->input_events = entry->input_events;
            input->input_event_count = entry->input_event_count;
            entry->input_events = NULL;
            entry->input_event_count = 0U;
            break;
          }
        cai_runtime_dispatch_remove_locked(runtime, delivery.sequence, 0);
      }
      return rc;
    }
    if (cai_runtime_origin_seen_locked(runtime, delivery.source,
                                       delivery.sequence))
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "duplicate committed input delivery");
    origin = cai_runtime_origin_new(delivery.source, delivery.sequence, error);
    if (origin == NULL)
      return CAI_ERR_NOMEM;
    origin->next = runtime->input_origins;
    runtime->input_origins = origin;
    return cai_runtime_replay_journal_event(runtime, &delivery.event, error);
  }
  if (event->sequence <= runtime->input_checkpoint_after)
    return CAI_OK;
  if (strcmp(event->type, "input_queue_dispatched") == 0) {
    if (cai_parse_unsigned_decimal(event->data, &end, &sequence) != 0 ||
        *end != '\0' || sequence == 0U || sequence >= event->sequence)
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "invalid input delivery acknowledgement");
    cai_runtime_dispatch_remove_locked(
        runtime, sequence, event->sequence > runtime->applied_event_sequence);
    return CAI_OK;
  }
  if (json == NULL)
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "missing input queue transition");
  if (strcmp(event->type, "input_queue_claim") == 0) {
    size_t n;
    unsigned long long length;
    if (cai_parse_unsigned_decimal(json, &end, &kind) != 0 || *end != '\n' ||
        (kind != CAI_RUNTIME_INPUT_INTERACTIVE &&
         kind != CAI_RUNTIME_INPUT_QUEUED_TURN))
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "invalid input claim delivery mode");
    json = end + 1U;
    if (cai_parse_unsigned_decimal(json, &end, &length) != 0 || *end != '\n' ||
        length == 0U || length >= sizeof(target))
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "invalid input claim destination");
    json = end + 1U;
    n = (size_t)length;
    if (strnlen(json, n + 1U) != n + 1U || json[n] != '\n')
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "truncated input claim destination");
    memcpy(target, json, n);
    target[n] = '\0';
    json += n + 1U;
  } else
    kind = CAI_RUNTIME_INPUT_QUEUED_TURN;
  memset(&record, 0, sizeof(record));
  rc = cai_input_record_parse(json, &record, error);
  if (rc == CAI_OK && strcmp(event->type, "input_queue_claim") == 0) {
    cai_input_entry pending;
    if (record.type != CAI_INPUT_RECORD_CLAIM || record.entry.action)
      rc = cai_set_error(error, CAI_ERR_PROTOCOL, "invalid model input claim");
    else {
      const unsigned long long *events = NULL;
      size_t count = 0U;
      rc = cai_runtime_queue_ensure(runtime, error);
      if (rc == CAI_OK)
        rc = cai_input_queue_lookup(runtime->input_queue, record.entry.id,
                                    &pending, error);
      if (rc == CAI_OK)
        rc = cai_input_queue_input_events(runtime->input_queue, record.entry.id,
                                          &events, &count, error);
      if (rc == CAI_OK) {
        dispatch = cai_runtime_dispatch_new(pending.text, target,
                                            (cai_runtime_input_kind)kind,
                                            events, count, error);
        if (dispatch == NULL)
          rc = CAI_ERR_NOMEM;
        else
          dispatch->sequence = event->sequence;
      }
    }
  }
  if (rc == CAI_OK)
    rc = cai_runtime_queue_ensure(runtime, error);
  if (rc == CAI_OK)
    rc = cai_input_queue_restore(runtime->input_queue, &record, event->sequence,
                                 error);
  if (rc == CAI_OK && dispatch != NULL)
    cai_runtime_dispatch_add_locked(runtime, dispatch);
  else if (dispatch != NULL)
    cai_runtime_dispatch_free(dispatch);
  cai_input_record_cleanup(&record);
  return rc;
}
static int cai_runtime_queue_deliver(cai_agent_runtime *runtime,
                                     cai_agent_runtime *only_child,
                                     cai_error *error) {
  cai_runtime_input_dispatch *entry;
  cai_agent_runtime *destination;
  char acknowledgement[32];
  size_t remaining = 128U;
  int rc;
next_delivery:
  pthread_mutex_lock(&runtime->lock);
  for (entry = runtime->input_dispatch; entry != NULL; entry = entry->next) {
    if (entry->failed) {
      pthread_mutex_unlock(&runtime->lock);
      return cai_set_error(error, CAI_ERR_TRANSPORT,
                           "input delivery outcome is unknown; reopen the "
                           "conversation to recover");
    }
    if (entry->inflight) {
      pthread_mutex_unlock(&runtime->lock);
      return CAI_OK;
    }
    destination = entry->pinned_child;
    if (destination == NULL && strcmp(entry->target, runtime->session_id) == 0)
      destination = runtime;
    if (destination == NULL) {
      cai_agent_runtime *child = runtime->active_subagent != NULL
                                     ? runtime->active_subagent
                                     : runtime->active_review;
      if (child != NULL && strcmp(entry->target, child->session_id) == 0) {
        destination = entry->pinned_child = child;
        ++runtime->child_input_users;
      } else {
        /* Interrupted/closed children are not resumed implicitly. Preserve an
         * undispatched instruction as a parent turn, like recovered steering.
         * A durable child acknowledgement has already removed its claim. */
        destination = runtime;
        entry->kind = CAI_RUNTIME_INPUT_QUEUED_TURN;
      }
    }
    if (destination == NULL ||
        (only_child != NULL && destination != only_child))
      continue;
    entry->inflight = 1;
    pthread_mutex_unlock(&runtime->lock);
    rc = cai_runtime_enqueue_input_origin(
        destination, entry->text, entry->kind, NULL, runtime->session_id,
        entry->sequence, destination == runtime ? entry->input_events : NULL,
        destination == runtime ? entry->input_event_count : 0U, error);
    pthread_mutex_lock(&runtime->lock);
    entry->inflight = 0;
    if (rc == CAI_ERR_INVALID || rc == CAI_ERR_LIMIT) {
      cai_error_cleanup(error);
      pthread_mutex_unlock(&runtime->lock);
      return CAI_OK;
    }
    if (rc == CAI_OK && destination != runtime) {
      snprintf(acknowledgement, sizeof(acknowledgement), "%llu",
               entry->sequence);
      rc = cai_runtime_append_journal_event_locked(
          runtime, "input_queue_dispatched", acknowledgement, NULL, error);
    }
    if (rc == CAI_OK)
      cai_runtime_dispatch_remove_locked(runtime, entry->sequence,
                                         destination != runtime);
    else
      entry->failed = 1;
    pthread_mutex_unlock(&runtime->lock);
    if (rc == CAI_OK && --remaining != 0U)
      goto next_delivery;
    return rc;
  }
  pthread_mutex_unlock(&runtime->lock);
  return CAI_OK;
}

/** Model history is mutated only by its execution owner. UI admission can
 * attach parent provenance while that owner is running a synchronous child. */
static int cai_runtime_queue_own_history(cai_agent_runtime *runtime,
                                         cai_error *error) {
  cai_runtime_input_dispatch *entry, **at;
  int rc;
  while (1) {
    pthread_mutex_lock(&runtime->lock);
    entry = runtime->input_history_pending;
    pthread_mutex_unlock(&runtime->lock);
    if (entry == NULL)
      return CAI_OK;
    while (entry->input_event_cursor < entry->input_event_count) {
      rc = cai_session_history_own_event(
          runtime->session, entry->input_events[entry->input_event_cursor],
          error);
      if (rc != CAI_OK)
        return rc;
      ++entry->input_event_cursor;
    }
    pthread_mutex_lock(&runtime->lock);
    for (at = &runtime->input_history_pending; *at != NULL; at = &(*at)->next)
      if (*at == entry) {
        *at = entry->next;
        break;
      }
    pthread_mutex_unlock(&runtime->lock);
    cai_runtime_dispatch_free(entry);
  }
}
