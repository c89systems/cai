if(NOT CAI_BUILD_BENCHMARKS)
  return()
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT TARGET cai_static)
  message(FATAL_ERROR "Benchmarks require Linux and CAI_BUILD_STATIC=ON")
endif()
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/bench/benchmark_build.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/benchmark_build.h" @ONLY)
add_executable(cai_benchmark bench/wire_benchmark.c)
target_include_directories(cai_benchmark PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(cai_benchmark PRIVATE cai_static)
target_link_options(cai_benchmark PRIVATE -Wl,--wrap=send -Wl,--wrap=recv -Wl,--wrap=write)
cai_configure_cli_target(cai_benchmark)
if(CAI_BUILD_APP AND TARGET cai_app_client_static)
  target_sources(cai_benchmark PRIVATE bench/frontend_benchmark.c
    cli/softline_presenter.c cli/transcript_presenter.c cli/literal.c
    cli/reasoning.c cli/theme.c cli/shell_text.c)
  target_include_directories(cai_benchmark PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/cli")
  target_compile_definitions(cai_benchmark PRIVATE CAI_BENCH_FRONTEND=1)
  target_link_libraries(cai_benchmark PRIVATE cai_app_client_static
    softline::softline_static libmdf::mdf_static)
endif()
if(CAI_BUILD_TESTS)
  add_test(NAME cai_benchmark_contract COMMAND bash
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmark_contract_test.sh"
    "$<TARGET_FILE:cai_benchmark>" "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties(cai_benchmark_contract PROPERTIES LABELS "unit;local;offline" TIMEOUT 20)
  if(TARGET cai_terminal_peer)
    add_dependencies(cai_benchmark cai_terminal_peer)
    set_tests_properties(cai_benchmark_contract PROPERTIES ENVIRONMENT
      "CAI_BENCH_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
  endif()
  add_test(NAME cai_benchmark_driver COMMAND bash
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmark_driver_test.sh"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/benchmark.sh" "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties(cai_benchmark_driver PROPERTIES LABELS "unit;local;offline" TIMEOUT 20)
  if(TARGET cai_tests)
    add_dependencies(cai_tests cai_benchmark)
  endif()
endif()
