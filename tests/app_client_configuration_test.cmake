cmake_minimum_required(VERSION 3.21)
set(root "${CAI_BINARY_DIR}/app-client-configuration-test")
file(REMOVE_RECURSE "${root}")
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${CAI_SOURCE_DIR}" -B "${root}"
  -G Ninja "-DCMAKE_TOOLCHAIN_FILE=${CAI_TOOLCHAIN_FILE}"
  "-DCPKT_TARGET_ID=${CAI_TARGET_ID}" "-DCAI_TARGET_ID=${CAI_TARGET_ID}"
  "-DCAI_DEPS_DIR=${CAI_DEPS_DIR}" -DCMAKE_BUILD_TYPE=Debug
  -DCAI_BUILD_APP_CLIENT=ON -DCAI_BUILD_APP=OFF -DCAI_BUILD_CLI=OFF
  -DCAI_BUILD_LUA=OFF -DCAI_BUILD_LUA_TOOLS=OFF
  -DCAI_BUILD_TESTS=OFF -DCAI_BUILD_EXAMPLES=OFF
  -DCMAKE_DISABLE_FIND_PACKAGE_softline=TRUE
  -DCMAKE_DISABLE_FIND_PACKAGE_libmdf=TRUE
  -DCMAKE_DISABLE_FIND_PACKAGE_lockdc=TRUE
  -DCMAKE_DISABLE_FIND_PACKAGE_Lua=TRUE
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Configure client without frontend/storage SDKs: ${output}\n${errors}")
endif()
file(READ "${root}/CMakeCache.txt" cache)
if(cache MATCHES "(^|\n)(softline|libmdf|lockdc|Lua)_DIR:")
  message(FATAL_ERROR "Client-only configuration discovered an application dependency")
endif()
if(NOT EXISTS "${root}/cai_app_client-config.cmake" OR
    EXISTS "${root}/cai_app-config.cmake")
  message(FATAL_ERROR "Client-only configuration selected the wrong product")
endif()
