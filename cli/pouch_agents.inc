/* Stored definitions use ordinary state objects. Publish initial metadata and
 * its INI attachment in one lease; readers never own a session collection. */
typedef struct stored_agent_metadata {
  char kind[16], name[16];
} stored_agent_metadata;
static const lonejson_field stored_agent_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(stored_agent_metadata, kind, "kind",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED_REQ(stored_agent_metadata, name, "name",
                                    LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(stored_agent_map, stored_agent_metadata,
                    stored_agent_fields);

int cai_cli_pouch_default_agent(cai_cli_pouch *pouch, cai_cli_producer produce,
                                void *seed, cai_source **out,
                                cai_error *error) {
  lc_describe_req describe;
  lc_describe_res observed = {0};
  lc_error local;
  lc_lease *lease = NULL;
  cai_source *source = NULL;
  lc_source *input = NULL;
  lc_attach_req attachment;
  lc_attach_res attached = {0};
  stored_agent_metadata metadata = {"agent", "default"};
  int rc, exists;
  *out = NULL;
  lc_error_init(&local);
  lc_describe_req_init(&describe);
  describe.ns = pouch->agents_namespace;
  describe.key = "agent.default";
  rc = lc_describe(pouch->sessions, &describe, &observed, &local);
  exists = rc == LC_OK && observed.state_etag != NULL &&
           observed.state_etag[0] != '\0';
  if (rc != LC_OK && local.http_status != 404) {
    rc = cai_cli_lc_error(error, &local);
    goto done;
  }
  /* Missing is expected on first use; do not pass that stale SDK error into
   * attachment streaming callbacks belonging to the initialization lease. */
  lc_error_cleanup(&local);
  lc_error_init(&local);
  rc = CAI_OK;
  if (!exists) {
    rc = acquire_wait(pouch, pouch->agents_namespace, "agent.default", 30L,
                      &lease, error);
    if (rc == CAI_OK &&
        (lease->state_etag == NULL || lease->state_etag[0] == '\0')) {
      /* Attachments require an initialized parent in the private lease. Both
       * changes remain unpublished until the same successful release. */
      rc = cai_cli_mapped_source(&stored_agent_map, &metadata, &source, error);
      if (rc == CAI_OK)
        rc = update(lease, source, error);
      cai_source_close(source);
      source = NULL;
      if (rc == CAI_OK)
        rc = cai_cli_stream_source(produce, seed, NULL, &source, error);
      if (rc == CAI_OK)
        rc = cai_cli_lc_source(source, &input, error);
      lc_attach_req_init(&attachment);
      attachment.name = "config.ini";
      attachment.content_type = "text/plain";
      if (rc == CAI_OK && lc_lease_attach(lease, &attachment, input, &attached,
                                          &local) != LC_OK)
        rc = cai_cli_lc_error(error, &local);
      lc_source_close(input);
      input = NULL;
      cai_source_close(source);
      source = NULL;
    }
    rc = release_mode(lease, rc != CAI_OK, rc, error);
  }
  if (rc == CAI_OK)
    rc = download_source_ns(pouch->sessions, pouch->agents_namespace,
                            "agent.default", NULL, &source, error);
  if (rc == CAI_OK)
    rc = parse_source(source, &stored_agent_map, &metadata, error);
  cai_source_close(source);
  source = NULL;
  if (rc == CAI_OK && (strcmp(metadata.kind, "agent") != 0 ||
                       strcmp(metadata.name, "default") != 0))
    rc = cai_cli_error(error, CAI_ERR_INVALID,
                       "invalid stored default agent metadata");
  if (rc == CAI_OK)
    rc = download_source_ns(pouch->sessions, pouch->agents_namespace,
                            "agent.default", "config.ini", out, error);
done:
  lc_attach_res_cleanup(&attached);
  lc_describe_res_cleanup(&observed);
  lc_error_cleanup(&local);
  return rc;
}
