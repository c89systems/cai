#include "cai_internal.h"
#include <cai/auth.h>
#include <cai/mcp.h>
#include <string.h>

static int chatgpt_headers(void *context, const char *url,
                           cai_mcp_header_set_fn write, void *destination,
                           cai_error *error) {
  cai_mcp_chatgpt_auth_context *config = context;
  cai_buffer_builder bearer;
  char *token = NULL, *account = NULL;
  int rc;
  (void)url;
  memset(&bearer, 0, sizeof(bearer));
  if (config == NULL || config->auth == NULL ||
      config->auth->access_token == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "ChatGPT app authentication is unavailable");
  rc = config->auth->access_token(config->auth, &token, error);
  if (rc == CAI_OK && (token == NULL || token[0] == '\0'))
    rc = cai_set_error(error, CAI_ERR_INVALID,
                       "ChatGPT app access token is missing");
  if (rc == CAI_OK && config->auth->account_id != NULL)
    rc = config->auth->account_id(config->auth, &account, error);
  if (rc == CAI_OK)
    rc = cai_buffer_append_cstr(&bearer, "Bearer ", error);
  if (rc == CAI_OK)
    rc = cai_buffer_append_cstr(&bearer, token, error);
  if (rc == CAI_OK &&
      (write(destination, "Authorization", bearer.data, error) != 0 ||
       (account != NULL && account[0] != '\0' &&
        write(destination, "ChatGPT-Account-Id", account, error) != 0) ||
       write(destination, "X-OpenAI-Product-Sku",
             config->product_sku != NULL ? config->product_sku : "codex",
             error) != 0 ||
       write(destination, "originator",
             config->originator != NULL ? config->originator
                                        : CAI_CHATGPT_AUTH_DEFAULT_ORIGINATOR,
             error) != 0))
    rc = CAI_ERR_INVALID;
  if (token != NULL)
    memset(token, 0, strlen(token));
  if (bearer.data != NULL)
    memset(bearer.data, 0, bearer.length);
  cai_free_mem(NULL, token);
  cai_free_mem(NULL, account);
  cai_free_mem(NULL, bearer.data);
  return rc;
}

static int chatgpt_refresh(void *context, cai_error *error) {
  cai_mcp_chatgpt_auth_context *config = context;
  if (config == NULL || config->auth == NULL || config->auth->refresh == NULL)
    return cai_set_error(error, CAI_ERR_INVALID, "ChatGPT auth cannot refresh");
  return config->auth->refresh(config->auth, error);
}

void cai_mcp_chatgpt_auth_provider_init(cai_mcp_client_auth_provider *provider,
                                        cai_mcp_chatgpt_auth_context *context) {
  if (provider != NULL) {
    memset(provider, 0, sizeof(*provider));
    provider->context = context;
    provider->headers = chatgpt_headers;
    provider->refresh = chatgpt_refresh;
  }
}
