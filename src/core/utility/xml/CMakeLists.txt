# ${CMAKE_SOURCE_DIR}/src/core/utility/xml/CMakeLists.txt
add_sourcetrail_library(
  NAME
  core::utility::utilityXml
  SOURCES
  utilityXml.cpp
  PUBLIC_HEADERS
  utilityXml.h
  PUBLIC_DEPS
  Sourcetrail::core::utility::TextAccess
  PRIVATE_DEPS
  Sourcetrail::core::utility::logging
  tinyxml)
