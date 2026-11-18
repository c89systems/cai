/** @file native_fixture.h
 * Stateless test pumps for fragmented native codec I/O. They retain no codec,
 * payload or policy; each test owns its native reader/encoder and assertions.
 */
#ifndef CAI_TEST_NATIVE_FIXTURE_H
#define CAI_TEST_NATIVE_FIXTURE_H
#include <c89cbor/cbor.h>
#include <cai/cai.h>
typedef int (*test_token_fn)(void *, const c89cbor_token *, cai_error *);
typedef int (*test_bytes_fn)(void *, const void *, size_t, cai_error *);
static int test_native_result(int status) {
  return status == C89CBOR_OK ? CAI_OK : CAI_ERR_PROTOCOL;
}
static int test_native_feed(c89cbor_decoder *decoder, const void *bytes,
                            size_t length, test_token_fn receive, void *context,
                            cai_error *error) {
  const unsigned char *at = bytes;
  for (;;) {
    c89cbor_token token;
    size_t consumed;
    int rc = c89cbor_decoder_next(decoder, at, length, &consumed, &token);
    if (consumed != 0U)
      at += consumed;
    length -= consumed;
    if (rc == C89CBOR_INPUT || rc == C89CBOR_DONE)
      return length == 0U ? CAI_OK : CAI_ERR_PROTOCOL;
    if (rc != C89CBOR_TOKEN)
      return CAI_ERR_PROTOCOL;
    rc = receive(context, &token, error);
    if (rc != CAI_OK)
      return rc;
  }
}
static int test_native_write(c89cbor_encoder *encoder,
                             const c89cbor_token *token,
                             test_bytes_fn write_bytes, void *context,
                             cai_error *error) {
  int status = c89cbor_encoder_put(encoder, token);
  if (status != C89CBOR_OK)
    return CAI_ERR_PROTOCOL;
  for (;;) {
    const void *bytes;
    size_t length;
    int rc;
    status = c89cbor_encoder_peek(encoder, &bytes, &length);
    if (status == C89CBOR_OK)
      return CAI_OK;
    if (status != C89CBOR_OUTPUT)
      return CAI_ERR_PROTOCOL;
    rc = write_bytes(context, bytes, length, error);
    if (rc != CAI_OK)
      return rc;
    status = c89cbor_encoder_advance(encoder, length);
    if (status == C89CBOR_OK)
      return CAI_OK;
    if (status != C89CBOR_OUTPUT)
      return CAI_ERR_PROTOCOL;
  }
}
#endif
