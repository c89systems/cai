#define _XOPEN_SOURCE 700
#include "../cli/executable_path.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

static int mode, calls;
static const char *image;
#ifdef __APPLE__
int _NSGetExecutablePath(char *buffer, uint32_t *size) {
  size_t n = strlen(image) + 1U;
  calls++;
  if (mode == 1 || n > *size) {
    *size = (uint32_t)n;
    return -1;
  }
  if (mode == 2)
    memset(buffer, 'x', *size);
  else if (mode == 3)
    buffer[0] = '\0';
  else
    memcpy(buffer, image, n);
  return 0;
}
#else
ssize_t __real_readlink(const char *, char *, size_t);
ssize_t __wrap_readlink(const char *, char *, size_t);
ssize_t __wrap_readlink(const char *path, char *buffer, size_t capacity) {
  size_t n;
  if (strcmp(path, "/proc/self/exe") != 0)
    return __real_readlink(path, buffer, capacity);
  calls++;
  if (mode == 0)
    return __real_readlink(path, buffer, capacity);
  if (mode == 1) {
    errno = EIO;
    return -1;
  }
  if (mode == 2) {
    memset(buffer, 'x', capacity);
    return (ssize_t)capacity;
  }
  if (mode == 3)
    return 0;
  n = strlen(image);
  if (n > capacity)
    n = capacity;
  memcpy(buffer, image, n);
  return (ssize_t)n;
}
#endif
int main(int argc, char **argv) {
  char path[PATH_MAX], small[4] = "xxx";
  char *expected;
  int i;
  assert(argc == 1);
  expected = realpath(argv[0], NULL);
  assert(expected != NULL);
  image = expected;
  assert(cai_cli_executable_path(path, sizeof(path), NULL) == CAI_OK);
  assert(strcmp(path, expected) == 0 && calls == 1);
  calls = 0;
  assert(cai_cli_executable_path(NULL, 0U, NULL) == CAI_ERR_INVALID);
  assert(cai_cli_executable_path(small, 0U, NULL) == CAI_ERR_INVALID);
  assert(cai_cli_executable_path(small, 1U, NULL) == CAI_ERR_INVALID);
  assert(calls == 0 && small[0] == '\0');
  for (i = 1; i <= 3; i++) {
    mode = i;
    strcpy(path, "previous executable must not survive failure");
    assert(cai_cli_executable_path(path, sizeof(path), NULL) != CAI_OK);
    assert(path[0] == '\0');
  }
  mode = 0;
  assert(cai_cli_executable_path(small, sizeof(small), NULL) != CAI_OK);
  assert(small[0] == '\0');
#ifdef __APPLE__
  {
    char cwd[PATH_MAX];
    size_t length;
    assert(getcwd(cwd, sizeof(cwd)) != NULL);
    length = strlen(cwd);
    assert(strncmp(expected, cwd, length) == 0 && expected[length] == '/');
    image = expected + length + 1U;
    assert(image[0] != '/');
    assert(cai_cli_executable_path(path, sizeof(path), NULL) == CAI_OK);
    assert(strcmp(path, expected) == 0);
  }
  /* dyld's path is not necessarily canonical. Resolve a real symlink and a
   * relative invocation path without changing cwd or consulting PATH. */
  image = "/proc/self/exe";
  assert(cai_cli_executable_path(path, sizeof(path), NULL) == CAI_OK);
  assert(strcmp(path, expected) == 0);
  image = "/this/executable/does/not/exist";
  assert(cai_cli_executable_path(path, sizeof(path), NULL) != CAI_OK &&
         path[0] == '\0');
  if (SIZE_MAX > UINT32_MAX) {
    calls = 0;
    assert(cai_cli_executable_path(small, (size_t)UINT32_MAX + 1U, NULL) ==
           CAI_ERR_INVALID);
    assert(calls == 0 && small[0] == '\0');
  }
#else
  mode = 4;
  image = "/ab";
  assert(cai_cli_executable_path(small, sizeof(small), NULL) == CAI_OK);
  assert(strcmp(small, image) == 0); /* capacity minus one is a valid path */
  image = "abc";
  assert(cai_cli_executable_path(small, sizeof(small), NULL) ==
             CAI_ERR_PROTOCOL &&
         small[0] == '\0');
#endif
  free(expected);
  return 0;
}
