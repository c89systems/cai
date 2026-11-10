#include "../cli/tool_result.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct capture {
  char text[16384];
  size_t length;
  int fail, fail_notice;
} capture;
static int write_text(void *context, const char *bytes, size_t count) {
  capture *c = context;
  assert(count != 0U);
  if (c->fail ||
      (c->fail_notice && count > strlen("*Output omitted") &&
       memcmp(bytes, "*Output omitted", strlen("*Output omitted")) == 0))
    return -1;
  assert(count < sizeof(c->text) - c->length);
  memcpy(c->text + c->length, bytes, count);
  c->length += count;
  c->text[c->length] = '\0';
  return 0;
}
static int write_sink(void *context, const void *bytes, size_t count,
                      cai_error *error) {
  (void)error;
  return write_text(context, bytes, count) == 0 ? CAI_OK : CAI_ERR_TRANSPORT;
}
static int format(capture *c, const char *tool, int action, int type,
                  const char *body) {
  cai_cli_tool_result *result = NULL;
  cai_agent_runtime_event event;
  cai_tool_display_config config;
  cai_tool_display *display = NULL;
  cai_sink_callbacks callbacks;
  cai_sink *sink = NULL;
  cai_error error;
  int handled, rc;
  memset(&event, 0, sizeof(event));
  event.type = type;
  event.tool_name = tool;
  event.tool_action = action;
  event.tool_call_id = "call";
  event.runtime_session_id = "session";
  event.data = body;
  event.data_length = body != NULL ? strlen(body) : 0U;
  cai_error_init(&error);
  assert(cai_cli_tool_result_open(&event, &result, &error) == CAI_OK);
  handled = result != NULL;
  if (handled) {
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.context = c;
    callbacks.write = write_sink;
    assert(cai_sink_from_callbacks(&callbacks, &sink, &error) == CAI_OK);
    memset(&config, 0, sizeof(config));
    config.sink = sink;
    config.color = 1;
    assert(cai_tool_display_new(&config, &display, &error) == CAI_OK);
    event.type = CAI_AGENT_EVENT_TOOL_CALL_STARTED;
    assert(cai_tool_display_event(display, &event, &error) == CAI_OK);
    event.type = type;
    rc = cai_cli_tool_result_heading(result, display, &event, &error);
    if (rc == CAI_OK)
      rc = cai_cli_tool_result_write(result, CAI_TOOL_OUTPUT_COMPACT, 80U,
                                     write_text, c, &error);
    assert(rc == (c->fail ? CAI_ERR_TRANSPORT : CAI_OK));
    /* Summary completion releases call state, including sink failures. */
    event.type = CAI_AGENT_EVENT_TOOL_CALL_STARTED;
    assert(cai_tool_display_event(display, &event, &error) == CAI_OK);
    cai_tool_display_close(display);
    cai_sink_close(sink);
  }
  cai_cli_tool_result_close(result);
  cai_error_cleanup(&error);
  return handled;
}
static int result(capture *c, const char *tool, const char *body) {
  memset(c, 0, sizeof(*c));
  return format(c, tool, CAI_AGENT_TOOL_ACTION_EXTERNAL,
                CAI_AGENT_EVENT_TOOL_CALL_COMPLETED, body);
}
static void streamed_text(void) {
  static const char *const input[] = {"## Header\n\n**body**\n",
                                      "{\"one\":1,\n\"two\":2}\n",
                                      "bad **line**\nnext\n"};
  static const char *const expected[] = {"## Header\n\n**body**\n\n",
                                         "    {\"one\":1,\n    \"two\":2}\n\n",
                                         ">     bad **line**\n>     next\n\n"};
  capture c;
  cai_cli_tool_text state;
  cai_error error;
  size_t i, step, offset, count, length;
  cai_error_init(&error);
  for (i = 0U; i < 3U; i++) {
    length = strlen(input[i]);
    for (step = 1U; step <= 7U; step++) {
      memset(&c, 0, sizeof(c));
      memset(&state, 0, sizeof(state));
      for (offset = 0U; offset < length; offset += count) {
        count = length - offset < step ? length - offset : step;
        assert(cai_cli_tool_text_write(&state, input[i] + offset, count,
                                       offset + count == length, i == 2U,
                                       write_text, &c, &error) == CAI_OK);
        assert(c.length != 0U); /* Writes before the payload is complete. */
      }
      assert(strcmp(c.text, expected[i]) == 0);
    }
  }
  memset(&c, 0, sizeof(c));
  memset(&state, 0, sizeof(state));
  c.fail = 1;
  assert(cai_cli_tool_text_write(&state, "body", 4U, 1, 0, write_text, &c,
                                 &error) == CAI_ERR_TRANSPORT);
  cai_error_cleanup(&error);
}
static void compact_text(void) {
  capture c, expected;
  cai_cli_tool_text state;
  cai_error error;
  char body[4096];
  size_t step, offset, n, width;
  cai_error_init(&error);
  memset(body, 'x', sizeof(body));
  body[0] = '{';
  memcpy(body + sizeof(body) - 12U, "HIDDEN_TAIL}", 12U);
  for (width = 8U; width <= 120U; width += 56U) {
    memset(&expected, 0, sizeof(expected));
    cai_cli_tool_text_init(&state, CAI_TOOL_OUTPUT_COMPACT, width);
    assert(cai_cli_tool_text_write(&state, body, sizeof(body), 1, 0, write_text,
                                   &expected, &error) == CAI_OK);
    assert(expected.length < 5U * width + 256U);
    assert(!strstr(expected.text, "HIDDEN_TAIL"));
    assert(strstr(expected.text, "Output omitted"));
    for (step = 1U; step <= 97U; step += 16U) {
      memset(&c, 0, sizeof(c));
      cai_cli_tool_text_init(&state, CAI_TOOL_OUTPUT_COMPACT, width);
      for (offset = 0U; offset < sizeof(body); offset += n) {
        n = sizeof(body) - offset;
        if (n > step)
          n = step;
        assert(cai_cli_tool_text_write(&state, body + offset, n,
                                       offset + n == sizeof(body), 0,
                                       write_text, &c, &error) == CAI_OK);
      }
      assert(strcmp(c.text, expected.text) == 0);
    }
  }
  memset(&c, 0, sizeof(c));
  cai_cli_tool_text_init(&state, CAI_TOOL_OUTPUT_FULL, 8U);
  assert(cai_cli_tool_text_write(&state, body, sizeof(body), 1, 0, write_text,
                                 &c, &error) == CAI_OK);
  assert(strstr(c.text, "HIDDEN_TAIL}") && !strstr(c.text, "Output omitted"));
  memset(&c, 0, sizeof(c));
  body[sizeof(body) - 1U] = '\0';
  assert(cai_cli_tool_busy_write("active", body, CAI_TOOL_OUTPUT_FULL, 8U,
                                 write_text, &c, &error) == CAI_OK);
  assert(strstr(c.text, "HIDDEN_TAIL") && !strstr(c.text, "Output omitted"));
  memset(&c, 0, sizeof(c));
  cai_cli_tool_text_init(&state, CAI_TOOL_OUTPUT_COMPACT, 80U);
  assert(cai_cli_tool_text_write(&state, "one\ntwo\nthree\nfour\nfive\nsix\n",
                                 strlen("one\ntwo\nthree\nfour\nfive\nsix\n"),
                                 1, 1, write_text, &c, &error) == CAI_OK);
  assert(strstr(c.text, ">     five") && !strstr(c.text, ">     six"));
  assert(strstr(c.text, "Output omitted"));
  /* A UTF-8 scalar at the cutoff must never be split into invalid bytes. */
  memset(&c, 0, sizeof(c));
  memset(body, 'x', 39U);
  memcpy(body + 39U, "é", 2U);
  cai_cli_tool_text_init(&state, CAI_TOOL_OUTPUT_COMPACT, 8U);
  assert(cai_cli_tool_text_write(&state, body, 41U, 1, 0, write_text, &c,
                                 &error) == CAI_OK);
  assert(!strstr(c.text, "é") && !strchr(c.text, '?'));
  assert(strstr(c.text, "Output omitted"));
  /* Content blocks share one budget, rather than five lines each. */
  memset(&c, 0, sizeof(c));
  assert(
      format(&c, "mcp_fixture", CAI_AGENT_TOOL_ACTION_MCP,
             CAI_AGENT_EVENT_TOOL_CALL_COMPLETED,
             "{\"content\":[{\"type\":\"text\",\"text\":\"one\\ntwo\\nthree\"},"
             "{\"type\":\"text\",\"text\":\"four\\nfive\\nsix\"},"
             "{\"type\":\"text\",\"text\":\"HIDDEN_TAIL\"}]}"));
  assert(!strstr(c.text, "HIDDEN_TAIL") && strstr(c.text, "Output omitted"));
  /* Empty content cannot flood the feed with blank lines. */
  memset(&c, 0, sizeof(c));
  cai_cli_tool_text_init(&state, CAI_TOOL_OUTPUT_COMPACT, 80U);
  for (offset = 0U; offset < 1000U; offset++)
    assert(cai_cli_tool_text_write(&state, "", 0U, 1, 0, write_text, &c,
                                   &error) == CAI_OK);
  assert(c.length == 0U);
  memset(body, 'x', sizeof(body));
  assert(cai_cli_tool_text_write(&state, body, 400U, 1, 0, write_text, &c,
                                 &error) == CAI_OK);
  assert(!strstr(c.text, "Output omitted"));
  memset(&c, 0, sizeof(c));
  cai_cli_tool_text_init(&state, CAI_TOOL_OUTPUT_COMPACT, 80U);
  c.fail_notice = 1;
  assert(cai_cli_tool_text_write(&state, body, sizeof(body), 1, 0, write_text,
                                 &c, &error) == CAI_ERR_TRANSPORT);
  cai_error_cleanup(&error);
}
int main(void) {
  capture c;
  size_t i;
  int action;
  static const char *invalid[] = {
      NULL,
      "",
      "not json",
      "{}",
      "{\"status\":\"delivered\"}",
      "{\"status\":\"failed\",\"recipient\":\"q\"}",
      "{\"status\":\"delivered\",\"recipient\":12}",
      "{\"status\":\"delivered\",\"recipient\":\"\"}",
      "{\"status\":\"delivered\",\"recipient\":\"q\"} trailing"};
  assert(!result(&c, "set_reasoning", "ok"));
  assert(!result(&c, "set_reasoning", "not ok"));
  memset(&c, 0, sizeof(c));
  assert(format(
      &c, "exec_command", CAI_AGENT_TOOL_ACTION_EXECUTE,
      CAI_AGENT_EVENT_TOOL_CALL_COMPLETED,
      "{\"status\":\"busy\",\"accepted\":false,\"session_id\":\"terminal-1\","
      "\"active_command\":\"read x; echo **literal**\",\"output\":\"\"}"));
  assert(strstr(c.text, "Busy ") && strstr(c.text, "command not started"));
  assert(strstr(c.text, "Active handle: `terminal-1`"));
  assert(strstr(c.text, ">     read x; echo **literal**"));
  assert(!strstr(c.text, "\"accepted\""));
  assert(!format(
      &c, "exec_command", CAI_AGENT_TOOL_ACTION_EXECUTE,
      CAI_AGENT_EVENT_TOOL_CALL_COMPLETED,
      "{\"status\":\"completed\",\"accepted\":true,\"output\":\"raw bytes\"}"));
  streamed_text();
  compact_text();
  for (action = CAI_AGENT_TOOL_ACTION_NONE;
       action <= CAI_AGENT_TOOL_ACTION_CHROME; action++) {
    memset(&c, 0, sizeof(c));
    assert(format(&c, "fixture", action, CAI_AGENT_EVENT_TOOL_CALL_FAILED,
                  "literal **failure**"));
    assert(strstr(c.text, ">     literal **failure**"));
  }
  assert(result(&c, "agent_message",
                "{\"status\":\"delivered\",\"recipient\":\"cai.y\",\"message_"
                "id\":\"id\"}"));
  assert(strstr(c.text, "Delivered message to ") && strstr(c.text, "cai.y"));
  assert(!strstr(c.text, "message_id") && !strstr(c.text, "{\""));
  assert(
      result(&c, "agent_message",
             "{\"status\":\"scheduled\",\"recipient\":\"cai.y\","
             "\"message_id\":\"id\",\"not_visible_until_unix\":1900000000}"));
  assert(strstr(c.text, "Scheduled message to ") && strstr(c.text, "cai.y"));
  assert(!strstr(c.text, "Delivered") && !strstr(c.text, "not_visible_until") &&
         !strstr(c.text, "{\""));
  assert(result(&c, "run_review_and_fix", "{\"status\":\"queued\"}"));
  assert(strstr(c.text, "Queued ") && strstr(c.text, "review-and-fix goal"));
  assert(!strstr(c.text, "{\"status\""));
  assert(result(
      &c, "tool_search",
      "{\"ok\":true,\"message\":\"Loaded 1 MCP tool(s) for the next request.\","
      "\"tools\":[{\"name\":\"mail_search\",\"input_schema\":{\"type\":"
      "\"object\"}}]}"));
  assert(strstr(c.text, "Discovered ") && strstr(c.text, "MCP tools"));
  assert(strstr(c.text, "Loaded 1 MCP tool(s)"));
  assert(!strstr(c.text, "input_schema") && !strstr(c.text, "{\""));
  assert(result(&c, "agent_message",
                "## Message queues\n\n| Queue | Workspace | Session |\n| "
                "--- | --- | --- |\n| cai.y | /work | session |\n"));
  assert(strstr(c.text, "Listed ") &&
         strstr(c.text, "| cai.y | /work | session |"));
  assert(
      result(&c, "todo_kanban",
             "{\"ok\":true,\"operation\":\"add_item\",\"code\":\"ok\",\"item_"
             "id\":\"DEF-001\",\"title\":\"Fix it\",\"status\":\"todo\"}"));
  assert(strstr(c.text, "Added task ") && strstr(c.text, "DEF-001"));
  assert(strstr(c.text, "Fix it — todo") && !strstr(c.text, "{\""));
  assert(
      result(&c, "todo_kanban",
             "{\"ok\":true,\"operation\":\"list_board\",\"code\":\"ok\","
             "\"items\":[{\"id\":\"DEF-001\",\"status\":\"todo\",\"title\":"
             "\"Pipe | **star** \\u001b[2J \\u009b\"}],\"truncated\":true}"));
  assert(strstr(c.text, "| Task | Status | Title |"));
  assert(strstr(c.text, "Pipe \\| \\*\\*star\\*\\*  \\[2J"));
  assert(!strstr(c.text, "\033[2J") && !strstr(c.text, "\302\233"));
  assert(strstr(c.text, "Result limit reached"));
  assert(result(&c, "todo_kanban",
                "{\"ok\":true,\"operation\":\"list_board\",\"code\":\"ok\","
                "\"items\":[]}"));
  assert(strstr(c.text, "No tasks."));
  assert(result(&c, "todo_kanban",
                "{\"ok\":true,\"operation\":\"list_boards\",\"code\":\"ok\","
                "\"boards\":[{\"key\":\"DEF\",\"name\":\"default\"}]}"));
  assert(strstr(c.text, "| Key | Board |") &&
         strstr(c.text, "| DEF | default |"));
  assert(result(&c, "todo_kanban",
                "{\"ok\":false,\"operation\":\"move_item\",\"code\":\"wip_"
                "limit_exceeded\",\"message\":\"Limit exceeded\"}"));
  assert(strstr(c.text, "\033[91m*\033[0m") &&
         strstr(c.text, "Kanban declined"));
  assert(strstr(c.text, "Limit exceeded"));
  assert(result(&c, "todo_kanban", "{\"operation\":\"list_board\"}"));
  assert(strstr(c.text, "    {\"operation\":\"list_board\"}"));
  assert(result(&c, "searxng_search",
                "{\"query\":\"query\",\"title\":\"Title\",\"url\":\"https://"
                "example.test\",\"snippet\":\"Result snippet\"}"));
  assert(strstr(c.text, "Searched ") && strstr(c.text, "Result snippet"));
  assert(result(&c, "reverse_geocode", "{\"label\":\"Gothenburg\"}"));
  assert(strstr(c.text, "Located ") && strstr(c.text, "Gothenburg"));
  for (i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); i++)
    assert(result(&c, "agent_message", invalid[i]));
  assert(result(&c, "custom_tool", "## Result\n\n**Useful details**"));
  assert(strstr(c.text, "## Result\n\n**Useful details**"));
  assert(result(&c, "custom_tool", "invalid \340\200\200 text"));
  assert(strstr(c.text, "invalid ??? text"));
  memset(&c, 0, sizeof(c));
  assert(format(&c, "set_reasoning", CAI_AGENT_TOOL_ACTION_MCP,
                CAI_AGENT_EVENT_TOOL_CALL_COMPLETED, "ok"));
  assert(format(&c, "set_reasoning", CAI_AGENT_TOOL_ACTION_EXTERNAL,
                CAI_AGENT_EVENT_TOOL_CALL_FAILED, "failure detail"));
  assert(strstr(c.text, ">     failure detail"));
  memset(&c, 0, sizeof(c));
  assert(format(&c, "run_review", CAI_AGENT_TOOL_ACTION_SUBAGENT,
                CAI_AGENT_EVENT_TOOL_CALL_FAILED, "pouch lease already held"));
  assert(strstr(c.text, ">     pouch lease already held"));
  memset(&c, 0, sizeof(c));
  assert(format(&c, "mcp_server_tool", CAI_AGENT_TOOL_ACTION_MCP,
                CAI_AGENT_EVENT_TOOL_CALL_COMPLETED,
                "{\"isError\":false,\"content\":[{\"type\":\"text\","
                "\"text\":\"## Server result\\n\\n**details**\"}]}"));
  assert(strstr(c.text, "## Server result\n\n**details**"));
  assert(!strstr(c.text, "\"content\""));
  memset(&c, 0, sizeof(c));
  assert(format(&c, "mcp_server_tool", CAI_AGENT_TOOL_ACTION_MCP,
                CAI_AGENT_EVENT_TOOL_CALL_COMPLETED,
                "{\"isError\":true,\"content\":[{\"type\":\"text\","
                "\"text\":\"bad **literal** request\"}]}"));
  assert(strstr(c.text, "\033[91m*\033[0m") && strstr(c.text, "MCP failed"));
  assert(strstr(c.text, ">     bad **literal** request"));
  memset(&c, 0, sizeof(c));
  assert(format(&c, "exec_command", CAI_AGENT_TOOL_ACTION_EXECUTE,
                CAI_AGENT_EVENT_TOOL_CALL_FAILED, "command failure"));
  assert(strstr(c.text, ">     command failure"));
  memset(&c, 0, sizeof(c));
  assert(format(&c, "apply_patch", CAI_AGENT_TOOL_ACTION_PATCH,
                CAI_AGENT_EVENT_TOOL_CALL_FAILED,
                "Patch context does not match in \"file.c\" (hunk 2).\n"
                "Expected: \"int    x;\"\nActual:   \"int  x;\""));
  assert(strstr(c.text, "\033[91m*\033[0m") && strstr(c.text, "Failed"));
  assert(strstr(c.text, ">     Patch context"));
  assert(strstr(
      c.text, "\n>     Expected: \"int    x;\"\n>     Actual:   \"int  x;\""));
  memset(&c, 0, sizeof(c));
  assert(format(&c, "apply_patch", CAI_AGENT_TOOL_ACTION_PATCH,
                CAI_AGENT_EVENT_TOOL_CALL_FAILED,
                "head\n```\n**source** [link](url)\033\302\233\nend"));
  assert(strstr(c.text, ">     head\n>     ```\n>     **source** [link](url)"));
  assert(strstr(c.text, "\n>     end\n\n"));
  assert(!strstr(c.text, "\033\302\233") && !strstr(c.text, "\302\233"));
  memset(&c, 0, sizeof(c));
  assert(format(&c, "apply_patch", CAI_AGENT_TOOL_ACTION_PATCH,
                CAI_AGENT_EVENT_TOOL_CALL_FAILED, NULL));
  assert(strstr(c.text, "no details provided"));
  memset(&c, 0, sizeof(c));
  assert(format(&c, "apply_patch", CAI_AGENT_TOOL_ACTION_PATCH,
                CAI_AGENT_EVENT_TOOL_CALL_FAILED, ""));
  assert(strstr(c.text, "no details provided"));
  memset(&c, 0, sizeof(c));
  c.fail = 1;
  assert(format(&c, "apply_patch", CAI_AGENT_TOOL_ACTION_PATCH,
                CAI_AGENT_EVENT_TOOL_CALL_FAILED, "failure detail"));
  c.fail = 1;
  assert(format(&c, "agent_message", CAI_AGENT_TOOL_ACTION_EXTERNAL,
                CAI_AGENT_EVENT_TOOL_CALL_COMPLETED,
                "{\"status\":\"delivered\",\"recipient\":\"q\"}"));
  {
    cai_cli_tool_result *prepared = NULL;
    cai_agent_runtime_event event;
    cai_error error;
    memset(&event, 0, sizeof(event));
    event.type = CAI_AGENT_EVENT_TOOL_CALL_COMPLETED;
    event.tool_action = CAI_AGENT_TOOL_ACTION_EXTERNAL;
    event.tool_name = "todo_kanban";
    event.data = "{\"ok\":true,\"operation\":\"add_item\",\"code\":\"ok\","
                 "\"message\":\"item added\"}";
    event.data_length = strlen(event.data);
    cai_error_init(&error);
    assert(cai_cli_tool_result_open(&event, &prepared, &error) == CAI_OK &&
           prepared != NULL);
    assert(cai_cli_tool_result_write(prepared, CAI_TOOL_OUTPUT_COMPACT, 80U,
                                     write_text, &c,
                                     &error) == CAI_ERR_TRANSPORT);
    cai_cli_tool_result_close(prepared);
    cai_error_cleanup(&error);
    prepared = NULL;
    event.type = CAI_AGENT_EVENT_TOOL_CALL_FAILED;
    event.tool_action = CAI_AGENT_TOOL_ACTION_PATCH;
    event.tool_name = "apply_patch";
    event.data = "Cannot find the requested context.";
    event.data_length = strlen(event.data);
    cai_error_init(&error);
    assert(cai_cli_tool_result_open(&event, &prepared, &error) == CAI_OK &&
           prepared != NULL);
    assert(cai_cli_tool_result_write(prepared, CAI_TOOL_OUTPUT_COMPACT, 80U,
                                     write_text, &c,
                                     &error) == CAI_ERR_TRANSPORT);
    cai_cli_tool_result_close(prepared);
    cai_error_cleanup(&error);
  }
  {
    capture *context = calloc(1U, sizeof(*context));
    assert(context != NULL);
    assert(result(context, "context_create",
                  "{\"id\":\"shared-key\",\"version\":2,\"kind\":\"text\","
                  "\"status\":\"created\"}"));
    assert(strstr(context->text, "shared-key") &&
           strstr(context->text, "version 2") &&
           !strstr(context->text, "\"status\""));
    assert(result(
        context, "context_get",
        "{\"version\":3,\"metadata\":{\"id\":\"shared-key\",\"title\":"
        "\"Notes\"},\"content\":\"Markdown body\",\"truncated\":false}"));
    assert(strstr(context->text, "shared-key") &&
           strstr(context->text, "Markdown body") &&
           !strstr(context->text, "\"metadata\""));
    assert(result(context, "context_query",
                  "{\"items\":[{\"version\":3,\"metadata\":{\"id\":\"one\","
                  "\"title\":\"First\"}},{\"version\":4,\"metadata\":{\"id\":"
                  "\"two\",\"title\":\"Second\"}}],\"cursor\":\"next\"}"));
    assert(strstr(context->text, "First") && strstr(context->text, "Second") &&
           strstr(context->text, "returned cursor") &&
           !strstr(context->text, "\"items\""));
    assert(result(context, "context_custom", "{\"custom\":true}"));
    assert(strstr(context->text, "context_custom") &&
           !strstr(context->text, "shared context"));
    free(context);
  }
  puts("built-in tool presentation checks passed");
  return 0;
}
