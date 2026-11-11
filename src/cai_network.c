/** @file Native socket setup shared by tools, feeders and UI adapters. */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "cai_internal.h"
#include <arpa/inet.h>
#include <cai/network.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <net/if.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct cai_net {
  int fd;
  char local[192], remote[192];
};
static int net_error(cai_error *e, const char *message) {
  return cai_set_error_detail(e, CAI_ERR_TRANSPORT, message, strerror(errno));
}
static int socket_flags(int fd, cai_error *e) {
  int flags = fcntl(fd, F_GETFL, 0), one = 1;
  (void)one;
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
      fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
    return net_error(e, "configure network socket");
#ifdef SO_NOSIGPIPE
  if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) < 0)
    return net_error(e, "configure network SIGPIPE handling");
#endif
  return CAI_OK;
}
static void endpoint(const struct sockaddr *address, socklen_t size,
                     char out[192]) {
  char host[128], port[16];
  out[0] = '\0';
  if (getnameinfo(address, size, host, sizeof(host), port, sizeof(port),
                  NI_NUMERICHOST | NI_NUMERICSERV) == 0)
    snprintf(out, 192U, address->sa_family == AF_INET6 ? "[%s]:%s" : "%s:%s",
             host, port);
}
static int split_address(const char *input, int ipv6, char host[1025],
                         char port[6], cai_error *e) {
  const char *begin, *end, *service;
  char *tail;
  unsigned long number;
  size_t length;
  if (input == NULL || *input == '\0')
    return cai_set_error(e, CAI_ERR_INVALID, "network address is required");
  begin = input;
  end = strchr(input, ':');
  if (*input == '[') {
    begin++;
    end = strchr(begin, ']');
    if (end == NULL || end[1] != ':')
      return cai_set_error(e, CAI_ERR_INVALID, "use [IPv6]:PORT");
    service = end + 2;
  } else if (end != NULL) {
    if (strchr(end + 1, ':') != NULL)
      return cai_set_error(e, CAI_ERR_INVALID, "use [IPv6]:PORT");
    service = end + 1;
  } else {
    end = input;
    service = input;
  }
  length = (size_t)(end - begin);
  if (length >= 1025U || *service < '0' || *service > '9')
    return cai_set_error(e, CAI_ERR_INVALID, "invalid network host or port");
  errno = 0;
  number = strtoul(service, &tail, 10);
  if (errno || *tail || number == 0UL || number > 65535UL)
    return cai_set_error(e, CAI_ERR_INVALID, "network port must be 1..65535");
  if (length) {
    memcpy(host, begin, length);
    host[length] = '\0';
  } else
    strcpy(host, ipv6 ? "::1" : "127.0.0.1");
  snprintf(port, 6U, "%lu", number);
  return CAI_OK;
}
static int multicast_interface(const char *name, int family, struct in_addr *v4,
                               unsigned int *v6, cai_error *e) {
  struct ifaddrs *interfaces = NULL, *item;
  unsigned int index;
  char *end;
  unsigned long numeric;
  v4->s_addr = htonl(INADDR_ANY);
  *v6 = 0U;
  if (name == NULL || *name == '\0')
    return CAI_OK;
  if (family == AF_INET && inet_pton(AF_INET, name, v4) == 1)
    return CAI_OK;
  index = if_nametoindex(name);
  if (family == AF_INET6) {
    if (!index) {
      errno = 0;
      numeric = strtoul(name, &end, 10);
      if (errno || *name == '\0' || *end || numeric == 0UL ||
          numeric > (unsigned long)UINT_MAX)
        return cai_set_error(e, CAI_ERR_INVALID, "unknown multicast interface");
      index = (unsigned int)numeric;
    }
    *v6 = index;
    return CAI_OK;
  }
  if (index == 0U || getifaddrs(&interfaces) < 0)
    return cai_set_error(e, CAI_ERR_INVALID, "unknown multicast interface");
  for (item = interfaces; item != NULL; item = item->ifa_next)
    if (item->ifa_addr != NULL && item->ifa_addr->sa_family == AF_INET &&
        strcmp(item->ifa_name, name) == 0) {
      *v4 = ((struct sockaddr_in *)item->ifa_addr)->sin_addr;
      break;
    }
  freeifaddrs(interfaces);
  return item != NULL
             ? CAI_OK
             : cai_set_error(e, CAI_ERR_INVALID,
                             "multicast interface has no IPv4 address");
}
static int multicast(cai_net *n, const cai_net_config *config,
                     const struct addrinfo *address, cai_error *e) {
  struct in_addr v4;
  unsigned int v6;
  int ttl = config->ttl ? (int)config->ttl : 1;
  int loop = !config->disable_loop, zero = 0, rc;
  rc = multicast_interface(config->interface_name, address->ai_family, &v4, &v6,
                           e);
  if (rc != CAI_OK)
    return rc;
  if (address->ai_family == AF_INET) {
    struct ip_mreq group;
    unsigned char ttl_byte = (unsigned char)ttl;
    unsigned char loop_byte = (unsigned char)loop;
    group.imr_multiaddr = ((struct sockaddr_in *)address->ai_addr)->sin_addr;
    group.imr_interface = v4;
    if (!IN_MULTICAST(ntohl(group.imr_multiaddr.s_addr)))
      return cai_set_error(e, CAI_ERR_INVALID, "destination must be multicast");
    if (config->mode == CAI_NET_MULTICAST_RECEIVE) {
      if (bind(n->fd, address->ai_addr, address->ai_addrlen) < 0 ||
          setsockopt(n->fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &group,
                     sizeof(group)) < 0)
        return net_error(e, "join IPv4 multicast group");
#ifdef IP_MULTICAST_ALL
      if (setsockopt(n->fd, IPPROTO_IP, IP_MULTICAST_ALL, &zero, sizeof(zero)) <
          0)
        return net_error(e, "isolate IPv4 multicast delivery");
#endif
    } else if (setsockopt(n->fd, IPPROTO_IP, IP_MULTICAST_IF, &v4, sizeof(v4)) <
                   0 ||
               setsockopt(n->fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl_byte,
                          sizeof(ttl_byte)) < 0 ||
               setsockopt(n->fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop_byte,
                          sizeof(loop_byte)) < 0)
      return net_error(e, "configure IPv4 multicast sender");
  } else {
    struct ipv6_mreq group;
    struct sockaddr_in6 target = *(struct sockaddr_in6 *)address->ai_addr;
    if (!IN6_IS_ADDR_MULTICAST(&target.sin6_addr))
      return cai_set_error(e, CAI_ERR_INVALID, "destination must be multicast");
    group.ipv6mr_multiaddr = target.sin6_addr;
    group.ipv6mr_interface = v6 ? v6 : target.sin6_scope_id;
    if (config->mode == CAI_NET_MULTICAST_RECEIVE) {
      target.sin6_scope_id = group.ipv6mr_interface;
      if (bind(n->fd, (struct sockaddr *)&target, sizeof(target)) < 0 ||
          setsockopt(n->fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &group,
                     sizeof(group)) < 0)
        return net_error(e, "join IPv6 multicast group");
    } else if (setsockopt(n->fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &v6,
                          sizeof(v6)) < 0 ||
               setsockopt(n->fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &ttl,
                          sizeof(ttl)) < 0 ||
               setsockopt(n->fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &loop,
                          sizeof(loop)) < 0)
      return net_error(e, "configure IPv6 multicast sender");
  }
  (void)zero;
  return CAI_OK;
}
void cai_net_config_init(cai_net_config *config) {
  if (config != NULL)
    memset(config, 0, sizeof(*config));
}
int cai_net_open(const cai_net_config *config, cai_net **out, cai_error *e) {
  struct addrinfo hints, *addresses = NULL, *address;
  struct sockaddr_storage local;
  socklen_t size = sizeof(local);
  cai_net *n;
  char *host;
  char port[6];
  int rc, one = 1, datagram;
  if (out != NULL)
    *out = NULL;
  if (config == NULL || out == NULL || config->mode < CAI_NET_TCP_LISTEN ||
      config->mode > CAI_NET_MULTICAST_RECEIVE || config->ttl > 255U)
    return cai_set_error(e, CAI_ERR_INVALID,
                         "valid network configuration required");
  host = malloc(1025U);
  n = calloc(1U, sizeof(*n));
  if (host == NULL || n == NULL) {
    free(host);
    free(n);
    return cai_set_error(e, CAI_ERR_NOMEM, "allocate network endpoint");
  }
  n->fd = -1;
  rc = split_address(config->address, config->ipv6, host, port, e);
  memset(&hints, 0, sizeof(hints));
  datagram = config->mode >= CAI_NET_MULTICAST_SEND;
  hints.ai_family = datagram ? AF_UNSPEC : config->ipv6 ? AF_INET6 : AF_INET;
  hints.ai_socktype = datagram ? SOCK_DGRAM : SOCK_STREAM;
  hints.ai_flags = AI_NUMERICSERV;
  if (rc == CAI_OK) {
    int resolved = getaddrinfo(host, port, &hints, &addresses);
    if (resolved != 0)
      rc =
          cai_set_error_detail(e, CAI_ERR_TRANSPORT, "resolve network endpoint",
                               gai_strerror(resolved));
  }
  free(host);
  for (address = addresses; rc == CAI_OK && address != NULL;
       address = address->ai_next) {
    n->fd =
        socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (n->fd >= 0)
      break;
  }
  if (rc == CAI_OK && n->fd < 0)
    rc = net_error(e, "open network socket");
  if (rc == CAI_OK)
    rc = socket_flags(n->fd, e);
  if (rc == CAI_OK && address->ai_family == AF_INET6 &&
      setsockopt(n->fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one)) < 0)
    rc = net_error(e, "select IPv6-only socket");
  if (rc == CAI_OK &&
      (config->mode == CAI_NET_TCP_LISTEN ||
       config->mode == CAI_NET_MULTICAST_RECEIVE) &&
      setsockopt(n->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0)
    rc = net_error(e, "configure socket reuse");
  if (rc == CAI_OK && datagram)
    rc = multicast(n, config, address, e);
  if (rc == CAI_OK && config->mode == CAI_NET_TCP_LISTEN) {
    if (bind(n->fd, address->ai_addr, address->ai_addrlen) < 0 ||
        listen(n->fd, 16) < 0)
      rc = net_error(e, "listen for TCP prompts");
  } else if (rc == CAI_OK && config->mode != CAI_NET_MULTICAST_RECEIVE) {
    if (connect(n->fd, address->ai_addr, address->ai_addrlen) < 0 &&
        errno != EINPROGRESS)
      rc = net_error(e, "connect network endpoint");
  }
  if (rc == CAI_OK) {
    endpoint(address->ai_addr, address->ai_addrlen, n->remote);
    if (getsockname(n->fd, (struct sockaddr *)&local, &size) == 0)
      endpoint((struct sockaddr *)&local, size, n->local);
    *out = n;
  } else
    cai_net_close(n, 1);
  if (addresses != NULL)
    freeaddrinfo(addresses);
  return rc;
}
int cai_net_fd(const cai_net *n) { return n != NULL ? n->fd : -1; }
const char *cai_net_local(const cai_net *n) {
  return n != NULL ? n->local : "";
}
const char *cai_net_remote(const cai_net *n) {
  return n != NULL ? n->remote : "";
}
int cai_net_connected(cai_net *n, cai_error *e) {
  int status = 0;
  socklen_t size = sizeof(status);
  if (n == NULL)
    return cai_set_error(e, CAI_ERR_INVALID, "network socket required");
  if (getsockopt(n->fd, SOL_SOCKET, SO_ERROR, &status, &size) < 0)
    return net_error(e, "read network connection status");
  if (status) {
    errno = status;
    return net_error(e, "connect network endpoint");
  }
  return CAI_OK;
}
int cai_net_accept(cai_net *listener, cai_net **out, cai_error *e) {
  cai_net *n;
  struct sockaddr_storage address;
  socklen_t size = sizeof(address);
  int fd, rc;
  if (listener == NULL || out == NULL)
    return cai_set_error(e, CAI_ERR_INVALID,
                         "listener and peer output required");
  *out = NULL;
  fd = accept(listener->fd, (struct sockaddr *)&address, &size);
  if (fd < 0)
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR
               ? CAI_OK
               : net_error(e, "accept TCP prompt");
  n = calloc(1U, sizeof(*n));
  if (n == NULL) {
    close(fd);
    return cai_set_error(e, CAI_ERR_NOMEM, "allocate TCP prompt peer");
  }
  n->fd = fd;
  rc = socket_flags(fd, e);
  if (rc != CAI_OK) {
    cai_net_close(n, 1);
    return rc;
  }
  endpoint((struct sockaddr *)&address, size, n->remote);
  strcpy(n->local, listener->local);
  *out = n;
  return CAI_OK;
}
int cai_net_send(cai_net *n, const void *bytes, size_t count, size_t *written,
                 cai_error *e) {
  ssize_t sent;
  int flags = 0;
  if (written != NULL)
    *written = 0U;
  if (n == NULL || written == NULL || (count != 0U && bytes == NULL))
    return cai_set_error(e, CAI_ERR_INVALID,
                         "network socket, data and output required");
#ifdef MSG_NOSIGNAL
  flags = MSG_NOSIGNAL;
#endif
  sent = send(n->fd, bytes, count, flags);
  if (sent < 0)
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR
               ? CAI_OK
               : net_error(e, "send network bytes");
  *written = (size_t)sent;
  return CAI_OK;
}
void cai_net_close(cai_net *n, int abort_transfer) {
  if (n != NULL) {
    if (n->fd >= 0) {
      if (abort_transfer) {
        struct linger immediate;
        immediate.l_onoff = 1;
        immediate.l_linger = 0;
        (void)setsockopt(n->fd, SOL_SOCKET, SO_LINGER, &immediate,
                         sizeof(immediate));
      }
      close(n->fd);
    }
    free(n);
  }
}
