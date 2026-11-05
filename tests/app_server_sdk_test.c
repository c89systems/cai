/** Reuse the observable owner/transport cases through the standalone SDK.
 * This fixture adapter only aligns the two public startup signatures. */
#include <cai/app.h>
#include <cai/app_server.h>
static int sdk_start(cai_app_server *self, const cai_config_context *context,
                     int argc, const char *const *argv, cai_error *error) {
  cai_attachment_config lifetime;
  cai_attachment_config_init(&lifetime);
  lifetime.persist = 1;
  return cai_app_server_start(self, context, argc, argv, &lifetime, error);
}
#define CAI_TEST_SERVER_SDK 1
#define cai_app cai_app_server
#define cai_app_open cai_app_server_open
#define cai_app_close cai_app_server_close
#define cai_app_start sdk_start
#define cai_app_start_server cai_app_server_start
#define cai_app_join cai_app_server_join
#define cai_app_control cai_app_server_control
#define cai_app_get_status cai_app_server_get_status
#define cai_app_status_fd cai_app_server_status_fd
#define cai_app_observe_status cai_app_server_observe_status
#define cai_app_connect_input cai_app_server_connect_input
#define cai_app_resume_input cai_app_server_resume_input
#include "app_controller_test.c"
