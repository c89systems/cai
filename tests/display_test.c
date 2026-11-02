#define _POSIX_C_SOURCE 200809L
#include "../src/cai_utf8.h"
#include <assert.h>
#include <cai/display.h>
#include <cai/dtg.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef CAI_TEST_DISPLAY_CLOCK
static int fake_clock, fake_clock_error;
static unsigned long long fake_milliseconds;
int __real_clock_gettime(clockid_t clock, struct timespec *now);
int __wrap_clock_gettime(clockid_t clock, struct timespec *now);
int __wrap_clock_gettime(clockid_t clock, struct timespec *now) {
  if (!fake_clock || clock != CLOCK_MONOTONIC)
    return __real_clock_gettime(clock, now);
  if (fake_clock_error)
    return -1;
  now->tv_sec = (time_t)(fake_milliseconds / 1000ULL);
  now->tv_nsec = (long)(fake_milliseconds % 1000ULL) * 1000000L;
  return 0;
}
#endif

typedef struct capture {
  char *bytes;
  size_t length;
  int fail;
  size_t fail_after;
} capture;
typedef struct input {
  const char *bytes;
  size_t length, offset, chunk;
  int fail;
  capture *output;
  int live;
} input;
static int write_capture(void *context, const void *bytes, size_t n,
                         cai_error *error) {
  capture *c = (capture *)context;
  char *next;
  (void)error;
  if (c->fail || (c->fail_after != 0U && c->length >= c->fail_after))
    return CAI_ERR_TRANSPORT;
  next = (char *)realloc(c->bytes, c->length + n + 1U);
  assert(next != NULL);
  c->bytes = next;
  memcpy(c->bytes + c->length, bytes, n);
  c->length += n;
  c->bytes[c->length] = '\0';
  return CAI_OK;
}
static size_t read_input(void *context, void *buffer, size_t capacity,
                         cai_error *error) {
  input *i = (input *)context;
  size_t n = i->length - i->offset;
  if (i->fail) {
    error->code = CAI_ERR_TRANSPORT;
    return 0U;
  }
  if (i->offset < i->length && i->output != NULL && i->output->length > 0U)
    i->live = 1;
  if (n > capacity)
    n = capacity;
  if (n > i->chunk)
    n = i->chunk;
  memcpy(buffer, i->bytes + i->offset, n);
  i->offset += n;
  return n;
}
static void reset(capture *c) {
  c->length = 0U;
  if (c->bytes != NULL)
    c->bytes[0] = '\0';
}
static int render_width(capture *c, const char *text, size_t length,
                        size_t chunk, int mode, int color, int source_failure,
                        size_t width) {
  cai_sink_callbacks sink_callbacks;
  cai_source_callbacks source_callbacks;
  cai_tool_display_config config;
  cai_source *source = NULL;
  cai_sink *sink = NULL;
  cai_error error;
  input i;
  int rc;
  memset(&i, 0, sizeof(i));
  i.bytes = text;
  i.length = length;
  i.chunk = chunk;
  i.fail = source_failure;
  i.output = c;
  memset(&sink_callbacks, 0, sizeof(sink_callbacks));
  sink_callbacks.write = write_capture;
  sink_callbacks.context = c;
  memset(&source_callbacks, 0, sizeof(source_callbacks));
  source_callbacks.read = read_input;
  source_callbacks.context = &i;
  cai_error_init(&error);
  assert(cai_sink_from_callbacks(&sink_callbacks, &sink, &error) == CAI_OK);
  assert(cai_source_from_callbacks(&source_callbacks, &source, &error) ==
         CAI_OK);
  memset(&config, 0, sizeof(config));
  config.sink = sink;
  config.mode = mode;
  config.color = color;
  rc = cai_patch_render_width(source, &config, width,
                              source_failure == 2 ? NULL : &error);
  if (rc == CAI_OK && length > 100U && chunk == 1U)
    assert(i.live);
  cai_source_close(source);
  cai_sink_close(sink);
  cai_error_cleanup(&error);
  return rc;
}
static int render(capture *c, const char *text, size_t length, size_t chunk,
                  int mode, int color, int source_failure) {
  return render_width(c, text, length, chunk, mode, color, source_failure, 0U);
}
static void numbered_patches(capture *c) {
  static const char patch[] =
      "*** Begin Patch\n*** Update File: file.c\n@@ -280 +280 @@\n "
      "same\n-old\n+abcdefghijklmnopqrstuvwxyz1234567890\n+next\n tail\n*** "
      "Add File: new.c\n+hello\n*** End Patch\n";
  static const char expected[] =
      "* Edited file.c\n 280  same\n 281 -old\n 281 "
      "+abcdefghijklmnopqrstuvwxyz\n      1234567890\n 282 +next\n 283  "
      "tail\n\n* Added new.c\n   1 +hello\n";
  cai_sink *sink = NULL;
  cai_sink_callbacks callbacks;
  cai_tool_display_config config;
  cai_tool_display *d = NULL;
  cai_agent_runtime_event e;
  cai_error error;
  size_t i;
  reset(c);
  assert(render_width(c, patch, sizeof(patch) - 1U, 1U, 0, 0, 0, 32U) ==
         CAI_OK);
  assert(strcmp(c->bytes, expected) == 0);
  reset(c);
  assert(render_width(c, patch, sizeof(patch) - 1U, 1U, 0, 1, 0, 32U) ==
         CAI_OK);
  assert(strstr(c->bytes,
                "\033[48;2;74;34;29m\033[97m 281 \033[91m-\033[97mold   "
                "                    \033[0m\n") != NULL);
  assert(strstr(c->bytes, "\033[48;2;33;58;43m\033[97m 281 "
                          "\033[92m+\033[97mabcdefghijklmnopqrstuvwxyz\033["
                          "0m\n\033[48;2;33;58;43m\033[97m     "
                          " 1234567890                \033[0m\n") != NULL);
  assert(strstr(c->bytes,
                "\033[48;2;33;58;43m\033[97m 282 \033[92m+\033[97mnext  "
                "                    \033[0m\n 283  tail\n") != NULL);
  reset(c);
  assert(render_width(c, "*** Update File: x\n@@\n+x\n",
                      strlen("*** Update File: x\n@@\n+x\n"), 1U, 0, 0, 0,
                      32U) == CAI_OK);
  assert(strstr(c->bytes, "     +x\n") != NULL); /* never invent positions */
  {
    static const char invalid_header[] =
        "*** Update File: x\n@@ -99999999999999999999999999999999 +1 "
        "@@\n+x\n@@ --1 +2 @@\n+y\n@@ -1 +2 missing\n+z\n";
    reset(c);
    assert(render_width(c, invalid_header, sizeof(invalid_header) - 1U, 1U, 0,
                        0, 0, 32U) == CAI_OK);
    assert(strstr(c->bytes, "     +x\n     +y\n     +z\n") != NULL);
  }
  reset(c);
  assert(render_width(c, "+a\033[2J\0b", 9U, 1U, 0, 1, 0, 32U) == CAI_OK);
  assert(strstr(c->bytes, "\033[91m") == NULL);
  assert(strstr(c->bytes, "\033[92m+\033[97ma\\x1b[2J\\x00b") != NULL);
  reset(c);
  assert(render_width(c, "+1234567890123456789012345界é\n",
                      strlen("+1234567890123456789012345界é\n"), 1U, 0, 0, 0,
                      32U) == CAI_OK);
  assert(strstr(c->bytes, "1234567890123456789012345\n      界é\n") != NULL);
  reset(c);
  assert(render_width(c, "+a\tb", 4U, 1U, 0, 0, 0, 32U) == CAI_OK);
  assert(strcmp(c->bytes, "     +a       b\n") == 0);
  {
    char *long_line = malloc(150002U);
    size_t j, added = 0U, plus = 0U, cell = 0U;
    int ansi = 0;
    assert(long_line != NULL);
    long_line[0] = '+';
    memset(long_line + 1U, 'a', 150000U);
    long_line[150001U] = '\0';
    reset(c);
    assert(render_width(c, long_line, 150001U, 1U, 0, 1, 0, 32U) == CAI_OK);
    for (j = 0U; j < c->length; j++) {
      unsigned char ch = (unsigned char)c->bytes[j];
      if (ch == 27U) {
        ansi = 1;
        continue;
      }
      if (ansi) {
        if (ch == 'm')
          ansi = 0;
        continue;
      }
      if (ch == 'a')
        added++;
      if (ch == '+')
        plus++;
      if (ch == '\n') {
        assert(cell == 32U);
        cell = 0U;
      } else
        cell++;
    }
    assert(added == 150000U && plus == 1U && cell == 0U);
    reset(c);
    c->fail_after = 100U;
    assert(render_width(c, long_line, 150001U, 1U, 0, 1, 0, 32U) ==
           CAI_ERR_TRANSPORT);
    c->fail_after = 0U;
    long_line[65535U] = (char)0xc3;
    long_line[65536U] = (char)0xa5;
    reset(c);
    assert(render_width(c, long_line, 150001U, 1U, 0, 1, 0, 32U) == CAI_OK);
    assert(strstr(c->bytes, "å") != NULL && strstr(c->bytes, "\\xc3") == NULL);
    free(long_line);
  }
  c->fail = 1;
  assert(render_width(c, patch, sizeof(patch) - 1U, 1U, 0, 1, 0, 32U) ==
         CAI_ERR_TRANSPORT);
  c->fail = 0;
  assert(render_width(c, patch, sizeof(patch) - 1U, 1U, 0, 1, 1, 32U) ==
         CAI_ERR_TRANSPORT);
  assert(render_width(c, patch, sizeof(patch) - 1U, 1U, 0, 0, 0, 31U) ==
         CAI_ERR_INVALID);
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.write = write_capture;
  callbacks.context = c;
  cai_error_init(&error);
  assert(cai_sink_from_callbacks(&callbacks, &sink, &error) == CAI_OK);
  memset(&config, 0, sizeof(config));
  config.sink = sink;
  assert(cai_tool_display_new(&config, &d, &error) == CAI_OK);
  assert(cai_tool_display_set_width(d, 40U, &error) == CAI_OK);
  memset(&e, 0, sizeof(e));
  e.tool_call_id = "positions";
  e.type = CAI_AGENT_EVENT_TOOL_CALL_STARTED;
  assert(cai_tool_display_event(d, &e, &error) == CAI_OK);
  e.type = CAI_AGENT_EVENT_TOOL_PATCH_LOCATIONS;
  for (i = 0U; i < strlen("280 290\n900 910\n"); i++) {
    e.data = "280 290\n900 910\n" + i;
    e.data_length = 1U;
    assert(cai_tool_display_event(d, &e, &error) == CAI_OK);
  }
  e.data = NULL;
  e.data_length = 0U;
  assert(cai_tool_display_event(d, &e, &error) == CAI_OK);
  reset(c);
  e.type = CAI_AGENT_EVENT_TOOL_PATCH;
  e.data = "*** Update File: x\n@@\n before\n-old\n+new\n@@ "
           "section\n-last\n+final\n";
  e.data_length = strlen(e.data);
  assert(cai_tool_display_event(d, &e, &error) == CAI_OK);
  e.data = NULL;
  e.data_length = 0U;
  assert(cai_tool_display_event(d, &e, &error) == CAI_OK);
  assert(strstr(c->bytes, " 290  before\n 281 -old\n 291 +new\n      ⋮\n 900 "
                          "-last\n 910 +final\n") != NULL);
  e.type = CAI_AGENT_EVENT_TOOL_CALL_STARTED;
  assert(cai_tool_display_event(d, &e, &error) == CAI_OK);
  e.type = CAI_AGENT_EVENT_TOOL_PATCH_LOCATIONS;
  e.data = "0 0\n";
  e.data_length = 4U;
  assert(cai_tool_display_event(d, &e, &error) == CAI_ERR_PROTOCOL);
  {
    static const char *bad[] = {"9999999999999999999999999999999999999 1\n",
                                "1 2 3\n", "1 2", "1 x\n"};
    size_t j;
    for (j = 0U; j < sizeof(bad) / sizeof(bad[0]); j++) {
      e.type = CAI_AGENT_EVENT_RUN_CANCELLED;
      e.data = NULL;
      e.data_length = 0U;
      assert(cai_tool_display_event(d, &e, &error) == CAI_OK);
      cai_error_cleanup(&error);
      cai_error_init(&error);
      e.type = CAI_AGENT_EVENT_TOOL_CALL_STARTED;
      assert(cai_tool_display_event(d, &e, &error) == CAI_OK);
      e.type = CAI_AGENT_EVENT_TOOL_PATCH_LOCATIONS;
      e.data = bad[j];
      e.data_length = strlen(e.data);
      if (j == 2U) {
        assert(cai_tool_display_event(d, &e, &error) == CAI_OK);
        e.data = NULL;
        e.data_length = 0U;
      }
      assert(cai_tool_display_event(d, &e, &error) == CAI_ERR_PROTOCOL);
    }
  }
  e.type = CAI_AGENT_EVENT_RUN_CANCELLED;
  e.data = NULL;
  e.data_length = 0U;
  assert(cai_tool_display_event(d, &e, &error) == CAI_OK);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  reset(c);
  e.type = CAI_AGENT_EVENT_TOOL_CALL_STARTED;
  assert(cai_tool_display_event(d, &e, &error) == CAI_OK);
  e.type = CAI_AGENT_EVENT_TOOL_PATCH;
  e.data = "*** Add File: resize.c\n+x\n";
  e.data_length = strlen(e.data);
  assert(cai_tool_display_event(d, &e, &error) == CAI_OK);
  assert(cai_tool_display_set_width(d, 32U, &error) == CAI_OK);
  e.data = "+abcdefghijklmnopqrstuvwxyz0123456789\n";
  e.data_length = strlen(e.data);
  assert(cai_tool_display_event(d, &e, &error) == CAI_OK);
  e.data = NULL;
  e.data_length = 0U;
  assert(cai_tool_display_event(d, &e, &error) == CAI_OK);
  assert(strstr(c->bytes,
                "   2 +abcdefghijklmnopqrstuvwxyz\n      0123456789\n") !=
         NULL);
  assert(cai_tool_display_set_width(NULL, 80U, &error) == CAI_ERR_INVALID);
  assert(cai_tool_display_set_width(d, 4097U, &error) == CAI_ERR_INVALID);
  cai_tool_display_close(d);
  cai_sink_close(sink);
  cai_error_cleanup(&error);
}
static void patches(capture *c) {
  static const char patch[] =
      "*** Begin Patch\n*** Update File: dir/file.c\n@@\n"
      " before0\n before1\n-old\n+new å🚀\n"
      " between0\n between1\n between2\n between3\n-old2\n+new2\n"
      " after0\n after1\n*** Add File: added.c\n+added\n"
      "*** Delete File: deleted.c\n*** End Patch\n";
  static const char expected[] =
      "* Edited dir/file.c\n@@\n before1\n-old\n+new å🚀\n"
      " between0\n ⋮\n between3\n-old2\n+new2\n after0\n"
      "\n* Added added.c\n+added\n\n* Deleted deleted.c\n";
  char *long_patch;
  size_t length, j;
  reset(c);
  assert(render(c, patch, sizeof(patch) - 1U, 1U, 0, 0, 0) == CAI_OK);
  assert(strcmp(c->bytes, expected) == 0);
  reset(c);
  assert(render(c, patch, sizeof(patch) - 1U, 7U, 0, 1, 0) == CAI_OK);
  assert(
      strstr(c->bytes,
             "\033[48;2;33;58;43m\033[97m\033[92m+\033[97mnew å🚀\033[0m\n") !=
      NULL);
  assert(strstr(c->bytes,
                "\033[48;2;74;34;29m\033[97m\033[91m-\033[97mold\033[0m\n") !=
         NULL);
  assert(strstr(c->bytes, "\033[92m*\033[0m \033[0m\033[36mEdited "
                          "\033[0;1m\033[96mdir/file.c\033[0m\n") != NULL);
  assert(strstr(c->bytes, "\n between0\n ⋮\n between3\n") != NULL);
  reset(c);
  assert(render(c, patch, sizeof(patch) - 1U, 4096U, 1, 0, 0) == CAI_OK);
  assert(strstr(c->bytes, "before0\n before1") != NULL);
  assert(strstr(c->bytes, "between1\n between2") != NULL);
  assert(strstr(c->bytes, "after0\n after1") != NULL);
  assert(strstr(c->bytes, "⋮") == NULL && strchr(c->bytes, '\033') == NULL);
  reset(c);
  assert(render(c, "*** Update File: x\n@@\n a\n-old\n+new\n z\n",
                sizeof("*** Update File: x\n@@\n a\n-old\n+new\n z\n") - 1U, 1U,
                0, 0, 0) == CAI_OK);
  reset(c);
  c->fail = 1;
  assert(render(c, patch, sizeof(patch) - 1U, 1U, 0, 0, 0) ==
         CAI_ERR_TRANSPORT);
  c->fail = 0;
  assert(render(c, patch, sizeof(patch) - 1U, 1U, 0, 0, 1) ==
         CAI_ERR_TRANSPORT);
  assert(render(c, patch, sizeof(patch) - 1U, 1U, 0, 0, 2) ==
         CAI_ERR_TRANSPORT);
  reset(c);
  assert(render(c, "+a\033[2J\0b", sizeof("+a\033[2J\0b") - 1U, 1U, 0, 0, 0) ==
         CAI_OK);
  assert(strcmp(c->bytes, "+a\\x1b[2J\\x00b\n") == 0);
  length = 150000U;
  long_patch = (char *)malloc(length + 2U);
  assert(long_patch != NULL);
  long_patch[0] = '+';
  memset(long_patch + 1, 'x', length - 1U);
  long_patch[length] = '\n';
  long_patch[length + 1U] = '\0';
  reset(c);
  assert(render(c, long_patch, length + 1U, 23U, 0, 0, 0) == CAI_OK);
  assert(c->length == length + 1U &&
         memcmp(c->bytes, long_patch, length + 1U) == 0);
  reset(c);
  assert(render(c, long_patch, length + 1U, 4096U, 1, 1, 0) == CAI_OK);
  for (j = 0U; j < c->length; j++)
    assert(c->bytes[j] != '\0');
  assert(strstr(c->bytes, "\033[48;2;33;58;43m\033[97mxxxxx") != NULL);
  reset(c);
  assert(render(c, long_patch, 65536U, 4096U, 0, 0, 0) == CAI_OK);
  assert(c->length == 65537U && c->bytes[65536U] == '\n');
  long_patch[65535] = (char)0xc3;
  long_patch[65536] = (char)0xa5;
  reset(c);
  assert(render(c, long_patch, length + 1U, 4096U, 1, 1, 0) == CAI_OK);
  assert(strstr(c->bytes, "å") != NULL && strstr(c->bytes, "\\xc3") == NULL);
  assert(strstr(c->bytes, "\xc3\033") == NULL);
  long_patch[65535] = 'x';
  long_patch[65536] = 'x';
  long_patch[0] = ' ';
  reset(c);
  assert(render(c, long_patch, length + 1U, 4096U, 0, 0, 0) == CAI_OK);
  assert(c->length < length && strstr(c->bytes, "[line clipped]") != NULL);
  reset(c);
  assert(render(c, long_patch, length + 1U, 4096U, 1, 0, 0) == CAI_OK);
  assert(c->length == length + 1U &&
         memcmp(c->bytes, long_patch, length + 1U) == 0);
  free(long_patch);
}
static void event(cai_tool_display *d, int type, const char *id,
                  const char *data, int action, int exit_code) {
  cai_agent_runtime_event e;
  cai_error error;
  memset(&e, 0, sizeof(e));
  e.type = type;
  e.tool_call_id = id;
  e.tool_action = action;
  e.runtime_session_id = "session";
  e.tool_name = "fixture";
  e.data = data;
  e.data_length = data != NULL ? strlen(data) : 0U;
  e.terminal_has_exit_code = 1;
  e.terminal_exit_code = exit_code;
  cai_error_init(&error);
  assert(cai_tool_display_event(d, &e, &error) == CAI_OK);
  cai_error_cleanup(&error);
}
static void palette_change(capture *c) {
  cai_sink_callbacks callbacks;
  cai_sink *sink = NULL;
  cai_tool_display_config config;
  cai_tool_display *display = NULL;
  cai_error error;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.write = write_capture;
  callbacks.context = c;
  memset(&config, 0, sizeof(config));
  cai_error_init(&error);
  assert(cai_sink_from_callbacks(&callbacks, &sink, &error) == CAI_OK);
  config.sink = sink;
  config.color = 1;
  assert(cai_tool_display_new(&config, &display, &error) == CAI_OK);
  event(display, CAI_AGENT_EVENT_TOOL_CALL_STARTED, "palette", NULL,
        CAI_AGENT_TOOL_ACTION_EXECUTE, 0);
  event(display, CAI_AGENT_EVENT_TERMINAL_COMMAND_STARTED, "palette",
        "palette command", 0, 0);
  event(display, CAI_AGENT_EVENT_TERMINAL_OUTPUT, "palette",
        "\033[32mone\n\xc3", 0, 0);
  assert(cai_tool_display_set_palette(display, "\033[38;5;141m",
                                      "\033[38;2;240;180;100m",
                                      &error) == CAI_OK);
  assert(cai_tool_display_set_color(display, 0, &error) == CAI_OK);
  reset(c);
  event(display, CAI_AGENT_EVENT_TERMINAL_OUTPUT, "palette", "\xa5 second\n", 0,
        0);
  assert(strstr(c->bytes, "å second\n") != NULL);
  assert(strchr(c->bytes, '\033') == NULL);
  assert(cai_tool_display_set_color(display, 1, &error) == CAI_OK);
  event(display, CAI_AGENT_EVENT_TERMINAL_COMMAND_COMPLETED, "palette",
        "three\nfour\nfive\nHIDDEN\n", 0, 0);
  assert(strstr(c->bytes, "palette command") != NULL &&
         strstr(c->bytes, "\033[92m*\033[0m") != NULL);
  assert(strstr(c->bytes, "\033[0m\033[38;5;141mRan \033[0;1m"
                          "\033[38;2;240;180;100mpalette command"
                          "\033[0m\033[38;5;141m (0ms)\033[0m\n") != NULL);
  assert(strstr(c->bytes, "HIDDEN") == NULL); /* Preserve the preview budget. */
  assert(cai_tool_display_set_color(display, 2, &error) == CAI_ERR_INVALID);
  assert(cai_tool_display_set_color(NULL, 1, &error) == CAI_ERR_INVALID);
  cai_tool_display_close(display);
  cai_sink_close(sink);
  cai_error_cleanup(&error);
}

static void palette_validation(capture *c) {
  static const char *const valid[] = {"",
                                      "\033[30m",
                                      "\033[37m",
                                      "\033[90m",
                                      "\033[97m",
                                      "\033[39m",
                                      "\033[38;5;0m",
                                      "\033[38;5;255m",
                                      "\033[38;2;0;128;255m"};
  static const char *const invalid[] = {
      NULL,
      "plain",
      "\033[0m",
      "\033[1;31m",
      "\033[48;5;3m",
      "\033[31mtext",
      "\033[38;5;256m",
      "\033[38;2;1;2m",
      "\033[38;2;1;2;256m",
      "\033[38;2;-1;2;3m",
      "\033[31m\033[32m",
      "\033]title\007",
      "\033[38;5;99999999999999999999999999999m"};
  cai_tool_display *display = NULL;
  cai_tool_display_config config;
  cai_sink_callbacks callbacks;
  cai_sink *sink = NULL;
  cai_error error;
  char expected[192], owned[32] = "\033[38;5;123m";
  size_t i;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.write = write_capture;
  callbacks.context = c;
  memset(&config, 0, sizeof(config));
  cai_error_init(&error);
  assert(cai_sink_from_callbacks(&callbacks, &sink, &error) == CAI_OK);
  config.sink = sink;
  config.color = 1;
  assert(cai_tool_display_new(&config, &display, &error) == CAI_OK);
  for (i = 0U; i < sizeof(valid) / sizeof(valid[0]); i++) {
    reset(c);
    assert(cai_tool_display_set_palette(display, valid[i], valid[i], &error) ==
           CAI_OK);
    event(display, CAI_AGENT_EVENT_TOOL_CALL_COMPLETED, "palette", NULL,
          CAI_AGENT_TOOL_ACTION_READ, 0);
    snprintf(expected, sizeof(expected),
             "\033[92m*\033[0m \033[0m%sRead "
             "\033[0;1m%sfixture\033[0m\n",
             valid[i], valid[i]);
    assert(strstr(c->bytes, expected) != NULL);
  }
  assert(cai_tool_display_set_palette(display, owned, "\033[34m", &error) ==
         CAI_OK);
  memset(owned, 'x', sizeof(owned)); /* The renderer owns its copy. */
  for (i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
    reset(c);
    assert(cai_tool_display_set_palette(display, invalid[i], "\033[31m",
                                        &error) == CAI_ERR_INVALID);
    assert(cai_tool_display_set_palette(display, "\033[31m", invalid[i],
                                        &error) == CAI_ERR_INVALID);
    event(display, CAI_AGENT_EVENT_TERMINAL_COMMAND_STARTED, "palette",
          "command", 0, 0);
    event(display, CAI_AGENT_EVENT_TERMINAL_OUTPUT, "palette", "output\n", 0,
          0);
    assert(strstr(c->bytes, "\033[90m*\033[0m \033[0m\033[38;5;123mRunning "
                            "\033[0;1m\033[34mcommand\033[0m\n") != NULL);
    event(display, CAI_AGENT_EVENT_TERMINAL_COMMAND_COMPLETED, "palette", NULL,
          0, 0);
  }
  assert(cai_tool_display_set_palette(NULL, "", "", &error) == CAI_ERR_INVALID);
  assert(cai_tool_display_set_color(display, 0, &error) == CAI_OK);
  reset(c);
  event(display, CAI_AGENT_EVENT_TOOL_CALL_COMPLETED, "plain", NULL,
        CAI_AGENT_TOOL_ACTION_READ, 0);
  assert(strstr(c->bytes, "* Read fixture\n") != NULL &&
         strchr(c->bytes, '\033') == NULL);
  cai_tool_display_close(display);
  cai_sink_close(sink);
  cai_error_cleanup(&error);
}

static void commands(capture *c) {
  cai_sink_callbacks callbacks;
  cai_sink *sink;
  cai_tool_display_config config;
  cai_tool_display *d;
  cai_error error;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.write = write_capture;
  callbacks.context = c;
  cai_error_init(&error);
  assert(cai_sink_from_callbacks(&callbacks, &sink, &error) == CAI_OK);
  memset(&config, 0, sizeof(config));
  config.sink = sink;
  config.color = 1;
  assert(cai_tool_display_new(&config, &d, &error) == CAI_OK);
  reset(c);
  event(d, CAI_AGENT_EVENT_TOOL_CALL_STARTED, "one", NULL,
        CAI_AGENT_TOOL_ACTION_EXECUTE, 0);
  event(d, CAI_AGENT_EVENT_TERMINAL_COMMAND_STARTED, "one", "printf args", 0,
        0);
  assert(c->length == 0U);
  event(d, CAI_AGENT_EVENT_TERMINAL_OUTPUT, "one", "1\n2", 0, 0);
  assert(strstr(c->bytes,
                "\033[0m\033[36mRunning \033[0;1m\033[96mprintf args\033[0m") !=
         NULL);
  assert(strstr(c->bytes, "1\n2") != NULL); /* Visible before completion. */
  assert(cai_tool_display_set_mode(d, CAI_TOOL_OUTPUT_FULL, &error) == CAI_OK);
  event(d, CAI_AGENT_EVENT_TERMINAL_OUTPUT, "one",
        "\n3\n4\n5\nHIDDEN6\nHIDDEN7\n", 0, 0);
  event(d, CAI_AGENT_EVENT_TERMINAL_COMMAND_COMPLETED, "one", NULL, 0, 0);
  assert(strstr(c->bytes, "HIDDEN") == NULL);
  assert(
      strstr(c->bytes,
             "\033[92m*\033[0m \033[0m\033[36mRan \033[0;1m\033[96mprintf args"
             "\033[0m\033[36m (0ms)\033[0m\n") != NULL);
  reset(c);
  event(d, CAI_AGENT_EVENT_TOOL_CALL_STARTED, "two", NULL,
        CAI_AGENT_TOOL_ACTION_EXECUTE, 0);
  event(d, CAI_AGENT_EVENT_TERMINAL_COMMAND_STARTED, "two", "exit 3", 0, 0);
  event(d, CAI_AGENT_EVENT_TERMINAL_OUTPUT, "two", "1\n2\n3\n4\n5\nSIX\nSEVEN",
        0, 0);
  event(d, CAI_AGENT_EVENT_TERMINAL_COMMAND_COMPLETED, "two", NULL, 0, 3);
  assert(strstr(c->bytes, "SIX\nSEVEN\n") != NULL);
  assert(
      strstr(c->bytes,
             "\033[91m*\033[0m \033[0m\033[36mRan "
             "\033[0;1m\033[96mexit 3\033[0m\033[36m (0ms)\033[0m\n  exit=3") !=
      NULL);
  reset(c);
  event(d, CAI_AGENT_EVENT_TOOL_CALL_STARTED, "quick", NULL,
        CAI_AGENT_TOOL_ACTION_EXECUTE, 0);
  event(d, CAI_AGENT_EVENT_TERMINAL_COMMAND_STARTED, "quick", "quick command",
        0, 0);
  event(d, CAI_AGENT_EVENT_TERMINAL_OUTPUT, "quick", "split\r", 0, 0);
  event(d, CAI_AGENT_EVENT_TERMINAL_COMMAND_COMPLETED, "quick", "\nfinal\r\n",
        0, 0);
  assert(strstr(c->bytes, "split\nfinal\n") != NULL);
  assert(strstr(c->bytes, "\\x0d") == NULL);
  reset(c);
  {
    cai_agent_runtime_event e;
    event(d, CAI_AGENT_EVENT_TOOL_CALL_STARTED, "signal", NULL,
          CAI_AGENT_TOOL_ACTION_EXECUTE, 0);
    event(d, CAI_AGENT_EVENT_TERMINAL_COMMAND_STARTED, "signal",
          "signal command", 0, 0);
    memset(&e, 0, sizeof(e));
    e.type = CAI_AGENT_EVENT_TERMINAL_COMMAND_CANCELLED;
    e.tool_call_id = "signal";
    e.runtime_session_id = "session";
    e.terminal_has_signal = 1;
    e.terminal_signal = 15;
    e.terminal_output_truncated = 1;
    assert(cai_tool_display_event(d, &e, &error) == CAI_OK);
    assert(
        strstr(
            c->bytes,
            "\033[91m*\033[0m \033[0m\033[36mCancelled "
            "\033[0;1m\033[96msignal command\033[0m\033[36m (0ms)\033[0m\n") !=
        NULL);
    assert(strstr(c->bytes, "exit=-1 signal=15") != NULL);
    assert(strstr(c->bytes, "truncated by the tool") != NULL);
  }
  reset(c);
  event(d, CAI_AGENT_EVENT_TOOL_CALL_STARTED, "utf8", NULL,
        CAI_AGENT_TOOL_ACTION_EXECUTE, 0);
  event(d, CAI_AGENT_EVENT_TERMINAL_COMMAND_STARTED, "utf8", "binary command",
        0, 0);
  event(d, CAI_AGENT_EVENT_TERMINAL_OUTPUT, "utf8", "split \xc3", 0, 0);
  event(d, CAI_AGENT_EVENT_TERMINAL_OUTPUT, "utf8",
        "\xa5 \xff \xc2\x80 \xed\xa0\x80 \xc3", 0, 0);
  event(d, CAI_AGENT_EVENT_TERMINAL_COMMAND_COMPLETED, "utf8", NULL, 0, 0);
  assert(strstr(c->bytes, "split å \\xff  \\xed\\xa0\\x80 \\xc3\n") != NULL);
  reset(c);
  event(d, CAI_AGENT_EVENT_TOOL_CALL_STARTED, "mcp", NULL,
        CAI_AGENT_TOOL_ACTION_MCP, 0);
  event(d, CAI_AGENT_EVENT_TOOL_CALL_COMPLETED, "mcp", "MCP result",
        CAI_AGENT_TOOL_ACTION_MCP, 0);
  assert(strstr(c->bytes,
                "*\033[0m \033[0m\033[36mMCP "
                "\033[0;1m\033[96mfixture\033[0m\nMCP result") != NULL);
  reset(c);
  event(d, CAI_AGENT_EVENT_TOOL_CALL_STARTED, "mcperror", NULL,
        CAI_AGENT_TOOL_ACTION_MCP, 0);
  event(d, CAI_AGENT_EVENT_TOOL_CALL_COMPLETED, "mcperror",
        "{\"content\":[{\"text\":\"failed\"}],\"isError\":true}",
        CAI_AGENT_TOOL_ACTION_MCP, 0);
  assert(strstr(c->bytes, "\033[91m*\033[0m \033[0m\033[36mMCP failed "
                          "\033[0;1m\033[96mfixture") != NULL);
  reset(c);
  event(d, CAI_AGENT_EVENT_TOOL_CALL_STARTED, "mcpsuccess", NULL,
        CAI_AGENT_TOOL_ACTION_MCP, 0);
  event(d, CAI_AGENT_EVENT_TOOL_CALL_COMPLETED, "mcpsuccess",
        "{\"content\":[{\"text\":\"isError: true\"}],\"isError\":false}",
        CAI_AGENT_TOOL_ACTION_MCP, 0);
  assert(
      strstr(c->bytes,
             "\033[92m*\033[0m \033[0m\033[36mMCP \033[0;1m\033[96mfixture") !=
      NULL);
  reset(c);
  event(d, CAI_AGENT_EVENT_TOOL_CALL_STARTED, "chrome", NULL,
        CAI_AGENT_TOOL_ACTION_CHROME, 0);
  event(d, CAI_AGENT_EVENT_TOOL_CALL_COMPLETED, "chrome", "page snapshot",
        CAI_AGENT_TOOL_ACTION_CHROME, 0);
  assert(strstr(c->bytes, "\033[92m*\033[0m \033[0m\033[36mUsed Chrome "
                          "\033[0;1m\033[96mfixture") != NULL);
  reset(c);
  event(d, CAI_AGENT_EVENT_TOOL_CALL_STARTED, "chromeerror", NULL,
        CAI_AGENT_TOOL_ACTION_CHROME, 0);
  event(d, CAI_AGENT_EVENT_TOOL_CALL_FAILED, "chromeerror", "Debugger detached",
        CAI_AGENT_TOOL_ACTION_CHROME, 0);
  assert(
      strstr(
          c->bytes,
          "\033[91m*\033[0m \033[0m\033[36mFailed \033[0;1m\033[96mfixture") !=
      NULL);
  assert(strstr(c->bytes, "Debugger detached") != NULL);
  reset(c);
  event(d, CAI_AGENT_EVENT_TOOL_CALL_STARTED, "badpatch", NULL,
        CAI_AGENT_TOOL_ACTION_PATCH, 0);
  event(d, CAI_AGENT_EVENT_TOOL_CALL_FAILED, "badpatch", "context mismatch",
        CAI_AGENT_TOOL_ACTION_PATCH, 0);
  assert(
      strstr(
          c->bytes,
          "\033[91m*\033[0m \033[0m\033[36mFailed \033[0;1m\033[96mfixture") !=
      NULL);
  assert(strstr(c->bytes, "Edited") == NULL);
  event(d, CAI_AGENT_EVENT_TOOL_CALL_STARTED, "cancel", NULL,
        CAI_AGENT_TOOL_ACTION_EXECUTE, 0);
  event(d, CAI_AGENT_EVENT_RUN_CANCELLED, "cancel", NULL, 0, 0);
  event(d, CAI_AGENT_EVENT_TOOL_CALL_STARTED, "cancel", NULL,
        CAI_AGENT_TOOL_ACTION_EXECUTE, 0);
  event(d, CAI_AGENT_EVENT_RUN_FAILED, "cancel", NULL, 0, 0);
  {
    cai_agent_runtime_event e;
    memset(&e, 0, sizeof(e));
    e.type = CAI_AGENT_EVENT_TOOL_PATCH;
    e.tool_call_id = "missing";
    e.runtime_session_id = "session";
    assert(cai_tool_display_event(d, &e, &error) == CAI_ERR_INVALID);
    cai_error_cleanup(&error);
    cai_error_init(&error);
    event(d, CAI_AGENT_EVENT_TOOL_CALL_STARTED, "incomplete", NULL,
          CAI_AGENT_TOOL_ACTION_EXECUTE, 0);
    e.type = CAI_AGENT_EVENT_TOOL_CALL_STARTED;
    e.tool_call_id = "incomplete";
    assert(cai_tool_display_event(d, &e, &error) == CAI_ERR_INVALID);
    cai_error_cleanup(&error);
    cai_error_init(&error);
    e.type = CAI_AGENT_EVENT_TERMINAL_COMMAND_COMPLETED;
    e.tool_call_id = "incomplete";
    assert(cai_tool_display_event(d, &e, &error) == CAI_ERR_INVALID);
    cai_error_cleanup(&error);
    cai_error_init(&error);
  }
  reset(c);
  {
    cai_agent_runtime_event e;
    memset(&e, 0, sizeof(e));
    e.tool_call_id = "shared";
    e.tool_name = "file_reader";
    e.type = CAI_AGENT_EVENT_TOOL_CALL_STARTED;
    e.runtime_session_id = "child-one";
    assert(cai_tool_display_event(d, &e, &error) == CAI_OK);
    e.runtime_session_id = "child-two";
    assert(cai_tool_display_event(d, &e, &error) == CAI_OK);
    e.type = CAI_AGENT_EVENT_TOOL_CALL_COMPLETED;
    e.tool_action = CAI_AGENT_TOOL_ACTION_READ;
    e.tool_path = "one.c";
    e.runtime_session_id = "child-one";
    assert(cai_tool_display_event(d, &e, &error) == CAI_OK);
    e.tool_path = "two.c";
    e.runtime_session_id = "child-two";
    assert(cai_tool_display_event(d, &e, &error) == CAI_OK);
    assert(strstr(c->bytes, "Read \033[0;1m\033[96mone.c") != NULL &&
           strstr(c->bytes, "Read \033[0;1m\033[96mtwo.c") != NULL);
  }
  assert(cai_tool_display_set_mode(d, 999, &error) == CAI_ERR_INVALID);
  cai_error_cleanup(&error);
  cai_tool_display_close(d);
  cai_sink_close(sink);
}
static void utf8_text(void) {
  static const char *const invalid[] = {"\xff",
                                        "\x80",
                                        "\xc0\xaf",
                                        "\xe0\x80\x80",
                                        "\xed\xa0\x80",
                                        "\xf0\x80\x80\x80",
                                        "\xf4\x90\x80\x80",
                                        "\xf0\x9f",
                                        "\xc3"};
  static const char valid[] =
      "ASCII å🚀€ \xe0\xa0\x80 \xed\x9f\xbf \xf4\x8f\xbf\xbf";
  char bytes[128];
  size_t i, j, n;
  strcpy(bytes, valid);
  cai_utf8_repair_text(bytes, strlen(bytes));
  assert(strcmp(bytes, valid) == 0);
  for (i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
    n = strlen(invalid[i]);
    strcpy(bytes, invalid[i]);
    cai_utf8_repair_text(bytes, n);
    for (j = 0U; j < n; j++)
      assert(bytes[j] == '?');
    assert(bytes[n] == '\0');
  }
}
static void command_data(cai_tool_display *d, int type, const char *id,
                         const char *data, size_t length) {
  cai_agent_runtime_event e;
  cai_error error;
  memset(&e, 0, sizeof(e));
  e.type = type;
  e.tool_call_id = id;
  e.runtime_session_id = "session";
  e.data = data;
  e.data_length = length;
  e.terminal_has_exit_code = 1;
  cai_error_init(&error);
  assert(cai_tool_display_event(d, &e, &error) == CAI_OK);
  cai_error_cleanup(&error);
}
/* Strip only SGR emitted by the renderer. Any other actual terminal escape is
 * a test failure rather than something this assertion silently sanitizes. */
static void without_sgr(char *out, const char *bytes) {
  while (*bytes != '\0') {
    if (*bytes != '\033') {
      *out++ = *bytes++;
      continue;
    }
    assert(*++bytes == '[');
    bytes++;
    while ((*bytes >= '0' && *bytes <= '9') || *bytes == ';' || *bytes == ':')
      bytes++;
    assert(*bytes++ == 'm');
  }
  *out = '\0';
}
static void command_control_case(const char *bytes, size_t length,
                                 const char *expected) {
  capture c;
  cai_sink_callbacks callbacks;
  cai_tool_display_config config;
  cai_tool_display *d;
  cai_sink *sink;
  cai_error error;
  char plain[8192];
  const char *body, *heading;
  size_t split, expected_length = strlen(expected);
  int color;
  memset(&c, 0, sizeof(c));
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.write = write_capture;
  callbacks.context = &c;
  cai_error_init(&error);
  assert(cai_sink_from_callbacks(&callbacks, &sink, &error) == CAI_OK);
  for (color = 0; color <= 1; color++) {
    memset(&config, 0, sizeof(config));
    config.sink = sink;
    config.color = color;
    config.mode = CAI_TOOL_OUTPUT_FULL;
    for (split = 0U; split <= length; split++) {
      assert(cai_tool_display_new(&config, &d, &error) == CAI_OK);
      event(d, CAI_AGENT_EVENT_TERMINAL_COMMAND_STARTED, "one", "command", 0,
            0);
      reset(&c);
      command_data(d, CAI_AGENT_EVENT_TERMINAL_OUTPUT, "one", bytes, split);
      if (color && c.length != 0U && strchr(c.bytes, '\033') != NULL)
        assert(strncmp(strrchr(c.bytes, '\033'), "\033[0m", 4U) == 0);
      command_data(d, CAI_AGENT_EVENT_TERMINAL_COMMAND_COMPLETED, "one",
                   bytes + split, length - split);
      assert(c.length < sizeof(plain));
      if (!color)
        assert(strchr(c.bytes, '\033') == NULL);
      without_sgr(plain, c.bytes);
      heading = split == 0U ? "* Ran command (0ms)\n" : "* Running command\n";
      assert(strncmp(plain, heading, strlen(heading)) == 0);
      body = plain + strlen(heading);
      if (strncmp(body, expected, expected_length) != 0 ||
          strcmp(body + expected_length,
                 split == 0U ? "\n" : "\n\n* Ran command (0ms)\n") != 0) {
        fprintf(stderr,
                "command controls color=%d split=%lu: [%s], expected [%s]\n",
                color, (unsigned long)split, plain, expected);
        abort();
      }
      assert(strstr(c.bytes, "\\x1b") == NULL);
      cai_tool_display_close(d);
    }
  }
  cai_sink_close(sink);
  cai_error_cleanup(&error);
  free(c.bytes);
}
#define CONTROL_CASE(input, expected)                                          \
  command_control_case(input, sizeof(input) - 1U, expected)
static void command_controls(void) {
  capture c;
  cai_sink_callbacks callbacks;
  cai_tool_display_config config;
  cai_tool_display *d;
  cai_sink *sink;
  cai_error error;
  char oversized[320];
  size_t i;
  CONTROL_CASE("## "
               "\033[32mfeature/cai-agent\033[m...\033[31morigin/feature/"
               "cai-agent\033[m\n"
               "\033[0m\033[35m/home/mike/g/cai/README.md\033[0m",
               "## "
               "feature/cai-agent...origin/feature/cai-agent\n/home/mike/g/cai/"
               "README.md");
  CONTROL_CASE("\033[1;31mred\033[22;39m normal\033[38;5;202m orange"
               "\033[48;2;1;2;3m background\033[0m END",
               "red normal orange background END");
  CONTROL_CASE("\033[4:3;38:2::1:2:3munderlined\033[m END", "underlined END");
  CONTROL_CASE(
      "\033]0;hidden\n\xc3\x9c title\007visible\033]8;;https://hidden\033\\"
      "link\033]8;;\033\\\033P\xe2\x9c\x93 hidden\033\\"
      "\033_\xf0\x9f\x9c\x80 hidden\033\\ END",
      "visiblelink END");
  CONTROL_CASE("\x9dhidden\x9c\xc2\x9b"
               "32mgreen\xc2\x9b"
               "0m\033]hidden\xc2\x9c END",
               "green END");
  CONTROL_CASE("\033]hidden\033\xc3\x9c\\still hidden\007END", "END");
  CONTROL_CASE("\033]52;c;hidden clipboard\007\033Xhidden SOS\033\\"
               "\033^hidden PM\033\\END",
               "END");
  CONTROL_CASE("\033]hidden\xe0\x80\x9c still hidden\007END", "END");
  CONTROL_CASE("\033]hidden\xe2\007END", "END");
  CONTROL_CASE("before\033[2J\033[H\033[?25l\033[3A\033(Bafter\0\007\b\177 END",
               "beforeafter END");
  CONTROL_CASE("progress\rnext\r\nlast", "progress\nnext\nlast");
  CONTROL_CASE("€🚀å\033[31mÜ\033[0m END", "€🚀åÜ END");
  CONTROL_CASE("invalid\xff\xed\xa0\x80\xc3",
               "invalid\\xff\\xed\\xa0\\x80\\xc3");
  CONTROL_CASE("keep\033[31", "keep");
  CONTROL_CASE("keep\033]unfinished", "keep");
  CONTROL_CASE("keep\033[?31mEND", "keepEND");
  CONTROL_CASE("keep\033[1;38;2;3mEND", "keepEND");
  CONTROL_CASE("keep\033[31\030END", "keepEND");
  memcpy(oversized, "keep\033[", 6U);
  memset(oversized + 6U, '1', sizeof(oversized) - 11U);
  memcpy(oversized + sizeof(oversized) - 5U, "mEND", 5U);
  command_control_case(oversized, sizeof(oversized) - 1U, "keepEND");

  memset(&c, 0, sizeof(c));
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.write = write_capture;
  callbacks.context = &c;
  cai_error_init(&error);
  assert(cai_sink_from_callbacks(&callbacks, &sink, &error) == CAI_OK);
  memset(&config, 0, sizeof(config));
  config.sink = sink;
  config.color = 1;
  config.preview_lines = 2U;
  assert(cai_tool_display_new(&config, &d, &error) == CAI_OK);
  event(d, CAI_AGENT_EVENT_TERMINAL_COMMAND_STARTED, "a", "first", 0, 0);
  reset(&c);
  event(d, CAI_AGENT_EVENT_TERMINAL_OUTPUT, "a", "\033[1;31mRED", 0, 0);
  assert(strstr(c.bytes, "\033[1m\033[31mRED\033[0m") != NULL);
  reset(&c);
  event(d, CAI_AGENT_EVENT_TERMINAL_COMMAND_STARTED, "b", "second", 0, 0);
  event(d, CAI_AGENT_EVENT_TERMINAL_OUTPUT, "b", "PLAIN", 0, 0);
  assert(strstr(c.bytes, "\033[31m") == NULL);
  reset(&c);
  event(d, CAI_AGENT_EVENT_TERMINAL_OUTPUT, "a", "STILL RED", 0, 0);
  assert(strcmp(c.bytes, "\033[1m\033[31mSTILL RED\033[0m") == 0);
  reset(&c);
  event(d, CAI_AGENT_EVENT_TERMINAL_OUTPUT, "a", "\033[22;39mNORMAL", 0, 0);
  assert(strstr(c.bytes, "\033[31m") == NULL &&
         strstr(c.bytes, "NORMAL") != NULL);
  /* Repeated style changes never grow retained state or hide reset handling. */
  for (i = 0U; i < 1000U; i++)
    event(d, CAI_AGENT_EVENT_TERMINAL_OUTPUT, "a", "\033[38;2;1;2;3m", 0, 0);
  reset(&c);
  event(d, CAI_AGENT_EVENT_TERMINAL_OUTPUT, "a", "RGB", 0, 0);
  assert(strcmp(c.bytes, "\033[38;2;1;2;3mRGB\033[0m") == 0);
  reset(&c);
  event(d, CAI_AGENT_EVENT_TERMINAL_OUTPUT, "a", "\033[38;2;999;0;0mBAD", 0, 0);
  assert(strcmp(c.bytes, "\033[38;2;1;2;3mBAD\033[0m") == 0);
  reset(&c);
  event(d, CAI_AGENT_EVENT_TERMINAL_OUTPUT, "a", "\xc3", 0, 0);
  assert(c.length == 0U);
  event(d, CAI_AGENT_EVENT_TERMINAL_OUTPUT, "b", "PLAIN", 0, 0);
  assert(strcmp(c.bytes, "PLAIN") == 0);
  reset(&c);
  event(d, CAI_AGENT_EVENT_TERMINAL_OUTPUT, "a", "\xa5", 0, 0);
  assert(strcmp(c.bytes, "\033[38;2;1;2;3må\033[0m") == 0);
  event(d, CAI_AGENT_EVENT_TERMINAL_OUTPUT, "a",
        "\033[1;2;3;4:3;5;7;8;9;11;51;53;73;26;38;5;9;48;2;1;2;3;58:2::4:5:6m"
        "ATTRIBUTES",
        0, 0);
  reset(&c);
  event(d, CAI_AGENT_EVENT_TERMINAL_OUTPUT, "a",
        "\033[22;23;24;25;27;28;29;10;54;55;75;39;49;59;50mPLAIN", 0, 0);
  assert(strcmp(c.bytes, "PLAIN") == 0);
  event(d, CAI_AGENT_EVENT_TERMINAL_COMMAND_COMPLETED, "a", NULL, 0, 0);
  event(d, CAI_AGENT_EVENT_TERMINAL_COMMAND_COMPLETED, "b", NULL, 0, 0);
  event(d, CAI_AGENT_EVENT_TERMINAL_COMMAND_STARTED, "preview", "preview", 0,
        0);
  reset(&c);
  event(d, CAI_AGENT_EVENT_TERMINAL_OUTPUT, "preview",
        "\033]hidden\n\n\007\033[32mfirst\nsecond\nHIDDEN\033[31m", 0, 0);
  assert(strstr(c.bytes, "first\nsecond\n\033[0m ⋮") != NULL);
  assert(strstr(c.bytes, "hidden") == NULL &&
         strstr(c.bytes, "HIDDEN") == NULL);
  assert(strstr(c.bytes, "\033[31m") == NULL);
  event(d, CAI_AGENT_EVENT_TERMINAL_COMMAND_COMPLETED, "preview", NULL, 0, 0);
  event(d, CAI_AGENT_EVENT_TERMINAL_COMMAND_STARTED, "failure", "failure", 0,
        0);
  c.fail = 1;
  {
    cai_agent_runtime_event e;
    memset(&e, 0, sizeof(e));
    e.type = CAI_AGENT_EVENT_TERMINAL_OUTPUT;
    e.tool_call_id = "failure";
    e.runtime_session_id = "session";
    e.data = "\033[31mtext";
    e.data_length = strlen(e.data);
    assert(cai_tool_display_event(d, &e, &error) == CAI_ERR_TRANSPORT);
  }
  cai_tool_display_close(d);
  cai_sink_close(sink);
  cai_error_cleanup(&error);
  free(c.bytes);
}
static void command_durations(capture *c) {
  static const unsigned long long durations[] = {
      0U, 1U, 999U, 1000U, 1001U, 59999U, 60000U, 63000U, 3600000U, 5025000U};
  static const char *const elapsed[] = {"0ms",    "1ms",     "999ms", "1s",
                                        "1s",     "59s",     "1m0s",  "1m3s",
                                        "1h0m0s", "1h23m45s"};
  cai_sink_callbacks callbacks;
  cai_sink *sink = NULL;
  cai_tool_display_config config;
  cai_tool_display *display = NULL;
  cai_agent_runtime_event e;
  cai_error error;
  size_t i;
  int replay, color, cancelled;
  char expected[256];
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.write = write_capture;
  callbacks.context = c;
  cai_error_init(&error);
  assert(cai_sink_from_callbacks(&callbacks, &sink, &error) == CAI_OK);
  for (color = 0; color < 2; color++) {
    for (cancelled = 0; cancelled < 2; cancelled++) {
      for (replay = 0; replay < 2; replay++) {
        for (i = 0U; i < sizeof(durations) / sizeof(durations[0]); i++) {
          reset(c);
          memset(&config, 0, sizeof(config));
          config.sink = sink;
          config.color = color;
          assert(cai_tool_display_new(&config, &display, &error) == CAI_OK);
          memset(&e, 0, sizeof(e));
          e.type = CAI_AGENT_EVENT_TOOL_CALL_COMPLETED;
          e.tool_call_id = "duration";
          e.runtime_session_id = "session";
          e.terminal_duration_ms = durations[i];
          if (replay)
            assert(cai_tool_display_replay_begin(
                       display, &e, cancelled ? "Cancelled" : "Ran", "command",
                       !cancelled, &error) == CAI_OK);
          else {
            event(display, CAI_AGENT_EVENT_TERMINAL_COMMAND_STARTED, "duration",
                  "command", 0, 0);
            e.type = cancelled ? CAI_AGENT_EVENT_TERMINAL_COMMAND_CANCELLED
                               : CAI_AGENT_EVENT_TERMINAL_COMMAND_COMPLETED;
            e.terminal_has_exit_code = 1;
            assert(cai_tool_display_event(display, &e, &error) == CAI_OK);
          }
          snprintf(expected, sizeof(expected),
                   color
                       ? "\033[0m\033[36m%s "
                         "\033[0;1m\033[96mcommand\033[0m\033[36m (%s)\033[0m\n"
                       : "%s command (%s)\n",
                   cancelled ? "Cancelled" : "Ran", elapsed[i]);
          assert(strstr(c->bytes, expected) != NULL);
          assert(strstr(c->bytes, " for ") == NULL);
          assert(strstr(c->bytes, " after ") == NULL);
          cai_tool_display_close(display);
        }
      }
    }
  }
  cai_sink_close(sink);
  cai_error_cleanup(&error);
}

static void command_delay(capture *c) {
#ifdef CAI_TEST_DISPLAY_CLOCK
  cai_tool_display_config config;
  cai_tool_display *display = NULL;
  cai_sink_callbacks callbacks;
  cai_sink *sink = NULL;
  cai_error error;
  cai_agent_runtime_event saved;
  const char *heading;
  size_t length;
  memset(&config, 0, sizeof(config));
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.write = write_capture;
  callbacks.context = c;
  cai_error_init(&error);
  assert(cai_sink_from_callbacks(&callbacks, &sink, &error) == CAI_OK);
  config.sink = sink;
  assert(cai_tool_display_new(&config, &display, &error) == CAI_OK);
  fake_clock = 1;
  fake_milliseconds = 10000ULL;
  assert(!cai_tool_display_needs_tick(NULL) &&
         !cai_tool_display_needs_tick(display));
  reset(c);
  event(display, CAI_AGENT_EVENT_TERMINAL_COMMAND_STARTED, "fast", "true", 0,
        0);
  assert(cai_tool_display_needs_tick(display));
  assert(c->length == 0U);
  event(display, CAI_AGENT_EVENT_TERMINAL_OUTPUT, "fast", NULL, 0, 0);
  fake_milliseconds += 1999ULL;
  assert(cai_tool_display_tick(display, &error) == CAI_OK && c->length == 0U);
  event(display, CAI_AGENT_EVENT_TERMINAL_COMMAND_COMPLETED, "fast", NULL, 0,
        0);
  assert(!cai_tool_display_needs_tick(display));
  assert(strstr(c->bytes, "Ran true (0ms)") != NULL &&
         strstr(c->bytes, "Running") == NULL);
  length = c->length;
  fake_milliseconds += 10000ULL;
  assert(cai_tool_display_tick(display, &error) == CAI_OK &&
         c->length == length);

  reset(c);
  fake_milliseconds = 30000ULL;
  event(display, CAI_AGENT_EVENT_TERMINAL_COMMAND_STARTED, "slow", "quiet", 0,
        0);
  fake_milliseconds = 29000ULL; /* A backward clock must not underflow. */
  assert(cai_tool_display_tick(display, &error) == CAI_OK && c->length == 0U);
  fake_milliseconds = 31999ULL;
  assert(cai_tool_display_tick(display, &error) == CAI_OK && c->length == 0U);
  fake_milliseconds = 32000ULL;
  assert(cai_tool_display_tick(display, &error) == CAI_OK);
  assert(strstr(c->bytes, "Running quiet") != NULL);
  assert(!cai_tool_display_needs_tick(display));
  length = c->length;
  fake_milliseconds++;
  assert(cai_tool_display_tick(display, &error) == CAI_OK &&
         c->length == length);
  event(display, CAI_AGENT_EVENT_TERMINAL_OUTPUT, "slow", "LIVE\n", 0, 0);
  heading = strstr(c->bytes, "Running");
  assert(heading != NULL && strstr(c->bytes, "LIVE") != NULL &&
         heading < strstr(c->bytes, "LIVE") &&
         strstr(heading + 1, "Running") == NULL);
  event(display, CAI_AGENT_EVENT_TERMINAL_COMMAND_COMPLETED, "slow", NULL, 0,
        0);
  assert(strstr(c->bytes, "Ran quiet") != NULL);

  reset(c);
  event(display, CAI_AGENT_EVENT_TERMINAL_COMMAND_STARTED, "output", "prints",
        0, 0);
  assert(c->length == 0U);
  event(display, CAI_AGENT_EVENT_TERMINAL_OUTPUT, "output", "FIRST\n", 0, 0);
  assert(!cai_tool_display_needs_tick(display));
  assert(strstr(c->bytes, "Running prints\nFIRST\n") != NULL);
  length = c->length;
  fake_milliseconds += 3000ULL;
  assert(cai_tool_display_tick(display, &error) == CAI_OK &&
         c->length == length);
  event(display, CAI_AGENT_EVENT_TERMINAL_COMMAND_COMPLETED, "output", NULL, 0,
        0);

  reset(c);
  event(display, CAI_AGENT_EVENT_TERMINAL_COMMAND_STARTED, "final",
        "final output", 0, 0);
  event(display, CAI_AGENT_EVENT_TERMINAL_COMMAND_COMPLETED, "final", "TAIL\n",
        0, 0);
  assert(strstr(c->bytes, "Running") == NULL);
  assert(strstr(c->bytes, "Ran final output (0ms)\nTAIL\n") != NULL);
  reset(c);
  event(display, CAI_AGENT_EVENT_TERMINAL_COMMAND_STARTED, "cancel",
        "cancelled", 0, 0);
  event(display, CAI_AGENT_EVENT_TERMINAL_COMMAND_CANCELLED, "cancel", NULL, 0,
        1);
  assert(strstr(c->bytes, "Cancelled cancelled") != NULL &&
         strstr(c->bytes, "Running") == NULL);
  reset(c);
  event(display, CAI_AGENT_EVENT_TERMINAL_COMMAND_STARTED, "error", "exit 2", 0,
        0);
  event(display, CAI_AGENT_EVENT_TERMINAL_COMMAND_COMPLETED, "error", NULL, 0,
        2);
  assert(strstr(c->bytes, "Ran exit 2") != NULL &&
         strstr(c->bytes, "exit=2") != NULL &&
         strstr(c->bytes, "Running") == NULL);

  reset(c);
  event(display, CAI_AGENT_EVENT_TERMINAL_COMMAND_STARTED, "dropped", "drop", 0,
        0);
  event(display, CAI_AGENT_EVENT_RUN_CANCELLED, "dropped", NULL, 0, 0);
  fake_milliseconds += 3000ULL;
  assert(cai_tool_display_tick(display, &error) == CAI_OK && c->length == 0U);

  memset(&saved, 0, sizeof(saved));
  saved.type = CAI_AGENT_EVENT_TOOL_CALL_COMPLETED;
  saved.runtime_session_id = "session";
  saved.tool_call_id = "saved";
  assert(cai_tool_display_replay_begin(display, &saved, "Ran", "saved command",
                                       1, &error) == CAI_OK);
  assert(!cai_tool_display_needs_tick(display));
  length = c->length;
  fake_milliseconds += 3000ULL;
  assert(cai_tool_display_tick(display, &error) == CAI_OK &&
         c->length == length);
  event(display, CAI_AGENT_EVENT_TERMINAL_COMMAND_COMPLETED, "saved", NULL, 0,
        0);

  reset(c);
  event(display, CAI_AGENT_EVENT_TERMINAL_COMMAND_STARTED, "fail",
        "sink failure", 0, 0);
  fake_milliseconds += 2000ULL;
  c->fail = 1;
  assert(cai_tool_display_tick(display, &error) == CAI_ERR_TRANSPORT);
  c->fail = 0;
  cai_error_cleanup(&error);
  cai_error_init(&error);
  fake_clock_error = 1;
  assert(cai_tool_display_tick(display, &error) == CAI_ERR_TRANSPORT);
  fake_clock_error = 0;
  cai_error_cleanup(&error);
  cai_error_init(&error);
  assert(cai_tool_display_tick(display, &error) == CAI_OK);
  assert(strstr(c->bytes, "Running sink failure") != NULL);
  event(display, CAI_AGENT_EVENT_TERMINAL_COMMAND_COMPLETED, "fail", NULL, 0,
        0);
  assert(cai_tool_display_tick(NULL, &error) == CAI_ERR_INVALID);
  fake_clock = 0;
  cai_tool_display_close(display);
  cai_sink_close(sink);
  cai_error_cleanup(&error);
#else
  (void)c;
#endif
}
static void timestamps(capture *c) {
  cai_tool_display_config config;
  cai_tool_display *display = NULL;
  cai_sink_callbacks callbacks;
  cai_sink *sink = NULL;
  cai_agent_runtime_event e;
  cai_error error;
  char stamp[CAI_DTG_CAPACITY], expected[128];
  time_t when = (time_t)1793622600;
  int color;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.context = c;
  callbacks.write = write_capture;
  cai_error_init(&error);
  assert(cai_sink_from_callbacks(&callbacks, &sink, &error) == CAI_OK);
  for (color = 0; color < 2; color++) {
    memset(&config, 0, sizeof(config));
    memset(&e, 0, sizeof(e));
    config.sink = sink;
    config.color = color;
    config.timestamps = config.utc = 1;
    assert(cai_tool_display_new(&config, &display, &error) == CAI_OK);
    c->length = 0U;
    e.type = CAI_AGENT_EVENT_REASONING_TOOL_RESULT;
    e.timestamp_ns = 1793622600123456789ULL;
    e.data = "requested high";
    e.data_length = strlen(e.data);
    assert(cai_tool_display_event(display, &e, &error) == CAI_OK);
    assert(cai_dtg_format_time(when, time(NULL), 1, stamp, sizeof(stamp)) ==
           CAI_DTG_OK);
    snprintf(expected, sizeof(expected), "%s%s%s", color ? "\033[0;2;90m" : "",
             stamp, color ? "\033[0m" : "");
    assert(strstr(c->bytes, expected) != NULL);
    assert(strstr(c->bytes, "requested high") != NULL);
    if (!color)
      assert(strchr(c->bytes, 27) == NULL);
    c->length = 0U;
    e.timestamp_ns = 0U; /* Legacy entries must never use the replay clock. */
    assert(cai_tool_display_event(display, &e, &error) == CAI_OK);
    assert(strstr(c->bytes, stamp) == NULL);
    cai_tool_display_close(display);
  }
  cai_sink_close(sink);
  cai_error_cleanup(&error);
}
int main(void) {
  capture c;
  memset(&c, 0, sizeof(c));
  utf8_text();
  assert(setlocale(LC_CTYPE, "C.UTF-8") != NULL ||
         setlocale(LC_CTYPE, "en_US.UTF-8") != NULL);
  numbered_patches(&c);
  patches(&c);
  palette_change(&c);
  palette_validation(&c);
  commands(&c);
  command_durations(&c);
  command_delay(&c);
  command_controls();
  timestamps(&c);
  free(c.bytes);
  puts("patch rendering, live previews, status, mode capture and failure "
       "checks passed");
  return 0;
}
