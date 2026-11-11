/** @file cai/app_network.h Typed attachment capabilities and frontend actions.
 */
#ifndef CAI_APP_NETWORK_H
#define CAI_APP_NETWORK_H
#include <cai/app_client.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum cai_app_network_operation {
  CAI_APP_NETWORK_CAPABILITY = 0,
  CAI_APP_NETWORK_PUBLISHER,
  CAI_APP_NETWORK_SEND,
  CAI_APP_NETWORK_RECEIPT,
  CAI_APP_NETWORK_CANCEL,
  CAI_APP_NETWORK_NOTICE
} cai_app_network_operation;
#define CAI_APP_NETWORK_CAN_SEND 1U
#define CAI_APP_NETWORK_AVAILABLE 2U
#define CAI_APP_NETWORK_UNCERTAIN 4U
/** One bounded record. Strings are borrowed through the callback/write only.
 * The envelope pins original attachment/generation/session epoch. Identity
 * never follows a reconnect or takeover automatically. */
typedef struct cai_app_network_event {
  cai_protocol_header header;
  cai_app_network_operation operation;
  uint64_t timeout_ms, transferred;
  unsigned int flags;
  int code;
  const char *address, *advertise, *interface_name, *socat, *prompt, *reason;
} cai_app_network_event;
typedef int (*cai_app_network_event_fn)(void *, const cai_app_network_event *,
                                        cai_error *);
/** Borrow an adapter through client close. No registration or socket opens. */
int cai_app_client_network_handler(cai_app_client *, cai_app_network_event_fn,
                                   void *, cai_error *);
/** Send a capability, receipt or diagnostic notice with the original scope. */
int cai_app_client_network_write(cai_app_client *,
                                 const cai_app_network_event *, cai_error *);
#ifdef __cplusplus
}
#endif
#endif
