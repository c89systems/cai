/* Observable append and recovery contracts; no timing sleeps or live model. */
static int context_acquires;
static int (*context_real_acquire)(lc_client *, const lc_acquire_req *,
                                   lc_lease **, lc_error *);
static int context_acquire_count(lc_client *client,
                                 const lc_acquire_req *request, lc_lease **out,
                                 lc_error *error) {
  context_acquires++;
  return context_real_acquire(client, request, out, error);
}

static void append_context_test(cai_cli_pouch *pouch, cai_error *error) {
  cai_agent_session_event event;
  cai_cli_session session;
  cai_cli_journal_context *entry;
  lc_collection *original;
  lc_lease *held = NULL;
  int i;
  save(pouch, "/work/context", "context-owned", error);
  event.type = "assistant_text_delta";
  event.data = "durable context output";
  event.sequence = 1U;
  assert(append_event(pouch, "/work/context", "context-owned", &event, error) ==
         CAI_OK);
  entry = journal_context_find(pouch, "context-owned");
  assert(entry != NULL);
  original = entry->collection;
  /* A live metadata lease must not block the already-owned journal. Public
   * metadata stays readable while these independent collection writes commit.
   */
  assert(acquire(pouch, "context-owned", &held, error) == CAI_OK);
  context_real_acquire = pouch->sessions->acquire;
  pouch->sessions->acquire = context_acquire_count;
  context_acquires = 0;
  for (i = 0; i < 16; i++) {
    event.sequence++;
    assert(append_event(pouch, "/work/context", "context-owned", &event,
                        error) == CAI_OK);
  }
  assert(context_acquires == 0);
  assert(journal_context_find(pouch, "context-owned")->collection == original);
  assert(cai_cli_pouch_session(pouch, "context-owned", &session, error) ==
         CAI_OK);
  assert(strcmp(session.id, "context-owned") == 0);
  pouch->sessions->acquire = context_real_acquire;
  assert(release(held, CAI_OK, error) == CAI_OK);
  held = NULL;
  /* A child cannot evict its parent's active receiver. */
  save(pouch, "smith-review:/work/context", "context-child", error);
  event.sequence = 1U;
  assert(append_event(pouch, "smith-review:/work/context", "context-child",
                      &event, error) == CAI_OK);
  assert(journal_context_find(pouch, "context-owned")->collection == original);
  assert(journal_context_find(pouch, "context-child") != NULL);
  assert(cai_cli_pouch_set_retention(pouch, "context-owned", 7L, error) ==
         CAI_OK);
  assert(entry->session.retention_days == 7);
  event.sequence = 18U;
  assert(append_event(pouch, "/wrong/context", "context-owned", &event,
                      error) == CAI_ERR_INVALID);
  cai_error_cleanup(error);
  cai_error_init(error);
  assert(append_event(pouch, "/work/context", "context-owned", &event, error) ==
         CAI_OK);
}

static int crash_release_pipe;
static int (*crash_real_release)(lc_lease *, const lc_release_req *,
                                 lc_error *);
static int crash_after_release(lc_lease *lease, const lc_release_req *request,
                               lc_error *error) {
  assert(crash_real_release(lease, request, error) == LC_OK);
  assert(write(crash_release_pipe, "R", 1U) == 1);
  close(crash_release_pipe);
  for (;;)
    pause();
  return LC_OK;
}

static const char *crash_expected(int mode) {
  return mode == 4   ? "COMMITTED AT RELEASE"
         : mode == 3 ? "COMMITTED BEFORE CRASH"
                     : "";
}

static void crashed_metadata_recovery(void) {
  cai_cli_pouch *pouch = calloc(1U, sizeof(*pouch));
  cai_cli_session session;
  cai_error error;
  lc_lease *lease = NULL;
  latest_index index;
  int pipes[2], status, received, mode;
  pid_t child;
  struct pollfd ready;
  char byte, key[70];
  const char *ns;
  assert(pouch != NULL);
  cai_error_init(&error);
  for (mode = 0; mode < 5; mode++) {
    assert(cai_cli_pouch_open(pouch, NULL, NULL, NULL, &error) == CAI_OK);
    save(pouch, "/work/crashed-metadata", "crash-metadata", &error);
    if (mode == 0 || mode >= 3)
      strcpy(key, "crash-metadata");
    else {
      if (mode == 1)
        strcpy(key, "lock-");
      assert(latest_key("/work/crashed-metadata", key + (mode == 1 ? 5 : 0),
                        &error) == CAI_OK);
    }
    cai_cli_pouch_close(pouch);
    assert(lease_guards == NULL);
    assert(pipe(pipes) == 0);
    child = fork();
    assert(child >= 0);
    if (child == 0) {
      close(pipes[0]);
      assert(cai_cli_pouch_open(pouch, NULL, NULL, NULL, &error) == CAI_OK);
      ns = mode == 0 || mode >= 3 ? pouch->sessions_namespace
                                  : pouch->scopes_namespace;
      assert(acquire_ns(pouch, ns, key, &lease, &error) == CAI_OK);
      if (mode == 0 || mode >= 3) {
        assert(get_session(lease, "/work/crashed-metadata", "crash-metadata",
                           &session, &error) == CAI_OK);
        if (mode == 3) {
          strcpy(session.first_prompt, "COMMITTED BEFORE CRASH");
          assert(update_session(lease, &session, &error) == CAI_OK);
          assert(release(lease, CAI_OK, &error) == CAI_OK);
          assert(acquire_ns(pouch, ns, key, &lease, &error) == CAI_OK);
        }
        if (mode == 4) {
          strcpy(session.first_prompt, crash_expected(mode));
          assert(update_session(lease, &session, &error) == CAI_OK);
          crash_real_release = lease->release;
          lease->release = crash_after_release;
          crash_release_pipe = pipes[1];
          /* The SDK commit has returned, but the CAI ownership guard has not
           * closed. Kill at that exact boundary; committed data must survive.
           */
          (void)release(lease, CAI_OK, &error);
          abort();
        }
        strcpy(session.first_prompt, "UNCOMMITTED CRASH DATA");
        assert(update_session(lease, &session, &error) == CAI_OK);
      }
      assert(write(pipes[1], "R", 1U) == 1);
      close(pipes[1]);
      for (;;)
        pause();
    }
    close(pipes[1]);
    ready.fd = pipes[0];
    ready.events = POLLIN;
    ready.revents = 0;
    received = poll(&ready, 1U, 10000) == 1 && read(pipes[0], &byte, 1U) == 1 &&
               byte == 'R';
    close(pipes[0]);
    if (received) {
      assert(cai_cli_pouch_open(pouch, NULL, NULL, NULL, &error) == CAI_OK);
      ns = mode == 0 || mode >= 3 ? pouch->sessions_namespace
                                  : pouch->scopes_namespace;
      assert(acquire_wait(pouch, ns, key, 0L, &lease, &error) != CAI_OK);
      assert(lease == NULL && strstr(error.message, "live CAI context"));
      cai_error_cleanup(&error);
      cai_error_init(&error);
      if (mode == 0 || mode >= 3) {
        assert(cai_cli_pouch_session(pouch, "crash-metadata", &session,
                                     &error) == CAI_OK);
        assert(strcmp(session.first_prompt, crash_expected(mode)) == 0);
      }
      cai_cli_pouch_close(pouch);
    }
    assert(kill(child, SIGKILL) == 0);
    assert(waitpid(child, &status, 0) == child);
    assert(received && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    assert(cai_cli_pouch_open(pouch, NULL, NULL, NULL, &error) == CAI_OK);
    ns = mode == 0 || mode >= 3 ? pouch->sessions_namespace
                                : pouch->scopes_namespace;
    /* Nonblocking acquisition proves recovery does not wait out the one-hour
     * native lease. The SDK receives its exact observed fence/identity. */
    assert(acquire_wait(pouch, ns, key, 0L, &lease, &error) == CAI_OK);
    assert(release(lease, CAI_OK, &error) == CAI_OK);
    lease = NULL;
    assert(cai_cli_pouch_session(pouch, "crash-metadata", &session, &error) ==
           CAI_OK);
    assert(strcmp(session.first_prompt, crash_expected(mode)) == 0);
    assert(latest_open(pouch, "/work/crashed-metadata", &lease, &index,
                       &error) == CAI_OK);
    assert(release(lease, CAI_OK, &error) == CAI_OK);
    lease = NULL;
    newest(pouch, "/work/crashed-metadata", "crash-metadata", &error);
    save(pouch, "/work/crashed-metadata", "crash-metadata", &error);
    cai_cli_pouch_close(pouch);
    assert(lease_guards == NULL);
  }
  cai_error_cleanup(&error);
  free(pouch);
}

static void foreign_metadata_owner(cai_cli_pouch *pouch, cai_error *error) {
  lc_acquire_req request;
  lc_release_req rollback;
  lc_lease *foreign = NULL, *other = NULL;
  lc_error local;
  lc_acquire_req_init(&request);
  lc_release_req_init(&rollback);
  lc_error_init(&local);
  request.ns = pouch->sessions_namespace;
  request.key = "foreign-metadata";
  request.owner = "external-owner";
  request.ttl_seconds = 3600L;
  assert(pouch->sessions->acquire(pouch->sessions, &request, &foreign,
                                  &local) == LC_OK);
  assert(acquire_wait(pouch, request.ns, request.key, 0L, &other, error) !=
         CAI_OK);
  assert(other == NULL && strstr(error->message, "different owner"));
  cai_error_cleanup(error);
  cai_error_init(error);
  rollback.rollback = 1;
  assert(lc_lease_release(foreign, &rollback, &local) == LC_OK);
  assert(acquire_wait(pouch, request.ns, request.key, 0L, &other, error) ==
         CAI_OK);
  assert(release(other, CAI_OK, error) == CAI_OK);
  lc_error_cleanup(&local);
}

static int context_release_failure(lc_lease *lease,
                                   const lc_release_req *request,
                                   lc_error *error) {
  (void)lease;
  (void)request;
  return lc_error_set(error, LC_ERR_TRANSPORT, 0L,
                      "injected metadata release failure", NULL, NULL, NULL);
}

static void abandoned_metadata_recovery(cai_cli_pouch *pouch,
                                        cai_error *error) {
  lc_lease *lease = NULL;
  cai_cli_session session;
  int found = 1;
  assert(acquire(pouch, "abandoned-metadata", &lease, error) == CAI_OK);
  assert(get_session(lease, "/work/abandoned", "abandoned-metadata", &session,
                     error) == CAI_OK);
  strcpy(session.first_prompt, "UNCOMMITTED FAILED RELEASE");
  assert(update_session(lease, &session, error) == CAI_OK);
  lease->release = context_release_failure;
  assert(release(lease, CAI_OK, error) == CAI_ERR_TRANSPORT);
  cai_error_cleanup(error);
  cai_error_init(error);
  lease = NULL;
  /* The failed release closes the unusable receiver and ownership guard.
   * A retry must recover its native lease without reopening the root client. */
  assert(acquire_wait(pouch, NULL, "abandoned-metadata", 0L, &lease, error) ==
         CAI_OK);
  assert(release(lease, CAI_OK, error) == CAI_OK);
  assert(session_read(pouch, "abandoned-metadata", &session, &found, error) ==
         CAI_OK);
  assert(!found);
}

static void context_namespace_and_expiry(cai_cli_pouch *pouch,
                                         cai_error *error) {
  cai_cli_pouch *other = calloc(1U, sizeof(*other));
  cai_cli_options *options = calloc(1U, sizeof(*options));
  cai_cli_session *items = NULL, session;
  lc_collection *collection = NULL;
  lc_error local;
  lc_lease *lease = NULL, *second = NULL;
  size_t count = 0U;
  int found = 0;
  assert(other != NULL && options != NULL);
  options->state_dir = pouch->state_directory;
  options->program_name = "another";
  options->namespace_prefix = "another";
  options->conversation_retention_days = 30L;
  assert(cai_cli_pouch_open_options(other, options, NULL, error) == CAI_OK);
  assert(other->sessions == pouch->sessions);
  save(pouch, "/work/ns-one", "namespace-same-id", error);
  save(other, "/work/ns-two", "namespace-same-id", error);
  assert(cai_cli_pouch_session(pouch, "namespace-same-id", &session, error) ==
         CAI_OK);
  assert(strcmp(session.workspace, "/work/ns-one") == 0);
  assert(cai_cli_pouch_session(other, "namespace-same-id", &session, error) ==
         CAI_OK);
  assert(strcmp(session.workspace, "/work/ns-two") == 0);
  assert(cai_cli_pouch_list(other, "/work/ns-two", &items, &count, error) ==
         CAI_OK);
  assert(count == 1U && strcmp(items[0].id, "namespace-same-id") == 0);
  free(items);
  items = NULL;
  /* Namespace separation applies to ownership guards as well as lookups. */
  assert(acquire(pouch, "namespace-same-id", &lease, error) == CAI_OK);
  assert(acquire_wait(other, NULL, "namespace-same-id", 0L, &second, error) ==
         CAI_OK);
  assert(release(second, CAI_OK, error) == CAI_OK);
  assert(release(lease, CAI_OK, error) == CAI_OK);
  cai_cli_pouch_close(other);
  free(other);
  free(options);
  /* Model SDK maintenance removing an expired collection, without sleeps or
   * manipulating a second application retention clock. Derived lookup and
   * prompt recall must disappear too, never recreate an empty journal. */
  save(pouch, "/work/expiry", "expired-context", error);
  assert(cai_cli_pouch_prompt_open(pouch, "expired-context", error) == CAI_OK);
  assert(cai_cli_pouch_prompt_append(pouch, "expired draft", error) == CAI_OK);
  cai_cli_pouch_prompt_close(pouch);
  cai_cli_pouch_end_use(pouch);
  lc_error_init(&local);
  assert(open_history_collection(pouch, "expired-context", "events", 0, 0,
                                 &collection, error) == CAI_OK);
  assert(collection->drop(collection, &local) == LC_OK);
  lc_collection_close(collection);
  collection = NULL;
  assert(cai_cli_pouch_list(pouch, "/work/expiry", &items, &count, error) ==
         CAI_OK);
  assert(count == 0U);
  free(items);
  assert(session_read(pouch, "expired-context", &session, &found, error) ==
         CAI_OK);
  assert(!found);
  assert(open_history_collection(pouch, "expired-context", "prompts", 0, 1,
                                 &collection, error) == CAI_OK);
  assert(collection == NULL);
  assert(open_history_collection(pouch, "expired-context", "events", 0, 1,
                                 &collection, error) == CAI_OK);
  assert(collection == NULL);
  lc_error_cleanup(&local);
}
