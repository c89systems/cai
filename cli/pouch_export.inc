#include "journal_projection.h"

int cai_cli_pouch_export(cai_cli_pouch *pouch, const char *id, cai_sink *sink,
                         cai_error *error) {
  lc_collection *collection = NULL;
  int rc = history_collection(pouch, id, "events", 0, &collection, error);
  if (rc == CAI_OK)
    rc = cai_cli_journal_project(collection, sink, error);
  if (collection != NULL)
    collection->close(collection);
  return rc;
}
