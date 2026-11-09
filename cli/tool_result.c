#define _POSIX_C_SOURCE 200809L
#include "tool_result.h"
#include "pouch_stream.h"
#include <stdlib.h>
#include <string.h>

typedef struct result_row {
  char *id, *key, *name, *title, *status, *type, *text;
} result_row;
static const lonejson_field row_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(result_row, id, "id"),
    LONEJSON_FIELD_STRING_ALLOC(result_row, key, "key"),
    LONEJSON_FIELD_STRING_ALLOC(result_row, name, "name"),
    LONEJSON_FIELD_STRING_ALLOC(result_row, title, "title"),
    LONEJSON_FIELD_STRING_ALLOC(result_row, status, "status"),
    LONEJSON_FIELD_STRING_ALLOC(result_row, type, "type"),
    LONEJSON_FIELD_STRING_ALLOC(result_row, text, "text")};
LONEJSON_MAP_DEFINE(row_map, result_row, row_fields);
typedef struct result_doc {
  char *operation, *code, *message, *status, *recipient;
  char *session_id, *active_command;
  char *board_name, *item_id, *title, *query, *url, *snippet, *label;
  int ok, has_ok, truncated, is_error;
  int accepted, has_accepted;
  lonejson_object_array items, boards, content;
} result_doc;
static const lonejson_field result_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(result_doc, operation, "operation"),
    LONEJSON_FIELD_STRING_ALLOC(result_doc, code, "code"),
    LONEJSON_FIELD_STRING_ALLOC(result_doc, message, "message"),
    LONEJSON_FIELD_STRING_ALLOC(result_doc, status, "status"),
    LONEJSON_FIELD_STRING_ALLOC(result_doc, session_id, "session_id"),
    LONEJSON_FIELD_STRING_ALLOC(result_doc, active_command, "active_command"),
    LONEJSON_FIELD_BOOL_PRESENT(result_doc, accepted, has_accepted, "accepted"),
    LONEJSON_FIELD_STRING_ALLOC(result_doc, recipient, "recipient"),
    LONEJSON_FIELD_STRING_ALLOC(result_doc, board_name, "board_name"),
    LONEJSON_FIELD_STRING_ALLOC(result_doc, item_id, "item_id"),
    LONEJSON_FIELD_STRING_ALLOC(result_doc, title, "title"),
    LONEJSON_FIELD_STRING_ALLOC(result_doc, query, "query"),
    LONEJSON_FIELD_STRING_ALLOC(result_doc, url, "url"),
    LONEJSON_FIELD_STRING_ALLOC(result_doc, snippet, "snippet"),
    LONEJSON_FIELD_STRING_ALLOC(result_doc, label, "label"),
    LONEJSON_FIELD_BOOL_PRESENT(result_doc, ok, has_ok, "ok"),
    LONEJSON_FIELD_BOOL(result_doc, truncated, "truncated"),
    LONEJSON_FIELD_BOOL(result_doc, is_error, "isError"),
    LONEJSON_FIELD_OBJECT_ARRAY(result_doc, content, "content", result_row,
                                &row_map, LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_OBJECT_ARRAY(result_doc, items, "items", result_row,
                                &row_map, LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_OBJECT_ARRAY(result_doc, boards, "boards", result_row,
                                &row_map, LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(result_map, result_doc, result_fields);

struct cai_cli_tool_result {
  result_doc doc;
  const char *verb, *label, *markdown;
  size_t markdown_length;
  int success, failure, generic, mcp, busy;
};
static int same(const char *a, const char *b) {
  return a != NULL && strcmp(a, b) == 0;
}
void cai_cli_tool_result_close(cai_cli_tool_result *r) {
  if (r != NULL) {
    lonejson_cleanup(&result_map, &r->doc);
    free(r);
  }
}
int cai_cli_tool_result_open(const cai_agent_runtime_event *e,
                             cai_cli_tool_result **out, cai_error *error) {
  static const char queue_heading[] = "## Message queues\n\n";
  cai_cli_tool_result *r;
  lonejson *json;
  lonejson_error local;
  const char *op;
  int recognized = 0;
  *out = NULL;
  if (e->type == CAI_AGENT_EVENT_TOOL_CALL_FAILED) {
    r = calloc(1U, sizeof(*r));
    if (r == NULL)
      return cai_cli_error(error, CAI_ERR_NOMEM, "prepare tool failure");
    r->verb =
        e->tool_action == CAI_AGENT_TOOL_ACTION_MCP ? "MCP failed" : "Failed";
    r->label = e->tool_name != NULL ? e->tool_name : "tool";
    r->failure = 1;
    r->markdown = e->data != NULL && e->data_length != 0U
                      ? e->data
                      : "Tool failed; no details provided.";
    r->markdown_length = e->data != NULL && e->data_length != 0U
                             ? e->data_length
                             : strlen(r->markdown);
    *out = r;
    return CAI_OK;
  }
  if (e->type != CAI_AGENT_EVENT_TOOL_CALL_COMPLETED ||
      (e->tool_action != CAI_AGENT_TOOL_ACTION_EXTERNAL &&
       e->tool_action != CAI_AGENT_TOOL_ACTION_MCP &&
       e->tool_action != CAI_AGENT_TOOL_ACTION_EXECUTE) ||
      (e->tool_action == CAI_AGENT_TOOL_ACTION_EXTERNAL &&
       same(e->tool_name, "set_reasoning")))
    return CAI_OK;
  r = calloc(1U, sizeof(*r));
  if (r == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "prepare tool presentation");
  r->success = 1;
  if (e->data == NULL || (e->tool_action != CAI_AGENT_TOOL_ACTION_MCP &&
                          e->tool_action != CAI_AGENT_TOOL_ACTION_EXECUTE &&
                          !same(e->tool_name, "run_review_and_fix") &&
                          !same(e->tool_name, "tool_search") &&
                          !same(e->tool_name, "agent_message") &&
                          !same(e->tool_name, "todo_kanban") &&
                          !same(e->tool_name, "searxng_search") &&
                          !same(e->tool_name, "reverse_geocode")))
    goto unrecognized;
  if (same(e->tool_name, "agent_message") &&
      e->data_length >= sizeof(queue_heading) - 1U &&
      memcmp(e->data, queue_heading, sizeof(queue_heading) - 1U) == 0) {
    r->verb = "Listed";
    r->label = "message queues";
    r->markdown = e->data;
    r->markdown_length = e->data_length;
  } else {
    lonejson_error_init(&local);
    json = lonejson_new(NULL, &local);
    if (json == NULL) {
      cai_cli_tool_result_close(r);
      return cai_cli_error(error, CAI_ERR_NOMEM, "allocate tool result parser");
    }
    recognized =
        lonejson_parse_buffer(json, &result_map, &r->doc, e->data,
                              e->data_length, &local) == LONEJSON_STATUS_OK;
    lonejson_free(json);
    if (!recognized)
      goto unrecognized;
    if (e->tool_action == CAI_AGENT_TOOL_ACTION_EXECUTE) {
      if (!same(r->doc.status, "busy") || !r->doc.has_accepted ||
          r->doc.accepted || r->doc.session_id == NULL ||
          r->doc.active_command == NULL) {
        cai_cli_tool_result_close(r);
        return CAI_OK;
      }
      r->busy = 1;
      r->verb = "Busy";
      r->label = "command not started";
    } else if (e->tool_action == CAI_AGENT_TOOL_ACTION_MCP) {
      if (r->doc.content.count == 0U && !r->doc.is_error)
        goto unrecognized;
      r->mcp = 1;
      r->success = !r->doc.is_error;
      r->verb = r->success ? "MCP" : "MCP failed";
      r->label = e->tool_name != NULL ? e->tool_name : "tool";
    } else if (same(e->tool_name, "tool_search")) {
      if (!r->doc.has_ok || r->doc.message == NULL)
        goto unrecognized;
      r->verb = "Discovered";
      r->label = "MCP tools";
      r->markdown = r->doc.message;
      r->markdown_length = strlen(r->markdown);
    } else if (same(e->tool_name, "run_review_and_fix")) {
      if (!same(r->doc.status, "queued"))
        goto unrecognized;
      r->verb = "Queued";
      r->label = "review-and-fix goal";
    } else if (same(e->tool_name, "agent_message")) {
      if ((!same(r->doc.status, "delivered") &&
           !same(r->doc.status, "scheduled")) ||
          r->doc.recipient == NULL || r->doc.recipient[0] == '\0')
        goto unrecognized;
      r->verb = same(r->doc.status, "scheduled") ? "Scheduled message to"
                                                 : "Delivered message to";
      r->label = r->doc.recipient;
    } else if (same(e->tool_name, "todo_kanban")) {
      op = r->doc.operation;
      if (!r->doc.has_ok || op == NULL || r->doc.code == NULL)
        goto unrecognized;
      r->success = r->doc.ok;
      r->verb = !r->success                 ? "Kanban declined"
                : same(op, "add_item")      ? "Added task"
                : same(op, "move_item")     ? "Moved task"
                : same(op, "complete_item") ? "Completed task"
                : same(op, "create_board")  ? "Created board"
                : same(op, "select_board")  ? "Selected board"
                : same(op, "set_wip_limit") ? "Updated board limit"
                : same(op, "get_item")      ? "Read task"
                : same(op, "current_board") ? "Read board"
                : same(op, "help")          ? "Read"
                                            : "Listed";
      r->label = !r->success                 ? r->doc.code
                 : r->doc.item_id != NULL    ? r->doc.item_id
                 : r->doc.board_name != NULL ? r->doc.board_name
                                             : "kanban boards";
    } else if (same(e->tool_name, "searxng_search")) {
      if (r->doc.query == NULL || r->doc.title == NULL || r->doc.url == NULL)
        goto unrecognized;
      r->verb = "Searched";
      r->label = r->doc.query;
    } else {
      if (r->doc.label == NULL)
        goto unrecognized;
      r->verb = "Located";
      r->label = r->doc.label;
    }
  }
  *out = r;
  return CAI_OK;
unrecognized:
  if (e->tool_action == CAI_AGENT_TOOL_ACTION_EXECUTE) {
    cai_cli_tool_result_close(r);
    return CAI_OK;
  }
  r->verb = e->tool_action == CAI_AGENT_TOOL_ACTION_MCP ? "MCP" : "Completed";
  r->label = e->tool_name != NULL ? e->tool_name : "tool";
  r->generic = 1;
  r->markdown = e->data;
  r->markdown_length = e->data_length;
  *out = r;
  return CAI_OK;
}

int cai_cli_tool_result_heading(cai_cli_tool_result *r,
                                cai_tool_display *display,
                                const cai_agent_runtime_event *event,
                                cai_error *error) {
  return cai_tool_display_summary(display, event, r->verb, r->label, r->success,
                                  error);
}

/* Escape metadata as text even inside Markdown tables. Description Markdown
 * remains in /todo see; tool receipts are deliberately short summaries. */
static int literal(int (*write)(void *, const char *, size_t), void *context,
                   const char *text, size_t length, int escape) {
  const unsigned char *p = (const unsigned char *)(text != NULL ? text : "");
  const unsigned char *end = p + length;
  char buffer[1024];
  size_t used = 0U, need, i;
  for (; p < end; p++) {
    if (used > sizeof(buffer) - 8U) {
      if (write(context, buffer, used) != 0)
        return -1;
      used = 0U;
    }
    if ((*p == '\n' || *p == '\t') && escape < 0) {
      buffer[used++] = (char)*p;
    } else if (*p < 32U || *p == 127U) {
      buffer[used++] = ' ';
    } else if (*p == 0xc2U && p + 1U < end && p[1] >= 0x80U && p[1] <= 0x9fU) {
      buffer[used++] = ' ';
      p++;
    } else if (*p >= 128U) {
      need = *p >= 0xc2U && *p < 0xe0U    ? 2U
             : *p >= 0xe0U && *p < 0xf0U  ? 3U
             : *p >= 0xf0U && *p <= 0xf4U ? 4U
                                          : 0U;
      for (i = 1U; need != 0U && i < need; i++)
        if (i >= (size_t)(end - p) || (p[i] & 0xc0U) != 0x80U)
          need = 0U;
      if (need != 0U &&
          ((*p == 0xe0U && p[1] < 0xa0U) || (*p == 0xedU && p[1] >= 0xa0U) ||
           (*p == 0xf0U && p[1] < 0x90U) || (*p == 0xf4U && p[1] >= 0x90U)))
        need = 0U;
      if (need == 0U)
        buffer[used++] = '?';
      else {
        memcpy(buffer + used, p, need);
        used += need;
        p += need - 1U;
      }
    } else {
      if (escape > 0 && strchr("\\|`*_[]<>!#~", *p) != NULL)
        buffer[used++] = '\\';
      buffer[used++] = (char)*p;
    }
  }
  return used != 0U ? write(context, buffer, used) : 0;
}
static int cell(int (*write)(void *, const char *, size_t), void *context,
                const char *text) {
  return literal(write, context, text, text != NULL ? strlen(text) : 0U, 1);
}
void cai_cli_tool_text_init(cai_cli_tool_text *state, int mode, size_t width) {
  memset(state, 0, sizeof(*state));
  state->full = mode == CAI_TOOL_OUTPUT_FULL;
  state->width = width < 8U ? 80U : width > 4096U ? 4096U : width;
}

/* Bound source bytes conservatively at the terminal width, keeping complete
 * UTF-8 scalars at the cutoff. Each source newline reserves the rest of its
 * budgeted row; Markdown layout remains the renderer's responsibility. */
static size_t preview_keep(cai_cli_tool_text *state, const char *text,
                           size_t length) {
  size_t i = 0U, n, cost, j;
  if (state->full)
    return length;
  if (state->omitted)
    return 0U;
  if (state->width == 0U)
    state->width = 80U;
  while (i < length) {
    unsigned char ch = (unsigned char)text[i];
    if (state->row >= 5U)
      break;
    if (ch == '\n') {
      state->row++;
      state->column = 0U;
      i++;
      continue;
    }
    n = ch >= 0xc2U && ch < 0xe0U    ? 2U
        : ch >= 0xe0U && ch < 0xf0U  ? 3U
        : ch >= 0xf0U && ch <= 0xf4U ? 4U
                                     : 1U;
    if (n > length - i)
      n = 1U;
    for (j = 1U; j < n; j++)
      if (((unsigned char)text[i + j] & 0xc0U) != 0x80U) {
        n = 1U;
        break;
      }
    cost = ch == '\t' ? 8U - state->column % 8U : n;
    if (cost > state->width - state->column) {
      state->row++;
      state->column = 0U;
      if (state->row >= 5U)
        break;
      cost = ch == '\t' ? 8U : n;
    }
    state->column += cost;
    i += n;
  }
  if (i < length)
    state->omitted = 1;
  return i;
}
int cai_cli_tool_text_write(cai_cli_tool_text *state, const char *text,
                            size_t length, int final, int failed,
                            int (*write)(void *, const char *, size_t),
                            void *context, cai_error *error) {
  size_t start = 0U, end;
  if (state->omitted && state->notified)
    return CAI_OK;
  length = preview_keep(state, text, length);
  if (length == 0U && !state->started && !state->omitted)
    return CAI_OK;
  if (!state->started) {
    size_t first = 0U;
    while (first < length && strchr(" \r\n\t", text[first]) != NULL)
      first++;
    state->format =
        failed                                                         ? 1
        : first < length && (text[first] == '{' || text[first] == '[') ? 2
                                                                       : 0;
    state->started = state->line_start = 1;
  }
  while (start < length) {
    if (state->line_start) {
      const char *prefix = state->format == 1   ? ">     "
                           : state->format == 2 ? "    "
                                                : "";
      if (prefix[0] != '\0' && write(context, prefix, strlen(prefix)) != 0)
        goto failed;
      state->line_start = 0;
    }
    for (end = start; end < length && text[end] != '\n'; end++) {
    }
    if (literal(write, context, text + start, end - start, -1) != 0)
      goto failed;
    if (end < length) {
      const char *newline = "\n";
      if (write(context, newline, strlen(newline)) != 0)
        goto failed;
      state->line_start = 1;
      end++;
    }
    start = end;
  }
  if (final) {
    if (!state->line_start && write(context, "\n", 1U) != 0)
      goto failed;
    if (write(context, "\n", 1U) != 0)
      goto failed;
    if (!state->full) {
      if (!state->line_start)
        state->row++;
      state->row++;
      state->column = 0U;
    }
    state->line_start = 1;
    state->started = 0;
    if (state->omitted && !state->notified) {
      const char *notice =
          "*Output omitted; use `/output full` for future calls. "
          "The complete result is available in the JSONL export.*\n\n";
      if (write(context, notice, strlen(notice)) != 0)
        goto failed;
      state->notified = 1;
    }
  }
  return CAI_OK;
failed:
  return cai_cli_error(error, CAI_ERR_TRANSPORT, "render tool result Markdown");
}

#define WRITE(s)                                                               \
  do {                                                                         \
    if (write(context, (s), strlen(s)) != 0)                                   \
      goto failed;                                                             \
  } while (0)
#define CELL(s)                                                                \
  do {                                                                         \
    if (cell(write, context, (s)) != 0)                                        \
      goto failed;                                                             \
  } while (0)
int cai_cli_tool_busy_write(const char *session_id, const char *command,
                            int mode, size_t width,
                            int (*write)(void *, const char *, size_t),
                            void *context, cai_error *error) {
  cai_cli_tool_text text;
  WRITE("The requested command was **not started**. A command is still "
        "running.\n\n");
  WRITE("Active handle: `");
  CELL(session_id);
  WRITE("`. Use `write_stdin` to poll or terminate it.\n\n");
  if (command != NULL && command[0] != '\0') {
    WRITE("Active command:\n\n");
    cai_cli_tool_text_init(&text, mode, width);
    return cai_cli_tool_text_write(&text, command, strlen(command), 1, 1, write,
                                   context, error);
  }
  return CAI_OK;
failed:
  return cai_cli_error(error, CAI_ERR_TRANSPORT, "render busy command receipt");
}

int cai_cli_tool_result_write(cai_cli_tool_result *r, int mode, size_t width,
                              int (*write)(void *, const char *, size_t),
                              void *context, cai_error *error) {
  result_row *rows;
  size_t i, count;
  int boards;
  if (r->busy)
    return cai_cli_tool_busy_write(r->doc.session_id, r->doc.active_command,
                                   mode, width, write, context, error);
  if (r->mcp) {
    cai_cli_tool_text text;
    cai_cli_tool_text_init(&text, mode, width);
    rows = r->doc.content.items;
    for (i = 0U; i < r->doc.content.count; i++) {
      if (rows[i].text != NULL) {
        if (cai_cli_tool_text_write(&text, rows[i].text, strlen(rows[i].text),
                                    1, !r->success, write, context,
                                    error) != CAI_OK)
          return CAI_ERR_TRANSPORT;
      } else {
        const char *type = rows[i].type != NULL ? rows[i].type : "unknown";
        if (cai_cli_tool_text_write(
                &text,
                "Non-text MCP content: ", strlen("Non-text MCP content: "), 0,
                !r->success, write, context, error) != CAI_OK ||
            cai_cli_tool_text_write(&text, type, strlen(type), 1, !r->success,
                                    write, context, error) != CAI_OK)
          return CAI_ERR_TRANSPORT;
      }
      if (text.omitted)
        break;
    }
    return CAI_OK;
  }
  if (r->generic || r->failure) {
    cai_cli_tool_text text;
    cai_cli_tool_text_init(&text, mode, width);
    return cai_cli_tool_text_write(&text, r->markdown, r->markdown_length, 1,
                                   r->failure, write, context, error);
  }
  if (r->markdown != NULL) {
    if (write(context, r->markdown, r->markdown_length) != 0)
      goto failed;
    return CAI_OK;
  }
  if (r->doc.title != NULL) {
    CELL(r->doc.title);
    if (r->doc.status != NULL) {
      WRITE(" — ");
      CELL(r->doc.status);
    }
    WRITE("\n\n");
  } else if (r->doc.message != NULL) {
    CELL(r->doc.message);
    WRITE("\n\n");
  }
  boards = same(r->doc.operation, "list_boards");
  rows = boards ? r->doc.boards.items : r->doc.items.items;
  count = boards ? r->doc.boards.count : r->doc.items.count;
  if (count != 0U) {
    WRITE(boards ? "| Key | Board |\n| --- | --- |\n"
                 : "| Task | Status | Title |\n| --- | --- | --- |\n");
    for (i = 0U; i < count; i++) {
      WRITE("| ");
      CELL(boards ? rows[i].key : rows[i].id);
      WRITE(" | ");
      CELL(boards ? rows[i].name : rows[i].status);
      if (!boards) {
        WRITE(" | ");
        CELL(rows[i].title);
      }
      WRITE(" |\n");
    }
  } else if (boards || same(r->doc.operation, "list_board") ||
             same(r->doc.operation, "snapshot_board") ||
             same(r->doc.operation, "current_work")) {
    WRITE(boards ? "No boards.\n" : "No tasks.\n");
  }
  if (r->doc.url != NULL) {
    CELL(r->doc.url);
    WRITE("\n\n");
  }
  if (r->doc.snippet != NULL) {
    CELL(r->doc.snippet);
    WRITE("\n");
  }
  if (r->doc.truncated)
    WRITE("\nResult limit reached; some entries are omitted.\n");
  return CAI_OK;
failed:
  return cai_cli_error(error, CAI_ERR_TRANSPORT, "render built-in tool result");
}
#undef WRITE
#undef CELL
