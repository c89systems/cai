#include "error.h"
#include "input_adapter.h"
#include <string.h>
static int request(void *context, const cai_input_scope *scope,
                   const cai_input_request *value, cai_error *error) {
  return cai_input_connection_request(context, scope, value, error);
}
static int poll_one(void *context, cai_input_observation **out,
                    cai_error *error) {
  return cai_input_connection_poll(context, out, error);
}
static int descriptor(void *context) {
  return cai_input_connection_fd(context);
}
static void close_connection(void *context) {
  cai_input_connection_close(context);
}
static unsigned long long item_identity(void *context,
                                        unsigned long long conversation) {
  (void)context;
  return conversation;
}
int cai_cli_input_local_open(cai_input_controller *controller,
                             cai_cli_input_adapter **out, cai_error *error) {
  cai_input_connection *connection = NULL;
  cai_cli_input_transport transport;
  int rc;
  if (out == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "Input adapter output required");
  *out = NULL;
  rc = cai_input_controller_connect(controller, 0U, &connection, error);
  if (rc != CAI_OK)
    return rc;
  memset(&transport, 0, sizeof(transport));
  transport.context = connection;
  transport.request = request;
  transport.poll = poll_one;
  transport.fd = descriptor;
  transport.close = close_connection;
  transport.identity = item_identity;
  rc = cai_cli_input_adapter_open(&transport, out, error);
  if (rc != CAI_OK)
    cai_input_connection_close(connection);
  return rc;
}
