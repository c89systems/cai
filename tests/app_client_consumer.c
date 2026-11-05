/** Public-header-only link/run probe for the independent client package. */
#include <assert.h>
#include <cai/app_client.h>
#include <string.h>
int main(void) {
  cai_app_client_config config;
  cai_app_client *client = NULL;
  cai_app_view view;
  unsigned long long turn;
  long long goal;
  cai_app_client_config_init(&config);
  assert(config.event == NULL && config.workspace == NULL);
  assert(cai_app_client_open(NULL, 0U, &client, NULL) == CAI_ERR_INVALID);
  assert(client == NULL);
  memset(&view, 0, sizeof(view));
  view.outcome = CAI_APP_NOT_RUN;
  cai_app_view_elapsed(&view, 0U, &turn, &goal);
  assert(turn == 0U && goal == 0LL);
  cai_app_client_close(client);
  return 0;
}
