#include "documents.h"
#include "error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int progress(cai_cli_documents *d, cai_error *error) {
  d->progress_bytes = 0U;
  return d->progress != NULL ? d->progress(d->progress_context, error) : CAI_OK;
}
static int document_record(cai_cli_documents *d, const char *kind,
                           const char *field, const char *text, int code,
                           long long http_status, cai_error *error) {
  cai_execution_record *record;
  cai_agent_execution *writer = NULL;
  int rc;
  /* The record includes a 4 KiB payload; keep it off small worker stacks. */
  record = calloc(1U, sizeof(*record));
  if (record == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate command metadata");
  record->kind = kind;
  record->call_id = d->active ? d->call : NULL;
  record->field = field;
  record->line_start = 1;
  record->code = code;
  record->http_status = http_status;
  rc = cai_agent_runtime_execution_open(d->runtime, record, &writer, error);
  free(record);
  if (rc == CAI_OK && text != NULL)
    rc = cai_agent_execution_write(writer, text, strlen(text), error);
  if (rc == CAI_OK)
    rc = cai_agent_execution_finish(writer, error);
  cai_agent_execution_close(writer);
  if (rc == CAI_OK)
    rc = progress(d, error);
  if (rc != CAI_OK)
    cai_cli_error(&d->error, rc, "persist command record");
  return rc;
}

int cai_cli_documents_finish(cai_cli_documents *d) {
  int rc = d->error.code;
  if (rc == CAI_OK && d->writer != NULL)
    rc = cai_agent_execution_finish(d->writer, &d->error);
  if (rc == CAI_OK && d->writer != NULL)
    rc = progress(d, &d->error);
  cai_agent_execution_close(d->writer);
  d->writer = NULL;
  return rc == CAI_OK ? 0 : -1;
}

int cai_cli_documents_bind(cai_cli_documents *d, cai_agent_runtime *runtime,
                           cai_error *error) {
  int rc = CAI_OK;
  if (d->runtime == runtime)
    return CAI_OK;
  if (cai_cli_documents_finish(d) != 0)
    rc = cai_cli_error(error, d->error.code, "finish command document");
  if (rc == CAI_OK && d->runtime != NULL && d->active)
    rc = document_record(d, "command.handoff", NULL, NULL, CAI_OK, 0, error);
  /* Always discard the old stream before its runtime is closed. The command
   * identity survives /new and /resume; no old-runtime pointer survives. */
  d->runtime = runtime;
  return rc;
}

int cai_cli_documents_begin(cai_cli_documents *d, const cai_input_claim *claim,
                            cai_error *error) {
  int n;
  if (d->active || d->runtime == NULL || claim == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID, "command context unavailable");
  if (cai_cli_documents_finish(d) != 0)
    return cai_cli_error(error, d->error.code, "finish operator notice");
  n = snprintf(d->call, sizeof(d->call), "command:%s:%llu:%llu:%llu",
               cai_agent_runtime_session_id(d->runtime), claim->entry.client_id,
               claim->entry.request_id, claim->entry.id);
  if (n < 0 || (size_t)n >= sizeof(d->call))
    return cai_cli_error(error, CAI_ERR_LIMIT,
                         "command identity exceeds limit");
  d->active = 1;
  return document_record(d, "command.input", "text", claim->text, CAI_OK, 0,
                         error);
}

int cai_cli_documents_write(cai_cli_documents *d, const void *bytes,
                            size_t length) {
  int rc = d->error.code;
  cai_execution_record *record;
  if (rc != CAI_OK || length == 0U)
    return rc == CAI_OK ? 0 : -1;
  if (d->writer == NULL) {
    record = calloc(1U, sizeof(*record));
    if (record == NULL) {
      cai_cli_error(&d->error, CAI_ERR_NOMEM, "allocate operator metadata");
      return -1;
    }
    record->kind = d->active ? "command.markdown" : "application.markdown";
    record->call_id = d->active ? d->call : NULL;
    record->field = "text";
    record->line_start = 1;
    rc = cai_agent_runtime_execution_open(d->runtime, record, &d->writer,
                                          &d->error);
    free(record);
  }
  if (rc == CAI_OK)
    rc = cai_agent_execution_write(d->writer, bytes, length, &d->error);
  if (rc == CAI_OK) {
    /* Small formatting writes do not justify a socket pass before the native
     * execution writer can have emitted a chunk. Finish always flushes. */
    if (length >= CAI_EXECUTION_CHUNK_BYTES - d->progress_bytes)
      rc = progress(d, &d->error);
    else
      d->progress_bytes += length;
  }
  return rc == CAI_OK ? 0 : -1;
}

int cai_cli_documents_delegate(cai_cli_documents *d, const char *text,
                               cai_error *error) {
  return document_record(d, "command.frontend", "text", text, CAI_OK, 0, error);
}

int cai_cli_documents_complete(cai_cli_documents *d, int outcome,
                               cai_error *error) {
  int rc = CAI_OK;
  if (cai_cli_documents_finish(d) != 0)
    rc = cai_cli_error(error, d->error.code,
                       d->error.message != NULL ? d->error.message
                                                : "write command document");
  if (rc == CAI_OK && d->runtime != NULL) {
    /* Keep an independent error output so persistence cannot replace the
     * borrowed failure fields while they are being recorded. */
    const char *values[4];
    static const char *const names[] = {"message", "detail", "provider_code",
                                        "request_id"};
    size_t i;
    long long status = error != NULL ? error->http_status : 0;
    cai_error local;
    values[0] = error != NULL && error->message != NULL ? error->message
                                                        : "Command failed.";
    values[1] = error != NULL ? error->detail : NULL;
    values[2] = error != NULL ? error->server_code : NULL;
    values[3] = error != NULL ? error->request_id : NULL;
    cai_error_init(&local);
    rc = document_record(
        d, outcome == CAI_OK ? "command.completed" : "command.failed", NULL,
        NULL, outcome, status, &local);
    for (i = 0U; rc == CAI_OK && outcome != CAI_OK && i < 4U; ++i)
      if (values[i] != NULL && values[i][0] != '\0')
        rc = document_record(d, "command.failed", names[i], values[i], outcome,
                             status, &local);
    if (rc != CAI_OK)
      cai_cli_error(error, rc,
                    local.message != NULL ? local.message
                                          : "record command result");
    cai_error_cleanup(&local);
  }
  d->active = 0;
  d->call[0] = '\0';
  return rc != CAI_OK ? rc : outcome;
}

void cai_cli_documents_close(cai_cli_documents *d) {
  cai_agent_execution_close(d->writer);
  cai_error_cleanup(&d->error);
  memset(d, 0, sizeof(*d));
}
