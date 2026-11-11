/** @file Frontend TCP action worker; the backend never opens these sockets. */
#ifndef CAI_CLI_PROMPT_SEND_H
#define CAI_CLI_PROMPT_SEND_H
#include <cai/app_network.h>
typedef struct cai_cli_prompt_send cai_cli_prompt_send;
int cai_cli_prompt_send_open(const cai_app_network_event *, int ipv6,
                             cai_cli_prompt_send **, cai_error *);
int cai_cli_prompt_send_fd(cai_cli_prompt_send *);
void cai_cli_prompt_send_cancel(cai_cli_prompt_send *);
/** Returns nonzero after completion; the result is borrowed through close. */
int cai_cli_prompt_send_result(cai_cli_prompt_send *,
                               const cai_app_network_event **);
/** Cancels then joins; call after withdrawing the attachment's capability. */
void cai_cli_prompt_send_close(cai_cli_prompt_send *);
#endif
