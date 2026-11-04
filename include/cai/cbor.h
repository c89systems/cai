/** @file cai/cbor.h
 * Incremental RFC 8949 codec for the CAI wire profile. No DOM or value buffer.
 * Single-owner handles; callbacks must not reenter the handle invoking them.
 */
#ifndef CAI_CBOR_H
#define CAI_CBOR_H
#include <cai/cai.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define CAI_CBOR_MAX_DEPTH 32U

typedef enum cai_cbor_type {
  CAI_CBOR_UINT = 0,
  /** argument N represents -1-N, including N == UINT64_MAX. */
  CAI_CBOR_NEGATIVE,
  CAI_CBOR_BYTES,
  CAI_CBOR_TEXT,
  CAI_CBOR_ARRAY,
  CAI_CBOR_MAP,
  CAI_CBOR_BOOL,
  CAI_CBOR_NULL,
  CAI_CBOR_DATA,
  CAI_CBOR_END
} cai_cbor_type;

/** BYTES/TEXT/ARRAY/MAP begin a value. argument is the byte/item/pair count
 * unless indefinite is nonzero. DATA borrows length bytes for this call only.
 * Every begun value, even an empty definite value, has an END token whose
 * argument is its begin type. Indefinite strings contain definite strings of
 * the same type, each with its own BEGIN/DATA/END sequence. Boolean argument
 * is 0 or 1. Other unused fields should be zero.
 *
 * Text is validated incrementally. A definite text value may split a Unicode
 * scalar between DATA calls, but separate chunks of an indefinite text value
 * must each contain valid UTF-8. Embedded NUL is valid text. Tags, floating
 * point, undefined and other simple values are outside this profile.
 */
typedef struct cai_cbor_token {
  cai_cbor_type type;
  uint64_t argument;
  int indefinite;
  const void *data;
  size_t length;
} cai_cbor_token;
typedef int (*cai_cbor_token_fn)(void *context, const cai_cbor_token *token,
                                 cai_error *error);
typedef struct cai_cbor_decoder cai_cbor_decoder;
typedef struct cai_cbor_encoder cai_cbor_encoder;

/** Decode exactly one item. State is fixed-size and heap-owned; strings and
 * containers are not accumulated. feed accepts any fragment boundary and
 * emits DATA before the whole value arrives. Tokens are provisional until
 * finish succeeds: consumers must not execute partially decoded requests.
 * Duplicate map-key policy belongs to the typed consumer, not this codec.
 * All callback/parse failures are terminal; close and open a fresh handle.
 */
int cai_cbor_decoder_open(cai_cbor_token_fn callback, void *context,
                          cai_cbor_decoder **out, cai_error *error);
int cai_cbor_decoder_feed(cai_cbor_decoder *decoder, const void *bytes,
                          size_t length, cai_error *error);
int cai_cbor_decoder_finish(cai_cbor_decoder *decoder, cai_error *error);
/** Reuse allocation/callback after a successful finish, without clearing or
 * allocating a value buffer. Resetting an unfinished/failed codec is an error.
 */
int cai_cbor_decoder_reset(cai_cbor_decoder *decoder, cai_error *error);
void cai_cbor_decoder_close(cai_cbor_decoder *decoder);

/** Write exactly one item to a borrowed synchronous sink. Each call emits
 * immediately, using at most a nine-byte header. Lengths are supplied by the
 * producer; unknown lengths use indefinite strings/containers. No sizing pass,
 * backpatching, materialization or spooling is performed. The sink must outlive
 * the encoder. I/O and validation failures are terminal, including a short
 * value at finish. close never implicitly finishes or writes bytes.
 */
int cai_cbor_encoder_open(cai_sink *sink, cai_cbor_encoder **out,
                          cai_error *error);
int cai_cbor_encoder_write(cai_cbor_encoder *encoder,
                           const cai_cbor_token *token, cai_error *error);
int cai_cbor_encoder_finish(cai_cbor_encoder *encoder, cai_error *error);
/** Reuse allocation/sink after successful finish only. */
int cai_cbor_encoder_reset(cai_cbor_encoder *encoder, cai_error *error);
void cai_cbor_encoder_close(cai_cbor_encoder *encoder);

#ifdef __cplusplus
}
#endif
#endif
