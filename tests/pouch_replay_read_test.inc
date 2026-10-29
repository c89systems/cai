/* Pure parser/range regressions: no waits or additional storage writes. */
static void replay_native_ranges(cai_error *error) {
  cai_cli_pouch *pouch = calloc(1U, sizeof(*pouch));
  collection_events read;
  lc_collection_read_metadata metadata;
  lc_collection_read_opts options;
  size_t i;
  assert(pouch != NULL);
  memset(&read, 0, sizeof(read));
  memset(&metadata, 0, sizeof(metadata));
  read.pouch = pouch;
  read.error = error;
  read.index_positions = 1;
  strcpy(pouch->event_position_session, "indexed");
  for (i = 0U; i < 513U; i++) {
    read.record.sequence = i * 3U + 1U;
    metadata.position = i * 2U + 1U;
    assert(event_position_add(&read, &metadata) == CAI_OK);
  }
  assert(pouch->event_position_count == 5U);
  lc_collection_read_opts_init(&options);
  event_position_window(pouch, "indexed", 388U, 772U, &options);
  assert(options.has_after && options.after == 257U);
  assert(options.has_through && options.through == 769U);
  assert(options.selector_lql == NULL);
  lc_collection_read_opts_init(&options);
  event_position_window(pouch, "indexed", 385U, 385U, &options);
  assert(options.after == 257U && options.through == 257U);
  lc_collection_read_opts_init(&options);
  event_position_window(pouch, "indexed", 1534U, 1537U, &options);
  assert(options.after == 769U && options.through == 1025U);
  /* A new append, another conversation and unbounded recovery must never be
   * truncated to a previous startup's captured upper position. */
  for (i = 0U; i < 3U; i++) {
    lc_collection_read_opts_init(&options);
    event_position_window(pouch, i == 0U ? "other" : "indexed", 0U,
                          i == 1U   ? 0U
                          : i == 2U ? 9999U
                                    : 10U,
                          &options);
    assert(!options.has_after && !options.has_through);
  }
  free(pouch->event_positions);
  free(pouch);
}

static void replay_large_commit(cai_error *error) {
  static const char prefix[] =
      "{\"record_type\":\"commit\",\"state\":{\"history_append\":\"";
  static const char suffix[] = "\"}}\n";
  collection_events read;
  lc_collection_read_metadata metadata;
  lc_sink sink;
  lc_error local;
  lonejson_error json_error;
  lonejson_config config = lonejson_default_config();
  char bytes[4096];
  size_t total;
  memset(&read, 0, sizeof(read));
  memset(&sink, 0, sizeof(sink));
  memset(&metadata, 0, sizeof(metadata));
  lc_error_init(&local);
  read.map = &journal_event_map;
  read.discard_data = 1;
  read.error = error;
  read.json = lonejson_new(NULL, &json_error);
  assert(read.json != NULL);
  sink.impl = &read;
  memset(bytes, 'x', sizeof(bytes));
  assert(collection_event_write(&sink, prefix, sizeof(prefix) - 1U, &local));
  for (total = 0U; total <= config.max_dynamic_string_bytes;
       total += sizeof(bytes))
    assert(collection_event_write(&sink, bytes, sizeof(bytes), &local));
  assert(collection_event_write(&sink, suffix, sizeof(suffix) - 1U, &local));
  metadata.kind = LC_COLLECTION_READ_METADATA_ENTRY;
  metadata.append_id = "commit-parser-fixture";
  metadata.position = 1U;
  assert(collection_event_boundary(&read, &metadata, &local) == LC_OK);
  assert(read.rc == CAI_OK && !read.limited && !read.initialized);
  collection_event_cleanup(&read);
  lonejson_free(read.json);
  lc_error_cleanup(&local);
}
