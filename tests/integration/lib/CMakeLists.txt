# ${CMAKE_SOURCE_DIR}/tests/integration/lib/CMakeLists.txt

set(test_lib_names
    RefreshInfoGeneratorTestSuite
    SourceGroupTestSuite
    PersistentStorageTestSuite
    SqliteBookmarkStorageTestSuite
    SqliteDatabaseIndexTestSuite
    SqliteIndexStorageTestSuite
    SqliteStorageTestSuite
    StorageTestSuite)

foreach(test_name IN LISTS test_lib_names)
  add_sourcetrail_test(
    NAME
    ${test_name}
    SOURCES
    ${test_name}.cpp
    DEPS
    GTest::gtest
    GTest::gmock
    Qt6::Core
    Qt6::Gui
    Qt6::Sql
    Sourcetrail::messaging
    Sourcetrail::gtest_main
    Sourcetrail::core::utility::TextAccess
    Sourcetrail::core::utility::file::FileSystem
    Sourcetrail::core::utility::file::utilityFile
    $<$<BOOL:${BUILD_CXX_LANGUAGE_PACKAGE}>:Sourcetrail::lib_cxx>
    lib::mocks
    Boost::date_time
    spdlog::spdlog
    TEST_PREFIX
    "integration.lib."
    WORKING_DIRECTORY
    "${CMAKE_BINARY_DIR}/test/")
endforeach()

if(BUILD_JAVA_LANGUAGE_PACKAGE)
  # Runs the built Sourcetrail on copies of the Angular + Spring fixture; the Java half needs a JDK 21 JAVA_HOME.
  add_sourcetrail_test(
    NAME
    TypeScriptSourceGroupTestSuite
    SOURCES
    TypeScriptSourceGroupTestSuite.cpp
    DEPS
    GTest::gtest
    Qt6::Core
    Qt6::Sql
    Sourcetrail::gtest_main
    TEST_PREFIX
    "integration.lib."
    WORKING_DIRECTORY
    "${CMAKE_BINARY_DIR}/test/")
  target_compile_definitions(
    TypeScriptSourceGroupTestSuite PRIVATE SOURCETRAIL_APP_PATH="$<TARGET_FILE:Sourcetrail>"
                                           TS_FIXTURE_PATH="${CMAKE_SOURCE_DIR}/testing/cli/angular-spring")
  add_dependencies(TypeScriptSourceGroupTestSuite Sourcetrail)
endif()
