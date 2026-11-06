#ifndef CAI_CLI_MARKDOWN_EXPORT_H
#define CAI_CLI_MARKDOWN_EXPORT_H
#include "journal_projection.h"
#include "pouch.h"
/** Write a closed conversation's existing handover semantics directly from
 * its retained native view. No client, agent, runtime or context spool. */
int cai_cli_markdown_export(cai_cli_journal_view *, cai_cli_pouch *,
                            const cai_cli_session *, const char *workspace,
                            cai_sink *, cai_error *);
#endif
