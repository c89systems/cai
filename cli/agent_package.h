/** @file Stored-agent source directories and streamed object publication. */
#ifndef CAI_CLI_AGENT_PACKAGE_H
#define CAI_CLI_AGENT_PACKAGE_H
#include "agents.h"
#include "pouch.h"

/** Normalize a source/display name once; canonical names fit storage keys. */
int cai_cli_agent_name(const char *, char out[65], cai_error *);
/** Initialize the complete default definition once, without a disk config
 * staging file. Resources are captured only by the winning initializer. */
int cai_cli_agent_package_initialize(cai_cli_pouch *, cai_cli_producer, void *,
                                     cai_config *resources, cai_error *);
/** Host-owned package utilities; no inference or workspace owner is started. */
int cai_cli_agent_package_run(const cai_cli_options *, cai_config *,
                              const cai_app_config *,
                              const cai_config_context *, int stop_fd,
                              int (*stop)(void *), void *, cai_error *);
/** A native-auth receiver bound to one agent object. It borrows the store
 * for the entire provider-auth lifetime; refresh updates only authentication.
 */
typedef struct cai_cli_agent_auth {
  cai_cli_pouch *pouch;
  char name[65];
  cai_blob_store storage;
} cai_cli_agent_auth;
/** Initialize the receiver; its load callback returns NULL for absent auth. */
int cai_cli_agent_auth_init(cai_cli_agent_auth *, cai_cli_pouch *,
                            const char *name, cai_error *);
/** Materialize a committed definition into a revision-specific cache and
 * return its config source. The caller owns the returned cache directory. */
int cai_cli_agent_package_load(cai_cli_pouch *, const char *name,
                               const char *cache, cai_source **config,
                               char **directory, char revision[21],
                               cai_error *);
/** Bind bundled instruction/resource paths after the config source is loaded.
 */
int cai_cli_agent_package_configure(cai_config *, const char *directory,
                                    cai_error *);
#endif
