/** UTF-8 acceptance and borrowed-span boundaries, independent of the codec's
 * incremental scalar state. Included after the common CBOR test helpers. */
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
typedef struct utf8_borrowed {
  const unsigned char *bytes;
  size_t length, seen;
} utf8_borrowed;
static int utf8_borrow(void *context, const cai_cbor_token *token,
                       cai_error *error) {
  utf8_borrowed *value = context;
  (void)error;
  if (token->type == CAI_CBOR_DATA) {
    assert(token->length <= value->length - value->seen);
    assert(token->data == value->bytes + value->seen);
    value->seen += token->length;
  }
  return CAI_OK;
}
/** Encoder and decoder receive independent input. A validating encoder must
 * not mask a decoder defect; both must agree with the reference grammar. */
static void utf8_check(const unsigned char *bytes, size_t length,
                       size_t split) {
  cai_cbor_decoder *decoder = NULL;
  cai_cbor_encoder *encoder = NULL;
  cai_sink *sink = NULL;
  cai_sink_callbacks callbacks;
  fragment_counter emitted;
  utf8_borrowed borrowed;
  unsigned char header_bytes[3];
  size_t header_length;
  int decoded, encoded,
      expected = utf8_reference(bytes, length) ? CAI_OK : CAI_ERR_PROTOCOL;
  assert(length <= 65535U && split <= length);
  header_bytes[0] = length < 24U    ? (unsigned char)(0x60U + length)
                    : length < 256U ? 0x78U
                                    : 0x79U;
  header_length = length < 24U ? 1U : length < 256U ? 2U : 3U;
  if (header_length == 2U)
    header_bytes[1] = (unsigned char)length;
  else if (header_length == 3U) {
    header_bytes[1] = (unsigned char)(length >> 8U);
    header_bytes[2] = (unsigned char)length;
  }
  memset(&emitted, 0, sizeof(emitted));
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.context = &emitted;
  callbacks.write = bounded_write;
  borrowed.bytes = bytes;
  borrowed.length = length;
  borrowed.seen = 0U;
  assert(cai_cbor_decoder_open(utf8_borrow, &borrowed, &decoder, NULL) ==
         CAI_OK);
  assert(cai_cbor_decoder_feed(decoder, header_bytes, header_length, NULL) ==
         CAI_OK);
  assert(cai_sink_from_callbacks(&callbacks, &sink, NULL) == CAI_OK);
  assert(cai_cbor_encoder_open(sink, &encoder, NULL) == CAI_OK);
  assert(write_token(encoder, CAI_CBOR_TEXT, length, 0, NULL, 0U) == CAI_OK);
  emitted.bytes = emitted.calls = 0U;
  decoded = cai_cbor_decoder_feed(decoder, bytes, split, NULL);
  if (decoded == CAI_OK)
    decoded =
        cai_cbor_decoder_feed(decoder, bytes + split, length - split, NULL);
  if (decoded == CAI_OK)
    decoded = cai_cbor_decoder_finish(decoder, NULL);
  encoded = write_token(encoder, CAI_CBOR_DATA, 0U, 0, bytes, split);
  if (encoded == CAI_OK)
    encoded = write_token(encoder, CAI_CBOR_DATA, 0U, 0, bytes + split,
                          length - split);
  if (encoded == CAI_OK)
    encoded = write_token(encoder, CAI_CBOR_END, CAI_CBOR_TEXT, 0, NULL, 0U);
  if (encoded == CAI_OK)
    encoded = cai_cbor_encoder_finish(encoder, NULL);
  assert(decoded == expected && encoded == expected);
  if (expected == CAI_OK)
    assert(borrowed.seen == length && emitted.bytes == length);
  else {
    assert(cai_cbor_decoder_feed(decoder, NULL, 0U, NULL) == expected);
    assert(write_token(encoder, CAI_CBOR_DATA, 0U, 0, NULL, 0U) == expected);
  }
  cai_cbor_decoder_close(decoder);
  cai_cbor_encoder_close(encoder);
  cai_sink_close(sink);
}
static void utf8_checks(void) {
  static const struct {
    unsigned char bytes[4];
    size_t length;
  } sequences[] = {{{0}, 1U},
                   {{0x7f}, 1U},
                   {{0xc2, 0x80}, 2U},
                   {{0xdf, 0xbf}, 2U},
                   {{0xe0, 0xa0, 0x80}, 3U},
                   {{0xed, 0x9f, 0xbf}, 3U},
                   {{0xee, 0x80, 0x80}, 3U},
                   {{0xef, 0xbf, 0xbf}, 3U},
                   {{0xf0, 0x90, 0x80, 0x80}, 4U},
                   {{0xf4, 0x8f, 0xbf, 0xbf}, 4U},
                   {{0xc0, 0x80}, 2U},
                   {{0xe0, 0x9f, 0xbf}, 3U},
                   {{0xed, 0xa0, 0x80}, 3U},
                   {{0xf0, 0x8f, 0xbf, 0xbf}, 4U},
                   {{0xf4, 0x90, 0x80, 0x80}, 4U},
                   {{0xf5, 0x80, 0x80, 0x80}, 4U},
                   {{0x80}, 1U},
                   {{0xff}, 1U},
                   {{0xc2}, 1U},
                   {{0xe0, 0xa0}, 2U},
                   {{0xf0, 0x90, 0x80}, 3U}};
  unsigned char bytes[96];
  unsigned int pair, mutation;
  size_t i, padding, split;
  for (pair = 0U; pair < 65536U; ++pair) {
    padding = 16U + (pair & 15U);
    memset(bytes, 'a', sizeof(bytes));
    bytes[padding] = (unsigned char)(pair >> 8U);
    bytes[padding + 1U] = (unsigned char)pair;
    utf8_check(bytes, padding + 18U, padding + 1U);
    utf8_check(bytes, padding + 18U, padding + 18U);
  }
  for (i = 0U; i < sizeof(sequences) / sizeof(sequences[0]); ++i)
    for (padding = 0U; padding < 16U; ++padding) {
      size_t length = 32U + padding + sequences[i].length;
      memset(bytes, 'x', sizeof(bytes));
      bytes[2] = 0U; /* Embedded NUL is ordinary CBOR text. */
      memcpy(bytes + 16U + padding, sequences[i].bytes, sequences[i].length);
      for (split = 0U; split <= length; ++split)
        utf8_check(bytes, length, split);
    }
  /* Mutate every byte of each multibyte boundary vector to all 256 values.
   * Feed both a whole span and a split at the mutation, with varied alignment.
   */
  for (i = 0U; i < sizeof(sequences) / sizeof(sequences[0]); ++i)
    for (padding = 0U; padding < sequences[i].length; ++padding)
      for (mutation = 0U; mutation < 256U; ++mutation) {
        size_t at = 16U + (mutation & 15U);
        memset(bytes, 'x', sizeof(bytes));
        memcpy(bytes + at, sequences[i].bytes, sequences[i].length);
        bytes[at + padding] = (unsigned char)mutation;
        utf8_check(bytes, 64U, 64U);
        utf8_check(bytes, 64U, at + padding + 1U);
      }
  {
    long page_size = sysconf(_SC_PAGESIZE);
    unsigned char *pages, *at;
    int fd = open("/dev/zero", O_RDWR);
    assert(page_size >= 4096 && fd >= 0);
    pages = mmap(NULL, (size_t)page_size * 3U, PROT_NONE, MAP_PRIVATE, fd, 0);
    assert(pages != MAP_FAILED);
    assert(close(fd) == 0);
    assert(mprotect(pages + page_size, (size_t)page_size,
                    PROT_READ | PROT_WRITE) == 0);
    for (i = 1U; i <= 65U; ++i) {
      at = pages + 2U * (size_t)page_size - i;
      memset(at, 'a', i);
      utf8_check(at, i, i);
      at[i - 1U] = 0xffU;
      utf8_check(at, i, i);
    }
    assert(munmap(pages, (size_t)page_size * 3U) == 0);
  }
}
