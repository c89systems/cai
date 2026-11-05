#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "utility.h"
#include "config.h"
#include "identity.h"
#include "log.h"
#include "login.h"
#include "messages.h"
#include "session_commands.h"
#include "skills.h"
#include "stores.h"
#include <cai/chrome.h>
#include <libmdf/mdf.h>
#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/** Utility resources have the invocation's lifetime. Large path aggregates
 * stay on the heap, including when run() is called on a default musl worker.
 * No tool registry, runtime, journal observer or frontend lives here. */
typedef struct utility_run {
  cai_cli_options options;
  cai_config *configuration;
  cai_environment *environment;
  cai_cli_log *log;
  cai_cli_pouch pouch, messaging;
  cai_cli_session *sessions;
  size_t session_count;
  cai_error error;
  cai_app_bridge *bridge;
  char workspace[PATH_MAX], paths[3][PATH_MAX];
  const char *operation;
} utility_run;

static int native_host(int argc, char **argv) {
  return argc > 1 && (strncmp(argv[1], "chrome-extension://", 19U) == 0 ||
                      strcmp(argv[1], "--chrome-native-host") == 0);
}
int cai_cli_utility_invocation(int argc, char **argv) {
  cai_cli_options options;
  if (native_host(argc, argv))
    return 1;
  return cai_cli_read_options(argc, argv, NULL, &options) != 1 ||
         cai_cli_options_utility(&options);
}
static int dispatch(utility_run *u) {
  cai_cli_options selected;
  cai_sink *destination = NULL;
  char id[CAI_AGENT_SESSION_ID_MAX];
  const char *store;
  int rc;
  if (u->options.install_skill != NULL)
    return cai_cli_install_skill(&u->options, u->log->logger);
  if (u->options.chrome_setup || u->options.chrome_remove) {
    u->operation = "Chrome registration";
    return cai_chrome_native_host_setup_at(
        u->options.program_name, u->options.chrome_manifest,
        u->options.chrome_remove,
        u->configuration->get(u->configuration, "chrome", "socket_directory"),
        u->log->logger, &u->error);
  }
  store = u->options.login             ? u->options.auth_store
          : u->options.message != NULL ? u->options.messaging_store
                                       : u->options.sessions_store;
  u->operation = "open lockd store";
  rc = cai_cli_store_options(u->configuration, &u->options, store, &selected,
                             &u->error);
  if (rc == CAI_OK)
    rc = cai_cli_pouch_open_options(&u->pouch, &selected, u->log->logger,
                                    &u->error);
  if (rc != CAI_OK || cai_app_bridge_cleanup_stop(u->bridge))
    return rc;
  if (u->options.login) {
    u->operation = "login";
    return cai_cli_login(&u->pouch.credentials, u->log->logger, u->bridge->fd,
                         cai_app_bridge_cleanup_stop, u->bridge);
  }
  if (u->options.message != NULL) {
    u->operation = "send message";
    rc = cai_sink_stdout(&destination, &u->error);
    if (rc == CAI_OK)
      rc = cai_cli_messages_send_once(&u->pouch, u->options.send_queue,
                                      u->options.message, destination,
                                      &u->error);
    if (rc == CAI_OK)
      rc = cai_sink_write(destination, "\n", 1U, &u->error);
  } else if (u->options.list || u->options.list_all || u->options.resume_list ||
             u->options.delete_list) {
    u->operation = "list conversations";
    rc =
        cai_cli_pouch_list(&u->pouch, u->options.list_all ? NULL : u->workspace,
                           &u->sessions, &u->session_count, &u->error);
    if (rc == CAI_OK && u->options.delete_list)
      rc = cai_cli_sessions_deletable(&u->pouch, u->sessions, &u->session_count,
                                      &u->error);
    if (rc == CAI_OK)
      rc = cai_sink_stdout(&destination, &u->error);
    if (rc == CAI_OK)
      rc = cai_cli_sessions_print(
          destination, u->sessions, u->session_count, !u->options.list_all,
          mdf_terminal_width(STDOUT_FILENO, 80), &u->error);
  } else if (u->options.delete_id != NULL) {
    cai_cli_pouch *messaging = &u->pouch;
    u->operation = "delete conversation";
    /* Optional empty strings normalize to NULL. Resolve/validate before the
     * handle-name comparison, just as the agent owner's store cache does. */
    rc =
        cai_cli_store_options(u->configuration, &u->options,
                              u->options.messaging_store, &selected, &u->error);
    if (rc == CAI_OK &&
        strcmp(u->options.messaging_store, u->options.sessions_store) != 0) {
      rc = cai_cli_pouch_open_options(&u->messaging, &selected, u->log->logger,
                                      &u->error);
      messaging = &u->messaging;
    }
    if (rc == CAI_OK)
      rc = cai_cli_session_delete(&u->pouch, messaging, &u->options,
                                  u->workspace, u->options.delete_id, id,
                                  &u->error);
    if (rc == CAI_OK &&
        (fprintf(stdout, "Deleted conversation %s.\n", id) < 0 ||
         fflush(stdout) != 0))
      rc = cai_cli_error(&u->error, CAI_ERR_TRANSPORT,
                         "write conversation deletion result");
  } else if (u->options.export_id != NULL) {
    u->operation = "export conversation";
    rc = cai_sink_stdout(&destination, &u->error);
    if (rc == CAI_OK)
      rc = cai_cli_session_export(&u->pouch, u->options.export_id, u->workspace,
                                  u->options.export_dir, u->options.export_out,
                                  NULL, destination, &u->error);
  } else if (u->options.import_file != NULL) {
    u->operation = "import conversation";
    rc = cai_cli_pouch_import(&u->pouch, u->options.import_file, u->workspace,
                              id, sizeof(id), &u->error);
    if (rc == CAI_OK && puts(id) == EOF)
      rc = cai_cli_error(&u->error, CAI_ERR_TRANSPORT,
                         "write conversation import result");
  } else
    rc = cai_cli_error(&u->error, CAI_ERR_INVALID,
                       "maintenance utility required");
  cai_sink_close(destination);
  return rc;
}

int cai_cli_utility_run(int argc, char **argv,
                        const cai_app_config *application,
                        cai_app_bridge *bridge) {
  utility_run *u = calloc(1U, sizeof(*u));
  int parsed, result = 1, rc, raw = native_host(argc, argv);
  if (u == NULL) {
    fputs("cannot allocate utility state\n", stderr);
    return 1;
  }
  u->bridge = bridge;
  u->operation = "utility";
  cai_error_init(&u->error);
  if (raw) {
    int explicit_mode = strcmp(argv[1], "--chrome-native-host") == 0;
    /* Chrome owns stdout. This process-scoped override is restored by run's
     * wrapper; it must not become a library controller's logging policy. */
    (void)unsetenv("LOG_OUTPUT");
    rc = cai_cli_log_new(&u->log, PSLOG_LEVEL_WARN, 0, application->logger,
                         &u->error);
    u->operation = "Chrome native host";
    if (rc != CAI_OK)
      goto done;
    if (argc != (explicit_mode ? 3 : 2)) {
      cai_cli_error(&u->error, CAI_ERR_INVALID,
                    "Invalid Chrome native host arguments");
      goto done;
    }
    parsed = cai_cli_configure(1, argv, &u->options, &u->configuration,
                               application, &u->error);
  } else
    parsed = cai_cli_configure(argc, argv, &u->options, &u->configuration,
                               application, &u->error);
  if (parsed <= 0) {
    result = parsed == 0 ? 0 : 2;
    if (parsed < 0 && u->error.code == CAI_OK)
      cai_cli_error(&u->error, CAI_ERR_INVALID, u->options.diagnostic);
    goto done;
  }
  rc = u->configuration->apply_environment(u->configuration, &u->error);
  if (rc == CAI_OK)
    rc = cai_config_environment(u->configuration, &u->environment, &u->error);
  u->options.environment = u->environment;
  if (rc != CAI_OK)
    goto done;
  if (u->log == NULL &&
      cai_cli_log_new(&u->log, PSLOG_LEVEL_WARN, 0, application->logger,
                      &u->error) != CAI_OK)
    goto done;
  if (raw) {
    const char *directory =
        u->configuration->get(u->configuration, "chrome", "socket_directory");
    rc = cai_cli_private_directory(directory, &u->error);
    if (rc == CAI_OK)
      rc = cai_chrome_native_host_run_at(
          argv[strcmp(argv[1], "--chrome-native-host") == 0 ? 2 : 1], directory,
          u->log->logger, &u->error);
  } else {
    const char **paths[3];
    size_t i;
    (void)setlocale(LC_CTYPE, "");
    if (realpath(u->options.directory, u->workspace) == NULL) {
      cai_cli_error(&u->error, CAI_ERR_INVALID, "cannot resolve workspace");
      result = 2;
      goto done;
    }
    paths[0] = &u->options.import_file;
    paths[1] = &u->options.install_skill;
    paths[2] = &u->options.chrome_manifest;
    for (i = 0U; i < 3U; ++i) {
      if (*paths[i] == NULL)
        continue;
      if (cai_cli_workspace_path(u->paths[i], PATH_MAX, u->workspace, *paths[i],
                                 &u->error) != CAI_OK)
        goto done;
      *paths[i] = u->paths[i];
    }
    rc = dispatch(u);
  }
  result = rc == CAI_OK ? 0 : 1;
done:
  (void)cai_app_bridge_cleanup_stop(bridge);
  if (u->error.code != CAI_OK) {
    if (u->log == NULL)
      (void)cai_cli_log_new(&u->log, PSLOG_LEVEL_WARN, 0, application->logger,
                            &u->error);
    if (u->log != NULL)
      cai_cli_log_error(u->log->logger, u->operation, &u->error);
    else
      fprintf(stderr, "%s: %s\n", u->operation,
              u->error.message != NULL ? u->error.message : "utility failed");
    cai_app_bridge_error(bridge, &u->error);
  }
  cai_cli_pouch_close(&u->messaging);
  cai_cli_pouch_close(&u->pouch);
  free(u->sessions);
  cai_config_close(u->configuration);
  cai_environment_close(u->environment);
  cai_error_cleanup(&u->error);
  if (cai_cli_log_release(u->log) != 0)
    result = 1;
  free(u);
  return result;
}
