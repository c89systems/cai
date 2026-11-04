#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "cai_internal.h"
#include "cai_workspace_private.h"
#include <cai/cbor.h>
#include <cai/workspace_registry.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define REGISTRY_SCHEMA "cai.workspace.registry"
struct cai_workspace_publication {
  int directory, name_lock;
  char file[44];
  struct stat published;
};
struct cai_workspace_registry {
  DIR *directory;
  cai_workspace_record record;
  char workspace[4096], name[256], id[33], file[44];
  struct stat current;
  size_t part, offset;
  int valid;
};
static int io(cai_error *error, const char *message) {
  return cai_set_error_detail(error, CAI_ERR_TRANSPORT, message,
                              strerror(errno));
}
static int bad(cai_error *error) {
  return cai_set_error(error, CAI_ERR_PROTOCOL,
                       "invalid workspace discovery record");
}
int cai_workspace_hash(const char *text, char out[33], cai_error *error) {
  static const char hex[] = "0123456789abcdef";
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int length;
  size_t i;
  if (EVP_Digest(text, strlen(text), digest, &length, EVP_sha256(), NULL) !=
          1 ||
      length < 16U)
    return cai_set_error(error, CAI_ERR_TRANSPORT, "hash workspace identity");
  for (i = 0U; i < 16U; ++i) {
    out[2U * i] = hex[digest[i] >> 4U];
    out[2U * i + 1U] = hex[digest[i] & 15U];
  }
  out[32] = '\0';
  return CAI_OK;
}
static int private_file(const struct stat *s) {
  return S_ISREG(s->st_mode) && s->st_uid == geteuid() &&
         (s->st_mode & 077U) == 0U && s->st_nlink == 1U;
}
static int open_directory(const char *path, int *out, cai_error *error) {
  struct stat s;
  int fd;
  *out = -1;
  if (path == NULL || path[0] != '/')
    return cai_set_error(error, CAI_ERR_INVALID,
                         "absolute runtime directory required");
  fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0)
    return io(error, "open discovery directory");
  if (fstat(fd, &s) != 0 || !S_ISDIR(s.st_mode) || s.st_uid != geteuid() ||
      (s.st_mode & 077U) != 0U) {
    close(fd);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "discovery directory must be private and owned");
  }
  *out = fd;
  return CAI_OK;
}
static int lock_at(int directory, const char *name, int *out,
                   cai_error *error) {
  struct stat s;
  int fd =
      openat(directory, name, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  *out = -1;
  if (fd < 0)
    return io(error, "open discovery ownership lock");
  if (fstat(fd, &s) != 0 || !private_file(&s)) {
    close(fd);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "discovery lock must be private and regular");
  }
  if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
    int rc =
        errno == EWOULDBLOCK || errno == EAGAIN
            ? cai_set_error(error, CAI_ERR_LIMIT,
                            "workspace discovery name or owner is already held")
            : io(error, "acquire discovery ownership lock");
    close(fd);
    return rc;
  }
  *out = fd;
  return CAI_OK;
}
static int name_valid(const char *name) {
  const unsigned char *p = (const unsigned char *)name;
  if (strlen(name) > 255U || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
    return 0;
  for (; *p; ++p)
    if (*p < 32U || *p == 127U || *p == '/' ||
        (*p == 0xc2U && p[1] >= 0x80U && p[1] <= 0x9fU))
      return 0;
  return 1;
}
static int fd_write(void *context, const void *data, size_t count,
                    cai_error *error) {
  int fd = *(int *)context;
  const unsigned char *p = data;
  while (count) {
    ssize_t n = write(fd, p, count);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return io(error, "write discovery metadata");
    p += n;
    count -= (size_t)n;
  }
  return CAI_OK;
}
static int encode(cai_cbor_encoder *encoder, cai_cbor_type type,
                  uint64_t argument, const void *data, size_t length,
                  cai_error *error) {
  cai_cbor_token token;
  memset(&token, 0, sizeof(token));
  token.type = type;
  token.argument = argument;
  token.data = data;
  token.length = length;
  return cai_cbor_encoder_write(encoder, &token, error);
}
static int string(cai_cbor_encoder *encoder, cai_cbor_type type,
                  const void *data, size_t length, cai_error *error) {
  int rc = encode(encoder, type, length, NULL, 0U, error);
  if (rc == CAI_OK)
    rc = encode(encoder, CAI_CBOR_DATA, 0U, data, length, error);
  if (rc == CAI_OK)
    rc = encode(encoder, CAI_CBOR_END, type, NULL, 0U, error);
  return rc;
}
int cai_workspace_publication_open(const char *socket_path,
                                   const char *workspace, const char *id,
                                   const char *name,
                                   const unsigned char instance[16],
                                   cai_workspace_publication **out,
                                   cai_error *error) {
  cai_workspace_publication *p;
  cai_cbor_encoder *encoder = NULL;
  cai_sink *sink = NULL;
  cai_sink_callbacks callbacks;
  char *root, hash[33], temporary[64], lock[44];
  struct stat stale;
  int fd = -1, rc;
  *out = NULL;
  if (name == NULL)
    name = "";
  if (!name_valid(name) || strlen(workspace) >= 4096U)
    return cai_set_error(
        error, CAI_ERR_INVALID,
        "invalid workspace discovery name or directory length");
  p = cai_alloc(NULL, sizeof(*p));
  root = strdup(socket_path);
  if (p == NULL || root == NULL) {
    cai_free_mem(NULL, p);
    free(root);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "allocate workspace publication");
  }
  memset(p, 0, sizeof(*p));
  p->directory = p->name_lock = -1;
  temporary[0] = '\0';
  *strrchr(root, '/') = '\0';
  rc = open_directory(root, &p->directory, error);
  free(root);
  if (rc == CAI_OK && name[0]) {
    rc = cai_workspace_hash(name, hash, error);
    if (rc == CAI_OK) {
      snprintf(lock, sizeof(lock), "nm-%s.lock", hash);
      rc = lock_at(p->directory, lock, &p->name_lock, error);
    }
  }
  if (rc == CAI_OK) {
    snprintf(temporary, sizeof(temporary), ".ws-%s.tmp", id);
    /* The caller owns this workspace's OS lock. Recover only its private
     * regular staging file after interrupted publication, without a scan. */
    if (fstatat(p->directory, temporary, &stale, AT_SYMLINK_NOFOLLOW) == 0) {
      if (!private_file(&stale))
        rc =
            cai_set_error(error, CAI_ERR_INVALID,
                          "discovery staging file must be private and regular");
      else if (unlinkat(p->directory, temporary, 0) != 0)
        rc = io(error, "remove interrupted discovery publication");
    } else if (errno != ENOENT)
      rc = io(error, "inspect discovery staging file");
  }
  if (rc == CAI_OK) {
    fd = openat(p->directory, temporary,
                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
      rc = io(error, "create discovery publication");
  }
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.write = fd_write;
  callbacks.context = &fd;
  if (rc == CAI_OK)
    rc = cai_sink_from_callbacks(&callbacks, &sink, error);
  if (rc == CAI_OK)
    rc = cai_cbor_encoder_open(sink, &encoder, error);
  if (rc == CAI_OK)
    rc = encode(encoder, CAI_CBOR_ARRAY, 4U, NULL, 0U, error);
  if (rc == CAI_OK)
    rc = string(encoder, CAI_CBOR_TEXT, REGISTRY_SCHEMA,
                strlen(REGISTRY_SCHEMA), error);
  if (rc == CAI_OK)
    rc = string(encoder, CAI_CBOR_BYTES, workspace, strlen(workspace), error);
  if (rc == CAI_OK)
    rc = string(encoder, CAI_CBOR_TEXT, name, strlen(name), error);
  if (rc == CAI_OK)
    rc = string(encoder, CAI_CBOR_BYTES, instance, 16U, error);
  if (rc == CAI_OK)
    rc = encode(encoder, CAI_CBOR_END, CAI_CBOR_ARRAY, NULL, 0U, error);
  if (rc == CAI_OK)
    rc = cai_cbor_encoder_finish(encoder, error);
  if (rc == CAI_OK && fstat(fd, &p->published) != 0)
    rc = io(error, "inspect discovery publication");
  snprintf(p->file, sizeof(p->file), "ws-%s.info", id);
  if (rc == CAI_OK &&
      renameat(p->directory, temporary, p->directory, p->file) != 0)
    rc = io(error, "publish workspace discovery metadata");
  if (rc != CAI_OK)
    p->published.st_ino = 0;
  cai_cbor_encoder_close(encoder);
  cai_sink_close(sink);
  if (fd >= 0) {
    close(fd);
    (void)unlinkat(p->directory, temporary, 0);
  }
  if (rc == CAI_OK)
    *out = p;
  else
    cai_workspace_publication_close(p);
  return rc;
}
void cai_workspace_publication_close(cai_workspace_publication *p) {
  if (p != NULL) {
    struct stat s;
    if (p->published.st_ino != 0 &&
        fstatat(p->directory, p->file, &s, AT_SYMLINK_NOFOLLOW) == 0 &&
        s.st_ino == p->published.st_ino && s.st_dev == p->published.st_dev)
      (void)unlinkat(p->directory, p->file, 0);
    if (p->name_lock >= 0)
      close(p->name_lock);
    if (p->directory >= 0)
      close(p->directory);
    cai_free_mem(NULL, p);
  }
}
static int decoded(void *context, const cai_cbor_token *t, cai_error *error) {
  cai_workspace_registry *r = context;
  const char *schema = REGISTRY_SCHEMA;
  size_t size = r->part == 2U   ? strlen(schema)
                : r->part == 4U ? sizeof(r->workspace) - 1U
                : r->part == 6U ? sizeof(r->name) - 1U
                                : 16U;
  void *out = r->part == 4U   ? (void *)r->workspace
              : r->part == 6U ? (void *)r->name
                              : (void *)r->record.instance;
  cai_cbor_type type =
      r->part == 1U || r->part == 2U || r->part == 5U || r->part == 6U
          ? CAI_CBOR_TEXT
          : CAI_CBOR_BYTES;
  if (t->indefinite)
    return bad(error);
  if (r->part == 0U) {
    if (t->type != CAI_CBOR_ARRAY || t->argument != 4U)
      return bad(error);
  } else if (r->part == 1U || r->part == 3U || r->part == 5U || r->part == 7U) {
    size_t limit = r->part == 1U   ? strlen(schema)
                   : r->part == 3U ? sizeof(r->workspace) - 1U
                   : r->part == 5U ? sizeof(r->name) - 1U
                                   : 16U;
    if (t->type != type || t->argument > limit ||
        ((r->part == 1U || r->part == 7U) && t->argument != limit))
      return bad(error);
    r->offset = 0U;
  } else if (r->part == 2U || r->part == 4U || r->part == 6U || r->part == 8U) {
    if (t->type == CAI_CBOR_DATA) {
      if (t->length > size - r->offset ||
          (r->part != 8U && memchr(t->data, 0, t->length) != NULL))
        return bad(error);
      if (r->part == 2U) {
        if (memcmp(t->data, schema + r->offset, t->length) != 0)
          return bad(error);
      } else
        memcpy((char *)out + r->offset, t->data, t->length);
      r->offset += t->length;
      return CAI_OK;
    }
    if (t->type != CAI_CBOR_END || t->argument != type)
      return bad(error);
  } else if (r->part != 9U || t->type != CAI_CBOR_END ||
             t->argument != CAI_CBOR_ARRAY)
    return bad(error);
  ++r->part;
  return CAI_OK;
}
int cai_workspace_registry_open(const char *runtime,
                                cai_workspace_registry **out,
                                cai_error *error) {
  cai_workspace_registry *r;
  int fd, rc;
  if (out != NULL)
    *out = NULL;
  if (out == NULL)
    return cai_set_error(error, CAI_ERR_INVALID, "registry output required");
  r = cai_alloc(NULL, sizeof(*r));
  if (r == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "allocate workspace discovery reader");
  memset(r, 0, sizeof(*r));
  rc = open_directory(runtime, &fd, error);
  if (rc == CAI_OK) {
    r->directory = fdopendir(fd);
    if (r->directory == NULL) {
      rc = io(error, "open workspace directory iterator");
      close(fd);
    }
  }
  if (rc == CAI_OK)
    *out = r;
  else
    cai_workspace_registry_close(r);
  return rc;
}
int cai_workspace_registry_next(cai_workspace_registry *r,
                                const cai_workspace_record **out,
                                cai_error *error) {
  struct dirent *entry;
  cai_cbor_decoder *decoder = NULL;
  char chunk[512], hash[33];
  int fd, rc = CAI_OK;
  ssize_t n = 0;
  if (out != NULL)
    *out = NULL;
  if (r == NULL || out == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "registry reader and record output required");
  r->valid = 0;
  for (;;) {
    errno = 0;
    entry = readdir(r->directory);
    if (entry == NULL)
      return errno ? io(error, "read discovery directory") : CAI_OK;
    if (strlen(entry->d_name) != 40U || strncmp(entry->d_name, "ws-", 3U) ||
        strcmp(entry->d_name + 35U, ".info") ||
        strspn(entry->d_name + 3U, "0123456789abcdef") != 32U)
      continue;
    memcpy(r->file, entry->d_name, 41U);
    memcpy(r->id, entry->d_name + 3U, 32U);
    r->id[32] = 0;
    fd = openat(dirfd(r->directory), r->file,
                O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd >= 0 || errno != ENOENT)
      break;
  }
  if (fd < 0) {
    if (errno == ELOOP || errno == EACCES)
      return cai_set_error_detail(error, CAI_ERR_INVALID,
                                  "discovery record is unsafe or unreadable",
                                  r->file);
    return io(error, "open discovery record");
  }
  if (fstat(fd, &r->current) != 0 || !private_file(&r->current) ||
      r->current.st_size > 8192) {
    close(fd);
    return bad(error);
  }
  memset(r->workspace, 0, sizeof(r->workspace));
  memset(r->name, 0, sizeof(r->name));
  r->part = r->offset = 0U;
  rc = cai_cbor_decoder_open(decoded, r, &decoder, error);
  while (rc == CAI_OK) {
    n = read(fd, chunk, sizeof(chunk));
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      break;
    rc = cai_cbor_decoder_feed(decoder, chunk, (size_t)n, error);
  }
  if (rc == CAI_OK)
    rc = n < 0 ? io(error, "read discovery record")
               : cai_cbor_decoder_finish(decoder, error);
  close(fd);
  cai_cbor_decoder_close(decoder);
  if (rc == CAI_OK &&
      (r->part != 10U || r->workspace[0] != '/' || !name_valid(r->name)))
    rc = bad(error);
  if (rc == CAI_OK)
    rc = cai_workspace_hash(r->workspace, hash, error);
  if (rc == CAI_OK && strcmp(hash, r->id))
    rc = bad(error);
  if (rc == CAI_OK) {
    r->record.workspace = r->workspace;
    r->record.name = r->name;
    r->record.id = r->id;
    r->valid = 1;
    *out = &r->record;
  }
  return rc;
}
int cai_workspace_registry_prune(cai_workspace_registry *r, cai_error *error) {
  char lock[44];
  struct stat s;
  int fd, rc;
  if (r == NULL || !r->valid)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "current validated discovery record required");
  snprintf(lock, sizeof(lock), "ws-%s.lock", r->id);
  rc = lock_at(dirfd(r->directory), lock, &fd, error);
  if (rc != CAI_OK)
    return rc;
  if (fstatat(dirfd(r->directory), r->file, &s, AT_SYMLINK_NOFOLLOW) != 0)
    rc = errno == ENOENT ? CAI_OK : io(error, "inspect stale discovery record");
  else if (s.st_dev != r->current.st_dev || s.st_ino != r->current.st_ino)
    rc =
        cai_set_error(error, CAI_ERR_INVALID,
                      "discovery record changed; read it again before pruning");
  else if (unlinkat(dirfd(r->directory), r->file, 0) != 0 && errno != ENOENT)
    rc = io(error, "remove stale discovery record");
  close(fd);
  if (rc == CAI_OK)
    r->valid = 0;
  return rc;
}
void cai_workspace_registry_close(cai_workspace_registry *r) {
  if (r != NULL) {
    if (r->directory != NULL)
      closedir(r->directory);
    cai_free_mem(NULL, r);
  }
}
