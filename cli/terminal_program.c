#define _GNU_SOURCE
#include "terminal_program.h"
#include "error.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#ifndef CLOSE_RANGE_CLOEXEC
#define CLOSE_RANGE_CLOEXEC (1U << 2)
#endif

/** Only one local foreground program exists. This transient waiter owns its
 * exact PID; the frontend never installs SIGCHLD handlers or scans processes.
 */
typedef struct terminal_program {
  int master, slave, epoll, completion[2], started[2];
  int thread_started, exited, raw_status, exec_errno, exec_ready;
  size_t drain;
  pid_t child;
  pthread_t waiter;
  char **arguments, **environment;
  const char *directory;
  long descriptor_limit;
  cai_cli_terminal_service_fn service;
  cai_cli_terminal_capture_fn capture;
  unsigned char capture_bytes[4096];
  size_t capture_length;
  void *context;
  cai_error *error;
} terminal_program;

static int program_error(cai_error *error, const char *operation, int code) {
  char message[256];
  snprintf(message, sizeof(message), "%s: %s (errno %d)", operation,
           strerror(code), code);
  return cai_cli_error(error, CAI_ERR_TRANSPORT, message);
}
static int private_fd(int fd) {
  if (fd >= 0 && fd < 3) {
    int moved = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    close(fd);
    return moved;
  }
  return fd;
}
static int descriptor(terminal_program *p, int fd) {
  struct epoll_event event;
  memset(&event, 0, sizeof(event));
  event.events = EPOLLIN | EPOLLET;
  if (fd == p->master)
    event.events |= EPOLLOUT;
  event.data.fd = fd;
  return epoll_ctl(p->epoll, EPOLL_CTL_ADD, fd, &event);
}
static void *wait_program(void *context) {
  terminal_program *p = context;
  siginfo_t info;
  int status = 0, result;
  memset(&info, 0, sizeof(info));
  /* Leave the child waitable: its PID cannot be reused while CLOSE may still
   * cancel it. Only the outer owner reaps, after joining this waiter. */
  do {
    result = waitid(P_PID, (id_t)p->child, &info, WEXITED | WNOWAIT);
  } while (result < 0 && errno == EINTR);
  if (result < 0)
    status = -errno;
  while (write(p->completion[1], &status, sizeof(status)) < 0 &&
         errno == EINTR) {
  }
  return NULL;
}
/** This path runs after fork in potentially threaded embedding hosts. Every
 * operation is async-signal-safe; all argv/environment storage already exists.
 */
static void child_program(terminal_program *p) {
  static const int signals[] = {SIGINT,  SIGQUIT, SIGTERM, SIGHUP,  SIGPIPE,
                                SIGCHLD, SIGTSTP, SIGTTIN, SIGTTOU, SIGWINCH};
  struct sigaction action;
  sigset_t mask;
  size_t i;
  int code;
  long fd;
  if (setsid() < 0 || ioctl(p->slave, TIOCSCTTY, 0) < 0 ||
      dup2(p->slave, 0) < 0 || dup2(p->slave, 1) < 0 || dup2(p->slave, 2) < 0 ||
      chdir(p->directory) != 0)
    goto fail;
  /* Exec must not inherit an embedding host's unrelated descriptors. */
#ifdef SYS_close_range
  if (syscall(SYS_close_range, 3U, ~0U, CLOSE_RANGE_CLOEXEC) != 0)
#endif
    for (fd = 3; fd < p->descriptor_limit; ++fd)
      (void)fcntl((int)fd, F_SETFD, FD_CLOEXEC);
  memset(&action, 0, sizeof(action));
  action.sa_handler = SIG_DFL;
  sigemptyset(&action.sa_mask);
  for (i = 0U; i < sizeof(signals) / sizeof(signals[0]); ++i)
    (void)sigaction(signals[i], &action, NULL);
  sigemptyset(&mask);
  (void)sigprocmask(SIG_SETMASK, &mask, NULL);
  execve(p->arguments[0], p->arguments, p->environment);
fail:
  code = errno;
  (void)write(p->started[1], &code, sizeof(code));
  _exit(127);
}
static int open_program(terminal_program *p, sl_terminal_event_t *e) {
  struct winsize geometry;
  char name[256];
  int pipes[2], rc;
  p->master =
      private_fd(posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK));
  if (p->master < 0 || grantpt(p->master) != 0 || unlockpt(p->master) != 0)
    return SL_ERROR_IO;
  rc = ptsname_r(p->master, name, sizeof(name));
  if (rc != 0) {
    errno = rc;
    return SL_ERROR_IO;
  }
  p->slave = private_fd(open(name, O_RDWR | O_NOCTTY | O_CLOEXEC));
  memset(&geometry, 0, sizeof(geometry));
  geometry.ws_row = (unsigned short)e->rows;
  geometry.ws_col = (unsigned short)e->columns;
  if (p->slave < 0 || ioctl(p->master, TIOCSWINSZ, &geometry) != 0)
    return SL_ERROR_IO;
  if (pipe2(pipes, O_CLOEXEC | O_NONBLOCK) != 0)
    return SL_ERROR_IO;
  p->completion[0] = private_fd(pipes[0]);
  p->completion[1] = private_fd(pipes[1]);
  if (pipe2(pipes, O_CLOEXEC | O_NONBLOCK) != 0)
    return SL_ERROR_IO;
  p->started[0] = private_fd(pipes[0]);
  p->started[1] = private_fd(pipes[1]);
  p->epoll = epoll_create1(EPOLL_CLOEXEC);
  if (p->completion[0] < 0 || p->completion[1] < 0 || p->started[0] < 0 ||
      p->started[1] < 0 || p->epoll < 0 || descriptor(p, p->master) != 0 ||
      descriptor(p, p->completion[0]) != 0 || descriptor(p, p->started[0]) != 0)
    return SL_ERROR_IO;
  p->child = fork();
  if (p->child == 0)
    child_program(p);
  if (p->child < 0)
    return SL_ERROR_IO;
  close(p->slave);
  close(p->started[1]);
  p->slave = p->started[1] = -1;
  rc = pthread_create(&p->waiter, NULL, wait_program, p);
  if (rc != 0) {
    errno = rc;
    return SL_ERROR_IO;
  }
  p->thread_started = 1;
  return SL_OK;
}
/** CLOSE never waits: Softline first restores the terminal, then the outer
 * owner joins the exact child waiter and releases its remaining resources. */
static void stop_program(terminal_program *p) {
  if (p->child > 0 && !p->exited) {
    pid_t foreground = tcgetpgrp(p->master);
    if (foreground > 0 && foreground != getpgrp())
      (void)kill(-foreground, SIGKILL);
    if (kill(-p->child, SIGKILL) != 0)
      (void)kill(p->child, SIGKILL);
  }
  if (p->master >= 0) {
    close(p->master);
    p->master = -1;
  }
}
static int relay(void *context, sl_terminal_event_t *e) {
  terminal_program *p = context;
  ssize_t n;
  switch (e->operation) {
  case SL_TERMINAL_OPEN:
    if (open_program(p, e) != SL_OK) {
      program_error(p->error, "open local terminal program", errno);
      return SL_ERROR_IO;
    }
    return SL_OK;
  case SL_TERMINAL_STEP: {
    struct epoll_event events[32];
    int timeout = -1;
    (void)epoll_wait(p->epoll, events, 32, 0);
    n = read(p->completion[0], &p->raw_status, sizeof(p->raw_status));
    if (n == (ssize_t)sizeof(p->raw_status) && !p->exited) {
      p->exited = 1;
      p->drain = 65536U;
    }
    if (!p->exec_ready) {
      n = read(p->started[0], &p->exec_errno, sizeof(p->exec_errno));
      if (n == 0 || n == (ssize_t)sizeof(p->exec_errno)) {
        p->exec_ready = 1;
        (void)epoll_ctl(p->epoll, EPOLL_CTL_DEL, p->started[0], NULL);
      }
    }
    if (p->service != NULL &&
        p->service(p->context, p->epoll, &timeout, p->error) != CAI_OK)
      return SL_ERROR_IO;
    e->fd = p->epoll;
    e->interests = SL_WATCH_READ;
    e->timeout_ms = timeout;
    e->ready = p->exec_ready;
    return SL_OK;
  }
  case SL_TERMINAL_READ:
    e->transferred = 0U;
    if (p->capture != NULL) {
      int accepted = 0;
      if (p->capture_length == 0U) {
        size_t capacity = e->length < sizeof(p->capture_bytes)
                              ? e->length
                              : sizeof(p->capture_bytes);
        n = read(p->master, p->capture_bytes, capacity);
        if (n <= 0)
          return n == 0 || errno == EIO ||
                         (p->exited &&
                          (errno == EAGAIN || errno == EWOULDBLOCK))
                     ? SL_OK
                 : errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK
                     ? SL_ERROR_NOT_READY
                     : SL_ERROR_IO;
        p->capture_length = (size_t)n;
      }
      if (p->capture(p->context, p->capture_bytes, p->capture_length, &accepted,
                     p->error) != CAI_OK)
        return SL_ERROR_IO;
      if (!accepted)
        return SL_ERROR_NOT_READY;
      if (p->capture_length > e->length)
        return SL_ERROR_INVALID;
      memcpy(e->data, p->capture_bytes, p->capture_length);
      e->transferred = p->capture_length;
      p->capture_length = 0U;
      return SL_OK;
    }
    if (p->exited && p->drain == 0U)
      return SL_OK;
    n = read(p->master, e->data,
             p->exited && e->length > p->drain ? p->drain : e->length);
    if (n > 0) {
      e->transferred = (size_t)n;
      if (p->exited)
        p->drain -= (size_t)n;
      return SL_OK;
    }
    if (n == 0 || errno == EIO ||
        (p->exited && (errno == EAGAIN || errno == EWOULDBLOCK)))
      return SL_OK;
    return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK
               ? SL_ERROR_NOT_READY
               : SL_ERROR_IO;
  case SL_TERMINAL_WRITE:
    if (p->exited)
      return SL_ERROR_NOT_READY;
    n = write(p->master, e->data, e->length);
    if (n > 0) {
      e->transferred = (size_t)n;
      return SL_OK;
    }
    return n < 0 && (errno == EINTR || errno == EAGAIN ||
                     errno == EWOULDBLOCK || errno == EIO)
               ? SL_ERROR_NOT_READY
               : SL_ERROR_IO;
  case SL_TERMINAL_RESIZE: {
    struct winsize size;
    memset(&size, 0, sizeof(size));
    size.ws_row = (unsigned short)e->rows;
    size.ws_col = (unsigned short)e->columns;
    return ioctl(p->master, TIOCSWINSZ, &size) == 0 ? SL_OK : SL_ERROR_IO;
  }
  case SL_TERMINAL_CLOSE:
    stop_program(p);
    return SL_OK;
  }
  return SL_ERROR_INVALID;
}
/** execve's signature predates const. Copy only the immutable pointer vector;
 * the synchronous caller retains its strings through child exec and join. */
static char **pointer_vector(const char *const *source) {
  char **result;
  size_t i, count = 0U;
  while (source[count] != NULL)
    ++count;
  if (count >= (size_t)-1 / sizeof(*result))
    return NULL;
  result = calloc(count + 1U, sizeof(*result));
  if (result != NULL)
    for (i = 0U; i < count; ++i)
      memcpy(&result[i], &source[i], sizeof(*result));
  return result;
}
int cai_cli_terminal_program(sl_t *sl, const char *const *argv,
                             const char *directory, cai_environment *env,
                             cai_cli_terminal_service_fn service,
                             cai_cli_terminal_capture_fn capture, void *context,
                             int *status, int *escaped, cai_error *error) {
  terminal_program *p;
  struct sigaction child_action;
  int rc, result, i;
  if (sl == NULL || argv == NULL || argv[0] == NULL || argv[0][0] == '\0' ||
      directory == NULL || directory[0] != '/' || env == NULL ||
      status == NULL || escaped == NULL)
    return cai_cli_error(
        error, CAI_ERR_INVALID,
        "terminal program requires argv, directory and environment");
  *status = 127 << 8;
  *escaped = 0;
  if (sigaction(SIGCHLD, NULL, &child_action) != 0 ||
      child_action.sa_handler == SIG_IGN ||
      (child_action.sa_flags & SA_NOCLDWAIT) != 0)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "local terminal programs require waitable children");
  p = calloc(1U, sizeof(*p));
  if (p == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate terminal program");
  p->master = p->slave = p->epoll = -1;
  p->raw_status = 127 << 8;
  p->completion[0] = p->completion[1] = p->started[0] = p->started[1] = -1;
  p->directory = directory;
  p->service = service;
  p->capture = capture;
  p->context = context;
  p->error = error;
  p->descriptor_limit = sysconf(_SC_OPEN_MAX);
  if (p->descriptor_limit < 0)
    p->descriptor_limit = 1024;
  p->arguments = pointer_vector(argv);
  p->environment = pointer_vector(cai_environment_entries(env));
  rc = p->arguments != NULL && p->environment != NULL
           ? CAI_OK
           : cai_cli_error(error, CAI_ERR_NOMEM,
                           "copy terminal program vectors");
  result = rc == CAI_OK ? sl_terminal_handoff(sl, relay, p) : SL_ERROR_NOMEM;
  stop_program(p);
  if (p->thread_started) {
    (void)pthread_join(p->waiter, NULL);
  }
  if (p->child > 0) {
    pid_t waited;
    do {
      waited = waitpid(p->child, &p->raw_status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0)
      p->raw_status = -errno;
  }
  if (rc == CAI_OK && result < 0) {
    const char *why = sl_last_error(sl);
    rc = error != NULL && error->code != CAI_OK
             ? error->code
             : cai_cli_error(error, CAI_ERR_TRANSPORT,
                             why != NULL ? why : "local terminal relay failed");
  }
  if (rc == CAI_OK && p->exec_errno != 0)
    rc = program_error(error, "execute local terminal program", p->exec_errno);
  if (rc == CAI_OK && p->raw_status < 0)
    rc =
        program_error(error, "wait for local terminal program", -p->raw_status);
  *status = p->raw_status;
  *escaped = result == SL_TERMINAL_ESCAPED;
  for (i = 0; i < 2; ++i) {
    if (p->completion[i] >= 0)
      close(p->completion[i]);
    if (p->started[i] >= 0)
      close(p->started[i]);
  }
  if (p->slave >= 0)
    close(p->slave);
  if (p->epoll >= 0)
    close(p->epoll);
  free(p->arguments);
  free(p->environment);
  free(p);
  return rc;
}
