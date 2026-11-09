# Test builds model musl workers without changing build-tool or main stacks.
set(CAI_TEST_GLIBC_THREADS FALSE)
string(TOLOWER "${CMAKE_BUILD_TYPE}" _cai_thread_build_type)
if(CAI_BUILD_TESTS AND _cai_thread_build_type STREQUAL "debug" AND
   CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CAI_TARGET_ID MATCHES "-linux-gnu$")
  set(CAI_TEST_GLIBC_THREADS TRUE)
endif()

function(cai_configure_test_threads target)
  if(CAI_TEST_GLIBC_THREADS)
    get_target_property(type ${target} TYPE)
    if(type STREQUAL "EXECUTABLE")
      target_sources(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tests/debug_thread_startup.c")
      target_link_libraries(${target} PRIVATE Threads::Threads)
    endif()
  endif()
endfunction()
