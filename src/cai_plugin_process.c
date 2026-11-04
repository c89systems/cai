#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "cai_internal.h"
#include "cai_plugins_internal.h"
#include "cai_process_environment.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif

typedef struct plugin_process {
  char **argv;
  size_t argc;
} plugin_process;

static void process_close(void *context) {
  plugin_process *process = context;
  size_t i;
  for (i = 0U; i < process->argc; i++)
    cai_free_mem(NULL, process->argv[i]);
  cai_free_mem(NULL, process->argv);
  cai_free_mem(NULL, process);
}
static double process_time(void) {
  struct timespec value;
  if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
    return -1.0;
  return (double)value.tv_sec + (double)value.tv_nsec / 1e9;
}
static int process_program(const char *name, char **environment,
                           const char *workspace, char path[4096],
                           cai_error *error) {
  const char *paths = "/usr/local/bin:/usr/bin:/bin", *end;
  char candidate[4096];
  size_t i, length;
  if (strchr(name, '/') != NULL) {
    if (name[0] == '/') {
      if (snprintf(candidate, sizeof(candidate), "%s", name) >=
          (int)sizeof(candidate))
        return cai_set_error(error, CAI_ERR_INVALID,
                             "plugin executable path too long");
    } else if (snprintf(candidate, sizeof(candidate), "%s/%s", workspace,
                        name) >= (int)sizeof(candidate))
      return cai_set_error(error, CAI_ERR_INVALID,
                           "plugin executable path too long");
    if (realpath(candidate, path) != NULL && access(path, X_OK) == 0)
      return CAI_OK;
  } else {
    for (i = 0U; environment[i] != NULL; i++)
      if (strncmp(environment[i], "PATH=", 5U) == 0)
        paths = environment[i] + 5U;
    do {
      end = strchr(paths, ':');
      length = end != NULL ? (size_t)(end - paths) : strlen(paths);
      if (length < 3000U) {
        if (length == 0U)
          snprintf(candidate, sizeof(candidate), "%s/%s", workspace, name);
        else if (paths[0] == '/')
          snprintf(candidate, sizeof(candidate), "%.*s/%s", (int)length, paths,
                   name);
        else
          snprintf(candidate, sizeof(candidate), "%s/%.*s/%s", workspace,
                   (int)length, paths, name);
        if (realpath(candidate, path) != NULL && access(path, X_OK) == 0)
          return CAI_OK;
      }
      paths = end != NULL ? end + 1U : NULL;
    } while (paths != NULL);
  }
  return cai_set_error_detail(error, CAI_ERR_INVALID,
                              "plugin executable is unavailable", name);
}
static void process_stop(pid_t pid) {
  if (kill(-pid, SIGKILL) != 0 && errno == ESRCH)
    (void)kill(pid, SIGKILL);
}
static int process_run(void *context, cai_source *arguments, cai_sink *output,
                       const cai_plugin_call *call, cai_error *error) {
  plugin_process *process = context;
  char **environment = NULL;
  struct process_buffers {
    char executable[4096], workspace[4096], diagnostics[2048];
    unsigned char input[4096], bytes[4096];
  } *buffers;
  size_t offset = 0U, available = 0U, written = 0U, diagnostic_length = 0U;
  size_t limit = call->max_output_bytes != 0U ? call->max_output_bytes
                                              : 4U * 1024U * 1024U;
  struct pollfd fds[3];
  struct rlimit descriptor_limit;
  pid_t pid, waited;
  int in[2] = {-1, -1}, out[2] = {-1, -1}, err[2] = {-1, -1};
  int workdir = -1, status = 0, reaped = 0, eof = 0, rc = CAI_OK, i, flags;
  long maxfd = 1024L;
  double deadline, now;
  ssize_t count;
  buffers = cai_alloc(NULL, sizeof(*buffers));
  if (buffers == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "allocate process buffers");
  if (call->workspace == NULL ||
      realpath(call->workspace, buffers->workspace) == NULL) {
    rc = cai_set_error(error, CAI_ERR_INVALID,
                       "plugin workspace is unavailable");
    goto cleanup;
  }
  if (call->cancelled != NULL && call->cancelled(call->cancel_context)) {
    rc = cai_set_error(error, CAI_ERR_CANCELLED, "plugin invocation cancelled");
    goto cleanup;
  }
  rc = cai_process_environment_new(call->environment, call->yolo, NULL,
                                   &environment, error);
  if (rc == CAI_OK)
    rc = process_program(process->argv[0], environment, buffers->workspace,
                         buffers->executable, error);
  if (rc != CAI_OK)
    goto cleanup;
  workdir = open(buffers->workspace, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (workdir < 0 || socketpair(AF_UNIX, SOCK_STREAM, 0, in) != 0 ||
      pipe(out) != 0 || pipe(err) != 0) {
    rc = cai_set_error(error, CAI_ERR_TRANSPORT, "open plugin pipes");
    goto cleanup;
  }
#ifdef SO_NOSIGPIPE
  {
    int enabled = 1;
    (void)setsockopt(in[0], SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                     sizeof(enabled));
  }
#endif
  for (i = 0; i < 2; i++)
    if (fcntl(in[i], F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(out[i], F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(err[i], F_SETFD, FD_CLOEXEC) != 0) {
      rc = cai_set_error(error, CAI_ERR_TRANSPORT, "configure plugin pipes");
      goto cleanup;
    }
  if (getrlimit(RLIMIT_NOFILE, &descriptor_limit) == 0 &&
      descriptor_limit.rlim_cur < (rlim_t)INT_MAX)
    maxfd = (long)descriptor_limit.rlim_cur;
  now = process_time();
  if (now < 0.0) {
    rc = cai_set_error(error, CAI_ERR_TRANSPORT, "read plugin clock");
    goto cleanup;
  }
  deadline =
      now +
      (double)(call->timeout_ms > 0L ? call->timeout_ms : 120000L) / 1000.0;
  pid = fork();
  if (pid < 0) {
    rc = cai_set_error(error, CAI_ERR_TRANSPORT, "fork plugin");
    goto cleanup;
  }
  if (pid == 0) {
    long fd;
    (void)setpgid(0, 0);
    if (dup2(in[1], STDIN_FILENO) < 0 || dup2(out[1], STDOUT_FILENO) < 0 ||
        dup2(err[1], STDERR_FILENO) < 0 || fchdir(workdir) != 0)
      _exit(126);
#if defined(__linux__) && defined(SYS_close_range)
    if (syscall(SYS_close_range, 3U, ~0U, 0U) != 0)
#endif
      for (fd = 3L; fd < maxfd; fd++)
        (void)close((int)fd);
    execve(buffers->executable, process->argv, environment);
    _exit(127);
  }
  (void)setpgid(pid, pid);
  close(in[1]);
  in[1] = -1;
  close(out[1]);
  out[1] = -1;
  close(err[1]);
  err[1] = -1;
  fds[0].fd = in[0];
  fds[1].fd = out[0];
  fds[2].fd = err[0];
  for (i = 0; i < 3; i++) {
    flags = fcntl(fds[i].fd, F_GETFL);
    if (flags < 0 || fcntl(fds[i].fd, F_SETFL, flags | O_NONBLOCK) != 0) {
      rc = cai_set_error(error, CAI_ERR_TRANSPORT, "configure plugin stream");
      break;
    }
  }
  while (rc == CAI_OK && (!reaped || fds[1].fd >= 0 || fds[2].fd >= 0)) {
    if (call->cancelled != NULL && call->cancelled(call->cancel_context)) {
      rc = cai_set_error(error, CAI_ERR_CANCELLED,
                         "plugin invocation cancelled");
      break;
    }
    now = process_time();
    if (now < 0.0 || now >= deadline) {
      rc = cai_set_error(error, CAI_ERR_LIMIT,
                         "plugin execution deadline exceeded");
      break;
    }
    if (fds[0].fd >= 0 && offset == available && !eof) {
      available = cai_source_read(arguments, buffers->input,
                                  sizeof(buffers->input), error);
      offset = 0U;
      if (error != NULL && error->code != CAI_OK) {
        rc = error->code;
        break;
      }
      eof = available == 0U;
    }
    if (fds[0].fd >= 0 && eof && offset == available) {
      (void)shutdown(fds[0].fd, SHUT_WR);
      fds[0].fd = -1;
    }
    fds[0].events = POLLOUT;
    fds[1].events = fds[2].events = POLLIN;
    if (poll(fds, 3U, 20) < 0) {
      if (errno == EINTR)
        continue;
      rc = cai_set_error(error, CAI_ERR_TRANSPORT, "poll plugin streams");
      break;
    }
    if (fds[0].fd >= 0 && fds[0].revents != 0 && offset < available) {
#ifdef MSG_NOSIGNAL
      count = send(fds[0].fd, buffers->input + offset, available - offset,
                   MSG_NOSIGNAL);
#else
      count = send(fds[0].fd, buffers->input + offset, available - offset, 0);
#endif
      if (count > 0)
        offset += (size_t)count;
      else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
               errno != EINTR)
        fds[0].fd = -1;
    }
    for (i = 1; rc == CAI_OK && i < 3; i++)
      if (fds[i].fd >= 0 && fds[i].revents != 0) {
        count = read(fds[i].fd, buffers->bytes, sizeof(buffers->bytes));
        if (count == 0)
          fds[i].fd = -1;
        else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                 errno != EINTR) {
          rc = cai_set_error(error, CAI_ERR_TRANSPORT, "read plugin stream");
          break;
        } else if (count > 0 && i == 1) {
          if ((size_t)count > limit - written) {
            rc = cai_set_error(error, CAI_ERR_LIMIT,
                               "plugin output exceeds configured limit");
            break;
          }
          rc = cai_sink_write(output, buffers->bytes, (size_t)count, error);
          written += (size_t)count;
        } else if (count > 0) {
          size_t room = sizeof(buffers->diagnostics) - 1U - diagnostic_length;
          size_t keep = (size_t)count < room ? (size_t)count : room;
          memcpy(buffers->diagnostics + diagnostic_length, buffers->bytes,
                 keep);
          diagnostic_length += keep;
        }
      }
    if (!reaped) {
      waited = waitpid(pid, &status, WNOHANG);
      if (waited == pid)
        reaped = 1;
      else if (waited < 0 && errno != EINTR) {
        rc = cai_set_error(error, CAI_ERR_TRANSPORT, "wait for plugin");
        break;
      }
    }
  }
  if (rc != CAI_OK)
    process_stop(pid);
  if (!reaped) {
    do {
      waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != pid && rc == CAI_OK)
      rc = cai_set_error(error, CAI_ERR_TRANSPORT, "reap plugin");
  }
  buffers->diagnostics[diagnostic_length] = '\0';
  if (rc == CAI_OK && (!WIFEXITED(status) || WEXITSTATUS(status) != 0))
    rc = cai_set_error_detail(error, CAI_ERR_INVALID, "plugin process failed",
                              buffers->diagnostics);
cleanup:
  for (i = 0; i < 2; i++) {
    if (in[i] >= 0)
      close(in[i]);
    if (out[i] >= 0)
      close(out[i]);
    if (err[i] >= 0)
      close(err[i]);
  }
  if (workdir >= 0)
    close(workdir);
  cai_process_environment_free(environment);
  cai_free_mem(NULL, buffers);
  return rc;
}
int cai_plugin_process_prepare(void *context,
                               const cai_plugin_definition *definition,
                               cai_plugin_handler *handler, cai_error *error) {
  plugin_process *process;
  char path[4096], resolved[4096];
  size_t i, directory_length;
  int has_entry;
  (void)context;
  has_entry =
      definition->entrypoint != NULL && definition->entrypoint[0] != '\0';
  if (!has_entry && definition->argc == 0U)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "process plugin needs argv or entrypoint");
  if (has_entry) {
    if (definition->entrypoint[0] == '/' ||
        snprintf(path, sizeof(path), "%s/%s", definition->directory,
                 definition->entrypoint) >= (int)sizeof(path) ||
        realpath(path, resolved) == NULL)
      return cai_set_error(error, CAI_ERR_INVALID,
                           "plugin entrypoint is unavailable");
    directory_length = strlen(definition->directory);
    if (strncmp(resolved, definition->directory, directory_length) != 0 ||
        (definition->directory[directory_length - 1U] != '/' &&
         resolved[directory_length] != '/'))
      return cai_set_error(error, CAI_ERR_INVALID,
                           "plugin entrypoint escapes package");
  }
  process = cai_alloc(NULL, sizeof(*process));
  if (process == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "allocate process plugin");
  memset(process, 0, sizeof(*process));
  process->argv = cai_alloc(NULL, (definition->argc + (size_t)has_entry + 1U) *
                                      sizeof(char *));
  if (process->argv == NULL) {
    process_close(process);
    return cai_set_error(error, CAI_ERR_NOMEM, "allocate plugin argv");
  }
  for (i = 0U; i < definition->argc + (size_t)has_entry; i++) {
    process->argv[i] =
        cai_strdup(NULL, i < definition->argc ? definition->argv[i] : resolved);
    if (process->argv[i] == NULL) {
      process_close(process);
      return cai_set_error(error, CAI_ERR_NOMEM, "copy plugin argv");
    }
    process->argc++;
  }
  process->argv[process->argc] = NULL;
  handler->context = process;
  handler->invoke = process_run;
  handler->destroy = process_close;
  return CAI_OK;
}
