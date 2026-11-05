#define _POSIX_C_SOURCE 200809L
#include "../cli/app_internal.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static int forbid_allocation, acknowledgements;
void *__real_calloc(size_t, size_t);
char *__real_strdup(const char *);
void *__wrap_calloc(size_t, size_t);
char *__wrap_strdup(const char *);
void *__wrap_calloc(size_t count, size_t size) {
  assert(!forbid_allocation);
  return __real_calloc(count, size);
}
char *__wrap_strdup(const char *text) {
  assert(!forbid_allocation);
  return __real_strdup(text);
}
static void acknowledged(void *context, unsigned long long id, int status,
                         const cai_error *error) {
  (void)context;
  (void)error;
  assert(status == CAI_OK && id != 0U);
  ++acknowledgements;
}
int main(void) {
  cai_app_config config;
  cai_app_bridge *bridge;
  cai_app_command *command;
  cai_error error;
  unsigned long long id, stop, next = 0U;
  size_t count;
  cai_app_config_init(&config);
  config.control_callback = acknowledged;
  cai_error_init(&error);
  assert(cai_app_bridge_open(&bridge, &config, &error) == CAI_OK);
  cai_app_bridge_begin(bridge);
  /* A fixture without a runtime: readiness is the only admission input. */
  bridge->status.runtime_ready = 1;
  for (count = 0U; count < 64U; ++count) {
    assert(cai_app_bridge_control(bridge, CAI_APP_PAUSE_GOAL, NULL, &id,
                                  &error) == CAI_OK);
    assert(id == count + 1U);
  }
  assert(cai_app_bridge_control(bridge, CAI_APP_PAUSE_GOAL, NULL, &id,
                                &error) == CAI_ERR_INVALID &&
         id == 0U);
  cai_error_cleanup(&error);
  forbid_allocation = 1;
  assert(cai_app_bridge_control(bridge, CAI_APP_STOP, NULL, &stop, &error) ==
         CAI_OK);
  assert(stop == 65U && cai_app_bridge_stopping(bridge));
  assert(cai_app_bridge_control(bridge, CAI_APP_STOP, NULL, &id, &error) ==
             CAI_OK &&
         id == stop);
  while ((command = cai_app_bridge_take(bridge)) != NULL) {
    assert(command->id == ++next);
    assert(command->type == (next == stop ? CAI_APP_STOP : CAI_APP_PAUSE_GOAL));
    cai_app_bridge_ack(bridge, command, CAI_OK, NULL);
  }
  assert(next == stop && acknowledgements == 65);
  /* An acknowledged STOP remains idempotent until the invocation ends. */
  assert(cai_app_bridge_control(bridge, CAI_APP_STOP, NULL, &id, &error) ==
             CAI_OK &&
         id == stop);
  assert(cai_app_bridge_take(bridge) == NULL);
  forbid_allocation = 0;
  cai_app_bridge_end(bridge, 0, NULL);
  cai_app_bridge_begin(bridge);
  bridge->status.runtime_ready = 1;
  for (count = 0U; count < 63U; ++count)
    assert(cai_app_bridge_control(bridge, CAI_APP_PAUSE_GOAL, NULL, &id,
                                  &error) == CAI_OK);
  assert(cai_app_bridge_control(bridge, CAI_APP_STOP, NULL, &stop, &error) ==
             CAI_OK &&
         stop == 64U);
  /* STOP occupies no ordinary slot, including when it precedes new work. */
  assert(cai_app_bridge_control(bridge, CAI_APP_PAUSE_GOAL, NULL, &id,
                                &error) == CAI_OK &&
         id == 65U);
  assert(cai_app_bridge_control(bridge, CAI_APP_PAUSE_GOAL, NULL, &id,
                                &error) == CAI_ERR_INVALID);
  cai_error_cleanup(&error);
  assert(cai_app_bridge_control(bridge, CAI_APP_STOP, NULL, &id, &error) ==
             CAI_OK &&
         id == stop);
  next = 0U;
  while ((command = cai_app_bridge_take(bridge)) != NULL) {
    assert(command->id == ++next);
    assert(command->type == (next == stop ? CAI_APP_STOP : CAI_APP_PAUSE_GOAL));
    cai_app_bridge_ack(bridge, command, CAI_OK, NULL);
  }
  assert(next == 65U && acknowledgements == 130);
  cai_app_bridge_end(bridge, 0, NULL);
  cai_app_bridge_begin(bridge);
  assert(!cai_app_bridge_stopping(bridge));
  /* Startup STOP reuses the same reserved storage, without requiring READY. */
  forbid_allocation = 1;
  assert(cai_app_bridge_control(bridge, CAI_APP_STOP, NULL, &id, &error) ==
             CAI_OK &&
         id == 1U);
  command = cai_app_bridge_take(bridge);
  assert(command != NULL && command->type == CAI_APP_STOP);
  cai_app_bridge_ack(bridge, command, CAI_OK, NULL);
  forbid_allocation = 0;
  cai_app_bridge_end(bridge, 0, NULL);
  assert(acknowledgements == 131);
  cai_app_bridge_close(bridge);
  cai_error_cleanup(&error);
  return 0;
}
