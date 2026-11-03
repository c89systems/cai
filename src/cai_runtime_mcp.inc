/* Catalog mutation stays on the runtime worker at request boundaries. Network
 * discovery does not hold the publication lock or block the frontend owner. */
typedef struct cai_runtime_search_arguments {
  char *query;
  long long limit;
  int has_limit;
} cai_runtime_search_arguments;
static const lonejson_field cai_runtime_search_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_runtime_search_arguments, query,
                                    "query"),
    LONEJSON_FIELD_I64_PRESENT(cai_runtime_search_arguments, limit, has_limit,
                               "limit")};
LONEJSON_MAP_DEFINE(cai_runtime_search_map, cai_runtime_search_arguments,
                    cai_runtime_search_fields);
typedef struct cai_runtime_search_sink {
  cai_sink *output;
  cai_error *error;
} cai_runtime_search_sink;
static lonejson_status cai_runtime_search_write(void *context, const void *data,
                                                size_t length,
                                                lonejson_error *error) {
  cai_runtime_search_sink *sink = context;
  (void)error;
  return cai_sink_write(sink->output, data, length, sink->error) == CAI_OK
             ? LONEJSON_STATUS_OK
             : LONEJSON_STATUS_CALLBACK_FAILED;
}
static int cai_runtime_search_tools(void *context, const char *arguments,
                                    cai_sink *output, cai_error *error) {
  cai_agent_runtime *runtime = context;
  cai_agent_impl *agent = CAI_AGENT_IMPL(runtime->agent);
  cai_runtime_search_arguments args;
  cai_runtime_search_sink sink;
  cai_mcp_search_match matches[32];
  lonejson_error local;
  lonejson_writer writer;
  lonejson_status status;
  size_t count = 0U, i, limit = 8U;
  int rc = CAI_OK;
  char message[128];
  memset(&args, 0, sizeof(args));
  memset(&writer, 0, sizeof(writer));
  lonejson_error_init(&local);
  status = CAI_LJ->parse_cstr(CAI_LJ, &cai_runtime_search_map, &args, arguments,
                              &local);
  if (status != LONEJSON_STATUS_OK || args.query == NULL ||
      args.query[0] == '\0' || strlen(args.query) > 4096U ||
      (args.has_limit && (args.limit < 1LL || args.limit > 32LL))) {
    rc = cai_set_error(error, CAI_ERR_INVALID,
                       "tool_search requires a nonempty query (max 4096 bytes) "
                       "and limit 1..32");
    goto done;
  }
  if (args.has_limit)
    limit = (size_t)args.limit;
  {
    cai_error search_error;
    cai_error_init(&search_error);
    count = cai_mcp_search_find(runtime->mcp_search, args.query, matches, limit,
                                &search_error);
    if (search_error.code != CAI_OK)
      rc = cai_set_error(error, search_error.code, search_error.message);
    cai_error_cleanup(&search_error);
  }
  if (rc != CAI_OK)
    goto done;
  snprintf(message, sizeof(message),
           count != 0U
               ? "Loaded %lu MCP tool(s) for the next request."
               : "No matching MCP tools. Try another query or source name.",
           (unsigned long)count);
  sink.output = output;
  sink.error = error;
  status = CAI_LJ->writer_init_sink(CAI_LJ, &writer, cai_runtime_search_write,
                                    &sink, &local);
#define SEARCH_WRITE(call)                                                     \
  do {                                                                         \
    if (status == LONEJSON_STATUS_OK)                                          \
      status = (call);                                                         \
  } while (0)
#define SEARCH_KEY(value)                                                      \
  SEARCH_WRITE(writer.key(&writer, value, sizeof(value) - 1U, &local))
  SEARCH_WRITE(writer.begin_object(&writer, &local));
  SEARCH_KEY("ok");
  SEARCH_WRITE(writer.bool_fn(&writer, 1, &local));
  SEARCH_KEY("message");
  SEARCH_WRITE(writer.string(&writer, message, strlen(message), &local));
  SEARCH_KEY("tools");
  SEARCH_WRITE(writer.begin_array(&writer, &local));
  for (i = 0U; status == LONEJSON_STATUS_OK && i < count; ++i) {
    const char *name =
        cai_tool_registry_name_at(agent->tools, matches[i].index);
    const char *description =
        cai_tool_registry_description_at(agent->tools, matches[i].index);
    const char *source =
        cai_mcp_search_source(runtime->mcp_search, matches[i].index);
    size_t source_length = strlen(source);
    size_t length = description != NULL ? strlen(description) : 0U;
    int truncated = length > 512U;
    if (truncated) {
      length = 512U;
      while (length != 0U &&
             ((unsigned char)description[length] & 0xc0U) == 0x80U)
        --length;
    }
    SEARCH_WRITE(writer.begin_object(&writer, &local));
    SEARCH_KEY("name");
    SEARCH_WRITE(writer.string(&writer, name, strlen(name), &local));
    if (source_length > 256U) {
      source_length = 256U;
      while (source_length != 0U &&
             ((unsigned char)source[source_length] & 0xc0U) == 0x80U)
        --source_length;
    }
    SEARCH_KEY("source");
    SEARCH_WRITE(writer.string(&writer, source, source_length, &local));
    SEARCH_KEY("description");
    SEARCH_WRITE(writer.string(&writer, description != NULL ? description : "",
                               length, &local));
    if (truncated) {
      SEARCH_KEY("description_truncated");
      SEARCH_WRITE(writer.bool_fn(&writer, 1, &local));
    }
    SEARCH_WRITE(writer.end_object(&writer, &local));
  }
  SEARCH_WRITE(writer.end_array(&writer, &local));
  SEARCH_WRITE(writer.end_object(&writer, &local));
  SEARCH_WRITE(writer.finish(&writer, &local));
#undef SEARCH_KEY
#undef SEARCH_WRITE
  if (writer.cleanup != NULL)
    writer.cleanup(&writer);
  if (status != LONEJSON_STATUS_OK) {
    rc = error != NULL && error->code != CAI_OK
             ? error->code
             : cai_set_error(error, CAI_ERR_TRANSPORT,
                             "write MCP discovery result");
    goto done;
  }
  pthread_mutex_lock(&agent->plugins_lock);
  for (i = 0U; i < count; ++i)
    cai_tool_registry_defer_internal(agent->tools, matches[i].index, 0);
  if (count != 0U)
    agent->mcp_tools_dirty = 1;
  pthread_mutex_unlock(&agent->plugins_lock);
done:
  CAI_LJ->cleanup(CAI_LJ, &cai_runtime_search_map, &args);
  return rc;
}
static int cai_runtime_register_mcp_search(cai_agent_runtime *runtime,
                                           cai_error *error) {
  runtime->mcp_search_tool =
      cai_tool_registry_count(CAI_AGENT_IMPL(runtime->agent)->tools);
  return cai_agent_register_raw_tool(
      runtime->agent, "tool_search", "Discover deferred MCP tools.",
      "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\","
      "\"description\":\"App, action, and relevant search "
      "words.\"},\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":32}}"
      ",\"required\":[\"query\"],\"additionalProperties\":false}",
      0, cai_runtime_search_tools, runtime, error);
}
/* Build a replacement index before publishing either schemas or metadata. */
static int cai_runtime_build_mcp_search(cai_agent_runtime *runtime,
                                        cai_tool_registry *tools, size_t base,
                                        const size_t *ends, int preserve,
                                        cai_mcp_search **out,
                                        char **description, cai_error *error) {
  cai_tool_registry *old = CAI_AGENT_IMPL(runtime->agent)->tools;
  size_t i, j, k, begin = runtime->mcp_tool_begin;
  int rc = cai_mcp_search_new(out, error);
  *description = NULL;
  for (i = 0U; rc == CAI_OK && i < runtime->mcp_client_count; ++i) {
    const cai_agent_mcp_options *options = &runtime->mcp_options[i];
    for (j = begin; rc == CAI_OK && j < ends[i]; ++j) {
      size_t local = j - base;
      const cai_mcp_client_tool *tool =
          cai_mcp_client_tool_at(runtime->mcp_clients[i], j - begin);
      const char *alias = cai_tool_registry_name_at(tools, local);
      int deferred = !options->eager;
      if (tool == NULL || alias == NULL) {
        rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP catalog changed during registration");
        break;
      }
      if (!options->eager) {
        const char *source =
            tool->source_name != NULL && tool->source_name[0] != '\0'
                ? tool->source_name
                : options->name;
        const char *hint = tool->source_description != NULL &&
                                   tool->source_description[0] != '\0'
                               ? tool->source_description
                               : options->description;
        rc = cai_mcp_search_add(*out, j, tool, alias, source, hint, error);
        if (preserve) {
          for (k = runtime->mcp_tool_begin;
               k < (runtime->mcp_client_count != 0U
                        ? runtime->mcp_tool_ends[runtime->mcp_client_count - 1U]
                        : 0U);
               ++k)
            if (!cai_tool_registry_deferred_internal(old, k) &&
                strcmp(alias, cai_tool_registry_name_at(old, k)) == 0) {
              deferred = 0;
              break;
            }
        }
      }
      cai_tool_registry_defer_internal(tools, local, deferred);
    }
    begin = ends[i];
  }
  if (rc == CAI_OK) {
    *description = cai_mcp_search_description(*out, error);
    if (*description == NULL)
      rc = error != NULL && error->code != CAI_OK ? error->code : CAI_ERR_NOMEM;
  }
  if (rc != CAI_OK) {
    cai_mcp_search_destroy(*out);
    *out = NULL;
    cai_free_mem(NULL, *description);
    *description = NULL;
  }
  return rc;
}
static int cai_runtime_prepare_initial_mcp(cai_agent_runtime *runtime,
                                           cai_error *error) {
  cai_tool_registry *tools = CAI_AGENT_IMPL(runtime->agent)->tools;
  char *description;
  int rc = cai_runtime_build_mcp_search(
      runtime, tools, 0U, runtime->mcp_tool_ends, 0, &runtime->mcp_search,
      &description, error);
  if (rc == CAI_OK) {
    cai_tool_registry_description_internal(tools, runtime->mcp_search_tool,
                                           description);
    cai_tool_registry_defer_internal(
        tools, runtime->mcp_search_tool,
        cai_mcp_search_count(runtime->mcp_search) == 0U);
  }
  return rc;
}
static int cai_runtime_apply_pending_mcp_pass(cai_agent_runtime *runtime,
                                              cai_error *error) {
  cai_agent_impl *agent = CAI_AGENT_IMPL(runtime->agent);
  cai_mcp_tool_registration_config policy;
  cai_tool_registry *next = NULL;
  cai_mcp_search *search = NULL;
  char *description = NULL;
  size_t *ends = NULL, i, old_count, new_count;
  char receipt[640];
  cai_error local;
  int rc, published = 0;
  pthread_mutex_lock(&runtime->lock);
  if (!runtime->mcp_refresh_pending) {
    pthread_mutex_unlock(&runtime->lock);
    return CAI_OK;
  }
  runtime->mcp_refresh_pending = 0;
  pthread_mutex_unlock(&runtime->lock);
  cai_error_init(&local);
  memset(&policy, 0, sizeof(policy));
  if (runtime->has_mcp_tool_config)
    policy = runtime->mcp_tool_config;
  if (CAI_CLIENT_IMPL(runtime->client)->api_protocol ==
      CAI_API_PROTOCOL_CHAT_COMPLETIONS)
    policy.raw_input_policy = CAI_MCP_RAW_INPUT_JSON;
  ends = cai_alloc(NULL, (runtime->mcp_client_count + 1U) * sizeof(*ends));
  rc = ends != NULL ? cai_tool_registry_new(&next, &local)
                    : cai_set_error(&local, CAI_ERR_NOMEM,
                                    "allocate MCP catalog boundaries");
  for (i = 0U; rc == CAI_OK && i < runtime->mcp_client_count; i++) {
    rc = cai_mcp_client_register_tools(runtime->mcp_clients[i], next, &policy,
                                       &local);
    ends[i] = runtime->mcp_tool_begin + cai_tool_registry_count(next);
  }
  if (rc == CAI_OK) {
    rc = cai_runtime_build_mcp_search(runtime, next, runtime->mcp_tool_begin,
                                      ends, 1, &search, &description, &local);
  }
  if (rc == CAI_OK) {
    pthread_mutex_lock(&agent->plugins_lock);
    old_count = runtime->mcp_client_count == 0U
                    ? 0U
                    : runtime->mcp_tool_ends[runtime->mcp_client_count - 1U] -
                          runtime->mcp_tool_begin;
    new_count = cai_tool_registry_count(next);
    rc = cai_tool_registry_replace_range_internal(
        agent->tools, runtime->mcp_tool_begin, old_count, next, &local);
    if (rc == CAI_OK) {
      if (agent->plugins != NULL)
        agent->plugins_base = agent->plugins_base - old_count + new_count;
      if (runtime->mcp_client_count != 0U)
        memcpy(runtime->mcp_tool_ends, ends,
               runtime->mcp_client_count * sizeof(*ends));
      agent->mcp_tools_dirty = 1;
      cai_mcp_search_destroy(runtime->mcp_search);
      runtime->mcp_search = search;
      search = NULL;
      if (runtime->mcp_client_count != 0U) {
        cai_tool_registry_description_internal(
            agent->tools, runtime->mcp_search_tool, description);
        description = NULL;
        cai_tool_registry_defer_internal(
            agent->tools, runtime->mcp_search_tool,
            cai_mcp_search_count(runtime->mcp_search) == 0U);
      }
      published = 1;
    }
    pthread_mutex_unlock(&agent->plugins_lock);
  }
  snprintf(
      receipt, sizeof(receipt), "%s",
      published
          ? "MCP connections updated."
          : "MCP catalog update failed; the previous tools remain installed.");
  if (!published && runtime->logger != NULL)
    runtime->logger->warnf(
        runtime->logger, "MCP catalog update failed", "error=%s",
        local.message != NULL ? local.message : "catalog unavailable");
  cai_tool_registry_destroy(next);
  cai_mcp_search_destroy(search);
  cai_free_mem(NULL, description);
  cai_free_mem(NULL, ends);
  cai_error_cleanup(&local);
  pthread_mutex_lock(&runtime->lock);
  rc = cai_runtime_enqueue_locked(runtime, CAI_AGENT_EVENT_MCP_UPDATED, receipt,
                                  strlen(receipt), NULL, NULL, runtime->state,
                                  error);
  pthread_cond_broadcast(&runtime->condition);
  pthread_mutex_unlock(&runtime->lock);
  return rc;
}

static int cai_runtime_apply_pending_mcp(cai_agent_runtime *runtime,
                                         cai_error *error) {
  int pending, rc;
  for (;;) {
    pthread_mutex_lock(&runtime->lock);
    pending = runtime->mcp_refresh_pending;
    pthread_mutex_unlock(&runtime->lock);
    if (!pending)
      return CAI_OK;
    rc = cai_runtime_apply_pending_mcp_pass(runtime, error);
    if (rc != CAI_OK)
      return rc;
    /* Discovery can take time. Collect controls which arrived during that
     * wait before constructing another inference request. */
    rc = cai_runtime_collect_owner_input(runtime, error);
    if (rc != CAI_OK)
      return rc;
  }
}

static int cai_runtime_request_mcp_refresh(cai_agent_runtime *runtime,
                                           cai_error *error) {
  cai_agent_runtime *child;
  int rc = CAI_OK;
  pthread_mutex_lock(&runtime->lock);
  if (runtime->stopping) {
    pthread_mutex_unlock(&runtime->lock);
    return cai_set_error(error, CAI_ERR_CANCELLED, "agent runtime is closing");
  }
  runtime->mcp_refresh_pending = 1;
  child = runtime->active_subagent != NULL ? runtime->active_subagent
                                           : runtime->active_review;
  if (child != NULL)
    runtime->child_input_users++;
  pthread_cond_broadcast(&runtime->condition);
  pthread_mutex_unlock(&runtime->lock);
  if (child != NULL) {
    rc = cai_runtime_request_mcp_refresh(child, error);
    pthread_mutex_lock(&runtime->lock);
    runtime->child_input_users--;
    pthread_cond_broadcast(&runtime->condition);
    pthread_mutex_unlock(&runtime->lock);
  }
  return rc;
}

int cai_agent_runtime_refresh_mcp(cai_agent_runtime *runtime,
                                  cai_error *error) {
  int rc = cai_runtime_owner(runtime, error);
  return rc == CAI_OK ? cai_runtime_request_mcp_refresh(runtime, error) : rc;
}
