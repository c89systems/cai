#include "../cli/input_adapter.h"
#include <assert.h>
#include <pty.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
  cai_input_queue_config qc;
  cai_input_queue *first = NULL, *second = NULL;
  cai_input_controller *controller = NULL;
  cai_cli_input_adapter *adapter = NULL;
  cai_input_state state;
  cai_input_entry input;
  cai_input_claim claim;
  sl_config_t config;
  sl_queue_request_t request;
  sl_queue_request_id_t id;
  sl_queue_entry_t entry;
  sl_queue_backend_info_t info;
  sl_queue_pending_info_t pending;
  sl_t *editor;
  int descriptors[2];
  size_t i;
  char *taken = NULL;
  cai_input_queue_config_init(&qc);
  assert(cai_input_queue_open(&qc, &first, NULL) == CAI_OK);
  assert(cai_input_queue_open(&qc, &second, NULL) == CAI_OK);
  assert(cai_input_controller_open(NULL, &controller, NULL) == CAI_OK);
  assert(cai_input_controller_select(controller, first, NULL) == CAI_OK);
  assert(cai_cli_input_local_open(controller, &adapter, NULL) == CAI_OK);
  assert(openpty(&descriptors[0], &descriptors[1], NULL, NULL, NULL) == 0);
  sl_config_init(&config);
  config.input_fd = descriptors[1];
  config.output_fd = descriptors[1];
  cai_cli_input_adapter_configure(adapter, &config);
  editor = sl_create_with_config(&config);
  assert(editor != NULL &&
         cai_cli_input_adapter_bind(adapter, editor, NULL) == CAI_OK);
  assert(cai_input_controller_pump(controller, 32U, NULL) == CAI_OK);
  assert(cai_cli_input_adapter_receive(adapter, 64U, NULL) == CAI_OK);
  assert(sl_queue_backend_info(editor, &info) == SL_OK && info.synchronized &&
         !info.visible_count);
  memset(&request, 0, sizeof(request));
  request.operation = SL_QUEUE_OFFER;
  request.mode = SL_PROMPT_QUEUE_MODE_STEER;
  request.text = "steer owned by the conversation";
  assert(sl_queue_backend_request(editor, &request, &id) == SL_OK);
  assert(sl_queue_backend_pending(editor, 0U, &pending) == SL_OK &&
         pending.pending);
  assert(cai_input_queue_get_state(first, &state, NULL) == CAI_OK &&
         !state.count);
  assert(cai_input_controller_pump(controller, 32U, NULL) == CAI_OK);
  assert(cai_cli_input_adapter_receive(adapter, 64U, NULL) == CAI_OK);
  assert(sl_queue_backend_info(editor, &info) == SL_OK &&
         info.visible_count == 1U && !info.pending_count);
  assert(sl_queue_backend_entry(editor, 0U, &entry) == SL_OK &&
         entry.mode == SL_PROMPT_QUEUE_MODE_STEER && !entry.ready);
  assert(sl_prompt_queue_take(editor, 0U, &taken) == SL_ERROR_UNSUPPORTED &&
         taken == NULL);
  memset(&request, 0, sizeof(request));
  request.operation = SL_QUEUE_BEGIN_EDIT;
  request.item_id = entry.id;
  request.item_revision = entry.revision;
  assert(sl_queue_backend_request(editor, &request, &id) == SL_OK);
  assert(cai_input_controller_pump(controller, 32U, NULL) == CAI_OK);
  assert(cai_cli_input_adapter_receive(adapter, 64U, NULL) == CAI_OK);
  assert(sl_queue_backend_entry(editor, 0U, &entry) == SL_OK &&
         entry.edit_held && entry.edit_token != NULL);
  memset(&request, 0, sizeof(request));
  request.operation = SL_QUEUE_END_EDIT;
  request.item_id = entry.id;
  request.item_revision = entry.revision;
  request.edit_token = entry.edit_token;
  request.text = "replacement accepted by backend";
  assert(sl_queue_backend_request(editor, &request, &id) == SL_OK);
  assert(cai_input_controller_pump(controller, 32U, NULL) == CAI_OK);
  assert(cai_cli_input_adapter_receive(adapter, 64U, NULL) == CAI_OK);
  assert(sl_queue_backend_entry(editor, 0U, &entry) == SL_OK &&
         !entry.edit_held && strcmp(entry.text, request.text) == 0);
  assert(cai_input_queue_lookup(first, 1U, &input, NULL) == CAI_OK);
  assert(sl_set_buffer(editor, "independent local draft") == SL_OK);
  for (i = 0U; i < 400U; ++i)
    assert(cai_input_queue_set_ready(first, input.id, (int)(i & 1U), NULL) ==
           CAI_OK);
  assert(cai_cli_input_adapter_receive(adapter, 512U, NULL) == CAI_OK);
  assert(sl_queue_backend_info(editor, &info) == SL_OK && !info.synchronized);
  memset(&request, 0, sizeof(request));
  request.operation = SL_QUEUE_SYNCHRONIZE;
  assert(sl_queue_backend_request(editor, &request, &id) == SL_OK);
  assert(cai_input_controller_pump(controller, 32U, NULL) == CAI_OK);
  assert(cai_cli_input_adapter_receive(adapter, 64U, NULL) == CAI_OK);
  assert(sl_queue_backend_info(editor, &info) == SL_OK && info.synchronized &&
         info.visible_count == 1U);
  assert(strcmp(sl_buffer(editor), "independent local draft") == 0);
  assert(cai_input_queue_set_ready(first, input.id, 1, NULL) == CAI_OK);
  assert(cai_input_queue_claim(first, input.id, input.revision, &claim, NULL) ==
         CAI_OK);
  cai_input_claim_cleanup(&claim);
  assert(cai_cli_input_adapter_receive(adapter, 64U, NULL) == CAI_OK);
  assert(sl_queue_backend_info(editor, &info) == SL_OK && !info.visible_count);
  assert(strcmp(sl_buffer(editor), "independent local draft") == 0);
  memset(&request, 0, sizeof(request));
  request.operation = SL_QUEUE_OFFER;
  request.text = "pending transport at context switch";
  assert(sl_queue_backend_request(editor, &request, &id) == SL_OK);
  assert(cai_input_controller_select(controller, second, NULL) == CAI_OK);
  assert(cai_input_controller_pump(controller, 32U, NULL) == CAI_OK);
  assert(cai_cli_input_adapter_receive(adapter, 64U, NULL) == CAI_OK);
  assert(sl_queue_backend_info(editor, &info) == SL_OK && info.synchronized &&
         !info.visible_count);
  assert(sl_queue_backend_pending(editor, 0U, &pending) == SL_OK &&
         !pending.pending && pending.status == SL_QUEUE_REJECTED);
  assert(cai_input_queue_get_state(first, &state, NULL) == CAI_OK &&
         !state.count);
  assert(cai_input_queue_get_state(second, &state, NULL) == CAI_OK &&
         !state.count);
  sl_destroy(editor);
  cai_cli_input_adapter_close(adapter);
  cai_input_controller_close(controller);
  cai_input_queue_close(first);
  cai_input_queue_close(second);
  close(descriptors[0]);
  close(descriptors[1]);
  return 0;
}
