#ifndef CAI_CLI_CATALOG_WIRE_H
#define CAI_CLI_CATALOG_WIRE_H
#include "input_wire.h"
#include <cai/app_catalog.h>
typedef struct cai_cli_catalog_decoder cai_cli_catalog_decoder;
/** One bounded row per frame, with direct size derivation and token emission.
 * No complete catalog, JSON or sizing serialization is retained. */
int cai_cli_catalog_write(const cai_cli_input_wire_output *,
                          const cai_app_catalog_event *, cai_error *);
int cai_cli_catalog_decoder_open(cai_cli_catalog_decoder **, cai_error *);
int cai_cli_catalog_decode(cai_cli_catalog_decoder *,
                           const cai_protocol_event *,
                           const cai_app_catalog_event **, cai_error *);
void cai_cli_catalog_decoder_close(cai_cli_catalog_decoder *);
#endif
