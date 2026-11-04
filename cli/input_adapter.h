#ifndef CAI_CLI_INPUT_ADAPTER_H
#define CAI_CLI_INPUT_ADAPTER_H
#include <cai/input_controller.h>
#include <softline/softline.h>
typedef struct cai_cli_input_adapter cai_cli_input_adapter;
/* Frontend-owned transport binding. request must copy/enqueue before returning;
 * it cannot run the model, wait for storage or reenter Softline. poll/fd are
 * optional together: poll returns an owned observation, closed by receive().
 * Native connections use them; socket owners push borrowed
 * observations through observe(). close releases only transport ownership,
 * never clears/stops the accepted backend queue. Context outlives detach.
 * open copies this table; successful construction transfers close ownership
 * to the adapter. On failure the caller still owns the transport context. */
typedef struct cai_cli_input_transport {
  void *context;
  int (*request)(void *, const cai_input_scope *, const cai_input_request *,
                 cai_error *);
  int (*poll)(void *, cai_input_observation **, cai_error *);
  int (*fd)(void *);
  void (*close)(void *);
  /* Opaque item/revision namespace, distinct from the live request epoch.
   * A retained socket editor keeps it stable across a verified producer
   * reconnect; native context selection uses its conversation epoch. */
  unsigned long long (*identity)(void *, unsigned long long conversation);
} cai_cli_input_transport;
int cai_cli_input_adapter_open(const cai_cli_input_transport *,
                               cai_cli_input_adapter **, cai_error *);
/* Native composition, using the same adapter without a wire loopback. */
int cai_cli_input_local_open(cai_input_controller *, cai_cli_input_adapter **,
                             cai_error *);
void cai_cli_input_adapter_configure(cai_cli_input_adapter *, sl_config_t *);
int cai_cli_input_adapter_bind(cai_cli_input_adapter *, sl_t *, cai_error *);
int cai_cli_input_adapter_receive(cai_cli_input_adapter *, size_t, cai_error *);
/* Owner-thread borrowed row/result; never takes its destructor. */
int cai_cli_input_adapter_observe(cai_cli_input_adapter *,
                                  const cai_input_observation *, cai_error *);
/* Softline settles retained accepted receipts for native edit/promotion on its
 * next iteration. Allow that follow-up to acquire its hold before claiming. */
int cai_cli_input_adapter_settling(cai_cli_input_adapter *);
/* Must follow sl_destroy: the editor borrows the constructor context. */
void cai_cli_input_adapter_close(cai_cli_input_adapter *);
#endif
