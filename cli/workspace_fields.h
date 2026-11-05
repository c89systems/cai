/** Single inventory of immutable startup identity. Runtime inference controls
 * and frontend preferences are deliberately absent. The primary parser records
 * which members were supplied, including explicit values equal to defaults. */
#ifndef CAI_CLI_WORKSPACE_FIELDS_H
#define CAI_CLI_WORKSPACE_FIELDS_H
/* clang-format off */
#define CAI_CLI_OWNER_FIELDS(X) \
  X("paths", "config_file", config_file, 's') \
  X("paths", "home_dir", home_dir, 's') \
  X("paths", "state_dir", state_dir, 's') \
  X("paths", "cache_dir", cache_dir, 's') \
  X("paths", "data_dir", data_dir, 's') \
  X("paths", "log_dir", log_dir, 's') \
  X("paths", "config_dir", config_dir, 's') \
  X("store.default", "key_file", pouch_key_file, 's') \
  X("store.default", "endpoint", lockd, 's') \
  X("store.default", "client_pem", lockd_client_pem, 's') \
  X("storage", "namespace_prefix", namespace_prefix, 's') \
  X("storage", "sessions_namespace", sessions_namespace, 's') \
  X("storage", "auth_namespace", auth_namespace, 's') \
  X("storage", "scopes_namespace", scopes_namespace, 's') \
  X("storage", "todo_namespace", todo_namespace, 's') \
  X("storage", "messages_namespace", messages_namespace, 's') \
  X("storage", "sessions_store", sessions_store, 's') \
  X("storage", "auth_store", auth_store, 's') \
  X("storage", "messaging_store", messaging_store, 's') \
  X("messaging", "queue_prefix", queue_prefix, 's') \
  X("messaging", "queue_name", message_queue, 's') \
  X("client", "provider", provider, 's') \
  X("client", "base_url", endpoint, 's') \
  X("client", "api_key_env", api_key_env, 's') \
  X("paths", "auth_json", auth_json, 's') \
  X("paths", "agents_md", agents_md, 's') \
  X("paths", "skills_dir", skills_dir, 's') \
  X("paths", "model_metadata", model_metadata, 's') \
  X("plugins", "directory", tools_dir, 's') \
  X("plugins", "workspace_directory", workspace_tools_dir, 's') \
  X("chrome", "socket", chrome_socket, 's') \
  X("searxng", "base_url", searxng_url, 's') \
  X("revgeo", "base_url", revgeo_url, 's') \
  X("agent", "identity", identity, 's') \
  X("agent", "developer_instructions", developer_instructions, 's') \
  X("server", "workspace_name", workspace_name, 's') \
  X("review", "model", review_model, 's') \
  X("review", "reasoning_effort", review_reasoning_effort, 's') \
  X("review", "reasoning_summary", review_reasoning_summary, 's') \
  X("server", "persist", server_persist, 'b') \
  X("storage", "disable_compression", disable_compression, 'b') \
  X("agent", "insecure", insecure, 'b') \
  X("plugins", "enabled", plugins, 'b') \
  X("agent", "web_search", web_search, 'b') \
  X("agent", "terminal", terminal, 'b') \
  X("agent", "image_generation", image_generation, 'b') \
  X("agent", "review_subagent", review_subagent, 'b') \
  X("agent", "codex_agents_md", codex_agents_md, 'b') \
  X("agent", "mcp", mcp, 'b') \
  X("agent", "read_file", read_file, 'b') \
  X("agent", "list_files", list_files, 'b') \
  X("agent", "apply_patch", apply_patch, 'b') \
  X("agent", "view_image", view_image, 'b') \
  X("agent", "goals", goals, 'b') \
  X("agent", "skills", skills, 'b') \
  X("agent", "subagent_steering", subagent_steering, 'b') \
  X("agent", "capacity_retry", capacity_retry, 'b') \
  X("agent", "review_and_fix_tool", review_and_fix_tool, 'b') \
  X("chrome", "enabled", chrome, 'b') \
  X("agent", "todo", todo, 'b') \
  X("agent", "searxng", searxng, 'b') \
  X("agent", "revgeo", revgeo, 'b') \
  X("messaging", "enabled", messaging, 'b') \
  X("agent", "reasoning_tool", reasoning_tool, 'b')
/* clang-format on */
/* The inventory uses one bit per field and leaves the configured names visible
 * in diagnostics. Keep transport limits and this mask in step if it grows. */
enum {
#define CAI_CLI_OWNER_ID(section, key, member, type) CAI_CLI_OWNER_##member,
  CAI_CLI_OWNER_FIELDS(CAI_CLI_OWNER_ID)
#undef CAI_CLI_OWNER_ID
      CAI_CLI_OWNER_COUNT
};
#define CAI_CLI_OWNER_BIT(member) (1ULL << CAI_CLI_OWNER_##member)
#endif
