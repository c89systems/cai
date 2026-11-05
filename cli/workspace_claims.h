#ifndef CAI_CLI_WORKSPACE_CLAIMS_H
#define CAI_CLI_WORKSPACE_CLAIMS_H
#include "options.h"
#include <cai/config.h>
#include <cai/workspace_claims.h>
typedef struct cai_cli_workspace_claims {
  cai_workspace_claim *values;
  size_t count, capacity;
} cai_cli_workspace_claims;
/** Build bounded named fingerprints from resolved startup identity. requested
 * selects only explicit settings; false builds the owner's complete inventory.
 * Initialize out to zero. A repeated build replaces its owned prior inventory;
 * failure leaves it empty. No SDK client, key file or directory is created;
 * cfg remains borrowed. */
int cai_cli_workspace_claims_build(const cai_cli_options *, const cai_config *,
                                   int requested, cai_cli_workspace_claims *,
                                   cai_error *);
/** Copy additional requirements, rejecting conflicts. Failure may retain a
 * partial merge; callers discard it rather than starting an attachment. */
int cai_cli_workspace_claims_merge(cai_cli_workspace_claims *,
                                   const cai_workspace_claim *, size_t,
                                   cai_error *);
/** Initialize to zero before build/merge; names and arrays are owned. */
void cai_cli_workspace_claims_cleanup(cai_cli_workspace_claims *);
#endif
