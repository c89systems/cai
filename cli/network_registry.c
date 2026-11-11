/** @file Retained collection claims, public endpoint records and UDP JSONL. */
#define _POSIX_C_SOURCE 200809L
#include "network_registry.h"
#include <arpa/inet.h>
#include <cai/network.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define NETWORK_LEASE_SECONDS 86400L
/* Claims outlive their lease by a full renewal interval, then maintenance can
 * reclaim crash leftovers. Refresh this metadata only with lease renewal. */
#define NETWORK_CLAIM_RETENTION (2L * NETWORK_LEASE_SECONDS)
typedef struct endpoint_record {
  struct endpoint_record *next;
  char id[96], kind[32], protocol[16], bind[256], advertised[256], host[256];
  char workspace[4096], workspace_name[256], server_instance[33],
      session_id[129];
  char role[16], socat[2048];
  lonejson_uint64 attachment_id;
  int available, host_local;
} endpoint_record;
#define TEXT(member)                                                           \
  LONEJSON_FIELD_STRING_FIXED(endpoint_record, member, #member,                \
                              LONEJSON_OVERFLOW_FAIL)
static const lonejson_field endpoint_fields[] = {
    TEXT(id),
    TEXT(kind),
    TEXT(protocol),
    TEXT(bind),
    TEXT(advertised),
    TEXT(host),
    TEXT(workspace),
    TEXT(workspace_name),
    TEXT(server_instance),
    TEXT(session_id),
    TEXT(role),
    TEXT(socat),
    LONEJSON_FIELD_U64(endpoint_record, attachment_id, "attachment_id"),
    LONEJSON_FIELD_BOOL(endpoint_record, available, "available"),
    LONEJSON_FIELD_BOOL(endpoint_record, host_local, "host_local")};
#undef TEXT
LONEJSON_MAP_DEFINE(endpoint_map, endpoint_record, endpoint_fields);
struct cai_cli_network_registry {
  cai_cli_pouch *pouch;
  const cai_cli_options *options;
  char ns[129], instance[33], workspace[4096], session[129];
  lc_collection *owner, *allocation;
  lc_unix_seconds expires;
  uint64_t renew_at;
  cai_net *publisher;
  cai_app_network_event metadata;
  char destination[256], socat[2048];
  endpoint_record *records;
  unsigned long dropped;
  unsigned long reported;
  uint64_t report_at;
  int failed;
};
static uint64_t now_ms(void) {
  struct timespec now;
  (void)clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}
static int claim_retention(lc_collection *collection, lc_error *error) {
  lc_collection_metadata_req request;
  lc_collection_info info;
  lc_collection_metadata_req_init(&request);
  request.has_retention_seconds = 1;
  request.retention_seconds = NETWORK_CLAIM_RETENTION;
  return collection->metadata(collection, &request, &info, error);
}
static int claim(cai_cli_network_registry *r, const char *name,
                 lc_collection **out, lc_error *error) {
  lc_collection_config config;
  lc_collection_lease_req lease;
  lc_collection *plain = NULL;
  int rc;
  lc_collection_config_init(&config);
  lc_collection_lease_req_init(&lease);
  config.ns = r->ns;
  config.name = name;
  lease.owner = r->instance;
  lease.ttl_seconds = NETWORK_LEASE_SECONDS;
  config.lease = &lease;
  config.has_retention_seconds = 1;
  config.retention_seconds = NETWORK_CLAIM_RETENTION;
  rc = lc_client_create_collection(r->pouch->sessions, &config, out, error);
  if (rc == LC_OK || error->server_code == NULL ||
      strcmp(error->server_code, "collection_exists"))
    return rc;
  lc_error_cleanup(error);
  config.lease = NULL;
  config.has_retention_seconds = 0;
  rc = lc_client_open_collection(r->pouch->sessions, &config, &plain, error);
  if (rc == LC_OK)
    rc = plain->acquire(plain, &lease, out, error);
  if (rc == LC_OK)
    rc = claim_retention(*out, error);
  if (plain != NULL)
    plain->close(plain);
  return rc;
}
static int release_state(lc_lease *lease, int result, lc_error *error) {
  lc_release_req request;
  lc_error local;
  int rc;
  lc_release_req_init(&request);
  request.rollback = result != LC_OK;
  lc_error_init(&local);
  rc = lease->release(lease, &request, &local);
  if (rc != LC_OK) {
    lease->close(lease);
    if (result == LC_OK)
      lc_error_set(error, local.code, local.http_status, local.message,
                   local.detail, local.server_code, local.correlation_id);
  }
  lc_error_cleanup(&local);
  return result == LC_OK ? rc : result;
}
static int save(cai_cli_network_registry *r, endpoint_record *record,
                int remove, cai_error *error) {
  lc_acquire_req request;
  lc_update_opts options;
  lc_lease *lease = NULL;
  lc_error local;
  int rc;
  if ((!remove && r->failed) || time(NULL) >= (time_t)r->expires)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "network registration ownership expired");
  lc_error_init(&local);
  lc_acquire_req_init(&request);
  lc_update_opts_init(&options);
  request.ns = r->ns;
  request.key = record->id;
  request.owner = r->instance;
  request.ttl_seconds = 30L;
  options.has_retention_seconds = 1;
  options.retention_seconds = NETWORK_LEASE_SECONDS;
  rc = lc_acquire(r->pouch->sessions, &request, &lease, &local);
  if (rc == LC_OK)
    rc = remove ? lc_lease_remove(lease, NULL, &local)
                : lc_lease_save(lease, &endpoint_map, record, &options, &local);
  if (lease != NULL)
    rc = release_state(lease, rc, &local);
  if (rc != LC_OK)
    rc = cai_cli_lc_error(error, &local);
  lc_error_cleanup(&local);
  return rc;
}
static endpoint_record *new_record(cai_cli_network_registry *r,
                                   uint64_t attachment) {
  endpoint_record *record = calloc(1U, sizeof(*record));
  if (record == NULL)
    return NULL;
  snprintf(record->id, sizeof(record->id), "endpoint-%s-%llu", r->instance,
           (unsigned long long)attachment);
  snprintf(record->server_instance, sizeof(record->server_instance), "%s",
           r->instance);
  snprintf(record->workspace, sizeof(record->workspace), "%s", r->workspace);
  snprintf(record->workspace_name, sizeof(record->workspace_name), "%s",
           r->options->workspace_name != NULL ? r->options->workspace_name
                                              : "");
  (void)gethostname(record->host, sizeof(record->host) - 1U);
  record->attachment_id = attachment;
  record->next = r->records;
  r->records = record;
  return record;
}
static int shell_quote(char *out, size_t capacity, const char *text) {
  size_t used = 0U;
  if (capacity < 3U)
    return 0;
  out[used++] = '\'';
  while (*text) {
    const char *part = *text == '\'' ? "'\\''" : text;
    size_t count = *text == '\'' ? 4U : 1U;
    if (count > capacity - used - 2U)
      return 0;
    memcpy(out + used, part, count);
    used += count;
    text++;
  }
  out[used++] = '\'';
  out[used] = '\0';
  return 1;
}
static int multicast_start(cai_cli_network_registry *r, cai_error *error) {
  const cai_cli_options *o = r->options;
  cai_net_config config;
  struct in_addr base;
  lc_error local;
  char name[320], group[INET_ADDRSTRLEN], *argument, *quoted;
  const char *port;
  int rc = CAI_OK, automatic = !strcmp(o->event_multicast, "auto");
  long index;
  endpoint_record *record;
  lc_error_init(&local);
  if (o->ip_multicast_ttl < 1L || o->ip_multicast_ttl > 255L ||
      o->event_multicast_count < 1L || o->event_multicast_count > 65535L ||
      o->event_multicast_port < 1L || o->event_multicast_port > 65535L)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "invalid multicast pool/port/hop limit");
  if (inet_pton(AF_INET, o->event_multicast_base, &base) != 1 ||
      !IN_MULTICAST(ntohl(base.s_addr)) ||
      (uint64_t)ntohl(base.s_addr) + (uint64_t)o->event_multicast_count - 1U >
          0xefffffffU)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "automatic pool must fit IPv4 multicast space");
  for (index = 0L; index < (automatic ? o->event_multicast_count : 1L);
       index++) {
    if (automatic) {
      struct in_addr address;
      address.s_addr = htonl(ntohl(base.s_addr) + (uint32_t)index);
      (void)inet_ntop(AF_INET, &address, group, sizeof(group));
      snprintf(r->destination, sizeof(r->destination), "%s:%ld", group,
               o->event_multicast_port);
    } else if (strlen(o->event_multicast) >= sizeof(r->destination))
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "multicast endpoint too long");
    else
      strcpy(r->destination, o->event_multicast);
    cai_net_config_init(&config);
    config.mode = CAI_NET_MULTICAST_SEND;
    config.address = r->destination;
    config.interface_name = o->ip_multicast_if;
    config.ttl = (unsigned int)o->ip_multicast_ttl;
    config.disable_loop = o->disable_multicast_loop;
    rc = cai_net_open(&config, &r->publisher, error);
    if (rc != CAI_OK)
      break;
    snprintf(r->destination, sizeof(r->destination), "%s",
             cai_net_remote(r->publisher));
    snprintf(name, sizeof(name), "allocation-%s", r->destination);
    rc = claim(r, name, &r->allocation, &local);
    if (rc == LC_OK)
      break;
    cai_net_close(r->publisher, 0);
    r->publisher = NULL;
    if (!automatic || local.server_code == NULL ||
        strcmp(local.server_code, "collection_leased")) {
      rc = cai_cli_lc_error(error, &local);
      break;
    }
    lc_error_cleanup(&local);
  }
  if (rc != CAI_OK || r->publisher == NULL) {
    if (rc == CAI_OK || (automatic && index == o->event_multicast_count))
      rc = cai_cli_error(error, CAI_ERR_LIMIT,
                         "CAI multicast allocation pool exhausted");
    lc_error_cleanup(&local);
    return rc;
  }
  {
    lc_collection_info info;
    rc = r->allocation->describe(r->allocation, &info, &local);
    if (rc != LC_OK)
      return cai_cli_lc_error(error, &local);
    if (info.lease.expires_at_unix < r->expires)
      r->expires = info.lease.expires_at_unix;
  }
  argument = malloc(1024U);
  quoted = malloc(2040U);
  if (argument == NULL || quoted == NULL) {
    free(argument);
    free(quoted);
    return cai_cli_error(error, CAI_ERR_NOMEM,
                         "format multicast receive command");
  }
  port = strrchr(r->destination, ':');
  if (r->destination[0] == '[') {
    size_t length = (size_t)(port - r->destination - 2);
    char *host = calloc(length + 1U, 1U);
    if (host == NULL) {
      free(argument);
      free(quoted);
      return cai_cli_error(error, CAI_ERR_NOMEM, "format IPv6 multicast");
    }
    memcpy(host, r->destination + 1, length);
    snprintf(argument, 1024U,
             "UDP6-RECV:%s,bind=[%s],reuseaddr,ipv6-join-group=[%s]:%s",
             port + 1, host, host,
             o->ip_multicast_if != NULL && *o->ip_multicast_if
                 ? o->ip_multicast_if
                 : "0");
    free(host);
  } else {
    size_t length = (size_t)(port - r->destination);
    memcpy(group, r->destination, length);
    group[length] = '\0';
    snprintf(argument, 1024U,
             "UDP4-RECV:%s,bind=%s,reuseaddr,ip-add-membership=%s:%s", port + 1,
             group, group,
             o->ip_multicast_if != NULL && *o->ip_multicast_if
                 ? o->ip_multicast_if
                 : "0.0.0.0");
  }
  if (!shell_quote(quoted, 2040U, argument))
    rc = cai_cli_error(error, CAI_ERR_LIMIT,
                       "multicast receive command too long");
  else
    snprintf(r->socat, sizeof(r->socat), "socat -u %s STDOUT", quoted);
  free(argument);
  free(quoted);
  if (rc != CAI_OK)
    return rc;
  r->metadata.header.kind = CAI_PROTOCOL_NETWORK;
  r->metadata.operation = CAI_APP_NETWORK_PUBLISHER;
  r->metadata.address = r->destination;
  r->metadata.socat = r->socat;
  r->metadata.interface_name = o->ip_multicast_if;
  record = new_record(r, 0U);
  if (record == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM,
                         "allocate publisher registration");
  strcpy(record->kind, "multicast_publisher");
  strcpy(record->protocol, r->destination[0] == '[' ? "udpv6" : "udpv4");
  strcpy(record->advertised, r->destination);
  strcpy(record->socat, r->socat);
  record->available = 1;
  rc = save(r, record, 0, error);
  if (rc == CAI_OK && r->pouch->logger != NULL) {
    pslog_field fields[3];
    fields[0] = pslog_str("destination", r->destination);
    fields[1] =
        pslog_str("interface", o->ip_multicast_if != NULL ? o->ip_multicast_if
                                                          : "routing default");
    fields[2] = pslog_str("socat", r->socat);
    r->pouch->logger->info(r->pouch->logger,
                           "Workspace event multicast enabled", fields, 3U);
  }
  lc_error_cleanup(&local);
  return rc;
}
static void observe(void *context, const char *id, unsigned long long position,
                    const cai_agent_session_event *event) {
  cai_cli_network_registry *r = context;
  cai_source *source = NULL;
  cai_error error;
  unsigned char bytes[1200];
  size_t count, sent;
  int rc = CAI_OK;
  (void)id;
  (void)position;
  /* No SDK calls, waits, logging or reentry under the journal lock. */
  if (r->publisher == NULL || time(NULL) >= (time_t)r->expires)
    return;
  cai_error_init(&error);
  rc = cai_session_event_source(event, &source, &error);
  while (rc == CAI_OK && (count = cai_source_read(source, bytes, sizeof(bytes),
                                                  &error)) != 0U) {
    rc = cai_net_send(r->publisher, bytes, count, &sent, &error);
    if (rc == CAI_OK && sent != count)
      rc = CAI_ERR_TRANSPORT;
  }
  if (rc == CAI_OK && error.code == CAI_OK) {
    rc = cai_net_send(r->publisher, "\n", 1U, &sent, &error);
    if (rc == CAI_OK && sent != 1U)
      rc = CAI_ERR_TRANSPORT;
  }
  if (rc != CAI_OK || error.code != CAI_OK)
    r->dropped++;
  cai_source_close(source);
  cai_error_cleanup(&error);
}
int cai_cli_network_registry_open(cai_cli_pouch *pouch,
                                  const cai_cli_options *options,
                                  const char *workspace, const char *instance,
                                  cai_cli_network_registry **out,
                                  cai_error *error) {
  cai_cli_network_registry *r = calloc(1U, sizeof(*r));
  lc_error local;
  char name[80];
  int rc;
  *out = NULL;
  if (r == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate network registry");
  r->pouch = pouch;
  r->options = options;
  rc = snprintf(r->ns, sizeof(r->ns), "%s.network",
                options->namespace_prefix != NULL ? options->namespace_prefix
                                                  : options->program_name);
  if (rc < 0 || (size_t)rc >= sizeof(r->ns)) {
    free(r);
    return cai_cli_error(error, CAI_ERR_LIMIT,
                         "network namespace exceeds 128 bytes");
  }
  snprintf(r->instance, sizeof(r->instance), "%s", instance);
  snprintf(r->workspace, sizeof(r->workspace), "%s", workspace);
  snprintf(name, sizeof(name), "owner-%s", instance);
  lc_error_init(&local);
  rc = claim(r, name, &r->owner, &local);
  if (rc == LC_OK) {
    lc_collection_info info;
    rc = r->owner->describe(r->owner, &info, &local);
    if (rc == LC_OK)
      r->expires = info.lease.expires_at_unix;
  }
  r->renew_at = now_ms() + (uint64_t)NETWORK_LEASE_SECONDS * 500U;
  if (rc != LC_OK)
    rc = cai_cli_lc_error(error, &local);
  if (rc == CAI_OK && options->event_multicast != NULL &&
      *options->event_multicast)
    rc = multicast_start(r, error);
  if (rc == CAI_OK) {
    pthread_mutex_lock(&pouch->journal_lock);
    pouch->network_observer = observe;
    pouch->network_observer_context = r;
    pthread_mutex_unlock(&pouch->journal_lock);
    *out = r;
  } else
    cai_cli_network_registry_close(r);
  lc_error_cleanup(&local);
  return rc;
}
int cai_cli_network_registry_listener(cai_cli_network_registry *r,
                                      uint64_t attachment,
                                      const cai_app_network_event *event,
                                      const char *session, int controller,
                                      cai_error *error) {
  endpoint_record *record;
  if (r == NULL)
    return CAI_OK;
  if (event->address == NULL || !*event->address) {
    cai_cli_network_registry_remove(r, attachment);
    return CAI_OK;
  }
  if (strlen(event->address) >= 256U ||
      (event->advertise != NULL && strlen(event->advertise) >= 256U))
    return cai_cli_error(error, CAI_ERR_LIMIT, "listener address too long");
  for (record = r->records;
       record != NULL && record->attachment_id != attachment;
       record = record->next) {
  }
  if (record == NULL)
    record = new_record(r, attachment);
  if (record == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM,
                         "allocate listener registration");
  strcpy(record->kind, "tcp_listener");
  strcpy(record->protocol, event->address[0] == '[' ? "tcpv6" : "tcpv4");
  strcpy(record->bind, event->address);
  strcpy(record->advertised, event->advertise != NULL && *event->advertise
                                 ? event->advertise
                                 : event->address);
  if ((strncmp(record->bind, "0.0.0.0:", 8U) == 0 ||
       strncmp(record->bind, "[::]:", 5U) == 0) &&
      (event->advertise == NULL || !*event->advertise))
    record->advertised[0] = '\0';
  record->host_local = !strncmp(record->advertised, "127.", 4U) ||
                       !strncmp(record->advertised, "[::1]:", 6U);
  snprintf(record->session_id, sizeof(record->session_id), "%s", session);
  strcpy(record->role, controller ? "controller" : "observer");
  record->available =
      controller && (event->flags & CAI_APP_NETWORK_AVAILABLE) != 0U;
  return save(r, record, 0, error);
}
void cai_cli_network_registry_remove(cai_cli_network_registry *r,
                                     uint64_t attachment) {
  endpoint_record **link;
  if (r == NULL)
    return;
  for (link = &r->records; *link != NULL; link = &(*link)->next)
    if ((*link)->attachment_id == attachment) {
      endpoint_record *record = *link;
      (void)save(r, record, 1, NULL);
      *link = record->next;
      free(record);
      break;
    }
}
int cai_cli_network_registry_session(cai_cli_network_registry *r,
                                     const char *session, cai_error *error) {
  endpoint_record *record;
  int rc = CAI_OK;
  if (r == NULL || !strcmp(r->session, session))
    return CAI_OK;
  snprintf(r->session, sizeof(r->session), "%s", session);
  for (record = r->records; rc == CAI_OK && record != NULL;
       record = record->next) {
    snprintf(record->session_id, sizeof(record->session_id), "%s", session);
    rc = save(r, record, 0, error);
  }
  return rc;
}
const cai_app_network_event *
cai_cli_network_registry_publisher(cai_cli_network_registry *r) {
  return r != NULL && r->publisher != NULL ? &r->metadata : NULL;
}
uint64_t cai_cli_network_registry_deadline(cai_cli_network_registry *r) {
  return r != NULL && !r->failed ? r->renew_at : 0U;
}
int cai_cli_network_registry_renew(cai_cli_network_registry *r,
                                   cai_error *error) {
  lc_collection_lease_info lease;
  lc_unix_seconds expires = 0;
  lc_error local;
  int rc = LC_OK;
  endpoint_record *record;
  if (r == NULL || r->failed)
    return CAI_OK;
  {
    unsigned long dropped;
    pthread_mutex_lock(&r->pouch->journal_lock);
    dropped = r->dropped;
    pthread_mutex_unlock(&r->pouch->journal_lock);
    if (dropped != r->reported && now_ms() >= r->report_at &&
        r->pouch->logger != NULL) {
      pslog_field field = pslog_u64("dropped_events", dropped);
      r->pouch->logger->warn(r->pouch->logger,
                             "Best-effort multicast events were dropped",
                             &field, 1U);
      r->reported = dropped;
      r->report_at = now_ms() + 1000U;
    }
  }
  if (now_ms() < r->renew_at)
    return CAI_OK;
  lc_error_init(&local);
  rc = r->owner->keepalive(r->owner, NETWORK_LEASE_SECONDS, &lease, &local);
  if (rc == LC_OK)
    expires = lease.expires_at_unix;
  if (rc == LC_OK && r->allocation != NULL)
    rc = r->allocation->keepalive(r->allocation, NETWORK_LEASE_SECONDS, &lease,
                                  &local);
  if (rc == LC_OK)
    rc = claim_retention(r->owner, &local);
  if (rc == LC_OK && r->allocation != NULL)
    rc = claim_retention(r->allocation, &local);
  if (rc == LC_OK) {
    pthread_mutex_lock(&r->pouch->journal_lock);
    r->expires =
        expires < lease.expires_at_unix ? expires : lease.expires_at_unix;
    pthread_mutex_unlock(&r->pouch->journal_lock);
    r->renew_at = now_ms() + (uint64_t)NETWORK_LEASE_SECONDS * 500U;
    for (record = r->records; rc == CAI_OK && record != NULL;
         record = record->next)
      rc = save(r, record, 0, error);
  } else {
    pthread_mutex_lock(&r->pouch->journal_lock);
    cai_net_close(r->publisher, 0);
    r->publisher = NULL;
    pthread_mutex_unlock(&r->pouch->journal_lock);
    rc = cai_cli_lc_error(error, &local);
    r->failed = 1;
  }
  lc_error_cleanup(&local);
  return rc;
}
void cai_cli_network_registry_close(cai_cli_network_registry *r) {
  if (r == NULL)
    return;
  pthread_mutex_lock(&r->pouch->journal_lock);
  if (r->pouch->network_observer_context == r) {
    r->pouch->network_observer = NULL;
    r->pouch->network_observer_context = NULL;
  }
  cai_net_close(r->publisher, 0);
  r->publisher = NULL;
  pthread_mutex_unlock(&r->pouch->journal_lock);
  while (r->records != NULL)
    cai_cli_network_registry_remove(r, r->records->attachment_id);
  if (r->allocation != NULL) {
    (void)lc_collection_drop(r->allocation, NULL);
    r->allocation->close(r->allocation);
  }
  if (r->owner != NULL) {
    (void)lc_collection_drop(r->owner, NULL);
    r->owner->close(r->owner);
  }
  free(r);
}
#include "network_registry_list.inc"
