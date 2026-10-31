#define _POSIX_C_SOURCE 200809L
#include <cai/app.h>
#include <pthread.h>
#include <stdio.h>

typedef struct worker_invocation {
  int argc;
  char **argv;
  int code;
} worker_invocation;

static void *run_application(void *context) {
  worker_invocation *invocation = context;
  cai_app_config config;
  cai_app *app = NULL;
  cai_error error;
  int rc;
  cai_app_config_init(&config);
  cai_error_init(&error);
  rc = cai_app_open(&config, &app, &error);
  if (rc == CAI_OK)
    rc = app->run(app, invocation->argc - 1,
                  (const char *const *)(invocation->argv + 1),
                  &invocation->code, &error);
  if (rc != CAI_OK) {
    fprintf(stderr, "embedded worker: %s\n",
            error.message != NULL ? error.message : "run failed");
    invocation->code = 1;
  }
  if (app != NULL)
    app->close(app);
  cai_error_cleanup(&error);
  return NULL;
}

int main(int argc, char **argv) {
  worker_invocation invocation;
  pthread_t thread;
  const pthread_attr_t *attributes = NULL;
#ifdef __GLIBC__
  pthread_attr_t bounded;
  /* Reproduce the musl contract in native glibc checks too. Musl itself uses
   * NULL/default attributes; no production or fixture stack is enlarged. */
  if (pthread_attr_init(&bounded) != 0)
    return 2;
  if (pthread_attr_setstacksize(&bounded, 128U * 1024U) != 0) {
    pthread_attr_destroy(&bounded);
    return 2;
  }
  attributes = &bounded;
#endif
  invocation.argc = argc;
  invocation.argv = argv;
  invocation.code = 1;
  if (pthread_create(&thread, attributes, run_application, &invocation) != 0)
    return 2;
#ifdef __GLIBC__
  pthread_attr_destroy(&bounded);
#endif
  if (pthread_join(thread, NULL) != 0)
    return 2;
  return invocation.code;
}
