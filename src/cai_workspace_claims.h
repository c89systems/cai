#ifndef CAI_WORKSPACE_CLAIMS_PRIVATE_H
#define CAI_WORKSPACE_CLAIMS_PRIVATE_H
#include <cai/wire.h>
#include <cai/workspace_claims.h>
typedef struct cai_workspace_claim_reader {
  const cai_workspace_claim *claims, *expected;
  size_t count, remaining, offset, length;
  int phase;
  char name[CAI_WORKSPACE_CLAIM_NAME + 1U];
  char previous[CAI_WORKSPACE_CLAIM_NAME + 1U];
  char mismatch[CAI_WORKSPACE_CLAIM_NAME + 1U];
} cai_workspace_claim_reader;
int cai_workspace_claim_name(const char *, size_t);
int cai_workspace_claim_copy(const cai_workspace_claim *, size_t,
                             cai_workspace_claim **, cai_error *);
void cai_workspace_claim_free(cai_workspace_claim *, size_t);
uint32_t cai_workspace_claim_size(const cai_workspace_claim *, size_t);
int cai_workspace_claim_write(cai_wire_peer *, const cai_workspace_claim *,
                              size_t, cai_error *);
/** Consume one nested claims value; 1=complete, 0=more, negative=invalid.
 * Only comparisons are provisional. Admission/rejection waits for COMPLETE. */
int cai_workspace_claim_read(cai_workspace_claim_reader *,
                             const cai_cbor_token *);
#endif
