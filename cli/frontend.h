#ifndef CAI_CLI_FRONTEND_H
#define CAI_CLI_FRONTEND_H
#include "options.h"
#include <cai/app.h>
#include <cai/app_client.h>

/** Executable/native host startup adapter. prepare/pump belong to the frontend
 * owner and never block. A completed target is borrowed until the next pump.
 * NULL boot attaches immediately to target; libraries never implicitly fork. */
typedef struct cai_cli_frontend_boot {
  void *context;
  int (*prepare)(void *, struct pollfd *, uint64_t *, cai_error *);
  int (*pump)(void *, uint64_t, cai_app_client_config *, int *, cai_error *);
} cai_cli_frontend_boot;
/** Terminal attachment. Opens no runtime/store/SDK client and owns no model
 * worker. The backend queue and public client supply all accepted work.
 * Caller retains resolved configuration/options through return. */
int cai_cli_frontend_run(const cai_app_config *, const cai_cli_options *,
                         const cai_app_client_config *,
                         const cai_cli_frontend_boot *, int *, cai_error *);
#endif
