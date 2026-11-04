/** @file cai/app_catalog.h Streaming presentation catalogs from an owner. */
#ifndef CAI_APP_CATALOG_H
#define CAI_APP_CATALOG_H
#include <cai/cai.h>
#include <cai/skills.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum cai_app_catalog_kind {
  CAI_APP_CATALOG_SKILLS = 0
} cai_app_catalog_kind;
typedef enum cai_app_catalog_phase {
  CAI_APP_CATALOG_BEGIN = 0,
  CAI_APP_CATALOG_ITEM,
  CAI_APP_CATALOG_END,
  CAI_APP_CATALOG_ERROR
} cai_app_catalog_phase;
/** One completed, borrowed catalog row. BEGIN replaces the previous candidate;
 * ITEM has a zero-based index; END's index is the accepted row count. Publish
 * a candidate only at END. ERROR discards it. Revision increases per owner;
 * conversation is the source epoch, never an authority grant. Text is literal
 * untrusted data: a name for ITEM, a diagnostic for ERROR, empty otherwise.
 * No descriptions, resource bodies or complete catalog are gathered by the
 * transport. A frontend may retain its explicit completion list. */
typedef struct cai_app_catalog_event {
  cai_app_catalog_kind kind;
  cai_app_catalog_phase phase;
  uint64_t revision, conversation, index;
  const char *text;
  int code;
} cai_app_catalog_event;
#ifdef __cplusplus
}
#endif
#endif
