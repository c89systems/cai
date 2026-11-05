# Typed application transport is an independent product. Neither compiling nor
# consuming it discovers an editor, renderer, storage SDK or Lua runtime.
if(NOT CAI_BUILD_APP_CLIENT)
  return()
endif()
add_library(cai_app_wire_objects OBJECT cli/input_wire.c cli/status_wire.c
  cli/transcript_wire.c cli/catalog_wire.c cli/error.c)
add_library(cai_app_client_objects OBJECT cli/app_client.c)
foreach(target cai_app_wire_objects cai_app_client_objects)
  target_link_libraries(${target} PRIVATE ${_cai_internal_link_target})
  cai_configure_c_target(${target})
endforeach()
if(CAI_BUILD_STATIC)
  add_library(cai_app_client_static STATIC $<TARGET_OBJECTS:cai_app_client_objects>
    $<TARGET_OBJECTS:cai_app_wire_objects>)
  add_library(cai::app_client_static ALIAS cai_app_client_static)
  set_target_properties(cai_app_client_static PROPERTIES
    OUTPUT_NAME cai_app_client EXPORT_NAME app_client_static)
  target_link_libraries(cai_app_client_static PUBLIC cai_static)
endif()
if(CAI_BUILD_SHARED)
  add_library(cai_app_client_shared SHARED $<TARGET_OBJECTS:cai_app_client_objects>
    $<TARGET_OBJECTS:cai_app_wire_objects>)
  add_library(cai::app_client_shared ALIAS cai_app_client_shared)
  set_target_properties(cai_app_client_shared PROPERTIES
    OUTPUT_NAME cai_app_client EXPORT_NAME app_client_shared
    VERSION ${PROJECT_VERSION} SOVERSION 0)
  if(APPLE)
    set_target_properties(cai_app_client_shared PROPERTIES
      BUILD_WITH_INSTALL_RPATH ON INSTALL_NAME_DIR "@rpath" MACOSX_RPATH ON)
  endif()
  target_link_libraries(cai_app_client_shared PUBLIC cai_shared)
  if(NOT APPLE)
    target_link_options(cai_app_client_shared PRIVATE -Wl,--no-undefined)
  endif()
  cai_apply_export_policy(cai_app_client_shared
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cai_app_client.exports")
  cai_configure_c_target(cai_app_client_shared)
endif()
foreach(target cai_app_client_static cai_app_client_shared)
  if(TARGET ${target})
    target_include_directories(${target} PUBLIC
      $<BUILD_INTERFACE:${CAI_PUBLIC_INCLUDE_DIR}>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    if(CAI_INSTALL)
      install(TARGETS ${target} EXPORT caiAppClientTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT cai-app-client
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT cai-app-client)
    endif()
  endif()
endforeach()
if(CAI_INSTALL)
  install(EXPORT caiAppClientTargets NAMESPACE cai:: FILE cai-app-client-targets.cmake
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cai_app_client" COMPONENT cai-app-client)
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/cai_app_client-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cai_app_client-config.cmake" @ONLY)
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/cai-config-version.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cai_app_client-config-version.cmake" @ONLY)
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/cai-app-client.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cai-app-client.pc" @ONLY)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/cai_app_client-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/cai_app_client-config-version.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cai_app_client" COMPONENT cai-app-client)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/cai-app-client.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig" COMPONENT cai-app-client)
  install(FILES "${CAI_PUBLIC_INCLUDE_DIR}/cai/app_client.h"
    "${CAI_PUBLIC_INCLUDE_DIR}/cai/app_view.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/cai" COMPONENT cai-app-client)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/application-client.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/libcai_app_client" COMPONENT cai-app-client)
endif()
if(CAI_BUILD_TESTS)
  if(CAI_INSTALL)
    add_test(NAME cai_app_client_configuration COMMAND "${CMAKE_COMMAND}"
      "-DCAI_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
      "-DCAI_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DCAI_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
      "-DCAI_TARGET_ID=${CAI_TARGET_ID}"
      "-DCAI_DEPS_DIR=${CAI_DEPS_DIR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_client_configuration_test.cmake")
    set_tests_properties(cai_app_client_configuration PROPERTIES LABELS "local;offline;packaging" TIMEOUT 30)
    add_test(NAME cai_app_client_install COMMAND "${CMAKE_COMMAND}"
      "-DCAI_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
      "-DCAI_C_COMPILER=${CMAKE_C_COMPILER}"
      "-DCAI_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DCAI_DEPENDENCY_PREFIXES=${CAI_CPKT_PREFIX};${CAI_CPKTMISC_PREFIX};${CAI_LONEJSON_PREFIX};${CAI_PSLOG_PREFIX}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_client_install_test.cmake")
    set_tests_properties(cai_app_client_install PROPERTIES LABELS "local;offline;packaging" TIMEOUT 30)
  endif()
  add_test(NAME cai_app_client_boundary COMMAND "${CMAKE_COMMAND}"
    "-DCAI_NM=${CMAKE_NM}"
    "-DCAI_CLIENT_OBJECTS=$<TARGET_OBJECTS:cai_app_client_objects>;$<TARGET_OBJECTS:cai_app_wire_objects>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_client_boundary_test.cmake")
  set_tests_properties(cai_app_client_boundary PROPERTIES LABELS "unit;local;offline" TIMEOUT 5)
  if(TARGET cai_app_client_static)
    add_executable(cai_app_client_test tests/app_client_test.c)
    target_link_libraries(cai_app_client_test PRIVATE cai_app_client_static)
    cai_configure_cli_target(cai_app_client_test)
    target_compile_options(cai_app_client_test PRIVATE -UNDEBUG)
    add_test(NAME cai_app_client COMMAND cai_app_client_test)
    set_tests_properties(cai_app_client PROPERTIES LABELS "unit;local;offline" TIMEOUT 5)
  endif()
  if(TARGET cai_app_client_shared)
    set(_cai_client_export_platform linux)
    if(APPLE)
      set(_cai_client_export_platform darwin)
    endif()
    add_test(NAME cai_app_client_exports COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/shared_export_policy_test.sh" "${CMAKE_NM}"
      "$<TARGET_FILE:cai_app_client_shared>"
      "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cai_app_client.exports" "${_cai_client_export_platform}")
    set_tests_properties(cai_app_client_exports PROPERTIES LABELS "unit;local;offline" TIMEOUT 5)
  endif()
endif()
