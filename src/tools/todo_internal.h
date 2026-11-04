#ifndef CAI_TODO_INTERNAL_H
#define CAI_TODO_INTERNAL_H
#include <cai/tools/todo.h>
/** Prepare a board snapshot response for the framework-neutral HTTP adapter.
 * head selects metadata-only HEAD handling; the response owns its resources. */
int cai_todo_http_snapshot(cai_todo *self, const char *board, int head,
                           cai_todo_http_response *response, cai_error *error);
#endif
