#ifndef CAI_CLI_TRANSCRIPT_FEED_H
#define CAI_CLI_TRANSCRIPT_FEED_H
#include "pouch.h"
#include "transcript_wire.h"
#include <cai/attachment.h>
typedef struct cai_cli_transcript_publisher cai_cli_transcript_publisher;
typedef struct cai_cli_transcript_frame {
  cai_transcript_event event;
  char type[129], session[129], collection[21], reason[192];
  unsigned char data[CAI_CLI_TRANSCRIPT_CHUNK];
} cai_cli_transcript_frame;
/* One replay worker for the conversation, sharing its existing SDK client.
 * Construct on the owner after restoration; close before runtime/store close.
 * No thread per frontend, read window or event. Idle waits have no polling. */
int cai_cli_transcript_feed_open(cai_cli_pouch *, const char *workspace,
                                 const char *session, uint64_t,
                                 cai_cli_transcript_publisher **, cai_error *);
int cai_cli_transcript_feed_request(cai_cli_transcript_publisher *,
                                    uint64_t attachment, uint64_t stream,
                                    const char *collection, uint64_t after,
                                    uint64_t sequence, long long replay_turns,
                                    cai_error *);
void cai_cli_transcript_feed_cancel(cai_cli_transcript_publisher *, uint64_t,
                                    uint64_t stream);
void cai_cli_transcript_feed_detach(cai_cli_transcript_publisher *, uint64_t);
/* Always watch fd for newly produced spans, and drain its wakeup in the owner
 * loop. Use ready() to service queued spans when that peer has output credit
 * (or an error); otherwise its socket POLLOUT releases backpressure. next()
 * copies one bounded span. No logical event is accumulated here. */
int cai_cli_transcript_feed_fd(cai_cli_transcript_publisher *);
void cai_cli_transcript_feed_drain(cai_cli_transcript_publisher *);
int cai_cli_transcript_feed_ready(cai_cli_transcript_publisher *, uint64_t,
                                  int *failed);
int cai_cli_transcript_feed_next(cai_cli_transcript_publisher *, uint64_t,
                                 cai_cli_transcript_frame *);
void cai_cli_transcript_frame_borrow(cai_cli_transcript_frame *);
void cai_cli_transcript_feed_close(cai_cli_transcript_publisher *);
/** Prompt recall shares the retained reader worker, without another SDK
 * client or a materialized history. Each channel has its own bounded ring. */
void cai_cli_transcript_feed_prompt_limit(cai_cli_transcript_publisher *,
                                          unsigned int);
int cai_cli_transcript_feed_prompts(cai_cli_transcript_publisher *, uint64_t,
                                    uint64_t, unsigned int, cai_error *);
int cai_cli_transcript_feed_prompt_ready(cai_cli_transcript_publisher *,
                                         uint64_t, int *);
int cai_cli_transcript_feed_prompt_next(cai_cli_transcript_publisher *,
                                        uint64_t, cai_cli_transcript_frame *);
#endif
