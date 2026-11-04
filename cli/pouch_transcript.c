#include "pouch.h"
#include <stdlib.h>
#include <string.h>
typedef struct transcript_read {
  cai_cli_transcript *parser;
  cai_error *error;
  int rc;
} transcript_read;
static int transcript_write(lc_sink *sink, const void *bytes, size_t count,
                            lc_error *error) {
  transcript_read *read = sink->impl;
  (void)error;
  read->rc = cai_cli_transcript_feed(read->parser, bytes, count, read->error);
  return read->rc == CAI_OK;
}
static int transcript_boundary(void *context,
                               const lc_collection_read_metadata *metadata,
                               lc_error *error) {
  transcript_read *read = context;
  (void)error;
  if (metadata->kind == LC_COLLECTION_READ_METADATA_ENTRY)
    read->rc = cai_cli_transcript_boundary(read->parser, metadata->position,
                                           metadata->append_id, read->error);
  return read->rc == CAI_OK ? LC_OK : LC_ERR_INVALID;
}
int cai_cli_pouch_transcript_read(cai_cli_pouch_transcript *reader,
                                  const lc_collection_read_opts *options,
                                  lc_collection_read_res *out,
                                  cai_cli_transcript_fn callback, void *context,
                                  cai_error *error) {
  lc_collection_read_opts selected;
  lc_sink sink;
  lc_error detail;
  transcript_read read;
  int rc;
  if (reader == NULL || reader->reading || out == NULL || callback == NULL ||
      (options != NULL &&
       (options->on_metadata != NULL || options->selector_lql != NULL)))
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "idle transcript context, result and consumer "
                         "required; query selectors "
                         "and metadata callbacks are unavailable");
  memset(&read, 0, sizeof(read));
  memset(&sink, 0, sizeof(sink));
  read.error = error;
  rc = cai_cli_transcript_open(callback, context, &read.parser, error);
  if (rc != CAI_OK)
    return rc;
  reader->reading = 1;
  lc_collection_read_opts_init(&selected);
  if (options != NULL)
    selected = *options;
  selected.on_metadata = transcript_boundary;
  selected.metadata_context = &read;
  sink.write = transcript_write;
  sink.impl = &read;
  lc_error_init(&detail);
  rc = reader->collection->read(reader->collection, &selected, &sink, out,
                                &detail);
  if (read.rc != CAI_OK)
    rc = read.rc;
  else if (out->stop_reason == LC_COLLECTION_READ_CANCELLED)
    rc = cai_cli_error(error, CAI_ERR_CANCELLED, "transcript read cancelled");
  else if (rc != LC_OK)
    rc = cai_cli_lc_error(error, &detail);
  else if (!out->complete)
    rc = cai_cli_error(error, CAI_ERR_PROTOCOL,
                       "transcript read ended without a complete window");
  else
    rc = cai_cli_transcript_finish(read.parser, error);
  cai_cli_transcript_close(read.parser);
  lc_error_cleanup(&detail);
  reader->reading = 0;
  return rc;
}
void cai_cli_pouch_transcript_close(cai_cli_pouch_transcript *reader) {
  if (reader == NULL || reader->reading)
    return;
  lc_collection_close(reader->collection);
  free(reader);
}
