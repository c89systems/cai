#ifndef CAI_CLI_BATCH_H
#define CAI_CLI_BATCH_H
#include "frontend.h"
/** Instruction/goal/review adapter. No runtime, storage client, terminal editor
 * or response spool. Combined goal/instruction startup still awaits its
 * dependent-admission cutover. Explicit shell output uses bounded live spans.
 */
int cai_cli_batch_mode(const cai_cli_options *);
/** Borrow options and boot through return. One producer owns this invocation's
 * sequential instructions; exact accepted execution records determine results.
 * The final response is read through a native journal range into libmdf. */
int cai_cli_batch_run(const cai_app_config *, const cai_cli_options *,
                      const cai_app_client_config *,
                      const cai_cli_frontend_boot *, int *, cai_error *);
#endif
