#define _POSIX_C_SOURCE 200809L
#include "export_job.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>
struct cai_cli_export_job {
  pthread_t thread;
  pthread_mutex_t lock;
  pthread_cond_t changed;
  cai_cli_export_result *current;
  int fd, stopping, cancelled, completed;
};
void cai_cli_export_result_close(cai_cli_export_result *result) {
  if (result != NULL) {
    cai_cli_export_close(result->source);
    cai_agent_handover_close(result->capture);
    cai_error_cleanup(&result->error);
    free(result);
  }
}
static int cancelled(void *context) {
  cai_cli_export_job *job = context;
  int stop;
  pthread_mutex_lock(&job->lock);
  stop = job->stopping || job->cancelled;
  pthread_mutex_unlock(&job->lock);
  return stop;
}
static int paths(void *context, const void *bytes, size_t length,
                 cai_error *error) {
  cai_cli_export_result *result = context;
  if (length >= sizeof(result->paths) - result->length)
    return cai_cli_error(error, CAI_ERR_LIMIT,
                         "export path listing exceeds limit");
  memcpy(result->paths + result->length, bytes, length);
  result->length += length;
  result->paths[result->length] = '\0';
  return CAI_OK;
}
static void *worker(void *context) {
  cai_cli_export_job *job = context;
  pthread_mutex_lock(&job->lock);
  for (;;) {
    cai_cli_export_result *result;
    cai_handover_config config;
    const cai_handover_config *active;
    cai_sink sink;
    uint64_t wake = 1U;
    while (!job->stopping && (job->current == NULL || job->completed))
      pthread_cond_wait(&job->changed, &job->lock);
    if (job->stopping)
      break;
    result = job->current;
    pthread_mutex_unlock(&job->lock);
    memset(&sink, 0, sizeof(sink));
    sink.callbacks.write = paths;
    sink.callbacks.context = result;
    active = cai_agent_handover_config(result->capture);
    if (active != NULL) {
      config = *active;
      config.cancelled = cancelled;
      config.cancel_context = job;
      active = &config;
    }
    result->status = cai_cli_export_run(result->source, active, cancelled, job,
                                        &sink, &result->error);
    cai_cli_export_close(result->source);
    result->source = NULL;
    cai_agent_handover_close(result->capture);
    result->capture = NULL;
    pthread_mutex_lock(&job->lock);
    job->completed = 1;
    while (write(job->fd, &wake, sizeof(wake)) < 0 && errno == EINTR) {
    }
  }
  pthread_mutex_unlock(&job->lock);
  return NULL;
}
int cai_cli_export_job_open(cai_cli_export_job **out, cai_error *error) {
  cai_cli_export_job *job = calloc(1U, sizeof(*job));
  *out = NULL;
  if (job == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate export worker");
  job->fd = eventfd(0U, EFD_NONBLOCK | EFD_CLOEXEC);
  if (job->fd < 0) {
    free(job);
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "create export wakeup");
  }
  if (pthread_mutex_init(&job->lock, NULL) != 0)
    goto fail_lock;
  if (pthread_cond_init(&job->changed, NULL) != 0)
    goto fail_condition;
  if (pthread_create(&job->thread, NULL, worker, job) != 0)
    goto fail_thread;
  *out = job;
  return CAI_OK;
fail_thread:
  pthread_cond_destroy(&job->changed);
fail_condition:
  pthread_mutex_destroy(&job->lock);
fail_lock:
  close(job->fd);
  free(job);
  return cai_cli_error(error, CAI_ERR_TRANSPORT, "initialize export worker");
}
int cai_cli_export_job_start(cai_cli_export_job *job, cai_cli_export *source,
                             cai_agent_handover *capture, const char *call,
                             cai_error *error) {
  cai_cli_export_result *result;
  if (source == NULL || call == NULL || strlen(call) >= 256U)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "export source and command identity required");
  result = calloc(1U, sizeof(*result));
  if (result == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate export result");
  cai_error_init(&result->error);
  strcpy(result->call, call);
  pthread_mutex_lock(&job->lock);
  if (job->stopping || job->current != NULL) {
    pthread_mutex_unlock(&job->lock);
    cai_cli_export_result_close(result);
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "export worker already occupied");
  }
  result->source = source;
  result->capture = capture;
  job->current = result;
  job->cancelled = job->completed = 0;
  pthread_cond_signal(&job->changed);
  pthread_mutex_unlock(&job->lock);
  return CAI_OK;
}
int cai_cli_export_job_active(cai_cli_export_job *job) {
  int active = 0;
  if (job != NULL) {
    pthread_mutex_lock(&job->lock);
    active = job->current != NULL;
    pthread_mutex_unlock(&job->lock);
  }
  return active;
}
int cai_cli_export_job_fd(cai_cli_export_job *job) {
  return job != NULL ? job->fd : -1;
}
void cai_cli_export_job_cancel(cai_cli_export_job *job) {
  if (job != NULL) {
    pthread_mutex_lock(&job->lock);
    job->cancelled = 1;
    pthread_mutex_unlock(&job->lock);
  }
}
cai_cli_export_result *cai_cli_export_job_take(cai_cli_export_job *job) {
  cai_cli_export_result *result = NULL;
  uint64_t wake;
  if (job == NULL)
    return NULL;
  pthread_mutex_lock(&job->lock);
  if (job->completed) {
    result = job->current;
    job->current = NULL;
    job->completed = 0;
    while (read(job->fd, &wake, sizeof(wake)) < 0 && errno == EINTR) {
    }
  }
  pthread_mutex_unlock(&job->lock);
  return result;
}
void cai_cli_export_job_close(cai_cli_export_job *job,
                              cai_cli_export_result **out) {
  if (out != NULL)
    *out = NULL;
  if (job == NULL)
    return;
  pthread_mutex_lock(&job->lock);
  job->stopping = 1;
  pthread_cond_signal(&job->changed);
  pthread_mutex_unlock(&job->lock);
  pthread_join(job->thread, NULL);
  if (job->current != NULL && !job->completed) {
    job->current->status = CAI_ERR_CANCELLED;
    cai_cli_error(&job->current->error, CAI_ERR_CANCELLED,
                  "conversation export cancelled");
  }
  if (out != NULL)
    *out = job->current;
  else
    cai_cli_export_result_close(job->current);
  close(job->fd);
  pthread_cond_destroy(&job->changed);
  pthread_mutex_destroy(&job->lock);
  free(job);
}
