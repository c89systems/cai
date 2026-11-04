/* Standalone process scaffolding. Native embedding uses public lifecycle
 * APIs directly; examples/app.c remains the minimal public-only example. */
#include "attach.h"
#include "options.h"
#include "process.h"
#include <cai/app.h>
#include <errno.h>
#include <limits.h>
#include <pslog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
  cai_app_config config;
  cai_app *app = NULL;
  cai_error error;
  cai_cli_options options;
  int code = 1;
  int rc;
  cai_app_config_init(&config);
  config.banner =
      isatty(STDOUT_FILENO) && getenv("NO_COLOR") == NULL &&
              (getenv("TERM") == NULL || strcmp(getenv("TERM"), "dumb") != 0)
          ? "\033[97mC\033[94mA\033[97mI\033[0m Smith (C) 2026 C89 Systems "
            "AB https://c89.systems\n\n"
          : "CAI Smith (C) 2026 C89 Systems AB https://c89.systems\n\n";
  config.introduction_margin_left = 0;
  config.introduction_margin_right = 0;
  config.introduction_markdown =
      "`cai` is an example of embedding `libcai_app`: "
      "<https://github.com/c89systems/cai/>.  \n"
      "**Enter** sends or queues; **Alt-Enter** steers. `/help` or `cai -h` "
      "for "
      "options.\n";
  cai_error_init(&error);
  /* Only the exec launcher uses this descriptor handoff. Values passed as
   * instructions or elsewhere in argv cannot select the private mode. */
  if (argc > 1 && strcmp(argv[1], "--_workspace-owner") == 0) {
    char *end = NULL;
    long descriptor;
    if (argc < 4 || strcmp(argv[3], "--") != 0 || argv[2][0] < '0' ||
        argv[2][0] > '9')
      return 2;
    errno = 0;
    descriptor = strtol(argv[2], &end, 10);
    if (errno || *end || descriptor < 3 || descriptor > INT_MAX)
      return 2;
    rc = cai_cli_owner_run(&config, (int)descriptor, argc - 4,
                           (const char *const *)(argv + 4), &code, &error);
    cai_error_cleanup(&error);
    return rc == CAI_OK ? code : 1;
  }
  /* Reuse the primary parser so a value such as -i '--headless' cannot
   * accidentally select a process mode. Help/version/dump remain utilities. */
  rc = cai_cli_read_options(argc, argv, NULL, &options);
  if (rc > 0 && options.server != NULL &&
      (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))) {
    error.code = CAI_ERR_INVALID;
    error.message = strdup("--server requires an interactive terminal");
    rc = CAI_ERR_INVALID;
  } else if (rc > 0 && options.headless)
    rc = cai_cli_headless_run(&config, argc - 1,
                              (const char *const *)(argv + 1), &code, &error);
  else if (rc > 0 && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) &&
           !options.non_interactive && !options.review && !options.temporary &&
           !options.list && !options.list_all && !options.resume_list &&
           !options.delete_list && options.delete_id == NULL &&
           options.export_id == NULL && options.import_file == NULL &&
           !options.login && !options.chrome_setup && !options.chrome_remove &&
           options.install_skill == NULL && options.message == NULL)
    rc = cai_cli_attach_run(&config, argc, argv, &code, &error);
  else {
    rc = cai_app_open(&config, &app, &error);
    if (rc == CAI_OK)
      rc = app->run(app, argc - 1, (const char *const *)(argv + 1), &code,
                    &error);
  }
  if (rc != CAI_OK) {
    pslog_config logging;
    pslog_logger *logger;
    pslog_default_config(&logging);
    logging.mode = PSLOG_MODE_JSON;
    logging.color = PSLOG_COLOR_AUTO;
    logging.min_level = PSLOG_LEVEL_WARN;
    logger = pslog_new_from_env("LOG_", &logging);
    if (logger != NULL) {
      logger->error(
          logger, error.message != NULL ? error.message : cai_status_string(rc),
          NULL, 0U);
      logger->destroy(logger);
    } else
      fputs("cai: cannot initialize application or diagnostic logger\n",
            stderr);
  }
  if (app != NULL)
    app->close(app);
  cai_error_cleanup(&error);
  return rc == CAI_OK ? code : 1;
}
