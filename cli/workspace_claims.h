#ifndef CAI_CLI_WORKSPACE_CLAIMS_H
#define CAI_CLI_WORKSPACE_CLAIMS_H
#include "options.h"
#include <cai/config.h>
#include <cai/workspace_claims.h>
typedef struct cai_cli_workspace_claims {
  cai_workspace_claim values[CAI_WORKSPACE_CLAIM_LIMIT];
  size_t count;
} cai_cli_workspace_claims;
/** Build bounded named fingerprints from resolved startup identity. requested
 * selects only explicit settings; false builds the owner's complete inventory.
 * No SDK client, key file or directory is created; cfg remains borrowed. */
int cai_cli_workspace_claims_build(const cai_cli_options *, const cai_config *,
                                   int requested, cai_cli_workspace_claims *,
                                   cai_error *);
int cai_cli_workspace_claims_merge(cai_cli_workspace_claims *,
                                   const cai_workspace_claim *, size_t,
                                   cai_error *);
#endif
