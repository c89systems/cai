static void *pouch_approval_worker(void *context) {
  static const char directory[] = "/approval/workspace";
  cai_cli_pouch *pouch = calloc(1U, sizeof(*pouch));
  workspace_approval *approval = calloc(1U, sizeof(*approval));
  cai_error error;
  lc_lease *lease;
  cai_source *source;
  char key[74], root[128];
  int mode;
  (void)context;
  assert(pouch != NULL && approval != NULL);
  cai_error_init(&error);
  assert(cai_cli_pouch_open(pouch, NULL, NULL, NULL, &error) == CAI_OK);
  real_query = pouch->sessions->query_keys;
  pouch->sessions->query_keys = query_count;
  query_calls = 0;
  assert(approval_key(directory, key, &error) == CAI_OK);
  assert(cai_cli_pouch_workspace_approval(pouch, directory, root, sizeof(root),
                                          &error) == CAI_OK &&
         root[0] == '\0');
  for (mode = 0; mode < 3; mode++) {
    memset(approval, 0, sizeof(*approval));
    approval->approved = 1;
    if (mode != 0)
      strcpy(approval->workspace, mode == 1 ? "/approval/other" : directory);
    if (mode != 2)
      strcpy(approval->session_id, "approval-audit");
    lease = NULL;
    source = NULL;
    assert(acquire_ns(pouch, pouch->scopes_namespace, key, &lease, &error) ==
           CAI_OK);
    assert(cai_cli_mapped_source(&approval_map, approval, &source, &error) ==
           CAI_OK);
    assert(update(lease, source, &error) == CAI_OK);
    cai_source_close(source);
    assert(release(lease, CAI_OK, &error) == CAI_OK);
    assert(cai_cli_pouch_workspace_approval(pouch, directory, root,
                                            sizeof(root),
                                            &error) == CAI_ERR_INVALID);
    cai_error_cleanup(&error);
    cai_error_init(&error);
  }
  assert(cai_cli_pouch_approve_workspace(pouch, directory, "approval-audit",
                                         &error) == CAI_OK);
  assert(cai_cli_pouch_workspace_approval(pouch, "/approval/workspace/sub/deep",
                                          root, sizeof(root),
                                          &error) == CAI_OK &&
         strcmp(root, directory) == 0);
  assert(cai_cli_pouch_workspace_approval(pouch, "/approval/workspace-other",
                                          root, sizeof(root),
                                          &error) == CAI_OK &&
         root[0] == '\0');
  assert(query_calls ==
         0); /* Authorization never rebuilds/scans session indexes. */
  pouch->sessions->query_keys = real_query;
  cai_cli_pouch_close(pouch);
  cai_error_cleanup(&error);
  free(approval);
  free(pouch);
  return NULL;
}
