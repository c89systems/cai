/** @file cai/transcript.h Streaming presentation, separate from model input. */
#ifndef CAI_TRANSCRIPT_H
#define CAI_TRANSCRIPT_H
#include <cai/cai.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum cai_transcript_kind {
  CAI_TRANSCRIPT_REPLAY_BEGIN = 0,
  CAI_TRANSCRIPT_EVENT_BEGIN,
  CAI_TRANSCRIPT_DATA,
  CAI_TRANSCRIPT_EVENT_END,
  CAI_TRANSCRIPT_REPLAY_END,
  CAI_TRANSCRIPT_ERROR,
  /** Live-only raw shell bytes anchored to user_shell_started's sequence.
   * offset is the raw byte offset. Never advances the durable replay cursor.
   * A slow/detached subscriber recovers the retained journal tail instead. */
  CAI_TRANSCRIPT_SHELL_OUTPUT,
  /** Contiguous live output completed at offset, before the saved tail.
   * No data and no durable position. Absent if live delivery was interrupted.
   */
  CAI_TRANSCRIPT_SHELL_OUTPUT_END
} cai_transcript_kind;
typedef enum cai_transcript_failure {
  CAI_TRANSCRIPT_FAILED = 0,
  CAI_TRANSCRIPT_SLOW,
  CAI_TRANSCRIPT_HISTORY_CHANGED,
  CAI_TRANSCRIPT_CONTEXT_CLOSED
} cai_transcript_failure;
/** Borrowed for one callback. Event bytes may contain NUL and split UTF-8
 * characters; consume length, not strlen. BEGIN/DATA are provisional until
 * EVENT_END validates length and the durable collection position. Retain only
 * accepted EVENT_END positions/sequences as reconnect cursors. An accepted
 * REPLAY_END also advances the position to its watermark, across non-display
 * records, while retaining the last event sequence. ERROR aborts
 * partial rendering and requires explicit synchronization; it never resubmits
 * model input. Replay and durable live events use the same shapes and
 * identities. SHELL_OUTPUT/SHELL_OUTPUT_END are the explicit live-only
 * exception: ordered bounded raw spans, anchored to the shell-start sequence,
 * without a durable position. They never advance a cursor and are absent from
 * replay/export. */
typedef struct cai_transcript_event {
  cai_transcript_kind kind;
  uint64_t stream, conversation, sequence, timestamp_ns, position, offset,
      watermark, visibility;
  /** Older presentation entries omitted by a selected initial replay. Zero
   * for cursor continuation, full replay and prompt recall. */
  uint64_t omitted;
  int64_t utc_offset_seconds;
  const char *session, *collection, *type;
  const void *data;
  size_t length;
  int has_data, code;
  cai_transcript_failure failure;
  const char *reason;
} cai_transcript_event;
#ifdef __cplusplus
}
#endif
#endif
