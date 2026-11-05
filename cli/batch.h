#ifndef CAI_CLI_BATCH_H
#define CAI_CLI_BATCH_H
#include "frontend.h"
/** Instruction/goal/review and redirected-input adapter. No runtime, storage
 * client, terminal editor or response spool. Combined goal/instruction startup
 * is one durable input; explicit shell output uses bounded live spans.
 */
int cai_cli_batch_mode(const cai_cli_options *);
/** Borrow options and boot through return. One producer owns this invocation's
 * sequential instructions; exact accepted execution records determine results.
 * Supplied work selects its final response through a native journal range.
 * Redirected input instead admits bounded lines into the backend queue and
 * renders the live transcript, draining accepted work at EOF. */
int cai_cli_batch_run(const cai_app_config *, const cai_cli_options *,
                      const cai_app_client_config *,
                      const cai_cli_frontend_boot *, int *, cai_error *);
#endif
