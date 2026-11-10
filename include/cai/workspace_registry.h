/** @file cai/workspace_registry.h
 * Incremental native discovery hints. No runtime, storage client or threads.
 */
#ifndef CAI_WORKSPACE_REGISTRY_H
#define CAI_WORKSPACE_REGISTRY_H
#include <cai/protocol.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct cai_workspace_registry cai_workspace_registry;
typedef struct cai_workspace_record {
  const char *workspace, *name, *id;
  unsigned char instance[CAI_WORKSPACE_INSTANCE_BYTES];
} cai_workspace_record;
/** Open an existing private, owned absolute runtime directory. Single-owner
 * handle. Does not connect, acquire authority or open session storage. */
int cai_workspace_registry_open(const char *runtime_directory,
                                cai_workspace_registry **out, cai_error *);
/** Read one bounded record at a time. Borrowed until the next call/close.
 * CAI_OK with *out == NULL means end. A malformed/unsafe record returns an
 * error but advances; callers may continue listing. System I/O/allocation
 * failures abort the operation instead. Other files are ignored.
 * Records are hints, including crashed owners: validate canonical identity and
 * instance with a workspace client probe before presenting them as live.
 * Metadata has at most 4095 workspace bytes and 255 name bytes. Names are
 * printable UTF-8 without slash; empty means no name. Escape display text.
 */
int cai_workspace_registry_next(cai_workspace_registry *,
                                const cai_workspace_record **out, cai_error *);
/** Remove only the current record, under its nonblocking workspace ownership
 * lock, and only if its inode is unchanged. CAI_ERR_LIMIT means a live/startup
 * owner still holds the lock; nothing is removed. Never unlinks socket/lock
 * files. Caller must first establish that the hint is stale by probing.
 */
int cai_workspace_registry_prune(cai_workspace_registry *, cai_error *);
void cai_workspace_registry_close(cai_workspace_registry *);
#ifdef __cplusplus
}
#endif
#endif
