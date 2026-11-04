#ifndef CAI_LJ_H
#define CAI_LJ_H

#include <lonejson.h>

/** Borrow the process-wide ordinary mapped-JSON runtime.
 * These configurations are initialized with pthread_once and freed at exit.
 * Callers own their parse state and must not destroy these shared runtimes. */
lonejson *cai_lonejson_runtime(void);
/** Borrow the shared runtime that preserves destination configuration. */
lonejson *cai_lonejson_runtime_preserve(void);
/** Borrow the shared runtime configured for streamed response values. */
lonejson *cai_lonejson_runtime_stream(void);
/** Borrow the shared runtime used for model catalog JSON. */
lonejson *cai_lonejson_runtime_catalog(void);
/** Borrow the shared runtime used for bounded Chrome protocol values. */
lonejson *cai_lonejson_runtime_chrome(void);
/** Create a separately owned runtime with explicit limits/configuration.
 * Pair a successful result with cai_lonejson_runtime_close. */
int cai_lonejson_runtime_open(const lonejson_config *config,
                              lonejson **out_runtime, cai_error *error);
/** Destroy an explicitly opened runtime and clear the caller's pointer. */
void cai_lonejson_runtime_close(lonejson **runtime);

#define CAI_LJ cai_lonejson_runtime()
#define CAI_LJ_PRESERVE cai_lonejson_runtime_preserve()
#define CAI_LJ_CHROME cai_lonejson_runtime_chrome()
#define CAI_LJ_STREAM cai_lonejson_runtime_stream()
#define CAI_LJ_CATALOG cai_lonejson_runtime_catalog()

#endif
