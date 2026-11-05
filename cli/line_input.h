#ifndef CAI_CLI_LINE_INPUT_H
#define CAI_CLI_LINE_INPUT_H
#include <cai/cai.h>
#include <poll.h>
/** Incremental non-terminal input. Owns one bounded operator input and one
 * 4 KiB read chunk; never changes descriptor flags or reads the entire stream.
 * The descriptor is borrowed. EOF still delivers a final unterminated line. */
typedef struct cai_cli_line_input cai_cli_line_input;
int cai_cli_line_input_open(int fd, cai_cli_line_input **out, cai_error *error);
/** Consume buffered bytes, and at most one read when readable is nonzero.
 * An owned line or EOF is returned separately; otherwise prepare/poll again.
 * Empty lines are skipped. Invalid NUL/oversized input fails without delivery.
 */
int cai_cli_line_input_next(cai_cli_line_input *, int readable, char **line,
                            int *eof, cai_error *);
void cai_cli_line_input_prepare(cai_cli_line_input *, struct pollfd *);
void cai_cli_line_input_close(cai_cli_line_input *);
#endif
