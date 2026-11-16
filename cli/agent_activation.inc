/** Included by the workspace owner. One transition owns prepared configuration
 * and services until the core activation commits; the old environment remains
 * valid throughout failure handling and is retired only after success. */
typedef struct cli_agent_change {
  cai_cli_options previous_options, next_options;
  cai_config *previous_config, *next_config;
  cai_environment *previous_environment, *next_environment;
  cli_agent_services *previous_services, *next_services;
  cai_agent_runtime *previous_runtime;
  cai_cli_messages *previous_messages;
  char previous_name[65], previous_revision[21];
  char previous_id[CAI_AGENT_SESSION_ID_MAX];
  char next_name[65], next_revision[21];
  char **arguments;
} cli_agent_change;

static void cli_agent_temporary_options(cai_cli_options *next,
                                        const cai_cli_options *previous) {
  if (!previous->temporary)
    return;
#define KEEP(field) next->field = previous->field
  KEEP(directory);
  KEEP(state_dir);
  KEEP(cache_dir);
  KEEP(data_dir);
  KEEP(clipboard_dir);
  KEEP(log_dir);
  KEEP(pouch_key_file);
  KEEP(export_dir);
  KEEP(runtime_dir);
  KEEP(lockd);
  KEEP(lockd_client_pem);
  KEEP(server_persist);
#undef KEEP
}

static int cli_agent_change_open(cli_state *state, const char *name,
                                 int builder, cli_agent_change *change,
                                 cai_error *error) {
  cai_config *resolved = NULL;
  char canonical[65];
  size_t i;
  int parsed, rc = cai_cli_agent_name(name, canonical, error);
  if (rc != CAI_OK)
    return rc;
  change->previous_options = state->options;
  change->previous_config = state->configuration;
  change->previous_environment = state->environment;
  change->previous_services = state->services;
  change->previous_runtime = state->runtime;
  strcpy(change->previous_id, cai_agent_runtime_session_id(state->runtime));
  change->previous_messages = state->messages;
  strcpy(change->previous_name, state->pouch.active_agent_name);
  strcpy(change->previous_revision, state->pouch.active_agent_revision);
  rc = cai_cli_agent_configuration_select(
      state->bootstrap_configuration, canonical, &state->normal_options,
      state->application, &state->startup, &resolved, error);
  if (rc != CAI_OK)
    goto done;
  change->arguments =
      calloc((size_t)state->invocation_argc + 3U, sizeof(*change->arguments));
  if (change->arguments == NULL) {
    rc = cai_cli_error(error, CAI_ERR_NOMEM,
                       "allocate agent selection arguments");
    goto done;
  }
  for (i = 0U; i < (size_t)state->invocation_argc; ++i)
    change->arguments[i] = state->invocation_argv[i];
  change->arguments[i] = strdup("--use");
  change->arguments[i + 1U] = strdup(canonical);
  if (change->arguments[i] == NULL || change->arguments[i + 1U] == NULL) {
    rc =
        cai_cli_error(error, CAI_ERR_NOMEM, "retain agent selection arguments");
    goto done;
  }
  parsed = cai_cli_configure_stored_at(
      state->invocation_argc + 2, change->arguments, resolved,
      &change->next_options, &change->next_config, state->application,
      &state->startup, error);
  resolved = NULL;
  if (parsed != 1) {
    rc = error->code != CAI_OK ? error->code
                               : cai_cli_error(error, CAI_ERR_INVALID,
                                               change->next_options.diagnostic);
    goto done;
  }
  change->next_options.agent_use = change->next_options.source_agent_name;
  change->next_options.agent_builder = builder;
  change->next_options.developer_instructions_override_file = NULL;
  change->next_options.review = change->next_options.review_and_fix = 0;
  change->next_options.non_interactive = 0;
  change->next_options.out = NULL;
  cli_agent_temporary_options(&change->next_options, &state->options);
  /* A manually selected mailbox belongs to this workspace owner, rather
   * than to an agent definition or the session being left. */
  if (state->message_queue_override[0] != '\0')
    change->next_options.message_queue = state->message_queue_override;
  (void)cai_cli_agent_configuration_identity(
      change->next_config, change->next_name, change->next_revision);
  rc = cai_config_environment(change->next_config, &change->next_environment,
                              error);
  if (rc != CAI_OK)
    goto done;
  change->next_options.environment = change->next_environment;
  change->next_services = calloc(1U, sizeof(*change->next_services));
  if (change->next_services == NULL) {
    rc =
        cai_cli_error(error, CAI_ERR_NOMEM, "allocate prepared agent services");
    goto done;
  }
done:
  cai_config_close(resolved);
  return rc;
}

static void cli_agent_change_close(cli_state *state, cli_agent_change *change) {
  if (change == NULL)
    return;
  cli_agent_services_close(change->next_services);
  cai_environment_close(change->next_environment);
  cai_config_close(change->next_config);
  if (change->arguments != NULL) {
    free(change->arguments[state->invocation_argc]);
    free(change->arguments[state->invocation_argc + 1]);
    free(change->arguments);
  }
  free(change);
}

static void cli_agent_change_install(cli_state *state, cli_agent_change *change,
                                     int next) {
  state->options = next ? change->next_options : change->previous_options;
  state->configuration = next ? change->next_config : change->previous_config;
  state->environment =
      next ? change->next_environment : change->previous_environment;
  state->services = next ? change->next_services : change->previous_services;
  strcpy(state->pouch.active_agent_name,
         next ? change->next_name : change->previous_name);
  strcpy(state->pouch.active_agent_revision,
         next ? change->next_revision : change->previous_revision);
}

static int cli_change_agent(cli_state *state, const char *name, int builder,
                            cai_error *error) {
  cli_agent_change *change = calloc(1U, sizeof(*change));
  int rc, installed = 0, detached = 0;
  if (change == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate agent activation");
  if (state->normal_options.review || state->review_runtime != NULL) {
    rc = cai_cli_error(error, CAI_ERR_INVALID,
                       "change agents outside an isolated review");
    goto done;
  }
  rc = cli_agent_change_open(state, name, builder, change, error);
  if (rc != CAI_OK)
    goto done;
  if (strcmp(change->next_name, state->pouch.active_agent_name) == 0 &&
      strcmp(change->next_revision, state->pouch.active_agent_revision) == 0 &&
      state->options.agent_builder == builder &&
      state->options.developer_instructions_override_file == NULL) {
    rc = CAI_OK;
    goto done;
  }
  rc = cai_cli_documents_bind(&state->documents, NULL, error);
  detached = rc == CAI_OK;
  if (rc == CAI_OK)
    rc = cai_agent_runtime_pump(state->runtime, 0L, error);
  if (rc != CAI_OK)
    goto done;
  cli_agent_change_install(state, change, 1);
  installed = 1;
  rc = cli_agent_services_open(state, error);
  if (rc == CAI_OK)
    rc = cli_prepare_runtime(state, NULL, 0, change->previous_runtime, error);
  if (rc != CAI_OK) {
    cli_agent_change_install(state, change, 0);
    state->runtime = change->previous_runtime;
    strcpy(state->pouch.active_agent_name, change->previous_name);
    strcpy(state->pouch.active_agent_revision, change->previous_revision);
    if (state->messages != change->previous_messages) {
      cai_cli_messages_close(state->messages);
      state->messages = change->previous_messages;
    }
    installed = 0;
    goto done;
  }
  /* Publication has succeeded. Update borrowed host pointers before retiring
   * the previous runtime; attachment cursors and queue clients stay intact. */
  change->next_config = NULL;
  change->next_environment = NULL;
  change->next_services = NULL;
  memset(&state->quota, 0, sizeof(state->quota));
  state->settings_notice_fields = 0U;
  rc = cai_cli_workspace_host_catalog(state->workspace_host, state->runtime,
                                      error);
  if (rc == CAI_OK)
    rc = cai_cli_documents_bind(&state->documents, state->runtime, error);
  if (rc == CAI_OK && state->messages != NULL) {
    rc =
        state->messages != change->previous_messages
            ? cai_cli_messages_start(
                  state->messages, cai_agent_runtime_session_id(state->runtime),
                  state->workspace, state->options.message_queue, error)
            : cai_cli_messages_set_receiving(state->messages,
                                             state->options.messaging, error);
  }
  if (rc == CAI_OK) {
    cai_cli_mcp_start(&state->services->mcp);
    if (!cai_cli_mcp_ready(&state->services->mcp))
      rc = cai_agent_runtime_refresh_mcp(state->runtime, error);
  }
  if (rc == CAI_OK)
    rc = cli_prompt_configure(state, error);
  if (rc == CAI_OK)
    rc = cli_publish_status(state, state->runtime, error);
  if (rc == CAI_OK)
    rc = cli_sync_status(state, error);
  cai_agent_runtime_close(change->previous_runtime);
  cli_agent_services_close(change->previous_services);
  cai_environment_close(change->previous_environment);
  if (change->previous_config != state->bootstrap_configuration)
    cai_config_close(change->previous_config);
  if (rc != CAI_OK)
    state->exit_requested =
        1; /* Durable selection survives attachment failure. */
done:
  if (detached && !installed)
    (void)cai_cli_documents_bind(&state->documents, state->runtime, NULL);
  cli_agent_change_close(state, change);
  return rc;
}

/** Session selection prepares the selected definition before closing the
 * current runtime. It never activates that definition on the session being
 * left. A failed destination open restores the previous session and services.
 */
static int cli_reopen_agent_session(cli_state *state, const char *id,
                                    int new_session, const char *override,
                                    cai_error *error) {
  cli_agent_change *change = calloc(1U, sizeof(*change));
  cai_cli_session *saved = NULL;
  const char *name = override != NULL ? override : "default";
  cai_error recovery;
  int rc, closed = 0;
  if (change == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM,
                         "allocate session agent selection");
  if (!new_session && override == NULL) {
    saved = calloc(1U, sizeof(*saved));
    if (saved == NULL) {
      rc = cai_cli_error(error, CAI_ERR_NOMEM, "read session agent selection");
      goto done;
    }
    rc = cai_cli_pouch_session(&state->pouch, id, saved, error);
    if (rc != CAI_OK)
      goto done;
    if (saved->agent_name[0] != '\0')
      name = saved->agent_name;
  }
  if (override == NULL && strcmp(name, state->pouch.active_agent_name) == 0) {
    /* No agent activation was requested. Preserve this owner's operational
     * controls and immutable resolved definition, including /queue changes. */
    change->previous_options = state->options;
    strcpy(change->previous_id, cai_agent_runtime_session_id(state->runtime));
    rc = cai_cli_messages_flush_decisions(state->messages, state->input, error);
    if (rc == CAI_OK)
      rc = cli_close_runtime(state, error);
    if (rc != CAI_OK)
      goto done;
    if (!new_session) {
      state->options.model_explicit = 0;
      state->options.reasoning_effort_explicit = 0;
      state->options.reasoning_summary_explicit = 0;
      state->options.service_tier = NULL;
    }
    rc = cli_open_runtime(state, id, new_session, error);
    if (rc != CAI_OK) {
      cai_error_init(&recovery);
      (void)cli_close_runtime(state, &recovery);
      cai_error_cleanup(&recovery);
      state->options = change->previous_options;
      cai_error_init(&recovery);
      if (cli_open_runtime(state, change->previous_id, 0, &recovery) != CAI_OK)
        state->exit_requested = 1;
      cai_error_cleanup(&recovery);
    }
    goto done;
  }
  rc = cli_agent_change_open(state, name, 0, change, error);
  if (rc != CAI_OK)
    goto done;
  /* Restoring a session uses its saved request controls. Explicit controls
   * from an attaching frontend are applied after this selection; the owner
   * must not replay its original -m/-r over later session settings. */
  change->next_options.model_explicit = 0;
  change->next_options.reasoning_effort_explicit = 0;
  change->next_options.reasoning_summary_explicit = 0;
  change->next_options.service_tier = NULL;
  cli_agent_change_install(state, change, 1);
  rc = cli_agent_services_open(state, error);
  if (rc == CAI_OK)
    rc = cli_open_plugins(state, error);
  cli_agent_change_install(state, change, 0);
  if (rc != CAI_OK)
    goto done;
  rc = cai_cli_messages_flush_decisions(state->messages, state->input, error);
  if (rc == CAI_OK)
    rc = cli_close_runtime(state, error);
  if (rc != CAI_OK)
    goto done;
  closed = 1;
  cli_agent_change_install(state, change, 1);
  rc = cli_open_runtime(state, id, new_session, error);
  if (rc == CAI_OK) {
    change->next_config = NULL;
    change->next_environment = NULL;
    change->next_services = NULL;
    cli_agent_services_close(change->previous_services);
    cai_environment_close(change->previous_environment);
    if (change->previous_config != state->bootstrap_configuration)
      cai_config_close(change->previous_config);
    memset(&state->quota, 0, sizeof(state->quota));
  } else {
    cai_error_init(&recovery);
    (void)cli_close_runtime(state, &recovery);
    cai_error_cleanup(&recovery);
    cli_agent_change_install(state, change, 0);
    cai_error_init(&recovery);
    if (cli_open_runtime(state, change->previous_id, 0, &recovery) != CAI_OK) {
      state->exit_requested = 1;
      cli_print_error(state, "restore session after selection failure",
                      &recovery);
    }
    cai_error_cleanup(&recovery);
  }
done:
  if (rc != CAI_OK && closed && state->runtime == NULL)
    state->exit_requested = 1;
  free(saved);
  cli_agent_change_close(state, change);
  return rc;
}
