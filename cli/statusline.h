#ifndef CAI_CLI_STATUSLINE_H
#define CAI_CLI_STATUSLINE_H
#include "status.h"
#include <softline/softline.h>
/** Frontend-only application of formatted status fields. The projection and
 * Markdown helpers in status.h remain independent of any terminal library. */
int cai_cli_status_apply(sl_t *, const cai_cli_status *);
#endif
