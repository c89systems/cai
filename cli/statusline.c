#include "statusline.h"
int cai_cli_status_apply(sl_t *sl, const cai_cli_status *status) {
  return sl_set_status_elements(sl, status->elements, status->count) == SL_OK
             ? 0
             : -1;
}
