# ${CMAKE_SOURCE_DIR}/src/core/utility/library/CMakeLists.txt
add_sourcetrail_interface(
  NAME
  core::utility::utilityLibrary
  DEPS
  Sourcetrail::core::utility::file::FilePath
  Sourcetrail::core::utility::utilityString
  ${CMAKE_DL_LIBS})
