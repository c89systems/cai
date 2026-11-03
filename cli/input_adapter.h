#ifndef CAI_CLI_INPUT_ADAPTER_H
#define CAI_CLI_INPUT_ADAPTER_H
#include <cai/input_controller.h>
#include <softline/softline.h>
typedef struct cai_cli_input_adapter cai_cli_input_adapter;
int cai_cli_input_adapter_open(cai_input_controller *, cai_cli_input_adapter **,
                               cai_error *);
void cai_cli_input_adapter_configure(cai_cli_input_adapter *, sl_config_t *);
int cai_cli_input_adapter_bind(cai_cli_input_adapter *, sl_t *, cai_error *);
int cai_cli_input_adapter_receive(cai_cli_input_adapter *, size_t, cai_error *);
/* Softline settles retained accepted receipts for native edit/promotion on its
 * next iteration. Allow that follow-up to acquire its hold before claiming. */
int cai_cli_input_adapter_settling(cai_cli_input_adapter *);
/* Must follow sl_destroy: the editor borrows the constructor context. */
void cai_cli_input_adapter_close(cai_cli_input_adapter *);
#endif
