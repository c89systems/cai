#include "shell_text.h"
#include <string.h>

int cai_cli_shell_plain(cai_cli_shell_text *shell, const char *bytes,
                        size_t length, int final,
                        cai_cli_shell_output_fn output, void *context,
                        cai_error *error) {
  static const char hex[] = "0123456789abcdef";
  char rendered[512];
  size_t at = 0U, used = 0U, count, i;
  unsigned long scalar;
  int rc;
  while (at < length || shell->pending_length != 0U) {
    unsigned char ch;
    if (at < length && shell->pending_length < sizeof(shell->pending))
      shell->pending[shell->pending_length++] = (unsigned char)bytes[at++];
    ch = shell->pending[0];
    count = ch < 128U                  ? 1U
            : ch >= 194U && ch <= 223U ? 2U
            : ch >= 224U && ch <= 239U ? 3U
            : ch >= 240U && ch <= 244U ? 4U
                                       : 0U;
    scalar = count > 1U ? ch & ((1U << (7U - count)) - 1U) : ch;
    for (i = 1U; i < count && i < shell->pending_length; i++) {
      if ((shell->pending[i] & 192U) != 128U) {
        count = 0U;
        break;
      }
      scalar = (scalar << 6) | (shell->pending[i] & 63U);
    }
    if (count > shell->pending_length) {
      if (at < length)
        continue;
      if (!final)
        break;
      count = 0U;
    }
    if ((count == 2U && scalar < 128UL) || (count == 3U && scalar < 2048UL) ||
        (count == 4U && scalar < 65536UL) || scalar > 1114111UL ||
        (scalar >= 55296UL && scalar <= 57343UL) ||
        (scalar >= 128UL && scalar <= 159UL) ||
        (count == 1U && ((ch < 32U && ch != '\n' && ch != '\t') || ch == 127U)))
      count = 0U;
    if (sizeof(rendered) - used < 4U) {
      rc = output(context, rendered, used, error);
      if (rc != CAI_OK)
        return rc;
      used = 0U;
    }
    if (count == 0U) {
      rendered[used++] = '\\';
      rendered[used++] = 'x';
      rendered[used++] = hex[ch >> 4];
      rendered[used++] = hex[ch & 15U];
      count = 1U;
    } else {
      memcpy(rendered + used, shell->pending, count);
      used += count;
    }
    shell->pending_length -= count;
    memmove(shell->pending, shell->pending + count, shell->pending_length);
  }
  return used != 0U ? output(context, rendered, used, error) : CAI_OK;
}
