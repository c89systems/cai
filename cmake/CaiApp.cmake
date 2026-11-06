add_library(cai_app_objects OBJECT cli/app.c cli/input_socket.c cli/frontend.c cli/batch.c cli/batch_document.c cli/terminal_program.c cli/run_frontend.c cli/existing_frontend.c cli/utility.c cli/kanban.c)
target_link_libraries(cai_app_objects PRIVATE cai_cli_support)
cai_configure_c_target(cai_app_objects)
add_library(cai_app_static STATIC $<TARGET_OBJECTS:cai_app_objects>
  $<TARGET_OBJECTS:cai_app_client_objects>
  $<TARGET_OBJECTS:cai_cli_support>)
add_library(cai::app_static ALIAS cai_app_static)
set_target_properties(cai_app_static PROPERTIES OUTPUT_NAME cai_app EXPORT_NAME app_static)
target_include_directories(cai_app_static PUBLIC
  $<BUILD_INTERFACE:${CAI_PUBLIC_INCLUDE_DIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(cai_app_static PUBLIC cai_app_server_static cai_static softline::softline_static
  libmdf::mdf_static lockdc::static)
set(CAI_APP_LUA_TOOLS_PC "")
set(CAI_LUA_TOOLS_JSON false)
if(CAI_BUILD_LUA_TOOLS)
  set(CAI_LUA_TOOLS_JSON true)
  # Fixtures that compile app_run.c use the same executor selection as the app.
  target_compile_definitions(cai_cli_support PUBLIC CAI_APP_LUA_TOOLS=1)
  target_link_libraries(cai_cli_support PUBLIC cai_lua_tools_static)
  target_link_libraries(cai_app_static PUBLIC cai_lua_tools_static)
  set(CAI_APP_LUA_TOOLS_PC " cai-lua-tools")
endif()
if(CAI_BUILD_SHARED)
  find_package(lockdc CONFIG REQUIRED COMPONENTS shared)
  add_library(cai_app_shared SHARED $<TARGET_OBJECTS:cai_app_objects>
    $<TARGET_OBJECTS:cai_app_client_objects>
    $<TARGET_OBJECTS:cai_cli_support>)
  add_library(cai::app_shared ALIAS cai_app_shared)
  set_target_properties(cai_app_shared PROPERTIES OUTPUT_NAME cai_app EXPORT_NAME app_shared
    VERSION ${PROJECT_VERSION} SOVERSION 0)
  target_include_directories(cai_app_shared PUBLIC
    $<BUILD_INTERFACE:${CAI_PUBLIC_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_link_libraries(cai_app_shared PUBLIC cai_app_server_shared cai_shared pslog::pslog_shared PRIVATE
    softline::softline libmdf::mdf_shared lockdc::shared)
  if(CAI_BUILD_LUA_TOOLS)
    target_link_libraries(cai_app_shared PRIVATE cai_lua_tools_shared)
  endif()
  target_link_options(cai_app_shared PRIVATE -Wl,--no-undefined)
  cai_apply_export_policy(cai_app_shared "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cai_app.exports")
  cai_configure_c_target(cai_app_shared)
endif()
if(CAI_BUILD_TESTS)
  add_test(NAME cai_app_owner_boundary COMMAND "${CMAKE_COMMAND}"
    "-DCAI_NM=${CMAKE_NM}"
    "-DCAI_OWNER_OBJECT=$<TARGET_OBJECTS:cai_app_owner_objects>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_owner_boundary_test.cmake")
  set_tests_properties(cai_app_owner_boundary PROPERTIES LABELS "unit;local;offline" TIMEOUT 5)
  add_executable(cai_presenter_test tests/presenter_test.c)
  target_link_libraries(cai_presenter_test PRIVATE cai_cli_support)
  cai_configure_cli_target(cai_presenter_test)
  target_compile_options(cai_presenter_test PRIVATE -UNDEBUG)
  add_test(NAME cai_presenter COMMAND cai_presenter_test)
  set_tests_properties(cai_presenter PROPERTIES LABELS "unit;local;offline" TIMEOUT 5)
  add_executable(cai_plain_feed_test tests/plain_feed_test.c)
  target_link_libraries(cai_plain_feed_test PRIVATE cai_cli_support)
  cai_configure_cli_target(cai_plain_feed_test)
  target_compile_options(cai_plain_feed_test PRIVATE -UNDEBUG)
  add_test(NAME cai_plain_feed COMMAND cai_plain_feed_test "${CMAKE_CURRENT_BINARY_DIR}/test-tmp")
  set_tests_properties(cai_plain_feed PROPERTIES LABELS "unit;local;offline" TIMEOUT 5)
  add_executable(cai_app_existing_host tests/app_existing_host.c tests/cli_storage_fixture.c)
  target_link_libraries(cai_app_existing_host PRIVATE cai_app_static)
  target_link_options(cai_app_existing_host PRIVATE "-Wl,--wrap=lc_client_open")
  cai_configure_cli_target(cai_app_existing_host)
  target_compile_options(cai_app_existing_host PRIVATE -UNDEBUG)
  add_executable(cai_line_input_test tests/line_input_test.c cli/line_input.c cli/error.c)
  target_link_libraries(cai_line_input_test PRIVATE cai_static)
  cai_configure_cli_target(cai_line_input_test)
  target_compile_options(cai_line_input_test PRIVATE -UNDEBUG)
  add_test(NAME cai_line_input COMMAND cai_line_input_test "${CMAKE_CURRENT_BINARY_DIR}/test-tmp")
  set_tests_properties(cai_line_input PROPERTIES LABELS "unit;local;offline" TIMEOUT 5)
  add_executable(cai_batch_document_test tests/batch_document_test.c cli/batch_document.c cli/error.c)
  target_link_libraries(cai_batch_document_test PRIVATE cai_static)
  cai_configure_cli_target(cai_batch_document_test)
  target_compile_options(cai_batch_document_test PRIVATE -UNDEBUG)
  add_test(NAME cai_batch_document COMMAND cai_batch_document_test)
  set_tests_properties(cai_batch_document PROPERTIES LABELS "unit;local;offline" TIMEOUT 5)
  add_executable(cai_log_environment_test tests/log_environment_test.c cli/log_environment.c)
  target_link_libraries(cai_log_environment_test PRIVATE cai_static pslog::pslog_static)
  cai_configure_cli_target(cai_log_environment_test)
  target_compile_options(cai_log_environment_test PRIVATE -UNDEBUG)
  add_test(NAME cai_log_environment COMMAND cai_log_environment_test)
  set_tests_properties(cai_log_environment PROPERTIES LABELS "unit;local;offline" TIMEOUT 5)
  # Compile the protocol consumer with core only: neither SDK storage nor
  # terminal objects are part of this client's link boundary.
  add_executable(cai_catalog_wire_test tests/catalog_wire_test.c
    cli/catalog_wire.c cli/command_help.c cli/error.c)
  target_link_libraries(cai_catalog_wire_test PRIVATE cai_static)
  cai_configure_cli_target(cai_catalog_wire_test)
  target_compile_options(cai_catalog_wire_test PRIVATE -UNDEBUG)
  add_test(NAME cai_catalog_wire COMMAND cai_catalog_wire_test)
  set_tests_properties(cai_catalog_wire PROPERTIES LABELS "unit;local;offline" TIMEOUT 5)
  add_executable(cai_status_host_test tests/status_host_test.c)
  target_link_libraries(cai_status_host_test PRIVATE cai_app_static)
  target_link_options(cai_status_host_test PRIVATE -Wl,--wrap=lc_client_open)
  cai_configure_cli_target(cai_status_host_test)
  target_compile_options(cai_status_host_test PRIVATE -UNDEBUG)
  add_test(NAME cai_status_host COMMAND cai_status_host_test)
  set_tests_properties(cai_status_host PROPERTIES LABELS "unit;local;offline" TIMEOUT 5)
  add_executable(cai_status_wire_test tests/status_wire_test.c)
  target_link_libraries(cai_status_wire_test PRIVATE cai_app_static)
  cai_configure_cli_target(cai_status_wire_test)
  target_compile_options(cai_status_wire_test PRIVATE -UNDEBUG)
  add_test(NAME cai_status_wire COMMAND cai_status_wire_test)
  set_tests_properties(cai_status_wire PROPERTIES LABELS "unit;local;offline" TIMEOUT 5)
  add_executable(cai_transcript_test tests/transcript_test.c)
  target_link_libraries(cai_transcript_test PRIVATE cai_app_static)
  target_link_options(cai_transcript_test PRIVATE "-Wl,--wrap=malloc"
    "-Wl,--wrap=calloc" "-Wl,--wrap=realloc")
  cai_configure_cli_target(cai_transcript_test)
  target_compile_options(cai_transcript_test PRIVATE -UNDEBUG)
  add_test(NAME cai_transcript COMMAND cai_transcript_test)
  set_tests_properties(cai_transcript PROPERTIES LABELS "unit;local;offline" TIMEOUT 10)
  add_executable(cai_transcript_store_test tests/transcript_store_test.c tests/cli_storage_fixture.c)
  target_link_libraries(cai_transcript_store_test PRIVATE cai_app_static)
  target_link_options(cai_transcript_store_test PRIVATE "-Wl,--wrap=lc_client_open")
  cai_configure_cli_target(cai_transcript_store_test)
  target_compile_options(cai_transcript_store_test PRIVATE -UNDEBUG)
  add_test(NAME cai_transcript_store COMMAND bash
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/transcript_store_test.sh"
    "$<TARGET_FILE:cai_transcript_store_test>" "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties(cai_transcript_store PROPERTIES LABELS "cli;local;offline" TIMEOUT 30)
  add_executable(cai_transcript_wire_test tests/transcript_wire_test.c)
  target_link_libraries(cai_transcript_wire_test PRIVATE cai_app_static)
  cai_configure_cli_target(cai_transcript_wire_test)
  target_compile_options(cai_transcript_wire_test PRIVATE -UNDEBUG)
  add_test(NAME cai_transcript_wire COMMAND cai_transcript_wire_test)
  set_tests_properties(cai_transcript_wire PROPERTIES LABELS "unit;local;offline" TIMEOUT 5)
  add_executable(cai_app_terminal_host tests/app_terminal_host.c)
  target_link_libraries(cai_app_terminal_host PRIVATE cai_app_static)
  target_link_options(cai_app_terminal_host PRIVATE -Wl,--wrap=lc_client_open)
  cai_configure_cli_target(cai_app_terminal_host)
  add_executable(cai_app_utility_host tests/app_utility_host.c)
  target_link_libraries(cai_app_utility_host PRIVATE cai_app_static)
  target_link_options(cai_app_utility_host PRIVATE
    -Wl,--wrap=cai_agent_runtime_open -Wl,--wrap=cai_workspace_server_open
    -Wl,--wrap=cai_client_open
    -Wl,--wrap=cai_agent_runtime_pump -Wl,--wrap=cai_agent_runtime_pump_bounded
    -Wl,--wrap=sl_create_with_config -Wl,--wrap=mdf_create
    -Wl,--wrap=setenv -Wl,--wrap=unsetenv -Wl,--wrap=clearenv
    -Wl,--wrap=setlocale -Wl,--wrap=sigaction
    -Wl,--wrap=chdir -Wl,--wrap=fchdir
    -Wl,--wrap=cai_cli_pouch_open_options)
  target_compile_options(cai_app_utility_host PRIVATE -UNDEBUG)
  cai_configure_cli_target(cai_app_utility_host)
  add_executable(cai_input_socket_test tests/input_socket_test.c)
  target_link_libraries(cai_input_socket_test PRIVATE cai_app_static)
  target_link_options(cai_input_socket_test PRIVATE -Wl,--wrap=lc_client_open)
  cai_configure_cli_target(cai_input_socket_test)
  target_compile_options(cai_input_socket_test PRIVATE -UNDEBUG)
  add_test(NAME cai_input_socket COMMAND cai_input_socket_test)
  set_tests_properties(cai_input_socket PROPERTIES LABELS "unit;local;offline" TIMEOUT 10)
  add_executable(cai_input_wire_test tests/input_wire_test.c)
  target_link_libraries(cai_input_wire_test PRIVATE cai_app_static)
  cai_configure_cli_target(cai_input_wire_test)
  target_compile_options(cai_input_wire_test PRIVATE -UNDEBUG)
  add_test(NAME cai_input_wire COMMAND cai_input_wire_test)
  set_tests_properties(cai_input_wire PROPERTIES LABELS "unit;local;offline" TIMEOUT 5)
  add_executable(cai_app_controller_test tests/app_controller_test.c tests/cli_storage_fixture.c)
  target_link_libraries(cai_app_controller_test PRIVATE cai_app_static)
  target_link_options(cai_app_controller_test PRIVATE "-Wl,--wrap=lc_client_open"
    "-Wl,--wrap=mdf_create" "-Wl,--wrap=sl_create_with_config")
  cai_configure_cli_target(cai_app_controller_test)
  target_compile_options(cai_app_controller_test PRIVATE -UNDEBUG)
  add_test(NAME cai_app_controller COMMAND bash
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_controller_test.sh"
    "$<TARGET_FILE:cai_app_controller_test>")
  set_tests_properties(cai_app_controller PROPERTIES LABELS "cli;local;offline" TIMEOUT 20)
  add_test(NAME cai_app_workspace_server COMMAND bash
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_workspace_server_test.sh"
    "$<TARGET_FILE:cai_app_controller_test>")
  set_tests_properties(cai_app_workspace_server PROPERTIES LABELS "cli;local;offline" TIMEOUT 20)
  add_executable(cai_app_mcp_environment_test tests/mcp_environment_test.c)
  target_link_libraries(cai_app_mcp_environment_test PRIVATE cai_cli_support cai_app_static)
  target_link_options(cai_app_mcp_environment_test PRIVATE "-Wl,--wrap=cai_mcp_streamable_http_client_open")
  cai_configure_cli_target(cai_app_mcp_environment_test)
  target_compile_options(cai_app_mcp_environment_test PRIVATE -UNDEBUG)
  add_test(NAME cai_app_mcp_environment COMMAND cai_app_mcp_environment_test "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties(cai_app_mcp_environment PROPERTIES LABELS "unit;local;offline" TIMEOUT 5)
  add_executable(cai_app_home_test tests/app_home_test.c)
  target_link_libraries(cai_app_home_test PRIVATE cai_cli_support cai_app_static)
  cai_configure_cli_target(cai_app_home_test)
  target_compile_options(cai_app_home_test PRIVATE -UNDEBUG)
  add_test(NAME cai_app_home COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_home_test.sh"
    "$<TARGET_FILE:cai_app_home_test>" "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties(cai_app_home PROPERTIES LABELS "cli;local;offline" TIMEOUT 10)
  add_executable(cai_app_identity_test tests/app_identity_test.c)
  target_sources(cai_app_identity_test PRIVATE tests/cli_storage_fixture.c)
  target_link_options(cai_app_identity_test PRIVATE "-Wl,--wrap=lc_client_open")
  target_link_libraries(cai_app_identity_test PRIVATE cai_cli_support cai_app_static)
  cai_configure_cli_target(cai_app_identity_test)
  target_compile_options(cai_app_identity_test PRIVATE -UNDEBUG)
  add_test(NAME cai_app_identity COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_identity_test.sh"
    "$<TARGET_FILE:cai_app_identity_test>" "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties(cai_app_identity PROPERTIES LABELS "cli;local;offline" TIMEOUT 10)
  add_executable(cai_kanban_test tests/kanban_test.c)
  target_sources(cai_kanban_test PRIVATE tests/cli_storage_fixture.c)
  target_link_options(cai_kanban_test PRIVATE "-Wl,--wrap=lc_client_open")
  target_link_libraries(cai_kanban_test PRIVATE cai_app_static)
  target_include_directories(cai_kanban_test PRIVATE "${CAI_PSLOG_INCLUDE_DIR}")
  cai_configure_cli_target(cai_kanban_test)
  target_compile_options(cai_kanban_test PRIVATE -UNDEBUG)
  add_test(NAME cai_app_kanban COMMAND bash
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/kanban_test.sh"
    "$<TARGET_FILE:cai_kanban_test>" "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties(cai_app_kanban PROPERTIES LABELS "cli;local;offline" TIMEOUT 15)
  add_executable(cai_app_stop_test tests/app_stop_test.c)
  target_link_libraries(cai_app_stop_test PRIVATE cai_app_static)
  cai_configure_cli_target(cai_app_stop_test)
  target_compile_options(cai_app_stop_test PRIVATE -UNDEBUG)
  target_link_options(cai_app_stop_test PRIVATE -Wl,--wrap=calloc -Wl,--wrap=strdup)
  add_test(NAME cai_app_stop COMMAND cai_app_stop_test)
  set_tests_properties(cai_app_stop PROPERTIES LABELS "unit;local;offline" TIMEOUT 5)
  add_executable(cai_app_test tests/app_test.c)
  if(TARGET cai_app_shared AND NOT CAI_TARGET_ID MATCHES "-linux-musl$")
    target_link_libraries(cai_app_test PRIVATE cai_app_shared)
  else()
    target_link_libraries(cai_app_test PRIVATE cai_app_static)
  endif()
  cai_configure_cli_target(cai_app_test)
  target_include_directories(cai_app_test PRIVATE "${CAI_PSLOG_INCLUDE_DIR}")
  target_compile_options(cai_app_test PRIVATE -UNDEBUG)
  add_test(NAME cai_app COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_test.sh"
    "$<TARGET_FILE:cai_app_test>" "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties(cai_app PROPERTIES LABELS "cli;local;offline" TIMEOUT 15)
  add_executable(cai_app_status_test tests/app_status_test.c)
  target_link_libraries(cai_app_status_test PRIVATE cai_cli_support cai_app_static)
  cai_configure_cli_target(cai_app_status_test)
  target_compile_options(cai_app_status_test PRIVATE -UNDEBUG)
  add_test(NAME cai_app_status COMMAND cai_app_status_test "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties(cai_app_status PROPERTIES LABELS "cli;local;offline" TIMEOUT 3)
  add_executable(cai_app_host tests/app_host.c)
  target_link_libraries(cai_app_host PRIVATE cai_app_static)
  cai_configure_cli_target(cai_app_host)
  target_compile_options(cai_app_host PRIVATE -UNDEBUG)
  add_executable(cai_app_worker_stack_host tests/app_worker_stack_host.c)
  target_link_libraries(cai_app_worker_stack_host PRIVATE cai_app_static)
  cai_configure_cli_target(cai_app_worker_stack_host)
  add_executable(cai_app_run_terminal_host tests/app_run_terminal_host.c tests/cli_storage_fixture.c)
  target_link_libraries(cai_app_run_terminal_host PRIVATE cai_app_static)
  target_link_options(cai_app_run_terminal_host PRIVATE -Wl,--wrap=lc_client_open)
  target_compile_options(cai_app_run_terminal_host PRIVATE -UNDEBUG)
  cai_configure_cli_target(cai_app_run_terminal_host)
  if(CAI_BUILD_CLI)
    add_test(NAME cai_app_run_batch COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_run_batch_test.sh"
      "$<TARGET_FILE:cai_app_run_terminal_host>")
    set_tests_properties(cai_app_run_batch PROPERTIES LABELS "cli;local;offline" TIMEOUT 20)
    add_test(NAME cai_app_run_terminal COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_run_terminal_test.sh"
      "$<TARGET_FILE:cai_app_run_terminal_host>")
    set_tests_properties(cai_app_run_terminal PROPERTIES LABELS "cli;local;offline" TIMEOUT 30
      ENVIRONMENT "CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
    add_test(NAME cai_app_worker_stack COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_worker_stack_test.sh"
      "$<TARGET_FILE:cai_app_worker_stack_host>")
    set_tests_properties(cai_app_worker_stack PROPERTIES
      LABELS "cli;local;offline" TIMEOUT 15
      ENVIRONMENT "CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
  endif()
  add_executable(cai_app_controls_host tests/app_controls_host.c)
  target_sources(cai_app_controls_host PRIVATE tests/cli_storage_fixture.c)
  target_link_options(cai_app_controls_host PRIVATE "-Wl,--wrap=lc_client_open"
    "-Wl,--wrap=lc_enqueue")
  target_link_libraries(cai_app_controls_host PRIVATE cai_app_static)
  cai_configure_cli_target(cai_app_controls_host)
  target_compile_options(cai_app_controls_host PRIVATE -UNDEBUG)
  add_executable(cai_app_startup_goal_test tests/app_startup_goal_test.c
    tests/cli_storage_fixture.c)
  target_link_options(cai_app_startup_goal_test PRIVATE "-Wl,--wrap=lc_client_open")
  target_link_libraries(cai_app_startup_goal_test PRIVATE cai_app_static)
  cai_configure_cli_target(cai_app_startup_goal_test)
  target_compile_options(cai_app_startup_goal_test PRIVATE -UNDEBUG)
  add_test(NAME cai_app_startup_goal COMMAND bash
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_startup_goal_test.sh"
    "$<TARGET_FILE:cai_app_startup_goal_test>")
  set_tests_properties(cai_app_startup_goal PROPERTIES LABELS "cli;local;offline" TIMEOUT 15)
  add_executable(cai_app_utility_stop_test tests/app_utility_stop_test.c tests/cli_storage_fixture.c)
  target_link_libraries(cai_app_utility_stop_test PRIVATE cai_app_static)
  cai_configure_cli_target(cai_app_utility_stop_test)
  target_compile_options(cai_app_utility_stop_test PRIVATE -UNDEBUG)
  target_link_options(cai_app_utility_stop_test PRIVATE "-Wl,--wrap=poll"
    "-Wl,--wrap=lc_enqueue" "-Wl,--wrap=lc_client_open"
    "-Wl,--wrap=cai_cli_pouch_open_options")
  add_test(NAME cai_app_utility_stop COMMAND bash
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_utility_stop_test.sh"
    "$<TARGET_FILE:cai_app_utility_stop_test>")
  set_tests_properties(cai_app_utility_stop PROPERTIES LABELS "cli;local;offline" TIMEOUT 5)
  if(CAI_BUILD_CLI)
    add_test(NAME cai_app_home_host COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_home_test.sh" "$<TARGET_FILE:cai_app_host>" host)
    set_tests_properties(cai_app_home_host PROPERTIES LABELS "cli;local;offline" TIMEOUT 15
      ENVIRONMENT "CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
    add_test(NAME cai_app_identity_cli COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_identity_cli_test.sh" "$<TARGET_FILE:cai_app_host>")
    set_tests_properties(cai_app_identity_cli PROPERTIES LABELS "cli;local;offline" TIMEOUT 15
      ENVIRONMENT "CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
    foreach(_control_case batch turns interactive)
      add_test(NAME cai_app_controls_${_control_case} COMMAND bash
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_controls_test.sh"
        "$<TARGET_FILE:cai_app_controls_host>" "${_control_case}")
      set_tests_properties(cai_app_controls_${_control_case} PROPERTIES
        LABELS "cli;local;offline" TIMEOUT 15
        ENVIRONMENT "CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
    endforeach()
    add_test(NAME cai_app_control_edges COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_controls_test.sh" "$<TARGET_FILE:cai_app_controls_host>" edges)
    set_tests_properties(cai_app_control_edges PROPERTIES LABELS "cli;local;offline" TIMEOUT 10
      ENVIRONMENT "CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
    add_test(NAME cai_app_input_boundaries COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_controls_test.sh"
      "$<TARGET_FILE:cai_app_controls_host>" boundaries)
    set_tests_properties(cai_app_input_boundaries PROPERTIES LABELS "cli;local;offline" TIMEOUT 10
      ENVIRONMENT "CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
    add_test(NAME cai_app_shell_stop COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_shell_stop_test.sh"
      "$<TARGET_FILE:cai_app_controls_host>")
    set_tests_properties(cai_app_shell_stop PROPERTIES LABELS "cli;local;offline" TIMEOUT 5
      ENVIRONMENT "CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
    foreach(action cancel stop)
      add_test(NAME cai_app_message_${action} COMMAND bash
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_controls_test.sh"
        "$<TARGET_FILE:cai_app_controls_host>" message_${action})
      set_tests_properties(cai_app_message_${action} PROPERTIES
        LABELS "cli;local;offline" TIMEOUT 3)
    endforeach()
    add_test(NAME cai_app_custom_tools COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_custom_tools_test.sh" "$<TARGET_FILE:cai_app_host>")
    set_tests_properties(cai_app_custom_tools PROPERTIES LABELS "cli;local;offline" TIMEOUT 10)
    add_test(NAME cai_app_embedding COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_embedding_test.sh" "$<TARGET_FILE:cai_app_host>")
    set_tests_properties(cai_app_embedding PROPERTIES LABELS "cli;local;offline" TIMEOUT 15
      ENVIRONMENT "CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
    add_test(NAME cai_app_goal_pursuit COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_goal_pursuit_test.sh"
      "$<TARGET_FILE:cai_app_host>" responses batch basic)
    set_tests_properties(cai_app_goal_pursuit PROPERTIES LABELS "cli;local;offline" TIMEOUT 15)
    add_test(NAME cai_app_themes COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_themes_test.sh"
      "$<TARGET_FILE:cai_app_host>" host)
    set_tests_properties(cai_app_themes PROPERTIES LABELS "cli;local;offline" TIMEOUT 15
      ENVIRONMENT "CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
    add_test(NAME cai_app_temp COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_temp_test.sh"
      "$<TARGET_FILE:cai_app_host>" "$<TARGET_FILE:cai_app_controls_host>" host)
    set_tests_properties(cai_app_temp PROPERTIES LABELS "cli;local;offline" TIMEOUT 30
      ENVIRONMENT "CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
    add_test(NAME cai_app_replay_tail COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_replay_tail_test.sh"
      "$<TARGET_FILE:cai_app_host>" host)
    set_tests_properties(cai_app_replay_tail PROPERTIES LABELS "cli;local;offline" TIMEOUT 15
      ENVIRONMENT "CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
  endif()
  if(TARGET cai_app_shared)
    add_test(NAME cai_app_exports COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/shared_export_policy_test.sh"
      "${CMAKE_NM}" "$<TARGET_FILE:cai_app_shared>"
      "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cai_app.exports" linux)
    set_tests_properties(cai_app_exports PROPERTIES LABELS "cli;local;offline" TIMEOUT 5)
  endif()
  if(CAI_INSTALL)
    set(_app_lua_runner "")
    if(CAI_BUILD_LUA)
      set(_app_lua_runner "$<TARGET_FILE:cai_lua_runner>")
    endif()
    set(_app_prefixes "${CAI_CPKT_PREFIX};${CAI_CPKTMISC_PREFIX};${CAI_LONEJSON_PREFIX};${_cai_softline_prefix};${_cai_libmdf_prefix};${_cai_lockdc_prefix};${_cai_liblql_prefix};${CAI_DEPS_DIR}/libpslog-${CAI_PSLOG_VERSION}-${CAI_TARGET_ID}")
    if(CAI_BUILD_LUA_TOOLS)
      add_test(NAME cai_plugins_install COMMAND "${CMAKE_COMMAND}"
        "-DCAI_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}" "-DCAI_C_COMPILER=${CMAKE_C_COMPILER}"
        "-DCAI_DEPENDENCY_PREFIXES=${_app_prefixes}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/plugins_install_test.cmake")
      set_tests_properties(cai_plugins_install PROPERTIES LABELS "cli;local;offline;packaging" TIMEOUT 30)
    endif()
    add_test(NAME cai_app_install COMMAND "${CMAKE_COMMAND}"
      "-DCAI_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}" "-DCAI_C_COMPILER=${CMAKE_C_COMPILER}"
      "-DCAI_DEPENDENCY_PREFIXES=${_app_prefixes}"
      "-DCAI_LUA_RUNNER=${_app_lua_runner}"
      "-DCAI_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DCAI_LUA_INCLUDE_DIR=${CAI_LUA_INCLUDE_DIR}"
      "-DCAI_PSLOG_LUA_INCLUDE_DIR=${CAI_PSLOG_LUA_INCLUDE_DIR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_install_test.cmake")
    set_tests_properties(cai_app_install PROPERTIES LABELS "cli;local;offline;packaging" TIMEOUT 30)
  endif()
endif()
if(CAI_INSTALL)
  install(TARGETS cai_app_static EXPORT caiAppTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT cai-app)
  if(TARGET cai_app_shared)
    install(TARGETS cai_app_shared EXPORT caiAppTargets
      LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT cai-app)
  endif()
  install(FILES "${CAI_PUBLIC_INCLUDE_DIR}/cai/app.h"
    "${CAI_PUBLIC_INCLUDE_DIR}/cai/app_types.h"
    "${CAI_PUBLIC_INCLUDE_DIR}/cai/app_server.h"
    "${CAI_PUBLIC_INCLUDE_DIR}/cai/app_client.h"
    "${CAI_PUBLIC_INCLUDE_DIR}/cai/app_terminal.h"
    "${CAI_PUBLIC_INCLUDE_DIR}/cai/app_view.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/cai" COMPONENT cai-app)
  install(EXPORT caiAppTargets NAMESPACE cai:: FILE cai-app-targets.cmake
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cai_app" COMPONENT cai-app)
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/cai_app-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cai_app-config.cmake" @ONLY)
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/cai-config-version.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cai_app-config-version.cmake" @ONLY)
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/cai-app.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cai-app.pc" @ONLY)
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/cai_app-dependencies.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/app-dependencies.json" @ONLY)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/app-dependencies.json"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/cai_app" RENAME dependencies.json COMPONENT cai-app)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/cai_app-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/cai_app-config-version.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cai_app" COMPONENT cai-app)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/cai-app.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig" COMPONENT cai-app)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/embedding.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/agent-messaging.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/kanban.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/libcai_app" COMPONENT cai-app)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/examples/app.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/app.lua"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/libcai_app/examples" COMPONENT cai-app)
endif()
