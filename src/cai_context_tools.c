/** @file cai_context_tools.c
 * Model-facing declarations and freeform-input adaptation. Storage operations
 * use only the adapter's public API and can also be called without inference.
 */
#define _POSIX_C_SOURCE 200809L
#include "cai_context_internal.h"
#include <cai/context.h>
#include <lonejson.h>
#include <stdlib.h>
#include <string.h>

static int fail(cai_error *error, int code, const char *message) {
  if (error != NULL) {
    cai_error_cleanup(error);
    error->code = code;
    error->message =
        strdup(message != NULL ? message : "context tool input failed");
  }
  return code;
}
static int from_lj(cai_error *error, const lonejson_error *detail) {
  return fail(error, CAI_ERR_INVALID, detail->message);
}
static size_t spool_read(void *ctx, void *b, size_t n, cai_error *e) {
  lonejson_read_result r = lonejson_spooled_read(ctx, b, n);
  if (r.error_code)
    fail(e, CAI_ERR_INVALID, "read context tool input");
  return r.bytes_read;
}
static int create_tool(void *ctx, lonejson_spooled *input, cai_sink *out,
                       cai_error *e) {
  cai_source_callbacks cb;
  cai_source *source = NULL;
  lonejson_error l;
  int rc;
  lonejson_error_init(&l);
  if (lonejson_spooled_rewind(input, &l) != LONEJSON_STATUS_OK)
    return from_lj(e, &l);
  memset(&cb, 0, sizeof(cb));
  cb.read = spool_read;
  cb.context = input;
  rc = cai_source_from_callbacks(&cb, &source, e);
  if (rc == CAI_OK)
    rc = cai_context_create(ctx, source, out, e);
  cai_source_close(source);
  return rc;
}
#define TOOL_CALLBACK(name)                                                    \
  static int name##_tool(void *ctx, const char *args, cai_sink *out,           \
                         cai_error *e) {                                       \
    return cai_context_call(ctx, "context_" #name, args, out, e);              \
  }
TOOL_CALLBACK(set)
TOOL_CALLBACK(query)
TOOL_CALLBACK(get)
TOOL_CALLBACK(upload)
TOOL_CALLBACK(download)
TOOL_CALLBACK(delete)
TOOL_CALLBACK(replace)
#undef TOOL_CALLBACK
#define ID_PROP "\"id\":{\"type\":\"string\"}"
#define VERSION_PROP "\"expected_version\":{\"type\":\"integer\",\"minimum\":0}"
#define PATH_PROP "\"path\":{\"type\":\"string\"}"
#define SCHEMA(properties, required)                                           \
  "{\"type\":\"object\",\"properties\":{" properties                           \
  "},\"required\":[" required "],\"additionalProperties\":false}"
typedef struct context_tool {
  const char *name, *description, *schema;
  cai_tool_raw_fn callback;
} context_tool;
static const context_tool tools[] = {
    {"context_set",
     "Change only supplied context metadata. Omitted fields are preserved; "
     "null clears an optional field. Never changes content. Shared across "
     "sessions and workspaces; use only when the task calls for storing or "
     "retrieving context. Use expected_version from get or the last receipt.",
     SCHEMA(
         ID_PROP
         "," VERSION_PROP
         ",\"title\":{\"type\":[\"string\",\"null\"]},\"filename\":{\"type\":["
         "\"string\",\"null\"]},\"date\":{\"type\":[\"string\",\"null\"]},"
         "\"media_type\":{\"type\":[\"string\",\"null\"]},\"tags\":{\"type\":["
         "\"array\",\"null\"],\"items\":{\"type\":\"string\"}}",
         "\"id\",\"expected_version\""),
     set_tool},
    {"context_query",
     "Search/list shared context only when the task requires it. query is full "
     "LQL over flat fields /id, /kind, /title, /filename, /tags[], /date, "
     "/media_type, /created_at, /updated_at, /size, /content. Example "
     "icontains{field=/content,value=\"deployment\"}. Returns bounded metadata "
     "and cursor, never full bodies/bytes. Pass the same query with cursor for "
     "another page. No automatic startup retrieval.",
     SCHEMA("\"query\":{\"type\":\"string\"},\"limit\":{\"type\":\"integer\","
            "\"minimum\":0,\"maximum\":32},\"cursor\":{\"type\":\"string\"}",
            ""),
     query_tool},
    {"context_get",
     "Read context metadata, version and up to 16 KiB of text. Binary bytes "
     "never enter inference; use context_download for complete text or files.",
     SCHEMA(ID_PROP, "\"id\""), get_tool},
    {"context_upload",
     "Store a local regular file as new shared context. Automatically "
     "classifies the full file: UTF-8 text becomes searchable JSON content; "
     "binary data becomes an attachment. Captures filename, media type and "
     "byte size; returns a new XID/version. No base64 needed.",
     SCHEMA(PATH_PROP, "\"path\""), upload_tool},
    {"context_download",
     "Download complete context directly to a local path without routing bytes "
     "through inference. Decodes text JSON to original text, or streams "
     "original attachment bytes. Never overwrites an existing file.",
     SCHEMA(ID_PROP "," PATH_PROP, "\"id\",\"path\""), download_tool},
    {"context_delete",
     "Permanently remove shared context and its attachments. Requires current "
     "version. This affects every agent using this namespace/store.",
     SCHEMA(ID_PROP "," VERSION_PROP, "\"id\",\"expected_version\""),
     delete_tool},
    {"context_replace",
     "Replace context content from a local file, preserving identity and "
     "metadata. Automatically detects text versus binary as context_upload "
     "does. Updates kind, size and timestamps; resets media type when kind "
     "changes. Requires current version.",
     SCHEMA(ID_PROP "," VERSION_PROP "," PATH_PROP,
            "\"id\",\"expected_version\",\"path\""),
     replace_tool}};
#undef SCHEMA
#undef ID_PROP
#undef VERSION_PROP
#undef PATH_PROP
static const char create_description[] =
    "Store the entire raw UTF-8 tool input as a new shared context document's "
    "searchable content. Send only the text/Markdown, with no headers, "
    "delimiters or metadata envelope. Returns generated XID and version; "
    "optionally use context_set afterward for title, filename, tags or date. "
    "Context persists independently of sessions. Store/retrieve only as "
    "the user's task requires; no automatic startup retrieval.";
static int tool_available(cai_context *context, const char *name) {
  return cai_context_files_available(context) ||
         (strcmp(name, "context_upload") && strcmp(name, "context_download") &&
          strcmp(name, "context_replace"));
}
int cai_context_register(cai_context *p, cai_agent *agent, cai_error *e) {
  size_t i;
  int rc;
  if (p == NULL || agent == NULL)
    return fail(e, CAI_ERR_INVALID, "context registration requires an agent");
  rc = cai_agent_register_custom_spooled_tool(
      agent, "context_create", create_description, NULL, create_tool, p, e);
  for (i = 0U; rc == CAI_OK && i < sizeof(tools) / sizeof(tools[0]); i++)
    if (tool_available(p, tools[i].name))
      rc = cai_agent_register_raw_tool(agent, tools[i].name,
                                       tools[i].description, tools[i].schema, 0,
                                       tools[i].callback, p, e);
  return rc;
}
int cai_context_register_registry(cai_context *p, cai_tool_registry *registry,
                                  cai_error *e) {
  size_t i;
  int rc;
  if (p == NULL || registry == NULL)
    return fail(e, CAI_ERR_INVALID, "context registration requires a registry");
  rc = cai_tool_registry_register_custom_spooled(
      registry, "context_create", create_description, NULL, create_tool, p, e);
  for (i = 0U; rc == CAI_OK && i < sizeof(tools) / sizeof(tools[0]); i++)
    if (tool_available(p, tools[i].name))
      rc = cai_tool_registry_register_raw(registry, tools[i].name,
                                          tools[i].description, tools[i].schema,
                                          0, tools[i].callback, p, e);
  return rc;
}
