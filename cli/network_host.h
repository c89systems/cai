/** @file Backend network ownership and worker-to-controller action mailbox. */
#ifndef CAI_CLI_NETWORK_HOST_H
#define CAI_CLI_NETWORK_HOST_H
#include "network_registry.h"
typedef struct cai_cli_network_host cai_cli_network_host;
typedef struct cai_cli_network_scope {
  uint64_t controller, generation, epoch;
  char session[129];
} cai_cli_network_scope;
typedef struct cai_cli_network_host_config {
  cai_cli_pouch *pouch;
  const cai_cli_options *options;
  const char *workspace, *instance;
  void *context;
  int (*scope)(void *, cai_cli_network_scope *, cai_error *);
  int (*send)(void *, uint64_t attachment, const cai_app_network_event *,
              cai_error *);
} cai_cli_network_host_config;
int cai_cli_network_host_open(const cai_cli_network_host_config *,
                              cai_cli_network_host **, cai_error *);
int cai_cli_network_host_receive(cai_cli_network_host *, uint64_t,
                                 const cai_app_network_event *, cai_error *);
void cai_cli_network_host_detach(cai_cli_network_host *, uint64_t);
int cai_cli_network_host_fd(cai_cli_network_host *);
uint64_t cai_cli_network_host_deadline(cai_cli_network_host *);
int cai_cli_network_host_pump(cai_cli_network_host *, cai_error *);
/** Tool-worker entry; retains arguments/sink only while this call is waiting.
 */
int cai_cli_network_host_call(void *, const char *, const char *, cai_sink *,
                              cai_error *);
void cai_cli_network_host_stop(cai_cli_network_host *);
void cai_cli_network_host_cancel(cai_cli_network_host *);
void cai_cli_network_host_close(cai_cli_network_host *);
#endif
