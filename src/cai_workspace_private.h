#ifndef CAI_WORKSPACE_PRIVATE_H
#define CAI_WORKSPACE_PRIVATE_H
#include <cai/cai.h>
typedef struct cai_workspace_publication cai_workspace_publication;
int cai_workspace_hash(const char *, char out[33], cai_error *);
/* Publication requires the caller's workspace ownership lock for its entire
 * lifetime. Name locks are native lifetime locks too, never polled/renewed. */
int cai_workspace_publication_open(const char *socket_path,
                                   const char *workspace, const char *id,
                                   const char *name,
                                   const unsigned char instance[16],
                                   cai_workspace_publication **out,
                                   cai_error *);
void cai_workspace_publication_close(cai_workspace_publication *);
#endif
