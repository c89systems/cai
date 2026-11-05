#ifndef CAI_CLI_BATCH_DOCUMENT_H
#define CAI_CLI_BATCH_DOCUMENT_H
#include <cai/app_client.h>
#include <cai/transcript_selection.h>
/** Metadata for one correlated execution field. Data stays in the native
 * journal. Filters are copied so a following input cannot change the selected
 * document. Root commands may move across a deliberate conversation handoff. */
typedef struct cai_cli_batch_document {
  cai_transcript_selection_result range;
  char kind[64], source[129], call[256], parent[256];
  uint64_t offset;
  int failed;
} cai_cli_batch_document;
/** Copy exact kind/source/call/parent filters. NULL optional filters impose
 * no identity constraint; a parent filter selects attributed child spans. */
int cai_cli_batch_document_init(cai_cli_batch_document *, const char *kind,
                                const char *source, const char *call,
                                const char *parent, cai_error *);
/** Discard selection and error state, preserving the copied identity filters.
 */
void cai_cli_batch_document_reset(cai_cli_batch_document *);
/** before is the accepted outer cursor preceding this typed END. Only
 * matching fields advance the selected range and decoded byte count. */
int cai_cli_batch_document_capture(cai_cli_batch_document *,
                                   const cai_transcript_position *before,
                                   const cai_app_execution_event *,
                                   cai_error *);
/** Close selection only between complete logical fields. Failed work cannot
 * be opened for successful output projection. */
int cai_cli_batch_document_finish(cai_cli_batch_document *, int code,
                                  cai_error *);

/** One cursor-continuation pass through the typed client. It retains fixed
 * metadata only, borrowing the sink through completion. Typed execution is
 * consumed before its enclosing raw END, as app_client guarantees. */
typedef struct cai_cli_batch_document_reader {
  cai_cli_batch_document document;
  cai_sink *sink;
  uint64_t stream, bytes, offset;
  int started, done, failed, busy;
} cai_cli_batch_document_reader;
int cai_cli_batch_document_read(cai_cli_batch_document_reader *,
                                const cai_cli_batch_document *, cai_sink *,
                                cai_error *);
int cai_cli_batch_document_execution(cai_cli_batch_document_reader *,
                                     const cai_app_execution_event *,
                                     cai_error *);
int cai_cli_batch_document_event(cai_cli_batch_document_reader *,
                                 const cai_transcript_event *, cai_error *);
#endif
