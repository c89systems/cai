#include "cai_internal.h"
#include <cai/attachment.h>
#include <pthread.h>
#include <string.h>

struct cai_attachment_controller {
  pthread_t owner;
  cai_attachment_state state;
  uint64_t slots[CAI_ATTACHMENT_LIMIT];
  uint64_t serial, now, timeout;
};
static int owner(cai_attachment_controller *controller, cai_error *error) {
  if (controller == NULL || !pthread_equal(controller->owner, pthread_self()))
    return cai_set_error(error, CAI_ERR_INVALID,
                         "attachment controller requires its owner thread");
  return CAI_OK;
}
static int find(const cai_attachment_controller *controller, uint64_t id) {
  size_t i;
  if (id != 0U)
    for (i = 0U; i < CAI_ATTACHMENT_LIMIT; ++i)
      if (controller->slots[i] == id)
        return (int)i;
  return -1;
}
static void stop(cai_attachment_controller *controller,
                 cai_attachment_stop_reason reason) {
  if (controller->state.phase != CAI_ATTACHMENT_STOPPING) {
    controller->state.phase = CAI_ATTACHMENT_STOPPING;
    controller->state.stop_reason = reason;
    controller->state.controller = 0U;
    controller->state.deadline_ms = 0U;
  }
}
static int advance(cai_attachment_controller *controller, uint64_t now,
                   cai_error *error) {
  if (now < controller->now)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "attachment monotonic clock moved backwards");
  controller->now = now;
  if (controller->state.deadline_ms != 0U &&
      now >= controller->state.deadline_ms)
    stop(controller, CAI_ATTACHMENT_STARTUP_ABANDONED);
  return CAI_OK;
}
void cai_attachment_config_init(cai_attachment_config *config) {
  if (config != NULL)
    memset(config, 0, sizeof(*config));
}
int cai_attachment_controller_open(const cai_attachment_config *config,
                                   cai_attachment_controller **out,
                                   cai_error *error) {
  cai_attachment_controller *controller;
  if (out == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "attachment controller output is required");
  *out = NULL;
  if (config != NULL && config->persist != 0 && config->persist != 1)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "attachment persistence must be zero or one");
  controller = cai_alloc(NULL, sizeof(*controller));
  if (controller == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "allocate attachment controller");
  memset(controller, 0, sizeof(*controller));
  controller->owner = pthread_self();
  if (config != NULL) {
    controller->state.persist = config->persist;
    controller->timeout = config->first_attachment_timeout_ms;
  }
  if (controller->timeout == 0U)
    controller->timeout = 30000U;
  *out = controller;
  return CAI_OK;
}
void cai_attachment_controller_close(cai_attachment_controller *controller) {
  if (owner(controller, NULL) == CAI_OK)
    cai_free_mem(NULL, controller);
}
int cai_attachment_controller_ready(cai_attachment_controller *controller,
                                    uint64_t now_ms, cai_error *error) {
  int rc = owner(controller, error);
  if (rc != CAI_OK)
    return rc;
  if (controller->state.phase != CAI_ATTACHMENT_STARTING ||
      now_ms < controller->now)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "server is not starting or clock moved backwards");
  if (!controller->state.persist && UINT64_MAX - now_ms < controller->timeout)
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "first attachment deadline exceeds clock range");
  controller->now = now_ms;
  controller->state.phase = CAI_ATTACHMENT_READY;
  if (!controller->state.persist)
    controller->state.deadline_ms = now_ms + controller->timeout;
  return CAI_OK;
}
int cai_attachment_controller_attach(cai_attachment_controller *controller,
                                     uint64_t now_ms, uint64_t *id,
                                     cai_error *error) {
  size_t i;
  int rc = owner(controller, error);
  if (id != NULL)
    *id = 0U;
  if (rc != CAI_OK)
    return rc;
  if (id == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "attachment identity output is required");
  rc = advance(controller, now_ms, error);
  if (rc != CAI_OK)
    return rc;
  if (controller->state.phase != CAI_ATTACHMENT_READY)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "server is not ready for attachments");
  if (controller->serial == UINT64_MAX ||
      (controller->state.controller == 0U &&
       controller->state.generation == UINT64_MAX) ||
      controller->state.count == CAI_ATTACHMENT_LIMIT)
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "server attachment capacity exhausted");
  for (i = 0U; i < CAI_ATTACHMENT_LIMIT; ++i)
    if (controller->slots[i] == 0U)
      break;
  *id = controller->slots[i] = ++controller->serial;
  ++controller->state.count;
  controller->state.deadline_ms = 0U;
  if (controller->state.controller == 0U) {
    controller->state.controller = *id;
    ++controller->state.generation;
  }
  return CAI_OK;
}
int cai_attachment_controller_detach(cai_attachment_controller *controller,
                                     uint64_t id, cai_error *error) {
  int slot, rc = owner(controller, error);
  if (rc != CAI_OK)
    return rc;
  slot = find(controller, id);
  if (slot < 0)
    return cai_set_error(error, CAI_ERR_INVALID, "attachment is not connected");
  controller->slots[slot] = 0U;
  --controller->state.count;
  if (controller->state.controller == id) {
    controller->state.controller = 0U;
    if (controller->state.generation != UINT64_MAX)
      ++controller->state.generation;
  }
  if (controller->state.count == 0U && !controller->state.persist)
    stop(controller, CAI_ATTACHMENT_LAST_DETACHED);
  return CAI_OK;
}
int cai_attachment_controller_takeover(cai_attachment_controller *controller,
                                       uint64_t id, uint64_t *generation,
                                       cai_error *error) {
  int rc = owner(controller, error);
  if (generation != NULL)
    *generation = 0U;
  if (rc != CAI_OK)
    return rc;
  if (generation == NULL || find(controller, id) < 0 ||
      controller->state.phase != CAI_ATTACHMENT_READY)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "takeover requires a connected ready attachment");
  if (controller->state.controller != id) {
    if (controller->state.generation == UINT64_MAX)
      return cai_set_error(error, CAI_ERR_LIMIT,
                           "attachment generations exhausted");
    controller->state.controller = id;
    ++controller->state.generation;
  }
  *generation = controller->state.generation;
  return CAI_OK;
}
int cai_attachment_controller_authorize(cai_attachment_controller *controller,
                                        uint64_t id, uint64_t generation,
                                        cai_error *error) {
  int rc = owner(controller, error);
  if (rc != CAI_OK)
    return rc;
  if (id == 0U || controller->state.phase != CAI_ATTACHMENT_READY ||
      controller->state.controller != id ||
      controller->state.generation != generation)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "attachment is observing, stale or stopping");
  return CAI_OK;
}
int cai_attachment_controller_stop(cai_attachment_controller *controller,
                                   cai_error *error) {
  int rc = owner(controller, error);
  if (rc == CAI_OK)
    stop(controller, CAI_ATTACHMENT_STOP_REQUESTED);
  return rc;
}
int cai_attachment_controller_advance(cai_attachment_controller *controller,
                                      uint64_t now_ms, cai_error *error) {
  int rc = owner(controller, error);
  return rc == CAI_OK ? advance(controller, now_ms, error) : rc;
}
int cai_attachment_controller_get_state(cai_attachment_controller *controller,
                                        cai_attachment_state *out,
                                        cai_error *error) {
  int rc = owner(controller, error);
  if (rc != CAI_OK)
    return rc;
  if (out == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "attachment state output is required");
  *out = controller->state;
  return CAI_OK;
}
