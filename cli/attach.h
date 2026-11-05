#ifndef CAI_CLI_ATTACH_H
#define CAI_CLI_ATTACH_H
#include "frontend.h"
#include <cai/app.h>
/** Executable-only, single-threaded discovery/launch state. The adapter drives
 * identity probes and the exact child watch through frontend interests. Options
 * and argv are borrowed until close; NULL program selects this executable only
 * if a launch is needed. No runtime or storage client is opened here. */
typedef struct cai_cli_attach_boot cai_cli_attach_boot;
int cai_cli_attach_boot_open(const cai_cli_options *, const char *program,
                             int argc, const char *const *argv,
                             cai_cli_attach_boot **, cai_error *);
const cai_cli_frontend_boot *cai_cli_attach_boot_adapter(cai_cli_attach_boot *);
/** After the terminal returns, cancel/reap an unfinished owned launch. Ready
 * owners follow attachment lifetime policy. Call only before executable exit:
 * a ready surviving child's local watch is abandoned without signalling it. */
void cai_cli_attach_boot_close(cai_cli_attach_boot *);
/** Executable discovery/quiet-exec composition. The reusable terminal
 * frontend itself never forks or opens storage. */
int cai_cli_attach_run(const cai_app_config *, int, char **, int *,
                       cai_error *);
#endif
