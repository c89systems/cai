/** @file cai/transcript_selection.h
 * Bounded final-response selection and range projection. These adapters own
 * no socket, runtime, storage reader, renderer, worker or response text.
 */
#ifndef CAI_TRANSCRIPT_SELECTION_H
#define CAI_TRANSCRIPT_SELECTION_H
#include <cai/transcript.h>
#ifdef __cplusplus
extern "C" {
#endif

/** An accepted subscription boundary, never a provisional DATA position. */
typedef struct cai_transcript_position {
  char session[129], collection[21];
  uint64_t stream, conversation, position, sequence;
} cai_transcript_position;
typedef enum cai_transcript_work_state {
  CAI_TRANSCRIPT_WORK_WAITING = 0,
  CAI_TRANSCRIPT_WORK_ACTIVE,
  CAI_TRANSCRIPT_WORK_COMPLETED,
  CAI_TRANSCRIPT_WORK_FAILED,
  CAI_TRANSCRIPT_WORK_CANCELLED
} cai_transcript_work_state;
/** Fixed metadata only. A range is usable after a matched terminal record.
 * text_bytes=0 needs no read. Otherwise continue from after using its original
 * session/collection and replay_turns=-1, stopping at through_position.
 * The stream ID belongs to the original observation, not the new range read.
 */
typedef struct cai_transcript_selection_result {
  cai_transcript_work_state state;
  long long code;
  cai_transcript_position after;
  uint64_t through_position, through_sequence, text_bytes;
} cai_transcript_selection_result;
typedef struct cai_transcript_selection cai_transcript_selection;
/** Observe from an accepted cursor before admitting work. call_id is the
 * queued turn identity input:SESSION:PRODUCER:REQUEST:ITEM. NULL explicitly
 * observes unowned/direct work in this session (for example goal pursuit).
 * It may join an already-active unowned turn at its next response boundary;
 * only text after the supplied cursor is available. Tagged turns and child
 * sessions cannot start it. All calls
 * stay on one owner thread. The adapter retains fixed metadata plus one bounded
 * incremental execution decoder, never the assistant response. */
int cai_transcript_selection_open(const cai_transcript_position *,
                                  const char *call_id,
                                  cai_transcript_selection **, cai_error *);
/** Suitable for a raw transcript callback. Only accepted END boundaries
 * publish cursors. Failure sticks until close; missing terminal records leave
 * work WAITING/ACTIVE. Empty completed responses replace earlier text. A
 * failed/cancelled turn may select its current accepted partial response.
 * Once its terminal record is accepted, the result is frozen and subsequent
 * spans are ignored; they cannot invalidate an already-proven outcome. */
int cai_transcript_selection_event(void *, const cai_transcript_event *,
                                   cai_error *);
int cai_transcript_selection_get(const cai_transcript_selection *,
                                 cai_transcript_selection_result *,
                                 cai_error *);
void cai_transcript_selection_close(cai_transcript_selection *);

typedef struct cai_transcript_projector cai_transcript_projector;
/** Project one terminal selection through a borrowed sink. The caller starts
 * an ordinary cursor-continuation subscription. Its REPLAY_BEGIN must precede
 * all data and cover the entire range. No whole-value staging or source read
 * occurs here. Empty selections are already done. Output is provisional until
 * accepted event boundaries; source/sink failure may leave partial output.
 * Sink callbacks may inspect done but must not reenter event or close.
 */
int cai_transcript_projector_open(const cai_transcript_selection_result *,
                                  cai_sink *, cai_transcript_projector **,
                                  cai_error *);
int cai_transcript_projector_event(void *, const cai_transcript_event *,
                                   cai_error *);
/** True only after the exact final boundary and byte count were accepted.
 * Subsequent spans are ignored. NULL and failed projectors return false. */
int cai_transcript_projector_done(const cai_transcript_projector *);
void cai_transcript_projector_close(cai_transcript_projector *);
#ifdef __cplusplus
}
#endif
#endif
