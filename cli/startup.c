#include "startup.h"
#include "error.h"
#include "goal_input.h"
#include "input_wire.h"
#include <stdlib.h>
#include <string.h>

int cai_cli_startup_next(cai_cli_startup *cursor, const cai_cli_options *o,
                         const char *session, char **text, int *control,
                         cai_error *error) {
  const char *prefix = NULL, *value = NULL;
  char number[32];
  size_t a, b;
  *text = NULL;
  *control = 0;
  while (value == NULL && cursor->stage < CAI_CLI_STARTUP_DONE) {
    switch (cursor->stage++) {
    case CAI_CLI_STARTUP_SELECT:
      if (o->new_session) {
        prefix = "/new";
        value = "";
      } else if (o->resume_id != NULL && strcmp(o->resume_id, session) != 0) {
        prefix = "/resume ";
        value = o->resume_id;
      }
      break;
    case CAI_CLI_STARTUP_MODEL:
      if (o->model_explicit) {
        prefix = "/model ";
        value = o->model;
      }
      break;
    case CAI_CLI_STARTUP_EFFORT:
      if (o->reasoning_effort_explicit) {
        prefix = "/reasoning ";
        value = o->reasoning_effort;
      }
      break;
    case CAI_CLI_STARTUP_SUMMARY:
      if (o->reasoning_summary_explicit) {
        prefix = "/reasoning-summary ";
        value = o->reasoning_summary;
      }
      break;
    case CAI_CLI_STARTUP_TIER:
      if (o->control_explicit & CAI_CLI_CONTROL_TIER) {
        prefix = "/tier ";
        value = o->service_tier;
      }
      break;
    case CAI_CLI_STARTUP_YIELD:
      if (o->control_explicit & CAI_CLI_CONTROL_YIELD) {
        prefix = "/yield_time_ms ";
        snprintf(number, sizeof(number), "%ld",
                 o->foreground ? 0L : o->yield_time_ms);
        value = number;
      }
      break;
    case CAI_CLI_STARTUP_YOLO:
      if (o->control_explicit & CAI_CLI_CONTROL_YOLO) {
        prefix = "/yolo ";
        value = o->yolo ? "on" : "off";
      }
      break;
    case CAI_CLI_STARTUP_REASONING_TOOL:
      if (o->control_explicit & CAI_CLI_CONTROL_REASONING_TOOL) {
        prefix = "/reasoning-tool ";
        value = o->reasoning_tool ? "on" : "off";
      }
      break;
    case CAI_CLI_STARTUP_SHELL_CONTEXT:
      if (o->control_explicit & CAI_CLI_CONTROL_SHELL_CONTEXT) {
        prefix = "/shell-context ";
        value = o->shell_context ? "on" : "off";
      }
      break;
    case CAI_CLI_STARTUP_NORMAL_EFFORT:
      if (o->control_explicit & CAI_CLI_CONTROL_NORMAL_EFFORT) {
        prefix = "/reasoning-tool normal ";
        value = o->normal_reasoning_effort;
      }
      break;
    case CAI_CLI_STARTUP_HIGH_EFFORT:
      if (o->control_explicit & CAI_CLI_CONTROL_HIGH_EFFORT) {
        prefix = "/reasoning-tool high ";
        value = o->high_reasoning_effort;
      }
      break;
    case CAI_CLI_STARTUP_HISTORY:
      if (o->control_explicit & CAI_CLI_CONTROL_HISTORY) {
        prefix = "/history ";
        snprintf(number, sizeof(number), "%d", o->prompt_history_limit);
        value = number;
      }
      break;
    case CAI_CLI_STARTUP_RETENTION:
      if (o->control_explicit & CAI_CLI_CONTROL_RETENTION) {
        prefix = "/retention ";
        snprintf(number, sizeof(number), "%ld", o->conversation_retention_days);
        value = number;
      }
      break;
    case CAI_CLI_STARTUP_AUTOQUEUE:
      if (o->control_explicit & CAI_CLI_CONTROL_AUTOQUEUE) {
        prefix = "/autoqueue ";
        value = o->enable_auto_enqueue ? "on" : "off";
      }
      break;
    case CAI_CLI_STARTUP_REVIEW_AND_FIX:
      if (o->review_and_fix) {
        prefix = o->review_uncommitted ? "/review-and-fix uncommitted"
                 : o->base != NULL     ? "/review-and-fix base "
                                       : "/review-and-fix";
        value = !o->review_uncommitted && o->base != NULL ? o->base : "";
      }
      break;
    case CAI_CLI_STARTUP_GOAL:
      if (o->goal != NULL) {
        const char *initial = cai_cli_instruction_at(o, 0U);
        if (initial != NULL) {
          int rc = cai_cli_goal_input_encode(o->goal, initial, text, error);
          if (rc == CAI_OK) {
            cursor->instruction = 1U;
            *control = 1;
          }
          return rc;
        }
        prefix = "/goal new ";
        value = o->goal;
      }
      break;
    case CAI_CLI_STARTUP_REVIEW:
      if (o->review) {
        value = cai_cli_instruction_at(o, 0U);
        prefix = value != NULL           ? "/review custom "
                 : o->base != NULL       ? "/review base "
                 : o->review_uncommitted ? "/review uncommitted"
                                         : "/review";
        if (value == NULL)
          value = o->base != NULL ? o->base : "";
        cursor->stage = CAI_CLI_STARTUP_DONE;
      }
      break;
    case CAI_CLI_STARTUP_INSTRUCTIONS:
      value = cai_cli_instruction_at(o, cursor->instruction);
      if (value != NULL) {
        ++cursor->instruction;
        cursor->stage = CAI_CLI_STARTUP_INSTRUCTIONS;
      }
      break;
    case CAI_CLI_STARTUP_DONE:
      break;
    }
  }
  if (value == NULL)
    return CAI_OK;
  a = prefix != NULL ? strlen(prefix) : 0U;
  b = strlen(value);
  if (a > CAI_CLI_INPUT_TEXT_MAX || b > CAI_CLI_INPUT_TEXT_MAX - a)
    return cai_cli_error(error, CAI_ERR_LIMIT,
                         "startup input exceeds input limit");
  *text = malloc(a + b + 1U);
  if (*text == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate startup input");
  if (a != 0U)
    memcpy(*text, prefix, a);
  memcpy(*text + a, value, b + 1U);
  *control = prefix != NULL;
  return CAI_OK;
}
