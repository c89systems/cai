typedef struct terminal_gate {
  int fd;
  int released;
} terminal_gate;
static void test_process_signal_scope_body(test_state *state) {
  cai_exec_tool_config exec;
  cai_terminal_tool_config terminal;
  cai_tool_registry *registry;
  cai_sink_callbacks callbacks;
  cai_sink *sink;
  cai_error error;
  write_state *writer = calloc(1U, sizeof(*writer));
  sigset_t blocked, original, seen;
  struct sigaction ignored, previous_pipe, pipe_action;
  int kind, tty, which;
  char expected[32];
  const char *arguments;
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGTERM);
  sigaddset(&blocked, SIGINT);
  sigaddset(&blocked, SIGHUP);
  sigaddset(&blocked, SIGQUIT);
  memset(&ignored, 0, sizeof(ignored));
  ignored.sa_handler = SIG_IGN;
  sigemptyset(&ignored.sa_mask);
  if (writer == NULL || pthread_sigmask(SIG_BLOCK, &blocked, &original) != 0) {
    test_fail(state, "process_signals_setup",
              "cannot allocate or block signals");
    free(writer);
    return;
  }
  if (sigaction(SIGPIPE, &ignored, &previous_pipe) != 0) {
    pthread_sigmask(SIG_SETMASK, &original, NULL);
    free(writer);
    test_fail(state, "process_signals_setup", "cannot set SIGPIPE disposition");
    return;
  }
  for (kind = 0; kind < 2; kind++) {
    memset(&exec, 0, sizeof(exec));
    memset(&terminal, 0, sizeof(terminal));
    memset(&callbacks, 0, sizeof(callbacks));
    exec.root_path = terminal.root_path = CAI_TEST_BINARY_DIR;
    exec.yolo = terminal.yolo = 1;
    exec.allow_pty = terminal.foreground = 1;
    registry = NULL;
    sink = NULL;
    cai_error_init(&error);
    callbacks.write = test_write;
    callbacks.context = writer;
    if (cai_tool_registry_new(&registry, &error) != CAI_OK ||
        (kind ? cai_tool_registry_register_terminal_tools(registry, &terminal,
                                                          &error)
              : cai_tool_registry_register_exec_tool(registry, &exec,
                                                     &error)) != CAI_OK ||
        cai_sink_from_callbacks(&callbacks, &sink, &error) != CAI_OK)
      test_fail(state, "process_signals_registry",
                "cannot register command tool");
    if (sink != NULL) {
      for (tty = 0; tty < 2; tty++)
        for (which = 0; which < 2; which++) {
          memset(writer, 0, sizeof(*writer));
          arguments =
              which
                  ? (tty ? "{\"cmd\":\"kill -s PIPE $$; exit 0\",\"tty\":true}"
                         : "{\"cmd\":\"kill -s PIPE $$; exit 0\"}")
                  : (tty ? "{\"cmd\":\"kill -s TERM $$; exit 0\",\"tty\":true}"
                         : "{\"cmd\":\"kill -s TERM $$; exit 0\"}");
          expect_int(state, "process_signals_run",
                     cai_tool_registry_run(registry,
                                           CAI_TERMINAL_EXEC_TOOL_NAME,
                                           arguments, sink, &error),
                     CAI_OK);
          snprintf(expected, sizeof(expected), "\"signal\":%d",
                   which ? SIGPIPE : SIGTERM);
          expect_substr(state, "process_signals_child_default", writer->buffer,
                        expected);
        }
    }
    cai_sink_close(sink);
    cai_tool_registry_destroy(registry);
    cai_error_cleanup(&error);
  }
  expect_int(state, "process_signals_parent_mask",
             pthread_sigmask(SIG_SETMASK, NULL, &seen), 0L);
  expect_int(state, "process_signals_parent_still_blocked",
             sigismember(&seen, SIGTERM), 1L);
  expect_int(state, "process_signals_parent_pipe",
             sigaction(SIGPIPE, NULL, &pipe_action), 0L);
  expect_int(state, "process_signals_parent_still_ignored",
             pipe_action.sa_handler == SIG_IGN, 1L);
  pthread_sigmask(SIG_SETMASK, &original, NULL);
  sigaction(SIGPIPE, &previous_pipe, NULL);
  free(writer);
}
#if defined(__linux__) && !defined(__GLIBC__)
extern int pthread_getattr_np(pthread_t, pthread_attr_t *);
#endif
static void *test_process_signal_worker(void *context) {
  test_state *state = context;
#if defined(__linux__) && !defined(__GLIBC__)
  pthread_attr_t attributes;
  size_t stack_size = 0U, default_size = 0U;
  int rc = pthread_attr_init(&attributes);
  expect_int(state, "process_signals_musl_default_attributes", rc, 0L);
  if (rc == 0) {
    expect_int(state, "process_signals_musl_default_stack",
               pthread_attr_getstacksize(&attributes, &default_size), 0L);
    expect_int(state, "process_signals_musl_default_stack_ceiling",
               default_size > 0U && default_size <= 128U * 1024U, 1L);
    pthread_attr_destroy(&attributes);
  }
  rc = pthread_getattr_np(pthread_self(), &attributes);
  expect_int(state, "process_signals_musl_worker_attributes", rc, 0L);
  if (rc == 0) {
    expect_int(state, "process_signals_musl_worker_stack",
               pthread_attr_getstacksize(&attributes, &stack_size), 0L);
    /* Inspect the configured default independently of libc's reported extent;
     * this worker was created with NULL attributes, without a stack override.
     */
    printf("musl default stack: %lu bytes; worker extent: %lu bytes\n",
           (unsigned long)default_size, (unsigned long)stack_size);
    pthread_attr_destroy(&attributes);
  }
#endif
  test_process_signal_scope_body(state);
  return NULL;
}
static void test_process_signal_scope(test_state *state) {
  pthread_t worker;
  int rc = pthread_create(&worker, NULL, test_process_signal_worker, state);
  expect_int(state, "process_signals_worker_start", rc, 0L);
  if (rc == 0)
    expect_int(state, "process_signals_worker_join", pthread_join(worker, NULL),
               0L);
}
static int test_terminal_release_output(void *context,
                                        const cai_terminal_event *event,
                                        cai_error *error) {
  terminal_gate *gate = context;
  (void)error;
  if (event->type == CAI_TERMINAL_EVENT_OUTPUT && event->output_length != 0U &&
      !gate->released) {
    gate->released = 1;
    return write(gate->fd, "go\n", 3U) == 3 ? CAI_OK : CAI_ERR_TRANSPORT;
  }
  return CAI_OK;
}
static void test_terminal_wait_policy(test_state *state) {
  char root[] = CAI_TEST_BINARY_DIR "/test-tmp/cai-terminal-wait-XXXXXX";
  char gate_path[PATH_MAX];
  cai_terminal_tool_config config;
  cai_tool_registry *registry;
  cai_sink_callbacks callbacks;
  cai_sink *sink;
  cai_error error;
  write_state writer;
  terminal_gate gate;
  int foreground;
  if (mkdtemp(root) == NULL) {
    test_fail(state, "terminal_wait_root", "mkdtemp failed");
    return;
  }
  snprintf(gate_path, sizeof(gate_path), "%s/gate", root);
  if (mkfifo(gate_path, 0600) != 0) {
    test_fail(state, "terminal_wait_fifo", "mkfifo failed");
    rmdir(root);
    return;
  }
  for (foreground = 0; foreground < 2; foreground++) {
    memset(&config, 0, sizeof(config));
    memset(&writer, 0, sizeof(writer));
    memset(&callbacks, 0, sizeof(callbacks));
    memset(&gate, 0, sizeof(gate));
    gate.fd = open(gate_path, O_RDWR | O_CLOEXEC);
    registry = NULL;
    sink = NULL;
    cai_error_init(&error);
    config.root_path = root;
    config.foreground = foreground;
    config.event_callback = test_terminal_release_output;
    config.event_context = &gate;
    callbacks.write = test_write;
    callbacks.context = &writer;
    expect_int(state, "terminal_wait_registry",
               cai_tool_registry_new(&registry, &error), CAI_OK);
    if (registry != NULL) {
      expect_int(
          state, "terminal_wait_register",
          cai_tool_registry_register_terminal_tools(registry, &config, &error),
          CAI_OK);
      expect_int(state, "terminal_wait_sink",
                 cai_sink_from_callbacks(&callbacks, &sink, &error), CAI_OK);
    }
    if (sink != NULL) {
      /* The UI releases the child after its first output. Returning to the
       * model on that output would omit LATE; foreground must also override
       * the model's explicit zero wait. No timing sleeps are needed. */
      expect_int(state, "terminal_wait_run",
                 cai_tool_registry_run(
                     registry, CAI_TERMINAL_EXEC_TOOL_NAME,
                     foreground ? "{\"cmd\":\"printf EARLY; read x < gate; "
                                  "printf LATE\",\"yield_time_ms\":0}"
                                : "{\"cmd\":\"printf EARLY; read x < gate; "
                                  "printf LATE\",\"yield_time_ms\":1000}",
                     sink, &error),
                 CAI_OK);
      expect_int(state, "terminal_wait_progress_before_return", gate.released,
                 1L);
      expect_substr(state, "terminal_wait_kept_first_output", writer.buffer,
                    "EARLYLATE");
      expect_substr(state, "terminal_wait_completed", writer.buffer,
                    "\"completed\":true");
    }
    cai_sink_close(sink);
    cai_tool_registry_destroy(registry);
    cai_error_cleanup(&error);
    close(gate.fd);
  }
  unlink(gate_path);
  rmdir(root);
}
static void test_terminal_process_modes(test_state *state) {
  char root[] = CAI_TEST_BINARY_DIR "/test-tmp/cai-terminal-modes-XXXXXX";
  const char *names[] = {"PAGER", "GIT_PAGER", "GH_PAGER"};
  char *saved[3];
  cai_terminal_tool_config config;
  cai_tool_registry *registry;
  cai_sink_callbacks callbacks;
  cai_sink *sink;
  cai_error error;
  write_state writer;
  int mode, yolo;
  size_t i;
  if (mkdtemp(root) == NULL) {
    test_fail(state, "terminal_modes_root", "mkdtemp failed");
    return;
  }
  for (i = 0U; i < 3U; i++) {
    const char *value = getenv(names[i]);
    saved[i] = value != NULL ? strdup(value) : NULL;
    setenv(names[i], "/must/not/run/pager", 1);
  }
  for (yolo = 0; yolo < 2; yolo++) {
    memset(&config, 0, sizeof(config));
    memset(&callbacks, 0, sizeof(callbacks));
    config.root_path = root;
    config.default_workdir = root;
    config.yolo = yolo;
    registry = NULL;
    sink = NULL;
    cai_error_init(&error);
    callbacks.write = test_write;
    callbacks.context = &writer;
    expect_int(state, "terminal_modes_registry",
               cai_tool_registry_new(&registry, &error), CAI_OK);
    if (registry != NULL) {
      expect_int(
          state, "terminal_modes_register",
          cai_tool_registry_register_terminal_tools(registry, &config, &error),
          CAI_OK);
      expect_int(state, "terminal_modes_sink",
                 cai_sink_from_callbacks(&callbacks, &sink, &error), CAI_OK);
    }
    if (sink != NULL) {
      for (mode = 0; mode < 2; mode++) {
        memset(&writer, 0, sizeof(writer));
        expect_int(
            state, "terminal_modes_run",
            cai_tool_registry_run(
                registry, CAI_TERMINAL_EXEC_TOOL_NAME,
                mode ? "{\"cmd\":\"for fd in 0 1 2; do test -t $fd && printf "
                       "TTY; done; printf '%s|%s|%s' $PAGER $GIT_PAGER "
                       "$GH_PAGER; printf stderr >&2\",\"tty\":true}"
                     : "{\"cmd\":\"for fd in 0 1 2; do test -t $fd || printf "
                       "PIPE; done; read x || printf EOF; printf '%s|%s|%s' "
                       "$PAGER $GIT_PAGER $GH_PAGER; printf stderr >&2\"}",
                sink, &error),
            CAI_OK);
        expect_substr(state, "terminal_modes_descriptors", writer.buffer,
                      mode ? "TTYTTYTTY" : "PIPEPIPEPIPEEOF");
        expect_substr(state, "terminal_modes_pagers", writer.buffer,
                      "cat|cat|cat");
        expect_substr(state, "terminal_modes_stderr", writer.buffer, "stderr");
        expect_substr(state, "terminal_modes_finished", writer.buffer,
                      "\"completed\":true");
      }
      memset(&writer, 0, sizeof(writer));
      expect_int(state, "terminal_modes_stale_handle",
                 cai_tool_registry_run(registry, CAI_TERMINAL_WRITE_TOOL_NAME,
                                       "{\"session_id\":\"terminal-1\"}", sink,
                                       &error),
                 CAI_ERR_INVALID);
    }
    cai_sink_close(sink);
    cai_tool_registry_destroy(registry);
    cai_error_cleanup(&error);
  }
  for (i = 0U; i < 3U; i++) {
    if (saved[i] != NULL) {
      setenv(names[i], saved[i], 1);
      free(saved[i]);
    } else
      unsetenv(names[i]);
  }
  rmdir(root);
}

/* A rejected second command must not steal output from the first handle. */
static void test_terminal_busy_output(test_state *state) {
  char root[] = CAI_TEST_BINARY_DIR "/test-tmp/cai-terminal-busy-XXXXXX";
  char path[PATH_MAX];
  cai_terminal_tool_config config;
  cai_tool_registry *registry = NULL;
  cai_sink_callbacks callbacks;
  cai_sink *sink = NULL;
  cai_error error;
  write_state writer;
  int gate = -1, ready = -1, initial_byte = 0;
  char signal;
  if (mkdtemp(root) == NULL) {
    test_fail(state, "terminal_busy_root", "mkdtemp failed");
    return;
  }
  snprintf(path, sizeof(path), "%s/gate", root);
  if (mkfifo(path, 0600) == 0)
    gate = open(path, O_RDWR | O_CLOEXEC);
  snprintf(path, sizeof(path), "%s/ready", root);
  if (mkfifo(path, 0600) == 0)
    ready = open(path, O_RDWR | O_CLOEXEC);
  memset(&config, 0, sizeof(config));
  memset(&callbacks, 0, sizeof(callbacks));
  memset(&writer, 0, sizeof(writer));
  config.root_path = root;
  callbacks.write = test_write;
  callbacks.context = &writer;
  cai_error_init(&error);
  if (gate < 0 || ready < 0 ||
      cai_tool_registry_new(&registry, &error) != CAI_OK ||
      cai_tool_registry_register_terminal_tools(registry, &config, &error) !=
          CAI_OK ||
      cai_sink_from_callbacks(&callbacks, &sink, &error) != CAI_OK) {
    test_fail(state, "terminal_busy_setup", "fixture initialization failed");
    goto cleanup;
  }
  expect_int(
      state, "terminal_busy_first",
      cai_tool_registry_run(registry, CAI_TERMINAL_EXEC_TOOL_NAME,
                            "{\"cmd\":\"printf PRESERVED; printf r > ready; "
                            "read x < gate; printf TAIL\",\"yield_time_ms\":1,"
                            "\"max_output_tokens\":1}",
                            sink, &error),
      CAI_OK);
  initial_byte = strstr(writer.buffer, "\"output\":\"P\"") != NULL;
  expect_int(state, "terminal_busy_child_ready", read(ready, &signal, 1U), 1L);
  memset(&writer, 0, sizeof(writer));
  expect_int(state, "terminal_busy_second",
             cai_tool_registry_run(registry, CAI_TERMINAL_EXEC_TOOL_NAME,
                                   "{\"cmd\":\"touch must-not-run\"}", sink,
                                   &error),
             CAI_OK);
  expect_substr(state, "terminal_busy_receipt", writer.buffer,
                "\"status\":\"busy\"");
  expect_substr(state, "terminal_busy_no_output", writer.buffer,
                "\"output\":\"\"");
  expect_int(state, "terminal_busy_release", write(gate, "go\n", 3U), 3L);
  memset(&writer, 0, sizeof(writer));
  expect_int(state, "terminal_busy_collect",
             cai_tool_registry_run(registry, CAI_TERMINAL_WRITE_TOOL_NAME,
                                   "{\"session_id\":\"terminal-1\"}", sink,
                                   &error),
             CAI_OK);
  expect_substr(state, "terminal_busy_preserved", writer.buffer,
                initial_byte ? "\"output\":\"RESERVEDTAIL\""
                             : "\"output\":\"PRESERVEDTAIL\"");
  snprintf(path, sizeof(path), "%s/must-not-run", root);
  expect_int(state, "terminal_busy_not_started", access(path, F_OK), -1L);
cleanup:
  cai_sink_close(sink);
  cai_tool_registry_destroy(registry);
  cai_error_cleanup(&error);
  if (gate >= 0)
    close(gate);
  if (ready >= 0)
    close(ready);
  snprintf(path, sizeof(path), "%s/gate", root);
  unlink(path);
  snprintf(path, sizeof(path), "%s/ready", root);
  unlink(path);
  snprintf(path, sizeof(path), "%s/must-not-run", root);
  unlink(path);
  rmdir(root);
}
