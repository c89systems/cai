# One backend implementation and one store registry for both public products.
if(NOT CAI_BUILD_APP_SERVER)
  return()
endif()
find_package(lockdc CONFIG REQUIRED COMPONENTS static)
add_library(cai_app_owner_objects OBJECT cli/app_run.c)
add_library(cai_app_server_objects OBJECT cli/server.c cli/identity.c cli/app_control.c
  cli/workspace_host.c cli/commands.c cli/options.c cli/help.c cli/config.c
  cli/runtime_options.c cli/instructions.c cli/mcp.c cli/skills.c cli/status.c cli/shell.c cli/shell_text.c
  cli/startup.c cli/goal_input.c cli/command_help.c cli/review_report.c cli/pouch.c
  cli/pouch_stream.c cli/pouch_transcript.c cli/transcript.c cli/transcript_feed.c
  cli/journal_projection.c
  cli/markdown_export.c
  cli/export_job.c
  cli/quota_refresh.c cli/session_commands.c cli/workspace_claims.c cli/log.c
  cli/log_environment.c cli/documents.c cli/messages.c cli/message_names.c
  cli/todo.c cli/stores.c cli/temp.c cli/replay.c)
foreach(target cai_app_owner_objects cai_app_server_objects)
  target_link_libraries(${target} PRIVATE ${_cai_internal_link_target} lockdc::static pslog::pslog_static)
  if(CAI_BUILD_LUA_TOOLS)
    target_compile_definitions(${target} PRIVATE CAI_APP_LUA_TOOLS=1)
    if(TARGET cai_lua_tools_static)
      target_link_libraries(${target} PRIVATE cai_lua_tools_static)
    else()
      target_link_libraries(${target} PRIVATE cai_lua_tools_shared)
    endif()
  endif()
  cai_configure_c_target(${target})
endforeach()
if(CAI_BUILD_STATIC)
  add_library(cai_app_server_static STATIC $<TARGET_OBJECTS:cai_app_owner_objects>
    $<TARGET_OBJECTS:cai_app_server_objects> $<TARGET_OBJECTS:cai_app_wire_objects>)
  add_library(cai::app_server_static ALIAS cai_app_server_static)
  set_target_properties(cai_app_server_static PROPERTIES
    OUTPUT_NAME cai_app_server EXPORT_NAME app_server_static)
  target_link_libraries(cai_app_server_static PUBLIC cai_static lockdc::static pslog::pslog_static)
  if(CAI_BUILD_LUA_TOOLS)
    target_link_libraries(cai_app_server_static PUBLIC cai_lua_tools_static)
  endif()
endif()
# Private services are shared by the in-project convenience facade, never
# installed as SDK declarations. Both DSOs use this same Pouch client registry.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cai_app_server.exports"
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cai_app_server_private.exports")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cai_app_server.exports" _server_public)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cai_app_server_private.exports" _server_private)
set(_server_symbols ${_server_public} ${_server_private})
list(REMOVE_DUPLICATES _server_symbols)
list(SORT _server_symbols)
string(JOIN "\n" _server_exports ${_server_symbols})
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/cai-app-server.exports" "${_server_exports}\n")
if(CAI_BUILD_SHARED)
  find_package(lockdc CONFIG REQUIRED COMPONENTS shared)
  add_library(cai_app_server_shared SHARED $<TARGET_OBJECTS:cai_app_owner_objects>
    $<TARGET_OBJECTS:cai_app_server_objects> $<TARGET_OBJECTS:cai_app_wire_objects>)
  add_library(cai::app_server_shared ALIAS cai_app_server_shared)
  set_target_properties(cai_app_server_shared PROPERTIES OUTPUT_NAME cai_app_server
    EXPORT_NAME app_server_shared VERSION ${PROJECT_VERSION} SOVERSION 0)
  target_link_libraries(cai_app_server_shared PUBLIC cai_shared pslog::pslog_shared PRIVATE lockdc::shared)
  if(CAI_BUILD_LUA_TOOLS)
    target_link_libraries(cai_app_server_shared PRIVATE cai_lua_tools_shared)
  endif()
  target_link_options(cai_app_server_shared PRIVATE -Wl,--no-undefined)
  cai_apply_export_policy(cai_app_server_shared "${CMAKE_CURRENT_BINARY_DIR}/cai-app-server.exports")
  cai_configure_c_target(cai_app_server_shared)
endif()
foreach(target cai_app_server_static cai_app_server_shared)
  if(TARGET ${target})
    target_include_directories(${target} PUBLIC
      $<BUILD_INTERFACE:${CAI_PUBLIC_INCLUDE_DIR}>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  endif()
endforeach()

if(CAI_INSTALL)
  if(TARGET cai_app_server_static)
    install(TARGETS cai_app_server_static EXPORT caiAppServerTargets
      ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT cai-app-server)
  endif()
  if(TARGET cai_app_server_shared)
    install(TARGETS cai_app_server_shared EXPORT caiAppServerTargets
      LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT cai-app-server)
  endif()
  install(EXPORT caiAppServerTargets NAMESPACE cai:: FILE cai-app-server-targets.cmake
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cai_app_server" COMPONENT cai-app-server)
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/cai_app_server-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cai_app_server-config.cmake" @ONLY)
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/cai-config-version.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cai_app_server-config-version.cmake" @ONLY)
  set(CAI_APP_SERVER_LUA_PC "")
  set(CAI_APP_SERVER_LUA_JSON false)
  if(CAI_BUILD_LUA_TOOLS)
    set(CAI_APP_SERVER_LUA_PC " cai-lua-tools")
    set(CAI_APP_SERVER_LUA_JSON true)
  endif()
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/cai_app_server-dependencies.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/server-dependencies.json" @ONLY)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/server-dependencies.json"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/cai_app_server" RENAME dependencies.json COMPONENT cai-app-server)
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/cai-app-server.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cai-app-server.pc" @ONLY)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/cai_app_server-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/cai_app_server-config-version.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cai_app_server" COMPONENT cai-app-server)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/cai-app-server.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig" COMPONENT cai-app-server)
  install(FILES "${CAI_PUBLIC_INCLUDE_DIR}/cai/app_server.h"
    "${CAI_PUBLIC_INCLUDE_DIR}/cai/app_types.h" "${CAI_PUBLIC_INCLUDE_DIR}/cai/app_view.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/cai" COMPONENT cai-app-server)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/application-server.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/libcai_app_server" COMPONENT cai-app-server)
endif()
if(CAI_BUILD_TESTS)
  add_test(NAME cai_app_server_boundary COMMAND "${CMAKE_COMMAND}"
    "-DCAI_NM=${CMAKE_NM}"
    "-DCAI_SERVER_OBJECTS=$<TARGET_OBJECTS:cai_app_owner_objects>;$<TARGET_OBJECTS:cai_app_server_objects>;$<TARGET_OBJECTS:cai_app_wire_objects>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_server_boundary_test.cmake")
  set_tests_properties(cai_app_server_boundary PROPERTIES LABELS "unit;local;offline" TIMEOUT 5)
  if(CAI_INSTALL)
    add_test(NAME cai_app_server_configuration COMMAND "${CMAKE_COMMAND}"
      "-DCAI_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
      "-DCAI_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DCAI_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}" "-DCAI_TARGET_ID=${CAI_TARGET_ID}"
      "-DCAI_DEPS_DIR=${CAI_DEPS_DIR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_server_configuration_test.cmake")
    set_tests_properties(cai_app_server_configuration PROPERTIES LABELS "local;offline;packaging" TIMEOUT 30)
    if(NOT CAI_BUILD_LUA_TOOLS)
      add_test(NAME cai_app_server_install COMMAND "${CMAKE_COMMAND}"
        "-DCAI_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}" "-DCAI_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DCAI_C_COMPILER=${CMAKE_C_COMPILER}"
        "-DCAI_DEPENDENCY_PREFIXES=${CAI_CPKT_PREFIX};${CAI_CPKTMISC_PREFIX};${CAI_LONEJSON_PREFIX};${CAI_PSLOG_PREFIX};${_cai_lockdc_prefix};${_cai_liblql_prefix}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_server_install_test.cmake")
      set_tests_properties(cai_app_server_install PROPERTIES LABELS "local;offline;packaging" TIMEOUT 30)
    endif()
  endif()
  if(TARGET cai_app_server_shared)
    add_test(NAME cai_app_server_exports COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/shared_export_policy_test.sh" "${CMAKE_NM}"
      "$<TARGET_FILE:cai_app_server_shared>" "${CMAKE_CURRENT_BINARY_DIR}/cai-app-server.exports" linux)
    set_tests_properties(cai_app_server_exports PROPERTIES LABELS "unit;local;offline" TIMEOUT 5)
  endif()
  add_custom_target(cai_app_server_tests)
  if(TARGET cai_app_server_static)
    add_executable(cai_journal_projection_test tests/journal_projection_test.c)
    target_link_libraries(cai_journal_projection_test PRIVATE cai_app_server_static)
    target_link_options(cai_journal_projection_test PRIVATE -Wl,--wrap=malloc
      -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free
      -Wl,--wrap=cai_client_open -Wl,--wrap=cai_agent_runtime_open)
    cai_configure_cli_target(cai_journal_projection_test)
    target_compile_options(cai_journal_projection_test PRIVATE -UNDEBUG)
    add_test(NAME cai_journal_projection COMMAND cai_journal_projection_test)
    set_tests_properties(cai_journal_projection PROPERTIES LABELS "unit;local;offline" TIMEOUT 30)
    add_dependencies(cai_app_server_tests cai_journal_projection_test)
  endif()
  if(TARGET cai_app_client_static AND TARGET cai_app_server_static)
    add_executable(cai_app_server_sdk_test tests/app_server_sdk_test.c tests/cli_storage_fixture.c)
    target_link_libraries(cai_app_server_sdk_test PRIVATE cai_app_server_static cai_app_client_static)
    target_link_options(cai_app_server_sdk_test PRIVATE -Wl,--wrap=lc_client_open)
    target_compile_options(cai_app_server_sdk_test PRIVATE -UNDEBUG)
    cai_configure_cli_target(cai_app_server_sdk_test)
    add_dependencies(cai_app_server_tests cai_app_server_sdk_test)
    add_test(NAME cai_app_server_controller COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_server_controller_test.sh"
      "$<TARGET_FILE:cai_app_server_sdk_test>")
    add_test(NAME cai_app_server_workspace COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_workspace_server_test.sh"
      "$<TARGET_FILE:cai_app_server_sdk_test>")
    set_tests_properties(cai_app_server_controller cai_app_server_workspace
      PROPERTIES LABELS "cli;local;offline" TIMEOUT 30)
  endif()
  if(CAI_BUILD_APP)
    foreach(variant static shared)
      if(variant STREQUAL "shared" AND (NOT CAI_BUILD_SHARED OR CAI_TARGET_ID MATCHES "-linux-musl$"))
        continue()
      endif()
      add_executable(cai_app_server_mix_${variant} tests/app_server_mix_test.c)
      target_link_libraries(cai_app_server_mix_${variant} PRIVATE cai_app_${variant})
      target_compile_options(cai_app_server_mix_${variant} PRIVATE -UNDEBUG)
      cai_configure_cli_target(cai_app_server_mix_${variant})
      add_dependencies(cai_app_server_tests cai_app_server_mix_${variant})
      add_test(NAME cai_app_server_mix_${variant} COMMAND bash
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_server_mix_test.sh"
        "$<TARGET_FILE:cai_app_server_mix_${variant}>")
      set_tests_properties(cai_app_server_mix_${variant} PROPERTIES LABELS "cli;local;offline" TIMEOUT 30)
    endforeach()
  endif()
endif()
