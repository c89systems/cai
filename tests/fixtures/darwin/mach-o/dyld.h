#ifndef CAI_TEST_DYLD_H
#define CAI_TEST_DYLD_H
#include <stdint.h>
/* Public SDK declaration used by the Linux fault-injection adapter test.
 * The production translation unit is also compiled against the real SDK. */
int _NSGetExecutablePath(char *, uint32_t *);
#endif
