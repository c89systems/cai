/** @file Shared startup identity for executable and native terminal receivers.
 * Hash only configuration metadata; neither discovery nor this projection opens
 * storage, loads agent context or interprets a tool result. */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "workspace_claims.h"
#include "error.h"
#include <limits.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct claim_field {
  const char *name;
  size_t offset;
  char type;
} claim_field;
static const claim_field fields[] = {
#define CAI_CLI_OWNER_FIELD(section, key, member, type)                        \
  {section "." key, offsetof(cai_cli_options, member), type},
    CAI_CLI_OWNER_FIELDS(CAI_CLI_OWNER_FIELD)
#undef CAI_CLI_OWNER_FIELD
};
typedef char
    inventory_fits_wire[CAI_CLI_OWNER_COUNT <= CAI_WORKSPACE_CLAIM_LIMIT &&
                                CAI_CLI_OWNER_COUNT <= 64
                            ? 1
                            : -1];
static int add(EVP_MD_CTX *hash, const char *text) {
  unsigned char present = text != NULL;
  return EVP_DigestUpdate(hash, &present, 1U) == 1 &&
         (text == NULL || EVP_DigestUpdate(hash, text, strlen(text) + 1U) == 1);
}
static int compare(const void *a, const void *b) {
  return strcmp(((const cai_workspace_claim *)a)->name,
                ((const cai_workspace_claim *)b)->name);
}
int cai_cli_workspace_claims_merge(cai_cli_workspace_claims *out,
                                   const cai_workspace_claim *claims, size_t n,
                                   cai_error *error) {
  size_t i, j;
  if (n > CAI_WORKSPACE_CLAIM_LIMIT || (n != 0U && claims == NULL))
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "invalid startup requirements");
  for (i = 0U; i < n; ++i) {
    if (claims[i].name == NULL ||
        strnlen(claims[i].name, CAI_WORKSPACE_CLAIM_NAME + 1U) >
            CAI_WORKSPACE_CLAIM_NAME)
      return cai_cli_error(error, CAI_ERR_INVALID, "invalid requirement name");
    for (j = 0U; j < out->count; ++j)
      if (strcmp(claims[i].name, out->values[j].name) == 0)
        break;
    if (j != out->count) {
      if (memcmp(claims[i].digest, out->values[j].digest,
                 CAI_WORKSPACE_CLAIM_DIGEST) != 0)
        return cai_cli_error(error, CAI_ERR_INVALID,
                             "conflicting terminal startup requirements");
    } else if (out->count == CAI_WORKSPACE_CLAIM_LIMIT)
      return cai_cli_error(error, CAI_ERR_LIMIT,
                           "too many startup requirements");
    else
      out->values[out->count++] = claims[i];
  }
  qsort(out->values, out->count, sizeof(out->values[0]), compare);
  return CAI_OK;
}
int cai_cli_workspace_claims_build(const cai_cli_options *options,
                                   const cai_config *cfg, int requested,
                                   cai_cli_workspace_claims *out,
                                   cai_error *error) {
  EVP_MD_CTX *hash;
  size_t i;
  int ok = 1;
  if (options == NULL || cfg == NULL || out == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "resolved startup configuration required");
  memset(out, 0, sizeof(*out));
  hash = EVP_MD_CTX_new();
  if (hash == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate startup fingerprint");
  for (i = 0U; ok && i < CAI_CLI_OWNER_COUNT; ++i) {
    const claim_field *f = &fields[i];
    const unsigned char *p = (const unsigned char *)options + f->offset;
    const char *value;
    cai_workspace_claim *claim;
    unsigned int length;
    char canonical[PATH_MAX];
    if (requested && !(options->owner_explicit & (1ULL << i)))
      continue;
    claim = &out->values[out->count];
    claim->name = f->name;
    if (f->type == 's')
      memcpy(&value, p, sizeof(value));
    else {
      int flag;
      memcpy(&flag, p, sizeof(flag));
      value = flag ? "true" : "false";
    }
    if (i == CAI_CLI_OWNER_config_file) {
      if (options->no_config)
        value = NULL;
      else {
        value = cai_config_origin(cfg);
        if (value == NULL) {
          ok = 0;
          break;
        }
      }
      if (value != NULL && value[0] == '/' &&
          realpath(value, canonical) != NULL)
        value = canonical;
    }
    ok = EVP_DigestInit_ex(hash, EVP_sha256(), NULL) == 1 &&
         add(hash, "cai.workspace.startup") && add(hash, f->name) &&
         add(hash, value);
    /* A selector alone is insufficient: identically named stores can refer to
     * different endpoints or keys in independently selected configurations. */
    if (ok && value != NULL &&
        (i == CAI_CLI_OWNER_sessions_store || i == CAI_CLI_OWNER_auth_store ||
         i == CAI_CLI_OWNER_messaging_store)) {
      char section[80];
      if (snprintf(section, sizeof(section), "store.%s", value) >=
          (int)sizeof(section))
        ok = 0;
      else {
        ok = add(hash, cfg->get(cfg, section, "endpoint")) &&
             add(hash, cfg->get(cfg, section, "key_file")) &&
             add(hash, cfg->get(cfg, section, "client_pem"));
        if (ok && strcmp(value, "default") == 0)
          ok = add(hash, options->lockd) && add(hash, options->state_dir) &&
               add(hash, options->pouch_key_file) &&
               add(hash, options->lockd_client_pem);
      }
    }
    if (ok)
      ok = EVP_DigestFinal_ex(hash, claim->digest, &length) == 1 &&
           length == CAI_WORKSPACE_CLAIM_DIGEST;
    if (ok)
      ++out->count;
  }
  EVP_MD_CTX_free(hash);
  if (!ok) {
    memset(out, 0, sizeof(*out));
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "cannot fingerprint resolved startup settings");
  }
  qsort(out->values, out->count, sizeof(out->values[0]), compare);
  return CAI_OK;
}
