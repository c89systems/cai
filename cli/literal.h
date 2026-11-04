#ifndef CAI_CLI_LITERAL_H
#define CAI_CLI_LITERAL_H
#include <libmdf/mdf.h>

/* Write an already available literal string inside a manual block. Does not
 * finish the renderer. Libmdf currently retains unbroken words; this must not
 * be advertised as a bounded remote-input streaming adapter. */
int cai_cli_literal_write(mdf *renderer, const char *text);

/* Render literal UTF-8 through libmdf's manual token stream, using bounded
 * chunks and preserving punctuation. The renderer must have a bound sink. */
int cai_cli_literal_render(mdf *renderer, const char *text);
#endif
