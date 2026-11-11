/** @file Field-by-field bounded network records over the workspace CBOR link.
 */
#ifndef CAI_CLI_NETWORK_WIRE_H
#define CAI_CLI_NETWORK_WIRE_H
#include "input_wire.h"
#include <cai/app_network.h>
typedef struct cai_cli_network_decoder cai_cli_network_decoder;
int cai_cli_network_write(const cai_cli_input_wire_output *,
                          const cai_app_network_event *, cai_error *);
int cai_cli_network_decoder_open(cai_cli_network_decoder **, cai_error *);
int cai_cli_network_decode(cai_cli_network_decoder *,
                           const cai_protocol_event *,
                           const cai_app_network_event **, cai_error *);
void cai_cli_network_decoder_close(cai_cli_network_decoder *);
#endif
