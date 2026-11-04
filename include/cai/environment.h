/** @file cai/environment.h
 * Immutable, shared environment views for independent embedded controllers.
 */
#ifndef CAI_ENVIRONMENT_H
#define CAI_ENVIRONMENT_H
#include <cai/cai.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct cai_environment_change {
  const char *name;
  /** NULL removes the name; an empty string preserves an empty value. */
  const char *value;
} cai_environment_change;
/** Copy a NULL-terminated NAME=value vector. NULL captures the process
 * environment; {NULL} explicitly selects an empty environment. Duplicate names
 * retain their first value, matching getenv. Names must be nonempty and contain
 * no '='. Values may contain '=' or newlines. No expansion/interpolation.
 * Capturing environ requires the host to serialize concurrent process-global
 * environment mutation during this call; explicit vectors need no such lock.
 */
int cai_environment_open(const char *const *entries, cai_environment **out,
                         cai_error *error);
/** Derive a separate immutable view. The last change for a name wins. Inputs
 * are copied; the base is unchanged. Zero changes shares a retained reference.
 * No process environment is consulted or modified. */
int cai_environment_derive(cai_environment *base,
                           const cai_environment_change *changes, size_t count,
                           cai_environment **out, cai_error *error);
/** Borrowed, stable until the caller releases its reference. Missing returns
 * NULL, distinct from an explicitly empty value. Concurrent reads are safe. */
const char *cai_environment_get(const cai_environment *environment,
                                const char *name);
size_t cai_environment_count(const cai_environment *environment);
/** Borrow the sorted, NULL-terminated immutable NAME=value vector. */
const char *const *cai_environment_entries(const cai_environment *environment);
/** Borrow the client's captured environment until client close. Retain it to
 * share with a child/controller that can outlive the client. */
cai_environment *cai_client_environment(const cai_client *client);
/** References may be retained/released from different threads. Do not race
 * the final release with a caller that does not already own a reference. */
int cai_environment_retain(cai_environment *environment, cai_error *error);
void cai_environment_close(cai_environment *environment);
#ifdef __cplusplus
}
#endif
#endif
