#define _POSIX_C_SOURCE 200809L
#include "error.h"
#include <stdlib.h>
#include <string.h>

int cai_cli_error(cai_error *error, int code, const char *message) {
  char *copy;
  if (error != NULL) {
    copy = strdup(message != NULL ? message : "lockd operation failed");
    cai_error_cleanup(error);
    error->code = code;
    error->message = copy;
  }
  return code;
}
