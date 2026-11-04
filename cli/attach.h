#ifndef CAI_CLI_ATTACH_H
#define CAI_CLI_ATTACH_H
#include <cai/app.h>
/** Executable discovery/quiet-exec composition. The reusable terminal
 * frontend itself never forks or opens storage. */
int cai_cli_attach_run(const cai_app_config *, int, char **, int *,
                       cai_error *);
#endif
