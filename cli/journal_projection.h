#ifndef CAI_CLI_JOURNAL_PROJECTION_H
#define CAI_CLI_JOURNAL_PROJECTION_H
#include <cai/cai.h>
#include <lc/lc.h>
/** Project one stable native journal into portable, retained JSONL. Borrows the
 * collection and sink. Indexes contain metadata and byte references only;
 * provider text flows through bounded parser/writer chunks, without a runtime,
 * context reconstruction, temporary context file or second storage client.
 * Failure invalidates the candidate output; publication belongs to the caller.
 */
int cai_cli_journal_project(lc_collection *, cai_sink *, cai_error *);
#endif
