/** @file cai/workspace_claims.h
 * Application-defined startup requirements, checked before attachment.
 */
#ifndef CAI_WORKSPACE_CLAIMS_H
#define CAI_WORKSPACE_CLAIMS_H
#include <stddef.h>
#define CAI_WORKSPACE_CLAIM_LIMIT 512U
#define CAI_WORKSPACE_CLAIM_NAME 80U
#define CAI_WORKSPACE_CLAIM_DIGEST 32U
/** Named opaque fingerprint; the application defines its meaning and hashing.
 * Names are nonempty ASCII letters/digits, dots, underscores or hyphens, up to
 * NAME bytes. Arrays must be strictly sorted by name, without duplicates.
 * Hosts and clients copy these bounded metadata at open. No values/credentials
 * are published in discovery. A missing or unequal requested claim rejects
 * HELLO before acquiring authority or affecting attachment lifetime. */
typedef struct cai_workspace_claim {
  const char *name;
  unsigned char digest[CAI_WORKSPACE_CLAIM_DIGEST];
} cai_workspace_claim;
#endif
