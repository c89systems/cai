/** @file A synchronous local utility handoff with interruptible child waiting.
 * Process-wide environment, cwd and signal dispositions remain host-owned. */
#define _GNU_SOURCE
#include "editor.h"
#include "error.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct editor_wait {
  pid_t child;
  int fd, status, failed;
} editor_wait;

static void *wait_editor(void *context) {
  editor_wait *waiter = context;
  pid_t result;
  char byte = 1;
  do {
    result = waitpid(waiter->child, &waiter->status, 0);
  } while (result < 0 && errno == EINTR);
  waiter->failed = result < 0;
  while (write(waiter->fd, &byte, 1U) < 0 && errno == EINTR) {
  }
  return NULL;
}

int cai_cli_editor_run(cai_environment *environment, const char *const *files,
                       size_t count, int stop_fd, int (*stop)(void *),
                       void *context, cai_error *error) {
  const char *editor;
  char **args = NULL, *command = NULL;
  char executable[] = "/bin/sh", option[] = "-c", identity[] = "cai-editor";
  const char *const *entries;
  char *const *child_environment;
  posix_spawnattr_t attributes;
  editor_wait waiter;
  pthread_t thread;
  sigset_t empty, defaults, blocked, previous;
  struct pollfd fds[2];
  pid_t foreground = -1;
  int pipefd[2] = {-1, -1}, rc = CAI_OK, initialized = 0, launched = 0;
  int mask_saved = 0, child_status, polled;
  size_t i, length;
  memset(&waiter, 0, sizeof(waiter));
  if (count == 0U || count > 16U || files == NULL || environment == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "editor requires filenames and environment");
  editor = cai_environment_get(environment, "VISUAL");
  entries = cai_environment_entries(environment);
  if (editor == NULL || *editor == '\0')
    editor = cai_environment_get(environment, "EDITOR");
  if (editor == NULL || *editor == '\0')
    editor = "vi";
  length = strlen(editor);
  if (length > 65536U)
    return cai_cli_error(error, CAI_ERR_LIMIT, "editor command exceeds 64 KiB");
  args = calloc(count + 5U, sizeof(*args));
  command = malloc(length + 32U);
  if (args == NULL || command == NULL) {
    rc = cai_cli_error(error, CAI_ERR_NOMEM, "allocate editor invocation");
    goto done;
  }
  strcpy(command, "exec ");
  strcat(command, editor);
  strcat(command, " \"$@\"");
  args[0] = executable;
  args[1] = option;
  args[2] = command;
  args[3] = identity;
  for (i = 0U; i < count; ++i) {
    if (files[i] == NULL || files[i][0] != '/') {
      rc = cai_cli_error(error, CAI_ERR_INVALID,
                         "editor filenames must be absolute");
      goto done;
    }
    args[4U + i] = strdup(files[i]);
    if (args[4U + i] == NULL) {
      rc = cai_cli_error(error, CAI_ERR_NOMEM, "copy editor filename");
      goto done;
    }
  }
  if (stop != NULL && stop(context)) {
    rc = cai_cli_error(error, CAI_ERR_CANCELLED, "agent edit cancelled");
    goto done;
  }
  if (pipe2(pipefd, O_CLOEXEC) != 0 || posix_spawnattr_init(&attributes) != 0) {
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "initialize editor process");
    goto done;
  }
  initialized = 1;
  sigemptyset(&empty);
  sigemptyset(&defaults);
  sigaddset(&defaults, SIGINT);
  sigaddset(&defaults, SIGQUIT);
  sigaddset(&defaults, SIGTERM);
  sigaddset(&defaults, SIGHUP);
  sigaddset(&defaults, SIGPIPE);
  sigaddset(&defaults, SIGTSTP);
  sigaddset(&defaults, SIGTTIN);
  sigaddset(&defaults, SIGTTOU);
  if (posix_spawnattr_setsigmask(&attributes, &empty) != 0 ||
      posix_spawnattr_setsigdefault(&attributes, &defaults) != 0 ||
      posix_spawnattr_setpgroup(&attributes, 0) != 0 ||
      posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK |
                                                POSIX_SPAWN_SETSIGDEF |
                                                POSIX_SPAWN_SETPGROUP) != 0) {
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "configure editor signals");
    goto done;
  }
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGTTOU);
  if (pthread_sigmask(SIG_BLOCK, &blocked, &previous) != 0) {
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                       "protect editor terminal handoff");
    goto done;
  }
  mask_saved = 1;
  foreground = tcgetpgrp(STDIN_FILENO);
  /* POSIX spawn borrows but does not modify the environment vector. Adapt its
   * historical mutable declaration without changing the captured host view. */
  memcpy(&child_environment, &entries, sizeof(child_environment));
  child_status = posix_spawn(&waiter.child, "/bin/sh", NULL, &attributes, args,
                             child_environment);
  if (child_status != 0) {
    waiter.child = 0;
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "start configured editor");
    goto done;
  }
  if (foreground >= 0 && tcsetpgrp(STDIN_FILENO, waiter.child) != 0) {
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "give terminal to editor");
    goto done;
  }
  if (foreground >= 0)
    (void)kill(-waiter.child, SIGCONT);
  waiter.fd = pipefd[1];
  if (pthread_create(&thread, NULL, wait_editor, &waiter) != 0) {
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "start editor waiter");
    goto done;
  }
  launched = 1;
  memset(fds, 0, sizeof(fds));
  fds[0].fd = pipefd[0];
  fds[1].fd = stop_fd;
  fds[0].events = fds[1].events = POLLIN;
  for (;;) {
    if (stop != NULL && stop(context)) {
      rc = cai_cli_error(error, CAI_ERR_CANCELLED, "agent edit cancelled");
      break;
    }
    polled = poll(fds, 2U, -1);
    if (polled < 0 && errno == EINTR)
      continue;
    if (polled < 0) {
      rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "wait for editor");
      break;
    }
    if (fds[0].revents != 0)
      break;
    if (fds[1].revents != 0 && (stop == NULL || !stop(context))) {
      if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
        rc =
            cai_cli_error(error, CAI_ERR_CANCELLED, "editor host disconnected");
        break;
      }
    }
  }
done:
  if (waiter.child > 0) {
    if (rc != CAI_OK)
      (void)kill(-waiter.child, SIGKILL);
    if (launched)
      pthread_join(thread, NULL);
    else {
      while (waitpid(waiter.child, &waiter.status, 0) < 0 && errno == EINTR) {
      }
    }
    if (rc == CAI_OK && (waiter.failed || !WIFEXITED(waiter.status) ||
                         WEXITSTATUS(waiter.status) != 0))
      rc = cai_cli_error(error, CAI_ERR_CANCELLED,
                         "editor exited without saving the agent");
  }
  if (foreground >= 0)
    (void)tcsetpgrp(STDIN_FILENO, foreground);
  if (mask_saved)
    (void)pthread_sigmask(SIG_SETMASK, &previous, NULL);
  if (initialized)
    posix_spawnattr_destroy(&attributes);
  if (pipefd[0] >= 0)
    close(pipefd[0]);
  if (pipefd[1] >= 0)
    close(pipefd[1]);
  free(command);
  if (args != NULL)
    for (i = 0U; i < count; ++i)
      free(args[4U + i]);
  free(args);
  return rc;
}
