/** One bounded accepted input. Length-prefixed fields preserve arbitrary
 * whitespace without JSON expansion or a second persistence transaction. */
#define CAI_RUNTIME_GOAL_INPUT_MAX 66048U
/** Stable across recovery, distinct after imports into another scope/session.
 * NUL-delimited strings avoid ambiguous joins without copying their contents.
 * The small marker is committed with the goal before preflight compaction. */
static int cai_runtime_goal_input_identity(cai_agent_runtime *runtime,
                                           unsigned long long sequence,
                                           char out[65], cai_error *error) {
  static const char hex[] = "0123456789abcdef";
  EVP_MD_CTX *digest = EVP_MD_CTX_new();
  unsigned char bytes[EVP_MAX_MD_SIZE];
  unsigned int length = 0U;
  char number[32];
  size_t i;
  int ok;
  snprintf(number, sizeof(number), "%llu", sequence);
  if (digest == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "allocate goal input identity");
  ok = EVP_DigestInit_ex(digest, EVP_sha256(), NULL) == 1 &&
       EVP_DigestUpdate(digest, runtime->session_scope,
                        strlen(runtime->session_scope) + 1U) == 1 &&
       EVP_DigestUpdate(digest, runtime->session_id,
                        strlen(runtime->session_id) + 1U) == 1 &&
       EVP_DigestUpdate(digest, number, strlen(number)) == 1 &&
       EVP_DigestFinal_ex(digest, bytes, &length) == 1 && length == 32U;
  EVP_MD_CTX_free(digest);
  if (!ok)
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "derive goal input identity");
  for (i = 0U; i < 32U; ++i) {
    out[2U * i] = hex[bytes[i] >> 4U];
    out[2U * i + 1U] = hex[bytes[i] & 15U];
  }
  out[64] = '\0';
  return CAI_OK;
}
static int cai_runtime_goal_input_encode(const char *objective,
                                         const char *prompt, const char *call,
                                         char **out, cai_error *error) {
  char header[80];
  size_t a, b, c, n;
  int written;
  *out = NULL;
  if (objective == NULL || objective[0] == '\0' || prompt == NULL ||
      prompt[0] == '\0')
    return cai_set_error(error, CAI_ERR_INVALID,
                         "goal objective and initial prompt are required");
  a = strlen(objective);
  b = strlen(prompt);
  c = call != NULL ? strlen(call) : 0U;
  if (a > CAI_RUNTIME_GOAL_INPUT_MAX || b > CAI_RUNTIME_GOAL_INPUT_MAX - a ||
      c > 255U)
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "goal initial input exceeds its bounded limit");
  written = snprintf(header, sizeof(header), "%lu\n%lu\n", (unsigned long)a,
                     (unsigned long)c);
  if (written < 0 || (size_t)written >= sizeof(header))
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "goal input header exceeds limit");
  n = (size_t)written;
  *out = cai_alloc(NULL, n + a + b + c + 1U);
  if (*out == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "encode goal initial input");
  memcpy(*out, header, n);
  memcpy(*out + n, objective, a);
  if (c != 0U)
    memcpy(*out + n + a, call, c);
  memcpy(*out + n + a + c, prompt, b + 1U);
  return CAI_OK;
}
/** Parse only the two bounded lengths before copying the owned input fields.
 * Recovery calls this before the ordinary input-consumed acknowledgements. */
static int cai_runtime_goal_input_parse(const char *data,
                                        cai_runtime_input_node *node,
                                        cai_error *error) {
  const char *p, *end;
  unsigned long long a, c;
  size_t n;
  if (data == NULL || cai_parse_unsigned_decimal(data, &end, &a) != 0 ||
      *end != '\n' || a == 0U || a > CAI_RUNTIME_GOAL_INPUT_MAX)
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "invalid goal input objective");
  p = end + 1U;
  if (cai_parse_unsigned_decimal(p, &end, &c) != 0 || *end != '\n' || c > 255U)
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "invalid goal input identity");
  p = end + 1U;
  n = strlen(p);
  if (a + c >= n || n - (size_t)c > CAI_RUNTIME_GOAL_INPUT_MAX)
    return cai_set_error(error, CAI_ERR_PROTOCOL, "invalid goal input payload");
  node->goal_objective = cai_strndup(NULL, p, (size_t)a);
  node->text = cai_strdup(NULL, p + (size_t)(a + c));
  memcpy(node->call_id, p + (size_t)a, (size_t)c);
  node->call_id[c] = '\0';
  return node->text != NULL && node->goal_objective != NULL
             ? CAI_OK
             : cai_set_error(error, CAI_ERR_NOMEM, "copy goal initial input");
}
