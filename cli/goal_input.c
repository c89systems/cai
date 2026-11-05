#include "goal_input.h"
#include "error.h"
#include "input_wire.h"
#include <lonejson.h>
#include <stdlib.h>
#include <string.h>

static const lonejson_field fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_cli_goal_input, objective, "objective"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_cli_goal_input, prompt, "prompt")};
LONEJSON_MAP_DEFINE(goal_input_map, cai_cli_goal_input, fields);

const char *cai_cli_goal_input_argument(const char *arg) {
  arg += strspn(arg, " \t\r\n");
  if (strncmp(arg, "start", 5U) != 0 ||
      (arg[5] != '\0' && strchr(" \t\r\n", arg[5]) == NULL))
    return NULL;
  arg += 5U;
  return arg + strspn(arg, " \t\r\n");
}

void cai_cli_goal_input_close(cai_cli_goal_input *input) {
  lonejson_cleanup(&goal_input_map, input);
  memset(input, 0, sizeof(*input));
}
int cai_cli_goal_input_parse(const char *text, cai_cli_goal_input *input,
                             cai_error *error) {
  lonejson_config config = lonejson_default_config();
  lonejson_error failure;
  lonejson *json;
  lonejson_status status;
  const char *p;
  memset(input, 0, sizeof(*input));
  if (text == NULL || strlen(text) > CAI_CLI_INPUT_TEXT_MAX)
    return cai_cli_error(error, CAI_ERR_LIMIT,
                         "goal start command exceeds limit");
  /* Mapped fields are C strings. Reject a genuine escaped NUL, while keeping
   * literal backslash-u text intact; JSON validation below owns other escapes.
   */
  for (p = text; *p != '\0'; ++p)
    if (*p == '\\' && p[1] != '\0') {
      if (strncmp(p + 1U, "u0000", 5U) == 0)
        return cai_cli_error(error, CAI_ERR_INVALID,
                             "goal start fields cannot contain NUL");
      ++p;
    }
  config.reject_duplicate_keys_by_default = 1;
  config.max_dynamic_string_bytes = CAI_CLI_INPUT_TEXT_MAX;
  config.max_alloc_bytes = 4U * CAI_CLI_INPUT_TEXT_MAX;
  lonejson_error_init(&failure);
  json = lonejson_new(&config, &failure);
  if (json == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "create goal start parser");
  status = lonejson_parse_buffer(json, &goal_input_map, input, text,
                                 strlen(text), &failure);
  lonejson_free(json);
  if (status != LONEJSON_STATUS_OK || input->objective == NULL ||
      input->objective[0] == '\0' || input->prompt == NULL ||
      input->prompt[0] == '\0')
    return cai_cli_error(
        error, CAI_ERR_INVALID,
        "Use /goal start {\"objective\":\"...\",\"prompt\":\"...\"}; both "
        "strings are required");
  return CAI_OK;
}
int cai_cli_goal_input_encode(const char *objective, const char *prompt,
                              char **out, cai_error *error) {
  static const char prefix[] = "/goal start ";
  cai_cli_goal_input value;
  lonejson_error failure;
  lonejson *json;
  lonejson_status status;
  size_t needed;
  *out = NULL;
  if (objective == NULL || prompt == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "goal start fields are required");
  memset(&value, 0, sizeof(value));
  /* Only the serializer view borrows these immutable strings. */
  memcpy(&value.objective, &objective, sizeof(objective));
  memcpy(&value.prompt, &prompt, sizeof(prompt));
  lonejson_error_init(&failure);
  json = lonejson_new(NULL, &failure);
  if (json == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "create goal start serializer");
  *out = malloc(CAI_CLI_INPUT_TEXT_MAX + 1U);
  if (*out == NULL) {
    lonejson_free(json);
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate bounded goal action");
  }
  memcpy(*out, prefix, sizeof(prefix) - 1U);
  status = lonejson_serialize_buffer(
      json, &goal_input_map, &value, *out + sizeof(prefix) - 1U,
      CAI_CLI_INPUT_TEXT_MAX + 1U - (sizeof(prefix) - 1U), &needed, &failure);
  lonejson_free(json);
  if (status != LONEJSON_STATUS_OK) {
    free(*out);
    *out = NULL;
    return cai_cli_error(error, CAI_ERR_LIMIT,
                         "goal start command exceeds input limit");
  }
  return CAI_OK;
}
