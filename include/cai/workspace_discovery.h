/** @file cai/workspace_discovery.h
 * Event-loop workspace lookup/listing through verified identity-only probes.
 */
#ifndef CAI_WORKSPACE_DISCOVERY_H
#define CAI_WORKSPACE_DISCOVERY_H
#include <cai/workspace_client.h>
#include <cai/workspace_registry.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct cai_workspace_discovery cai_workspace_discovery;
typedef enum cai_workspace_discovery_phase {
  CAI_WORKSPACE_DISCOVERING = 0,
  CAI_WORKSPACE_FOUND,
  CAI_WORKSPACE_LISTED,
  CAI_WORKSPACE_MISSING,
  CAI_WORKSPACE_AMBIGUOUS,
  CAI_WORKSPACE_UNAVAILABLE,
  CAI_WORKSPACE_DISCOVERY_FAILED
} cai_workspace_discovery_phase;
typedef struct cai_workspace_discovery_state {
  cai_workspace_discovery_phase phase;
  size_t matches, errors;
  /** Latest observation/failure code; phase determines the operation outcome.
   */
  int error_code;
  /** Distinct live identities for an ambiguous name/ID selector. */
  char first[33], second[33];
} cai_workspace_discovery_state;
typedef struct cai_workspace_discovery_result {
  cai_workspace_record record;
  cai_attachment_phase phase;
  int persist;
} cai_workspace_discovery_result;
typedef struct cai_workspace_discovery_event {
  /** Borrowed until callback returns. Non-NULL for a candidate; NULL for
   * malformed registry records. Strings require literal display escaping. */
  const cai_workspace_discovery_result *entry;
  /** NULL for a fully verified STARTING/READY owner. Other entries could not
   * be verified; they are never returned as a successful lookup. A verified
   * event is still only a candidate: wait for FOUND before attaching after a
   * lookup, so a later name/ID ambiguity cannot acquire control prematurely. */
  const cai_error *error;
} cai_workspace_discovery_event;
typedef struct cai_workspace_discovery_config {
  const char *runtime_directory;
  /** NULL/empty lists verified owners. Otherwise match name or complete stable
   * ID without precedence; multiple live matches are ambiguous. Explicit paths
   * start with /, ./ or ../, are . or .., or contain a slash. They bypass the
   * registry and never create a directory/server or fall back to the cwd. */
  const char *selector;
  /** Absolute base required for relative explicit paths. No process cwd or
   * environment is changed or consulted to interpret those paths. */
  const char *directory;
  /** Zero selects ten seconds per probe. Listing/lookup remains cancellable
   * by close and does not hold a controller attachment while waiting. */
  uint64_t probe_timeout_ms;
  /** Optional borrowed callback/context, called on the opening thread.
   * Errors are inline observations; one bad/stale hint does not block others.
   * Callback failure terminates only this operation. State may be queried;
   * callbacks must not reenter pump or close. No runtime/storage/worker is
   * created. */
  int (*event)(void *, const cai_workspace_discovery_event *, cai_error *);
  void *context;
} cai_workspace_discovery_config;
void cai_workspace_discovery_config_init(cai_workspace_discovery_config *);
int cai_workspace_discovery_open(const cai_workspace_discovery_config *,
                                 uint64_t now_ms, cai_workspace_discovery **,
                                 cai_error *);
/** One borrowed socket interest, or fd=-1. A due deadline also schedules local
 * registry work; terminal operations have no descriptor/deadline. Never idle
 * poll or sleep inside the operation. */
int cai_workspace_discovery_prepare(cai_workspace_discovery *, struct pollfd *,
                                    uint64_t *deadline_ms, cai_error *);
/** Advance at most one registry candidate/probe per call. MISSING, AMBIGUOUS
 * and UNAVAILABLE are ordinary terminal outcomes (CAI_OK), not host failures.
 * Failed probes attempt conditional hint pruning under the workspace lock;
 * active/changed owners and socket/lock files are never removed. Directory
 * corruption observations are counted and skipped. */
int cai_workspace_discovery_pump(cai_workspace_discovery *, uint64_t now_ms,
                                 cai_error *);
int cai_workspace_discovery_get_state(cai_workspace_discovery *,
                                      cai_workspace_discovery_state *,
                                      cai_error *);
/** Borrowed until close, non-NULL only for FOUND. Pin record.instance in the
 * subsequent workspace/app client greeting before closing this handle. No
 * attachment, producer, input submission or authority is implied by FOUND. */
const cai_workspace_discovery_result *
cai_workspace_discovery_get_result(const cai_workspace_discovery *);
void cai_workspace_discovery_close(cai_workspace_discovery *);
#ifdef __cplusplus
}
#endif
#endif
