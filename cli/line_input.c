#include "line_input.h"
#include "error.h"
#include "input_wire.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
struct cai_cli_line_input {
  int fd, eof;
  size_t first, last, used;
  unsigned char chunk[4096];
  char line[CAI_CLI_INPUT_TEXT_MAX + 2U];
};
int cai_cli_line_input_open(int fd, cai_cli_line_input **out,
                            cai_error *error) {
  *out = calloc(1U, sizeof(**out));
  if (*out == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate bounded line input");
  (*out)->fd = fd;
  return CAI_OK;
}
int cai_cli_line_input_next(cai_cli_line_input *r, int readable, char **line,
                            int *eof, cai_error *error) {
  *line = NULL;
  *eof = 0;
  for (;;) {
    if (r->first == r->last && !r->eof) {
      ssize_t n;
      if (!readable)
        return CAI_OK;
      readable = 0;
      do {
        n = read(r->fd, r->chunk, sizeof(r->chunk));
      } while (n < 0 && errno == EINTR);
      if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
          return CAI_OK;
        return cai_cli_error(error, CAI_ERR_TRANSPORT, "read redirected input");
      }
      r->first = 0U;
      r->last = (size_t)n;
      r->eof = n == 0;
    }
    if (r->first == r->last && r->eof) {
      if (r->used == 0U) {
        *eof = 1;
        return CAI_OK;
      }
    } else {
      unsigned char ch = r->chunk[r->first++];
      if (ch == 0U)
        return cai_cli_error(error, CAI_ERR_INVALID,
                             "redirected input contains NUL");
      if (ch != '\n') {
        if (r->used > CAI_CLI_INPUT_TEXT_MAX ||
            (r->used == CAI_CLI_INPUT_TEXT_MAX && ch != '\r'))
          return cai_cli_error(error, CAI_ERR_LIMIT,
                               "redirected input line exceeds input limit");
        r->line[r->used++] = (char)ch;
        continue;
      }
    }
    if (r->used != 0U && r->line[r->used - 1U] == '\r')
      --r->used;
    if (r->used == 0U)
      continue;
    *line = malloc(r->used + 1U);
    if (*line == NULL)
      return cai_cli_error(error, CAI_ERR_NOMEM, "copy redirected input line");
    memcpy(*line, r->line, r->used);
    (*line)[r->used] = '\0';
    r->used = 0U;
    return CAI_OK;
  }
}
void cai_cli_line_input_prepare(cai_cli_line_input *r, struct pollfd *fd) {
  memset(fd, 0, sizeof(*fd));
  fd->fd = !r->eof && r->first == r->last ? r->fd : -1;
  fd->events = POLLIN;
}
void cai_cli_line_input_close(cai_cli_line_input *r) { free(r); }
