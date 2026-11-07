#include "../cli/command_help.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct capture {
  char expected[32768], actual[32768];
  size_t length, calls;
  int fail;
} capture;
static int write_help(void *context, const char *data, size_t length) {
  capture *c = context;
  ++c->calls;
  if (c->fail && c->calls == 2U)
    return -1;
  assert(length < sizeof(c->expected) - c->length);
  memcpy(c->expected + c->length, data, length);
  c->length += length;
  c->expected[c->length] = '\0';
  return 0;
}
int main(void) {
  capture *c = calloc(1U, sizeof(*c));
  static const size_t sizes[] = {1U, 7U, 256U, 4096U};
  size_t i;
  int attached;
  assert(c != NULL);
  for (attached = 0; attached <= 1; ++attached) {
    memset(c, 0, sizeof(*c));
    assert(cai_cli_command_help("custom-app", attached, write_help, c) ==
           CAI_OK);
    assert(strstr(c->expected, "custom-app --help") != NULL);
    assert(strstr(c->expected, "/help -p") != NULL);
    assert((strstr(c->expected, "## Workspace connections") != NULL) ==
           attached);
    for (i = 0U; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
      cai_cli_help_reader *source = NULL;
      size_t used = 0U, n;
      int error = 0;
      assert(cai_cli_help_reader_open("custom-app", attached, &source, NULL) ==
             CAI_OK);
      assert(cai_cli_help_reader_read(source, NULL, 0U, &error) == 0U &&
             !error);
      do {
        assert(used + sizes[i] < sizeof(c->actual));
        n = cai_cli_help_reader_read(source, c->actual + used, sizes[i],
                                     &error);
        assert(!error);
        used += n;
      } while (n != 0U);
      assert(used == c->length && memcmp(c->actual, c->expected, used) == 0);
      assert(cai_cli_help_reader_read(source, c->actual, 1U, &error) == 0U &&
             !error);
      {
        size_t position;
        /* Every byte offset, including paragraph boundaries and EOF, must
         * yield the same bounded suffix as the inline document. */
        for (position = 0U; position <= c->length; ++position) {
          size_t wanted = c->length - position;
          if (wanted > 7U)
            wanted = 7U;
          assert(cai_cli_help_reader_seek(source, position) == 0);
          n = cai_cli_help_reader_read(source, c->actual, 7U, &error);
          assert(!error && n == wanted &&
                 memcmp(c->actual, c->expected + position, n) == 0);
        }
        assert(cai_cli_help_reader_seek(source, 3U) == 0);
        assert(cai_cli_help_reader_seek(source, c->length + 1U) == -1);
        assert(cai_cli_help_reader_seek(source, (size_t)-1) == -1);
        assert(cai_cli_help_reader_read(source, c->actual, 7U, &error) == 7U &&
               !error && memcmp(c->actual, c->expected + 3U, 7U) == 0);
        assert(cai_cli_help_reader_seek(source, 0U) == 0);
      }
      cai_cli_help_reader_close(source);
    }
  }
  assert(cai_cli_help_reader_seek(NULL, 0U) == -1);
  c->calls = 0U;
  c->fail = 1;
  assert(cai_cli_command_help(NULL, 0, write_help, c) == CAI_ERR_TRANSPORT);
  assert(c->calls == 2U);
  free(c);
  return 0;
}
