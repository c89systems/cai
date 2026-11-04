#ifndef CAI_CLI_TRANSCRIPT_H
#define CAI_CLI_TRANSCRIPT_H
#include <cai/session_store.h>
typedef struct cai_cli_transcript cai_cli_transcript;
typedef enum cai_cli_transcript_kind {
  CAI_CLI_TRANSCRIPT_BEGIN = 0,
  CAI_CLI_TRANSCRIPT_DATA,
  CAI_CLI_TRANSCRIPT_END
} cai_cli_transcript_kind;
typedef struct cai_cli_transcript_event {
  cai_cli_transcript_kind kind;
  unsigned long long sequence, timestamp_ns, position;
  long long utc_offset_seconds;
  const char *type;
  const void *data;
  size_t length;
  int has_data;
} cai_cli_transcript_event;
/** Borrowed callbacks. BEGIN/DATA are provisional; only END accepts an event
 * after the collection's authenticated entry boundary. Consumer failure is
 * terminal. Never accumulate the event or reenter/close this parser here. */
typedef int (*cai_cli_transcript_fn)(void *, const cai_cli_transcript_event *,
                                     cai_error *);
int cai_cli_transcript_open(cai_cli_transcript_fn, void *,
                            cai_cli_transcript **out, cai_error *);
/** Raw compact journal JSON + LF, in bounded arbitrary chunks. Commit state is
 * discarded incrementally; data strings are delivered decoded, without a
 * size limit, full-value allocation, spool or extra JSON representation.
 * Canonical event headers precede data; event type names are at most 128 bytes.
 * Any error is terminal: discard the candidate and open a new parser. */
int cai_cli_transcript_feed(cai_cli_transcript *, const void *, size_t,
                            cai_error *);
/** Call once per complete stored entry, after its payload. Positions and event
 * sequences must increase; deleted positions/sequences may leave gaps. */
int cai_cli_transcript_boundary(cai_cli_transcript *,
                                unsigned long long position,
                                const char *append_id, cai_error *);
/** Success requires an authenticated boundary, not merely syntactically
 * complete JSON. A partial record is never implicitly committed. */
int cai_cli_transcript_finish(cai_cli_transcript *, cai_error *);
void cai_cli_transcript_close(cai_cli_transcript *);
#endif
