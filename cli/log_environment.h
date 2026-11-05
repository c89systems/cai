#ifndef CAI_CLI_LOG_ENVIRONMENT_H
#define CAI_CLI_LOG_ENVIRONMENT_H
#include <cai/environment.h>
#include <pslog.h>
/** Construct from an immutable invocation environment, using only public
 * libpslog configuration/output APIs. Relative file outputs use directory;
 * no getenv, chdir or environment installation. Borrow environment through
 * logger destruction. seed output must be unowned and remains borrowed. */
pslog_logger *cai_cli_logger_from_environment(const pslog_config *,
                                              const cai_environment *,
                                              const char *directory);
#endif
