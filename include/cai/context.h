/** @file cai/context.h
 * Shared, searchable context tools backed by a borrowed liblockdc client.
 * This optional adapter never owns the client or retrieves context implicitly.
 */
#ifndef CAI_CONTEXT_H
#define CAI_CONTEXT_H
#include <cai/cai.h>
#include <lc/lc.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct cai_context cai_context;
typedef struct cai_context_config {
  /** Borrowed until close, after every registered agent/registry is closed. */
  lc_client *client;
  /** Copied namespace; NULL selects cai.context. */
  const char *namespace_name;
  /** Copied filesystem root for upload/download; NULL disables file tools. */
  const char *root_path;
  /** Permit ordinary host paths. Does not enable overwriting downloads. */
  int yolo;
} cai_context_config;
void cai_context_config_init(cai_context_config *config);
int cai_context_open(const cai_context_config *config, cai_context **out,
                     cai_error *error);
void cai_context_close(cai_context *context);
/** Change file policy only while tools are idle, as with runtime YOLO changes.
 */
void cai_context_set_yolo(cai_context *context, int enabled);
/** Raw UTF-8 source to a new searchable object; receipt includes ID/version.
 * Source and sink are borrowed, never closed. No full-value staging occurs. */
int cai_context_create(cai_context *context, cai_source *text,
                       cai_sink *receipt, cai_error *error);
/** Execute a structured context tool without inference. Supported names are
 * context_set, context_query, context_get, context_upload, context_download,
 * context_delete and context_replace (replace content from a local file).
 * File tools obey root_path/yolo. Results are bounded; downloads are not. */
int cai_context_call(cai_context *context, const char *tool,
                     const char *arguments_json, cai_sink *result,
                     cai_error *error);
/** Export all context to DIR/ID/filename plus DIR/ID.context.json metadata.
 * Filenames are preserved, destinations must not exist, and errors preserve
 * earlier completed items. Export is paginated, not a cross-object snapshot. */
int cai_context_export(cai_context *context, const char *directory,
                       cai_sink *receipt, cai_error *error);
/** Borrow context through the entire lifetime of the receiving catalog. */
int cai_context_register(cai_context *context, cai_agent *agent,
                         cai_error *error);
int cai_context_register_registry(cai_context *context,
                                  cai_tool_registry *registry,
                                  cai_error *error);
#ifdef __cplusplus
}
#endif
#endif
