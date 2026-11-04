#ifndef CAI_CHROME_PROTOCOL_H
#define CAI_CHROME_PROTOCOL_H
#include "cai_internal.h"
#include <cai/chrome.h>
#include <pslog.h>
#include <stdint.h>
#define CAI_CHROME_REQUEST_LIMIT (1024U * 1024U)
#define CAI_CHROME_RESPONSE_LIMIT (16U * 1024U * 1024U)
#define CAI_CHROME_CLEANUP_TIMEOUT_MS 5000L
/** A mapped, bounded JSON-RPC frame. The values preserve JSON null/results. */
typedef struct cai_chrome_rpc_error {
  int has_code;
  int64_t code;
  const char *message;
} cai_chrome_rpc_error;
#define CAI_CHROME_JSON_FIELD(type, member, key)                               \
  {key,                                                                        \
   LONEJSON__KEY_LEN(key),                                                     \
   LONEJSON__KEY_FIRST(key),                                                   \
   LONEJSON__KEY_LAST(key),                                                    \
   offsetof(type, member),                                                     \
   LONEJSON_FIELD_KIND_JSON_VALUE,                                             \
   LONEJSON_STORAGE_FIXED,                                                     \
   LONEJSON_OVERFLOW_FAIL,                                                     \
   LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE,                                 \
   0U,                                                                         \
   0U,                                                                         \
   NULL,                                                                       \
   NULL,                                                                       \
   0U,                                                                         \
   LONEJSON_SPOOL_CLASS_DEFAULT}
/** Parsed RPC envelope with independent member-presence flags.
 * A missing result differs from an explicit JSON null result. */
typedef struct cai_chrome_rpc {
  int has_id, has_params, has_result, has_error;
  const char *jsonrpc;
  lonejson_json_value id;
  const char *method;
  lonejson_json_value params;
  lonejson_json_value result;
  cai_chrome_rpc_error error;
} cai_chrome_rpc;
/** Decoder map for complete extension RPC envelopes. */
extern const lonejson_map cai_chrome_rpc_map;
/** Encoder map for outbound RPC method/parameter requests. */
extern const lonejson_map cai_chrome_request_map;
/** Encoder map for successful RPC replies. */
extern const lonejson_map cai_chrome_success_map;
/** Encoder map for failed RPC replies. */
extern const lonejson_map cai_chrome_failure_map;
/** reply_method is the outstanding request method, or NULL for a request or
 * unsolicited frame. Extension void successes may omit the result member. */
int cai_chrome_parse(const char *json, const char *reply_method,
                     cai_chrome_rpc *rpc, cai_error *error);
/** Serialize a mapped RPC frame into a caller-owned JSON string. */
char *cai_chrome_serialize(const lonejson_map *map, const void *doc,
                           cai_error *error);
/** Materialize one stored JSON value into a caller-owned string. */
char *cai_chrome_value(const lonejson_json_value *value, cai_error *error);
/** Set a protocol JSON value from explicitly supplied complete JSON text. */
int cai_chrome_value_buffer(lonejson_json_value *value, const char *json,
                            cai_error *error);
/** Read the monotonic clock used for Chrome operation deadlines. */
long long cai_chrome_now_ms(void);
/** Create/validate a private native-host socket/control directory. */
int cai_chrome_private_directory(const char *path, cai_error *error);
/** Connect to a native-host Unix socket, returning an owned descriptor. */
int cai_chrome_connect_socket(const char *path, int *out, cai_error *error);
/** Private shutdown endpoint; never forwarded to the extension. */
typedef struct cai_chrome_stop_request {
  const char *executable;
} cai_chrome_stop_request;
extern const lonejson_map cai_chrome_stop_map;
/** Tool state lives in the connection, borrowed by a registry. */
typedef struct cai_chrome_tool_tab {
  struct cai_chrome_tool_tab *next;
  int64_t id;
} cai_chrome_tool_tab;
/** Release locally tracked tab claims without freeing the connection. */
void cai_chrome_clear_tool_tabs(cai_chrome *chrome);
/** Connection-owned Chrome transport, lazy discovery and tab-claim state.
 * Keeping a browser tab does not retain its old agent turn's local claim. */
struct cai_chrome {
  int fd;
  long timeout_ms;
  size_t limit;
  unsigned long sequence;
  pslog_logger *logger;
  char *session_id;
  char *turn_id;
  char *pending_image;
  cai_chrome_tool_tab *tool_tabs;
  int used;
  int cleanup;
  int lazy;
  char *socket_path;
  char *socket_directory;
};
#endif
