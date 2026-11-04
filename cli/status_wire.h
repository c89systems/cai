#ifndef CAI_CLI_STATUS_WIRE_H
#define CAI_CLI_STATUS_WIRE_H
#include "input_wire.h"
#include <cai/app_view.h>
typedef struct cai_cli_status_decoder cai_cli_status_decoder;
void cai_cli_status_cleanup(cai_app_view *);
int cai_cli_status_copy(cai_app_view *, const cai_app_view *, cai_error *);
/* Elapsed-only differences during active pursuit do not schedule records. */
int cai_cli_status_equal(const cai_app_view *, const cai_app_view *);
int cai_cli_status_representable(const cai_app_view *);
/* Checks the complete known-field size before beginning a record. Oversized
 * or unrepresentable views produce a small unavailable record instead. */
int cai_cli_status_write(const cai_cli_input_wire_output *,
                         const cai_app_view *, cai_error *);
int cai_cli_status_decoder_open(cai_cli_status_decoder **, cai_error *);
int cai_cli_status_decode(cai_cli_status_decoder *, const cai_protocol_event *,
                          const cai_app_view **, cai_error *);
/* Move a completed candidate without copying its strings. */
void cai_cli_status_take(cai_cli_status_decoder *, cai_app_view *);
void cai_cli_status_decoder_close(cai_cli_status_decoder *);
#endif
