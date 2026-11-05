/** Private shared lifecycle used by the combined facade and server SDK. */
#ifndef CAI_CLI_SERVER_INTERNAL_H
#define CAI_CLI_SERVER_INTERNAL_H
#include "app_internal.h"
#include <cai/app_server.h>
struct cai_app_server {
  cai_app_config config;
  cai_app_setting *settings;
  char **strings;
  size_t string_count;
  int running;
  cai_app_bridge *bridge;
  pthread_t owner;
  int asynchronous, controller_code;
  int argument_count;
  char **arguments;
  char *directory;
  cai_config_context startup;
  cai_attachment_config lifetime;
  int server, lifetime_explicit, frontend_startup;
};
int cai_app_owner_start(cai_app_server *, const cai_config_context *, int,
                        const char *const *, int, const cai_attachment_config *,
                        int, cai_error *);
#endif
