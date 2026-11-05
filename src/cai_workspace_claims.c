/** @file Incremental comparison of bounded application startup metadata.
 * Values are opaque fingerprints; this transport layer has no config policy.
 */
#define _POSIX_C_SOURCE 200809L
#include "cai_workspace_claims.h"
#include "cai_internal.h"
#include <stdlib.h>
#include <string.h>
int cai_workspace_claim_name(const char *name, size_t n) {
  size_t i;
  if (name == NULL || n == 0U || n > CAI_WORKSPACE_CLAIM_NAME)
    return 0;
  for (i = 0U; i < n; ++i) {
    unsigned char c = (unsigned char)name[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
      return 0;
  }
  return 1;
}
void cai_workspace_claim_free(cai_workspace_claim *claims, size_t n) {
  size_t i;
  for (i = 0U; claims != NULL && i < n; ++i) {
    void *owned;
    memcpy(&owned, &claims[i].name, sizeof(owned));
    free(owned);
  }
  free(claims);
}
int cai_workspace_claim_copy(const cai_workspace_claim *in, size_t n,
                             cai_workspace_claim **out, cai_error *error) {
  cai_workspace_claim *copy;
  size_t i;
  *out = NULL;
  if (n > CAI_WORKSPACE_CLAIM_LIMIT || (n != 0U && in == NULL))
    return cai_set_error(error, CAI_ERR_INVALID, "invalid workspace claims");
  if (n == 0U)
    return CAI_OK;
  for (i = 0U; i < n; ++i)
    if (in[i].name == NULL ||
        !cai_workspace_claim_name(
            in[i].name, strnlen(in[i].name, CAI_WORKSPACE_CLAIM_NAME + 1U)) ||
        (i != 0U && strcmp(in[i - 1U].name, in[i].name) >= 0))
      return cai_set_error(error, CAI_ERR_INVALID,
                           "workspace claims must have sorted unique names");
  copy = calloc(n, sizeof(*copy));
  if (copy == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "allocate workspace claims");
  for (i = 0U; i < n; ++i) {
    copy[i] = in[i];
    copy[i].name = strdup(in[i].name);
    if (copy[i].name == NULL) {
      cai_workspace_claim_free(copy, i);
      return cai_set_error(error, CAI_ERR_NOMEM, "copy workspace claim name");
    }
  }
  *out = copy;
  return CAI_OK;
}
/** Lengths come from validated bounded fields, never a sizing serialization. */
uint32_t cai_workspace_claim_size(const cai_workspace_claim *claims, size_t n) {
  uint32_t size = n < 24U ? 1U : n <= 255U ? 2U : 3U;
  size_t i;
  for (i = 0U; i < n; ++i) {
    size_t length = strlen(claims[i].name);
    size += 1U + (length < 24U ? 1U : 2U) + (uint32_t)length + 34U;
  }
  return size;
}
static int token(cai_wire_peer *peer, cai_cbor_type type, uint64_t value,
                 const void *data, size_t n, cai_error *error) {
  cai_cbor_token t;
  memset(&t, 0, sizeof(t));
  t.type = type;
  t.argument = value;
  t.data = data;
  t.length = n;
  return cai_wire_peer_write(peer, &t, error);
}
static int bytes(cai_wire_peer *peer, cai_cbor_type type, const void *p,
                 size_t n, cai_error *error) {
  int rc = token(peer, type, n, NULL, 0U, error);
  if (rc == CAI_OK)
    rc = token(peer, CAI_CBOR_DATA, 0U, p, n, error);
  if (rc == CAI_OK)
    rc = token(peer, CAI_CBOR_END, type, NULL, 0U, error);
  return rc;
}
int cai_workspace_claim_write(cai_wire_peer *peer,
                              const cai_workspace_claim *claims, size_t n,
                              cai_error *error) {
  size_t i;
  int rc = token(peer, CAI_CBOR_ARRAY, n, NULL, 0U, error);
  for (i = 0U; i < n && rc == CAI_OK; ++i) {
    rc = token(peer, CAI_CBOR_ARRAY, 2U, NULL, 0U, error);
    if (rc == CAI_OK)
      rc = bytes(peer, CAI_CBOR_TEXT, claims[i].name, strlen(claims[i].name),
                 error);
    if (rc == CAI_OK)
      rc = bytes(peer, CAI_CBOR_BYTES, claims[i].digest,
                 CAI_WORKSPACE_CLAIM_DIGEST, error);
    if (rc == CAI_OK)
      rc = token(peer, CAI_CBOR_END, CAI_CBOR_ARRAY, NULL, 0U, error);
  }
  return rc == CAI_OK
             ? token(peer, CAI_CBOR_END, CAI_CBOR_ARRAY, NULL, 0U, error)
             : rc;
}
int cai_workspace_claim_read(cai_workspace_claim_reader *r,
                             const cai_cbor_token *t) {
  size_t i;
  if (t == NULL || t->indefinite)
    return -1;
  switch (r->phase) {
  case 0:
    if (t->type != CAI_CBOR_ARRAY || t->argument > CAI_WORKSPACE_CLAIM_LIMIT)
      return -1;
    r->remaining = (size_t)t->argument;
    r->phase = r->remaining ? 1 : 8;
    break;
  case 1:
    if (t->type != CAI_CBOR_ARRAY || t->argument != 2U)
      return -1;
    r->phase = 2;
    break;
  case 2:
    if (t->type != CAI_CBOR_TEXT || t->argument == 0U ||
        t->argument > CAI_WORKSPACE_CLAIM_NAME)
      return -1;
    r->length = (size_t)t->argument;
    r->offset = 0U;
    r->phase = 3;
    break;
  case 3:
    if (t->type == CAI_CBOR_DATA) {
      if (t->length > r->length - r->offset ||
          (t->length && !cai_workspace_claim_name(t->data, t->length)))
        return -1;
      if (t->length)
        memcpy(r->name + r->offset, t->data, t->length);
      r->offset += t->length;
    } else if (t->type == CAI_CBOR_END && t->argument == CAI_CBOR_TEXT &&
               r->offset == r->length) {
      r->name[r->length] = '\0';
      if (strcmp(r->previous, r->name) >= 0)
        return -1;
      strcpy(r->previous, r->name);
      r->expected = NULL;
      for (i = 0U; i < r->count; ++i)
        if (strcmp(r->name, r->claims[i].name) == 0)
          r->expected = &r->claims[i];
      if (r->expected == NULL && !r->mismatch[0])
        strcpy(r->mismatch, r->name);
      r->phase = 4;
    } else
      return -1;
    break;
  case 4:
    if (t->type != CAI_CBOR_BYTES || t->argument != CAI_WORKSPACE_CLAIM_DIGEST)
      return -1;
    r->offset = 0U;
    r->phase = 5;
    break;
  case 5:
    if (t->type == CAI_CBOR_DATA) {
      if (t->length > CAI_WORKSPACE_CLAIM_DIGEST - r->offset)
        return -1;
      if (t->length && r->expected != NULL && !r->mismatch[0] &&
          memcmp(r->expected->digest + r->offset, t->data, t->length) != 0)
        strcpy(r->mismatch, r->name);
      r->offset += t->length;
    } else if (t->type == CAI_CBOR_END && t->argument == CAI_CBOR_BYTES &&
               r->offset == CAI_WORKSPACE_CLAIM_DIGEST)
      r->phase = 6;
    else
      return -1;
    break;
  case 6:
    if (t->type != CAI_CBOR_END || t->argument != CAI_CBOR_ARRAY)
      return -1;
    r->phase = --r->remaining ? 1 : 8;
    break;
  case 8:
    if (t->type != CAI_CBOR_END || t->argument != CAI_CBOR_ARRAY)
      return -1;
    r->phase = 9;
    return 1;
  default:
    return -1;
  }
  return 0;
}
