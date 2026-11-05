#define _POSIX_C_SOURCE 200809L
#include "../cli/input_wire.h"
#include "../cli/line_input.h"
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static void expect_line(cai_cli_line_input *r, const char *expected) {
  char *line = NULL;
  int eof = 0, i;
  for (i = 0; i < 32 && line == NULL && !eof; ++i)
    assert(cai_cli_line_input_next(r, 1, &line, &eof, NULL) == CAI_OK);
  if (expected == NULL)
    assert(eof && line == NULL);
  else
    assert(line != NULL && strcmp(line, expected) == 0);
  free(line);
}
static void fragmented(void) {
  cai_cli_line_input *r;
  struct pollfd wait;
  char *line = NULL;
  int fd[2], eof = 0, flags;
  assert(pipe(fd) == 0);
  flags = fcntl(fd[0], F_GETFL);
  assert(cai_cli_line_input_open(fd[0], &r, NULL) == CAI_OK);
  assert(cai_cli_line_input_next(r, 0, &line, &eof, NULL) == CAI_OK);
  assert(line == NULL && !eof);
  assert(write(fd[1], "a\342", 2U) == 2);
  assert(cai_cli_line_input_next(r, 1, &line, &eof, NULL) == CAI_OK);
  assert(line == NULL && !eof);
  cai_cli_line_input_prepare(r, &wait);
  assert(wait.fd == fd[0] && wait.events == POLLIN);
  assert(write(fd[1], "\202\254\r\n\nsecond\nthird", 17U) == 17);
  close(fd[1]);
  expect_line(r, "a€");
  /* The second complete input is already in the bounded chunk. No descriptor
   * interest is necessary until the consumer has advanced through it. */
  cai_cli_line_input_prepare(r, &wait);
  assert(wait.fd == -1);
  expect_line(r, "second");
  expect_line(r, "third");
  expect_line(r, NULL);
  assert(fcntl(fd[0], F_GETFL) == flags);
  cai_cli_line_input_close(r);
  assert(fcntl(fd[0], F_GETFL) ==
         flags); /* Borrowed descriptor remains open. */
  close(fd[0]);
}
static void limits(const char *directory) {
  char *path = malloc(strlen(directory) + 32U);
  char *value = malloc(CAI_CLI_INPUT_TEXT_MAX + 3U);
  char *line = NULL;
  cai_cli_line_input *r;
  int mode, fd, eof, rc, i;
  assert(path != NULL && value != NULL);
  for (mode = 0; mode < 5; ++mode) {
    size_t n = CAI_CLI_INPUT_TEXT_MAX;
    sprintf(path, "%s/line-input-XXXXXX", directory);
    fd = mkstemp(path);
    assert(fd >= 0 && unlink(path) == 0);
    memset(value, 'x', n);
    if (mode == 1)
      value[n++] = '\r';
    else if (mode == 2)
      value[n++] = 'x';
    else if (mode == 3) {
      n = 3U;
      value[1] = '\0';
    }
    if (mode != 4)
      value[n++] = '\n';
    assert(write(fd, value, n) == (ssize_t)n && lseek(fd, 0, SEEK_SET) == 0);
    assert(cai_cli_line_input_open(fd, &r, NULL) == CAI_OK);
    line = NULL;
    eof = 0;
    rc = CAI_OK;
    for (i = 0; i < 32 && rc == CAI_OK && line == NULL && !eof; ++i)
      rc = cai_cli_line_input_next(r, 1, &line, &eof, NULL);
    if (mode == 2 || mode == 3)
      assert(rc == (mode == 2 ? CAI_ERR_LIMIT : CAI_ERR_INVALID) &&
             line == NULL);
    else
      assert(rc == CAI_OK && line != NULL &&
             strlen(line) == CAI_CLI_INPUT_TEXT_MAX);
    free(line);
    cai_cli_line_input_close(r);
    close(fd);
  }
  free(value);
  free(path);
}
static void *worker(void *context) {
  fragmented();
  limits(context);
  return NULL;
}
int main(int argc, char **argv) {
  pthread_t thread;
  assert(argc == 2);
  /* The large bounded input belongs to the heap, including on musl. */
  assert(pthread_create(&thread, NULL, worker, argv[1]) == 0);
  assert(pthread_join(thread, NULL) == 0);
  return 0;
}
