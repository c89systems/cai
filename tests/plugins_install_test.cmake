cmake_minimum_required(VERSION 3.21)
set(root "${CAI_BINARY_DIR}/plugins-install-test")
file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${root}/source")
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${CAI_BINARY_DIR}" --prefix "${root}/stage"
  RESULT_VARIABLE result OUTPUT_QUIET)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "plugin SDK installation failed")
endif()
file(RENAME "${root}/stage" "${root}/relocated")
set(prefix "${root}/relocated")
file(READ "${prefix}/share/cai_lua_tools/dependencies.json" manifest)
string(JSON lua_version GET "${manifest}" dependencies 0 version)
string(JSON provider GET "${manifest}" dependencies 0 providedBy)
string(JSON abi GET "${manifest}" abiVersion)
if(NOT lua_version STREQUAL "5.5.1" OR NOT provider STREQUAL "cpkt" OR NOT abi EQUAL 0)
  message(FATAL_ERROR "invalid Lua tool adapter dependency metadata")
endif()
file(WRITE "${root}/source/main.c" [=[
#include <cai/plugins.h>
#ifdef WITH_LUA
#include <cai/lua_tools.h>
#endif
int main(void) {
  cai_plugins_config config;
  cai_plugins *plugins = 0;
  cai_error error;
  int rc;
  cai_error_init(&error); cai_plugins_config_init(&config);
  rc = cai_plugins_open(&config, &plugins, &error);
#ifdef WITH_LUA
  if (rc == CAI_OK) {
    cai_lua_tools *tools = 0;
    cai_lua_tools_config lua;
    cai_lua_tools_config_init(&lua);
    rc = cai_lua_tools_open(&lua, &tools, &error);
    if (rc == CAI_OK) rc = cai_lua_tools_attach(tools, plugins, &error);
    cai_plugins_close(plugins); plugins = 0;
    cai_lua_tools_close(tools);
  }
#endif
  cai_plugins_close(plugins); cai_error_cleanup(&error); return rc;
}
]=])
file(WRITE "${root}/source/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.21)
project(plugin_consumer C)
find_package(cai CONFIG REQUIRED)
if(WITH_LUA)
  find_package(cai_lua_tools CONFIG REQUIRED)
  foreach(variant lua_tools_static lua_tools)
    add_executable(${variant} main.c)
    target_compile_definitions(${variant} PRIVATE WITH_LUA=1)
    target_link_libraries(${variant} PRIVATE cai::${variant})
  endforeach()
else()
  if(Lua_FOUND)
    message(FATAL_ERROR "core cai must not discover Lua")
  endif()
  add_executable(core main.c)
  target_link_libraries(core PRIVATE cai::cai_shared)
endif()
]=])
set(prefixes "${prefix};${CAI_DEPENDENCY_PREFIXES}")
set(loader "${prefix}/lib")
set(pkg_path "${prefix}/lib/pkgconfig")
foreach(dep IN LISTS CAI_DEPENDENCY_PREFIXES)
  string(APPEND loader ":${dep}/lib")
  string(APPEND pkg_path ":${dep}/lib/pkgconfig")
endforeach()
foreach(mode core lua)
  set(args -DCMAKE_DISABLE_FIND_PACKAGE_Lua=TRUE)
  set(programs core)
  if(mode STREQUAL "lua")
    set(args -DWITH_LUA=ON)
    set(programs lua_tools_static lua_tools)
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" -S "${root}/source" -B "${root}/${mode}" -G Ninja
    "-DCMAKE_C_COMPILER=${CAI_C_COMPILER}" "-DCMAKE_PREFIX_PATH=${prefixes}" ${args}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "configure ${mode} installed consumer: ${output}\n${errors}")
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" --build "${root}/${mode}" --parallel 4
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "link ${mode} installed consumer: ${output}\n${errors}")
  endif()
  foreach(program IN LISTS programs)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env "LD_LIBRARY_PATH=${loader}" "${root}/${mode}/${program}" RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
      message(FATAL_ERROR "run ${program} installed consumer failed: ${result}")
    endif()
  endforeach()
endforeach()
find_program(PKG_CONFIG pkg-config REQUIRED)
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "PKG_CONFIG_PATH=${pkg_path}"
  "${PKG_CONFIG}" --cflags --libs --static cai-lua-tools
  RESULT_VARIABLE result OUTPUT_VARIABLE flags ERROR_VARIABLE errors OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Lua adapter pkg-config failed: ${errors}")
endif()
separate_arguments(arguments UNIX_COMMAND "${flags}")
execute_process(COMMAND "${CAI_C_COMPILER}" -DWITH_LUA=1 -x c++ -std=c++98 -Wall -Wextra -Werror
  "${root}/source/main.c" -x none ${arguments} -o "${root}/cpp-consumer"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "C++ public plugin headers/linkage: ${output}\n${errors}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "LD_LIBRARY_PATH=${loader}" "${root}/cpp-consumer" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "run C++ plugin consumer failed: ${result}")
endif()
