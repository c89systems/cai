/** @file cai/workspace.h
 * Native per-workspace Unix endpoint ownership and discovery primitives.
 * Handles and operations belong to one host event-loop thread.
 */
#ifndef CAI_WORKSPACE_H
#define CAI_WORKSPACE_H
#include <cai/cai.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct cai_workspace_endpoint cai_workspace_endpoint;
/** Resolve canonical workspace and runtime directories without changing cwd.
 * runtime_directory must already be a private directory owned by this user.
 * Derive a short socket name from SHA-256 of the canonical workspace. The
 * protocol greeting must still verify the full workspace identity.
 * No lock/socket/store is opened until listen/connect is explicitly called.
 */
int cai_workspace_endpoint_open(const char *runtime_directory,
                                const char *workspace,
                                cai_workspace_endpoint **out, cai_error *error);
const char *cai_workspace_endpoint_directory(const cai_workspace_endpoint *);
const char *cai_workspace_endpoint_id(const cai_workspace_endpoint *);
const char *cai_workspace_endpoint_socket(const cai_workspace_endpoint *);
/** Acquire an exclusive nonblocking OS lock and create a private listener.
 * CAI_ERR_LIMIT means another owner holds the lock or a live endpoint exists.
 * Reclaims only an owned private stale socket while holding that lock. Never
 * removes a regular file/symlink or a live endpoint. The lock file remains on
 * disk after close to avoid split locks on different inodes.
 */
int cai_workspace_endpoint_listen(cai_workspace_endpoint *, cai_error *);
/** Borrow the listening descriptor for POLLIN; -1 before listen. */
int cai_workspace_endpoint_fd(const cai_workspace_endpoint *);
/** Nonblocking accept. CAI_OK with *descriptor == -1 means no pending peer.
 * Successful descriptors are nonblocking/CLOEXEC and owned by the caller;
 * Unix peer credentials must match the effective user. */
int cai_workspace_endpoint_accept(cai_workspace_endpoint *, int *descriptor,
                                  cai_error *);
/** Connect without waiting. Successful descriptors transfer to the caller;
 * connecting == 1 requires POLLOUT then connect_finish. Missing/refused socket
 * returns CAI_ERR_TRANSPORT; unsafe endpoint returns CAI_ERR_INVALID. Neither
 * case unlinks an endpoint. connect_finish also verifies peer credentials.
 */
int cai_workspace_endpoint_connect(const cai_workspace_endpoint *,
                                   int *descriptor, int *connecting,
                                   cai_error *);
int cai_workspace_endpoint_connect_finish(int descriptor, cai_error *);
/** Close this listener and release its lock. Unlinks only the socket inode
 * created by this handle, while still holding ownership. Connected descriptors
 * returned by accept/connect remain caller-owned. NULL is accepted. */
void cai_workspace_endpoint_close(cai_workspace_endpoint *);

#ifdef __cplusplus
}
#endif
#endif
