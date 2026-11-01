#define CAI_CUT_FAIL(e, m) cai_set_error((e), CAI_ERR_INVALID, (m))
#include "cai_session_cut.h"
#undef CAI_CUT_FAIL

typedef struct cai_local_cut {
  cai_cut_plan plan;
  unsigned long long ordinal;
  int output;
  char temporary[PATH_MAX];
} cai_local_cut;

static int cai_local_cut_record(void *context, int fd, long start, long end,
                                cai_error *error) {
  cai_local_cut *cut = context;
  unsigned char bytes[2048];
  long offset = start;
  size_t amount;
  ssize_t got;
  int rc = CAI_OK;
  cut->ordinal++;
  if (cut->output >= 0 && cai_cut_removed(&cut->plan, cut->ordinal))
    return CAI_OK;
  while (offset <= end && rc == CAI_OK) {
    amount = end - offset + 1L > (long)sizeof(bytes)
                 ? sizeof(bytes)
                 : (size_t)(end - offset + 1L);
    got = pread(fd, bytes, amount, (off_t)offset);
    if (got <= 0)
      return cai_set_error(error, CAI_ERR_TRANSPORT, "read history cut record");
    if (cut->output < 0)
      rc = cai_cut_feed(&cut->plan, bytes, (size_t)got, error);
    else
      rc = cai_store_write_all(cut->output, bytes, (size_t)got, error);
    offset += got;
  }
  if (rc == CAI_OK && cut->output < 0)
    rc = cai_cut_end(&cut->plan, cut->ordinal, error);
  return rc;
}
static int
cai_local_session_trim(void *context, const char *scope, const char *id,
                       const cai_agent_session_trim_request *request,
                       cai_agent_session_trim_source_fn make_replacement,
                       void *replacement_context, unsigned long long sequence,
                       cai_error *error) {
  cai_local_session_store *store = context;
  cai_local_cut *cut;
  cai_source *replacement = NULL;
  lonejson_error local;
  char hash[65], filename[CAI_STORE_SESSION_FILENAME_MAX], prefix[160];
  char bytes[2048];
  const char *temporary_name;
  unsigned long long created_at = 0U;
  size_t n;
  int scope_fd = -1, input = -1, guard = -1, rc, published = 0;
  if (store == NULL || request == NULL || make_replacement == NULL)
    return cai_set_error(error, CAI_ERR_INVALID, "invalid history cut request");
  rc = cai_store_can_write(store, error);
  if (rc != CAI_OK)
    return rc;
  cut = calloc(1U, sizeof(*cut));
  if (cut == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "allocate local history cut");
  cut->output = -1;
  cut->plan.request = request;
  cut->plan.json = cai_cut_json(&local);
  rc = cut->plan.json != NULL
           ? CAI_OK
           : cai_set_error(error, CAI_ERR_NOMEM, "allocate history cut parser");
  if (rc == CAI_OK)
    rc = cai_store_session_filename_from_id(id, filename, error);
  if (rc == CAI_OK)
    rc = cai_store_open_scope(store, scope, &scope_fd, hash, error);
  if (rc == CAI_OK)
    rc = cai_store_mutation_lock(scope_fd, filename, &guard, error);
  if (rc == CAI_OK)
    rc = cai_store_can_write(store, error);
  if (rc == CAI_OK) {
    input = openat(scope_fd, filename, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    rc = input >= 0
             ? cai_store_validate_private_regular_fd(input, error)
             : cai_set_error(error, CAI_ERR_TRANSPORT, "open cut journal");
  }
  if (rc == CAI_OK)
    rc = cai_local_visit_complete_records(input, cai_local_cut_record, cut,
                                          error);
  if (rc == CAI_OK)
    rc = cai_cut_finish(&cut->plan, error);
  if (rc == CAI_OK)
    rc = make_replacement(replacement_context, cut->plan.revision,
                          (size_t)cut->plan.bytes, &replacement, error);
  if (rc == CAI_OK &&
      snprintf(cut->temporary, sizeof(cut->temporary), "%s/%s/.trim-XXXXXX",
               store->root_directory, hash) >= (int)sizeof(cut->temporary))
    rc = cai_set_error(error, CAI_ERR_INVALID, "history cut path too long");
  if (rc == CAI_OK) {
    cut->output = mkstemp(cut->temporary);
    if (cut->output < 0 || fcntl(cut->output, F_SETFD, FD_CLOEXEC) != 0)
      rc = cai_set_error(error, CAI_ERR_TRANSPORT, "create cut journal");
  }
  if (rc == CAI_OK) {
    cut->ordinal = 0U;
    rc = cai_local_visit_complete_records(input, cai_local_cut_record, cut,
                                          error);
  }
  if (rc == CAI_OK)
    rc = cai_store_commit_created_at_ns(&created_at, error);
  if (rc == CAI_OK) {
    (void)snprintf(prefix, sizeof(prefix),
                   "{\"record_type\":\"commit\",\"commit_created_at_ns\":%llu,"
                   "\"applied_event_sequence\":%llu,\"state\":",
                   created_at, sequence);
    rc = cai_store_write_all(cut->output, prefix, strlen(prefix), error);
  }
  while (rc == CAI_OK) {
    n = cai_source_read(replacement, bytes, sizeof(bytes), error);
    if (n == 0U)
      break;
    rc = cai_store_write_all(cut->output, bytes, n, error);
  }
  if (rc == CAI_OK && error != NULL && error->code != CAI_OK)
    rc = error->code;
  if (rc == CAI_OK)
    rc = cai_store_write_all(cut->output, "}\n", 2U, error);
  if (rc == CAI_OK && cai_store_sync_record(cut->output) != 0)
    rc = cai_set_error(error, CAI_ERR_TRANSPORT, "sync cut journal");
  if (rc == CAI_OK) {
    temporary_name = strrchr(cut->temporary, '/') + 1;
    if (renameat(scope_fd, temporary_name, scope_fd, filename) != 0)
      rc = cai_set_error(error, CAI_ERR_TRANSPORT, "publish cut journal");
    else
      published = 1;
  }
  if (rc == CAI_OK && fsync(scope_fd) != 0) {
    cai_store_uncertain(store);
    rc = cai_set_error(
        error, CAI_ERR_TRANSPORT,
        "history cut published but directory sync failed; reopen");
  }
  cai_source_close(replacement);
  if (cut->output >= 0)
    close(cut->output);
  if (!published && cut->output >= 0)
    (void)unlink(cut->temporary);
  if (input >= 0)
    close(input);
  if (guard >= 0)
    close(guard);
  if (scope_fd >= 0)
    close(scope_fd);
  cai_cut_record_close(&cut->plan);
  lonejson_free(cut->plan.json);
  free(cut);
  return rc;
}
