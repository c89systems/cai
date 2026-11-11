/** @file Strict incremental UTF-8, including overlong/surrogate rejection. */
#include <cai/utf8.h>
#include <string.h>
int cai_utf8_feed(cai_utf8_state *state, const void *bytes, size_t count) {
  const unsigned char *data = bytes;
  size_t i = 0U;
  unsigned int pending;
  uint32_t scalar, minimum;
  if (state == NULL || state->failed || (count && data == NULL))
    return 0;
  pending = state->pending;
  scalar = state->scalar;
  minimum = state->minimum;
  while (i < count) {
    unsigned int byte = data[i];
    /* Keep the CBOR word-at-a-time ASCII path, including unaligned spans. */
    if (pending == 0U && byte < 0x80U) {
      while (count - i >= sizeof(uint64_t)) {
        uint64_t word;
        memcpy(&word, data + i, sizeof(word));
        if ((word & UINT64_C(0x8080808080808080)) != 0U)
          break;
        i += sizeof(word);
      }
      while (i < count && data[i] < 0x80U)
        ++i;
      continue;
    }
    ++i;
    if (pending) {
      if ((byte & 0xc0U) != 0x80U)
        goto invalid;
      scalar = (scalar << 6U) | (byte & 0x3fU);
      if (--pending == 0U && (scalar < minimum || scalar > 0x10ffffU ||
                              (scalar >= 0xd800U && scalar <= 0xdfffU)))
        goto invalid;
    } else if (byte >= 0xc2U && byte <= 0xdfU) {
      pending = 1U;
      scalar = byte & 0x1fU;
      minimum = 0x80U;
    } else if (byte >= 0xe0U && byte <= 0xefU) {
      pending = 2U;
      scalar = byte & 0x0fU;
      minimum = 0x800U;
    } else if (byte >= 0xf0U && byte <= 0xf4U) {
      pending = 3U;
      scalar = byte & 0x07U;
      minimum = 0x10000U;
    } else
      goto invalid;
  }
  state->pending = pending;
  state->scalar = scalar;
  state->minimum = minimum;
  return 1;
invalid:
  state->failed = 1;
  return 0;
}
int cai_utf8_complete(const cai_utf8_state *state) {
  return state != NULL && !state->failed && state->pending == 0U;
}
