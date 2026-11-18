static int utf8_reference(const unsigned char *bytes, size_t length) {
  size_t i = 0U;
  while (i < length) {
    unsigned int lead = bytes[i++], second;
    size_t tail;
    if (lead < 0x80U)
      continue;
    if (lead >= 0xc2U && lead <= 0xdfU)
      tail = 1U;
    else if (lead >= 0xe0U && lead <= 0xefU)
      tail = 2U;
    else if (lead >= 0xf0U && lead <= 0xf4U)
      tail = 3U;
    else
      return 0;
    if (tail > length - i)
      return 0;
    second = bytes[i];
    if ((lead == 0xe0U && second < 0xa0U) ||
        (lead == 0xedU && second > 0x9fU) ||
        (lead == 0xf0U && second < 0x90U) || (lead == 0xf4U && second > 0x8fU))
      return 0;
    while (tail-- != 0U)
      if (bytes[i] < 0x80U || bytes[i++] > 0xbfU)
        return 0;
  }
  return 1;
}
/** Validate independent producer and consumer inputs against the reference. */
static int utf8_put(fixture *f, const c89cbor_token *token) {
  size_t offset = 0U;
  int rc = c89cbor_encoder_put(&f->encoder, token);
  if (rc != C89CBOR_OK)
    return rc;
  for (;;) {
    const void *bytes;
    size_t length;
    rc = c89cbor_encoder_peek(&f->encoder, &bytes, &length);
    if (rc != C89CBOR_OUTPUT)
      return rc;
    assert(length <= C89CBOR_CHUNK);
    if (token->type == C89CBOR_DATA) {
      assert(bytes == (const unsigned char *)token->data + offset);
      offset += length;
    }
    rc = c89cbor_encoder_advance(&f->encoder, length);
    if (rc == C89CBOR_OK)
      return rc;
    assert(rc == C89CBOR_OUTPUT);
  }
}
static void utf8_check(const unsigned char *input, size_t length,
                       size_t split) {
  fixture *f = open_fixture();
  unsigned char header[3];
  c89cbor_token token;
  size_t n = length < 24U ? 1U : length < 256U ? 2U : 3U;
  int decoded, encoded,
      expected = utf8_reference(input, length) ? C89CBOR_OK : C89CBOR_MALFORMED;
  assert(length <= 65535U && split <= length);
  header[0] = length < 24U    ? (unsigned char)(0x60U + length)
              : length < 256U ? 0x78U
                              : 0x79U;
  if (n == 2U)
    header[1] = (unsigned char)length;
  else if (n == 3U) {
    header[1] = (unsigned char)(length >> 8U);
    header[2] = (unsigned char)length;
  }
  assert(feed(f, header, n) == C89CBOR_OK);
  decoded = feed(f, input, split);
  if (decoded == C89CBOR_OK)
    decoded = feed(f, input + split, length - split);
  if (decoded == C89CBOR_OK)
    decoded = c89cbor_decoder_finish(&f->decoder);
  memset(&token, 0, sizeof(token));
  token.type = C89CBOR_TEXT;
  token.argument = length;
  assert(utf8_put(f, &token) == C89CBOR_OK);
  token.type = C89CBOR_DATA;
  token.argument = 0U;
  token.data = input;
  token.length = split;
  encoded = utf8_put(f, &token);
  if (encoded == C89CBOR_OK) {
    token.data = input + split;
    token.length = length - split;
    encoded = utf8_put(f, &token);
  }
  if (encoded == C89CBOR_OK) {
    memset(&token, 0, sizeof(token));
    token.type = C89CBOR_END;
    token.argument = C89CBOR_TEXT;
    encoded = utf8_put(f, &token);
  }
  assert(decoded == expected && encoded == expected);
  close_fixture(f);
}
static void utf8_checks(void) {
  unsigned int a, b;
  size_t split;
  for (a = 0U; a <= 255U; ++a)
    for (b = 0U; b <= 255U; ++b) {
      unsigned char value[2];
      value[0] = (unsigned char)a;
      value[1] = (unsigned char)b;
      for (split = 0U; split <= 2U; ++split)
        utf8_check(value, sizeof(value), split);
    }
  {
    static const unsigned char good[] = {0x61, 0xe2, 0x82, 0xac,
                                         0xf4, 0x8f, 0xbf, 0xbf};
    static const unsigned char bad[] = {0x61, 0xf4, 0x90, 0x80, 0x80};
    size_t length = C89CBOR_CHUNK * 2U + 4U;
    unsigned char *text = malloc(length);
    assert(text != NULL);
    memset(text, 'a', length);
    memcpy(text + C89CBOR_CHUNK - 1U, good + 1U, 3U);
    for (split = 0U; split <= sizeof(good); ++split)
      utf8_check(good, sizeof(good), split);
    for (split = 0U; split <= sizeof(bad); ++split)
      utf8_check(bad, sizeof(bad), split);
    utf8_check(text, length, C89CBOR_CHUNK);
    utf8_check(text, length, length);
    text[length - 1U] = 0xffU;
    utf8_check(text, length, C89CBOR_CHUNK);
    free(text);
  }
}
