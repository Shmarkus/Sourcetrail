# ${CMAKE_SOURCE_DIR}/src/st/CMakeLists.txt
# st: the agent CLI. Queries read the index through PersistentStorage; indexing runs the adjacent Sourcetrail.
add_library(Sourcetrail_st_core)
add_library(Sourcetrail::st_core ALIAS Sourcetrail_st_core)

target_sources(
  Sourcetrail_st_core
  PRIVATE StArchitecture.cpp
          StBoundaries.cpp
          StClient.cpp
          StCommand.cpp
          StFiles.cpp
          StFilter.cpp
          StFreshness.cpp
          StGaps.cpp
          StGraph.cpp
          StIndexCache.cpp
          StInput.cpp
          StKey.cpp
          StLombok.cpp
          StOutput.cpp
          StProject.cpp
          StQueries.cpp
          StServer.cpp
          StSocket.cpp
          StWatch.cpp)

target_include_directories(Sourcetrail_st_core PUBLIC ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_BINARY_DIR}/src/lib)

target_include_directories(Sourcetrail_st_core SYSTEM PRIVATE ${EXTERNAL_INCLUDE_PATHS})

target_link_libraries(
  Sourcetrail_st_core
  PUBLIC Sourcetrail::lib
         Sourcetrail::lib_java
         Sourcetrail::core::utility::file::FileInfo
         Sourcetrail::core::utility::file::FilePath
         Sourcetrail::core::utility::file::FileSystem
         Sourcetrail::core::utility::TextAccess
         Sourcetrail::core::utility::TimeStamp
         Sourcetrail::core::utility::utilityString
         Qt6::Core
         Qt6::Gui
         Qt6::Network)

add_executable(st StMain.cpp)
target_link_libraries(st PRIVATE Sourcetrail::st_core)
# st runs the Sourcetrail beside it to index.
set_target_properties(st PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/app/")
add_dependencies(st Sourcetrail)

if(ENABLE_UNIT_TEST)
  add_sourcetrail_test(
    NAME
    st_test
    SOURCES
    tests/StTest.cpp
    tests/StQueryTest.cpp
    DEPS
    Sourcetrail::st_core
    TEST_PREFIX
    "unittests.st."
    WORKING_DIRECTORY
    "${CMAKE_BINARY_DIR}/test/")
  target_include_directories(st_test PRIVATE ${CMAKE_CURRENT_LIST_DIR}/tests)
  # StIndex runs the built st, which runs the Sourcetrail beside it, on copies of the Lombok and the Angular + Spring
  # fixtures.
  target_compile_definitions(
    st_test PRIVATE ST_APP_PATH="$<TARGET_FILE:st>" ST_LOMBOK_FIXTURE="${CMAKE_SOURCE_DIR}/testing/cli/lombok"
                    ST_TS_FIXTURE="${CMAKE_SOURCE_DIR}/testing/cli/angular-spring"
                    ST_PY_FIXTURE="${CMAKE_SOURCE_DIR}/testing/cli/python")
  add_dependencies(st_test st)
endif()
