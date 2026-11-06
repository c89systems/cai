#ifndef CAI_CLI_EXECUTABLE_PATH_H
#define CAI_CLI_EXECUTABLE_PATH_H
#include <cai/cai.h>
/** Resolve this running executable for the standalone quiet-owner launcher.
 * Uses native image identity, never PATH or an unverified argv[0]. The caller
 * owns the bounded destination; failure leaves it empty. No host mutation. */
int cai_cli_executable_path(char *buffer, size_t capacity, cai_error *error);
#endif
