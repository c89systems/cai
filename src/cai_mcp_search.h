#ifndef CAI_MCP_SEARCH_H
#define CAI_MCP_SEARCH_H
#include "cai_internal.h"

/* Immutable catalog index, owned by one runtime worker. No network I/O. */
typedef struct cai_mcp_search cai_mcp_search;
typedef struct cai_mcp_search_match {
  size_t index;
  double score;
} cai_mcp_search_match;
int cai_mcp_search_new(cai_mcp_search **out, cai_error *error);
void cai_mcp_search_destroy(cai_mcp_search *search);
int cai_mcp_search_add(cai_mcp_search *search, size_t registry_index,
                       const cai_mcp_client_tool *tool, const char *alias,
                       const char *source, const char *description,
                       cai_error *error);
size_t cai_mcp_search_find(const cai_mcp_search *search, const char *query,
                           cai_mcp_search_match *matches, size_t limit,
                           cai_error *error);
/* Caller owns returned description. Sources are deduplicated and bounded. */
char *cai_mcp_search_description(const cai_mcp_search *search,
                                 cai_error *error);
size_t cai_mcp_search_count(const cai_mcp_search *search);
const char *cai_mcp_search_source(const cai_mcp_search *search, size_t index);
#endif
