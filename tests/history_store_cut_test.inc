/* Exercise real file-store publication and streaming prefix reuse, without
 * inference or timed waits. The CLI trim test covers the Pouch/runtime path. */
#include <dirent.h>
#include <unistd.h>
#define CAI_CUT_FAIL(e, m) cai_set_error((e), CAI_ERR_INVALID, (m))
#include "../src/cai_session_cut.h"
#undef CAI_CUT_FAIL

typedef struct history_cut_source {
  cai_session *session;
  unsigned long long revision;
  size_t bytes;
  int fail;
} history_cut_source;

static void history_cut_planner(void) {
  static const char large_prefix[] =
      "{\"record_type\":\"event\",\"sequence\":6,\"data\":\"";
  static const char escaped[] = "λ\\n\\\"";
  static const char *const records[] = {
      "{\"record_type\":\"commit\",\"state\":{\"revision\":1,\"base_bytes\":0,"
      "\"end_bytes\":0}}",
      "{\"record_type\":\"event\",\"sequence\":1,\"data\":\"keep\"}",
      "{\"record_type\":\"commit\",\"state\":{\"revision\":2,\"base_bytes\":0,"
      "\"end_bytes\":10}}",
      "{\"record_type\":\"event\",\"sequence\":2,\"data\":\"queued\"}",
      "{\"record_type\":\"event\",\"sequence\":3,\"data\":\"footer\"}",
      "{\"record_type\":\"commit\",\"state\":{\"revision\":3,\"base_bytes\":10,"
      "\"end_bytes\":10}}",
      "{\"record_type\":\"event\",\"sequence\":4,\"data\":\"removed\"}",
      "{\"record_type\":\"commit\",\"state\":{\"revision\":4,\"base_bytes\":10,"
      "\"end_bytes\":20}}",
      "{\"record_type\":\"event\",\"sequence\":5,\"data\":\"result\"}"};
  cai_cut_plan *plan = calloc(1U, sizeof(*plan));
  cai_agent_session_event_range early = {2U, 2U};
  cai_agent_session_trim_request request;
  cai_error error;
  lonejson_error local;
  char chunk[1024];
  size_t i, j;
  assert(plan != NULL);
  memset(&request, 0, sizeof(request));
  request.first_sequence = 4U;
  request.through_sequence = 6U;
  request.history_bytes = 10U;
  request.ranges = &early;
  request.range_count = 1U;
  plan->request = &request;
  plan->json = cai_cut_json(&local);
  assert(plan->json != NULL);
  cai_error_init(&error);
  for (i = 0U; i < sizeof(records) / sizeof(records[0]); i++) {
    for (j = 0U; records[i][j] != '\0'; j++)
      assert(cai_cut_feed(plan, records[i] + j, 1U, &error) == CAI_OK);
    assert(cai_cut_end(plan, i + 1U, &error) == CAI_OK);
  }
  /* A body larger than lonejson's default dynamic-string cap is discarded
   * incrementally, including UTF-8/JSON escapes spanning transport chunks. */
  assert(cai_cut_feed(plan, large_prefix, sizeof(large_prefix) - 1U, &error) ==
         CAI_OK);
  for (j = 0U; j < sizeof(escaped) - 1U; j++)
    assert(cai_cut_feed(plan, escaped + j, 1U, &error) == CAI_OK);
  memset(chunk, 'x', sizeof(chunk));
  for (i = 0U; i < 2048U; i++)
    assert(cai_cut_feed(plan, chunk, sizeof(chunk), &error) == CAI_OK);
  assert(cai_cut_feed(plan, "\"}", 2U, &error) == CAI_OK);
  assert(cai_cut_end(plan, 10U, &error) == CAI_OK);
  assert(cai_cut_finish(plan, &error) == CAI_OK);
  assert(plan->anchor == 6U && plan->revision == 3U && plan->bytes == 10U);
  assert(plan->count == 2U && cai_cut_removed(plan, 4U) &&
         cai_cut_removed(plan, 7U) && cai_cut_removed(plan, 10U));
  assert(!cai_cut_removed(plan, 5U) && !cai_cut_removed(plan, 6U));
  /* Duplicate/reordered event identities cannot silently select a new cut. */
  assert(cai_cut_feed(plan, records[1], strlen(records[1]), &error) == CAI_OK);
  assert(cai_cut_end(plan, 11U, &error) == CAI_ERR_INVALID);
  clear(&error);
  assert(cai_cut_feed(plan, "{", 1U, &error) == CAI_OK);
  assert(cai_cut_end(plan, 12U, &error) == CAI_ERR_INVALID);
  cai_cut_record_close(plan);
  clear(&error);
  /* Reject a cut that exceeds the SDK's atomic range bound before mutation. */
  plan->event_count = 0U;
  for (i = 0U; i < CAI_CUT_MAX_RANGES; i++)
    assert(cai_cut_add(plan->events, &plan->event_count, i * 2U + 1U, &error) ==
           CAI_OK);
  assert(cai_cut_add(plan->events, &plan->event_count, 999U, &error) ==
         CAI_ERR_INVALID);
  cai_error_cleanup(&error);
  lonejson_free(plan->json);
  free(plan);
}

static size_t history_cut_failed_read(void *context, void *bytes, size_t count,
                                      cai_error *error) {
  (void)context;
  (void)bytes;
  (void)count;
  cai_set_error(error, CAI_ERR_TRANSPORT, "injected cut producer failure");
  return 0U;
}
static int history_cut_make(void *context, unsigned long long revision,
                            size_t bytes, cai_source **out, cai_error *error) {
  history_cut_source *cut = context;
  cai_source_callbacks callbacks;
  cut->revision = revision;
  cut->bytes = bytes;
  if (cut->fail == 1)
    return cai_set_error(error, CAI_ERR_TRANSPORT, "injected cut preparation");
  if (cut->fail == 2) {
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.read = history_cut_failed_read;
    return cai_source_from_callbacks(&callbacks, out, error);
  }
  return cai_session_journal_trim_source(cut->session, revision, bytes, out,
                                         error);
}
static void history_store_commit(cai_agent_session_store *store,
                                 cai_session *session, const char *id,
                                 unsigned long long sequence,
                                 cai_error *error) {
  cai_source *source;
  assert(cai_session_journal_source(session, &source, error) == CAI_OK);
  assert(store->commit(store->context, "test", id, source, sequence, error) ==
         CAI_OK);
  cai_source_close(source);
  assert(cai_session_journal_accept(session, error) == CAI_OK);
}
static void history_store_read(cai_agent_session_store *store, const char *id,
                               journal_bytes *bytes, cai_error *error) {
  cai_source *source;
  unsigned long long sequence;
  assert(store->load_id(store->context, "test", id, &source, &sequence,
                        error) == CAI_OK);
  assert(source != NULL);
  journal_bytes_capture(source, bytes, 17U, error);
  cai_source_close(source);
}
static void history_store_event(cai_agent_session_store *store, const char *id,
                                unsigned long long sequence, const char *data,
                                cai_error *error) {
  cai_agent_session_event event;
  event.sequence = sequence;
  event.type = "test_event";
  event.data = data;
  assert(store->append_event(store->context, "test", id, &event, error) ==
         CAI_OK);
}
static void history_cut_cleanup(const char *root) {
  DIR *directory = opendir(root), *scope;
  struct dirent *entry, *file;
  char path[256], leaf[512];
  assert(directory != NULL);
  while ((entry = readdir(directory)) != NULL) {
    if (entry->d_name[0] == '.')
      continue;
    assert(snprintf(path, sizeof(path), "%s/%s", root, entry->d_name) > 0);
    scope = opendir(path);
    assert(scope != NULL);
    while ((file = readdir(scope)) != NULL) {
      if (strcmp(file->d_name, ".") == 0 || strcmp(file->d_name, "..") == 0)
        continue;
      assert(snprintf(leaf, sizeof(leaf), "%s/%s", path, file->d_name) > 0);
      assert(unlink(leaf) == 0);
    }
    assert(closedir(scope) == 0 && rmdir(path) == 0);
  }
  assert(closedir(directory) == 0 && rmdir(root) == 0);
}
static void history_store_cuts(void) {
  static const char compact_json[] =
      "[{\"type\":\"compaction\",\"encrypted_content\":\"opaque\"}]";
  char directory[] = "history-cut-XXXXXX";
  char *absolute;
  cai_agent_local_session_store_config store_config;
  cai_agent_session_store store;
  cai_client_config config;
  cai_agent_config agent_config;
  cai_client *client;
  cai_agent *agent;
  cai_session *session, *restored;
  cai_error error;
  cai_agent_session_event_range ranges[2] = {{4U, 5U}, {2U, 2U}};
  cai_agent_session_trim_request request;
  history_cut_source cut;
  journal_bytes before = {0}, after = {0}, compact = {0};
  cai_source_callbacks callbacks;
  cai_source *source;
  int mode, imported;
  cai_error_init(&error);
  assert(mkdtemp(directory) != NULL);
  absolute = realpath(directory, NULL);
  assert(absolute != NULL);
  cai_agent_local_session_store_config_init(&store_config);
  store_config.root_directory = absolute;
  assert(cai_agent_local_session_store_open(&store_config, &store, &error) ==
         CAI_OK);
  assert(store.trim != NULL);
  cai_client_config_init(&config);
  config.api_key = "fixture";
  config.base_url = "http://127.0.0.1:1/v1";
  config.logger_disabled = 1;
  cai_agent_config_init(&agent_config);
  agent_config.enable_local_history = 1;
  agent_config.session_continuity = CAI_SESSION_CONTINUITY_CLIENT_HISTORY;
  agent_config.model = "gpt-6-luna";
  agent_config.reasoning_effort = "high";
  agent_config.service_tier = "flex";
  assert(cai_client_open(&config, &client, &error) == CAI_OK);
  assert(cai_client_new_agent(client, &agent_config, &agent, &error) == CAI_OK);
  for (imported = 0; imported <= 1; imported++) {
    const char *id = imported ? "imported" : "incremental";
    memset(&cut, 0, sizeof(cut));
    memset(&request, 0, sizeof(request));
    assert(cai_agent_new_session(agent, &session, &error) == CAI_OK);
    CAI_SESSION_IMPL(session)->goal_objective =
        cai_strdup(NULL, "Goal survives trim");
    CAI_SESSION_IMPL(session)->goal_status = cai_strdup(NULL, "paused");
    assert(CAI_SESSION_IMPL(session)->goal_objective != NULL &&
           CAI_SESSION_IMPL(session)->goal_status != NULL);
    if (!imported)
      history_store_commit(&store, session, id, 0U, &error);
    history_store_event(&store, id, 1U, "KEEP prompt", &error);
    assert(cai_session_history_begin(session, CAI_AGENT_HISTORY_TURN, 1U, 1U,
                                     "KEEP", &error) == CAI_OK);
    assert(session->add_user_text(session, "KEEP context λ", &error) == CAI_OK);
    assert(cai_session_commit_pending_inputs(session, &error) == CAI_OK);
    if (!imported)
      history_store_commit(&store, session, id, 1U, &error);
    history_store_event(&store, id, 2U, "DROP queued prompt", &error);
    history_store_event(&store, id, 3U, "KEEP footer", &error);
    if (!imported)
      history_store_commit(&store, session, id, 1U, &error);
    assert(cai_session_history_begin(session, CAI_AGENT_HISTORY_TURN, 4U, 2U,
                                     "DROP", &error) == CAI_OK);
    assert(session->add_user_text(session, "DROP context", &error) == CAI_OK);
    assert(cai_session_commit_pending_inputs(session, &error) == CAI_OK);
    history_store_event(&store, id, 4U, "DROP consumed input", &error);
    history_store_commit(&store, session, id, 4U, &error);
    history_store_event(&store, id, 5U, "DROP tool result", &error);
    CAI_SESSION_IMPL(session)->usage.usage.total_tokens = 42LL;
    CAI_SESSION_IMPL(session)->usage.estimated_spend_usd = 0.125;
    assert(cai_session_history_trim(session, 1U, 5U, &error) == CAI_OK);
    request.first_sequence = 4U;
    request.through_sequence = 5U;
    request.ranges = ranges;
    request.range_count = 2U;
    request.history_bytes = CAI_SESSION_IMPL(session)->history.size_fn(
        &CAI_SESSION_IMPL(session)->history);
    cut.session = session;
    history_store_read(&store, id, &before, &error);
    for (mode = 1; mode <= 2; mode++) {
      cut.fail = mode;
      assert(store.trim(store.context, "test", id, &request, history_cut_make,
                        &cut, 5U, &error) == CAI_ERR_TRANSPORT);
      clear(&error);
      history_store_read(&store, id, &after, &error);
      assert(before.used == after.used &&
             memcmp(before.data, after.data, before.used) == 0);
      free(after.data);
      memset(&after, 0, sizeof(after));
    }
    cut.fail = 0;
    assert(store.trim(store.context, "test", id, &request, history_cut_make,
                      &cut, 5U, &error) == CAI_OK);
    assert(cut.bytes == (imported ? 0U : request.history_bytes));
    CAI_SESSION_IMPL(session)->journal_revision = cut.revision;
    assert(cai_session_journal_accept(session, &error) == CAI_OK);
    history_store_read(&store, id, &after, &error);
    assert(strstr(after.data, "DROP") == NULL);
    assert(strstr(after.data, "KEEP footer") != NULL);
    assert(cai_agent_new_session(agent, &restored, &error) == CAI_OK);
    after.chunk = 1U;
    assert(journal_bytes_restore(restored, &after, &error) == CAI_OK);
    journal_test_equal(session, restored, &error);
    restored->close(restored);
    free(before.data);
    free(after.data);
    memset(&before, 0, sizeof(before));
    memset(&after, 0, sizeof(after));
    /* A new append after the replacement uses the correct rebased revision.
     * Compaction is indivisible and trimming it returns to an empty prefix. */
    journal_bytes_append(&compact, compact_json, sizeof(compact_json) - 1U);
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.read = journal_bytes_read;
    callbacks.context = &compact;
    assert(cai_source_from_callbacks(&callbacks, &source, &error) == CAI_OK);
    assert(session->import_history_source(session, source, &error) == CAI_OK);
    cai_source_close(source);
    assert(cai_session_history_begin(session, CAI_AGENT_HISTORY_COMPACTION, 0U,
                                     0U, "Compacted history",
                                     &error) == CAI_OK);
    cai_session_history_compacted(session);
    history_store_commit(&store, session, id, 5U, &error);
    /* Trimming a later turn keeps the compaction entry as the context base. */
    assert(cai_session_history_begin(session, CAI_AGENT_HISTORY_TURN, 6U, 6U,
                                     "AFTER COMPACTION", &error) == CAI_OK);
    assert(session->add_user_text(session, "DROP after compaction", &error) ==
           CAI_OK);
    assert(cai_session_commit_pending_inputs(session, &error) == CAI_OK);
    history_store_event(&store, id, 6U, "DROP after compaction", &error);
    history_store_commit(&store, session, id, 6U, &error);
    assert(cai_session_history_trim(session, 1U, 6U, &error) == CAI_OK);
    request.first_sequence = request.through_sequence = 6U;
    request.history_bytes = CAI_SESSION_IMPL(session)->history.size_fn(
        &CAI_SESSION_IMPL(session)->history);
    request.range_count = 0U;
    assert(store.trim(store.context, "test", id, &request, history_cut_make,
                      &cut, 6U, &error) == CAI_OK);
    assert(cut.bytes == request.history_bytes);
    CAI_SESSION_IMPL(session)->journal_revision = cut.revision;
    assert(cai_session_journal_accept(session, &error) == CAI_OK);
    history_store_read(&store, id, &after, &error);
    assert(strstr(after.data, "DROP after compaction") == NULL);
    assert(cai_agent_new_session(agent, &restored, &error) == CAI_OK);
    assert(journal_bytes_restore(restored, &after, &error) == CAI_OK);
    journal_test_equal(session, restored, &error);
    restored->close(restored);
    free(after.data);
    memset(&after, 0, sizeof(after));
    assert(cai_session_history_trim(session, 1U, 6U, &error) == CAI_OK);
    request.first_sequence = 1U;
    request.history_bytes = 0U;
    request.range_count = 0U;
    assert(store.trim(store.context, "test", id, &request, history_cut_make,
                      &cut, 6U, &error) == CAI_OK);
    CAI_SESSION_IMPL(session)->journal_revision = cut.revision;
    assert(cai_session_journal_accept(session, &error) == CAI_OK);
    history_store_read(&store, id, &after, &error);
    assert(strstr(after.data, "KEEP") == NULL);
    assert(strstr(after.data, "opaque") == NULL);
    assert(cai_agent_new_session(agent, &restored, &error) == CAI_OK);
    assert(journal_bytes_restore(restored, &after, &error) == CAI_OK);
    journal_test_equal(session, restored, &error);
    restored->close(restored);
    session->close(session);
    free(after.data);
    free(compact.data);
    memset(&after, 0, sizeof(after));
    memset(&compact, 0, sizeof(compact));
  }
  cai_agent_local_session_store_close(&store);
  agent->close(agent);
  client->close(client);
  cai_error_cleanup(&error);
  history_cut_cleanup(directory);
  free(absolute);
}
