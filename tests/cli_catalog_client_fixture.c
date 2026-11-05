#define _POSIX_C_SOURCE 200809L
#include "../src/cai_internal.h"

int __real_cai_client_open(const cai_client_config *, cai_client **,
                           cai_error *);
int __wrap_cai_client_open(const cai_client_config *, cai_client **,
                           cai_error *);

/** Enable the catalog parser on the fixture's loopback endpoint. All process,
 * native owner, socket, encrypted journal and frontend code stays production.
 */
int __wrap_cai_client_open(const cai_client_config *config, cai_client **out,
                           cai_error *error) {
  int rc = __real_cai_client_open(config, out, error);
  if (rc == CAI_OK)
    CAI_CLIENT_IMPL(*out)->catalog_policy_origin = CAI_CHATGPT_CODEX_BASE_URL;
  return rc;
}
