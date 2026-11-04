#ifndef CAI_INPUT_QUEUE_INTERNAL_H
#define CAI_INPUT_QUEUE_INTERNAL_H
#include <cai/input_queue.h>
/* One admission path: receipt lookup precedes authorization, and denied
 * mutations retain a normal durable rejected receipt without their payload. */
int cai_input_queue_admit(cai_input_queue_client *client,
                          const cai_input_request *request,
                          int (*authorize)(void *, cai_error *), void *context,
                          cai_input_result *result, cai_error *error);
#endif
