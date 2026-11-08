#ifndef CAI_CLI_TRANSCRIPT_FEED_H
#define CAI_CLI_TRANSCRIPT_FEED_H
#include "pouch.h"
#include "transcript_wire.h"
#include <cai/attachment.h>
typedef struct cai_cli_transcript_publisher cai_cli_transcript_publisher;
typedef struct cai_cli_transcript_frame {
  cai_transcript_event event;
  char type[129], session[129], collection[21], reason[192];
  char call_id[257], terminal_id[257];
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
/** Live-only bounded raw spans. Ordered after their durable shell-start event
 * in each existing live ring, never persisted or accumulated. A peer that
 * misses any chunk receives no later chunks/end for that recording; its
 * ordinary journal stream still carries the saved tail. final has no data.
 * A subscribed guard supplies backpressure: accepted=0 means no peer was
 * modified and this exact span must be retried. Zero guard is best effort. */
int cai_cli_transcript_feed_shell(cai_cli_transcript_publisher *,
                                  uint64_t guard, uint64_t sequence,
                                  uint64_t offset, const void *, size_t,
                                  int final, int *accepted, cai_error *);
int cai_cli_transcript_feed_ready(cai_cli_transcript_publisher *, uint64_t,
                                  int *failed);
/** Queue one bounded ephemeral tool span between complete durable events.
 * Slow/catching-up peers skip live bytes and use ordinary tool-result ranges.
 * No journal writes, waiting, extra readers or worker creation. */
int cai_cli_transcript_feed_terminal(cai_cli_transcript_publisher *,
                                     const cai_agent_runtime_event *,
                                     cai_error *);
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
