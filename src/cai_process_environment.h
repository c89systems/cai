#ifndef CAI_PROCESS_ENVIRONMENT_H
#define CAI_PROCESS_ENVIRONMENT_H
#include <cai/environment.h>
/** Snapshot before fork. Restricted children preserve HOME, PATH and every
 * XDG_* entry verbatim; YOLO children inherit the complete environment. */
int cai_process_environment_new(const cai_environment *base, int yolo,
                                const char *const *defaults, char ***out,
                                cai_error *error);
/** Free an environment vector returned by cai_process_environment_new. */
void cai_process_environment_free(char **environment);
#endif
