#ifndef CAI_CLI_LITERAL_H
#define CAI_CLI_LITERAL_H
#include <libmdf/mdf.h>

/** Incremental terminal-safety adapter for libmdf write_literal. Owns only one
 * bounded sanitization chunk and an incomplete UTF-8 character; renderer is
 * borrowed. No Markdown escaping or whole-input buffer. Libmdf owns word
 * storage, wrapping and semantic newlines across fragments. */
typedef struct cai_cli_literal {
  mdf *renderer;
  char chunk[256];
  unsigned char rune[4];
  size_t used, pending;
  int failed, finished;
} cai_cli_literal;
/** Start a fresh literal field inside a caller-owned manual block. */
void cai_cli_literal_init(cai_cli_literal *, mdf *);
/** Accept arbitrary byte spans, including split UTF-8. Terminal controls and
 * malformed bytes are repaired, not interpreted. A failed sink is sticky;
 * neither feed nor finish retries uncertain output. */
int cai_cli_literal_feed(cai_cli_literal *, const void *, size_t);
/** End the field, repairing an incomplete last character and flushing its
 * bounded sanitization chunk. Does not finish the renderer or close its block.
 */
int cai_cli_literal_finish(cai_cli_literal *);
/** Write an already available literal string through that same adapter. */
int cai_cli_literal_write(mdf *renderer, const char *text);

/* Render literal UTF-8 through libmdf write_literal, using bounded
 * chunks and preserving punctuation. The renderer must have a bound sink. */
int cai_cli_literal_render(mdf *renderer, const char *text);
#endif
