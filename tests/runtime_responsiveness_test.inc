/* A condition gate models a slow durable append, with no timing sleeps. */
typedef struct responsive_store {
  runtime_session_store_state saved;
  pthread_mutex_t lock;
  pthread_cond_t ready;
  int entered, released, fail, gate_end, senders_started;
  int failed_records, completed_records;
} responsive_store;

static int responsive_append(void *context, const char *scope, const char *id,
                             const cai_agent_session_event *event,
                             cai_error *error) {
  responsive_store *store = context;
  int fail = 0, rc;
  const char *type =
      store->gate_end ? "assistant_text_end" : "assistant_text_delta";
  if ((store->gate_end != 2 && strcmp(event->type, type) == 0) ||
      (store->gate_end == 2 && strcmp(event->type, "execution") == 0 &&
       event->data != NULL &&
       strstr(event->data, "\"kind\":\"turn.completed\"") != NULL)) {
    pthread_mutex_lock(&store->lock);
    if (!store->entered) {
      store->entered = 1;
      pthread_cond_broadcast(&store->ready);
      while (!store->released)
        pthread_cond_wait(&store->ready, &store->lock);
      fail = store->fail;
    }
    pthread_mutex_unlock(&store->lock);
  }
  if (fail)
    return cai_set_error(error, CAI_ERR_TRANSPORT, "gated append failed");
  rc = test_runtime_session_store_append_event(&store->saved, scope, id, event,
                                               error);
  if (rc == CAI_OK && strcmp(event->type, "execution") == 0 &&
      event->data != NULL) {
    if (strstr(event->data, "\"kind\":\"turn.failed\"") != NULL)
      store->failed_records++;
    if (strstr(event->data, "\"kind\":\"turn.completed\"") != NULL)
      store->completed_records++;
  }
  return rc;
}

typedef struct responsive_events {
  size_t count, texts, text_bytes, largest;
  int failed, completed, cancelled, completion_error;
  unsigned long long sequence;
  unsigned long long text_hash;
} responsive_events;

static int responsive_event(void *context, const cai_agent_runtime_event *event,
                            cai_error *error) {
  responsive_events *seen = context;
  if (event->sequence <= seen->sequence)
    return cai_set_error(error, CAI_ERR_INVALID, "event order changed");
  seen->sequence = event->sequence;
  seen->count++;
  if (event->type == CAI_AGENT_EVENT_TEXT_DELTA) {
    size_t i;
    seen->texts++;
    seen->text_bytes += event->data_length;
    if (event->data_length > seen->largest)
      seen->largest = event->data_length;
    if (event->data_length != 0U &&
        ((unsigned char)event->data[0] & 0xc0U) == 0x80U)
      return cai_set_error(error, CAI_ERR_INVALID, "split UTF-8 scalar");
    for (i = 0U; i < event->data_length; i++)
      seen->text_hash = seen->text_hash * 33U + (unsigned char)event->data[i];
  }
  seen->failed |= event->type == CAI_AGENT_EVENT_RUN_FAILED;
  if (event->type == CAI_AGENT_EVENT_RUN_FAILED && event->data != NULL &&
      strstr(event->data, "gated append failed") != NULL)
    seen->completion_error = 1;
  seen->completed += event->type == CAI_AGENT_EVENT_RUN_COMPLETED;
  seen->cancelled |= event->type == CAI_AGENT_EVENT_RUN_CANCELLED;
  return CAI_OK;
}

typedef struct responsive_sender {
  responsive_store *store;
  cai_agent_runtime *runtime;
  int rc;
} responsive_sender;

static void *responsive_send(void *context) {
  responsive_sender *sender = context;
  cai_error error;
  cai_error_init(&error);
  pthread_mutex_lock(&sender->store->lock);
  sender->store->senders_started++;
  pthread_cond_broadcast(&sender->store->ready);
  pthread_mutex_unlock(&sender->store->lock);
  sender->rc = cai_agent_runtime_submit_queued_threadsafe(
      sender->runtime, "concurrent queued input", &error);
  cai_error_cleanup(&error);
  return NULL;
}

static void test_agent_runtime_responsiveness(test_state *state) {
  static const char body[] =
      "event: response.output_text.delta\ndata: {\"delta\":\"one\"}\n\n"
      "event: response.output_text.delta\ndata: {\"delta\":\"two\"}\n\n"
      "event: response.output_text.delta\ndata: {\"delta\":\"three\"}\n\n"
      "event: response.completed\ndata: "
      "{\"response\":{\"id\":\"resp_fair\"}}\n\n";
  static const mock_http_expectation script[] = {
      {"POST /v1/responses HTTP/", NULL, 0U, NULL, 0U, 200, "OK",
       "text/event-stream", NULL, body}};
  http_mock_client mock;
  cai_agent_runtime_config config;
  cai_agent_session_store callbacks;
  cai_agent_runtime *runtime;
  cai_agent_runtime_metrics metrics;
  cai_agent_run_state run_state;
  cai_agent_preset preset;
  responsive_store *store;
  responsive_events seen;
  cai_error error;
  struct pollfd wake;
  size_t before;
  unsigned long long expected_hash = 0U;
  char *large;
  mock_http_expectation large_script;
  mock_http_expectation concurrent_script[3];
  responsive_sender senders[2];
  pthread_t threads[2];
  int mode, i;
  large = calloc(1U, 5000U);
  if (large == NULL)
    return;
  strcpy(large, "event: response.output_text.delta\ndata: {\"delta\":\"");
  before = strlen(large);
  memset(large + before, 'x', 4097U);
  large[before + 1023U] = (char)0xe2;
  large[before + 1024U] = (char)0x82;
  large[before + 1025U] = (char)0xac;
  for (i = 0; i < 4097; i++)
    expected_hash =
        expected_hash * 33U + (unsigned char)large[before + (size_t)i];
  strcpy(large + before + 4097U, "\"}\n\nevent: response.completed\ndata: "
                                 "{\"response\":{\"id\":\"resp_large\"}}\n\n");
  large_script = script[0];
  large_script.body = large;
  for (i = 0; i < 3; i++)
    concurrent_script[i] = script[0];
  for (mode = 0; mode < 9; mode++) {
    if (http_mock_client_open_script(state, "responsive",
                                     mode == 5   ? concurrent_script
                                     : mode == 4 ? &large_script
                                                 : script,
                                     mode == 5 ? 3U : 1U, &mock) != 0) {
      free(large);
      return;
    }
    store = calloc(1U, sizeof(*store));
    if (store == NULL) {
      http_mock_client_close(state, "responsive", &mock);
      free(large);
      return;
    }
    memset(&seen, 0, sizeof(seen));
    memset(&callbacks, 0, sizeof(callbacks));
    memset(&preset, 0, sizeof(preset));
    (void)pthread_mutex_init(&store->lock, NULL);
    (void)pthread_cond_init(&store->ready, NULL);
    store->fail = mode == 1 || mode >= 7;
    store->gate_end = mode >= 6 ? 2 : mode == 3 || mode == 4;
    preset.name = "responsive";
    preset.prompt_version = "fixture";
    preset.default_model = "gpt-5-nano";
    preset.default_identity = "Fixture";
    preset.developer_instructions = "Answer the user.";
    callbacks.context = store;
    callbacks.commit = test_runtime_session_store_commit;
    callbacks.load_latest = test_runtime_session_store_load;
    callbacks.load_events_after = test_runtime_session_store_load_events_after;
    callbacks.append_event = responsive_append;
    cai_agent_runtime_config_init(&config);
    config.preset_descriptor = &preset;
    config.workspace_directory = "/tmp";
    config.session_store = &callbacks;
    config.record_transcript = 1;
    config.event_callback = mode == 8 ? NULL : responsive_event;
    config.event_context = &seen;
    cai_error_init(&error);
    runtime = NULL;
    expect_int(state, "responsive_open",
               cai_agent_runtime_open(mock.client, &config, &runtime, &error),
               CAI_OK);
    if (runtime != NULL) {
      expect_int(state, "responsive_zero_budget",
                 cai_agent_runtime_pump_bounded(runtime, 0L, 0U, &error),
                 CAI_ERR_INVALID);
      cai_error_cleanup(&error);
      cai_error_init(&error);
      expect_int(state, "responsive_submit",
                 cai_agent_runtime_submit(runtime, "hello", &error), CAI_OK);
      pthread_mutex_lock(&store->lock);
      while (!store->entered)
        pthread_cond_wait(&store->ready, &store->lock);
      pthread_mutex_unlock(&store->lock);
      if (mode == 5) {
        memset(senders, 0, sizeof(senders));
        for (i = 0; i < 2; i++) {
          senders[i].runtime = runtime;
          senders[i].store = store;
          if (pthread_create(&threads[i], NULL, responsive_send, &senders[i]) !=
              0)
            abort();
        }
        pthread_mutex_lock(&store->lock);
        while (store->senders_started != 2)
          pthread_cond_wait(&store->ready, &store->lock);
        pthread_mutex_unlock(&store->lock);
      }
      /* These owner calls must finish while the durable writer stays gated.
       * The old lock placement deadlocks here until CTest's deadline. */
      expect_int(state, "responsive_state_during_append",
                 cai_agent_runtime_state(runtime, &run_state, &error), CAI_OK);
      expect_int(state, "responsive_metrics_during_append",
                 cai_agent_runtime_get_metrics(runtime, &metrics, &error),
                 CAI_OK);
      if (mode >= 6) {
        expect_int(state, "responsive_timer_stops_before_final_append",
                   metrics.turn_active, 0L);
        expect_int(state, "responsive_final_append_not_settled", run_state,
                   CAI_AGENT_SAMPLING);
      }
      before = seen.count;
      expect_int(state, "responsive_bounded_pump",
                 cai_agent_runtime_pump_bounded(runtime, 0L, 1U, &error),
                 CAI_OK);
      expect_int(state, "responsive_one_callback", seen.count - before,
                 mode == 8 ? 0L : 1L);
      if (mode < 3 || mode == 5)
        expect_int(state, "responsive_no_text_before_ack", seen.texts, 0L);
      if (mode == 3 || mode == 4) {
        memset(&wake, 0, sizeof(wake));
        expect_int(state, "responsive_wakeup_fd",
                   cai_agent_runtime_wakeup_fd(runtime, &wake.fd, &error),
                   CAI_OK);
        wake.events = POLLIN;
        expect_int(state, "responsive_pending_stays_readable",
                   poll(&wake, 1U, 0), 1L);
      }
      if (mode == 2)
        expect_int(state, "responsive_cancel_during_append",
                   cai_agent_runtime_cancel_turn(runtime, &error), CAI_OK);
      pthread_mutex_lock(&store->lock);
      store->released = 1;
      pthread_cond_broadcast(&store->ready);
      pthread_mutex_unlock(&store->lock);
      if (mode == 5)
        for (i = 0; i < 2; i++) {
          (void)pthread_join(threads[i], NULL);
          expect_int(state, "responsive_concurrent_durable_acceptance",
                     senders[i].rc, CAI_OK);
        }
      for (i = 0; i < 200 && !seen.failed &&
                  seen.completed < (mode == 5 ? 3 : 1) && !seen.cancelled;
           i++) {
        before = seen.count;
        expect_int(state, "responsive_drain",
                   cai_agent_runtime_pump_bounded(runtime, 100L, 1U, &error),
                   CAI_OK);
        expect_int(state, "responsive_budget_preserved",
                   seen.count - before <= 1U, 1L);
        if (mode >= 6) {
          expect_int(state, "responsive_poll_only_state",
                     cai_agent_runtime_state(runtime, &run_state, &error),
                     CAI_OK);
          if (run_state != CAI_AGENT_SAMPLING &&
              (mode == 8 || seen.count == before))
            break;
        }
      }
      expect_int(state, "responsive_outcome",
                 mode == 8                ? run_state == CAI_AGENT_FAILED
                 : mode == 1 || mode == 7 ? seen.failed
                 : mode == 2              ? seen.cancelled
                                          : seen.completed,
                 mode == 5 ? 3L : 1L);
      if (mode >= 7) {
        expect_int(state, "responsive_failed_completion_never_succeeds",
                   seen.completed, 0L);
        if (mode == 7)
          expect_int(state, "responsive_completion_error_reported",
                     seen.completion_error, 1L);
        expect_int(state, "responsive_completion_failure_is_durable",
                   store->failed_records > 0, 1L);
        expect_int(state, "responsive_no_durable_success_after_failure",
                   store->completed_records, 0L);
      }
      if (mode == 0 || mode == 3)
        expect_int(state, "responsive_all_text_delivered", seen.texts, 3L);
      if (mode == 4) {
        expect_int(state, "responsive_large_delta_complete", seen.text_bytes,
                   4097L);
        expect_int(state, "responsive_large_delta_bounded",
                   seen.largest <= 1024U, 1L);
        expect_int(state, "responsive_large_delta_sliced", seen.texts, 5L);
        expect_int(state, "responsive_large_delta_exact_content",
                   seen.text_hash == expected_hash, 1L);
      }
      if (mode == 1)
        expect_int(state, "responsive_failed_text_not_published", seen.texts,
                   0L);
      if (mode == 5) {
        size_t item;
        expect_int(state, "responsive_all_queued_turns", seen.texts, 9L);
        for (item = 1U; item < store->saved.replay_event_count; item++)
          expect_int(state, "responsive_concurrent_journal_order",
                     store->saved.replay_event_sequences[item] >
                         store->saved.replay_event_sequences[item - 1U],
                     1L);
      }
      cai_agent_runtime_close(runtime);
    }
    cai_error_cleanup(&error);
    (void)pthread_cond_destroy(&store->ready);
    (void)pthread_mutex_destroy(&store->lock);
    free(store);
    http_mock_client_close(state, "responsive", &mock);
  }
  free(large);
}
