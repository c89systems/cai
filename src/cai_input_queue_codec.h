#ifndef CAI_INPUT_QUEUE_CODEC_H
#define CAI_INPUT_QUEUE_CODEC_H
#include <cai/input_queue.h>
#include <cai/session_store.h>
typedef struct cai_input_delivery {
  cai_agent_session_event event;
  char source[CAI_AGENT_SESSION_ID_MAX];
  unsigned long long sequence;
  int local;
} cai_input_delivery;
int cai_input_delivery_parse(const cai_agent_session_event *,
                             cai_input_delivery *, cai_error *);
/* A single bounded transition. Text is encoded once, not once per view. */
int cai_input_record_json(const cai_input_record *, char **, cai_error *);
int cai_input_record_parse(const char *, cai_input_record *, cai_error *);
void cai_input_record_cleanup(cai_input_record *);
typedef struct cai_input_checkpoint_restore cai_input_checkpoint_restore;
/* Atomic context cuts retain bounded pending state, streamed item by item.
 * Ordinary conversation commits never write a queue checkpoint. */
int cai_input_checkpoint_source(cai_input_queue *, unsigned long long,
                                cai_source **, cai_error *);
int cai_input_checkpoint_begin(const cai_input_queue_config *,
                               cai_input_checkpoint_restore **, cai_error *);
int cai_input_checkpoint_feed(cai_input_checkpoint_restore *, const void *,
                              size_t, cai_error *);
int cai_input_checkpoint_finish(cai_input_checkpoint_restore *,
                                cai_input_queue **, unsigned long long *,
                                cai_error *);
void cai_input_checkpoint_close(cai_input_checkpoint_restore *);
#endif
