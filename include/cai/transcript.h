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
  CAI_TRANSCRIPT_SHELL_OUTPUT_END,
  /** Best-effort command-tool bytes, scoped by source session/call/terminal.
   * sequence is the terminal command ID; offset counts retained command bytes.
   * No durable position or replay cursor is advanced. Gaps recover through the
   * ordinary saved tool result, whose output_offset identifies the byte range.
   */
  CAI_TRANSCRIPT_TERMINAL_OUTPUT
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
 * identities. SHELL_OUTPUT/SHELL_OUTPUT_END and TERMINAL_OUTPUT are live-only
 * exceptions: ordered bounded spans, without a durable position. Shell spans
 * are anchored to the shell-start sequence; tool spans identify the source
 * runtime/call/command. They never advance a cursor and are absent from
 * replay/export, which use retained shell tails and command results. */
typedef struct cai_transcript_event {
  cai_transcript_kind kind;
  uint64_t stream, conversation, sequence, timestamp_ns, position, offset,
      watermark, visibility;
  /** Older presentation entries omitted by a selected initial replay. Zero
   * for cursor continuation, full replay and prompt recall. */
  uint64_t omitted;
  int64_t utc_offset_seconds;
  const char *session, *collection, *type;
  /** Present only on TERMINAL_OUTPUT, borrowed for this callback. */
  const char *call_id, *terminal_id;
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
