#ifndef CAI_INPUT_QUEUE_CODEC_H
#define CAI_INPUT_QUEUE_CODEC_H
#include <cai/input_queue.h>
#include <cai/session_store.h>
/** Decoded delivery provenance. event borrows the source event's strings;
 * source holds the stable origin identity used to deduplicate consumption. */
typedef struct cai_input_delivery {
  cai_agent_session_event event;
  char source[CAI_AGENT_SESSION_ID_MAX];
  unsigned long long sequence;
  int local;
} cai_input_delivery;
/** Decode delivery provenance without copying or taking ownership of text. */
int cai_input_delivery_parse(const cai_agent_session_event *,
                             cai_input_delivery *, cai_error *);
/** A single bounded transition. Text is encoded once, not once per view.
 * The caller owns the returned JSON string. */
int cai_input_record_json(const cai_input_record *, char **, cai_error *);
/** Decode an owned transition; pair successful initialization with cleanup. */
int cai_input_record_parse(const char *, cai_input_record *, cai_error *);
/** Release owned strings in a decoded transition. */
void cai_input_record_cleanup(cai_input_record *);
/** Incremental restoration candidate; not a second active input queue. */
typedef struct cai_input_checkpoint_restore cai_input_checkpoint_restore;
/** Atomic context cuts retain bounded pending state, streamed item by item.
 * Ordinary session commits never write a queue checkpoint. */
int cai_input_checkpoint_source(cai_input_queue *, unsigned long long,
                                cai_source **, cai_error *);
/** Open an unpublished checkpoint decoder using the supplied queue policy. */
int cai_input_checkpoint_begin(const cai_input_queue_config *,
                               cai_input_checkpoint_restore **, cai_error *);
/** Consume checkpoint bytes without materializing the complete checkpoint. */
int cai_input_checkpoint_feed(cai_input_checkpoint_restore *, const void *,
                              size_t, cai_error *);
/** Publish the restored queue and input watermark only after complete input. */
int cai_input_checkpoint_finish(cai_input_checkpoint_restore *,
                                cai_input_queue **, unsigned long long *,
                                cai_error *);
/** Discard decoder resources, including an unfinished restore candidate. */
void cai_input_checkpoint_close(cai_input_checkpoint_restore *);
#endif
