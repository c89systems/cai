#ifndef CAI_TERMINAL_TEXT_H
#define CAI_TERMINAL_TEXT_H
#include <cai/cai.h>

/** Bounded, sanitized logical-line tail. Not a screen/alternate-screen
 * emulator. Both the runtime and PTY host consume raw bytes incrementally. */
typedef struct cai_terminal_text {
  char *rows, *line, *line_storage;
  size_t byte_limit, line_limit, start, length, lines;
  size_t line_length, cursor;
  unsigned char utf8[4];
  size_t utf8_length, utf8_need;
  unsigned int state, string_kind;
  size_t parameter;
  int parameter_seen, truncated;
} cai_terminal_text;

/** Allocate a tail limited by both logical line count and retained bytes. */
int cai_terminal_text_init(cai_terminal_text *text, size_t lines, size_t bytes,
                           cai_error *error);
/** Release owned tail buffers after the last write/finish. */
void cai_terminal_text_close(cai_terminal_text *text);
/** Consume raw terminal bytes, carrying split UTF-8 and escape sequences
 * across writes. Retention remains bounded by the initialized limits. */
void cai_terminal_text_write(cai_terminal_text *text, const void *bytes,
                             size_t length);
/** Caller owns the bounded UTF-8 tail; incomplete controls are discarded. */
int cai_terminal_text_finish(cai_terminal_text *text, char **out,
                             cai_error *error);
#endif
