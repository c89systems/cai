#ifndef CAI_MCP_SEARCH_H
#define CAI_MCP_SEARCH_H
#include "cai_internal.h"

/** Catalog search index owned by one runtime worker. Queries perform no
 * network I/O; rebuild the index when the registered catalog changes. */
typedef struct cai_mcp_search cai_mcp_search;
/** One ranked result, referring to the tool's original registry index. */
typedef struct cai_mcp_search_match {
  size_t index;
  double score;
} cai_mcp_search_match;
/** Allocate an empty index; the caller releases it with destroy. */
int cai_mcp_search_new(cai_mcp_search **out, cai_error *error);
/** Release indexed metadata and token storage; accepts NULL. */
void cai_mcp_search_destroy(cai_mcp_search *search);
/** Index discovery metadata for a deferred tool without calling the tool. */
int cai_mcp_search_add(cai_mcp_search *search, size_t registry_index,
                       const cai_mcp_client_tool *tool, const char *alias,
                       const char *source, const char *description,
                       cai_error *error);
/** Write up to limit ranked matches into caller storage. Returns the number
 * written; use error to distinguish an invalid query from no matches. */
size_t cai_mcp_search_find(const cai_mcp_search *search, const char *query,
                           cai_mcp_search_match *matches, size_t limit,
                           cai_error *error);
/** Caller owns returned description. Sources are deduplicated and bounded. */
char *cai_mcp_search_description(const cai_mcp_search *search,
                                 cai_error *error);
/** Count indexed entries, without contacting configured MCP connections. */
size_t cai_mcp_search_count(const cai_mcp_search *search);
/** Borrow an indexed source name for the lifetime of the index. */
const char *cai_mcp_search_source(const cai_mcp_search *search, size_t index);
#endif
