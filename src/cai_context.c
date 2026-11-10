#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "cai_context_internal.h"
#include <cai/context.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <lonejson.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CONTEXT_PREVIEW_BYTES 16384U
#define CONTEXT_PAGE_KEYS 32U
#define CONTEXT_META_BYTES 1024U
struct cai_context {
  lc_client *client;
  char *ns, *root;
  char owner[LC_XID_STRING_SIZE];
  int root_fd, yolo;
};
typedef struct context_args {
  char *id, *path, *title, *filename, *date, *media_type, *query, *cursor;
  lonejson_string_array tags;
  lonejson_int64 expected_version, limit;
  int has_version;
  unsigned metadata;
} context_args;
static const lonejson_field args_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(context_args, id, "id"),
    LONEJSON_FIELD_STRING_ALLOC(context_args, path, "path"),
    LONEJSON_FIELD_STRING_ALLOC(context_args, title, "title"),
    LONEJSON_FIELD_STRING_ALLOC(context_args, filename, "filename"),
    LONEJSON_FIELD_STRING_ALLOC(context_args, date, "date"),
    LONEJSON_FIELD_STRING_ALLOC(context_args, media_type, "media_type"),
    LONEJSON_FIELD_STRING_ALLOC(context_args, query, "query"),
    LONEJSON_FIELD_STRING_ALLOC(context_args, cursor, "cursor"),
    LONEJSON_FIELD_STRING_ARRAY(context_args, tags, "tags",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_I64_PRESENT(context_args, expected_version, has_version,
                               "expected_version"),
    LONEJSON_FIELD_I64(context_args, limit, "limit")};
LONEJSON_MAP_DEFINE(args_map, context_args, args_fields);
/** Metadata is bounded; content is always delivered incrementally to a visitor.
 */
typedef struct context_doc {
  char id[21], kind[8], title[1025], filename[256], date[128], media_type[128];
  lonejson_int64 created_at, updated_at, size;
  lonejson_string_array tags;
  lonejson_json_value content;
} context_doc;
static const lonejson_field doc_fields[] = {
    LONEJSON_FIELD_STRING_FIXED(context_doc, id, "id", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED(context_doc, kind, "kind",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED(context_doc, title, "title",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED(context_doc, filename, "filename",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED(context_doc, date, "date",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED(context_doc, media_type, "media_type",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_I64(context_doc, created_at, "created_at"),
    LONEJSON_FIELD_I64(context_doc, updated_at, "updated_at"),
    LONEJSON_FIELD_I64(context_doc, size, "size"),
    LONEJSON_FIELD_STRING_ARRAY(context_doc, tags, "tags",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_JSON_VALUE(context_doc, content, "content")};
LONEJSON_MAP_DEFINE(doc_map, context_doc, doc_fields);
/* The metadata serializer deliberately omits content. */
static const lonejson_field meta_fields[] = {
    LONEJSON_FIELD_STRING_FIXED(context_doc, id, "id", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED(context_doc, kind, "kind",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED(context_doc, title, "title",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED(context_doc, filename, "filename",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED(context_doc, date, "date",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED(context_doc, media_type, "media_type",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_I64(context_doc, created_at, "created_at"),
    LONEJSON_FIELD_I64(context_doc, updated_at, "updated_at"),
    LONEJSON_FIELD_I64(context_doc, size, "size"),
    LONEJSON_FIELD_STRING_ARRAY(context_doc, tags, "tags",
                                LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(meta_map, context_doc, meta_fields);
static int fail(cai_error *e, int code, const char *message) {
  if (e != NULL) {
    cai_error_cleanup(e);
    e->code = code;
    e->message = strdup(message != NULL ? message : "context operation failed");
  }
  return code;
}
static int from_lc(cai_error *e, const lc_error *l) {
  int code = l->code == LC_ERR_NOMEM ? CAI_ERR_NOMEM
             : l->code == LC_ERR_TRANSPORT || l->code == LC_ERR_TIMEOUT
                 ? CAI_ERR_TRANSPORT
             : l->http_status ? CAI_ERR_SERVER
                              : CAI_ERR_INVALID;
  fail(e, code, l->message);
  if (e != NULL) {
    e->http_status = l->http_status;
    if (l->detail != NULL)
      e->detail = strdup(l->detail);
    if (l->server_code != NULL)
      e->server_code = strdup(l->server_code);
    if (l->correlation_id != NULL)
      e->request_id = strdup(l->correlation_id);
  }
  return code;
}
static int from_lj(cai_error *e, const lonejson_error *l) {
  return fail(e, CAI_ERR_INVALID, l->message);
}
void cai_context_config_init(cai_context_config *c) {
  memset(c, 0, sizeof(*c));
}
int cai_context_open(const cai_context_config *c, cai_context **out,
                     cai_error *e) {
  cai_context *p;
  const char *ns;
  lc_error local;
  if (out == NULL)
    return fail(e, CAI_ERR_INVALID, "context output is required");
  *out = NULL;
  if (c == NULL || c->client == NULL)
    return fail(e, CAI_ERR_INVALID,
                "context requires a borrowed lockdc client");
  ns = c->namespace_name != NULL ? c->namespace_name : "cai.context";
  if (!*ns || strlen(ns) > 128U ||
      strspn(ns, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ012345678"
                 "9._-") != strlen(ns))
    return fail(e, CAI_ERR_INVALID, "invalid context namespace");
  p = calloc(1U, sizeof(*p));
  if (p == NULL)
    return fail(e, CAI_ERR_NOMEM, "allocate context adapter");
  p->root_fd = -1;
  p->ns = strdup(ns);
  p->client = c->client;
  p->yolo = c->yolo;
  lc_error_init(&local);
  if (lc_xid_new(p->owner, &local) != LC_OK) {
    int rc = from_lc(e, &local);
    lc_error_cleanup(&local);
    cai_context_close(p);
    return rc;
  }
  lc_error_cleanup(&local);
  if (c->root_path != NULL) {
    p->root = realpath(c->root_path, NULL);
    if (p->root != NULL)
      p->root_fd = open(p->root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (p->root_fd < 0) {
      cai_context_close(p);
      return fail(e, CAI_ERR_INVALID, "open context filesystem root");
    }
  }
  if (p->ns == NULL) {
    cai_context_close(p);
    return fail(e, CAI_ERR_NOMEM, "copy context namespace");
  }
  *out = p;
  return CAI_OK;
}
void cai_context_close(cai_context *p) {
  if (p != NULL) {
    if (p->root_fd >= 0)
      close(p->root_fd);
    free(p->root);
    free(p->ns);
    free(p);
  }
}
void cai_context_set_yolo(cai_context *p, int enabled) {
  if (p != NULL)
    p->yolo = enabled != 0;
}
int cai_context_files_available(const cai_context *context) {
  return context != NULL && context->root_fd >= 0;
}
static int valid_filename(const char *s) {
  size_t i;
  if (s == NULL)
    return 1;
  if (!*s || strlen(s) > 255U || !strcmp(s, ".") || !strcmp(s, ".."))
    return 0;
  for (i = 0U; s[i]; i++)
    if ((unsigned char)s[i] < 32U || s[i] == 127 || s[i] == '/' || s[i] == '\\')
      return 0;
  return 1;
}
/** Walk descriptors, never resolve a sandbox check separately from opening. */
static int open_path(cai_context *p, const char *path, int writing,
                     int directory, cai_error *e) {
  char *copy, *part, *next;
  const char *relative = path;
  int parent, fd = -1;
  size_t n;
  if (p->root_fd < 0 || path == NULL || !*path) {
    fail(e, CAI_ERR_INVALID, "context file path/root required");
    return -1;
  }
  if (p->yolo) {
    fd = openat(p->root_fd, path,
                (directory ? O_RDONLY | O_DIRECTORY
                 : writing ? O_WRONLY | O_CREAT | O_EXCL
                           : O_RDONLY | O_NONBLOCK) |
                    O_CLOEXEC | (writing ? O_NOFOLLOW : 0),
                0600);
    if (fd < 0)
      fail(e, CAI_ERR_INVALID, "open context file (downloads never overwrite)");
    return fd;
  }
  if (path[0] == '/') {
    n = strlen(p->root);
    if (strncmp(path, p->root, n) ||
        (n != 1U && path[n] != '/' && path[n] != '\0')) {
      fail(e, CAI_ERR_INVALID, "context path is outside workspace");
      return -1;
    }
    relative = path + n;
    if (*relative == '/')
      relative++;
  }
  copy = strdup(relative);
  if (copy == NULL) {
    fail(e, CAI_ERR_NOMEM, "allocate context path");
    return -1;
  }
  parent = dup(p->root_fd);
  if (!*relative && directory) {
    free(copy);
    if (parent < 0)
      fail(e, CAI_ERR_INVALID, "open context directory");
    return parent;
  }
  part = copy;
  if (parent < 0)
    goto done;
  while (part != NULL && *part) {
    next = strchr(part, '/');
    if (next != NULL)
      *next++ = '\0';
    if (!strcmp(part, "..") || !*part)
      break;
    if (!strcmp(part, ".")) {
      if (next == NULL) {
        if (directory)
          fd = dup(parent);
        break;
      }
      part = next;
      continue;
    }
    if (next == NULL) {
      fd = openat(parent, part,
                  (directory ? O_RDONLY | O_DIRECTORY
                   : writing ? O_WRONLY | O_CREAT | O_EXCL
                             : O_RDONLY | O_NONBLOCK) |
                      O_CLOEXEC | O_NOFOLLOW,
                  0600);
      break;
    }
    fd = openat(parent, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(parent);
    parent = fd;
    fd = -1;
    if (parent < 0)
      break;
    part = next;
  }
  if (parent >= 0)
    close(parent);
done:
  free(copy);
  if (fd < 0)
    fail(e, CAI_ERR_INVALID,
         "open context path: missing, unsafe, or already exists");
  return fd;
}
/** UTF-8 classification covers every byte, not merely an extension or prefix.
 */
static int classify(int fd, int *text, struct stat *before, cai_error *e) {
  unsigned char b[2048];
  unsigned long value = 0UL, minimum = 0UL;
  unsigned remaining = 0U;
  ssize_t n;
  size_t i;
  int first = 1;
  *text = 1;
  if (fstat(fd, before) || !S_ISREG(before->st_mode))
    return fail(e, CAI_ERR_INVALID, "context upload requires a regular file");
  while ((n = read(fd, b, sizeof(b))) != 0) {
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return fail(e, CAI_ERR_INVALID, "read context upload");
    }
    if (first && ((n >= 5 && !memcmp(b, "%PDF-", 5U)) ||
                  (n >= 4 && !memcmp(b, "PK\003\004", 4U))))
      *text = 0;
    first = 0;
    for (i = 0U; i < (size_t)n && *text; i++) {
      unsigned c = b[i];
      if (remaining) {
        if ((c & 0xc0U) != 0x80U) {
          *text = 0;
          break;
        }
        value = (value << 6) | (c & 63U);
        if (--remaining == 0U && (value < minimum || value > 0x10ffffUL ||
                                  (value >= 0xd800UL && value <= 0xdfffUL)))
          *text = 0;
      } else if (c < 128U) {
        if ((c < 32U && c != '\n' && c != '\r' && c != '\t' && c != '\f') ||
            c == 127U)
          *text = 0;
      } else if (c >= 0xc2U && c <= 0xdfU) {
        remaining = 1U;
        value = c & 31U;
        minimum = 128UL;
      } else if (c >= 0xe0U && c <= 0xefU) {
        remaining = 2U;
        value = c & 15U;
        minimum = 2048UL;
      } else if (c >= 0xf0U && c <= 0xf4U) {
        remaining = 3U;
        value = c & 7U;
        minimum = 65536UL;
      } else
        *text = 0;
    }
    if (!*text)
      break;
  }
  if (remaining)
    *text = 0;
  if (lseek(fd, 0, SEEK_SET) < 0)
    return fail(e, CAI_ERR_INVALID, "rewind context upload");
  return CAI_OK;
}
static int acquire(cai_context *p, const char *id, int create, lc_lease **lease,
                   lc_error *e) {
  lc_acquire_req req;
  lc_acquire_req_init(&req);
  req.ns = p->ns;
  req.key = id;
  req.owner = p->owner;
  req.ttl_seconds = 300;
  req.if_not_exists = create;
  return p->client->acquire(p->client, &req, lease, e);
}
/** Long transfers renew at their next chunk; there is no polling service. */
static int renew(lc_lease *lease, lc_error *error) {
  lc_keepalive_req request;
  if (lease == NULL ||
      lease->lease_expires_at_unix > (lc_unix_seconds)time(NULL) + 150)
    return LC_OK;
  lc_keepalive_req_init(&request);
  request.ttl_seconds = 300;
  return lease->keepalive(lease, &request, error);
}
static int finish(lc_lease *lease, int rc, lc_error *e) {
  lc_release_req req;
  lc_error local;
  int released;
  lc_release_req_init(&req);
  req.rollback = rc != LC_OK;
  lc_error_init(&local);
  released = lease->release(lease, &req, &local);
  if (released != LC_OK) {
    lease->close(lease);
    if (rc == LC_OK) {
      lc_error_cleanup(e);
      *e = local;
      memset(&local, 0, sizeof(local));
      rc = released;
    }
  }
  lc_error_cleanup(&local);
  return rc;
}
static lonejson_status output_write(void *ctx, const void *bytes, size_t n,
                                    lonejson_error *e) {
  cai_error error;
  int rc;
  (void)e;
  cai_error_init(&error);
  rc = cai_sink_write(ctx, bytes, n, &error);
  cai_error_cleanup(&error);
  return rc == CAI_OK ? LONEJSON_STATUS_OK : LONEJSON_STATUS_IO_ERROR;
}
static int receipt(cai_sink *sink, const char *id, lc_version version,
                   const char *kind, const char *status, const char *path,
                   cai_error *e) {
  lonejson_writer w;
  lonejson_error l;
  lonejson *j;
  lonejson_status rc;
  lonejson_error_init(&l);
  j = lonejson_new(NULL, &l);
  if (j == NULL)
    return fail(e, CAI_ERR_NOMEM, "allocate context receipt");
  memset(&w, 0, sizeof(w));
  rc = lonejson_writer_init_sink(j, &w, output_write, sink, &l);
#define EMIT(x)                                                                \
  do {                                                                         \
    if (rc == LONEJSON_STATUS_OK)                                              \
      rc = (x);                                                                \
  } while (0)
#define KV(k, v)                                                               \
  do {                                                                         \
    EMIT(lonejson_writer_key(&w, k, strlen(k), &l));                           \
    EMIT(lonejson_writer_string(&w, v, strlen(v), &l));                        \
  } while (0)
  EMIT(lonejson_writer_begin_object(&w, &l));
  KV("id", id);
  KV("status", status);
  KV("kind", kind);
  EMIT(lonejson_writer_key(&w, "version", 7U, &l));
  EMIT(lonejson_writer_i64(&w, version, &l));
  if (path != NULL)
    KV("path", path);
  EMIT(lonejson_writer_end_object(&w, &l));
  EMIT(lonejson_writer_finish(&w, &l));
#undef KV
#undef EMIT
  lonejson_writer_cleanup(&w);
  lonejson_free(j);
  return rc == LONEJSON_STATUS_OK ? CAI_OK : from_lj(e, &l);
}
/** Pull generator: writer backpressure retains only one raw text chunk. */
typedef struct create_stream {
  context_doc *doc;
  cai_source *text;
  lc_lease *lease;
  lonejson_generator generator;
  lonejson *json;
  unsigned phase;
  unsigned char chunk[1024];
  size_t length;
  size_t tag_index;
  int pending, eof;
  cai_error source_error;
} create_stream;
static lonejson_status produce(lonejson_writer *w, void *ctx,
                               lonejson_error *e) {
  create_stream *s = ctx;
  lonejson_status rc;
  cai_error ce;
  const char *key = NULL, *value = NULL;
  for (;;) {
    switch (s->phase) {
    case 0:
      rc = lonejson_writer_begin_object(w, e);
      break;
    case 1:
      key = "id";
      goto key_event;
    case 2:
      value = s->doc->id;
      goto string_event;
    case 3:
      key = "kind";
      goto key_event;
    case 4:
      value = s->doc->kind;
      goto string_event;
    case 5:
      key = "filename";
      goto key_event;
    case 6:
      value = s->doc->filename;
      goto string_event;
    case 7:
      key = "media_type";
      goto key_event;
    case 8:
      value = s->doc->media_type;
      goto string_event;
    case 9:
      key = "created_at";
      goto key_event;
    case 10:
      rc = lonejson_writer_i64(w, s->doc->created_at, e);
      break;
    case 11:
      key = "updated_at";
      goto key_event;
    case 12:
      rc = lonejson_writer_i64(w, s->doc->updated_at, e);
      break;
    case 13:
      s->phase = 15U;
      continue;
    case 15:
      key = "title";
      goto key_event;
    case 16:
      value = s->doc->title;
      goto string_event;
    case 17:
      key = "date";
      goto key_event;
    case 18:
      value = s->doc->date;
      goto string_event;
    case 19:
      key = "tags";
      goto key_event;
    case 20:
      rc = lonejson_writer_begin_array(w, e);
      break;
    case 21:
      if (s->tag_index == s->doc->tags.count) {
        s->phase++;
        continue;
      }
      value = s->doc->tags.items[s->tag_index];
      rc = lonejson_writer_string(w, value, strlen(value), e);
      if (rc != LONEJSON_STATUS_OK)
        return rc;
      s->tag_index++;
      continue;
    case 22:
      rc = lonejson_writer_end_array(w, e);
      break;
    case 23:
      if (s->text == NULL) {
        s->phase = 27U;
        continue;
      }
      key = "content";
      goto key_event;
    case 24:
      rc = lonejson_writer_string_begin(w, e);
      break;
    case 25:
      if (!s->pending) {
        cai_error_init(&ce);
        s->length = s->text->read(s->text, s->chunk, sizeof(s->chunk), &ce);
        if (ce.code != CAI_OK) {
          s->source_error = ce;
          return LONEJSON_STATUS_IO_ERROR;
        }
        cai_error_cleanup(&ce);
        if (s->doc->size > INT64_MAX - (lonejson_int64)s->length)
          return LONEJSON_STATUS_IO_ERROR;
        s->doc->size += (lonejson_int64)s->length;
        s->eof = s->length == 0U;
        s->pending = 1;
      }
      if (s->eof) {
        s->phase++;
        continue;
      }
      rc = lonejson_writer_string_chunk(w, s->chunk, s->length, e);
      if (rc != LONEJSON_STATUS_OK)
        return rc;
      s->pending = 0;
      continue;
    case 26:
      rc = lonejson_writer_string_end(w, e);
      break;
    case 27:
      key = "size";
      goto key_event;
    case 28:
      rc = lonejson_writer_i64(w, s->doc->size, e);
      break;
    case 29:
      rc = lonejson_writer_end_object(w, e);
      break;
    default:
      return LONEJSON_STATUS_OK;
    }
    if (rc != LONEJSON_STATUS_OK)
      return rc;
    s->phase++;
    continue;
  key_event:
    rc = lonejson_writer_key(w, key, strlen(key), e);
    if (rc != LONEJSON_STATUS_OK)
      return rc;
    s->phase++;
    continue;
  string_event:
    rc = lonejson_writer_string(w, value, strlen(value), e);
    if (rc != LONEJSON_STATUS_OK)
      return rc;
    s->phase++;
  }
}
static size_t generated_read(void *ctx, void *b, size_t n, lc_error *e) {
  create_stream *s = ctx;
  size_t count = 0U;
  int eof = 0;
  if (renew(s->lease, e) != LC_OK)
    return 0U;
  if (lonejson_generator_read(&s->generator, b, n, &count, &eof) !=
      LONEJSON_STATUS_OK) {
    lc_error_set(
        e, s->source_error.code != CAI_OK ? LC_ERR_TRANSPORT : LC_ERR_INVALID,
        0,
        s->source_error.message != NULL ? s->source_error.message
                                        : "encode context content",
        s->generator.error.message, NULL, NULL);
    return 0U;
  }
  return count;
}
static int save_content(lc_lease *lease, context_doc *doc, cai_source *text,
                        lc_error *e) {
  create_stream *s = calloc(1U, sizeof(*s));
  lc_source *source = NULL;
  lonejson_error l;
  int rc;
  if (s == NULL)
    return lc_error_set(e, LC_ERR_NOMEM, 0, "allocate context encoder", NULL,
                        NULL, NULL);
  s->doc = doc;
  s->text = text;
  s->lease = lease;
  if (text != NULL)
    doc->size = 0;
  lonejson_error_init(&l);
  s->json = lonejson_new(NULL, &l);
  rc = s->json != NULL &&
               lonejson_writer_generator_init(s->json, &s->generator, produce,
                                              s) == LONEJSON_STATUS_OK
           ? LC_OK
           : LC_ERR_NOMEM;
  if (rc == LC_OK)
    rc = lc_source_from_callbacks(generated_read, NULL, NULL, s, &source, e);
  if (rc == LC_OK)
    rc = lease->update(lease, source, NULL, e);
  lc_source_close(source);
  lonejson_generator_cleanup(&s->generator);
  lonejson_free(s->json);
  cai_error_cleanup(&s->source_error);
  free(s);
  return rc;
}
int cai_context_create(cai_context *p, cai_source *text, cai_sink *out,
                       cai_error *e) {
  context_doc *doc;
  lc_lease *lease = NULL;
  lc_error l;
  lc_version version = 0;
  int rc;
  if (p == NULL || text == NULL || out == NULL)
    return fail(e, CAI_ERR_INVALID,
                "context creation requires source and receipt");
  doc = calloc(1U, sizeof(*doc));
  if (doc == NULL)
    return fail(e, CAI_ERR_NOMEM, "allocate context document");
  lc_error_init(&l);
  strcpy(doc->kind, "text");
  strcpy(doc->media_type, "text/markdown");
  doc->created_at = doc->updated_at = (lonejson_int64)time(NULL);
  rc = lc_xid_new(doc->id, &l);
  if (rc == LC_OK)
    rc = acquire(p, doc->id, 1, &lease, &l);
  if (rc == LC_OK) {
    rc = save_content(lease, doc, text, &l);
    version = lease->version;
    rc = finish(lease, rc, &l);
  }
  rc = rc == LC_OK ? receipt(out, doc->id, version, "text", "created", NULL, e)
                   : from_lc(e, &l);
  lc_error_cleanup(&l);
  free(doc);
  return rc;
}

/* SDK push bytes feed lonejson's incremental parser directly. No JSON copy. */
typedef struct state_reader {
  lonejson_curl_parse parser;
  lonejson_value_visitor visitor;
  cai_sink *content;
  cai_error *error;
  lc_lease *lease;
  char *preview;
  size_t used;
  int truncated, strings, sink_failed;
} state_reader;
static lonejson_status content_begin(void *ctx, lonejson_error *e) {
  state_reader *s = ctx;
  (void)e;
  s->strings++;
  return LONEJSON_STATUS_OK;
}
static lonejson_status content_container(void *ctx, lonejson_error *error) {
  (void)ctx;
  error->code = LONEJSON_STATUS_INVALID_ARGUMENT;
  snprintf(error->message, sizeof(error->message),
           "context content must be a JSON string");
  return error->code;
}
static lonejson_status content_chunk(void *ctx, const char *b, size_t n,
                                     lonejson_error *e) {
  state_reader *s = ctx;
  size_t take;
  (void)e;
  if (s->content != NULL &&
      cai_sink_write(s->content, b, n, s->error) != CAI_OK) {
    s->sink_failed = 1;
    return LONEJSON_STATUS_IO_ERROR;
  }
  if (s->preview != NULL) {
    take = n < CONTEXT_PREVIEW_BYTES - s->used
               ? n
               : CONTEXT_PREVIEW_BYTES - s->used;
    memcpy(s->preview + s->used, b, take);
    s->used += take;
    if (take < n)
      s->truncated = 1;
  }
  return LONEJSON_STATUS_OK;
}
static int parse_write(lc_sink *sink, const void *b, size_t n, lc_error *e) {
  state_reader *s = sink->impl;
  char buffer[2048];
  const unsigned char *bytes = b;
  size_t take;
  if (renew(s->lease, e) != LC_OK)
    return 0;
  while (n) {
    take = n < sizeof(buffer) ? n : sizeof(buffer);
    memcpy(buffer, bytes, take);
    if (s->parser.write_callback(&s->parser, buffer, 1U, take) != take) {
      lc_error_set(e, LC_ERR_INVALID, 0, "parse context state",
                   s->parser.error.message, NULL, NULL);
      return 0;
    }
    bytes += take;
    n -= take;
  }
  return 1;
}
static int read_doc(cai_context *p, const char *id, lc_lease *lease,
                    context_doc *doc, state_reader *s, lc_version *version,
                    cai_error *e) {
  lonejson *j;
  lonejson_error je;
  lonejson_config config;
  lc_error le;
  lc_get_opts opts;
  lc_get_res res;
  lc_sink sink;
  int rc;
  memset(&res, 0, sizeof(res));
  memset(&sink, 0, sizeof(sink));
  lc_error_init(&le);
  lonejson_error_init(&je);
  config = lonejson_default_config();
  config.max_alloc_bytes = 1024U * 1024U;
  config.max_dynamic_string_bytes = 128U;
  config.json_value_max_total_bytes = 0U;
  config.json_value_max_string_bytes = 0U;
  j = lonejson_new(&config, &je);
  if (j == NULL)
    return fail(e, CAI_ERR_NOMEM, "allocate context reader");
  s->error = e;
  s->visitor = lonejson_default_value_visitor();
  s->visitor.string_begin = content_begin;
  s->visitor.string_chunk = content_chunk;
  s->visitor.array_begin = content_container;
  s->visitor.object_begin = content_container;
  if (lonejson_curl_parse_init(&s->parser, j, &doc_map, doc) !=
      LONEJSON_STATUS_OK) {
    lonejson_free(j);
    return fail(e, CAI_ERR_NOMEM, "initialize context parser");
  }
  lonejson_json_value_set_parse_visitor(&doc->content, &s->visitor, s, &je);
  sink.write = parse_write;
  sink.impl = s;
  lc_get_opts_init(&opts);
  opts.public_read = lease == NULL;
  rc = lease != NULL ? lease->get(lease, &sink, &opts, &res, &le)
                     : p->client->get_in_namespace(p->client, p->ns, id, &opts,
                                                   &sink, &res, &le);
  if (rc == LC_OK && s->parser.finish(&s->parser) != LONEJSON_STATUS_OK)
    rc = lc_error_set(&le, LC_ERR_INVALID, 0, "invalid context document",
                      s->parser.error.message, NULL, NULL);
  if (rc == LC_OK &&
      (res.no_content || strcmp(doc->id, id) ||
       (strcmp(doc->kind, "text") && strcmp(doc->kind, "file")) ||
       (!strcmp(doc->kind, "text") && s->strings != 1) ||
       (!strcmp(doc->kind, "file") && s->strings != 0)))
    rc = lc_error_set(
        &le, LC_ERR_INVALID, 0,
        "context item does not exist or has an invalid kind/content", NULL,
        NULL, NULL);
  *version = res.version;
  lonejson_curl_parse_cleanup(&s->parser);
  lonejson_free(j);
  lc_get_res_cleanup(&res);
  if (rc != LC_OK)
    rc = s->sink_failed && e != NULL && e->code != CAI_OK ? e->code
                                                          : from_lc(e, &le);
  lc_error_cleanup(&le);
  return rc;
}
static lonejson_status metadata_presence(void *ctx,
                                         const lonejson_value_path *path,
                                         lonejson_error *e) {
  static const char *const names[] = {"title", "filename", "date", "media_type",
                                      "tags"};
  context_args *a = ctx;
  size_t i;
  (void)e;
  if (path->segment_count == 1U)
    for (i = 0U; i < 5U; i++)
      if (strlen(names[i]) == path->segments[0].len &&
          !memcmp(path->segments[0].data, names[i], path->segments[0].len))
        a->metadata |= 1U << i;
  return LONEJSON_STATUS_OK;
}
static int parse_args(const char *input, context_args *a, cai_error *e) {
  lonejson *j;
  lonejson_error l;
  lonejson_path_value_visitor v;
  lonejson_status rc;
  size_t i;
  if (input == NULL || strlen(input) > 65536U)
    return fail(e, CAI_ERR_INVALID, "context arguments exceed 64 KiB");
  lonejson_error_init(&l);
  j = lonejson_new(NULL, &l);
  if (j == NULL)
    return fail(e, CAI_ERR_NOMEM, "allocate context arguments");
  rc = lonejson_parse_cstr(j, &args_map, a, input, &l);
  v = lonejson_default_path_value_visitor();
  v.string_begin = metadata_presence;
  v.null_value = metadata_presence;
  v.array_begin = metadata_presence;
  if (rc == LONEJSON_STATUS_OK)
    rc = lonejson_visit_path_value_cstr(j, input, &v, a, &l);
  lonejson_free(j);
  if (rc != LONEJSON_STATUS_OK)
    return from_lj(e, &l);
  if ((a->title != NULL && strlen(a->title) > CONTEXT_META_BYTES) ||
      !valid_filename(a->filename) ||
      (a->date != NULL && strlen(a->date) > 127U) ||
      (a->media_type != NULL && strlen(a->media_type) > 127U) ||
      a->tags.count > 32U)
    return fail(e, CAI_ERR_INVALID, "invalid or oversized context metadata");
  for (i = 0U; i < a->tags.count; i++)
    if (strlen(a->tags.items[i]) > 128U)
      return fail(e, CAI_ERR_INVALID, "context tag exceeds 128 bytes");
  return CAI_OK;
}
/* Only explicitly bounded metadata and previews use this fixed-capacity sink.
 */
typedef struct bounded_text {
  char *bytes;
  size_t count, capacity;
} bounded_text;
static lonejson_status bounded_write(void *ctx, const void *b, size_t n,
                                     lonejson_error *e) {
  bounded_text *s = ctx;
  (void)e;
  if (n >= s->capacity - s->count)
    return LONEJSON_STATUS_IO_ERROR;
  memcpy(s->bytes + s->count, b, n);
  s->count += n;
  s->bytes[s->count] = '\0';
  return LONEJSON_STATUS_OK;
}
static int set_metadata(lc_lease *lease, context_args *a, cai_error *e) {
  static const char *const names[] = {"title", "filename", "date",
                                      "media_type"};
  const char *values[4];
  const char *mutations[5];
  bounded_text *buffers;
  lonejson_writer w;
  lonejson_error je;
  lonejson *j;
  lc_mutate_req req;
  lc_error le;
  size_t i, count = 0U;
  int rc = CAI_OK;
  lonejson_status st = LONEJSON_STATUS_OK;
  buffers = calloc(5U, sizeof(*buffers));
  if (buffers == NULL)
    return fail(e, CAI_ERR_NOMEM, "allocate context metadata");
  values[0] = a->title;
  values[1] = a->filename;
  values[2] = a->date;
  values[3] = a->media_type;
  lonejson_error_init(&je);
  lc_error_init(&le);
  j = lonejson_new(NULL, &je);
  if (j == NULL) {
    free(buffers);
    return fail(e, CAI_ERR_NOMEM, "allocate metadata writer");
  }
  for (i = 0U; i < 5U && st == LONEJSON_STATUS_OK; i++) {
    bounded_text *b;
    if (i < 4U && !(a->metadata & (1U << i)))
      continue;
    b = &buffers[count];
    b->capacity = 8192U;
    b->bytes = malloc(b->capacity);
    if (b->bytes == NULL) {
      rc = fail(e, CAI_ERR_NOMEM, "allocate metadata mutation");
      break;
    }
    b->count = (size_t)snprintf(b->bytes, b->capacity,
                                "/%s=", i == 4U ? "updated_at" : names[i]);
    memset(&w, 0, sizeof(w));
    st = lonejson_writer_init_sink(j, &w, bounded_write, b, &je);
    if (st == LONEJSON_STATUS_OK) {
      if (i == 4U)
        st = lonejson_writer_i64(&w, (lonejson_int64)time(NULL), &je);
      else if (values[i] != NULL)
        st = lonejson_writer_string(&w, values[i], strlen(values[i]), &je);
      else
        st = lonejson_writer_string(&w, "", 0U, &je);
    }
    if (st == LONEJSON_STATUS_OK)
      st = lonejson_writer_finish(&w, &je);
    lonejson_writer_cleanup(&w);
    mutations[count++] = b->bytes;
  }
  if (rc == CAI_OK && st != LONEJSON_STATUS_OK)
    rc = from_lj(e, &je);
  if (rc == CAI_OK) {
    lc_mutate_req_init(&req);
    req.mutations = mutations;
    req.mutation_count = count;
    req.has_if_version = 1;
    req.if_version = a->expected_version;
    if (lease->mutate(lease, &req, &le) != LC_OK)
      rc = from_lc(e, &le);
  }
  for (i = 0U; i < 5U; i++)
    free(buffers[i].bytes);
  free(buffers);
  lonejson_free(j);
  lc_error_cleanup(&le);
  return rc;
}
static size_t fd_read(void *ctx, void *b, size_t n, cai_error *e) {
  int fd = *(int *)ctx;
  ssize_t got;
  do {
    got = read(fd, b, n);
  } while (got < 0 && errno == EINTR);
  if (got < 0) {
    fail(e, CAI_ERR_INVALID, "read context file");
    return 0U;
  }
  return (size_t)got;
}
static int fd_write(void *ctx, const void *b, size_t n, cai_error *e) {
  int fd = *(int *)ctx;
  const char *bytes = b;
  ssize_t wrote;
  while (n) {
    wrote = write(fd, bytes, n);
    if (wrote < 0 && errno == EINTR)
      continue;
    if (wrote <= 0)
      return fail(e, CAI_ERR_INVALID, "write context download");
    n -= (size_t)wrote;
    bytes += wrote;
  }
  return CAI_OK;
}
typedef struct leased_file {
  int fd;
  lc_lease *lease;
} leased_file;
static size_t leased_read(void *ctx, void *b, size_t n, lc_error *e) {
  leased_file *file = ctx;
  ssize_t got;
  if (renew(file->lease, e) != LC_OK)
    return 0U;
  do {
    got = read(file->fd, b, n);
  } while (got < 0 && errno == EINTR);
  if (got < 0) {
    lc_error_set(e, LC_ERR_INVALID, 0, "read context attachment", NULL, NULL,
                 NULL);
    return 0U;
  }
  return (size_t)got;
}
static int upload(cai_context *p, context_args *a, cai_sink *out, int replace,
                  cai_error *e) {
  int fd, text, rc;
  struct stat before, after;
  context_doc *doc;
  lc_lease *lease = NULL;
  lc_error le;
  cai_source *source = NULL;
  cai_source_callbacks cb;
  lc_source *binary = NULL;
  lc_attach_req attach;
  lc_attach_res attached;
  lc_version version = 0;
  leased_file file;
  const char *name, *dot;
  fd = open_path(p, a->path, 0, 0, e);
  if (fd < 0)
    return e != NULL ? e->code : CAI_ERR_INVALID;
  rc = classify(fd, &text, &before, e);
  if (rc == CAI_OK && !p->yolo && before.st_nlink != 1)
    rc = fail(e, CAI_ERR_INVALID,
              "context upload rejects hard-linked files in restricted mode");
  doc = calloc(1U, sizeof(*doc));
  if (doc == NULL)
    rc = fail(e, CAI_ERR_NOMEM, "allocate context upload");
  lc_error_init(&le);
  memset(&attached, 0, sizeof(attached));
  if (rc == CAI_OK && replace) {
    state_reader *reader = calloc(1U, sizeof(*reader));
    if (reader == NULL)
      rc = fail(e, CAI_ERR_NOMEM, "allocate context replacement reader");
    else {
      rc = read_doc(p, a->id, NULL, doc, reader, &version, e);
      free(reader);
    }
    if (rc == CAI_OK && version != a->expected_version)
      rc = fail(e, CAI_ERR_INVALID,
                "context version conflict; read the item again");
  }
  if (rc == CAI_OK && !replace) {
    name = strrchr(a->path, '/');
    name = name != NULL ? name + 1 : a->path;
    if (!valid_filename(name))
      rc = fail(e, CAI_ERR_INVALID, "unsafe upload filename");
    else {
      strcpy(doc->filename, name);
      strcpy(doc->kind, text ? "text" : "file");
      dot = strrchr(name, '.');
      strcpy(doc->media_type, text ? dot != NULL && (!strcmp(dot, ".md") ||
                                                     !strcmp(dot, ".markdown"))
                                         ? "text/markdown"
                                         : "text/plain"
                                   : "application/octet-stream");
      doc->created_at = (lonejson_int64)time(NULL);
      if (lc_xid_new(doc->id, &le) != LC_OK)
        rc = from_lc(e, &le);
    }
  }
  if (rc == CAI_OK) {
    doc->updated_at = (lonejson_int64)time(NULL);
    doc->size = before.st_size;
    if (acquire(p, doc->id, !replace, &lease, &le) != LC_OK)
      rc = from_lc(e, &le);
  }
  if (rc == CAI_OK && replace && lease->version != a->expected_version)
    rc = fail(e, CAI_ERR_INVALID,
              "context version conflict; read the item again");
  if (rc == CAI_OK && replace) {
    int was_text = !strcmp(doc->kind, "text");
    if (text != was_text) {
      strcpy(doc->kind, text ? "text" : "file");
      strcpy(doc->media_type, text ? "text/plain" : "application/octet-stream");
    }
    if (text && !was_text) {
      int deleted;
      if (lease->delete_all_attachments(lease, &deleted, &le) != LC_OK)
        rc = from_lc(e, &le);
    }
  }
  if (rc == CAI_OK) {
    memset(&cb, 0, sizeof(cb));
    cb.read = fd_read;
    cb.context = &fd;
    rc = cai_source_from_callbacks(&cb, &source, e);
  }
  if (rc == CAI_OK && !text) {
    lc_attach_req_init(&attach);
    attach.name = "content";
    /* Editable document metadata never becomes a transport header. */
    attach.content_type = "application/octet-stream";
    file.fd = fd;
    file.lease = lease;
    if (lc_source_from_callbacks(leased_read, NULL, NULL, &file, &binary,
                                 &le) != LC_OK ||
        lease->attach(lease, &attach, binary, &attached, &le) != LC_OK)
      rc = from_lc(e, &le);
  }
  if (rc == CAI_OK &&
      save_content(lease, doc, text ? source : NULL, &le) != LC_OK)
    rc = from_lc(e, &le);
  if (rc == CAI_OK && (fstat(fd, &after) || before.st_size != after.st_size ||
                       before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
                       before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
                       before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
                       before.st_ctim.tv_nsec != after.st_ctim.tv_nsec))
    rc = fail(e, CAI_ERR_INVALID,
              "context upload file changed while being read");
  if (lease != NULL) {
    version = lease->version;
    if (finish(lease, rc == CAI_OK ? LC_OK : LC_ERR_INVALID, &le) != LC_OK &&
        rc == CAI_OK)
      rc = from_lc(e, &le);
  }
  if (rc == CAI_OK)
    rc = receipt(out, doc->id, version, doc->kind,
                 replace ? "replaced" : "created", NULL, e);
  lc_attach_res_cleanup(&attached);
  lc_source_close(binary);
  cai_source_close(source);
  close(fd);
  if (doc != NULL) {
    lonejson_cleanup(&doc_map, doc);
    free(doc);
  }
  lc_error_cleanup(&le);
  return rc;
}
static int write_doc(cai_sink *out, context_doc *doc, lc_version version,
                     state_reader *s, cai_error *e) {
  lonejson *j;
  lonejson_writer w;
  lonejson_error l;
  lonejson_status rc;
  size_t start, need;
  lonejson_error_init(&l);
  j = lonejson_new(NULL, &l);
  if (j == NULL)
    return fail(e, CAI_ERR_NOMEM, "allocate context result");
  memset(&w, 0, sizeof(w));
  rc = lonejson_writer_init_sink(j, &w, output_write, out, &l);
#define EMIT(x)                                                                \
  do {                                                                         \
    if (rc == LONEJSON_STATUS_OK)                                              \
      rc = (x);                                                                \
  } while (0)
  EMIT(lonejson_writer_begin_object(&w, &l));
  EMIT(lonejson_writer_key(&w, "version", 7U, &l));
  EMIT(lonejson_writer_i64(&w, version, &l));
  EMIT(lonejson_writer_key(&w, "metadata", 8U, &l));
  EMIT(lonejson_writer_mapped(&w, &meta_map, doc, &l));
  if (s != NULL && s->preview != NULL && !strcmp(doc->kind, "text")) {
    /* Never end a bounded preview in the middle of a UTF-8 code point. */
    if (s->used) {
      start = s->used - 1U;
      while (start && ((unsigned char)s->preview[start] & 0xc0U) == 0x80U)
        start--;
      need = (unsigned char)s->preview[start] < 128U   ? 1U
             : (unsigned char)s->preview[start] < 224U ? 2U
             : (unsigned char)s->preview[start] < 240U ? 3U
                                                       : 4U;
      if (s->used - start < need) {
        s->used = start;
        s->truncated = 1;
      }
    }
    EMIT(lonejson_writer_key(&w, "content", 7U, &l));
    EMIT(lonejson_writer_string(&w, s->preview, s->used, &l));
    EMIT(lonejson_writer_key(&w, "truncated", 9U, &l));
    EMIT(lonejson_writer_bool(&w, s->truncated, &l));
  }
  EMIT(lonejson_writer_end_object(&w, &l));
  EMIT(lonejson_writer_finish(&w, &l));
#undef EMIT
  lonejson_writer_cleanup(&w);
  lonejson_free(j);
  return rc == LONEJSON_STATUS_OK ? CAI_OK : from_lj(e, &l);
}
typedef struct key_page {
  char keys[CONTEXT_PAGE_KEYS][21];
  size_t count, length;
} key_page;
static int key_begin(void *ctx, lc_error *e) {
  key_page *p = ctx;
  p->length = 0U;
  if (p->count >= CONTEXT_PAGE_KEYS) {
    lc_error_set(e, LC_ERR_INVALID, 0, "context page exceeds requested limit",
                 NULL, NULL, NULL);
    return 0;
  }
  return 1;
}
static int key_chunk(void *ctx, const char *b, size_t n, lc_error *e) {
  key_page *p = ctx;
  if (n > 20U - p->length) {
    lc_error_set(e, LC_ERR_INVALID, 0, "invalid context key", NULL, NULL, NULL);
    return 0;
  }
  memcpy(p->keys[p->count] + p->length, b, n);
  p->length += n;
  return 1;
}
static int key_end(void *ctx, lc_error *e) {
  key_page *p = ctx;
  p->keys[p->count][p->length] = '\0';
  if (!lc_xid_is_valid(p->keys[p->count])) {
    lc_error_set(e, LC_ERR_INVALID, 0,
                 "context namespace contains a non-XID context key", NULL, NULL,
                 NULL);
    return 0;
  }
  p->count++;
  return 1;
}
static int query_page(cai_context *p, const context_args *a, key_page *page,
                      lc_query_res *res, cai_error *e) {
  lc_query_req req;
  lc_query_key_handler handler;
  lc_error l;
  int rc;
  memset(page, 0, sizeof(*page));
  lc_error_init(&l);
  lc_query_req_init(&req);
  handler.begin = key_begin;
  handler.chunk = key_chunk;
  handler.end = key_end;
  req.ns = p->ns;
  req.limit = a->limit > 0 ? (long)a->limit : 20L;
  req.cursor = a->cursor;
  if (a->query != NULL && *a->query)
    req.selector_lql = a->query;
  else
    req.selector_lql = "exists{/id}";
  req.refresh = "wait_for";
  rc = p->client->query_keys(p->client, &req, &handler, page, res, &l);
  if (rc != LC_OK)
    rc = from_lc(e, &l);
  lc_error_cleanup(&l);
  return rc;
}
static int query(cai_context *p, context_args *a, cai_sink *out, cai_error *e) {
  key_page page;
  lc_query_res res;
  context_doc *doc;
  state_reader *s;
  lonejson_writer w;
  lonejson *j;
  lonejson_error l;
  lc_version version;
  size_t i;
  int rc;
  memset(&res, 0, sizeof(res));
  rc = query_page(p, a, &page, &res, e);
  if (rc != CAI_OK) {
    lc_query_res_cleanup(&res);
    return rc;
  }
  doc = calloc(1U, sizeof(*doc));
  s = calloc(1U, sizeof(*s));
  lonejson_error_init(&l);
  j = lonejson_new(NULL, &l);
  memset(&w, 0, sizeof(w));
  if (doc == NULL || s == NULL || j == NULL)
    rc = fail(e, CAI_ERR_NOMEM, "allocate context page");
  if (rc == CAI_OK && lonejson_writer_init_sink(j, &w, output_write, out, &l) !=
                          LONEJSON_STATUS_OK)
    rc = from_lj(e, &l);
  if (rc == CAI_OK && cai_sink_write(out, "{\"items\":[", 10U, e) != CAI_OK)
    rc = e != NULL ? e->code : CAI_ERR_INVALID;
  for (i = 0U; rc == CAI_OK && i < page.count; i++) {
    rc = read_doc(p, page.keys[i], NULL, doc, s, &version, e);
    if (rc == CAI_OK && i)
      rc = cai_sink_write(out, ",", 1U, e);
    if (rc == CAI_OK)
      rc = write_doc(out, doc, version, NULL, e);
    lonejson_cleanup(&doc_map, doc);
    memset(doc, 0, sizeof(*doc));
    memset(s, 0, sizeof(*s));
  }
  if (rc == CAI_OK)
    rc = cai_sink_write(out, "],\"cursor\":", 11U, e);
  if (rc == CAI_OK) {
    lonejson_status st =
        res.cursor != NULL
            ? lonejson_writer_string(&w, res.cursor, strlen(res.cursor), &l)
            : lonejson_writer_null(&w, &l);
    if (st == LONEJSON_STATUS_OK)
      st = lonejson_writer_finish(&w, &l);
    if (st != LONEJSON_STATUS_OK)
      rc = from_lj(e, &l);
  }
  if (rc == CAI_OK)
    rc = cai_sink_write(out, "}", 1U, e);
  lonejson_writer_cleanup(&w);
  lonejson_free(j);
  free(doc);
  free(s);
  lc_query_res_cleanup(&res);
  return rc;
}
/** Download publication uses a held parent descriptor and an exclusive link. */
typedef struct file_output {
  int parent, fd;
  char temporary[64];
  char *name;
} file_output;
static int output_open(cai_context *p, const char *path, file_output *f,
                       cai_error *e) {
  char *directory, *slash;
  char xid[21];
  lc_error l;
  struct stat existing;
  memset(f, 0, sizeof(*f));
  f->parent = f->fd = -1;
  if (path == NULL || !*path)
    return fail(e, CAI_ERR_INVALID, "context download path required");
  directory = strdup(path);
  if (directory == NULL)
    return fail(e, CAI_ERR_NOMEM, "allocate download path");
  slash = strrchr(directory, '/');
  f->name = strdup(slash != NULL ? slash + 1 : path);
  if (slash != NULL) {
    if (slash == directory)
      slash[1] = '\0';
    else
      *slash = '\0';
  } else {
    free(directory);
    directory = strdup(".");
  }
  if (f->name == NULL || directory == NULL) {
    free(directory);
    return fail(e, CAI_ERR_NOMEM, "allocate download name");
  }
  if (!*f->name || !strcmp(f->name, ".") || !strcmp(f->name, "..")) {
    free(directory);
    return fail(e, CAI_ERR_INVALID, "invalid download filename");
  }
  f->parent = open_path(p, directory, 0, 1, e);
  free(directory);
  if (f->parent < 0)
    return e != NULL ? e->code : CAI_ERR_INVALID;
  if (fstatat(f->parent, f->name, &existing, AT_SYMLINK_NOFOLLOW) == 0)
    return fail(e, CAI_ERR_INVALID,
                "context download destination already exists");
  if (errno != ENOENT)
    return fail(e, CAI_ERR_INVALID, "inspect context download destination");
  lc_error_init(&l);
  if (lc_xid_new(xid, &l) != LC_OK) {
    int rc = from_lc(e, &l);
    lc_error_cleanup(&l);
    return rc;
  }
  lc_error_cleanup(&l);
  snprintf(f->temporary, sizeof(f->temporary), ".context-%s.tmp", xid);
  f->fd = openat(f->parent, f->temporary,
                 O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (f->fd < 0)
    return fail(e, CAI_ERR_INVALID, "create context download");
  return CAI_OK;
}
static int output_close(file_output *f, int publish, cai_error *e) {
  int rc = CAI_OK;
  if (f->fd >= 0) {
    if (publish && fsync(f->fd)) {
      publish = 0;
      rc = fail(e, CAI_ERR_INVALID, "flush context download");
    }
    if (close(f->fd) && publish) {
      publish = 0;
      rc = fail(e, CAI_ERR_INVALID, "close context download");
    }
  }
  if (f->parent >= 0 && f->temporary[0] && f->fd >= 0) {
    if (publish && linkat(f->parent, f->temporary, f->parent, f->name, 0))
      rc = fail(e, CAI_ERR_INVALID,
                "publish context download: destination already exists or is "
                "unwritable");
    unlinkat(f->parent, f->temporary, 0);
  }
  if (f->parent >= 0)
    close(f->parent);
  free(f->name);
  return rc;
}
static int binary_write(lc_sink *sink, const void *b, size_t n, lc_error *e) {
  state_reader *s = sink->impl;
  if (renew(s->lease, e) != LC_OK)
    return 0;
  if (cai_sink_write(s->content, b, n, s->error) != CAI_OK) {
    lc_error_set(e, LC_ERR_INVALID, 0, "write context attachment", NULL, NULL,
                 NULL);
    return 0;
  }
  return 1;
}
static int download(cai_context *p, context_args *a, cai_sink *out,
                    cai_error *e) {
  file_output file;
  cai_sink *sink = NULL;
  cai_sink_callbacks cb;
  context_doc *doc;
  state_reader *s;
  lc_lease *lease = NULL;
  lc_error l;
  lc_attachment_get_req req;
  lc_attachment_get_res res;
  lc_sink binary;
  lc_version version = 0;
  int rc, closed;
  memset(&res, 0, sizeof(res));
  lc_error_init(&l);
  doc = calloc(1U, sizeof(*doc));
  s = calloc(1U, sizeof(*s));
  if (doc == NULL || s == NULL) {
    free(doc);
    free(s);
    return fail(e, CAI_ERR_NOMEM, "allocate context download");
  }
  rc = output_open(p, a->path, &file, e);
  if (rc == CAI_OK) {
    memset(&cb, 0, sizeof(cb));
    cb.context = &file.fd;
    cb.write = fd_write;
    rc = cai_sink_from_callbacks(&cb, &sink, e);
  }
  if (rc == CAI_OK && acquire(p, a->id, 0, &lease, &l) != LC_OK)
    rc = from_lc(e, &l);
  if (rc == CAI_OK) {
    s->content = sink;
    s->lease = lease;
    rc = read_doc(p, a->id, lease, doc, s, &version, e);
  }
  if (rc == CAI_OK && a->has_version && version != a->expected_version)
    rc = fail(e, CAI_ERR_INVALID,
              "context changed during export; retry the export");
  if (rc == CAI_OK && !strcmp(doc->kind, "file")) {
    lc_attachment_get_req_init(&req);
    req.selector.name = "content";
    memset(&binary, 0, sizeof(binary));
    binary.write = binary_write;
    binary.impl = s;
    if (lease->get_attachment(lease, &req, &binary, &res, &l) != LC_OK)
      rc = from_lc(e, &l);
  }
  if (lease != NULL &&
      finish(lease, rc == CAI_OK ? LC_OK : LC_ERR_INVALID, &l) != LC_OK &&
      rc == CAI_OK)
    rc = from_lc(e, &l);
  cai_sink_close(sink);
  closed = output_close(&file, rc == CAI_OK, e);
  if (rc == CAI_OK)
    rc = closed;
  if (rc == CAI_OK)
    rc = receipt(out, a->id, version, doc->kind, "downloaded", a->path, e);
  lc_attachment_get_res_cleanup(&res);
  lc_error_cleanup(&l);
  lonejson_cleanup(&doc_map, doc);
  free(doc);
  free(s);
  return rc;
}
/* A text read is push-based in liblockdc and a state update is pull-based.
 * This bounded pipe joins those public APIs without staging a whole document.
 * It is used only when replacing array-valued metadata, which scalar LQL
 * mutations cannot express. The caller holds the object's mutation lease. */
typedef struct content_pipe {
  pthread_mutex_t lock;
  pthread_cond_t condition;
  unsigned char bytes[8192];
  size_t head, count;
  int done, cancelled, ready;
  cai_context *context;
  const char *id;
  cai_error error;
} content_pipe;
static int pipe_write(void *ctx, const void *bytes, size_t n,
                      cai_error *error) {
  content_pipe *p = ctx;
  const unsigned char *b = bytes;
  size_t take, tail;
  pthread_mutex_lock(&p->lock);
  p->ready = 1;
  pthread_cond_broadcast(&p->condition);
  while (n && !p->cancelled) {
    while (p->count == sizeof(p->bytes) && !p->cancelled)
      pthread_cond_wait(&p->condition, &p->lock);
    if (p->cancelled)
      break;
    tail = (p->head + p->count) % sizeof(p->bytes);
    take = sizeof(p->bytes) - tail;
    if (take > sizeof(p->bytes) - p->count)
      take = sizeof(p->bytes) - p->count;
    if (take > n)
      take = n;
    memcpy(p->bytes + tail, b, take);
    p->count += take;
    n -= take;
    b += take;
    pthread_cond_broadcast(&p->condition);
  }
  pthread_mutex_unlock(&p->lock);
  return n ? fail(error, CAI_ERR_CANCELLED, "context metadata update cancelled")
           : CAI_OK;
}
static size_t pipe_read(void *ctx, void *b, size_t n, cai_error *error) {
  content_pipe *p = ctx;
  size_t take;
  pthread_mutex_lock(&p->lock);
  while (!p->count && !p->done)
    pthread_cond_wait(&p->condition, &p->lock);
  take = sizeof(p->bytes) - p->head;
  if (take > p->count)
    take = p->count;
  if (take > n)
    take = n;
  memcpy(b, p->bytes + p->head, take);
  p->head = (p->head + take) % sizeof(p->bytes);
  p->count -= take;
  if (!take && p->done && p->error.code != CAI_OK)
    fail(error, p->error.code, p->error.message);
  pthread_cond_broadcast(&p->condition);
  pthread_mutex_unlock(&p->lock);
  return take;
}
static void *copy_content(void *ctx) {
  content_pipe *p = ctx;
  context_doc *doc = calloc(1U, sizeof(*doc));
  state_reader *reader = calloc(1U, sizeof(*reader));
  cai_sink_callbacks cb;
  cai_sink *sink = NULL;
  lc_version version;
  int rc = CAI_OK;
  memset(&cb, 0, sizeof(cb));
  cb.context = p;
  cb.write = pipe_write;
  if (doc == NULL || reader == NULL)
    rc = fail(&p->error, CAI_ERR_NOMEM, "allocate context content reader");
  if (rc == CAI_OK)
    rc = cai_sink_from_callbacks(&cb, &sink, &p->error);
  if (rc == CAI_OK) {
    reader->content = sink;
    rc = read_doc(p->context, p->id, NULL, doc, reader, &version, &p->error);
  }
  if (doc != NULL) {
    lonejson_cleanup(&doc_map, doc);
    free(doc);
  }
  cai_sink_close(sink);
  free(reader);
  pthread_mutex_lock(&p->lock);
  p->done = 1;
  pthread_cond_broadcast(&p->condition);
  pthread_mutex_unlock(&p->lock);
  return NULL;
}
static int set_stream_metadata(cai_context *context, lc_lease *lease,
                               context_doc *doc, context_args *a,
                               cai_error *error) {
  content_pipe *p = NULL;
  cai_source *source = NULL;
  cai_source_callbacks cb;
  pthread_t worker;
  lonejson_string_array old;
  lc_error local;
  int rc = CAI_OK, started = 0, lock_ready = 0, condition_ready = 0;
  if (a->metadata & 1U)
    snprintf(doc->title, sizeof(doc->title), "%s",
             a->title != NULL ? a->title : "");
  if (a->metadata & 2U)
    snprintf(doc->filename, sizeof(doc->filename), "%s",
             a->filename != NULL ? a->filename : "");
  if (a->metadata & 4U)
    snprintf(doc->date, sizeof(doc->date), "%s",
             a->date != NULL ? a->date : "");
  if (a->metadata & 8U)
    snprintf(doc->media_type, sizeof(doc->media_type), "%s",
             a->media_type != NULL ? a->media_type : "");
  old = doc->tags;
  doc->tags = a->tags;
  a->tags = old;
  doc->updated_at = (lonejson_int64)time(NULL);
  lc_error_init(&local);
  if (!strcmp(doc->kind, "text")) {
    p = calloc(1U, sizeof(*p));
    if (p == NULL)
      rc = fail(error, CAI_ERR_NOMEM, "allocate context content pipe");
    if (rc == CAI_OK) {
      cai_error_init(&p->error);
      p->context = context;
      p->id = doc->id;
      lock_ready = pthread_mutex_init(&p->lock, NULL) == 0;
      condition_ready =
          lock_ready && pthread_cond_init(&p->condition, NULL) == 0;
      if (!condition_ready)
        rc = fail(error, CAI_ERR_NOMEM, "initialize context pipe");
    }
    if (rc == CAI_OK) {
      started = pthread_create(&worker, NULL, copy_content, p) == 0;
      if (!started)
        rc = fail(error, CAI_ERR_NOMEM, "start context content reader");
    }
    if (rc == CAI_OK) {
      pthread_mutex_lock(&p->lock);
      while (!p->ready && !p->done)
        pthread_cond_wait(&p->condition, &p->lock);
      if (p->done && p->error.code != CAI_OK)
        rc = fail(error, p->error.code, p->error.message);
      pthread_mutex_unlock(&p->lock);
    }
    if (rc == CAI_OK) {
      memset(&cb, 0, sizeof(cb));
      cb.context = p;
      cb.read = pipe_read;
      rc = cai_source_from_callbacks(&cb, &source, error);
    }
  }
  if (rc == CAI_OK && save_content(lease, doc, source, &local) != LC_OK)
    rc = from_lc(error, &local);
  if (started) {
    pthread_mutex_lock(&p->lock);
    p->cancelled = 1;
    pthread_cond_broadcast(&p->condition);
    pthread_mutex_unlock(&p->lock);
    pthread_join(worker, NULL);
    if (rc == CAI_OK && p->error.code != CAI_OK)
      rc = fail(error, p->error.code, p->error.message);
  }
  cai_source_close(source);
  if (condition_ready)
    pthread_cond_destroy(&p->condition);
  if (lock_ready)
    pthread_mutex_destroy(&p->lock);
  if (p != NULL) {
    cai_error_cleanup(&p->error);
    free(p);
  }
  lc_error_cleanup(&local);
  return rc;
}
static int mutate(cai_context *p, const char *tool, context_args *a,
                  cai_sink *out, cai_error *e) {
  lc_lease *lease = NULL;
  lc_error l;
  lc_remove_req req;
  context_doc *doc;
  state_reader *s;
  lc_version version = 0;
  int rc;
  doc = calloc(1U, sizeof(*doc));
  s = calloc(1U, sizeof(*s));
  if (doc == NULL || s == NULL) {
    free(doc);
    free(s);
    return fail(e, CAI_ERR_NOMEM, "allocate context mutation");
  }
  lc_error_init(&l);
  rc = acquire(p, a->id, 0, &lease, &l) == LC_OK ? CAI_OK : from_lc(e, &l);
  if (rc == CAI_OK)
    rc = read_doc(p, a->id, lease, doc, s, &version, e);
  if (rc == CAI_OK && version != a->expected_version)
    rc = fail(e, CAI_ERR_INVALID,
              "context version conflict; read the item again");
  if (rc == CAI_OK && !strcmp(tool, "context_set"))
    rc = (a->metadata & 16U) ? set_stream_metadata(p, lease, doc, a, e)
                             : set_metadata(lease, a, e);
  else if (rc == CAI_OK) {
    int deleted;
    lc_remove_req_init(&req);
    req.has_if_version = 1;
    req.if_version = a->expected_version;
    if (lease->delete_all_attachments(lease, &deleted, &l) != LC_OK ||
        lease->remove(lease, &req, &l) != LC_OK)
      rc = from_lc(e, &l);
  }
  if (lease != NULL) {
    version = lease->version;
    if (finish(lease, rc == CAI_OK ? LC_OK : LC_ERR_INVALID, &l) != LC_OK &&
        rc == CAI_OK)
      rc = from_lc(e, &l);
  }
  if (rc == CAI_OK)
    rc = receipt(out, a->id, version, doc->kind,
                 !strcmp(tool, "context_set") ? "updated" : "deleted", NULL, e);
  lonejson_cleanup(&doc_map, doc);
  free(doc);
  free(s);
  lc_error_cleanup(&l);
  return rc;
}
int cai_context_call(cai_context *p, const char *tool, const char *input,
                     cai_sink *out, cai_error *error) {
  context_args a;
  cai_error local;
  cai_error *e = error;
  int rc, versioned, has_id;
  if (e == NULL) {
    cai_error_init(&local);
    e = &local;
  }
  memset(&a, 0, sizeof(a));
  if (p == NULL || tool == NULL || out == NULL)
    rc = fail(e, CAI_ERR_INVALID,
              "context call requires adapter, tool and result sink");
  else
    rc = parse_args(input, &a, e);
  has_id = tool != NULL && strcmp(tool, "context_query") &&
           strcmp(tool, "context_upload");
  versioned = tool != NULL && (!strcmp(tool, "context_set") ||
                               !strcmp(tool, "context_delete") ||
                               !strcmp(tool, "context_replace"));
  if (rc == CAI_OK && has_id && !lc_xid_is_valid(a.id))
    rc = fail(e, CAI_ERR_INVALID, "context tool requires a valid XID");
  if (rc == CAI_OK && versioned && (!a.has_version || a.expected_version < 0))
    rc =
        fail(e, CAI_ERR_INVALID,
             "context mutation requires expected_version from a previous read");
  if (rc == CAI_OK && !strcmp(tool, "context_set") && !a.metadata)
    rc = fail(e, CAI_ERR_INVALID, "supply at least one context metadata field");
  if (rc == CAI_OK &&
      (!strcmp(tool, "context_upload") || !strcmp(tool, "context_replace")))
    rc = upload(p, &a, out, !strcmp(tool, "context_replace"), e);
  else if (rc == CAI_OK && !strcmp(tool, "context_query")) {
    if (a.limit < 0 || a.limit > CONTEXT_PAGE_KEYS)
      rc = fail(e, CAI_ERR_INVALID,
                "context query limit must be 0..32 (0 selects 20)");
    else
      rc = query(p, &a, out, e);
  } else if (rc == CAI_OK && !strcmp(tool, "context_get")) {
    context_doc *doc = calloc(1U, sizeof(*doc));
    state_reader *s = calloc(1U, sizeof(*s));
    lc_version v;
    if (doc == NULL || s == NULL ||
        (s->preview = malloc(CONTEXT_PREVIEW_BYTES + 1U)) == NULL)
      rc = fail(e, CAI_ERR_NOMEM, "allocate context preview");
    else {
      rc = read_doc(p, a.id, NULL, doc, s, &v, e);
      if (rc == CAI_OK)
        rc = write_doc(out, doc, v, s, e);
    }
    if (doc != NULL) {
      lonejson_cleanup(&doc_map, doc);
      free(doc);
    }
    if (s != NULL) {
      free(s->preview);
      free(s);
    }
  } else if (rc == CAI_OK && !strcmp(tool, "context_download"))
    rc = download(p, &a, out, e);
  else if (rc == CAI_OK && versioned)
    rc = mutate(p, tool, &a, out, e);
  else if (rc == CAI_OK)
    rc = fail(e, CAI_ERR_INVALID, "unknown context tool");
  lonejson_cleanup(&args_map, &a);
  if (error == NULL)
    cai_error_cleanup(&local);
  return rc;
}
static int ensure_directory(cai_context *p, const char *path, cai_error *e) {
  char *copy, *part, *next;
  const char *relative = path;
  int fd, child, rc = CAI_OK;
  size_t n;
  if (p->root_fd < 0 || path == NULL || !*path)
    return fail(e, CAI_ERR_INVALID,
                "context export requires a directory and filesystem root");
  fd = dup(p->root_fd);
  if (*path == '/') {
    if (p->yolo) {
      close(fd);
      fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      relative++;
    } else {
      n = strlen(p->root);
      if (strncmp(path, p->root, n) ||
          (n != 1U && path[n] != '/' && path[n] != '\0')) {
        if (fd >= 0)
          close(fd);
        return fail(e, CAI_ERR_INVALID, "context export is outside workspace");
      }
      relative += n;
      if (*relative == '/')
        relative++;
    }
  }
  copy = strdup(relative);
  if (copy == NULL || fd < 0) {
    free(copy);
    if (fd >= 0)
      close(fd);
    return fail(e, CAI_ERR_NOMEM, "open context export directory");
  }
  part = copy;
  while (*part) {
    next = strchr(part, '/');
    if (next != NULL)
      *next++ = '\0';
    if (!*part || (!p->yolo && !strcmp(part, ".."))) {
      rc = fail(e, CAI_ERR_INVALID, "unsafe context export directory");
      break;
    }
    if (strcmp(part, ".")) {
      if (mkdirat(fd, part, 0700) && errno != EEXIST) {
        rc = fail(e, CAI_ERR_INVALID, "create context export directory");
        break;
      }
      child = openat(fd, part,
                     O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                         (p->yolo ? 0 : O_NOFOLLOW));
      if (child < 0) {
        rc = fail(e, CAI_ERR_INVALID, "open context export directory");
        break;
      }
      close(fd);
      fd = child;
    }
    if (next == NULL)
      break;
    part = next;
  }
  close(fd);
  free(copy);
  return rc;
}
static int discard_write(void *ctx, const void *b, size_t n, cai_error *e) {
  (void)ctx;
  (void)b;
  (void)n;
  (void)e;
  return CAI_OK;
}
int cai_context_export(cai_context *p, const char *directory, cai_sink *out,
                       cai_error *e) {
  context_args args, item;
  key_page page;
  lc_query_res res;
  context_doc *doc;
  state_reader *s;
  cai_sink_callbacks cb;
  cai_sink *discard = NULL, *file_sink = NULL;
  lc_version version;
  file_output file;
  char *cursor = NULL, *path = NULL, *dir = NULL;
  char fallback[32], summary[128];
  size_t i, n, total = 0U;
  int rc, closed;
  if (p == NULL || out == NULL || directory == NULL)
    return fail(e, CAI_ERR_INVALID,
                "context export requires adapter, directory and receipt");
  rc = ensure_directory(p, directory, e);
  doc = calloc(1U, sizeof(*doc));
  s = calloc(1U, sizeof(*s));
  if (doc == NULL || s == NULL)
    rc = fail(e, CAI_ERR_NOMEM, "allocate context export");
  memset(&args, 0, sizeof(args));
  args.limit = CONTEXT_PAGE_KEYS;
  memset(&cb, 0, sizeof(cb));
  cb.write = discard_write;
  if (rc == CAI_OK)
    rc = cai_sink_from_callbacks(&cb, &discard, e);
  while (rc == CAI_OK) {
    memset(&res, 0, sizeof(res));
    args.cursor = cursor;
    rc = query_page(p, &args, &page, &res, e);
    for (i = 0U; rc == CAI_OK && i < page.count; i++) {
      rc = read_doc(p, page.keys[i], NULL, doc, s, &version, e);
      n = strlen(directory) + 21U + 3U;
      dir = malloc(n);
      if (dir == NULL)
        rc = fail(e, CAI_ERR_NOMEM, "allocate context export path");
      if (rc == CAI_OK) {
        snprintf(dir, n, "%s/%s", directory, doc->id);
        rc = ensure_directory(p, dir, e);
      }
      if (rc == CAI_OK && doc->filename[0] && !valid_filename(doc->filename))
        rc = fail(e, CAI_ERR_INVALID, "stored context filename is unsafe");
      if (rc == CAI_OK) {
        const char *name = doc->filename;
        if (!*name) {
          snprintf(fallback, sizeof(fallback), "%s.%s", doc->id,
                   !strcmp(doc->kind, "text") ? "md" : "bin");
          name = fallback;
        }
        n = strlen(dir) + strlen(name) + 2U;
        path = malloc(n);
        if (path == NULL)
          rc = fail(e, CAI_ERR_NOMEM, "allocate context download path");
        else {
          snprintf(path, n, "%s/%s", dir, name);
          memset(&item, 0, sizeof(item));
          item.id = doc->id;
          item.path = path;
          item.has_version = 1;
          item.expected_version = version;
          rc = download(p, &item, discard, e);
        }
      }
      free(path);
      path = NULL;
      if (rc == CAI_OK) {
        n = strlen(dir) + sizeof(".context.json");
        path = malloc(n);
        if (path == NULL)
          rc = fail(e, CAI_ERR_NOMEM, "allocate context manifest path");
        else {
          snprintf(path, n, "%s.context.json", dir);
          rc = output_open(p, path, &file, e);
          memset(&cb, 0, sizeof(cb));
          cb.write = fd_write;
          cb.context = &file.fd;
          if (rc == CAI_OK)
            rc = cai_sink_from_callbacks(&cb, &file_sink, e);
          if (rc == CAI_OK)
            rc = write_doc(file_sink, doc, version, NULL, e);
          cai_sink_close(file_sink);
          file_sink = NULL;
          closed = output_close(&file, rc == CAI_OK, e);
          if (rc == CAI_OK)
            rc = closed;
        }
      }
      if (rc == CAI_OK)
        total++;
      free(path);
      path = NULL;
      free(dir);
      dir = NULL;
      lonejson_cleanup(&doc_map, doc);
      memset(doc, 0, sizeof(*doc));
      memset(s, 0, sizeof(*s));
    }
    free(cursor);
    cursor = res.cursor != NULL ? strdup(res.cursor) : NULL;
    if (res.cursor != NULL && cursor == NULL && rc == CAI_OK)
      rc = fail(e, CAI_ERR_NOMEM, "copy context cursor");
    lc_query_res_cleanup(&res);
    if (cursor == NULL)
      break;
  }
  if (rc == CAI_OK) {
    n = (size_t)snprintf(summary, sizeof(summary),
                         "{\"status\":\"exported\",\"items\":%lu}",
                         (unsigned long)total);
    rc = cai_sink_write(out, summary, n, e);
  }
  free(cursor);
  free(path);
  free(dir);
  cai_sink_close(discard);
  if (doc != NULL) {
    lonejson_cleanup(&doc_map, doc);
    free(doc);
  }
  free(s);
  return rc;
}
