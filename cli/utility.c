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
#include "theme.h"
#include <cai/chrome.h>
#include <libmdf/mdf.h>
#include <limits.h>
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
  cai_config_context startup;
  cai_cli_log *log;
  cai_cli_pouch pouch, messaging;
  cai_cli_session *sessions;
  size_t session_count;
  cai_error error;
  cai_app_bridge *bridge;
  char workspace[PATH_MAX], paths[3][PATH_MAX];
  const char *operation;
} utility_run;

/** Native messaging reserves stdout for frames, including on configuration
 * failure. Mask only this logger's output override, never the host's env. */
static int utility_log(utility_run *u, const cai_app_config *application,
                       int raw) {
  cai_environment *environment =
      u->environment != NULL ? u->environment : u->startup.environment;
  cai_environment *masked = NULL;
  cai_environment_change change;
  int rc = CAI_OK;
  if (raw) {
    change.name = "LOG_OUTPUT";
    change.value = NULL;
    rc = cai_environment_derive(environment, &change, 1U, &masked, &u->error);
    environment = masked;
  }
  if (rc == CAI_OK)
    rc = cai_cli_log_new_at(&u->log, PSLOG_LEVEL_WARN, 0, application->logger,
                            environment, u->workspace, &u->error);
  cai_environment_close(masked);
  return rc;
}

/** Resolve the core Chrome helper's default path from the invocation view.
 * Supplying it explicitly keeps registration independent of process env. */
static int chrome_manifest_path(utility_run *u) {
  const char *root = cai_environment_get(u->environment, "CHROME_CONFIG_HOME");
  const char *suffix = "/google-chrome/NativeMessagingHosts/"
                       "com.openai.codexextension.json";
  int n;
  if (u->options.chrome_manifest != NULL)
    return CAI_OK;
  if (root == NULL || root[0] == '\0')
    root = cai_environment_get(u->environment, "XDG_CONFIG_HOME");
  if (root == NULL || root[0] == '\0') {
    root = cai_environment_get(u->environment, "HOME");
    suffix = "/.config/google-chrome/NativeMessagingHosts/"
             "com.openai.codexextension.json";
  }
  if (root == NULL || root[0] != '/')
    return cai_cli_error(&u->error, CAI_ERR_INVALID,
                         "Chrome manifest requires an absolute config home");
  n = snprintf(u->paths[2], sizeof(u->paths[2]), "%s%s", root, suffix);
  if (n < 0 || (size_t)n >= sizeof(u->paths[2]))
    return cai_cli_error(&u->error, CAI_ERR_INVALID,
                         "Chrome manifest path is too long");
  u->options.chrome_manifest = u->paths[2];
  return CAI_OK;
}

static int native_host(int argc, char **argv) {
  return argc > 1 && (strncmp(argv[1], "chrome-extension://", 19U) == 0 ||
                      strcmp(argv[1], "--chrome-native-host") == 0);
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
    rc = chrome_manifest_path(u);
    if (rc != CAI_OK)
      return rc;
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
                         cai_app_bridge_cleanup_stop, u->bridge, u->environment,
                         u->workspace);
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
  if (getcwd(u->workspace, sizeof(u->workspace)) == NULL) {
    cai_cli_error(&u->error, CAI_ERR_TRANSPORT,
                  "capture utility working directory");
    goto done;
  }
  u->startup.directory = u->workspace;
  if (cai_environment_open(NULL, &u->startup.environment, &u->error) != CAI_OK)
    goto done;
  if (raw) {
    int explicit_mode = strcmp(argv[1], "--chrome-native-host") == 0;
    u->operation = "Chrome native host";
    if (argc != (explicit_mode ? 3 : 2)) {
      cai_cli_error(&u->error, CAI_ERR_INVALID,
                    "Invalid Chrome native host arguments");
      goto done;
    }
    parsed = cai_cli_configure_at(1, argv, &u->options, &u->configuration,
                                  application, &u->startup, &u->error);
  } else
    parsed = cai_cli_configure_at(argc, argv, &u->options, &u->configuration,
                                  application, &u->startup, &u->error);
  if (parsed <= 0) {
    result = parsed == 0 ? 0 : 2;
    if (parsed == 0 && u->options.list_themes) {
      cai_sink *sink = NULL;
      rc = cai_sink_stdout(&sink, &u->error);
      if (rc == CAI_OK)
        rc = cai_cli_theme_list(sink, &u->error);
      cai_sink_close(sink);
      if (rc != CAI_OK)
        result = 1;
    }
    if (parsed < 0 && u->error.code == CAI_OK)
      cai_cli_error(&u->error, CAI_ERR_INVALID, u->options.diagnostic);
    goto done;
  }
  if (cai_cli_theme_validate(&u->options, &u->error) != CAI_OK) {
    result = 2;
    goto done;
  }
  if (u->options.headless && !cai_cli_options_utility(&u->options)) {
    cai_cli_error(&u->error, CAI_ERR_INVALID,
                  "--headless is an executable process mode; native hosts "
                  "must use cai_app_start_server");
    result = 2;
    goto done;
  }
  rc = cai_config_environment(u->configuration, &u->environment, &u->error);
  u->options.environment = u->environment;
  if (rc != CAI_OK)
    goto done;
  if (!raw && realpath(u->options.directory, u->workspace) == NULL) {
    cai_cli_error(&u->error, CAI_ERR_INVALID, "cannot resolve workspace");
    result = 2;
    goto done;
  }
  if (utility_log(u, application, raw) != CAI_OK)
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
    if (u->log == NULL && u->startup.environment != NULL)
      (void)utility_log(u, application, raw);
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
  cai_environment_close(u->startup.environment);
  cai_error_cleanup(&u->error);
  if (cai_cli_log_release(u->log) != 0)
    result = 1;
  free(u);
  return result;
}
