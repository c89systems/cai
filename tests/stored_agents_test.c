/* Real encrypted repository, ordinary state leases and default worker stacks.
 * The storage fixture removes fsync waits only; production CLI e2e retains
 * them. */
#define _POSIX_C_SOURCE 200809L
#include "../cli/agent_package.h"
#include "../cli/agents.h"
#include "../cli/config.h"
#include "../cli/pouch.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

unsigned int cai_test_storage_open_count(void);
typedef struct producer_input {
  const char *text;
  int calls, fail;
} producer_input;
typedef struct initializer {
  cai_cli_pouch *pouch;
  producer_input *input;
  char bytes[256];
  int rc;
} initializer;
typedef struct fixture {
  char root[4096];
  cai_cli_options options;
  cai_config *first, *second;
  cai_config_context startup;
  cai_cli_pouch session, left, right;
} fixture;
static pthread_mutex_t producer_lock = PTHREAD_MUTEX_INITIALIZER;
static int read_acquires;
static int (*real_acquire)(lc_client *, const lc_acquire_req *, lc_lease **,
                           lc_error *);
static int count_acquire(lc_client *client, const lc_acquire_req *req,
                         lc_lease **out, lc_error *error) {
  ++read_acquires;
  return real_acquire(client, req, out, error);
}

static int produce(void *context, cai_sink *sink, cai_error *error) {
  producer_input *input = context;
  int rc;
  pthread_mutex_lock(&producer_lock);
  ++input->calls;
  pthread_mutex_unlock(&producer_lock);
  rc = cai_sink_write(sink, input->text, strlen(input->text), error);
  return rc == CAI_OK && input->fail
             ? cai_cli_error(error, CAI_ERR_TRANSPORT,
                             "injected agent upload failure")
             : rc;
}

static int default_agent(cai_cli_pouch *pouch, cai_cli_producer writer,
                         void *context, cai_source **out, cai_error *error) {
  int rc =
      cai_cli_agent_package_initialize(pouch, writer, context, NULL, error);
  *out = NULL;
  return rc == CAI_OK ? cai_cli_pouch_agent_read(pouch, "default", "config.ini",
                                                 out, error)
                      : rc;
}

static void *initialize(void *context) {
  initializer *i = context;
  cai_source *source = NULL;
  cai_error error;
  size_t used = 0U, n;
  cai_error_init(&error);
  i->rc = default_agent(i->pouch, produce, i->input, &source, &error);
  while (i->rc == CAI_OK &&
         (n = cai_source_read(source, i->bytes + used,
                              sizeof(i->bytes) - used - 1U, &error)) != 0U) {
    used += n;
    assert(used < sizeof(i->bytes) - 1U);
  }
  if (i->rc == CAI_OK)
    i->rc = error.code;
  i->bytes[used] = '\0';
  cai_source_close(source);
  if (i->rc != CAI_OK)
    fprintf(stderr, "initialize agent: %d: %s\n", i->rc,
            error.message != NULL ? error.message : "");
  cai_error_cleanup(&error);
  return NULL;
}

typedef struct auth_input {
  const char *text;
  size_t offset;
  int fail;
} auth_input;
static size_t auth_read(void *context, void *buffer, size_t count,
                        cai_error *error) {
  auth_input *input = context;
  size_t left = strlen(input->text) - input->offset;
  if (input->fail && input->offset != 0U) {
    cai_cli_error(error, CAI_ERR_TRANSPORT,
                  "injected credential source failure");
    return 0U;
  }
  if (count > left)
    count = left;
  if (input->fail && count > 8U)
    count = 8U;
  memcpy(buffer, input->text + input->offset, count);
  input->offset += count;
  return count;
}
static void check_auth(cai_cli_pouch *pouch, cai_error *error) {
  static const char saved[] =
      "{\"tokens\":{\"refresh_token\":\"native-secret\"}}";
  cai_cli_agent_auth auth;
  cai_source_callbacks callbacks = {0};
  cai_source *source = NULL;
  auth_input input;
  char bytes[256];
  size_t used = 0U, n;
  assert(cai_cli_agent_auth_init(&auth, pouch, "default", error) == CAI_OK);
  assert(auth.storage.load(&auth, "auth.json", &source, error) == CAI_OK);
  assert(source == NULL);
  input.text = saved;
  input.offset = 0U;
  input.fail = 0;
  callbacks.context = &input;
  callbacks.read = auth_read;
  assert(cai_source_from_callbacks(&callbacks, &source, error) == CAI_OK);
  assert(auth.storage.replace(&auth, "auth.json", source, error) == CAI_OK);
  cai_source_close(source);
  source = NULL;
  input.offset = 0U;
  input.fail = 1;
  assert(cai_source_from_callbacks(&callbacks, &source, error) == CAI_OK);
  assert(auth.storage.replace(&auth, "auth.json", source, error) != CAI_OK);
  cai_source_close(source);
  cai_error_cleanup(error);
  cai_error_init(error);
  source = NULL;
  assert(auth.storage.load(&auth, "auth.json", &source, error) == CAI_OK);
  assert(source != NULL);
  while ((n = cai_source_read(source, bytes + used, sizeof(bytes) - used - 1U,
                              error)) != 0U) {
    used += n;
    assert(used < sizeof(bytes) - 1U);
  }
  bytes[used] = '\0';
  assert(error->code == CAI_OK && strcmp(bytes, saved) == 0);
  cai_source_close(source);
}

static void *run(void *context) {
  fixture *f = context;
  char *args[9];
  cai_error error;
  cai_config *bootstrap = NULL, *quoted = NULL;
  cai_config *environment_agent = NULL;
  cai_app_config application;
  static const cai_app_setting allowed[] = {
      {"review", "allowed_models", "example"}};
  size_t allowed_count;
  cai_source *source = NULL;
  cai_cli_options bootstrap_options;
  producer_input input;
  initializer a, b;
  pthread_t one, two;
  char *large;
  size_t size = 96U * 1024U;
  int rc;
  static const char *const empty_environment[] = {NULL};
  cai_error_init(&error);
  cai_app_config_init(&application);
  application.settings = allowed;
  application.setting_count = 1U;
  f->startup.directory = f->root;
  assert(cai_environment_open(empty_environment, &f->startup.environment,
                              &error) == CAI_OK);
  args[0] = strdup("cai");
  args[1] = strdup("--no-config");
  args[2] = strdup("--home");
  args[3] = f->root;
  args[4] = strdup("-C");
  args[5] = f->root;
  args[6] = NULL;
  rc = cai_cli_configure_agent_at(6, args, &f->options, &f->first, &application,
                                  &f->startup, &error);
  if (rc != 1)
    fprintf(stderr, "agent configure: %s\n", error.message);
  assert(rc == 1 && cai_test_storage_open_count() == 1U);
  assert(cai_config_get_list(f->first, "review", "allowed_models",
                             &allowed_count) != NULL);
  assert(allowed_count == 1U);
  assert(cai_config_set(f->first, "agent", "developer_instructions", "seed",
                        &error) == CAI_OK);
  assert(cai_config_set(f->first, "env", "AGENT_ONLY", "seed", &error) ==
         CAI_OK);
  assert(cai_cli_agent_bootstrap_configuration(f->first, &f->startup,
                                               &f->second, &error) == CAI_OK);
  assert(cai_test_storage_open_count() == 1U);
  assert(cai_config_get(f->second, "agent", "developer_instructions") == NULL);
  assert(cai_config_getenv(f->second, "AGENT_ONLY") == NULL);
  assert(strcmp(cai_config_get(f->second, "paths", "state_dir"),
                cai_config_get(f->first, "paths", "state_dir")) == 0);
  cai_config_close(f->second);
  f->second = NULL;
  assert(cai_cli_pouch_open_options(&f->session, &f->options, NULL, &error) ==
         CAI_OK);
  real_acquire = f->session.sessions->acquire;
  f->session.sessions->acquire = count_acquire;
  assert(cai_cli_configure_agent_at(6, args, &bootstrap_options, &f->second,
                                    NULL, &f->startup, &error) == 1);
  assert(read_acquires == 0); /* Existing definitions use public reads only. */
  assert(cai_config_get_list(f->second, "review", "allowed_models",
                             &allowed_count) == NULL &&
         allowed_count == 0U);
  f->session.sessions->acquire = real_acquire;
  assert(cai_test_storage_open_count() == 1U);
  cai_config_close(f->second);
  f->second = NULL;
  assert(cai_test_storage_open_count() ==
         1U); /* Session still owns the shared client. */

  /* Two default creators race on a fresh namespace, sharing only the client. */
  bootstrap_options = f->options;
  bootstrap_options.namespace_prefix = "race";
  assert(cai_cli_pouch_open_options(&f->left, &bootstrap_options, NULL,
                                    &error) == CAI_OK);
  assert(cai_cli_pouch_open_options(&f->right, &bootstrap_options, NULL,
                                    &error) == CAI_OK);
  memset(&input, 0, sizeof(input));
  input.text = "[agent]\nmodel = \"winner\"\n";
  memset(&a, 0, sizeof(a));
  memset(&b, 0, sizeof(b));
  a.pouch = &f->left;
  b.pouch = &f->right;
  a.input = b.input = &input;
  assert(pthread_create(&one, NULL, initialize, &a) == 0);
  assert(pthread_create(&two, NULL, initialize, &b) == 0);
  assert(pthread_join(one, NULL) == 0 && pthread_join(two, NULL) == 0);
  if (a.rc != CAI_OK || b.rc != CAI_OK || input.calls != 1)
    fprintf(stderr, "initializers: %d / %d; producer calls: %d\n", a.rc, b.rc,
            input.calls);
  assert(a.rc == CAI_OK && b.rc == CAI_OK && input.calls == 1);
  assert(strcmp(a.bytes, input.text) == 0 && strcmp(a.bytes, b.bytes) == 0);
  check_auth(&f->left, &error);
  /* Credential refresh preserves the definition and its configuration bytes. */
  assert(default_agent(&f->left, produce, &input, &source, &error) == CAI_OK);
  {
    char bytes[128];
    size_t n = cai_source_read(source, bytes, sizeof(bytes) - 1U, &error);
    bytes[n] = '\0';
    assert(strcmp(bytes, input.text) == 0);
  }
  cai_source_close(source);
  source = NULL;
  cai_cli_pouch_close(&f->left);
  cai_cli_pouch_close(&f->right);

  /* A failed streamed attachment must not publish a half-created default. */
  bootstrap_options.namespace_prefix = "failed";
  assert(cai_cli_pouch_open_options(&f->left, &bootstrap_options, NULL,
                                    &error) == CAI_OK);
  input.calls = 0;
  input.fail = 1;
  assert(default_agent(&f->left, produce, &input, &source, &error) != CAI_OK);
  assert(source == NULL);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  input.fail = 0;
  assert(default_agent(&f->left, produce, &input, &source, &error) == CAI_OK);
  cai_source_close(source);
  source = NULL;
  assert(input.calls == 2);
  cai_cli_pouch_close(&f->left);

  /* Bootstrap settings found in an existing stored attachment fail closed. */
  bootstrap_options.namespace_prefix = "bad";
  assert(cai_cli_pouch_open_options(&f->left, &bootstrap_options, NULL,
                                    &error) == CAI_OK);
  input.text = "[storage]\nsessions_store = \"elsewhere\"\n";
  assert(default_agent(&f->left, produce, &input, &source, &error) == CAI_OK);
  cai_source_close(source);
  source = NULL;
  assert(cai_cli_configure_at(6, args, &bootstrap_options, &bootstrap, NULL,
                              &f->startup, &error) == 1);
  bootstrap_options.namespace_prefix = "bad";
  assert(cai_cli_agent_configuration(bootstrap, bootstrap, &bootstrap_options,
                                     NULL, &f->startup, &quoted,
                                     &error) == CAI_ERR_INVALID);
  assert(quoted == NULL && strstr(error.message, "bootstrap settings") != NULL);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  cai_cli_pouch_close(&f->left);

  /* A command environment must never be reapplied as bootstrap selectors. */
  bootstrap_options = f->options;
  bootstrap_options.namespace_prefix = "environment";
  assert(cai_cli_pouch_open_options(&f->left, &bootstrap_options, NULL,
                                    &error) == CAI_OK);
  input.text = "[env]\nCAI_STORAGE_SESSIONS_STORE = \"elsewhere\"\n";
  assert(default_agent(&f->left, produce, &input, &source, &error) == CAI_OK);
  cai_source_close(source);
  source = NULL;
  args[6] = strdup("--namespace-prefix");
  args[7] = strdup("environment");
  args[8] = NULL;
  assert(args[6] != NULL && args[7] != NULL);
  assert(cai_cli_configure_agent_at(8, args, &bootstrap_options,
                                    &environment_agent, NULL, &f->startup,
                                    &error) == 1);
  assert(strcmp(bootstrap_options.sessions_store, "default") == 0);
  assert(
      strcmp(cai_config_getenv(environment_agent, "CAI_STORAGE_SESSIONS_STORE"),
             "elsewhere") == 0);
  cai_config_close(environment_agent);
  cai_cli_pouch_close(&f->left);
  free(args[6]);
  free(args[7]);
  args[6] = NULL;
  bootstrap_options = f->options;

  /* Escaped values span multiple transport chunks, without a full INI spool. */
  large = malloc(size + 1U);
  assert(large != NULL);
  memset(large, 'x', size);
  large[size] = '\0';
  large[13] = '\n';
  large[26] = '\t';
  large[40] = '"';
  large[55] = '\\';
  assert(cai_config_set(bootstrap, "agent", "developer_instructions", large,
                        &error) == CAI_OK);
  assert(cai_config_set(bootstrap, "", "config_env_has_precedence", "true",
                        &error) == CAI_OK);
  bootstrap_options.namespace_prefix = "quoted";
  assert(cai_cli_agent_configuration(bootstrap, bootstrap, &bootstrap_options,
                                     NULL, &f->startup, &quoted,
                                     &error) == CAI_OK);
  assert(strcmp(cai_config_get(quoted, "agent", "developer_instructions"),
                large) == 0);
  assert(cai_config_get_bool(quoted, "", "config_env_has_precedence", 0));
  free(large);
  cai_config_close(quoted);
  large = malloc(1024U * 1024U + 1U);
  assert(large != NULL);
  memset(large, '\\', 1024U * 1024U);
  large[1024U * 1024U] = '\0';
  assert(cai_config_set(bootstrap, "agent", "developer_instructions", large,
                        &error) == CAI_OK);
  free(large);
  bootstrap_options.namespace_prefix = "line-limit";
  assert(cai_cli_agent_configuration(bootstrap, bootstrap, &bootstrap_options,
                                     NULL, &f->startup, &quoted,
                                     &error) != CAI_OK);
  assert(quoted == NULL);
  assert(error.message != NULL && strstr(error.message, "encoded INI line"));
  cai_error_cleanup(&error);
  cai_error_init(&error);
  assert(cai_config_set(bootstrap, "agent", "developer_instructions", "retry",
                        &error) == CAI_OK);
  assert(cai_cli_agent_configuration(bootstrap, bootstrap, &bootstrap_options,
                                     NULL, &f->startup, &quoted,
                                     &error) == CAI_OK);
  assert(strcmp(cai_config_get(quoted, "agent", "developer_instructions"),
                "retry") == 0);
  cai_config_close(quoted);
  cai_config_close(bootstrap);
  cai_config_close(f->first);
  f->first = NULL;
  cai_cli_pouch_close(&f->session);
  assert(cai_test_storage_open_count() == 1U);
  /* close() dispatch released the last repository reference, allowing reopen.
   */
  assert(cai_cli_configure_agent_at(6, args, &f->options, &f->first, NULL,
                                    &f->startup, &error) == 1);
  assert(cai_test_storage_open_count() == 2U);
  cai_cli_agent_configuration_detach(f->first);
  assert(cai_config_get(f->first, "paths", "state_dir") != NULL);
  cai_config_close(f->first);
  free(args[0]);
  free(args[1]);
  free(args[2]);
  free(args[4]);
  cai_environment_close(f->startup.environment);
  cai_error_cleanup(&error);
  return NULL;
}

int main(int argc, char **argv) {
  fixture *f = calloc(1U, sizeof(*f));
  pthread_t worker;
  assert(argc == 2 && f != NULL);
  assert(snprintf(f->root, sizeof(f->root), "%s/stored-agents.XXXXXX",
                  argv[1]) < (int)sizeof(f->root));
  assert(mkdtemp(f->root) != NULL);
  assert(pthread_create(&worker, NULL, run, f) == 0);
  assert(pthread_join(worker, NULL) == 0);
  free(f);
  return 0;
}
