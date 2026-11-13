/* Standalone process scaffolding. Native embedding uses public lifecycle
 * APIs directly; examples/app.c remains the minimal public-only example. */
#include "attach.h"
#include "batch.h"
#include "feed_tcp.h"
#include "log.h"
#include "options.h"
#include "process.h"
#include <cai/app.h>
#include <pslog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/** Example-owned startup artwork; libcai_app only receives the selected text.
 */
static const char cai_banner_ansi[] = {
#include "cai_banner_ansi.inc"
};
static const char cai_banner_txt[] = {
#include "cai_banner_txt.inc"
};

int main(int argc, char **argv) {
  cai_app_config config;
  cai_app *app = NULL;
  cai_error error;
  cai_cli_options options;
  int code = 1;
  int diagnostic_presented = 0;
  int rc;
  rc = cai_cli_feed_tcp(argc, argv);
  if (rc >= 0)
    return rc;
  cai_app_config_init(&config);
  config.banner =
      isatty(STDOUT_FILENO) && getenv("NO_COLOR") == NULL &&
              (getenv("TERM") == NULL || strcmp(getenv("TERM"), "dumb") != 0)
          ? cai_banner_ansi
          : cai_banner_txt;
  config.introduction_margin_left = 0;
  config.introduction_margin_right = 0;
  config.introduction_markdown =
      "`cai` is an example of embedding `libcai_app`: "
      "<https://github.com/c89systems/cai/>.  \n"
      "**Enter** sends or queues; **Alt-Enter** steers. `/help` or `cai -h` "
      "for "
      "options.\n";
  cai_error_init(&error);
  /* Reuse the primary parser so a value such as -i '--headless' cannot
   * accidentally select a process mode. Help/version/dump remain utilities. */
  rc = cai_cli_read_options(argc, argv, NULL, &options);
  if (rc > 0 && options.server != NULL && !cai_cli_batch_mode(&options) &&
      (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))) {
    error.code = CAI_ERR_INVALID;
    error.message = strdup("--server requires an interactive terminal");
    rc = CAI_ERR_INVALID;
  } else if (rc > 0 && options.headless)
    rc = cai_cli_headless_run(&config, argc - 1,
                              (const char *const *)(argv + 1), &code, &error);
  else if (rc > 0 && (cai_cli_batch_mode(&options) ||
                      (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) &&
                       !options.non_interactive && !options.review &&
                       !cai_cli_options_utility(&options))))
    rc = cai_cli_attach_run(&config, argc, argv, &code, &diagnostic_presented,
                            &error);
  else {
    rc = cai_app_open(&config, &app, &error);
    if (rc == CAI_OK)
      rc = app->run(app, argc - 1, (const char *const *)(argv + 1), &code,
                    &error);
  }
  if (rc != CAI_OK && !diagnostic_presented) {
    cai_cli_log *log = NULL;
    if (cai_cli_log_new(&log, PSLOG_LEVEL_WARN, 0, NULL, NULL) == CAI_OK) {
      log->logger->error(log->logger,
                         error.message != NULL ? error.message
                                               : cai_status_string(rc),
                         NULL, 0U);
      (void)cai_cli_log_release(log);
    } else
      fputs("cai: cannot initialize application or diagnostic logger\n",
            stderr);
  }
  if (app != NULL)
    app->close(app);
  cai_error_cleanup(&error);
  return rc == CAI_OK ? code : 1;
}
