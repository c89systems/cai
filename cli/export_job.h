/** @file One retained export worker per workspace owner. No UI/runtime calls.
 */
#ifndef CAI_CLI_EXPORT_JOB_H
#define CAI_CLI_EXPORT_JOB_H
#include "session_commands.h"
typedef struct cai_cli_export_job cai_cli_export_job;
typedef struct cai_cli_export_result {
  char call[256], paths[2U * PATH_MAX + 2U];
  size_t length;
  int status;
  cai_error error;
  /* Transferred input resources, cleared by the worker before completion. */
  cai_cli_export *source;
  cai_agent_handover *capture;
} cai_cli_export_result;
int cai_cli_export_job_open(cai_cli_export_job **, cai_error *);
/** Transfer source/capture on success; one operation at a time. */
int cai_cli_export_job_start(cai_cli_export_job *, cai_cli_export *,
                             cai_agent_handover *, const char *call,
                             cai_error *);
int cai_cli_export_job_active(cai_cli_export_job *);
int cai_cli_export_job_fd(cai_cli_export_job *);
void cai_cli_export_job_cancel(cai_cli_export_job *);
/** Owner takes a completed result once; NULL means no result is ready. */
cai_cli_export_result *cai_cli_export_job_take(cai_cli_export_job *);
void cai_cli_export_result_close(cai_cli_export_result *);
/** Cancel, join, and release resources before the borrowed store closes.
 * Optional out takes the unconsumed outcome for owner-thread settlement;
 * otherwise it is discarded. A job which never started is cancelled. */
void cai_cli_export_job_close(cai_cli_export_job *, cai_cli_export_result **);
#endif
