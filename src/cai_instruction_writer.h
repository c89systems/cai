/** @file cai_instruction_writer.h
 * Private, allocation-free template emission shared by agent construction and
 * runtime-free instruction export. All input strings are borrowed for the call.
 */
#ifndef CAI_INSTRUCTION_WRITER_H
#define CAI_INSTRUCTION_WRITER_H

#include <cai/smith.h>

typedef struct cai_instruction_template {
  const char *text;
  const char *identity;
  const char *tools;
  const char *repository;
  const char *skills;
  const char *extension;
  unsigned long capabilities;
} cai_instruction_template;

/** Expand into bounded sink writes, or count bytes when sink is NULL. The
 * prefix ends immediately before the final tools substitution. Disabled-tool
 * policy fragments are removed only from that model prefix, as in live agents.
 * No complete rendered value is allocated or retained. */
int cai_instruction_template_write(const cai_instruction_template *input,
                                   cai_sink *sink, size_t *length,
                                   size_t *prefix_length, cai_error *error);

#endif
