/** Owner-thread activation: retain the old receiver until candidate validation
 * and publication finish. Session context and accepted queue entries move by
 * ownership; no serialization or second restore is used for transfer. */
static int cai_runtime_bind_value(cai_agent_runtime *runtime, const char *name,
                                  const char *revision, int force,
                                  cai_error *error) {
  cai_session_binding *binding = &CAI_SESSION_IMPL(runtime->session)->binding;
  cai_session_binding previous = *binding;
  char backend[65];
  int rc;
  if (name == NULL || *name == '\0' || strlen(name) >= sizeof(binding->name) ||
      strspn(name, "abcdefghijklmnopqrstuvwxyz0123456789_-") != strlen(name) ||
      revision == NULL || strlen(revision) >= sizeof(binding->revision) ||
      strspn(revision, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ012"
                       "3456789._-") != strlen(revision))
    return cai_set_error(error, CAI_ERR_INVALID,
                         "invalid agent binding name or revision");
  rc = cai_runtime_binding_hash(runtime, backend, error);
  if (rc != CAI_OK)
    return rc;
  if (binding->backend[0] != '\0' && strcmp(binding->backend, backend) != 0)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "agent backend/account differs from the saved "
                         "context; provider conversion is not supported");
  if (strcmp(binding->name, name) != 0 ||
      strcmp(binding->revision, revision) != 0) {
    binding->instructions[0] = '\0';
  }
  strcpy(binding->name, name);
  strcpy(binding->revision, revision);
  strcpy(binding->backend, backend);
  rc = cai_runtime_record_binding(runtime, error);
  if (rc == CAI_OK &&
      (force || memcmp(&previous, binding, sizeof(previous)) != 0))
    rc = cai_runtime_commit(runtime, 0, error);
  if (rc != CAI_OK)
    *binding = previous;
  return rc;
}

int cai_agent_runtime_bind_agent(cai_agent_runtime *runtime, const char *name,
                                 const char *revision, cai_error *error) {
  int rc = cai_runtime_owner(runtime, error);
  if (rc != CAI_OK)
    return rc;
  pthread_mutex_lock(&runtime->lock);
  rc = cai_runtime_history_ready(runtime, error);
  if (rc == CAI_OK &&
      (runtime->review_mode || runtime->parent_runtime != NULL ||
       runtime->terminal_event_pending))
    rc = cai_set_error(error, CAI_ERR_INVALID,
                       "agent binding requires an idle ordinary runtime");
  if (rc == CAI_OK)
    runtime->model_switching = 1;
  pthread_mutex_unlock(&runtime->lock);
  if (rc != CAI_OK)
    return rc;
  rc = cai_runtime_bind_value(runtime, name, revision, 0, error);
  pthread_mutex_lock(&runtime->lock);
  runtime->model_switching = 0;
  pthread_cond_broadcast(&runtime->condition);
  pthread_mutex_unlock(&runtime->lock);
  return rc;
}

int cai_agent_runtime_get_agent(cai_agent_runtime *runtime,
                                cai_agent_runtime_binding *out,
                                cai_error *error) {
  int rc = cai_runtime_owner(runtime, error);
  if (rc != CAI_OK)
    return rc;
  if (out == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "agent binding output required");
  pthread_mutex_lock(&runtime->lock);
  rc = cai_runtime_history_ready(runtime, error);
  if (rc == CAI_OK) {
    const cai_session_binding *binding =
        &CAI_SESSION_IMPL(runtime->session)->binding;
    strcpy(out->name, binding->name);
    strcpy(out->revision, binding->revision);
    out->sequence = binding->sequence;
  }
  pthread_mutex_unlock(&runtime->lock);
  return rc;
}

/** The session shell stays with its registered goal handlers. Its private
 * context changes owner, and every borrowed runtime/agent hook is rebound. */
static void cai_runtime_activation_session(cai_agent_runtime *runtime) {
  cai_session_impl *session = CAI_SESSION_IMPL(runtime->session);
  session->agent = runtime->agent;
  session->todo = runtime->todo;
  session->input_checkpoint_context = runtime;
  session->retry_request_context = runtime;
}

static void cai_runtime_activation_swap(cai_agent_runtime *a,
                                        cai_agent_runtime *b) {
  void *context = a->session->impl;
  a->session->impl = b->session->impl;
  b->session->impl = context;
#define MOVE(type, member)                                                     \
  {                                                                            \
    type temporary = a->member;                                                \
    a->member = b->member;                                                     \
    b->member = temporary;                                                     \
  }
  MOVE(char *, session_id);
  MOVE(cai_agent_session_store, local_store);
  MOVE(const cai_agent_session_store *, session_store);
  MOVE(int, owns_local_store);
  MOVE(int, record_transcript);
  MOVE(cai_input_queue *, input_queue);
  MOVE(cai_runtime_input_origin *, input_origins);
  MOVE(unsigned long long, next_event_sequence);
  MOVE(unsigned long long, commit_event_sequence);
  MOVE(unsigned long long, applied_event_sequence);
  MOVE(unsigned long long, next_sequence);
  MOVE(unsigned long long, journal_v2_start_sequence);
  MOVE(int, history_compaction_pending);
  MOVE(int, history_turn_open);
  MOVE(unsigned int, goal_empty_turns);
  MOVE(int, has_last_turn_timing);
  MOVE(unsigned long long, last_turn_duration_ms);
  MOVE(long long, last_turn_finished_unix_seconds);
#undef MOVE
  if (a->owns_local_store)
    a->session_store = &a->local_store;
  if (b->owns_local_store)
    b->session_store = &b->local_store;
  cai_runtime_activation_session(a);
  cai_runtime_activation_session(b);
}

int cai_agent_runtime_activate(cai_agent_runtime *current, cai_client *client,
                               const cai_agent_runtime_config *config,
                               const char *name, const char *revision,
                               cai_agent_runtime **out, cai_error *error) {
  cai_agent_runtime_config *prepared = NULL;
  cai_agent_runtime *candidate = NULL;
  const cai_allocator *old_allocator, *next_allocator;
  cai_session_binding binding;
  char old_backend[65], next_backend[65];
  char *model = NULL, *hash = NULL, *preset = NULL, *prompt = NULL;
  int rc = cai_runtime_owner(current, error), swapped = 0, rebound = 0;
  unsigned long long next_sequence = 0U;
  if (out != NULL)
    *out = NULL;
  if (rc != CAI_OK)
    return rc;
  if (out == NULL || config == NULL || client == NULL)
    return cai_set_error(
        error, CAI_ERR_INVALID,
        "activation requires client, configuration and output");
  pthread_mutex_lock(&current->lock);
  rc = cai_runtime_history_ready(current, error);
  if (rc == CAI_OK &&
      cai_terminal_tools_busy_internal(CAI_AGENT_IMPL(current->agent)->tools))
    rc = cai_set_error(
        error, CAI_ERR_INVALID,
        "finish or stop the managed command before changing agents");
  if (rc == CAI_OK &&
      (current->review_mode || current->parent_runtime != NULL ||
       current->journal_parent != NULL || current->terminal_event_pending ||
       current->event_head != NULL))
    rc = cai_set_error(error, CAI_ERR_INVALID,
                       "agent activation requires a settled ordinary runtime");
  if (rc == CAI_OK)
    current->model_switching = 1;
  pthread_mutex_unlock(&current->lock);
  if (rc != CAI_OK)
    return rc;
  old_allocator = &CAI_CLIENT_IMPL(current->client)->allocator;
  next_allocator = &CAI_CLIENT_IMPL(client)->allocator;
  if (old_allocator->malloc_fn != next_allocator->malloc_fn ||
      old_allocator->realloc_fn != next_allocator->realloc_fn ||
      old_allocator->free_fn != next_allocator->free_fn ||
      old_allocator->context != next_allocator->context) {
    rc = cai_set_error(
        error, CAI_ERR_INVALID,
        "agent activation requires compatible session allocators");
    goto done;
  }
  prepared = cai_alloc(NULL, sizeof(*prepared));
  if (prepared == NULL) {
    rc = cai_set_error(error, CAI_ERR_NOMEM,
                       "allocate activation configuration");
    goto done;
  }
  *prepared = *config;
  prepared->session_store = NULL;
  prepared->disable_default_session_store = 1;
  prepared->resume_latest = 0;
  prepared->resume_session_id = prepared->session_id = NULL;
  prepared->record_workspace_approval = 0;
  prepared->record_transcript = 0;
  prepared->event_callback = NULL;
  prepared->synchronize_input_boundaries = 0;
  prepared->replay_callback = NULL;
  rc = cai_agent_runtime_open(client, prepared, &candidate, error);
  if (rc != CAI_OK)
    goto done;
  pthread_mutex_lock(&candidate->lock);
  candidate->model_switching = 1;
  pthread_mutex_unlock(&candidate->lock);
  if (candidate->review_mode ||
      strcmp(candidate->workspace_directory, current->workspace_directory) !=
          0 ||
      strcmp(candidate->session_scope, current->session_scope) != 0) {
    rc = cai_set_error(
        error, CAI_ERR_INVALID,
        "activation cannot change workspace, session scope or isolation");
    goto done;
  }
  rc = cai_runtime_binding_hash(current, old_backend, error);
  if (rc == CAI_OK)
    rc = cai_runtime_binding_hash(candidate, next_backend, error);
  if (rc == CAI_OK && strcmp(old_backend, next_backend) != 0)
    rc = cai_set_error(error, CAI_ERR_INVALID,
                       "agent backend/account differs from the saved context; "
                       "provider conversion is not supported");
  if (rc == CAI_OK && cai_runtime_model_switch_requires_compaction(
                          current, candidate->model_compaction_hash,
                          candidate->model_context_window,
                          candidate->model_auto_compact_token_limit))
    rc = cai_set_error(error, CAI_ERR_INVALID,
                       "agent model requires context conversion; select its "
                       "model before changing agents");
  if (rc != CAI_OK)
    goto done;
  {
    cai_session_impl *session = CAI_SESSION_IMPL(current->session);
    binding = session->binding;
    model = cai_strdup(old_allocator, session->state_model);
    hash = cai_strdup(old_allocator, session->state_model_compaction_hash);
    preset = cai_strdup(old_allocator, session->state_preset_name);
    prompt = cai_strdup(old_allocator, session->state_preset_prompt_version);
    if ((session->state_model != NULL && model == NULL) ||
        (session->state_model_compaction_hash != NULL && hash == NULL) ||
        (session->state_preset_name != NULL && preset == NULL) ||
        (session->state_preset_prompt_version != NULL && prompt == NULL)) {
      rc = cai_set_error(error, CAI_ERR_NOMEM,
                         "retain activation rollback metadata");
      goto done;
    }
  }
  cai_runtime_activation_swap(current, candidate);
  swapped = 1;
  if (candidate->input_queue != NULL) {
    rc = cai_input_queue_rebind(candidate->input_queue, current, candidate,
                                error);
    rebound = rc == CAI_OK;
  }
  if (rc == CAI_OK)
    rc = cai_runtime_set_session_preset_metadata(candidate, error);
  if (rc == CAI_OK)
    rc = cai_runtime_refresh_goal_projection(candidate, error);
  if (rc == CAI_OK && candidate->todo != NULL &&
      CAI_SESSION_IMPL(candidate->session)->state_todo_board != NULL)
    rc = cai_todo_select(candidate->todo,
                         CAI_SESSION_IMPL(candidate->session)->state_todo_board,
                         1, error);
  if (rc == CAI_OK)
    rc = cai_runtime_bind_value(candidate, name, revision, 0, error);
  if (rc == CAI_OK) {
    candidate->event_callback = config->event_callback;
    candidate->event_context = config->event_context;
    candidate->synchronize_input_boundaries =
        config->synchronize_input_boundaries;
    pthread_mutex_lock(&candidate->lock);
    candidate->model_switching = 0;
    pthread_cond_broadcast(&candidate->condition);
    pthread_mutex_unlock(&candidate->lock);
    *out = candidate;
    candidate = NULL;
  }
done:
  if (rc != CAI_OK && swapped) {
    cai_session_impl *session = CAI_SESSION_IMPL(candidate->session);
    cai_free_mem(old_allocator, session->state_model);
    cai_free_mem(old_allocator, session->state_model_compaction_hash);
    cai_free_mem(old_allocator, session->state_preset_name);
    cai_free_mem(old_allocator, session->state_preset_prompt_version);
    session->state_model = model;
    model = NULL;
    session->state_model_compaction_hash = hash;
    hash = NULL;
    session->state_preset_name = preset;
    preset = NULL;
    session->state_preset_prompt_version = prompt;
    prompt = NULL;
    session->binding = binding;
    next_sequence = candidate->next_event_sequence;
    if (rebound)
      (void)cai_input_queue_rebind(candidate->input_queue, candidate, current,
                                   NULL);
    cai_runtime_activation_swap(current, candidate);
    /* Orphaned append-only provenance chunks cannot reuse an event identity. */
    if (next_sequence > current->next_event_sequence)
      current->next_event_sequence = next_sequence;
  }
  cai_agent_runtime_close(candidate);
  cai_free_mem(NULL, prepared);
  cai_free_mem(old_allocator, model);
  cai_free_mem(old_allocator, hash);
  cai_free_mem(old_allocator, preset);
  cai_free_mem(old_allocator, prompt);
  pthread_mutex_lock(&current->lock);
  current->model_switching = 0;
  pthread_cond_broadcast(&current->condition);
  pthread_mutex_unlock(&current->lock);
  return rc;
}
