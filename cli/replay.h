#ifndef CAI_CLI_REPLAY_H
#define CAI_CLI_REPLAY_H
#include <cai/session_store.h>

/* Console-only tail index. Retains sequence ranges and pending input IDs,
 * never session text. Feed only events visible to the runtime. */
typedef struct cai_cli_replay cai_cli_replay;
int cai_cli_replay_open(size_t limit, cai_cli_replay **out, cai_error *error);
int cai_cli_replay_feed(cai_cli_replay *replay,
                        const cai_agent_session_event *event, cai_error *error);
int cai_cli_replay_finish(cai_cli_replay *replay, cai_error *error);
int cai_cli_replay_contains(void *replay, unsigned long long sequence);
/* Zero when no console events were selected. */
unsigned long long cai_cli_replay_first(const cai_cli_replay *replay);
unsigned long long cai_cli_replay_last(const cai_cli_replay *replay);
size_t cai_cli_replay_omitted(const cai_cli_replay *replay);
/* A validated review's Markdown replaces only its final raw JSON text. The
 * recovery pass indexes source ranges, never buffers reviewer output. */
int cai_cli_replay_child_text_visible(const cai_cli_replay *replay,
                                      const char *child,
                                      unsigned long long source_sequence);
void cai_cli_replay_close(cai_cli_replay *replay);

/** Session-lifetime sequence metadata for attached replay. No event
 * bodies or child reports are retained/decoded. Feed at the existing recovery
 * and durable append seams; callers serialize access with the journal lock. */
int cai_cli_replay_index_open(cai_cli_replay **, cai_error *);
/** Copy only the selected sequence ranges for an independent reader window.
 * -1 selects all indexed entries; zero selects none. No journal read. */
int cai_cli_replay_select(const cai_cli_replay *, long long, cai_cli_replay **,
                          cai_error *);
/** Apply an acknowledged atomic cut to metadata. On failure discard the index
 * rather than serving a stale selection. Event identities are never reused. */
int cai_cli_replay_cut(cai_cli_replay *, const cai_agent_session_trim_request *,
                       cai_error *);

/* Presentation order for durable input journals. Only pending prompt text is
 * retained; assistant/shell events pass directly to the renderer. */
typedef struct cai_cli_replay_order cai_cli_replay_order;
int cai_cli_replay_order_open(int journal_v2, cai_cli_replay_order **out,
                              cai_error *error);
int cai_cli_replay_order_feed(cai_cli_replay_order *order,
                              const cai_agent_session_event *event,
                              cai_agent_session_event_fn emit, void *context,
                              cai_error *error);
void cai_cli_replay_order_close(cai_cli_replay_order *order);
#endif
