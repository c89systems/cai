/** @file Bounded backend multicast observation and reusable host tool
 * contracts. */
#define _POSIX_C_SOURCE 200809L
#include "cai_internal.h"
#include "cai_network_jsonl.h"
#include <cai/network.h>
#include <cai/network_tools.h>
#include <cai/utf8.h>
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

typedef struct listen_arguments {
  char address[1025], interface_name[128];
  lonejson_int64 duration_ms, max_bytes;
  int has_duration, has_max_bytes;
} listen_arguments;
static const lonejson_field listen_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(listen_arguments, address, "address",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED(listen_arguments, interface_name, "interface",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_I64_PRESENT(listen_arguments, duration_ms, has_duration,
                               "duration_ms"),
    LONEJSON_FIELD_I64_PRESENT(listen_arguments, max_bytes, has_max_bytes,
                               "max_bytes")};
LONEJSON_MAP_DEFINE(listen_map, listen_arguments, listen_fields);
int cai_network_tool_cancel_requested(void) {
  return cai_stream_thread_cancel_requested();
}
static uint64_t now_ms(void) {
  struct timespec now;
  (void)clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}
typedef struct listener_output {
  cai_sink *sink;
  cai_error *error;
} listener_output;
static lonejson_status output(void *context, const void *bytes, size_t count,
                              lonejson_error *error) {
  listener_output *out = context;
  (void)error;
  return cai_sink_write(out->sink, bytes, count, out->error) == CAI_OK
             ? LONEJSON_STATUS_OK
             : LONEJSON_STATUS_IO_ERROR;
}
static int listen_tool(void *context, const char *arguments, cai_sink *sink,
                       cai_error *error) {
  listen_arguments *args;
  cai_net_config config;
  cai_net *net = NULL;
  cai_network_jsonl *lines = NULL;
  cai_network_jsonl_stats observed;
  lonejson_error local;
  lonejson *json;
  lonejson_writer writer;
  listener_output destination;
  struct pollfd fd;
  unsigned char bytes[2048], scalar[4];
  size_t scalar_count = 0U, total = 0U;
  cai_utf8_state utf8;
  uint64_t until;
  int rc = CAI_OK, invalid = 0, truncated = 0;
  lonejson_status status;
  (void)context;
  args = calloc(1U, sizeof(*args));
  if (args == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "allocate multicast input");
  args->duration_ms = 1000;
  args->max_bytes = 32768;
  lonejson_error_init(&local);
  json = lonejson_new(NULL, &local);
  if (json == NULL) {
    free(args);
    return cai_set_error(error, CAI_ERR_NOMEM, "allocate multicast parser");
  }
  if (arguments == NULL ||
      lonejson_parse_buffer(json, &listen_map, args, arguments,
                            strlen(arguments), &local) != LONEJSON_STATUS_OK)
    rc = cai_set_error(error, CAI_ERR_INVALID,
                       "valid multicast arguments required");
  if (!args->has_duration)
    args->duration_ms = 1000;
  if (!args->has_max_bytes)
    args->max_bytes = 32768;
  if (rc == CAI_OK && (args->duration_ms < 1 || args->duration_ms > 60000 ||
                       args->max_bytes < 1 || args->max_bytes > 1048576))
    rc = cai_set_error(error, CAI_ERR_INVALID,
                       "duration_ms must be 1..60000; max_bytes 1..1048576");
  cai_net_config_init(&config);
  config.mode = CAI_NET_MULTICAST_RECEIVE;
  config.address = args->address;
  config.interface_name = args->interface_name;
  if (rc == CAI_OK)
    rc = cai_net_open(&config, &net, error);
  if (rc == CAI_OK)
    rc = cai_network_jsonl_open(&lines, error);
  memset(&observed, 0, sizeof(observed));
  memset(&writer, 0, sizeof(writer));
  memset(&utf8, 0, sizeof(utf8));
  destination.sink = sink;
  destination.error = error;
  status = rc == CAI_OK ? lonejson_writer_init_sink(json, &writer, output,
                                                    &destination, &local)
                        : LONEJSON_STATUS_IO_ERROR;
#define EMIT(call)                                                             \
  do {                                                                         \
    if (status == LONEJSON_STATUS_OK)                                          \
      status = (call);                                                         \
  } while (0)
#define KEY(name)                                                              \
  EMIT(lonejson_writer_key(&writer, name, sizeof(name) - 1U, &local))
  EMIT(lonejson_writer_begin_object(&writer, &local));
  KEY("content");
  EMIT(lonejson_writer_string_begin(&writer, &local));
  until = now_ms() + (uint64_t)args->duration_ms;
  fd.fd = cai_net_fd(net);
  fd.events = POLLIN;
  while (rc == CAI_OK && status == LONEJSON_STATUS_OK && now_ms() < until &&
         total < (size_t)args->max_bytes) {
    uint64_t remaining = until - now_ms();
    int ready;
    ssize_t count;
    size_t i, used;
    if (cai_stream_thread_cancel_requested()) {
      rc = cai_set_error(error, CAI_ERR_CANCELLED,
                         "multicast observation cancelled");
      break;
    }
    ready = poll(&fd, 1U, remaining > 50U ? 50 : (int)remaining);
    if (ready < 0) {
      if (errno == EINTR)
        continue;
      rc = cai_set_error(error, CAI_ERR_TRANSPORT, "poll multicast stream");
      break;
    }
    if (!ready)
      continue;
    count = recv(fd.fd, bytes, sizeof(bytes), MSG_TRUNC);
    if (count < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        continue;
      rc = cai_set_error(error, CAI_ERR_TRANSPORT, "receive multicast stream");
      break;
    }
    used = (size_t)count;
    if (used > sizeof(bytes)) {
      used = sizeof(bytes);
      truncated = 1;
    }
    if (used > (size_t)args->max_bytes - total) {
      used = (size_t)args->max_bytes - total;
      truncated = 1;
    }
    total += used;
    cai_network_jsonl_feed(lines, bytes, used);
    for (i = 0U; i < used && status == LONEJSON_STATUS_OK; i++) {
      if (scalar_count == sizeof(scalar) ||
          !cai_utf8_feed(&utf8, bytes + i, 1U)) {
        invalid = 1;
        scalar_count = 0U;
        memset(&utf8, 0, sizeof(utf8));
        EMIT(lonejson_writer_string_chunk(&writer, "?", 1U, &local));
        continue;
      }
      scalar[scalar_count++] = bytes[i];
      if (cai_utf8_complete(&utf8)) {
        EMIT(lonejson_writer_string_chunk(&writer, scalar, scalar_count,
                                          &local));
        scalar_count = 0U;
      }
    }
  }
  invalid |= scalar_count != 0U;
  truncated |= total == (size_t)args->max_bytes;
  EMIT(lonejson_writer_string_end(&writer, &local));
  KEY("bytes");
  EMIT(lonejson_writer_u64(&writer, total, &local));
  KEY("truncated");
  EMIT(lonejson_writer_bool(&writer, truncated, &local));
  KEY("invalid_utf8");
  EMIT(lonejson_writer_bool(&writer, invalid, &local));
  KEY("external_data");
  EMIT(lonejson_writer_bool(&writer, 1, &local));
  if (lines != NULL)
    cai_network_jsonl_get(lines, &observed);
  KEY("events");
  EMIT(lonejson_writer_u64(&writer, observed.events, &local));
  KEY("invalid_lines");
  EMIT(lonejson_writer_u64(&writer, observed.invalid_lines, &local));
  KEY("partial_line");
  EMIT(lonejson_writer_bool(&writer, observed.partial_line, &local));
  EMIT(lonejson_writer_end_object(&writer, &local));
  EMIT(lonejson_writer_finish(&writer, &local));
#undef KEY
#undef EMIT
  if (rc == CAI_OK && status != LONEJSON_STATUS_OK)
    rc = cai_set_error(error, CAI_ERR_PROTOCOL, local.message);
  lonejson_writer_cleanup(&writer);
  lonejson_free(json);
  free(args);
  cai_net_close(net, 0);
  cai_network_jsonl_close(lines);
  return rc;
}
static int host_call(void *context, const char *name, const char *args,
                     cai_sink *sink, cai_error *error) {
  const cai_network_tools_config *config = context;
  return config != NULL && config->network_tools && config->call != NULL
             ? config->call(config->context, name, args, sink, error)
             : cai_set_error(error, CAI_ERR_INVALID,
                             "network frontend capability unavailable");
}
#define HOST_TOOL(name)                                                        \
  static int name##_tool(void *ctx, const char *args, cai_sink *out,           \
                         cai_error *e) {                                       \
    return host_call(ctx, #name, args, out, e);                                \
  }
HOST_TOOL(network_list)
HOST_TOOL(prompt_listener_info)
HOST_TOOL(prompt_send)
#undef HOST_TOOL
typedef struct network_tool {
  const char *name, *description, *schema;
  cai_tool_raw_fn callback;
} network_tool;
#define SCHEMA(properties, required)                                           \
  "{\"type\":\"object\",\"properties\":{" properties                           \
  "},\"required\":[" required "],\"additionalProperties\":false}"
#define ADDRESS "\"address\":{\"type\":\"string\"}"
#define ATTACHMENT "\"attachment_id\":{\"type\":\"integer\",\"minimum\":1}"
static const network_tool tools[] = {
    {"event_stream_listen",
     "Observe external, best-effort journal JSONL from a multicast group on "
     "demand. Loss, reordering and joining mid-line may corrupt records. "
     "Bounded duration/output; no delivery, identity or completion guarantee. "
     "Invalid UTF-8 is replaced and reported.",
     SCHEMA(ADDRESS
            ",\"interface\":{\"type\":\"string\"},\"duration_ms\":{\"type\":"
            "\"integer\",\"minimum\":1,\"maximum\":60000},\"max_bytes\":{"
            "\"type\":\"integer\",\"minimum\":1,\"maximum\":1048576}",
            "\"address\""),
     listen_tool},
    {"network_list",
     "List a bounded page of registered listeners and event publishers across "
     "this store. Follow the returned cursor with the same workspace filter. "
     "Registrations are discovery hints; inspect locality and availability. No "
     "network messages are sent.",
     SCHEMA(
         "\"workspace\":{\"type\":\"string\"},\"cursor\":{\"type\":\"string\"}",
         ""),
     network_list_tool},
    {"prompt_listener_info",
     "Get a selected frontend attachment's usable reply endpoint and explicit "
     "reply-tool instructions. Omit attachment only for an unambiguous "
     "controller. Endpoints belong to attachments, not saved sessions.",
     SCHEMA(ATTACHMENT, ""), prompt_listener_info_tool},
    {"prompt_send",
     "Request one TCP transfer by a selected controlling frontend. The backend "
     "never opens the TCP connection. Successful transmission does not prove "
     "remote admission or inference. No automatic retry; uncertain delivery "
     "requires inspection. Include reply instructions only when requested.",
     SCHEMA(ADDRESS
            "," ATTACHMENT
            ",\"prompt\":{\"type\":\"string\",\"maxLength\":66048},\"timeout_"
            "ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":60000},"
            "\"include_reply\":{\"type\":\"boolean\"}",
            "\"address\",\"prompt\""),
     prompt_send_tool}};
#undef SCHEMA
#undef ADDRESS
#undef ATTACHMENT
int cai_network_tools_register(cai_agent *agent,
                               cai_network_tools_config *config,
                               cai_error *error) {
  size_t i;
  int rc = CAI_OK;
  for (i = 0U; rc == CAI_OK && i < sizeof(tools) / sizeof(tools[0]); i++) {
    if (i == 0U ? config != NULL && config->disable_event_stream_listener
                : config == NULL || !config->network_tools)
      continue;
    rc = cai_agent_register_raw_tool(agent, tools[i].name, tools[i].description,
                                     tools[i].schema, 0, tools[i].callback,
                                     config, error);
  }
  return rc;
}
int cai_network_tools_register_registry(cai_tool_registry *registry,
                                        cai_network_tools_config *config,
                                        cai_error *error) {
  size_t i;
  int rc = CAI_OK;
  for (i = 0U; rc == CAI_OK && i < sizeof(tools) / sizeof(tools[0]); i++) {
    if (i == 0U ? config != NULL && config->disable_event_stream_listener
                : config == NULL || !config->network_tools)
      continue;
    rc = cai_tool_registry_register_raw(registry, tools[i].name,
                                        tools[i].description, tools[i].schema,
                                        0, tools[i].callback, config, error);
  }
  return rc;
}
