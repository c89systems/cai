#ifndef CAI_CLI_PLAIN_FEED_H
#define CAI_CLI_PLAIN_FEED_H
#include "options.h"
#include <cai/app_client.h>
/** Non-terminal rendering adapter. Uses the same libmdf presenter, literal
 * safety and one-pass tool formatter as the terminal, without an editor,
 * margins, status rows, prompt echo or response accumulation. */
typedef struct cai_cli_plain_feed cai_cli_plain_feed;
int cai_cli_plain_feed_open(const cai_cli_options *, cai_cli_plain_feed **,
                            cai_error *);
int cai_cli_plain_feed_transcript(cai_cli_plain_feed *,
                                  const cai_transcript_event *, cai_error *);
int cai_cli_plain_feed_execution(cai_cli_plain_feed *,
                                 const cai_app_execution_event *, cai_error *);
int cai_cli_plain_feed_notice(cai_cli_plain_feed *, const char *, cai_error *);
int cai_cli_plain_feed_finish(cai_cli_plain_feed *, cai_error *);
void cai_cli_plain_feed_close(cai_cli_plain_feed *);
#endif
