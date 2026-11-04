#ifndef CAI_CLI_BOOTSTRAP_H
#define CAI_CLI_BOOTSTRAP_H
#include <cai/app.h>
#define CAI_CLI_BOOTSTRAP_SESSION 129U
#define CAI_CLI_BOOTSTRAP_PATH 4096U
#define CAI_CLI_BOOTSTRAP_MESSAGE 1025U
/* One bounded metadata observation, never a transcript or encoded record. */
typedef struct cai_cli_bootstrap_message {
  int ready, exit_code, code, clipped, persist;
  unsigned char instance[CAI_WORKSPACE_INSTANCE_BYTES];
  char session[CAI_CLI_BOOTSTRAP_SESSION];
  char log_path[CAI_CLI_BOOTSTRAP_PATH];
  char message[CAI_CLI_BOOTSTRAP_MESSAGE];
} cai_cli_bootstrap_message;
typedef struct cai_cli_bootstrap_decoder cai_cli_bootstrap_decoder;
int cai_cli_bootstrap_decoder_open(cai_cli_bootstrap_decoder **, cai_error *);
/* The result is borrowed and becomes visible only on protocol COMPLETE. */
int cai_cli_bootstrap_decode(cai_cli_bootstrap_decoder *,
                             const cai_protocol_event *,
                             const cai_cli_bootstrap_message **, cai_error *);
void cai_cli_bootstrap_decoder_close(cai_cli_bootstrap_decoder *);
int cai_cli_bootstrap_write(cai_wire_peer *, const cai_cli_bootstrap_message *,
                            cai_error *);
/* Explicit process failure wins. Otherwise only CAI_APP_FAILED selects the
 * status error; recovered diagnostics do not turn successful STOP into failure.
 */
int cai_cli_bootstrap_status(cai_cli_bootstrap_message *, int ready,
                             int exit_code, const cai_app_status *,
                             const cai_error *, cai_error *);
#endif
