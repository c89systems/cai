# Exercise the compiled owner boundary rather than grepping source includes.
execute_process(COMMAND "${CAI_NM}" -u "${CAI_OWNER_OBJECT}"
  RESULT_VARIABLE result OUTPUT_VARIABLE symbols ERROR_VARIABLE failure)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Cannot inspect runtime owner: ${failure}")
endif()
if(symbols MATCHES "[ \t](sl_|mdf_|cai_cli_presenter_|cai_cli_completions_|cai_cli_input_adapter_)")
  message(FATAL_ERROR "Runtime owner requires a terminal adapter: ${symbols}")
endif()
