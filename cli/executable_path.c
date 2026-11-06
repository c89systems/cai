#define _XOPEN_SOURCE 700
#include "executable_path.h"
#include "error.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

int cai_cli_executable_path(char *buffer, size_t capacity, cai_error *error) {
  int rc = CAI_OK;
  if (buffer != NULL && capacity != 0U)
    buffer[0] = '\0';
  if (buffer == NULL || capacity < 2U)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "executable path destination requires two bytes");
#ifdef __APPLE__
  {
    char *resolved;
    uint32_t size;
    if (capacity > UINT32_MAX)
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "executable path capacity exceeds native limit");
    size = (uint32_t)capacity;
    /* dyld may return a symlink or relative path. Canonicalize before any
     * launcher operation, while still in the original process directory. */
    if (_NSGetExecutablePath(buffer, &size) != 0 ||
        memchr(buffer, '\0', capacity) == NULL || buffer[0] == '\0')
      rc = CAI_ERR_TRANSPORT;
    else {
      resolved = realpath(buffer, NULL);
      if (resolved == NULL)
        rc = CAI_ERR_TRANSPORT;
      else {
        if (strlen(resolved) >= capacity)
          rc = CAI_ERR_LIMIT;
        else
          memcpy(buffer, resolved, strlen(resolved) + 1U);
        free(resolved);
      }
    }
  }
#else
  {
    ssize_t n = readlink("/proc/self/exe", buffer, capacity);
    if (n <= 0 || (size_t)n >= capacity)
      rc = CAI_ERR_TRANSPORT;
    else
      buffer[n] = '\0';
  }
#endif
  if (rc == CAI_OK && buffer[0] == '/')
    return CAI_OK;
  buffer[0] = '\0';
  return cai_cli_error(
      error, rc == CAI_OK ? CAI_ERR_PROTOCOL : rc,
      "resolve running executable for quiet workspace startup");
}
