/** Installed-server probe: no combined facade or renderer headers are used. */
#include <assert.h>
#include <cai/app_server.h>
int main(void) {
  cai_app_config config;
  cai_app_status status;
  cai_app_server *server = NULL;
  cai_config_context context;
  unsigned long long id = 9U;
  int code;
  cai_app_config_init(&config);
  assert(cai_app_server_open(&config, &server, NULL) == CAI_OK &&
         server != NULL);
  cai_app_status_init(&status);
  assert(cai_app_server_get_status(server, &status, NULL) == CAI_OK);
  assert(status.outcome == CAI_APP_NOT_RUN && !status.running);
  assert(cai_app_server_status_fd(server) >= 0);
  assert(cai_app_server_control(server, CAI_APP_STOP, NULL, &id, NULL) ==
             CAI_ERR_INVALID &&
         id == 0U);
  assert(cai_app_server_join(server, &code, NULL) == CAI_ERR_INVALID);
  context.directory = "relative";
  context.environment = NULL;
  assert(cai_app_server_start(server, &context, 0, NULL, NULL, NULL) ==
         CAI_ERR_INVALID);
  cai_app_status_cleanup(&status);
  cai_app_server_close(server);
  return 0;
}
