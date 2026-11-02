# ${CMAKE_SOURCE_DIR}/src/app/tests/CMakeLists.txt
add_sourcetrail_test(
  NAME
  headless_console_test
  SOURCES
  HeadlessConsoleTestSuite.cpp
  DEPS
  Qt6::Core
  TEST_PREFIX
  "integration.app."
  WORKING_DIRECTORY
  "${CMAKE_BINARY_DIR}/test/")

target_compile_definitions(headless_console_test PRIVATE SOURCETRAIL_APP_PATH="$<TARGET_FILE:Sourcetrail>")
add_dependencies(headless_console_test Sourcetrail)
