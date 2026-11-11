/** @file cai/network.h Bounded native network transports for host event loops.
 */
#ifndef CAI_NETWORK_H
#define CAI_NETWORK_H
#include <cai/session_store.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct cai_net cai_net;
typedef enum cai_net_mode {
  CAI_NET_TCP_LISTEN = 0,
  CAI_NET_TCP_CONNECT,
  CAI_NET_MULTICAST_SEND,
  CAI_NET_MULTICAST_RECEIVE
} cai_net_mode;
typedef struct cai_net_config {
  cai_net_mode mode;
  /** PORT, :PORT, HOST:PORT or [IPv6]:PORT. Borrowed during open. */
  const char *address;
  /** TCP family; multicast infers its family from the destination. */
  int ipv6;
  const char *interface_name;
  /** Zero selects the ordinary one-hop multicast default. */
  unsigned int ttl;
  int disable_loop;
} cai_net_config;
void cai_net_config_init(cai_net_config *);
/** Resolves names synchronously on the caller. All returned sockets are
 * nonblocking and close-on-exec. TCP connect may still require POLLOUT and
 * cai_net_connected(). Hosts resolve/open outbound endpoints off their UI loop.
 * Receivers bind to their selected group and restrict multicast delivery. */
int cai_net_open(const cai_net_config *, cai_net **out, cai_error *);
int cai_net_fd(const cai_net *);
/** Borrow the numeric local endpoint through close. */
const char *cai_net_local(const cai_net *);
/** Borrow the numeric destination endpoint through close. */
const char *cai_net_remote(const cai_net *);
int cai_net_connected(cai_net *, cai_error *);
/** Accept one peer. Success with *out=NULL means no pending connection. */
int cai_net_accept(cai_net *, cai_net **out, cai_error *);
/** Send without SIGPIPE. Success with *written=0 means try after POLLOUT. */
int cai_net_send(cai_net *, const void *, size_t, size_t *written, cai_error *);
/** abort_transfer requests an abortive TCP close for a failed partial prompt.
 */
void cai_net_close(cai_net *, int abort_transfer);
/** Pull the exact stored session-event JSON object, borrowing event strings
 * through source close. No whole-event allocation or spool. The caller adds
 * its JSONL separator after successfully consuming this source. */
int cai_session_event_source(const cai_agent_session_event *, cai_source **,
                             cai_error *);
#ifdef __cplusplus
}
#endif
#endif
