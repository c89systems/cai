/** @file Backend endpoint registry and best-effort event publisher. */
#ifndef CAI_CLI_NETWORK_REGISTRY_H
#define CAI_CLI_NETWORK_REGISTRY_H
#include "options.h"
#include "pouch.h"
#include <cai/app_network.h>
typedef struct cai_cli_network_registry cai_cli_network_registry;
int cai_cli_network_registry_open(cai_cli_pouch *, const cai_cli_options *,
                                  const char *, const char *,
                                  cai_cli_network_registry **, cai_error *);
int cai_cli_network_registry_listener(cai_cli_network_registry *, uint64_t,
                                      const cai_app_network_event *,
                                      const char *, int, cai_error *);
void cai_cli_network_registry_remove(cai_cli_network_registry *, uint64_t);
int cai_cli_network_registry_session(cai_cli_network_registry *, const char *,
                                     cai_error *);
int cai_cli_network_registry_list(cai_cli_network_registry *, const char *,
                                  const char *, cai_sink *, cai_error *);
/** Read-only utility listing through an already open history client. */
int cai_cli_network_registry_print(cai_cli_pouch *, const cai_cli_options *,
                                   const char *, cai_sink *, cai_error *);
/** NULL when publishing is disabled. Metadata is borrowed through close. */
const cai_app_network_event *
cai_cli_network_registry_publisher(cai_cli_network_registry *);
uint64_t cai_cli_network_registry_deadline(cai_cli_network_registry *);
int cai_cli_network_registry_renew(cai_cli_network_registry *, cai_error *);
void cai_cli_network_registry_close(cai_cli_network_registry *);
#endif
