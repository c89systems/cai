/* CAI owns these metadata keys; other applications may read their public
 * state. Hold a native guard for each SDK lease's complete lifetime. A new
 * guard owner can therefore recover a dead predecessor through fenced SDK
 * operations, including on shared Pouch roots. Never infer death from a PID,
 * a timeout, or the logical owner name alone. */
typedef struct cai_cli_lease_guard {
  struct cai_cli_lease_guard *next;
  const lc_lease *lease;
  int fd;
} cai_cli_lease_guard;
static cai_cli_lease_guard *lease_guards;

static void lease_guard_close(cai_cli_lease_guard *guard) {
  if (guard != NULL) {
    if (guard->fd >= 0)
      close(guard->fd);
    free(guard);
  }
}

static int lease_guard_open(cai_cli_pouch *pouch, const char *ns,
                            const char *key, long wait_seconds,
                            cai_cli_lease_guard **out, cai_error *error) {
  cai_cli_lease_guard *guard;
  EVP_MD_CTX *hash;
  unsigned char digest[32];
  unsigned int length;
  char name[65];
  struct timespec start, now, pause_time;
  int root, directory, rc = CAI_OK;
  size_t i;
  *out = NULL;
  if (ns == NULL || key == NULL || ns[0] == '\0' || key[0] == '\0')
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "metadata namespace and key are required");
  if (pouch->root_directory[0] == '\0')
    return CAI_OK; /* Remote state uses the server's lease policy. */
  hash = EVP_MD_CTX_new();
  if (hash == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate lease guard hash");
  if (EVP_DigestInit_ex(hash, EVP_sha256(), NULL) != 1 ||
      EVP_DigestUpdate(hash, ns, strlen(ns) + 1U) != 1 ||
      EVP_DigestUpdate(hash, key, strlen(key)) != 1 ||
      EVP_DigestFinal_ex(hash, digest, &length) != 1 ||
      length != sizeof(digest))
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "hash metadata lease guard");
  EVP_MD_CTX_free(hash);
  if (rc != CAI_OK)
    return rc;
  for (i = 0U; i < sizeof(digest); i++)
    snprintf(name + i * 2U, sizeof(name) - i * 2U, "%02x", digest[i]);
  guard = calloc(1U, sizeof(*guard));
  if (guard == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate metadata lease guard");
  guard->fd = -1;
  root = open(pouch->root_directory,
              O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  directory = root < 0 ? -1 : session_private_directory(root, ".lease-guards");
  if (root >= 0)
    close(root);
  if (directory >= 0) {
    guard->fd = openat(directory, name,
                       O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    close(directory);
  }
  if (!session_private_fd(guard->fd, 0) ||
      clock_gettime(CLOCK_MONOTONIC, &start) != 0)
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                       "open private metadata ownership guard");
  while (rc == CAI_OK && flock(guard->fd, LOCK_EX | LOCK_NB) != 0) {
    if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) {
      rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "lock metadata ownership");
      break;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || wait_seconds <= 0L ||
        now.tv_sec - start.tv_sec > wait_seconds ||
        (now.tv_sec - start.tv_sec == wait_seconds &&
         now.tv_nsec >= start.tv_nsec)) {
      rc = cai_cli_error(error, CAI_ERR_INVALID,
                         "metadata is owned by another live CAI context");
      break;
    }
    pause_time.tv_sec = 0;
    pause_time.tv_nsec = 10000000L;
    (void)nanosleep(&pause_time, NULL);
  }
  if (rc == CAI_OK)
    *out = guard;
  else
    lease_guard_close(guard);
  return rc;
}

static int lease_guard_recover(cai_cli_pouch *pouch, const char *ns,
                               const char *key, lc_error *error) {
  lc_describe_req request;
  lc_describe_res observed;
  lc_release_op release_request;
  lc_release_res result;
  int rc;
  memset(&observed, 0, sizeof(observed));
  memset(&result, 0, sizeof(result));
  lc_describe_req_init(&request);
  lc_release_op_init(&release_request);
  request.ns = ns;
  request.key = key;
  rc = pouch->sessions->describe(pouch->sessions, &request, &observed, error);
  if (rc == LC_OK && observed.lease_id != NULL &&
      observed.lease_id[0] != '\0') {
    if (observed.owner == NULL ||
        strcmp(observed.owner, pouch->program_name) != 0)
      rc = lc_error_set(error, LC_ERR_SERVER, 409L,
                        "metadata lease belongs to a different owner", NULL,
                        "lease_held", NULL);
    else {
      release_request.lease.ns = ns;
      release_request.lease.key = key;
      release_request.lease.lease_id = observed.lease_id;
      release_request.lease.fencing_token = observed.fencing_token;
      release_request.lease.txn_id = observed.txn_id;
      release_request.rollback = 1;
      rc = pouch->sessions->release(pouch->sessions, &release_request, &result,
                                    error);
    }
  }
  lc_release_res_cleanup(&result);
  lc_describe_res_cleanup(&observed);
  return rc;
}

static cai_cli_lease_guard *lease_guard_take(const lc_lease *lease) {
  cai_cli_lease_guard **link, *found = NULL;
  pthread_mutex_lock(&clients_lock);
  for (link = &lease_guards; *link != NULL; link = &(*link)->next)
    if ((*link)->lease == lease) {
      found = *link;
      *link = found->next;
      break;
    }
  pthread_mutex_unlock(&clients_lock);
  return found;
}
