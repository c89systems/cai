/** @file Bounded incremental validation of best-effort event JSONL. */
#ifndef CAI_NETWORK_JSONL_H
#define CAI_NETWORK_JSONL_H
#include <cai/cai.h>
typedef struct cai_network_jsonl cai_network_jsonl;
typedef struct cai_network_jsonl_stats {
  unsigned long long events, invalid_lines;
  int partial_line;
} cai_network_jsonl_stats;
int cai_network_jsonl_open(cai_network_jsonl **, cai_error *);
void cai_network_jsonl_feed(cai_network_jsonl *, void *, size_t);
void cai_network_jsonl_get(cai_network_jsonl *, cai_network_jsonl_stats *);
void cai_network_jsonl_close(cai_network_jsonl *);
#endif
