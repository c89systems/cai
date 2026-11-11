/** @file Frontend-owned TCP receipts using the ordinary Softline editor/queue.
 */
#ifndef CAI_CLI_PROMPT_LISTENER_H
#define CAI_CLI_PROMPT_LISTENER_H
#include <cai/cai.h>
#include <softline/softline.h>
#include <stdint.h>
typedef struct cai_cli_prompt_listener cai_cli_prompt_listener;
typedef struct cai_cli_prompt_scope {
  uint64_t attachment, generation, epoch;
  unsigned char instance[16];
  sl_t *editor;
  int ready, parked;
} cai_cli_prompt_scope;
typedef void (*cai_cli_prompt_scope_fn)(void *, cai_cli_prompt_scope *);
typedef void (*cai_cli_prompt_notice_fn)(void *, const char *);
typedef void (*cai_cli_prompt_changed_fn)(void *);
int cai_cli_prompt_listener_open(const char *, int, unsigned long,
                                 cai_cli_prompt_scope_fn,
                                 cai_cli_prompt_notice_fn,
                                 cai_cli_prompt_changed_fn, void *,
                                 cai_cli_prompt_listener **, cai_error *);
/** Move readiness watches to the active editor; retain the original attachment.
 */
int cai_cli_prompt_listener_watch(cai_cli_prompt_listener *, sl_t *,
                                  cai_error *);
const char *cai_cli_prompt_listener_address(const cai_cli_prompt_listener *);
/** Nonzero only while an incoming receipt owns the ordinary draft. */
int cai_cli_prompt_listener_draft(const cai_cli_prompt_listener *);
int cai_cli_prompt_listener_active(const cai_cli_prompt_listener *);
/** Settle the callback-driven sl_submit handoff and recover its original scope.
 * Called only from the native queue's OFFER callback, without editor reentry.
 */
int cai_cli_prompt_listener_submission(cai_cli_prompt_listener *,
                                       cai_cli_prompt_scope *);
/** Native editor idle detects a callback-submit rejected before transport. */
void cai_cli_prompt_listener_idle(cai_cli_prompt_listener *);
void cai_cli_prompt_listener_cancel(cai_cli_prompt_listener *);
void cai_cli_prompt_listener_close(cai_cli_prompt_listener *);
#endif
