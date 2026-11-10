/* Stable append receivers and descriptor hints for one app session and
 * its children. The caller holds journal_lock through use. No metadata lease
 * remains held while sampling; public descriptor reads remain available. */
static int journal_context_open(cai_cli_pouch *pouch, const char *scope,
                                const char *id, cai_cli_journal_context **out,
                                cai_error *error) {
  cai_cli_journal_context *entry = journal_context_find(pouch, id);
  lc_lease *lease = NULL;
  long seconds;
  int rc;
  *out = NULL;
  if (entry != NULL) {
    if (strcmp(scope, entry->session.workspace) != 0)
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "session belongs to another directory");
    *out = entry;
    return CAI_OK;
  }
  rc = history_supported(pouch, error);
  if (rc != CAI_OK)
    return rc;
  entry = calloc(1U, sizeof(*entry));
  if (entry == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate journal context");
  /* Also pins internal child sessions against deletion and stale-index
   * cleanup. Shared public readers remain permitted. Process exit releases
   * the native guard; there is no expiring per-delta metadata lease. */
  rc = session_guard_open(pouch, id, 0, &entry->guard, NULL, error);
  if (rc == CAI_OK)
    rc = acquire(pouch, id, &lease, error);
  if (rc == CAI_OK)
    rc = get_session(lease, scope, id, &entry->session, error);
  if (rc == CAI_OK)
    rc = session_retention(pouch, &entry->session, &seconds, error);
  if (rc == CAI_OK)
    rc = collection_owner_open(pouch, id, "events",
                               entry->session.commit_position == 0U, 0,
                               &entry->collection, error);
  if (rc == CAI_OK)
    rc = update_session(lease, &entry->session, error);
  rc = release(lease, rc, error);
  if (rc == CAI_OK) {
    entry->next = pouch->journal_contexts;
    pouch->journal_contexts = entry;
    *out = entry;
  } else {
    session_guard_close(entry->guard);
    free(entry);
  }
  return rc;
}

static void journal_contexts_close(cai_cli_pouch *pouch) {
  cai_cli_journal_context *entry;
  if (!pouch->journal_lock_initialized)
    return;
  pthread_mutex_lock(&pouch->journal_lock);
  while ((entry = pouch->journal_contexts) != NULL) {
    pouch->journal_contexts = entry->next;
    session_guard_close(entry->guard);
    free(entry);
  }
  pthread_mutex_unlock(&pouch->journal_lock);
}

/* SDK collection maintenance decides expiry. Clean only a derived hint whose
 * backing collection is actually gone, under the native deletion guard. No
 * application clock or secondary expiry policy is involved. */
static int journal_prune_missing_hint(cai_cli_pouch *pouch,
                                      const cai_cli_session *session,
                                      cai_error *error) {
  cai_cli_session_guard *guard = NULL;
  lc_collection *collection = NULL, *prompts = NULL;
  lc_lease *lease = NULL;
  lc_error local;
  cai_error guarded;
  int rc, busy = 0;
  if (strcmp(session->history_backend, "collections") != 0)
    return CAI_OK;
  cai_error_init(&guarded);
  rc = session_guard_open(pouch, session->id, 1, &guard, &busy, &guarded);
  if (rc != CAI_OK) {
    if (!busy)
      rc = cai_cli_error(error, rc, guarded.message);
    cai_error_cleanup(&guarded);
    return busy ? CAI_OK : rc;
  }
  cai_error_cleanup(&guarded);
  lc_error_init(&local);
  rc = open_history_collection(pouch, session->id, "events", 0, 1, &collection,
                               error);
  if (rc == CAI_OK && collection == NULL) {
    /* Never wait behind a crash-stale lease just to tidy a lookup hint. */
    cai_error_init(&guarded);
    rc = acquire_wait(pouch, NULL, session->id, 0L, &lease, &guarded);
    if (rc != CAI_OK) {
      if (pouch->logger != NULL) {
        pslog_field fields[2];
        fields[0] = pslog_str("session_id", session->id);
        fields[1] = pslog_str("detail", guarded.message != NULL
                                            ? guarded.message
                                            : "metadata unavailable");
        pouch->logger->warn(
            pouch->logger, "Stale session lookup cleanup deferred", fields, 2U);
      }
      cai_error_cleanup(&guarded);
      lc_error_cleanup(&local);
      session_guard_close(guard);
      return CAI_OK;
    }
    cai_error_cleanup(&guarded);
    rc = collection_owner_open(pouch, session->id, "prompts", 0, 1, &prompts,
                               error);
    if (rc == CAI_OK && prompts != NULL &&
        collection_owner_drop(pouch, prompts, &local) != LC_OK)
      rc = cai_cli_lc_error(error, &local);
    if (rc == CAI_OK && lease->state_etag != NULL &&
        lease->state_etag[0] != '\0' &&
        lease->remove(lease, NULL, &local) != LC_OK)
      rc = cai_cli_lc_error(error, &local);
    rc = release(lease, rc, error);
    if (rc == CAI_OK && unlinkat(guard->directory, session->id, 0) != 0 &&
        errno != ENOENT)
      rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "remove expired session registry entry");
  }
  collection_owner_remove(pouch, prompts);
  lc_collection_close(collection);
  lc_error_cleanup(&local);
  session_guard_close(guard);
  return rc;
}
