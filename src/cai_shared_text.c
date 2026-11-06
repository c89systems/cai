#include "cai_shared_text.h"
#include "cai_internal.h"
#include <string.h>

int cai_shared_text_adopt(const cai_allocator *allocator, char *text,
                          cai_shared_text **out, cai_error *error) {
  cai_shared_text *value;
  *out = NULL;
  if (text == NULL)
    return CAI_OK;
  value = cai_alloc(allocator, sizeof(*value));
  if (value == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "allocate immutable text owner");
  memset(value, 0, sizeof(*value));
  if (allocator != NULL)
    value->allocator = *allocator;
  if (pthread_mutex_init(&value->lock, NULL) != 0) {
    cai_free_mem(allocator, value);
    return cai_set_error(error, CAI_ERR_TRANSPORT, "initialize text ownership");
  }
  value->references = 1U;
  value->data = text;
  value->length = strlen(text);
  *out = value;
  return CAI_OK;
}
int cai_shared_text_create(const cai_allocator *allocator, const char *text,
                           cai_shared_text **out, cai_error *error) {
  char *copy;
  int rc;
  *out = NULL;
  if (text == NULL)
    return CAI_OK;
  copy = cai_strdup(allocator, text);
  if (copy == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "copy immutable text");
  rc = cai_shared_text_adopt(allocator, copy, out, error);
  if (rc != CAI_OK)
    cai_free_mem(allocator, copy);
  return rc;
}
void cai_shared_text_retain(cai_shared_text *text) {
  if (text != NULL) {
    pthread_mutex_lock(&text->lock);
    ++text->references;
    pthread_mutex_unlock(&text->lock);
  }
}
void cai_shared_text_release(cai_shared_text *text) {
  int last;
  cai_allocator allocator;
  if (text == NULL)
    return;
  pthread_mutex_lock(&text->lock);
  last = --text->references == 0U;
  pthread_mutex_unlock(&text->lock);
  if (last) {
    allocator = text->allocator;
    pthread_mutex_destroy(&text->lock);
    cai_free_mem(&allocator, text->data);
    cai_free_mem(&allocator, text);
  }
}
