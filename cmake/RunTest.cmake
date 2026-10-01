foreach(required EXPLORER INPUT EXPECTED)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "RunTest.cmake: -D${required}= is required")
  endif()
endforeach()

execute_process(
  COMMAND "${EXPLORER}" "${INPUT}"
  OUTPUT_VARIABLE actual
  ERROR_VARIABLE errors
  RESULT_VARIABLE status)

if(NOT status EQUAL 0)
  message(FATAL_ERROR "explorer failed (${status}):\n${errors}")
endif()

file(READ "${EXPECTED}" expected)
if(NOT actual STREQUAL expected)
  message(FATAL_ERROR
    "explorer output did not match:\n"
    "--- expected ---\n${expected}"
    "--- actual ---\n${actual}")
endif()
