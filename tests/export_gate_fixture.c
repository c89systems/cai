#define _POSIX_C_SOURCE 200809L
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../cli/journal_projection.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Gate actual SDK delivery, after it has captured a read view. Only this
 * export receiver is instrumented; ordinary owner writes remain untouched. */
static int (*original_read)(lc_collection *, const lc_collection_read_opts *,
                            lc_sink *, lc_collection_read_res *, lc_error *);
static pthread_t first_worker;
static int have_worker;
static size_t read_number;
typedef struct gate {
  lc_sink *destination;
  const lc_collection_read_opts *options;
  int entered;
} gate;
static int gate_write(lc_sink *sink, const void *bytes, size_t length,
                      lc_error *error) {
  gate *g = sink->impl;
  const char *directory = getenv("CAI_TEST_EXPORT_GATE");
  const char *selected = getenv("CAI_TEST_EXPORT_GATE_PASS");
  size_t pass = selected != NULL ? (size_t)strtoul(selected, NULL, 10) : 1U;
  if (!g->entered && read_number == pass) {
    char *path = malloc(PATH_MAX);
    struct pollfd fd;
    FILE *marker;
    char token = 0;
    int stop = 0;
    assert(path);
    g->entered = 1;
    if (have_worker)
      assert(pthread_equal(first_worker, pthread_self()));
    else {
      first_worker = pthread_self();
      have_worker = 1;
    }
    snprintf(path, PATH_MAX, "%s/release", directory);
    fd.fd = open(path, O_RDONLY | O_NONBLOCK);
    assert(fd.fd >= 0);
    fd.events = POLLIN;
    snprintf(path, PATH_MAX, "%s/entered", directory);
    marker = fopen(path, "w");
    assert(marker && fputs("SDK read gated\n", marker) >= 0 &&
           fclose(marker) == 0);
    free(path);
    for (;;) {
      if (g->options->cancelled != NULL &&
          g->options->cancelled(g->options->cancel_context)) {
        stop = 1;
        break;
      }
      assert(poll(&fd, 1U, 20) >= 0 || errno == EINTR);
      if (read(fd.fd, &token, 1U) == 1)
        break;
    }
    close(fd.fd);
    if (!stop && token == 'f')
      return 0;
  }
  return g->destination->write(g->destination, bytes, length, error);
}
static int gate_read(lc_collection *collection,
                     const lc_collection_read_opts *options, lc_sink *sink,
                     lc_collection_read_res *result, lc_error *error) {
  lc_sink chained;
  gate g;
  ++read_number;
  memset(&g, 0, sizeof(g));
  memset(&chained, 0, sizeof(chained));
  g.destination = sink;
  g.options = options;
  chained.write = gate_write;
  chained.impl = &g;
  return original_read(collection, options, &chained, result, error);
}
int __real_cai_cli_journal_view_open(lc_collection *,
                                     const cai_cli_journal_options *,
                                     cai_cli_journal_view **, cai_error *);
int __wrap_cai_cli_journal_view_open(lc_collection *,
                                     const cai_cli_journal_options *,
                                     cai_cli_journal_view **, cai_error *);
int __wrap_cai_cli_journal_view_open(lc_collection *collection,
                                     const cai_cli_journal_options *options,
                                     cai_cli_journal_view **out,
                                     cai_error *error) {
  if (getenv("CAI_TEST_EXPORT_GATE") != NULL) {
    assert(collection->read != gate_read);
    original_read = collection->read;
    collection->read = gate_read;
    read_number = 0U;
  }
  return __real_cai_cli_journal_view_open(collection, options, out, error);
}
