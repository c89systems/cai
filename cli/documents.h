#ifndef CAI_CLI_DOCUMENTS_H
#define CAI_CLI_DOCUMENTS_H
#include <cai/agent_runtime.h>
/* Owner-thread adapter from the shared command handlers to execution history.
 * No renderer, complete-document accumulator, store or worker is owned here. */
typedef struct cai_cli_documents {
  cai_agent_runtime *runtime;
  cai_agent_execution *writer;
  cai_error error;
  char call[256];
  int active;
} cai_cli_documents;
int cai_cli_documents_bind(cai_cli_documents *, cai_agent_runtime *,
                           cai_error *);
int cai_cli_documents_begin(cai_cli_documents *, const cai_input_claim *,
                            cai_error *);
int cai_cli_documents_write(cai_cli_documents *, const void *, size_t);
int cai_cli_documents_finish(cai_cli_documents *);
int cai_cli_documents_delegate(cai_cli_documents *, const char *, cai_error *);
int cai_cli_documents_complete(cai_cli_documents *, int, cai_error *);
void cai_cli_documents_close(cai_cli_documents *);
#endif
