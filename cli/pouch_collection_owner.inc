/* SDK leases are the mutation authority. The native guard establishes only
 * process liveness, allowing conditional takeover of our own abandoned lease.
 * The persisted random label bridges a crash during acquire/takeover itself;
 * neither a PID nor a common application name is evidence of ownership. */
/* Keep ownership for the running context without frequent durable heartbeats.
 * The liveness guard permits immediate recovery despite this long TTL. */
#define COLLECTION_LEASE_SECONDS (24L * 3600L)
#define COLLECTION_RENEW_SECONDS (23L * 3600L)

typedef struct cai_cli_collection_owner {
  struct cai_cli_collection_owner *next;
  lc_collection *collection;
  cai_cli_lease_guard *guard;
  char name[192];
  int dropped;
} cai_cli_collection_owner;

static int collection_guard_owner(cai_cli_pouch *pouch,
                                  cai_cli_lease_guard *guard, const char *id,
                                  const char *kind, char owner[129],
                                  cai_error *error) {
  char token[33];
  unsigned char random[16];
  struct stat info;
  size_t i, offset;
  ssize_t count;
  int valid = 0;
  if (fstat(guard->fd, &info) != 0)
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "read collection liveness guard");
  if (info.st_size == 32 && pread(guard->fd, token, 32U, 0) == 32) {
    token[32] = '\0';
    valid = strspn(token, "0123456789abcdef") == 32U;
  }
  if (!valid && info.st_size != 0) {
    lc_collection *collection = NULL;
    lc_collection_info observed;
    lc_error local;
    int rc;
    lc_error_init(&local);
    rc = open_history_collection(pouch, id, kind, 0, 1, &collection, error);
    if (rc == CAI_OK && collection != NULL) {
      if (collection->describe(collection, &observed, &local) != LC_OK)
        rc = cai_cli_lc_error(error, &local);
      else if (observed.lease.active)
        rc =
            cai_cli_error(error, CAI_ERR_INVALID,
                          "Collection liveness record is damaged; wait for the "
                          "current lease to expire before reopening");
    }
    lc_collection_close(collection);
    lc_error_cleanup(&local);
    if (rc != CAI_OK)
      return rc;
  }
  if (!valid) {
    if (RAND_bytes(random, sizeof(random)) != 1)
      return cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "create collection owner identity");
    for (i = 0U; i < sizeof(random); i++)
      snprintf(token + i * 2U, sizeof(token) - i * 2U, "%02x",
               (unsigned int)random[i]);
    for (offset = 0U; offset < 32U; offset += (size_t)count) {
      do {
        count = pwrite(guard->fd, token + offset, 32U - offset, (off_t)offset);
      } while (count < 0 && errno == EINTR);
      if (count <= 0)
        return cai_cli_error(error, CAI_ERR_TRANSPORT,
                             "write collection owner identity");
    }
    if (ftruncate(guard->fd, 32) != 0 || fsync(guard->fd) != 0)
      return cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "persist collection owner identity");
  }
  snprintf(owner, 129U, "%s:%s", pouch->program_name, token);
  return CAI_OK;
}

static void collection_owner_failed(cai_cli_pouch *pouch,
                                    const lc_error *error) {
  char byte = 'L';
  ssize_t written;
  if (pouch->ownership_failed)
    return;
  pthread_mutex_lock(&pouch->ownership_health_lock);
  pouch->ownership_failed = 1;
  snprintf(pouch->ownership_error, sizeof(pouch->ownership_error),
           "Conversation collection ownership lost or renewal failed: %s. "
           "Close this instance and reopen the conversation.",
           error != NULL && error->message != NULL ? error->message
                                                   : "lease unavailable");
  pthread_mutex_unlock(&pouch->ownership_health_lock);
  if (pouch->ownership_pipe[1] >= 0) {
    do {
      written = write(pouch->ownership_pipe[1], &byte, 1U);
    } while (written < 0 && errno == EINTR);
  }
}

/* Called with ownership_lock; renewal never takes journal_lock or runs on the
 * UI thread. A failure is terminal for this restored conversation context. */
static void collection_owners_renew(cai_cli_pouch *pouch) {
  cai_cli_collection_owner *entry;
  lc_collection_lease_info info;
  lc_error local;
  lc_error_init(&local);
  for (entry = pouch->collection_owners;
       entry != NULL && !pouch->ownership_failed; entry = entry->next)
    if (!entry->dropped && entry->collection->keepalive(
                               entry->collection, COLLECTION_LEASE_SECONDS,
                               &info, &local) != LC_OK)
      collection_owner_failed(pouch, &local);
  lc_error_cleanup(&local);
}

static void *collection_owners_worker(void *context) {
  cai_cli_pouch *pouch = context;
  struct timespec deadline;
  int rc;
  pthread_mutex_lock(&pouch->ownership_lock);
  while (!pouch->ownership_stopping && !pouch->ownership_failed) {
    while (pouch->collection_owners == NULL && !pouch->ownership_stopping)
      pthread_cond_wait(&pouch->ownership_condition, &pouch->ownership_lock);
    if (pouch->ownership_stopping)
      break;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
      collection_owner_failed(pouch, NULL);
      break;
    }
    deadline.tv_sec += COLLECTION_RENEW_SECONDS;
    do {
      rc = pthread_cond_timedwait(&pouch->ownership_condition,
                                  &pouch->ownership_lock, &deadline);
    } while (rc == 0 && !pouch->ownership_stopping);
    if (!pouch->ownership_stopping) {
      if (rc == ETIMEDOUT)
        collection_owners_renew(pouch);
      else
        collection_owner_failed(pouch, NULL);
    }
  }
  pthread_mutex_unlock(&pouch->ownership_lock);
  return NULL;
}

static int collection_owners_init(cai_cli_pouch *pouch, cai_error *error) {
  pthread_condattr_t attr;
  int i, rc;
  pouch->ownership_pipe[0] = pouch->ownership_pipe[1] = -1;
  if (pthread_mutex_init(&pouch->ownership_lock, NULL) != 0)
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "initialize collection ownership mutex");
  if (pthread_mutex_init(&pouch->ownership_health_lock, NULL) != 0) {
    pthread_mutex_destroy(&pouch->ownership_lock);
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "initialize ownership health mutex");
  }
  rc = pthread_condattr_init(&attr);
  if (rc == 0) {
    rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (rc == 0)
      rc = pthread_cond_init(&pouch->ownership_condition, &attr);
    pthread_condattr_destroy(&attr);
  }
  if (rc != 0) {
    pthread_mutex_destroy(&pouch->ownership_health_lock);
    pthread_mutex_destroy(&pouch->ownership_lock);
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "initialize collection renewal condition");
  }
  pouch->ownership_initialized = 1;
  if (pipe(pouch->ownership_pipe) != 0)
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "create collection ownership wakeup");
  for (i = 0; i < 2; i++)
    if (fcntl(pouch->ownership_pipe[i], F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(pouch->ownership_pipe[i], F_SETFL, O_NONBLOCK) != 0)
      return cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "configure collection ownership wakeup");
  return CAI_OK;
}

int cai_cli_pouch_ownership_check(cai_cli_pouch *pouch, cai_error *error) {
  int rc = CAI_OK;
  if (!pouch->ownership_initialized)
    return CAI_OK;
  /* UI/batch health checks never wait for a lease-control I/O operation. */
  pthread_mutex_lock(&pouch->ownership_health_lock);
  if (pouch->ownership_failed)
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT, pouch->ownership_error);
  pthread_mutex_unlock(&pouch->ownership_health_lock);
  return rc;
}
int cai_cli_pouch_ownership_fd(cai_cli_pouch *pouch) {
  return pouch->ownership_initialized ? pouch->ownership_pipe[0] : -1;
}

/* Borrowed receiver, retained until end_use/close or explicit owner removal.
 * This is the only writable collection opener; ordinary readers never acquire.
 */
static int collection_owner_open(cai_cli_pouch *pouch, const char *id,
                                 const char *kind, int create, int optional,
                                 lc_collection **out, cai_error *error) {
  cai_cli_collection_owner *entry;
  lc_collection *plain = NULL, *owned = NULL;
  lc_collection_info info;
  lc_collection_lease_req request;
  lc_error local;
  char name[192], key[208], owner[129];
  int rc, created = 0;
  *out = NULL;
  rc = journal_write_ready(pouch, error);
  if (rc == CAI_OK)
    rc = history_supported(pouch, error);
  if (rc != CAI_OK)
    return rc;
  snprintf(name, sizeof(name), "%s.%s", kind, id);
  snprintf(key, sizeof(key), "!collection.%s", name);
  pthread_mutex_lock(&pouch->ownership_lock);
  if (pouch->ownership_failed) {
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT, pouch->ownership_error);
    pthread_mutex_unlock(&pouch->ownership_lock);
    return rc;
  }
  for (entry = pouch->collection_owners; entry != NULL; entry = entry->next)
    if (strcmp(entry->name, name) == 0) {
      *out = entry->collection;
      pthread_mutex_unlock(&pouch->ownership_lock);
      return CAI_OK;
    }
  entry = calloc(1U, sizeof(*entry));
  if (entry == NULL) {
    pthread_mutex_unlock(&pouch->ownership_lock);
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate collection ownership");
  }
  lc_error_init(&local);
  rc = lease_guard_open(pouch, pouch->sessions_namespace, key, 0L,
                        &entry->guard, error);
  if (rc == CAI_ERR_INVALID && error != NULL && error->message != NULL &&
      strstr(error->message, "live CAI context") != NULL)
    rc = cai_cli_error(error, CAI_ERR_INVALID,
                       "Conversation collection is owned by another live CAI "
                       "context; close that instance before resuming");
  if (rc == CAI_OK)
    rc = collection_guard_owner(pouch, entry->guard, id, kind, owner, error);
  lc_collection_lease_req_init(&request);
  request.owner = owner;
  request.ttl_seconds = COLLECTION_LEASE_SECONDS;
  if (rc == CAI_OK)
    rc = open_history_collection_options(pouch, id, kind, create, optional,
                                         &request, &created, &plain, error);
  if (rc == CAI_OK && plain != NULL) {
    if (created) {
      owned = plain;
      plain = NULL;
    } else {
      rc = plain->describe(plain, &info, &local);
      if (rc == LC_OK && info.lease.active &&
          strcmp(info.lease.owner, owner) == 0)
        rc = plain->takeover(plain, &request, info.lease.lease_id, &owned,
                             &local);
      else if (rc == LC_OK)
        rc = plain->acquire(plain, &request, &owned, &local);
      if (rc != LC_OK)
        rc = cai_cli_lc_error(error, &local);
    }
  }
  if (rc == CAI_OK && owned != NULL) {
    entry->collection = owned;
    strcpy(entry->name, name);
    entry->next = pouch->collection_owners;
    pouch->collection_owners = entry;
    if (!pouch->ownership_running) {
      if (pthread_create(&pouch->ownership_thread, NULL,
                         collection_owners_worker, pouch) != 0) {
        pouch->collection_owners = entry->next;
        rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "start collection renewal service");
      } else
        pouch->ownership_running = 1;
    }
    if (rc == CAI_OK) {
      pthread_cond_signal(&pouch->ownership_condition);
      *out = owned;
    }
  }
  if (rc != CAI_OK || owned == NULL) {
    if (owned != NULL && owned->release(owned, &local) != LC_OK)
      lc_collection_close(owned);
    lease_guard_close(entry->guard);
    free(entry);
  }
  lc_collection_close(plain);
  lc_error_cleanup(&local);
  pthread_mutex_unlock(&pouch->ownership_lock);
  return rc;
}

static void collection_owner_remove(cai_cli_pouch *pouch,
                                    lc_collection *collection) {
  cai_cli_collection_owner **link, *entry = NULL;
  lc_error local;
  if (collection == NULL || !pouch->ownership_initialized)
    return;
  pthread_mutex_lock(&pouch->ownership_lock);
  for (link = &pouch->collection_owners; *link != NULL; link = &(*link)->next)
    if ((*link)->collection == collection) {
      entry = *link;
      *link = entry->next;
      break;
    }
  if (entry != NULL) {
    lc_error_init(&local);
    if (entry->dropped || collection->release(collection, &local) != LC_OK) {
      /* A failed release leaves ownership durable. The liveness guard lets
       * the next context inspect and conditionally reclaim exactly that owner.
       */
      if (!entry->dropped && pouch->logger != NULL)
        pouch->logger->warn(pouch->logger,
                            "Collection lease release failed; ownership will "
                            "be recovered on reopen",
                            NULL, 0U);
      lc_collection_close(collection);
    }
    lc_error_cleanup(&local);
    lease_guard_close(entry->guard);
    free(entry);
  }
  pthread_mutex_unlock(&pouch->ownership_lock);
}

static int collection_owner_drop(cai_cli_pouch *pouch,
                                 lc_collection *collection, lc_error *error) {
  cai_cli_collection_owner *entry;
  int rc;
  pthread_mutex_lock(&pouch->ownership_lock);
  rc = collection->drop(collection, error);
  if (rc == LC_OK)
    for (entry = pouch->collection_owners; entry != NULL; entry = entry->next)
      if (entry->collection == collection) {
        entry->dropped = 1;
        break;
      }
  pthread_mutex_unlock(&pouch->ownership_lock);
  return rc;
}

static void collection_owners_close(cai_cli_pouch *pouch) {
  if (!pouch->ownership_initialized)
    return;
  pthread_mutex_lock(&pouch->ownership_lock);
  pouch->ownership_stopping = 1;
  pthread_cond_signal(&pouch->ownership_condition);
  pthread_mutex_unlock(&pouch->ownership_lock);
  if (pouch->ownership_running)
    pthread_join(pouch->ownership_thread, NULL);
  while (pouch->collection_owners != NULL)
    collection_owner_remove(pouch, pouch->collection_owners->collection);
  pthread_mutex_lock(&pouch->ownership_lock);
  pthread_mutex_lock(&pouch->ownership_health_lock);
  pouch->ownership_running = pouch->ownership_stopping =
      pouch->ownership_failed = 0;
  pouch->ownership_error[0] = '\0';
  pthread_mutex_unlock(&pouch->ownership_health_lock);
  pthread_mutex_unlock(&pouch->ownership_lock);
  if (pouch->ownership_pipe[0] >= 0) {
    char bytes[64];
    while (read(pouch->ownership_pipe[0], bytes, sizeof(bytes)) > 0)
      ;
  }
}
