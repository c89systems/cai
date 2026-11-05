#ifndef CAI_CLI_EXISTING_FRONTEND_H
#define CAI_CLI_EXISTING_FRONTEND_H
#include "app_internal.h"
#include "config.h"
/** Native run's explicit --server composition. Discovery and presentation
 * only: never launches a process, opens storage or constructs a runtime.
 * Host STOP detaches this client; the remote owner keeps its own lifetime. */
int cai_cli_existing_frontend(const cai_app_config *, cai_app_bridge *,
                              const cai_cli_options *, cai_config *, int *,
                              cai_error *);
#endif
