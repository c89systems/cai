/** @file Shared startup identity for executable and native terminal receivers.
 * Hash only configuration metadata; neither discovery nor this projection opens
 * storage, loads agent context or interprets a tool result. */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "workspace_claims.h"
#include "error.h"
#include <limits.h>
#include <locale.h>
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
    inventory_fits_wire[CAI_CLI_OWNER_COUNT <= CAI_WORKSPACE_CLAIM_LIMIT ? 1
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
void cai_cli_workspace_claims_cleanup(cai_cli_workspace_claims *out) {
  size_t i;
  if (out == NULL)
    return;
  for (i = 0U; i < out->count; ++i) {
    void *owned;
    memcpy(&owned, &out->values[i].name, sizeof(owned));
    free(owned);
  }
  free(out->values);
  memset(out, 0, sizeof(*out));
}
static int push(cai_cli_workspace_claims *out, const cai_workspace_claim *claim,
                cai_error *error) {
  size_t n = strlen(claim->name);
  cai_workspace_claim *next;
  char *name;
  if (n == 0U || n > CAI_WORKSPACE_CLAIM_NAME ||
      strspn(claim->name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
                          "0123456789._-") != n)
    return cai_cli_error(
        error, CAI_ERR_INVALID,
        "startup setting name exceeds the supported wire form");
  if (out->count == CAI_WORKSPACE_CLAIM_LIMIT)
    return cai_cli_error(error, CAI_ERR_LIMIT,
                         "too many startup requirements (maximum 512)");
  if (out->count == out->capacity) {
    size_t capacity = out->capacity != 0U ? out->capacity * 2U : 32U;
    if (capacity > CAI_WORKSPACE_CLAIM_LIMIT)
      capacity = CAI_WORKSPACE_CLAIM_LIMIT;
    next = realloc(out->values, capacity * sizeof(*next));
    if (next == NULL)
      return cai_cli_error(error, CAI_ERR_NOMEM, "grow startup requirements");
    out->values = next;
    out->capacity = capacity;
  }
  name = strdup(claim->name);
  if (name == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "copy startup setting name");
  out->values[out->count] = *claim;
  out->values[out->count++].name = name;
  return CAI_OK;
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
    } else {
      int rc = push(out, &claims[i], error);
      if (rc != CAI_OK)
        return rc;
    }
  }
  if (out->count > 1U)
    qsort(out->values, out->count, sizeof(out->values[0]), compare);
  return CAI_OK;
}
/** Fixed options already project resolved identities. Mutable settings travel
 * as acknowledged commands, and rendering preferences belong to each client.
 * Every other accepted schema field is part of the owner's configuration. */
static int supplemental(const char *section, const char *key) {
  char name[CAI_WORKSPACE_CLAIM_NAME + 1U];
  static const char *const local[] = {"paths.directory",
                                      "paths.runtime_dir",
                                      "todo.default_board",
                                      "storage.conversation_retention_days",
                                      "messaging.enable_auto_enqueue",
                                      "agent.model",
                                      "agent.reasoning_effort",
                                      "agent.reasoning_summary",
                                      "agent.service_tier",
                                      "agent.yolo",
                                      "agent.reasoning_tool",
                                      "agent.normal_reasoning_effort",
                                      "agent.high_reasoning_effort",
                                      "terminal.yield_time_ms",
                                      "terminal.default_yield_time_ms",
                                      "terminal.foreground",
                                      "runtime.shell_context_max_lines",
                                      "runtime.shell_context_max_bytes",
                                      "review.theme_offset",
                                      "subagent.theme_offset"};
  size_t i;
  if (strcmp(section, "ui") == 0)
    return 0;
  if (snprintf(name, sizeof(name), "%s%s%s", section,
               *section != '\0' ? "." : "", key) >= (int)sizeof(name))
    return 1; /* push reports a bounded-name error for explicit oversized keys.
               */
  for (i = 0U; i < CAI_CLI_OWNER_COUNT; ++i)
    if (strcmp(name, fields[i].name) == 0)
      return 0;
  for (i = 0U; i < sizeof(local) / sizeof(local[0]); ++i)
    if (strcmp(name, local[i]) == 0)
      return 0;
  return 1;
}
typedef struct projection {
  const cai_config *config;
  cai_cli_workspace_claims *out;
  EVP_MD_CTX *hash;
  locale_t numeric_locale;
} projection;
static int fingerprint(projection *p, const char *section, const char *key,
                       const cai_config_field *field, const char *const *values,
                       size_t count, cai_error *error) {
  char name[CAI_WORKSPACE_CLAIM_NAME + 1U], number[32];
  cai_workspace_claim claim;
  size_t i;
  unsigned int size;
  int ok;
  if (!supplemental(section, key))
    return CAI_OK;
  if (snprintf(name, sizeof(name), "%s%s%s", section,
               *section != '\0' ? "." : "", key) >= (int)sizeof(name))
    return cai_cli_error(error, CAI_ERR_LIMIT,
                         "startup setting name is too long");
  claim.name = name;
  ok = EVP_DigestInit_ex(p->hash, EVP_sha256(), NULL) == 1 &&
       add(p->hash, "cai.workspace.config") && add(p->hash, name);
  for (i = 0U; i < count && ok; ++i) {
    const char *value = values[i];
    if (value != NULL && field != NULL && field->type == CAI_CONFIG_INTEGER) {
      snprintf(number, sizeof(number), "%lld", strtoll(value, NULL, 10));
      value = number;
    } else if (value != NULL && field != NULL &&
               field->type == CAI_CONFIG_REAL) {
      double real = strtod_l(value, NULL, p->numeric_locale);
      uint64_t bits = 0U;
      /* Encode the numeric value independently of the process locale. */
      if (real != 0.0)
        memcpy(&bits, &real, sizeof(bits));
      snprintf(number, sizeof(number), "%016llx", (unsigned long long)bits);
      value = number;
    } else if (field != NULL && field->type == CAI_CONFIG_STRING &&
               value != NULL && *value == '\0')
      value = NULL;
    ok = add(p->hash, value);
    if (ok && value != NULL && strcmp(key, "api_key_env") == 0)
      ok = add(p->hash, cai_config_getenv(p->config, value));
  }
  if (ok)
    ok = EVP_DigestFinal_ex(p->hash, claim.digest, &size) == 1 &&
         size == CAI_WORKSPACE_CLAIM_DIGEST;
  return ok ? push(p->out, &claim, error)
            : cai_cli_error(error, CAI_ERR_INVALID,
                            "cannot fingerprint supplemental configuration");
}
static int explicit_entry(void *context, const char *section, const char *key,
                          const cai_config_field *field,
                          const char *const *values, size_t count,
                          cai_error *error) {
  projection *p = context;
  if (strcmp(section, "env") == 0) {
    const char *effective = cai_config_getenv(p->config, key);
    return fingerprint(p, section, key, NULL, &effective, 1U, error);
  }
  return fingerprint(p, section, key, field, values, count, error);
}
static int schema_field(projection *p, const cai_config_field *f,
                        const char *section, cai_error *error) {
  size_t count;
  const char *fallback = f->default_value;
  const char *const *values =
      cai_config_get_list(p->config, section, f->key, &count);
  if (values == NULL) {
    values = &fallback;
    count = f->type == CAI_CONFIG_STRINGS ? 0U : 1U;
  }
  return fingerprint(p, section, f->key, f, values, count, error);
}
static int supplement(projection *p, int requested, cai_error *error) {
  size_t i, j;
  int rc = CAI_OK;
  cai_environment *environment = NULL;
  if (requested)
    return cai_config_visit(p->config, explicit_entry, p, error);
  for (i = 0U; i < cai_config_field_count(p->config) && rc == CAI_OK; ++i) {
    const cai_config_field *f = cai_config_field_at(p->config, i);
    const char *star = strchr(f->section, '*');
    if (star == NULL)
      rc = schema_field(p, f, f->section, error);
    else {
      for (j = 0U; j < cai_config_section_count(p->config) && rc == CAI_OK;
           ++j) {
        const char *section = cai_config_section_at(p->config, j);
        if (strncmp(section, f->section, (size_t)(star - f->section)) == 0)
          rc = schema_field(p, f, section, error);
      }
    }
  }
  if (rc == CAI_OK)
    rc = cai_config_environment(p->config, &environment, error);
  if (rc == CAI_OK) {
    const char *const *entries = cai_environment_entries(environment);
    for (i = 0U; entries[i] != NULL && rc == CAI_OK; ++i) {
      char key[CAI_WORKSPACE_CLAIM_NAME - 3U];
      const char *eq = strchr(entries[i], '=');
      size_t length = (size_t)(eq - entries[i]);
      const char *value = eq + 1U;
      /* Config [env] names are identifiers. Other inherited shell names are
       * outside that schema and need no claim. Oversized explicit keys fail. */
      if (length == 0U || length >= sizeof(key) ||
          strspn(entries[i], "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVW"
                             "XYZ0123456789_") != length)
        continue;
      memcpy(key, entries[i], length);
      key[length] = '\0';
      rc = fingerprint(p, "env", key, NULL, &value, 1U, error);
    }
  }
  cai_environment_close(environment);
  return rc;
}
int cai_cli_workspace_claims_build(const cai_cli_options *options,
                                   const cai_config *cfg, int requested,
                                   cai_cli_workspace_claims *out,
                                   cai_error *error) {
  EVP_MD_CTX *hash;
  size_t i;
  int ok = 1, rc = CAI_OK;
  projection state;
  if (options == NULL || cfg == NULL || out == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "resolved startup configuration required");
  cai_cli_workspace_claims_cleanup(out);
  hash = EVP_MD_CTX_new();
  if (hash == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate startup fingerprint");
  state.numeric_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
  if (state.numeric_locale == (locale_t)0) {
    EVP_MD_CTX_free(hash);
    return cai_cli_error(error, CAI_ERR_NOMEM,
                         "allocate numeric fingerprint locale");
  }
  for (i = 0U; ok && i < CAI_CLI_OWNER_COUNT; ++i) {
    const claim_field *f = &fields[i];
    const unsigned char *p = (const unsigned char *)options + f->offset;
    const char *value;
    cai_workspace_claim claim;
    unsigned int length;
    char canonical[PATH_MAX], number[32];
    if (requested && !options->owner_explicit[i])
      continue;
    claim.name = f->name;
    if (f->type == 's')
      memcpy(&value, p, sizeof(value));
    else if (f->type == 'l') {
      long integer;
      memcpy(&integer, p, sizeof(integer));
      snprintf(number, sizeof(number), "%ld", integer);
      value = number;
    } else if (f->type == 'z') {
      size_t integer;
      memcpy(&integer, p, sizeof(integer));
      snprintf(number, sizeof(number), "%llu", (unsigned long long)integer);
      value = number;
    } else {
      int flag;
      memcpy(&flag, p, sizeof(flag));
      value = flag ? "true" : "false";
    }
    /* Non-custom providers accept these through the core INI schema, while
     * CLI endpoint/key-variable flags remain custom-provider-only. */
    if (value == NULL && i == CAI_CLI_OWNER_endpoint)
      value = cai_config_get(cfg, "client", "base_url");
    if (value == NULL && i == CAI_CLI_OWNER_api_key_env)
      value = cai_config_get(cfg, "client", "api_key_env");
    if ((i == CAI_CLI_OWNER_endpoint || i == CAI_CLI_OWNER_api_key_env) &&
        value != NULL && *value == '\0')
      value = NULL;
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
    if (ok && value != NULL && i == CAI_CLI_OWNER_api_key_env)
      ok = add(hash, cai_config_getenv(cfg, value));
    /* The same -z file targets a different initial runtime with --review.
     * Never silently reuse an owner's override for the other role. */
    if (ok && value != NULL &&
        i == CAI_CLI_OWNER_developer_instructions_override_file)
      ok = add(hash, options->review ? "review" : "agent");
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
      ok = EVP_DigestFinal_ex(hash, claim.digest, &length) == 1 &&
           length == CAI_WORKSPACE_CLAIM_DIGEST;
    if (ok)
      rc = push(out, &claim, error);
    if (rc != CAI_OK)
      break;
  }
  state.config = cfg;
  state.out = out;
  state.hash = hash;
  if (ok && rc == CAI_OK)
    rc = supplement(&state, requested, error);
  EVP_MD_CTX_free(hash);
  freelocale(state.numeric_locale);
  if (!ok || rc != CAI_OK) {
    cai_cli_workspace_claims_cleanup(out);
    if (rc != CAI_OK)
      return rc;
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "cannot fingerprint resolved startup settings");
  }
  if (out->count > 1U)
    qsort(out->values, out->count, sizeof(out->values[0]), compare);
  return CAI_OK;
}
