#ifndef CAI_CLI_JOURNAL_PROJECTION_H
#define CAI_CLI_JOURNAL_PROJECTION_H
#include "transcript.h"
#include <cai/cai.h>
#include <lc/lc.h>

/** One stable retained selection over a borrowed collection receiver. Calls
 * are synchronous and serialized; a read/consumer failure makes the view
 * terminal. Opening streams past payload bytes and keeps only revisions,
 * byte references and metadata. Keep the
 * receiver and its SDK client alive until after closing this view. */
typedef struct cai_cli_journal_view cai_cli_journal_view;
typedef struct cai_cli_journal_options {
  int (*cancelled)(void *context);
  void *cancel_context;
} cai_cli_journal_options;
/** NULL options selects no cancellation callback; otherwise the callbacks are
 * copied and their context remains borrowed until close. */
int cai_cli_journal_view_open(lc_collection *, const cai_cli_journal_options *,
                              cai_cli_journal_view **, cai_error *);
void cai_cli_journal_view_close(cai_cli_journal_view *);
/** Emit portable JSONL with retained metadata, checkpoint and observations. */
int cai_cli_journal_view_jsonl(cai_cli_journal_view *, cai_sink *, cai_error *);
/** Emit the final metadata object, including folded boundary/visibility arrays.
 * Values flow through the JSON writer; arbitrary text is not accumulated. */
int cai_cli_journal_view_metadata(cai_cli_journal_view *, cai_sink *,
                                  cai_error *);
/** Producer adapter for cai_handover_config.history. Emits decoded retained
 * length-framed provider arrays; each invocation is one linear range read.
 * Repeated calls reuse the same captured head and receiver. */
int cai_cli_journal_view_history(void *, cai_sink *, cai_error *);
/** Stream only retained observations. BEGIN/DATA remain provisional until END;
 * callers must abandon their candidate if the view or consumer fails. */
int cai_cli_journal_view_events(cai_cli_journal_view *, cai_cli_transcript_fn,
                                void *, cai_error *);
/** Project one stable native journal into portable, retained JSONL. Borrows the
 * collection and sink. Indexes contain metadata and byte references only;
 * provider text flows through bounded parser/writer chunks, without a runtime,
 * context reconstruction, temporary context file or second storage client.
 * Failure invalidates the candidate output; publication belongs to the caller.
 */
int cai_cli_journal_project(lc_collection *, cai_sink *, cai_error *);
#endif
