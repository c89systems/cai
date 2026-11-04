#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "cai_internal.h"
#include <cai/workspace.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

struct cai_workspace_endpoint {
  char *workspace;
  char id[33];
  char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
  char lock_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
  dev_t device;
  ino_t inode;
  int listener, lock;
};
static int transport(cai_error *error, const char *message) {
  return cai_set_error_detail(error, CAI_ERR_TRANSPORT, message,
                              strerror(errno));
}
static int private_socket(const char *path, struct stat *status,
                          cai_error *error) {
  if (lstat(path, status) != 0)
    return transport(error, "Inspect workspace socket");
  if (!S_ISSOCK(status->st_mode) || status->st_uid != geteuid() ||
      (status->st_mode & 077U) != 0U)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Workspace endpoint must be a private owned socket");
  return CAI_OK;
}
static void address_set(struct sockaddr_un *address, const char *path) {
  memset(address, 0, sizeof(*address));
  address->sun_family = AF_UNIX;
  memcpy(address->sun_path, path, strlen(path) + 1U);
}
static int descriptor_flags(int descriptor, cai_error *error) {
  int flags = fcntl(descriptor, F_GETFL), fdflags = fcntl(descriptor, F_GETFD);
  if (flags < 0 || fdflags < 0 ||
      fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0 ||
      fcntl(descriptor, F_SETFD, fdflags | FD_CLOEXEC) != 0)
    return transport(error, "Configure workspace socket descriptor");
  return CAI_OK;
}
static int stream_socket(void) {
#if defined(SOCK_CLOEXEC) && defined(SOCK_NONBLOCK)
  return socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
#else
  return socket(AF_UNIX, SOCK_STREAM, 0);
#endif
}
static int peer_user(int descriptor, cai_error *error) {
#if defined(__linux__)
  struct ucred credentials;
  socklen_t length = sizeof(credentials);
  if (getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials, &length) !=
          0 ||
      credentials.uid != geteuid())
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Workspace socket peer belongs to another user");
#elif defined(__APPLE__)
  uid_t user;
  gid_t group;
  if (getpeereid(descriptor, &user, &group) != 0 || user != geteuid())
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Workspace socket peer belongs to another user");
#else
#error Workspace peer credentials require a supported Unix implementation.
#endif
  return CAI_OK;
}
int cai_workspace_endpoint_open(const char *runtime_directory,
                                const char *workspace,
                                cai_workspace_endpoint **out,
                                cai_error *error) {
  cai_workspace_endpoint *endpoint;
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int length;
  struct stat status;
  char *runtime, *directory;
  size_t i;
  int n, rc = CAI_OK;
  if (out != NULL)
    *out = NULL;
  if (out == NULL || runtime_directory == NULL || runtime_directory[0] != '/' ||
      workspace == NULL || workspace[0] == '\0')
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Workspace and absolute runtime directory required");
  if (lstat(runtime_directory, &status) != 0 || !S_ISDIR(status.st_mode) ||
      status.st_uid != geteuid() || (status.st_mode & 077U) != 0U)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Runtime directory must be private and owned by you");
  runtime = realpath(runtime_directory, NULL);
  if (runtime == NULL)
    return transport(error, "Resolve runtime directory");
  directory = realpath(workspace, NULL);
  if (directory == NULL) {
    free(runtime);
    return transport(error, "Resolve workspace directory");
  }
  if (stat(directory, &status) != 0 || !S_ISDIR(status.st_mode)) {
    free(runtime);
    free(directory);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Workspace must be a directory");
  }
  endpoint = cai_alloc(NULL, sizeof(*endpoint));
  if (endpoint == NULL) {
    free(runtime);
    free(directory);
    return cai_set_error(error, CAI_ERR_NOMEM, "Allocate workspace endpoint");
  }
  memset(endpoint, 0, sizeof(*endpoint));
  endpoint->listener = endpoint->lock = -1;
  endpoint->workspace = directory;
  if (EVP_Digest(directory, strlen(directory), digest, &length, EVP_sha256(),
                 NULL) != 1 ||
      length < 16U)
    rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                       "Hash canonical workspace identity");
  if (rc == CAI_OK) {
    static const char hex[] = "0123456789abcdef";
    for (i = 0U; i < 16U; ++i) {
      endpoint->id[i * 2U] = hex[digest[i] >> 4U];
      endpoint->id[i * 2U + 1U] = hex[digest[i] & 15U];
    }
    n = snprintf(endpoint->socket_path, sizeof(endpoint->socket_path),
                 "%s/ws-%s.sock", runtime, endpoint->id);
    if (n < 0 || (size_t)n >= sizeof(endpoint->socket_path))
      rc = cai_set_error(error, CAI_ERR_INVALID,
                         "Workspace socket path exceeds Unix limit; select "
                         "a shorter runtime directory");
    n = snprintf(endpoint->lock_path, sizeof(endpoint->lock_path),
                 "%s/ws-%s.lock", runtime, endpoint->id);
    if (n < 0 || (size_t)n >= sizeof(endpoint->lock_path))
      rc = cai_set_error(error, CAI_ERR_INVALID,
                         "Workspace lock path exceeds Unix socket limit");
  }
  free(runtime);
  if (rc == CAI_OK)
    *out = endpoint;
  else
    cai_workspace_endpoint_close(endpoint);
  return rc;
}
const char *
cai_workspace_endpoint_directory(const cai_workspace_endpoint *self) {
  return self != NULL ? self->workspace : NULL;
}
const char *cai_workspace_endpoint_id(const cai_workspace_endpoint *self) {
  return self != NULL ? self->id : NULL;
}
const char *cai_workspace_endpoint_socket(const cai_workspace_endpoint *self) {
  return self != NULL ? self->socket_path : NULL;
}
int cai_workspace_endpoint_listen(cai_workspace_endpoint *self,
                                  cai_error *error) {
  struct sockaddr_un address;
  struct stat status;
  int descriptor, rc = CAI_OK;
  if (self == NULL || self->listener >= 0 || self->lock >= 0)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Workspace endpoint is missing or already listening");
  descriptor =
      open(self->lock_path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (descriptor < 0)
    return transport(error, "Open workspace ownership lock");
  if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_nlink != 1 || status.st_uid != geteuid() ||
      (status.st_mode & 077U) != 0U)
    rc = cai_set_error(error, CAI_ERR_INVALID,
                       "Workspace ownership lock must be private and regular");
  if (rc == CAI_OK && flock(descriptor, LOCK_EX | LOCK_NB) != 0)
    rc = errno == EWOULDBLOCK || errno == EAGAIN
             ? cai_set_error(error, CAI_ERR_LIMIT,
                             "Workspace already has a server owner")
             : transport(error, "Acquire workspace ownership lock");
  if (rc != CAI_OK) {
    close(descriptor);
    return rc;
  }
  self->lock = descriptor;
  address_set(&address, self->socket_path);
  if (lstat(self->socket_path, &status) == 0) {
    rc = private_socket(self->socket_path, &status, error);
    if (rc == CAI_OK) {
      /* Never remove an endpoint which still accepts connections, even if
       * another implementation did not participate in our ownership lock. */
      descriptor = stream_socket();
      if (descriptor < 0)
        rc = transport(error, "Probe existing workspace socket");
      else {
        rc = descriptor_flags(descriptor, error);
        if (rc == CAI_OK) {
          if (connect(descriptor, (struct sockaddr *)&address,
                      sizeof(address)) == 0 ||
              errno == EINPROGRESS || errno == EAGAIN || errno == EWOULDBLOCK)
            rc = cai_set_error(error, CAI_ERR_LIMIT,
                               "Workspace already has a live endpoint");
          else if (errno != ECONNREFUSED && errno != ENOENT)
            rc = transport(error, "Probe existing workspace socket");
        }
        close(descriptor);
      }
    }
    if (rc == CAI_OK && unlink(self->socket_path) != 0 && errno != ENOENT)
      rc = transport(error, "Remove stale workspace socket");
  } else if (errno != ENOENT)
    rc = transport(error, "Inspect workspace socket path");
  if (rc == CAI_OK) {
    self->listener = stream_socket();
    if (self->listener < 0)
      rc = transport(error, "Create workspace listener");
    else
      rc = descriptor_flags(self->listener, error);
  }
  if (rc == CAI_OK &&
      bind(self->listener, (struct sockaddr *)&address, sizeof(address)) != 0)
    rc = transport(error, "Bind workspace socket");
  if (rc == CAI_OK) {
    if (lstat(self->socket_path, &status) != 0)
      rc = transport(error, "Inspect bound workspace socket");
    else {
      self->device = status.st_dev;
      self->inode = status.st_ino;
    }
  }
  if (rc == CAI_OK &&
      (chmod(self->socket_path, 0600) != 0 || listen(self->listener, 16) != 0))
    rc = transport(error, "Prepare private workspace listener");
  if (rc != CAI_OK) {
    if (self->inode != 0 && lstat(self->socket_path, &status) == 0 &&
        status.st_ino == self->inode && status.st_dev == self->device)
      (void)unlink(self->socket_path);
    if (self->listener >= 0)
      close(self->listener);
    close(self->lock);
    self->listener = self->lock = -1;
    self->inode = 0;
  }
  return rc;
}
int cai_workspace_endpoint_fd(const cai_workspace_endpoint *self) {
  return self != NULL ? self->listener : -1;
}
int cai_workspace_endpoint_accept(cai_workspace_endpoint *self, int *out,
                                  cai_error *error) {
  int descriptor, rc;
  if (out != NULL)
    *out = -1;
  if (self == NULL || self->listener < 0 || out == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Workspace accept requires listener and output");
  do {
#if defined(__linux__)
    descriptor =
        accept4(self->listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
#else
    descriptor = accept(self->listener, NULL, NULL);
#endif
  } while (descriptor < 0 && errno == EINTR);
  if (descriptor < 0)
    return errno == EAGAIN || errno == EWOULDBLOCK
               ? CAI_OK
               : transport(error, "Accept workspace connection");
  rc = descriptor_flags(descriptor, error);
  if (rc == CAI_OK)
    rc = peer_user(descriptor, error);
  if (rc == CAI_OK)
    *out = descriptor;
  else
    close(descriptor);
  return rc;
}
int cai_workspace_endpoint_connect_finish(int descriptor, cai_error *error) {
  int failure;
  socklen_t length = sizeof(failure);
  if (getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &failure, &length) != 0)
    return transport(error, "Inspect workspace connection");
  if (failure != 0) {
    errno = failure;
    return transport(error, "Connect workspace socket");
  }
  return peer_user(descriptor, error);
}
int cai_workspace_endpoint_connect(const cai_workspace_endpoint *self, int *out,
                                   int *connecting, cai_error *error) {
  struct sockaddr_un address;
  struct stat status;
  int descriptor, rc;
  if (out != NULL)
    *out = -1;
  if (connecting != NULL)
    *connecting = 0;
  if (self == NULL || out == NULL || connecting == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Workspace connect requires endpoint and outputs");
  rc = private_socket(self->socket_path, &status, error);
  if (rc != CAI_OK)
    return rc;
  descriptor = stream_socket();
  if (descriptor < 0)
    return transport(error, "Create workspace connection");
  rc = descriptor_flags(descriptor, error);
  address_set(&address, self->socket_path);
  if (rc == CAI_OK &&
      connect(descriptor, (struct sockaddr *)&address, sizeof(address)) != 0) {
    if (errno == EINPROGRESS)
      *connecting = 1;
    else
      rc = transport(error, "Connect workspace socket");
  }
  if (rc == CAI_OK && !*connecting)
    rc = peer_user(descriptor, error);
  if (rc == CAI_OK)
    *out = descriptor;
  else
    close(descriptor);
  return rc;
}
void cai_workspace_endpoint_close(cai_workspace_endpoint *self) {
  if (self != NULL) {
    struct stat status;
    if (self->listener >= 0) {
      if (self->lock >= 0 && self->inode != 0 &&
          lstat(self->socket_path, &status) == 0 &&
          status.st_dev == self->device && status.st_ino == self->inode &&
          S_ISSOCK(status.st_mode))
        (void)unlink(self->socket_path);
      close(self->listener);
    }
    if (self->lock >= 0)
      close(self->lock);
    free(self->workspace);
    cai_free_mem(NULL, self);
  }
}
