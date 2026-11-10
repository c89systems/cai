cmake_minimum_required(VERSION 3.21)
set(root "${CAI_BINARY_DIR}/app-install-test")
file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${root}/source")
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${CAI_BINARY_DIR}"
  --prefix "${root}/stage" RESULT_VARIABLE result OUTPUT_QUIET)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "install CAI application SDK failed")
endif()
file(RENAME "${root}/stage" "${root}/relocated")
set(prefix "${root}/relocated")
foreach(path include/cai/app.h include/cai/app_client.h include/cai/workspace_registry.h
    include/cai/workspace_discovery.h
    lib/libcai_app.a lib/pkgconfig/cai-app.pc
    lib/cmake/cai_app/cai_app-config.cmake share/doc/libcai_app/embedding.md
    share/cai_app/dependencies.json
    share/doc/libcai_app/examples/app.c share/doc/libcai_app/examples/app.lua)
  if(NOT EXISTS "${prefix}/${path}")
    message(FATAL_ERROR "missing application SDK payload: ${path}")
  endif()
endforeach()
foreach(path include/cai/context.h lib/libcai_context.a lib/pkgconfig/cai-context.pc
    lib/cmake/cai_context/cai_context-config.cmake
    share/cai_context/dependencies.json share/doc/libcai_context/context.md
    share/doc/libcai_context/LICENSE)
  if(NOT EXISTS "${prefix}/${path}")
    message(FATAL_ERROR "missing context SDK payload: ${path}")
  endif()
endforeach()
file(READ "${prefix}/share/cai_context/dependencies.json" context_manifest)
string(JSON context_project GET "${context_manifest}" project)
string(JSON context_digest GET "${context_manifest}" dependencies 0 sha256)
string(LENGTH "${context_digest}" context_digest_length)
if(NOT context_project STREQUAL "libcai_context" OR NOT context_digest_length EQUAL 64)
  message(FATAL_ERROR "invalid context dependency manifest")
endif()
file(READ "${prefix}/share/cai_app/dependencies.json" manifest)
string(JSON abi GET "${manifest}" abiVersion)
string(JSON dependency_count LENGTH "${manifest}" dependencies)
if(NOT abi EQUAL 0 OR NOT dependency_count EQUAL 4)
  message(FATAL_ERROR "invalid application dependency manifest")
endif()
foreach(index RANGE 0 3)
  string(JSON digest GET "${manifest}" dependencies ${index} sha256)
  string(LENGTH "${digest}" length)
  if(NOT length EQUAL 64 OR NOT digest MATCHES "^[a-f0-9]+$")
    message(FATAL_ERROR "invalid application dependency digest")
  endif()
endforeach()
file(COPY "${prefix}/share/doc/libcai_app/examples/app.c" DESTINATION "${root}/source")
file(RENAME "${root}/source/app.c" "${root}/source/example.c")
file(WRITE "${root}/source/main.c" [=[
#define main cai_example_main
#include "example.c"
#undef main
#include <assert.h>
#include <cai/skills.h>
#include <cai/app_client.h>
#include <cai/workspace_registry.h>
#include <cai/workspace_discovery.h>
int main(int argc, char **argv) {
  cai_app *app = NULL;
  cai_app_status status;
  cai_error error;
  cai_skill_metadata *metadata = NULL;
  cai_app_client_config client;
  cai_workspace_registry *registry = NULL;
  cai_workspace_discovery_config discovery;
  const cai_workspace_record *record = NULL;
  unsigned long long id;
  cai_error_init(&error);
  cai_app_client_config_init(&client);
  assert(client.event == NULL && client.handshake_timeout_ms == 0U);
  assert(client.expected_instance == NULL);
  cai_workspace_discovery_config_init(&discovery);
  assert(discovery.selector == NULL && discovery.probe_timeout_ms == 0U);
  assert(cai_workspace_registry_open(NULL, &registry, &error) == CAI_ERR_INVALID);
  assert(registry == NULL);
  cai_error_cleanup(&error);
  assert(cai_workspace_registry_next(NULL, &record, &error) == CAI_ERR_INVALID);
  assert(record == NULL);
  cai_workspace_registry_close(registry);
  cai_error_cleanup(&error);
  assert(cai_skill_metadata_open(NULL, NULL, &metadata, &error) == CAI_ERR_INVALID);
  assert(metadata == NULL);
  cai_error_cleanup(&error);
  cai_app_status_init(&status);
  assert(cai_app_open(NULL, &app, &error) == CAI_OK);
  assert(cai_app_get_status(app, &status, &error) == CAI_OK);
  assert(status.outcome == CAI_APP_NOT_RUN && app->get_status != NULL);
  assert(cai_app_control(app, CAI_APP_STOP, NULL, &id, &error) == CAI_ERR_INVALID);
  assert(id == 0U && app->control != NULL);
  cai_app_status_cleanup(&status);
  cai_error_cleanup(&error);
  app->close(app);
  return cai_example_main(argc, argv);
}
]=])
file(WRITE "${root}/source/context.c" [=[
#include <cai/context.h>
#include <assert.h>
int main(void) {
  cai_context_config config;
  cai_context *context = NULL;
  cai_error error;
  cai_error_init(&error);
  cai_context_config_init(&config);
  assert(config.client == NULL && config.root_path == NULL && !config.yolo);
  assert(cai_context_open(&config, &context, &error) == CAI_ERR_INVALID);
  assert(context == NULL);
  cai_context_close(context);
  cai_error_cleanup(&error);
  return 0;
}
]=])
file(WRITE "${root}/source/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.21)
project(app_consumer C)
find_package(cai_app CONFIG REQUIRED)
find_package(cai_context CONFIG REQUIRED)
add_executable(context_static context.c)
target_link_libraries(context_static PRIVATE cai::context_static)
target_compile_options(context_static PRIVATE -std=c89 -Wall -Wextra -Werror -Wpedantic -Wno-long-long)
if(TARGET cai::context)
  add_executable(context_shared context.c)
  target_link_libraries(context_shared PRIVATE cai::context)
  target_compile_options(context_shared PRIVATE -std=c89 -Wall -Wextra -Werror -Wpedantic -Wno-long-long)
endif()
add_executable(app_static main.c)
target_link_libraries(app_static PRIVATE cai::app_static)
target_compile_options(app_static PRIVATE -std=c89 -Wall -Wextra -Werror -Wpedantic -Wno-long-long)
if(TARGET cai::app_shared)
  add_executable(app_shared main.c)
  target_link_libraries(app_shared PRIVATE cai::app_shared)
  target_compile_options(app_shared PRIVATE -std=c89 -Wall -Wextra -Werror -Wpedantic -Wno-long-long)
endif()
]=])
set(prefixes "${prefix};${CAI_DEPENDENCY_PREFIXES}")
file(MAKE_DIRECTORY "${root}/wrong-server")
file(WRITE "${root}/wrong-server/cai_app_server-config.cmake"
  "set(cai_app_server_VERSION 9999.0.0)\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${root}/source" -B "${root}/mismatched-server"
  -G Ninja "-DCMAKE_C_COMPILER=${CAI_C_COMPILER}" "-DCMAKE_PREFIX_PATH=${prefixes}"
  "-Dcai_app_server_DIR=${root}/wrong-server"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(result EQUAL 0 OR NOT errors MATCHES "matching cai_app_server release")
  message(FATAL_ERROR "combined SDK accepted a different private backend release: ${output}\n${errors}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${root}/source" -B "${root}/consumer"
  -G Ninja "-DCMAKE_C_COMPILER=${CAI_C_COMPILER}"
  "-DCMAKE_PREFIX_PATH=${prefixes}" -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=ON
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "configure public installed application consumer:\n${output}\n${errors}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${root}/source" -B "${root}/missing-yaml"
  -G Ninja "-DCMAKE_C_COMPILER=${CAI_C_COMPILER}"
  "-DCMAKE_PREFIX_PATH=${prefixes}" -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=ON
  -DCMAKE_DISABLE_FIND_PACKAGE_CpktYaml=TRUE
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(result EQUAL 0 OR NOT errors MATCHES "CpktYaml")
  message(FATAL_ERROR "installed application must reject unavailable CpktYaml: ${output}\n${errors}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${root}/consumer" --parallel 4
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "link public installed application consumer:\n${output}\n${errors}")
endif()
set(loader "${prefix}/lib")
set(pkg_path "${prefix}/lib/pkgconfig")
foreach(dependency IN LISTS CAI_DEPENDENCY_PREFIXES)
  string(APPEND loader ":${dependency}/lib")
  string(APPEND pkg_path ":${dependency}/lib/pkgconfig")
endforeach()
foreach(variant static shared)
  if(EXISTS "${root}/consumer/context_${variant}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env "LD_LIBRARY_PATH=${loader}"
      "${root}/consumer/context_${variant}" RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
      message(FATAL_ERROR "run installed ${variant} context consumer failed")
    endif()
  endif()
  if(EXISTS "${root}/consumer/app_${variant}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env "LD_LIBRARY_PATH=${loader}"
      "${root}/consumer/app_${variant}" --help RESULT_VARIABLE result OUTPUT_VARIABLE output)
    if(NOT result EQUAL 0 OR NOT output MATCHES "Usage: cai ")
      message(FATAL_ERROR "run installed ${variant} application consumer failed")
    endif()
  endif()
endforeach()
find_program(PKG_CONFIG pkg-config REQUIRED)
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "PKG_CONFIG_PATH=${pkg_path}"
  "${PKG_CONFIG}" --cflags --libs --static cai-context
  RESULT_VARIABLE result OUTPUT_VARIABLE flags ERROR_VARIABLE errors OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "installed context pkg-config metadata: ${errors}")
endif()
separate_arguments(arguments UNIX_COMMAND "${flags}")
execute_process(COMMAND "${CAI_C_COMPILER}" -std=c89 -Wall -Wextra -Werror
  "${root}/source/context.c" ${arguments} -o "${root}/context-pkg-consumer"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "link context pkg-config consumer: ${output}\n${errors}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "LD_LIBRARY_PATH=${loader}"
  "${root}/context-pkg-consumer" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "run installed context pkg-config consumer failed")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "PKG_CONFIG_PATH=${pkg_path}"
  "${PKG_CONFIG}" --cflags --libs --static cai-app
  RESULT_VARIABLE result OUTPUT_VARIABLE flags ERROR_VARIABLE errors OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "installed application pkg-config metadata: ${errors}")
endif()
separate_arguments(arguments UNIX_COMMAND "${flags}")
execute_process(COMMAND "${CAI_C_COMPILER}" -std=c89 -Wall -Wextra -Werror
  "${root}/source/main.c" ${arguments} -o "${root}/pkg-consumer"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "link application pkg-config consumer: ${output}\n${errors}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "LD_LIBRARY_PATH=${loader}"
  "${root}/pkg-consumer" --version RESULT_VARIABLE result OUTPUT_QUIET)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "run installed pkg-config application consumer failed")
endif()
if(NOT CAI_LUA_RUNNER STREQUAL "")
  if(NOT EXISTS "${prefix}/lib/lua/5.5/cai/app.so")
    message(FATAL_ERROR "application Lua module was not installed")
  endif()
  file(WRITE "${root}/lua-consumer.lua" [=[
local app = assert(require("cai.app").open {program_name = "installed-host", invocation_name = "installed host", no_config = true})
assert(app:help():find("Usage: installed host [options]", 1, true))
assert(app:run {"--help"} == 0)
assert(app:get_status().outcome == "completed")
assert(not app:control("stop"))
app:close()
]=])
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env "LD_LIBRARY_PATH=${loader}"
    "LUA_CPATH=${prefix}/lib/lua/5.5/?.so;;"
    "${CAI_LUA_RUNNER}" "${root}/lua-consumer.lua"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "run installed Lua application facade: ${output}\n${errors}")
  endif()
  # Compile the shipped facade sources against the relocated public SDK.
  # No cli/ or src/ private headers are available in this source product.
  execute_process(COMMAND bash "${CAI_SOURCE_DIR}/scripts/stage_lua_rock_sources.sh"
    "${CAI_SOURCE_DIR}" "${root}/lua-source" 0.0.0
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "stage Lua facade sources: ${output}\n${errors}")
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env "PKG_CONFIG_PATH=${pkg_path}"
    "${CMAKE_COMMAND}" -S "${root}/lua-source/lua" -B "${root}/lua-build" -G Ninja
    "-DCMAKE_C_COMPILER=${CAI_C_COMPILER}"
    "-DCAI_LUA_INCLUDE_DIR=${CAI_LUA_INCLUDE_DIR}"
    "-DCAI_PSLOG_LUA_INCLUDE_DIR=${CAI_PSLOG_LUA_INCLUDE_DIR}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "configure packaged Lua facade: ${output}\n${errors}")
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" --build "${root}/lua-build" --parallel 4
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "build packaged Lua facade: ${output}\n${errors}")
  endif()
  if(NOT EXISTS "${root}/lua-build/cai/app.so")
    message(FATAL_ERROR "packaged Lua build omitted the application facade")
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env "LD_LIBRARY_PATH=${loader}"
    "LUA_CPATH=${root}/lua-build/?.so;;"
    "${CAI_LUA_RUNNER}" "${root}/lua-consumer.lua"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "run packaged Lua application facade: ${output}\n${errors}")
  endif()
endif()
file(REMOVE_RECURSE "${root}")
