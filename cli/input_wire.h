#ifndef CAI_CLI_INPUT_WIRE_H
#define CAI_CLI_INPUT_WIRE_H
#include <cai/input_controller.h>
#include <cai/protocol.h>
#define CAI_CLI_INPUT_TEXT_MAX 66048U
typedef enum cai_cli_input_result_kind {
  CAI_CLI_INPUT_RECEIPT = 0,
  CAI_CLI_INPUT_BIND_REJECTED,
  CAI_CLI_INPUT_RESET,
  CAI_CLI_INPUT_CONTROL_RESULT,
  CAI_CLI_INPUT_HISTORY_RESULT
} cai_cli_input_result_kind;
typedef struct cai_cli_input_wire_message {
  cai_protocol_header header;
  cai_input_request request;
  cai_input_observation observation;
  unsigned long long producer;
  const char *session;
  unsigned long long delivery_after;
  int managed_delivery;
  cai_cli_input_result_kind category;
} cai_cli_input_wire_message;
typedef struct cai_cli_input_wire_decoder cai_cli_input_wire_decoder;
int cai_cli_input_wire_decoder_open(cai_cli_input_wire_decoder **, cai_error *);
/* A complete borrowed command/row is returned only after COMPLETE; valid until
 * the next HEADER or close. Holds one bounded input string, never a snapshot.
 */
int cai_cli_input_wire_decode(cai_cli_input_wire_decoder *,
                              const cai_protocol_event *,
                              const cai_cli_input_wire_message **, cai_error *);
void cai_cli_input_wire_decoder_close(cai_cli_input_wire_decoder *);
typedef struct cai_cli_input_wire_output {
  void *context;
  int (*begin)(void *, const cai_protocol_header *, uint32_t, cai_error *);
  int (*write)(void *, const cai_cbor_token *, cai_error *);
  int (*end)(void *, cai_error *);
} cai_cli_input_wire_output;
/* Synchronously emit fields, without gathering/serializing a value to size it.
 * Borrowed input strings must remain valid through this call. */
int cai_cli_input_wire_write(const cai_cli_input_wire_output *,
                             const cai_cli_input_wire_message *, cai_error *);
#endif
