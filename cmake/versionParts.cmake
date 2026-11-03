# ${CMAKE_SOURCE_DIR}/cmake/versionParts.cmake
# Sets VERSION_MAJOR, VERSION_MINOR and VERSION_PATCH in the caller's scope from a version string like "2.0.0" or
# "2.0.0-fork". A suffix after the patch number is not part of any of them.
function(sourcetrail_version_parts version_string)
  string(
    REGEX
    REPLACE "^([0-9]+)\\..*"
            "\\1"
            major
            "${version_string}")
  string(
    REGEX
    REPLACE "^[0-9]+\\.([0-9]+).*"
            "\\1"
            minor
            "${version_string}")
  string(
    REGEX
    REPLACE "^[0-9]+\\.[0-9]+\\.([0-9]+).*"
            "\\1"
            patch
            "${version_string}")
  set(VERSION_MAJOR
      "${major}"
      PARENT_SCOPE)
  set(VERSION_MINOR
      "${minor}"
      PARENT_SCOPE)
  set(VERSION_PATCH
      "${patch}"
      PARENT_SCOPE)
endfunction()
