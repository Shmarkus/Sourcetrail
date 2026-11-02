# ${CMAKE_SOURCE_DIR}/src/external/CMakeLists.txt
add_library(CppSQLite3 sqlite/CppSQLite3.cpp)
add_library(CppSQLite::CppSQLite3 ALIAS CppSQLite3)
target_link_libraries(
  CppSQLite3
  PUBLIC SQLite::SQLite3
  PRIVATE fmt::fmt)
target_include_directories(CppSQLite3 SYSTEM PUBLIC ${CMAKE_CURRENT_LIST_DIR}/sqlite)
set_source_files_properties(CppSQLite3 PROPERTIES COMPILE_FLAGS "-w")

# tinyxml: XML parsing for the Java package (Maven effective poms)
add_library(tinyxml tinyxml/tinystr.cpp tinyxml/tinyxml.cpp tinyxml/tinyxmlerror.cpp tinyxml/tinyxmlparser.cpp)
target_include_directories(tinyxml SYSTEM PUBLIC ${CMAKE_CURRENT_LIST_DIR}/tinyxml)
target_compile_options(tinyxml PRIVATE -w)
