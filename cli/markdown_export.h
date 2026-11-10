#ifndef CAI_CLI_MARKDOWN_EXPORT_H
#define CAI_CLI_MARKDOWN_EXPORT_H
#include "journal_projection.h"
#include "pouch.h"
#include <cai/handover.h>
/** Write an existing handover directly from retained native ranges. NULL
 * active prepares closed-session metadata/policy; otherwise the captured
 * live header, goal and instructions remain authoritative. No runtime calls
 * or context spool. All source callbacks and their contexts remain borrowed. */
int cai_cli_markdown_export(cai_cli_journal_view *, cai_cli_pouch *,
                            const cai_cli_session *, const char *workspace,
                            const cai_handover_config *active,
                            const cai_cli_options *, cai_sink *, cai_error *);
#endif
