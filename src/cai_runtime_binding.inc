/** Definition provenance is separate from inference history. A commit refers
 * to one bounded instruction stream; ordinary commits retain only its digest
 * and first sequence. Failed publications cannot replace a committed binding.
 */
static int cai_runtime_bind_value(cai_agent_runtime *, const char *,
                                  const char *, int, cai_error *);
static int cai_runtime_binding_hash(cai_agent_runtime *runtime, char out[65],
                                    cai_error *error) {
  cai_client_impl *client = CAI_CLIENT_IMPL(runtime->client);
  EVP_MD_CTX *hash = EVP_MD_CTX_new();
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int length = 0U;
  const char *fields[7];
  char protocol[32], *account = NULL;
  size_t i;
  int rc = CAI_OK;
  if (client->chatgpt_auth != NULL && client->chatgpt_auth->account_id != NULL)
    rc =
        client->chatgpt_auth->account_id(client->chatgpt_auth, &account, error);
  snprintf(protocol, sizeof(protocol), "%d", (int)client->api_protocol);
  fields[0] = protocol;
  fields[1] = client->base_url;
  fields[2] = client->metadata_provider;
  fields[3] = client->organization_id;
  fields[4] = client->project_id;
  fields[5] = account;
  /* API-key rotation is not evidence of a different account. Explicit
   * organization/project IDs and OAuth account identity define that boundary.
   */
  fields[6] = client->chatgpt_auth == NULL ? "api_key" : "chatgpt";
  if (rc == CAI_OK &&
      (hash == NULL || EVP_DigestInit_ex(hash, EVP_sha256(), NULL) != 1))
    rc = cai_set_error(error, CAI_ERR_NOMEM,
                       "initialize backend identity digest");
  for (i = 0U; rc == CAI_OK && i < sizeof(fields) / sizeof(fields[0]); ++i) {
    const char *field = fields[i] != NULL ? fields[i] : "";
    if (EVP_DigestUpdate(hash, field, strlen(field) + 1U) != 1)
      rc = cai_set_error(error, CAI_ERR_TRANSPORT, "hash backend identity");
  }
  if (rc == CAI_OK &&
      (EVP_DigestFinal_ex(hash, digest, &length) != 1 || length != 32U))
    rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                       "finish backend identity digest");
  if (rc == CAI_OK)
    for (i = 0U; i < length; ++i)
      snprintf(out + i * 2U, 3U, "%02x", (unsigned int)digest[i]);
  EVP_MD_CTX_free(hash);
  cai_string_destroy(account);
  return rc;
}

static int cai_runtime_record_binding(cai_agent_runtime *runtime,
                                      cai_error *error) {
  cai_session_binding *binding = &CAI_SESSION_IMPL(runtime->session)->binding;
  cai_shared_text *text = CAI_AGENT_IMPL(runtime->agent)->instruction_text;
  cai_runtime_execution_stream *stream;
  unsigned char digest[SHA256_DIGEST_LENGTH];
  char hash[65], title[256], identity[80];
  size_t i;
  unsigned long long sequence;
  int rc;
  if (binding->name[0] == '\0' || runtime->session_store == NULL)
    return CAI_OK;
  if (SHA256((const unsigned char *)(text != NULL ? text->data : ""),
             text != NULL ? text->length : 0U, digest) == NULL)
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "hash instruction provenance");
  for (i = 0U; i < sizeof(digest); ++i)
    snprintf(hash + 2U * i, 3U, "%02x", (unsigned int)digest[i]);
  if (binding->sequence != 0U && strcmp(binding->instructions, hash) == 0)
    return CAI_OK;
  stream = cai_alloc(NULL, sizeof(*stream));
  if (stream == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "allocate instruction provenance stream");
  cai_runtime_execution_init(stream, runtime, "agent.activation", NULL, NULL,
                             "instructions");
  stream->journal_type = "agent_activation";
  stream->journal_only = 1;
  pthread_mutex_lock(&runtime->lock);
  sequence = runtime->next_event_sequence + 1U;
  pthread_mutex_unlock(&runtime->lock);
  snprintf(identity, sizeof(identity), "agent:%llu:%llu", sequence,
           binding->sequence);
  stream->record.call_id = identity;
  snprintf(title, sizeof(title), "# Agent %s (%s)\n\n", binding->name,
           binding->revision);
  rc = cai_runtime_execution_write(stream, title, strlen(title), error);
  if (rc == CAI_OK && text != NULL)
    rc = cai_runtime_execution_write(stream, text->data, text->length, error);
  if (rc == CAI_OK)
    rc = cai_runtime_execution_emit(stream, stream->length, 1, error);
  if (rc == CAI_OK) {
    strcpy(binding->instructions, hash);
    binding->sequence = sequence;
    pthread_mutex_lock(&runtime->lock);
    if (runtime->next_event_sequence > runtime->commit_event_sequence)
      runtime->commit_event_sequence = runtime->next_event_sequence;
    pthread_mutex_unlock(&runtime->lock);
  }
  cai_free_mem(NULL, stream);
  return rc;
}
