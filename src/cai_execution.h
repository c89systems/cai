#ifndef CAI_EXECUTION_PRIVATE_H
#define CAI_EXECUTION_PRIVATE_H
#include <cai/execution.h>
/** Encode one bounded execution chunk, not a whole logical result.
 * On success the caller owns the returned JSON string; release with
 * cai_free_mem(NULL, ...). Encoding failure must not publish a journal entry.
 */
int cai_execution_record_json(const cai_execution_record *, char **,
                              cai_error *);
#endif
