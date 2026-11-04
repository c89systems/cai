#ifndef CAI_CLI_TRANSCRIPT_PRESENTER_H
#define CAI_CLI_TRANSCRIPT_PRESENTER_H
#include "softline_presenter.h"
#include <cai/transcript.h>

/** Owner-thread projection from decoded transcript spans to the shared
 * terminal presenter. Owns fixed metadata only; no runtime, store, worker,
 * queue, complete message or replay mirror. The presenter is borrowed.
 * Execution/tool records have their separate typed client callback; this
 * adapter never prints their encoded JSON or repeats queued admissions. */
typedef struct cai_cli_transcript_presenter cai_cli_transcript_presenter;
int cai_cli_transcript_presenter_open(cai_cli_presenter *,
                                      cai_cli_transcript_presenter **,
                                      cai_error *);
/** Suitable for cai_app_client_config.transcript. BEGIN/DATA render
 * provisionally. END alone completes a literal input; consecutive assistant
 * deltas retain one Markdown document until assistant_text_end. Errors stick
 * until reset. Unknown event types are skipped without collecting their data.
 * This is presentation, not default tail/visibility selection. */
int cai_cli_transcript_presenter_event(void *, const cai_transcript_event *,
                                       cai_error *);
/** Abandon unfinished renderer documents without flushing, acknowledging
 * cursors, resubmitting input or changing the draft. Pending Markdown words
 * can span already accepted delta records: restarting this view must replay
 * from a document boundary, not blindly from the raw client's last cursor.
 * Already displayed bytes remain visible. Call before closing the presenter. */
void cai_cli_transcript_presenter_reset(cai_cli_transcript_presenter *);
void cai_cli_transcript_presenter_close(cai_cli_transcript_presenter *);
#endif
