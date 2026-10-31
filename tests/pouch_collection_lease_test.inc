/* Consumer ownership contracts against the released SDK. Barriers and direct
 * renewal exercise failure paths without waiting for a lease TTL or inference.
 */
static pthread_mutex_t lease_wait_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t lease_wait_changed = PTHREAD_COND_INITIALIZER;
static pthread_cond_t *lease_wait_condition;
static struct timespec lease_wait_deadline;
static int lease_wait_observed;

static int observe_lease_wait(pthread_cond_t *condition, pthread_mutex_t *mutex,
                              const struct timespec *deadline) {
  pthread_mutex_lock(&lease_wait_lock);
  if (condition == lease_wait_condition) {
    lease_wait_deadline = *deadline;
    lease_wait_observed = 1;
    pthread_cond_signal(&lease_wait_changed);
  }
  pthread_mutex_unlock(&lease_wait_lock);
  return pthread_cond_timedwait(condition, mutex, deadline);
}

static void assert_daily_lease(const lc_collection_lease_info *info,
                               time_t started) {
  assert(info->active);
  assert(info->expires_at_unix >= (lc_unix_seconds)started + 86400L);
  assert(info->expires_at_unix <= (lc_unix_seconds)time(NULL) + 86400L);
}

static void clear_lease_error(cai_error *error) {
  cai_error_cleanup(error);
  cai_error_init(error);
}
static void *renew_with_journal_busy(void *context) {
  cai_cli_pouch *pouch = context;
  pthread_mutex_lock(&pouch->ownership_lock);
  collection_owners_renew(pouch);
  pthread_mutex_unlock(&pouch->ownership_lock);
  return NULL;
}
static void *check_with_renewal_busy(void *context) {
  assert(cai_cli_pouch_ownership_check(context, NULL) == CAI_OK);
  return NULL;
}
static int fail_collection_release(lc_collection *collection, lc_error *error) {
  (void)collection;
  return lc_error_set(error, LC_ERR_TRANSPORT, 0L,
                      "injected collection release failure", NULL, NULL, NULL);
}

static void collection_lease_failures(void) {
  cai_cli_pouch *pouch = calloc(1U, sizeof(*pouch));
  cai_cli_lease_guard *guard = NULL;
  lc_collection *public = NULL, *owned = NULL, *foreign = NULL;
  lc_collection_info before, after;
  lc_collection_lease_req request;
  cai_error error;
  lc_error local;
  assert(pouch != NULL);
  cai_error_init(&error);
  lc_error_init(&local);
  lc_collection_lease_req_init(&request);
  assert(cai_cli_pouch_open(pouch, NULL, NULL, NULL, &error) == CAI_OK);
  assert(open_history_collection(pouch, "lease-failures", "events", 1, 0,
                                 &public, &error) == CAI_OK);
  request.owner = pouch->program_name;
  assert(public->acquire(public, &request, &foreign, &local) == LC_OK);
  /* The application name alone is not proof of an abandoned CAI owner. */
  assert(collection_owner_open(pouch, "lease-failures", "events", 0, 0, &owned,
                               &error) != CAI_OK &&
         owned == NULL);
  clear_lease_error(&error);
  assert(lease_guard_open(pouch, pouch->sessions_namespace,
                          "!collection.events.lease-failures", 0L, &guard,
                          &error) == CAI_OK);
  assert(ftruncate(guard->fd, 0) == 0 && write(guard->fd, "bad", 3U) == 3);
  lease_guard_close(guard);
  assert(collection_owner_open(pouch, "lease-failures", "events", 0, 0, &owned,
                               &error) != CAI_OK &&
         owned == NULL);
  assert(strstr(error.message, "damaged") != NULL);
  clear_lease_error(&error);
  assert(foreign->describe(foreign, &before, &local) == LC_OK &&
         before.lease.active);
  assert(foreign->release(foreign, &local) == LC_OK);
  /* A damaged marker is repairable once there is no live SDK lease. */
  assert(collection_owner_open(pouch, "lease-failures", "events", 0, 0, &owned,
                               &error) == CAI_OK);
  assert(owned->describe(owned, &before, &local) == LC_OK);
  owned->release = fail_collection_release;
  collection_owner_remove(pouch, owned);
  owned = NULL;
  /* Failed release leaves a durable lease; immediate conditional recovery
   * retains history and does not wait for that abandoned lease to expire. */
  assert(collection_owner_open(pouch, "lease-failures", "events", 0, 0, &owned,
                               &error) == CAI_OK);
  assert(owned->describe(owned, &after, &local) == LC_OK);
  assert(after.lease.active &&
         after.lease.fencing_token > before.lease.fencing_token);
  assert(strcmp(after.lease.lease_id, before.lease.lease_id) != 0);
  lc_collection_close(public);
  cai_cli_pouch_close(pouch);
  lc_error_cleanup(&local);
  cai_error_cleanup(&error);
  free(pouch);
}
static void collection_lease_contracts(void) {
  cai_cli_pouch *owner = calloc(1U, sizeof(*owner));
  cai_cli_pouch *peer = calloc(1U, sizeof(*peer));
  lc_collection *owned = NULL, *public = NULL, *replacement = NULL;
  lc_collection_info before, after;
  lc_collection_lease_req request;
  cai_source *source = NULL;
  cai_error error;
  lc_error local;
  cai_agent_session_event event;
  unsigned long long sequence;
  pthread_t renewal;
  struct pollfd notification;
  struct timespec waiting_since, observed_at, timeout;
  time_t acquired_at, renewed_at;
  char bytes[256];
  size_t count, total = 0U;
  assert(owner != NULL && peer != NULL);
  cai_error_init(&error);
  lc_error_init(&local);
  lc_collection_lease_req_init(&request);
  assert(cai_cli_pouch_open(owner, NULL, NULL, NULL, &error) == CAI_OK);
  cai_cli_pouch_begin_use(owner);
  assert(clock_gettime(CLOCK_MONOTONIC, &waiting_since) == 0);
  acquired_at = time(NULL);
  pthread_mutex_lock(&lease_wait_lock);
  lease_wait_condition = &owner->ownership_condition;
  lease_wait_observed = 0;
  pthread_mutex_unlock(&lease_wait_lock);
  save(owner, "/work/leased", "leased-session", &error);
  assert(clock_gettime(CLOCK_REALTIME, &timeout) == 0);
  timeout.tv_sec += 3;
  pthread_mutex_lock(&lease_wait_lock);
  while (!lease_wait_observed)
    assert(pthread_cond_timedwait(&lease_wait_changed, &lease_wait_lock,
                                  &timeout) == 0);
  assert(clock_gettime(CLOCK_MONOTONIC, &observed_at) == 0);
  /* Inspect the actual worker wait, not a second copy of its policy. */
  assert(lease_wait_deadline.tv_sec >= waiting_since.tv_sec + 23L * 3600L);
  assert(lease_wait_deadline.tv_sec <= observed_at.tv_sec + 23L * 3600L);
  lease_wait_condition = NULL;
  pthread_mutex_unlock(&lease_wait_lock);
  owner->capture_session = 0;
  assert(collection_owner_open(owner, "leased-session", "events", 0, 0, &owned,
                               &error) == CAI_OK);
  assert(owned->describe(owned, &before, &local) == LC_OK &&
         before.lease.active);
  assert_daily_lease(&before.lease, acquired_at);
  assert(cai_cli_pouch_prompt_open(owner, "leased-session", &error) == CAI_OK);
  assert(cai_cli_pouch_prompt_append(owner, "kept draft", &error) == CAI_OK);
  assert(cai_cli_pouch_open(peer, NULL, NULL, NULL, &error) == CAI_OK);
  assert(owner->sessions == peer->sessions);
  cai_cli_pouch_begin_use(peer);
  assert(peer->store.load_id(peer, "/work/leased", "leased-session", &source,
                             &sequence, &error) != CAI_OK &&
         source == NULL);
  clear_lease_error(&error);
  cai_cli_pouch_end_use(peer);
  /* Store callbacks also serve resumed children, after root identity capture
   * has ended. They must still acquire before returning writable history. */
  assert(!peer->capture_session);
  assert(peer->store.load_id(peer, "/work/leased", "leased-session", &source,
                             &sequence, &error) != CAI_OK &&
         source == NULL);
  clear_lease_error(&error);
  /* Public reads neither acquire ownership nor change the held lease. */
  assert(load_id(peer, "/work/leased", "leased-session", &source, &sequence,
                 &error) == CAI_OK &&
         source != NULL);
  while ((count = cai_source_read(source, bytes, sizeof(bytes), &error)) != 0U)
    total += count;
  assert(total > 0U && error.code == CAI_OK);
  cai_source_close(source);
  source = NULL;
  assert(!peer->ownership_running && peer->collection_owners == NULL);
  assert(cai_cli_pouch_prompt_open_read(peer, "leased-session", &error) ==
         CAI_OK);
  assert(cai_cli_pouch_prompt_append(peer, "must not write", &error) ==
         CAI_ERR_INVALID);
  clear_lease_error(&error);
  cai_cli_pouch_prompt_close(peer);
  assert(open_history_collection(peer, "leased-session", "events", 0, 0,
                                 &public, &error) == CAI_OK);
  request.owner =
      before.lease.owner; /* Equal labels never share credentials. */
  assert(public->acquire(public, &request, &replacement, &local) != LC_OK);
  assert(replacement == NULL);
  event.type = "assistant_text_delta";
  event.data = "leased output";
  event.sequence = 1U;
  assert(append_event(owner, "/work/leased", "leased-session", &event,
                      &error) == CAI_OK);
  save(owner, "smith-review:/work/leased", "leased-child", &error);
  assert(owned->describe(owned, &after, &local) == LC_OK);
  assert(strcmp(before.lease.lease_id, after.lease.lease_id) == 0);
  /* Renewal runs on a default-stack worker while the journal mutex is held. */
  renewed_at = time(NULL);
  pthread_mutex_lock(&owner->journal_lock);
  assert(pthread_create(&renewal, NULL, renew_with_journal_busy, owner) == 0);
  assert(pthread_join(renewal, NULL) == 0);
  pthread_mutex_unlock(&owner->journal_lock);
  assert(cai_cli_pouch_ownership_check(owner, &error) == CAI_OK);
  assert(owned->describe(owned, &after, &local) == LC_OK);
  assert_daily_lease(&after.lease, renewed_at);
  assert(strcmp(before.lease.lease_id, after.lease.lease_id) == 0);
  assert(before.lease.fencing_token == after.lease.fencing_token);
  /* A slow control operation may retain ownership_lock. The UI's health
   * snapshot remains available independently, without waiting for disk I/O. */
  pthread_mutex_lock(&owner->ownership_lock);
  assert(pthread_create(&renewal, NULL, check_with_renewal_busy, owner) == 0);
  assert(pthread_join(renewal, NULL) == 0);
  pthread_mutex_unlock(&owner->ownership_lock);
  /* A deliberate external takeover invalidates the old runtime. No silent
   * reacquisition, writes, or stale release may affect the replacement owner.
   */
  request.owner = "external-replacement";
  assert(public->takeover(public, &request, after.lease.lease_id, &replacement,
                          &local) == LC_OK);
  pthread_mutex_lock(&owner->ownership_lock);
  collection_owners_renew(owner);
  pthread_mutex_unlock(&owner->ownership_lock);
  assert(cai_cli_pouch_ownership_check(owner, &error) == CAI_ERR_TRANSPORT);
  clear_lease_error(&error);
  notification.fd = cai_cli_pouch_ownership_fd(owner);
  notification.events = POLLIN;
  notification.revents = 0;
  assert(poll(&notification, 1U, 0) == 1);
  event.sequence = 2U;
  assert(append_event(owner, "/work/leased", "leased-session", &event,
                      &error) != CAI_OK);
  clear_lease_error(&error);
  cai_cli_pouch_close(owner);
  assert(replacement->describe(replacement, &after, &local) == LC_OK);
  assert(after.lease.active && strcmp(after.lease.owner, request.owner) == 0);
  assert(after.live_count == 2U); /* One commit and one event. */
  assert(replacement->release(replacement, &local) == LC_OK);
  lc_collection_close(public);
  cai_cli_pouch_close(peer);
  lc_error_cleanup(&local);
  cai_error_cleanup(&error);
  free(peer);
  free(owner);
}

static void collection_crash_takeover(void) {
  cai_cli_pouch *pouch = calloc(1U, sizeof(*pouch));
  cai_error error;
  lc_error local;
  lc_collection *owned = NULL;
  lc_collection_info before, after;
  cai_source *source = NULL;
  unsigned long long sequence;
  struct pollfd ready;
  int pipes[2], status;
  char token;
  pid_t child;
  assert(pouch != NULL && pipe(pipes) == 0);
  cai_error_init(&error);
  lc_error_init(&local);
  child = fork();
  assert(child >= 0);
  if (child == 0) {
    close(pipes[0]);
    assert(cai_cli_pouch_open(pouch, NULL, NULL, NULL, &error) == CAI_OK);
    cai_cli_pouch_begin_use(pouch);
    save(pouch, "/work/lease-crash", "lease-crash", &error);
    assert(write(pipes[1], "R", 1U) == 1);
    close(pipes[1]);
    for (;;)
      pause();
  }
  close(pipes[1]);
  ready.fd = pipes[0];
  ready.events = POLLIN;
  ready.revents = 0;
  assert(poll(&ready, 1U, 10000) == 1 && read(pipes[0], &token, 1U) == 1);
  close(pipes[0]);
  assert(cai_cli_pouch_open(pouch, NULL, NULL, NULL, &error) == CAI_OK);
  assert(open_history_collection(pouch, "lease-crash", "events", 0, 0, &owned,
                                 &error) == CAI_OK);
  assert(owned->describe(owned, &before, &local) == LC_OK &&
         before.lease.active);
  lc_collection_close(owned);
  owned = NULL;
  cai_cli_pouch_begin_use(pouch);
  assert(pouch->store.load_id(pouch, "/work/lease-crash", "lease-crash",
                              &source, &sequence, &error) != CAI_OK &&
         source == NULL);
  clear_lease_error(&error);
  cai_cli_pouch_end_use(pouch);
  assert(kill(child, SIGKILL) == 0 && waitpid(child, &status, 0) == child);
  assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
  cai_cli_pouch_begin_use(pouch);
  /* No TTL sleep: reclaim the observed lease after process death. */
  assert(pouch->store.load_id(pouch, "/work/lease-crash", "lease-crash",
                              &source, &sequence, &error) == CAI_OK &&
         source != NULL);
  cai_source_close(source);
  assert(collection_owner_open(pouch, "lease-crash", "events", 0, 0, &owned,
                               &error) == CAI_OK);
  assert(owned->describe(owned, &after, &local) == LC_OK && after.lease.active);
  assert(strcmp(before.lease.lease_id, after.lease.lease_id) != 0);
  assert(after.lease.fencing_token > before.lease.fencing_token);
  assert(after.live_count == before.live_count);
  cai_cli_pouch_end_use(pouch);
  assert(open_history_collection(pouch, "lease-crash", "events", 0, 0, &owned,
                                 &error) == CAI_OK);
  assert(owned->describe(owned, &after, &local) == LC_OK &&
         !after.lease.active);
  lc_collection_close(owned);
  cai_cli_pouch_close(pouch);
  cai_error_cleanup(&error);
  lc_error_cleanup(&local);
  free(pouch);
}
