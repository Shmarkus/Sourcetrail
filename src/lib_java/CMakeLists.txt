# ${CMAKE_SOURCE_DIR}/src/lib_java/CMakeLists.txt
find_package(JNI REQUIRED)

add_library(Sourcetrail_lib_java)
add_library(Sourcetrail::lib_java ALIAS Sourcetrail_lib_java)

target_sources(
  Sourcetrail_lib_java
  PRIVATE data/indexer/IndexerCommandJava.cpp
          data/indexer/IndexerJava.cpp
          data/parser/java/JavaEnvironment.cpp
          data/parser/java/JavaEnvironmentFactory.cpp
          data/parser/java/JavaParser.cpp
          project/SourceGroupFactoryModuleJava.cpp
          project/SourceGroupJava.cpp
          project/SourceGroupJavaEmpty.cpp
          project/SourceGroupJavaGradle.cpp
          project/SourceGroupJavaMaven.cpp
          utility/utilityDelombokPathAliases.cpp
          utility/utilityDirectorySync.cpp
          utility/utilityGradle.cpp
          utility/utilityGradleCache.cpp
          utility/utilityJava.cpp
          utility/utilityMaven.cpp
          LanguagePackageJava.cpp)

target_include_directories(
  Sourcetrail_lib_java
  PUBLIC ${CMAKE_CURRENT_LIST_DIR}
         ${CMAKE_CURRENT_LIST_DIR}/data/indexer/
         ${CMAKE_CURRENT_LIST_DIR}/data/parser/java/
         ${CMAKE_CURRENT_LIST_DIR}/project/
         ${CMAKE_CURRENT_LIST_DIR}/utility/)

target_include_directories(Sourcetrail_lib_java PRIVATE ${LIB_INCLUDE_PATHS} ${CMAKE_BINARY_DIR}/src/lib)

target_include_directories(
  Sourcetrail_lib_java SYSTEM
  PUBLIC ${JNI_INCLUDE_DIRS}
  PRIVATE ${EXTERNAL_INCLUDE_PATHS})

target_link_libraries(
  Sourcetrail_lib_java
  PUBLIC Sourcetrail::core::utility::utilityLibrary
  PRIVATE Sourcetrail::core
          Sourcetrail::core::utility::logging
          Sourcetrail::core::utility::ScopedFunctor
          Sourcetrail::core::utility::SingleValueCache
          Sourcetrail::core::utility::TextAccess
          Sourcetrail::core::utility::TextCodec
          Sourcetrail::core::utility::utility
          Sourcetrail::core::utility::utilityXml
          Sourcetrail::core::utility::file::FileManager
          Sourcetrail::core::utility::file::FilePathFilter
          Sourcetrail::core::utility::file::FileRegister
          Sourcetrail::core::utility::file::FileSystem
          Sourcetrail::core::utility::file::utilityFile
          Sourcetrail::lib
          Sourcetrail::lib_gui::utility
          Sourcetrail::libGui::utility::utilityApp
          Sourcetrail::messaging
          Sourcetrail::scheduling
          Qt6::Core
          tinyxml)
