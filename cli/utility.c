#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "utility.h"
#include "config.h"
#include "identity.h"
#include "instructions.h"
#include "log.h"
#include "login.h"
#include "messages.h"
#include "session_commands.h"
#include "skills.h"
#include "stores.h"
#include "theme.h"
#include <cai/chrome.h>
#include <cai/workspace.h>
#include <cai/workspace_client.h>
#include <cai/workspace_registry.h>
#include <errno.h>
#include <libmdf/mdf.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
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

static uint64_t control_time(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return 0U;
  return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/** EOF alone could also mean a rejected/stale control request. Confirm the
 * original endpoint was removed, without signalling or inspecting any PID. */
static int await_endpoint(cai_workspace_endpoint *target,
                          const struct stat *original, uint64_t until,
                          cai_error *error) {
  struct stat current;
  for (;;) {
    if (lstat(cai_workspace_endpoint_socket(target), &current) != 0) {
      if (errno == ENOENT)
        return CAI_OK;
      return cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "inspect stopped workspace endpoint");
    }
    if (current.st_dev != original->st_dev ||
        current.st_ino != original->st_ino)
      return CAI_OK;
    if (control_time() >= until)
      return cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "workspace endpoint did not close after shutdown");
    if (poll(NULL, 0U, 20) < 0 && errno != EINTR)
      return cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "wait for workspace endpoint cleanup");
  }
}

/** Administrative stop uses only the verified socket client. Taking control
 * is explicit in -X; no conversation, transcript or inference is opened. */
static int terminate_one(utility_run *u, const char *workspace,
                         const unsigned char *instance, int *stopped) {
  cai_workspace_client_config config;
  cai_workspace_client *client = NULL;
  cai_workspace_endpoint *target = NULL;
  cai_workspace_client_state state;
  struct stat original;
  struct pollfd interest;
  uint64_t deadline, now = control_time(), until = now + 30000U;
  int rc, stage = 0;
  *stopped = 0;
  cai_workspace_client_config_init(&config);
  config.runtime_directory = u->options.runtime_dir;
  config.workspace = workspace;
  config.expected_instance = instance;
  config.handshake_timeout_ms = 10000U;
  rc = cai_workspace_endpoint_open(config.runtime_directory, workspace, &target,
                                   &u->error);
  if (rc == CAI_OK)
    rc = cai_workspace_client_open(&config, now, &client, &u->error);
  while (rc == CAI_OK) {
    rc = cai_workspace_client_get_state(client, &state, &u->error);
    if (rc != CAI_OK)
      break;
    if (state.phase == CAI_WORKSPACE_DISCONNECTED) {
      if (stage == 2) {
        rc = await_endpoint(target, &original, until, &u->error);
        *stopped = rc == CAI_OK;
      } else
        rc = cai_cli_error(&u->error, CAI_ERR_TRANSPORT,
                           "workspace disconnected before shutdown was sent");
      break;
    }
    if (state.phase == CAI_WORKSPACE_CONNECTED && stage != 2) {
      if (stage == 0 &&
          lstat(cai_workspace_endpoint_socket(target), &original) != 0) {
        rc = cai_cli_error(&u->error, CAI_ERR_TRANSPORT,
                           "workspace endpoint disappeared before shutdown");
        break;
      }
      if (state.controller == state.attachment) {
        rc = cai_workspace_client_shutdown(client, 2U, state.generation,
                                           &u->error);
        stage = 2;
      } else if (stage == 0) {
        rc = cai_workspace_client_takeover(client, 1U, state.generation,
                                           &u->error);
        stage = 1;
      }
      if (rc != CAI_OK)
        break;
    }
    now = control_time();
    if (now >= until) {
      rc = cai_cli_error(&u->error, CAI_ERR_TRANSPORT,
                         "workspace server shutdown timed out");
      break;
    }
    rc = cai_workspace_client_prepare(client, &interest, &deadline, &u->error);
    if (rc != CAI_OK)
      break;
    if (!deadline || deadline > until)
      deadline = until;
    if (poll(&interest, 1U, deadline > now ? (int)(deadline - now) : 0) < 0 &&
        errno != EINTR) {
      rc = cai_cli_error(&u->error, CAI_ERR_TRANSPORT,
                         "wait for workspace shutdown");
      break;
    }
    rc = cai_workspace_client_pump(client, control_time(), &u->error);
  }
  cai_workspace_client_close(client);
  cai_workspace_endpoint_close(target);
  return rc;
}

static int terminate_servers(utility_run *u) {
  cai_workspace_registry *registry = NULL;
  const cai_workspace_record *record;
  cai_workspace_endpoint *endpoint = NULL;
  struct stat status;
  size_t count = 0U;
  int stopped, rc, failures = 0;
  u->operation = "terminate workspace server";
  if (!u->options.all) {
    rc = terminate_one(u, u->workspace, NULL, &stopped);
    if (rc == CAI_OK)
      puts("Workspace server stopped.");
    return rc;
  }
  if (lstat(u->options.runtime_dir, &status) != 0 && errno == ENOENT) {
    puts("Workspace servers stopped: 0.");
    return CAI_OK;
  }
  rc =
      cai_workspace_registry_open(u->options.runtime_dir, &registry, &u->error);
  while (rc == CAI_OK) {
    rc = cai_workspace_registry_next(registry, &record, &u->error);
    if (rc == CAI_ERR_INVALID || rc == CAI_ERR_PROTOCOL) {
      ++failures;
      cai_cli_log_error(u->log->logger, u->operation, &u->error);
      cai_error_cleanup(&u->error);
      cai_error_init(&u->error);
      rc = CAI_OK;
      continue;
    }
    if (rc != CAI_OK || record == NULL)
      break;
    /* Ignore vanished crash hints, but never signal a PID or unlink sockets.
     * Pin the instance so a replacement owner is not stopped by a stale hint.
     */
    rc = cai_workspace_endpoint_open(u->options.runtime_dir, record->workspace,
                                     &endpoint, &u->error);
    if (rc == CAI_OK &&
        lstat(cai_workspace_endpoint_socket(endpoint), &status) != 0 &&
        errno == ENOENT) {
      cai_workspace_endpoint_close(endpoint);
      endpoint = NULL;
      continue;
    }
    cai_workspace_endpoint_close(endpoint);
    endpoint = NULL;
    if (rc == CAI_OK)
      rc = terminate_one(u, record->workspace, record->instance, &stopped);
    if (rc == CAI_ERR_TRANSPORT &&
        cai_workspace_registry_prune(registry, NULL) == CAI_OK) {
      /* A crashed owner can leave both a hint and a refused socket. Pruning
       * requires its released lifetime lock and unchanged record inode. */
      cai_error_cleanup(&u->error);
      cai_error_init(&u->error);
      rc = CAI_OK;
      continue;
    }
    if (rc == CAI_OK && stopped)
      ++count;
    if (rc != CAI_OK) {
      ++failures;
      cai_cli_log_error(u->log->logger, u->operation, &u->error);
      cai_error_cleanup(&u->error);
      cai_error_init(&u->error);
      rc = CAI_OK;
    }
  }
  cai_workspace_registry_close(registry);
  if (rc == CAI_OK)
    printf("Workspace servers stopped: %lu.\n", (unsigned long)count);
  if (rc == CAI_OK && failures)
    rc = cai_cli_error(
        &u->error, CAI_ERR_TRANSPORT,
        "some workspace servers could not be stopped; see preceding errors");
  return rc;
}
static int dispatch(utility_run *u) {
  cai_cli_options selected;
  cai_sink *destination = NULL;
  char id[CAI_AGENT_SESSION_ID_MAX];
  const char *store;
  int rc;
  if (u->options.terminate_headless)
    return terminate_servers(u);
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
      rc = cai_cli_messages_send_once(
          &u->pouch, u->options.send_queue, u->options.message,
          u->options.message_delay_seconds, destination, &u->error);
    if (rc == CAI_OK)
      rc = cai_sink_write(destination, "\n", 1U, &u->error);
  } else if (u->options.list || u->options.list_all || u->options.resume_list ||
             u->options.delete_list) {
    u->operation = "list conversations";
    rc = cai_cli_pouch_list(
        &u->pouch,
        (u->options.list_all || u->options.all) ? NULL : u->workspace,
        &u->sessions, &u->session_count, &u->error);
    if (rc == CAI_OK && u->options.delete_list)
      rc = cai_cli_sessions_deletable(&u->pouch, u->sessions, &u->session_count,
                                      &u->error);
    if (rc == CAI_OK)
      rc = cai_sink_stdout(&destination, &u->error);
    if (rc == CAI_OK)
      rc = cai_cli_sessions_print(destination, u->sessions, u->session_count,
                                  !(u->options.list_all || u->options.all),
                                  mdf_terminal_width(STDOUT_FILENO, 80),
                                  &u->error);
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
                                  NULL, &u->options, destination, &u->error);
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
  if (u->options.dump_developer_instructions != NULL) {
    u->operation = "dump developer instructions";
    rc = cai_cli_dump_instructions(&u->options, &u->error);
    result = rc == CAI_OK ? 0 : 1;
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
