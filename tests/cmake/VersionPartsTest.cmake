# ${CMAKE_SOURCE_DIR}/tests/cmake/VersionPartsTest.cmake
# Run with: cmake -P tests/cmake/VersionPartsTest.cmake
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/versionParts.cmake)

function(expect_version_parts version_string major minor patch)
  sourcetrail_version_parts("${version_string}")
  if(NOT
     "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}"
     STREQUAL
     "${major}.${minor}.${patch}")
    message(
      FATAL_ERROR
        "'${version_string}' gave ${VERSION_MAJOR}/${VERSION_MINOR}/${VERSION_PATCH}, expected ${major}/${minor}/${patch}")
  endif()
endfunction()

expect_version_parts("2.0.0" 2 0 0)
expect_version_parts("2.0.0-fork" 2 0 0)
expect_version_parts("12.34.56-fork.1" 12 34 56)
expect_version_parts("2025.4.1" 2025 4 1)
