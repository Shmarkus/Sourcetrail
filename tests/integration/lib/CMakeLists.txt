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
  # Run the built Sourcetrail on copies of the Angular + Spring fixture (with the Python one); the Java half needs a JDK 21
  # JAVA_HOME.
  foreach(suite IN ITEMS TypeScriptSourceGroupTestSuite EndpointLinkingTestSuite PythonSourceGroupTestSuite)
    add_sourcetrail_test(
      NAME
      ${suite}
      SOURCES
      ${suite}.cpp
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
      ${suite} PRIVATE SOURCETRAIL_APP_PATH="$<TARGET_FILE:Sourcetrail>"
                       TS_FIXTURE_PATH="${CMAKE_SOURCE_DIR}/testing/cli/angular-spring"
                       PY_FIXTURE_PATH="${CMAKE_SOURCE_DIR}/testing/cli/python")
    add_dependencies(${suite} Sourcetrail)
  endforeach()
endif()

if(BUILD_CXX_LANGUAGE_PACKAGE)
  # Run the built Sourcetrail on a copy of the C++ fixture, with the compilation database CMake writes for it.
  add_sourcetrail_test(
    NAME
    CxxSourceGroupTestSuite
    SOURCES
    CxxSourceGroupTestSuite.cpp
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
    CxxSourceGroupTestSuite
    PRIVATE SOURCETRAIL_APP_PATH="$<TARGET_FILE:Sourcetrail>"
            # MixedProjectTest's own SetUp, which this suite replaces, names the Angular + Spring fixture.
            TS_FIXTURE_PATH="${CMAKE_SOURCE_DIR}/testing/cli/angular-spring"
            CPP_FIXTURE_PATH="${CMAKE_SOURCE_DIR}/testing/cli/cpp"
            CMAKE_COMMAND_PATH="${CMAKE_COMMAND}"
            CMAKE_GENERATOR_NAME="${CMAKE_GENERATOR}"
            CMAKE_MAKE_PROGRAM_PATH="${CMAKE_MAKE_PROGRAM}")
  add_dependencies(CxxSourceGroupTestSuite Sourcetrail)
endif()
