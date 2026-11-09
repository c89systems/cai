if(CAI_BUILD_TESTS OR CAI_BUILD_FUZZERS)
  if(CAI_TEST_GLIBC_THREADS)
    # Capture the configured environment, retaining unrelated loader tunables.
    # This controls caching only; executables/core startup own stack defaults.
    string(REPLACE ":" ";" _cai_tunables "$ENV{GLIBC_TUNABLES}")
    list(FILTER _cai_tunables EXCLUDE REGEX "^glibc\\.pthread\\.stack_cache_size=")
    list(FILTER _cai_tunables EXCLUDE REGEX "^$")
    list(APPEND _cai_tunables "glibc.pthread.stack_cache_size=0")
    list(JOIN _cai_tunables ":" _cai_tunables)
  endif()
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-tmp")
  get_property(tests DIRECTORY PROPERTY TESTS)
  foreach(test IN LISTS tests)
    set_property(TEST ${test} APPEND PROPERTY ENVIRONMENT
      "TMPDIR=${CMAKE_CURRENT_BINARY_DIR}/test-tmp")
    if(CAI_TEST_GLIBC_THREADS)
      set_property(TEST ${test} APPEND PROPERTY ENVIRONMENT
        "GLIBC_TUNABLES=${_cai_tunables}")
    endif()
    if(CAI_TEST_PYTHON)
      set_property(TEST ${test} APPEND PROPERTY ENVIRONMENT "CAI_TEST_PYTHON=${CAI_TEST_PYTHON}")
    endif()
    get_test_property(${test} LABELS labels)
    if("integration" IN_LIST labels AND NOT "offline" IN_LIST labels AND
       NOT "local" IN_LIST labels)
      set_property(TEST ${test} APPEND PROPERTY LABELS live)
    endif()
  endforeach()
endif()
