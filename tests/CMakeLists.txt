# ${CMAKE_SOURCE_DIR}/tests/CMakeLists.txt
add_test(NAME cmake.versionParts COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/VersionPartsTest.cmake)

if(ENABLE_INTEGRATION_TEST)
  add_subdirectory(integration)
endif()
