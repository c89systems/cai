/* Authorization is not a derived session index. The policy record survives
 * session expiry/deletion; the approving journal is its audit. Imported events
 * and model output never write authorization records. */
typedef struct workspace_approval {
  char workspace[PATH_MAX];
  char session_id[CAI_AGENT_SESSION_ID_MAX];
  lonejson_int64 approved;
} workspace_approval;
static const lonejson_field approval_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(workspace_approval, workspace, "workspace",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED_REQ(workspace_approval, session_id,
                                    "session_id", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_I64_REQ(workspace_approval, approved, "approved")};
LONEJSON_MAP_DEFINE(approval_map, workspace_approval, approval_fields);
static int approval_key(const char *workspace, char key[74], cai_error *error) {
  memcpy(key, "approval-", 9U);
  return latest_key(workspace, key + 9U, error);
}
int cai_cli_pouch_workspace_approval(cai_cli_pouch *pouch,
                                     const char *workspace, char *root,
                                     size_t capacity, cai_error *error) {
  workspace_approval *approval = calloc(1U, sizeof(*approval));
  lc_get_opts options;
  lc_get_res result;
  lc_error local;
  char key[74], *slash;
  int rc = CAI_OK;
  if (approval == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM,
                         "allocate workspace approval lookup");
  if (workspace == NULL || workspace[0] != '/' || root == NULL ||
      strlen(workspace) >= capacity) {
    free(approval);
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "canonical workspace approval path required");
  }
  strcpy(root, workspace);
  lc_get_opts_init(&options);
  options.public_read = 1;
  lc_error_init(&local);
  for (;;) {
    memset(approval, 0, sizeof(*approval));
    memset(&result, 0, sizeof(result));
    rc = approval_key(root, key, error);
    if (rc == CAI_OK &&
        lc_load_in_namespace(pouch->sessions, pouch->scopes_namespace, key,
                             &approval_map, approval, &options, &result,
                             &local) != LC_OK) {
      if (local.http_status == 404) {
        lc_error_cleanup(&local);
        lc_error_init(&local);
      } else
        rc = cai_cli_lc_error(error, &local);
    }
    lc_get_res_cleanup(&result);
    if (rc == CAI_OK && approval->approved == 1 &&
        (strcmp(approval->workspace, root) != 0 ||
         approval->session_id[0] == '\0'))
      rc = cai_cli_error(error, CAI_ERR_INVALID,
                         "invalid workspace approval identity");
    if (rc == CAI_OK && approval->workspace[0] != '\0' &&
        strcmp(approval->workspace, root) != 0)
      rc = cai_cli_error(error, CAI_ERR_INVALID,
                         "workspace approval scope mismatch");
    if (rc != CAI_OK || approval->approved == 1)
      break;
    if (strcmp(root, "/") == 0) {
      root[0] = '\0';
      break;
    }
    slash = strrchr(root, '/');
    if (slash == root)
      root[1] = '\0';
    else
      *slash = '\0';
  }
  lc_error_cleanup(&local);
  free(approval);
  return rc;
}
int cai_cli_pouch_approve_workspace(cai_cli_pouch *pouch, const char *workspace,
                                    const char *session_id, cai_error *error) {
  workspace_approval *approval = calloc(1U, sizeof(*approval));
  lc_lease *lease = NULL;
  cai_source *source = NULL;
  char key[74];
  int rc;
  if (approval == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate workspace approval");
  if (workspace == NULL || workspace[0] != '/' ||
      strlen(workspace) >= sizeof(approval->workspace) || session_id == NULL ||
      session_id[0] == '\0' ||
      strlen(session_id) >= sizeof(approval->session_id)) {
    free(approval);
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "invalid workspace approval identity");
  }
  strcpy(approval->workspace, workspace);
  strcpy(approval->session_id, session_id);
  approval->approved = 1;
  rc = approval_key(workspace, key, error);
  if (rc == CAI_OK)
    rc = acquire_ns(pouch, pouch->scopes_namespace, key, &lease, error);
  if (rc == CAI_OK)
    rc = cai_cli_mapped_source(&approval_map, approval, &source, error);
  if (rc == CAI_OK)
    rc = update(lease, source, error);
  cai_source_close(source);
  rc = release(lease, rc, error);
  free(approval);
  return rc;
}
