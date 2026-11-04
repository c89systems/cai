#ifndef CAI_CLI_LITERAL_H
#define CAI_CLI_LITERAL_H
#include <libmdf/mdf.h>

/** Incremental literal adapter for the released libmdf manual-token API.
 * Owns only one escape chunk and an incomplete UTF-8 character; renderer is
 * borrowed. Chunk boundaries never become newlines or block boundaries.
 * Libmdf itself currently retains unbroken words until its wrapping decision;
 * the forthcoming write_literal API will replace this adapter. */
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
 * bounded escape chunk. Does not finish the renderer or close its block. */
int cai_cli_literal_finish(cai_cli_literal *);
/** Write an already available literal string through that same adapter. */
int cai_cli_literal_write(mdf *renderer, const char *text);

/* Render literal UTF-8 through libmdf's manual token stream, using bounded
 * chunks and preserving punctuation. The renderer must have a bound sink. */
int cai_cli_literal_render(mdf *renderer, const char *text);
#endif
