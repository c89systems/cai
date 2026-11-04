#ifndef CAI_WORKSPACE_PRIVATE_H
#define CAI_WORKSPACE_PRIVATE_H
#include <cai/cai.h>
/** Owns one discovery record and optional name reservation until close.
 * The server retains its separate workspace ownership lock throughout. */
typedef struct cai_workspace_publication cai_workspace_publication;
/** Hash an already selected workspace identity into a NUL-terminated socket
 * key. The caller supplies a 33-byte output buffer. */
int cai_workspace_hash(const char *, char out[33], cai_error *);
/** Publication requires the caller's workspace ownership lock for its entire
 * lifetime. Name locks are native lifetime locks too, never polled/renewed. */
int cai_workspace_publication_open(const char *socket_path,
                                   const char *workspace, const char *id,
                                   const char *name,
                                   const unsigned char instance[16],
                                   cai_workspace_publication **out,
                                   cai_error *);
/** Remove this instance's publication and release its native name lock. */
void cai_workspace_publication_close(cai_workspace_publication *);
#endif
