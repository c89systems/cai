#ifndef CAI_CLI_TRANSCRIPT_WIRE_H
#define CAI_CLI_TRANSCRIPT_WIRE_H
#include "input_wire.h"
#include <cai/transcript.h>
#define CAI_CLI_TRANSCRIPT_CHUNK 4096U
typedef struct cai_cli_transcript_message {
  cai_protocol_header header;
  cai_transcript_event event;
  int64_t replay_turns;
} cai_cli_transcript_message;
typedef struct cai_cli_transcript_decoder cai_cli_transcript_decoder;
int cai_cli_transcript_decoder_open(cai_cli_transcript_decoder **, cai_error *);
int cai_cli_transcript_decode(cai_cli_transcript_decoder *,
                              const cai_protocol_event *,
                              const cai_cli_transcript_message **, cai_error *);
void cai_cli_transcript_decoder_close(cai_cli_transcript_decoder *);
/* Derive frame lengths directly from known fields/chunk lengths. No sizing
 * serialization, logical-value buffer, JSON or spool. */
int cai_cli_transcript_write(const cai_cli_input_wire_output *,
                             const cai_cli_transcript_message *, cai_error *);
#endif
