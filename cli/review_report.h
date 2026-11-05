#ifndef CAI_CLI_REVIEW_REPORT_H
#define CAI_CLI_REVIEW_REPORT_H

#include <stddef.h>

/** Shared boundary titles for manual, model-invoked and replayed reviewers. */
#define CAI_CLI_REVIEW_START "**Review started**"
#define CAI_CLI_REVIEW_END "**Review finished**"

/* Format one libcai-validated review report. Caller releases *out with free. */
int cai_cli_review_format(const char *json, size_t length,
                          const char *output_type, char **out);

#endif
