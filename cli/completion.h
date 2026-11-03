#ifndef CAI_CLI_COMPLETION_H
#define CAI_CLI_COMPLETION_H
#include <softline/softline.h>

/* The controller supplies names; the frontend owns editor interaction. Names
 * include their trigger, and emit/context are borrowed for this call only. */
typedef int (*cai_cli_completion_catalog_fn)(void *context, char trigger,
                                             sl_completion_emit_t emit,
                                             void *emit_context);
typedef struct cai_cli_completions {
  const char *workspace;
  cai_cli_completion_catalog_fn catalog;
  void *context;
} cai_cli_completions;

/* Keep the workspace and callback context alive until the editor is closed.
 * Files are resolved one directory at a time, never by recursively scanning.
 */
int cai_cli_completions_open(cai_cli_completions *completions, sl_t *editor,
                             const char *workspace,
                             cai_cli_completion_catalog_fn catalog,
                             void *context);
#endif
