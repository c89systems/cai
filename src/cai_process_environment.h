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
/** Resolve argv[0] before fork, using the explicit child PATH/cwd. Relative
 * PATH components are relative to workspace. No shell, chdir or host getenv.
 * path has 4096 bytes. Returns an absolute executable path. */
int cai_process_program(const char *name, char *const *environment,
                        const char *workspace, char path[4096],
                        cai_error *error);
#endif
