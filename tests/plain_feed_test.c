#define _POSIX_C_SOURCE 200809L
#include "../cli/plain_feed.h"
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static void record(cai_cli_plain_feed *feed, const char *tool, const char *kind,
                   const char *field, const char *value, size_t fragment) {
  cai_execution_record *r = calloc(1U, sizeof(*r));
  cai_app_execution_event event;
  size_t at = 0U, length = strlen(value);
  assert(r != NULL);
  memset(&event, 0, sizeof(event));
  r->session_id = "plain-fixture";
  r->call_id = tool;
  r->tool_name = tool;
  r->kind = kind;
  r->field = field;
  event.record = r;
  do {
    size_t n = length - at;
    if (n > fragment)
      n = fragment;
    assert(n < sizeof(r->data));
    memcpy(r->data, value + at, n);
    r->data[n] = '\0';
    r->offset = at;
    at += n;
    r->final = at == length;
    assert(cai_cli_plain_feed_execution(feed, &event, NULL) == CAI_OK);
  } while (at != length);
  free(r);
}
static void render(const char *directory, size_t fragment, char out[4096]) {
  char *path = malloc(strlen(directory) + 32U);
  cai_cli_options options;
  cai_cli_plain_feed *feed = NULL;
  cai_transcript_event event;
  int fd, original;
  ssize_t n;
  assert(path != NULL);
  sprintf(path, "%s/plain-feed-XXXXXX", directory);
  fd = mkstemp(path);
  assert(fd >= 0 && unlink(path) == 0);
  assert(fflush(stdout) == 0);
  original = dup(STDOUT_FILENO);
  assert(original >= 0 && dup2(fd, STDOUT_FILENO) == STDOUT_FILENO);
  cai_cli_options_init(&options);
  options.tool_output = "full";
  assert(cai_cli_plain_feed_open(&options, &feed, NULL) == CAI_OK);
  memset(&event, 0, sizeof(event));
  event.kind = CAI_TRANSCRIPT_REPLAY_BEGIN;
  event.stream = 1U;
  assert(cai_cli_plain_feed_transcript(feed, &event, NULL) == CAI_OK);
  record(feed, "custom_tool", "tool.started", NULL, "", fragment);
  record(feed, "custom_tool", "tool.arguments", "data", "{}", fragment);
  record(feed, "custom_tool", "tool.output", "data",
         "UNBROKENWORDACROSSCHUNKS.", fragment);
  record(feed, "custom_tool", "tool.completed", NULL, "", fragment);
  /* This Markdown word remains buffered until the raw command transition.
   * It must precede child bytes even if no Running receipt was printed. */
  event.kind = CAI_TRANSCRIPT_EVENT_BEGIN;
  event.sequence = 1U;
  event.type = "assistant_text_delta";
  event.has_data = 1;
  assert(cai_cli_plain_feed_transcript(feed, &event, NULL) == CAI_OK);
  event.kind = CAI_TRANSCRIPT_DATA;
  event.data = "BEFORECOMMAND";
  event.length = strlen(event.data);
  assert(cai_cli_plain_feed_transcript(feed, &event, NULL) == CAI_OK);
  event.kind = CAI_TRANSCRIPT_EVENT_END;
  event.offset = event.length;
  event.length = 0U;
  event.data = NULL;
  assert(cai_cli_plain_feed_transcript(feed, &event, NULL) == CAI_OK);
  record(feed, "exec_command", "tool.started", NULL, "", fragment);
  record(feed, "exec_command", "tool.arguments", "data",
         "{\"cmd\":\"printf COMMAND_BYTES\"}", fragment);
  event.kind = CAI_TRANSCRIPT_TERMINAL_OUTPUT;
  event.session = "plain-fixture";
  event.call_id = "exec_command";
  event.terminal_id = "terminal-1";
  event.sequence = 7U;
  event.offset = 0U;
  event.data = "LIVE_PROGRESS\n";
  event.length = strlen(event.data);
  assert(cai_cli_plain_feed_transcript(feed, &event, NULL) == CAI_OK);
  /* Progress is flushed before the authoritative result exists. */
  n = pread(fd, out, 4095U, 0);
  assert(n > 0 && n < 4095);
  out[n] = '\0';
  assert(strstr(out, "LIVE_PROGRESS\n") != NULL);
  record(feed, "exec_command", "tool.output", "data",
         "{\"session_id\":\"terminal-1\",\"command_id\":7,\"output_offset\":0,"
         "\"output\":\"LIVE_PROGRESS\\nCOMMAND_BYTES\",\"exit_code\":0,"
         "\"duration_ms\":1}",
         fragment);
  record(feed, "exec_command", "tool.completed", NULL, "", fragment);
  assert(cai_cli_plain_feed_finish(feed, NULL) == CAI_OK);
  cai_cli_plain_feed_close(feed);
  assert(fflush(stdout) == 0);
  assert(dup2(original, STDOUT_FILENO) == STDOUT_FILENO);
  close(original);
  assert(lseek(fd, 0, SEEK_SET) == 0);
  n = read(fd, out, 4095U);
  assert(n > 0 && n < 4095);
  out[n] = '\0';
  assert(strstr(out, "UNBROKENWORDACROSSCHUNKS.") != NULL);
  assert(strstr(out, "BEFORECOMMAND") != NULL &&
         strstr(out, "COMMAND_BYTES") != NULL);
  assert(strstr(out, "BEFORECOMMAND") < strstr(out, "COMMAND_BYTES"));
  assert(strstr(strstr(out, "LIVE_PROGRESS\n") + strlen("LIVE_PROGRESS\n"),
                "LIVE_PROGRESS\n") == NULL);
  assert(strstr(out, "Ran printf COMMAND_BYTES (1ms)") != NULL);
  close(fd);
  free(path);
}
static void *worker(void *context) {
  char *whole = malloc(4096U), *split = malloc(4096U);
  size_t fragment;
  assert(whole != NULL && split != NULL);
  render(context, 255U, whole);
  for (fragment = 1U; fragment <= 9U; ++fragment) {
    render(context, fragment, split);
    assert(strcmp(whole, split) == 0);
  }
  free(whole);
  free(split);
  return NULL;
}
int main(int argc, char **argv) {
  pthread_t thread;
  assert(argc == 2);
  assert(pthread_create(&thread, NULL, worker, argv[1]) == 0);
  assert(pthread_join(thread, NULL) == 0);
  return 0;
}
