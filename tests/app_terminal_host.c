#include <cai/app_terminal.h>
#include <lc/lc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

int __wrap_lc_client_open(const lc_client_config *, lc_client **, lc_error *);
int __wrap_lc_client_open(const lc_client_config *config, lc_client **out,
                          lc_error *error) {
  (void)config;
  (void)out;
  (void)error;
  abort(); /* A public terminal receiver must never create a storage client. */
}
typedef struct invocation {
  int argc, code, rc;
  char **argv;
} invocation;
static void *terminal(void *context) {
  invocation *call = context;
  cai_app_client_config target;
  cai_error error;
  cai_app_client_config_init(&target);
  target.runtime_directory = call->argv[1];
  target.workspace = call->argv[2];
  cai_error_init(&error);
  call->rc = cai_app_terminal_run(NULL, &target, call->argc - 3,
                                  (const char *const *)(call->argv + 3),
                                  &call->code, &error);
  if (call->rc != CAI_OK)
    fprintf(stderr, "terminal host: %s\n",
            error.message != NULL ? error.message : "failed");
  cai_error_cleanup(&error);
  return NULL;
}
int main(int argc, char **argv) {
  pthread_t owner;
  invocation call;
  if (argc < 3)
    return 2;
  call.argc = argc;
  call.argv = argv;
  call.code = call.rc = 1;
  /* No stack override: this exercises the public adapter on musl's default
   * 128 KiB pthread stack, including configuration, renderers and callbacks. */
  if (pthread_create(&owner, NULL, terminal, &call) != 0)
    return 1;
  if (pthread_join(owner, NULL) != 0)
    return 1;
  return call.rc == CAI_OK ? call.code : 1;
}
