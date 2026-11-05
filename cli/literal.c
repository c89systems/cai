#include "literal.h"
#include "../src/cai_utf8.h"
#include <string.h>

static int literal_bytes(cai_cli_literal *state, const char *data,
                         size_t length) {
  if (state->renderer->write_literal(state->renderer, data, length) != MDF_OK) {
    state->failed = 1;
    return -1;
  }
  return 0;
}

static int literal_flush(cai_cli_literal *state) {
  if (state->used != 0U && literal_bytes(state, state->chunk, state->used) != 0)
    return -1;
  state->used = 0U;
  return 0;
}

/** Filter terminal controls without escaping or interpreting Markdown. The
 * literal API owns word decisions and newline layout across all fragments. */
static int literal_ascii(cai_cli_literal *state, unsigned char ch) {
  if (state->used == sizeof(state->chunk) && literal_flush(state) != 0)
    return -1;
  state->chunk[state->used++] =
      (ch < 32U && ch != '\n') || ch == 127U ? ' ' : (char)ch;
  return 0;
}

/** At most four bytes await a Unicode decision. Invalid leading bytes consume
 * one byte so a later valid character is not swallowed by a malformed prefix.
 */
static int literal_runes(cai_cli_literal *state, int ending) {
  while (state->pending != 0U) {
    unsigned char ch = state->rune[0];
    char rune[4];
    size_t need = ch >= 0xc2U && ch < 0xe0U    ? 2U
                  : ch >= 0xe0U && ch < 0xf0U  ? 3U
                  : ch >= 0xf0U && ch <= 0xf4U ? 4U
                                               : 1U;
    size_t consumed;
    if (!ending && state->pending < need)
      break;
    if (need > state->pending)
      need = state->pending;
    memcpy(rune, state->rune, need);
    cai_utf8_repair_text(rune, need);
    consumed = rune[0] == '?' ? 1U : need;
    if (ch < 128U) {
      if (literal_ascii(state, ch) != 0)
        return -1;
    } else {
      if (literal_flush(state) != 0)
        return -1;
      /* C1 controls are valid UTF-8 but invalid terminal feed contents. */
      if (need == 2U && (unsigned char)rune[0] == 0xc2U &&
          (unsigned char)rune[1] < 0xa0U) {
        if (literal_bytes(state, " ", 1U) != 0)
          return -1;
      } else if (literal_bytes(state, rune, consumed) != 0)
        return -1;
    }
    state->pending -= consumed;
    memmove(state->rune, state->rune + consumed, state->pending);
  }
  return 0;
}

void cai_cli_literal_init(cai_cli_literal *state, mdf *renderer) {
  memset(state, 0, sizeof(*state));
  state->renderer = renderer;
}

int cai_cli_literal_feed(cai_cli_literal *state, const void *bytes,
                         size_t length) {
  const unsigned char *data = bytes;
  size_t i;
  if (state == NULL || state->renderer == NULL || state->finished ||
      state->failed)
    return -1;
  if (bytes == NULL && length != 0U) {
    state->failed = 1;
    return -1;
  }
  for (i = 0U; i < length; ++i) {
    if (state->pending == 0U && data[i] < 128U) {
      if (literal_ascii(state, data[i]) != 0)
        return -1;
    } else {
      state->rune[state->pending++] = data[i];
      if (literal_runes(state, 0) != 0)
        return -1;
    }
  }
  return literal_flush(state);
}

int cai_cli_literal_finish(cai_cli_literal *state) {
  if (state == NULL || state->renderer == NULL || state->finished ||
      state->failed)
    return -1;
  if (literal_runes(state, 1) != 0 || literal_flush(state) != 0)
    return -1;
  state->finished = 1;
  return 0;
}

int cai_cli_literal_write(mdf *renderer, const char *text) {
  cai_cli_literal state;
  cai_cli_literal_init(&state, renderer);
  if (cai_cli_literal_feed(&state, text, strlen(text)) != 0)
    return -1;
  return cai_cli_literal_finish(&state);
}

int cai_cli_literal_render(mdf *renderer, const char *text) {
  if (cai_cli_literal_write(renderer, text) != 0)
    return -1;
  return renderer->finish(renderer) == MDF_OK ? 0 : -1;
}
