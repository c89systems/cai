#ifndef CAI_CHROME_HOST_H
#define CAI_CHROME_HOST_H
#include <cai/chrome.h>
#include <pslog.h>
/** Resolve the native-host discovery directory; create it only when asked. */
int cai_cli_chrome_directory(char *out, size_t capacity, int create,
                             cai_error *error);
/** Check supported system registration paths before changing user setup. */
int cai_cli_chrome_check_system_registration(const char *const *paths,
                                             size_t count, cai_error *error);
#endif
