/** @file cai/handover.h
 * Runtime-independent Markdown handover formatting from borrowed metadata and
 * synchronous streaming producers. No model, tool, storage or UI dependency.
 */
#ifndef CAI_HANDOVER_H
#define CAI_HANDOVER_H

#include <cai/cai.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Emit a borrowed value into sink, propagating its first failure. A producer
 * neither closes the sink nor retains it after returning. No complete value
 * needs to exist in memory. NULL write denotes an absent value. */
typedef struct cai_handover_source {
  int (*write)(void *context, cai_sink *sink, cai_error *error);
  void *context;
} cai_handover_source;

/** Inputs for the existing cai-agent-handover/1 document. All strings and
 * source contexts remain borrowed for the synchronous write. Metadata strings
 * are rendered as indented literals; text sources retain Markdown while
 * removing C0 controls other than tab/newline and DEL, matching runtime
 * exports.
 */
typedef struct cai_handover_config {
  const char *preset_name;
  const char *preset_prompt_version;
  const char *active_model;
  const char *recorded_model;
  const char *reasoning_effort;
  const char *runtime_state;
  const char *session_id;
  const char *workspace_directory;
  const char *session_scope;
  int terminal_enabled;
  int image_generation_enabled;
  size_t mcp_client_count;
  const char *goal_status;
  int goal_has_token_budget;
  long long goal_token_budget;
  long long goal_tokens_used;
  long long goal_elapsed_seconds;
  /** A present producer means a durable goal exists, including empty text. */
  cai_handover_source goal_objective;
  cai_handover_source developer_instructions;
  /** Replay the same stable retained history on each of exactly two calls:
   * <decimal byte length> LF <one-item provider JSON array> LF, repeated.
   * The first pass indexes bounded item identities, the second emits text.
   * Neither pass materializes provider values. Index memory grows with item
   * count, never with text size. The caller owns snapshot consistency across
   * the two calls and must propagate read/cancellation failures.
   */
  cai_handover_source history;
  /** Optional bounded cancellation check, called during source consumption
   * and output. Non-zero stops the operation with CAI_ERR_CANCELLED. */
  int (*cancelled)(void *context);
  void *cancel_context;
} cai_handover_config;

/** Initialize absent/disabled fields to zero. */
void cai_handover_config_init(cai_handover_config *config);
/** Write the handover without constructing a runtime or accumulating context.
 * Uses bounded payload chunks on the caller's thread; sink remains open. A
 * failure invalidates the partial document and stops further production.
 * Execution-history sections can be appended separately by their owner.
 */
int cai_handover_write(const cai_handover_config *config, cai_sink *sink,
                       cai_error *error);

#ifdef __cplusplus
}
#endif
#endif
