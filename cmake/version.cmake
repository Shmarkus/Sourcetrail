# ${CMAKE_SOURCE_DIR}/cmake/version.cmake
# VERSION_STRING keeps a tag's suffix (v2.0.0-fork gives "2.0.0-fork") for display; the numeric parts drop it.
include(${CMAKE_CURRENT_LIST_DIR}/versionParts.cmake)

if(EXISTS "${CMAKE_SOURCE_DIR}/.git")
  find_package(Git)

  # Only version tags: a release build checks out its `v1.2.3` tag, and the rolling `latest` tag is no version.
  execute_process(
    COMMAND ${GIT_EXECUTABLE} describe --abbrev=0 --tags --match "[0-9]*" --match "v[0-9]*"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE VERSION_STRING
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE GIT_RESULT)
  string(
    REGEX
    REPLACE "^v"
            ""
            VERSION_STRING
            "${VERSION_STRING}")

  # Check if the Git command was successful
  if(NOT
     GIT_RESULT
     EQUAL
     0)
    message(
      WARNING "Failed to run 'git describe' command. Git might not be installed or the repository is not initialized.")
    set(VERSION_MAJOR "1")
    set(VERSION_MINOR "0")
    set(VERSION_PATCH "0")
    set(VERSION_STRING "1.0.0")
  else()
    sourcetrail_version_parts("${VERSION_STRING}")
  endif()
endif()

if(SOURCETRAIL_CMAKE_VERBOSE)
  message(STATUS "Version: ${VERSION_STRING}")
  message(STATUS "Version year: ${VERSION_MAJOR}")
  message(STATUS "Version minor: ${VERSION_MINOR}")
  message(STATUS "Version commit: ${VERSION_PATCH}")
endif()
