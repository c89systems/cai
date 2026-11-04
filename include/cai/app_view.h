/** @file cai/app_view.h Bounded presentation metadata from a workspace owner.
 */
#ifndef CAI_APP_VIEW_H
#define CAI_APP_VIEW_H
#include <cai/agent_runtime.h>
#include <cai/quota.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct cai_app_goal_view {
  int has_goal;
  const char *status;
  int has_token_budget;
  long long token_budget, tokens_used, remaining_tokens, elapsed_seconds;
  long long created_at, updated_at;
} cai_app_goal_view;
/** Ephemeral status, not model context or an authority grant. All strings are
 * literal data, borrowed until the client's next pump or close. Frontends must
 * escape/sanitize them. No goal objective or detailed error body is copied.
 * A received unavailable view replaces the old view; a disconnected view is
 * explicitly stale. Inspect flags before using values. */
typedef struct cai_app_view {
  uint64_t revision, conversation, received_ms;
  int available, stale, code;
  char reason[192];
  /** outcome uses cai_app_outcome values; runtime_state uses
   * cai_agent_run_state values. */
  int running, runtime_ready, outcome, exit_code, runtime_state;
  const char *session_id, *provider, *last_response_id;
  const char *provider_conversation_id, *log_path;
  cai_agent_runtime_settings effective, pending;
  cai_agent_runtime_metrics metrics;
  cai_app_goal_view goal;
  cai_chatgpt_quota quota;
  int yolo, foreground;
  long long yield_time_ms;
} cai_app_view;
/** Advance active durations from the last accepted observation using the
 * frontend's monotonic clock. Stale/unavailable views never advance. Saturates
 * on overflow and ignores a clock earlier than received_ms. */
void cai_app_view_elapsed(const cai_app_view *, uint64_t now_ms,
                          unsigned long long *turn_ms, long long *goal_seconds);
#ifdef __cplusplus
}
#endif
#endif
