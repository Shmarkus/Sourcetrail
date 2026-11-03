# ${CMAKE_SOURCE_DIR}/src/lib_java/tests/CMakeLists.txt
set(java_test_names
    GradleCacheTestSuite
    JavaIndexSampleProjectsTestSuite
    JavaLoggingTestSuite
    JavaParserTestSuite
    SourceGroupJavaTestSuite
    UtilityGradleTestSuite
    UtilityMavenTestSuite)

foreach(test_name IN LISTS java_test_names)
  add_sourcetrail_test(
    NAME
    ${test_name}
    SOURCES
    ${test_name}.cpp
    JavaTestEnvironment.cpp
    DEPS
    Sourcetrail::lib
    Sourcetrail::lib_java
    Sourcetrail::lib_gui::utility
    Sourcetrail::helper::TestStorage
    Sourcetrail::core::utility::TextAccess
    Sourcetrail::core::utility::file::FileSystem
    Sourcetrail::core::utility::TimeStamp
    Qt6::Core
    TEST_PREFIX
    "unittests.lib_java."
    WORKING_DIRECTORY
    "${CMAKE_BINARY_DIR}/test/")
  target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_LIST_DIR})
  add_dependencies(${test_name} java_indexer_jar)
endforeach()

# indexes a copy of the Lombok fixture with the built application
target_compile_definitions(
  JavaLoggingTestSuite PRIVATE SOURCETRAIL_APP_PATH="$<TARGET_FILE:Sourcetrail>"
                               SOURCETRAIL_LOMBOK_FIXTURE="${CMAKE_SOURCE_DIR}/testing/cli/lombok")
add_dependencies(JavaLoggingTestSuite Sourcetrail)

# SourceGroupJavaGradle.h includes SingleValueCache.h
target_link_libraries(SourceGroupJavaTestSuite PRIVATE Sourcetrail::core::utility::SingleValueCache)
