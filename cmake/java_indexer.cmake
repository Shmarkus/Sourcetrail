# Builds java_indexer/ with Maven and stages java-indexer.jar plus its dependency jars into bin/app/data/java/lib,
# the directory the Java package loads them from (build/app/data links to bin/app/data).
find_program(MAVEN_EXECUTABLE mvn REQUIRED)

set(JAVA_INDEXER_SOURCE_DIR "${CMAKE_SOURCE_DIR}/java_indexer")
set(JAVA_INDEXER_LIB_DIR "${CMAKE_SOURCE_DIR}/bin/app/data/java/lib")
set(JAVA_INDEXER_JAR "${JAVA_INDEXER_LIB_DIR}/java-indexer.jar")

file(
  GLOB_RECURSE JAVA_INDEXER_SOURCES
  CONFIGURE_DEPENDS
  "${JAVA_INDEXER_SOURCE_DIR}/src/*")

add_custom_command(
  OUTPUT "${JAVA_INDEXER_JAR}"
  COMMAND "${CMAKE_COMMAND}" -E rm -rf "${JAVA_INDEXER_SOURCE_DIR}/target/lib"
  COMMAND "${MAVEN_EXECUTABLE}" -q -f "${JAVA_INDEXER_SOURCE_DIR}/pom.xml" package
  COMMAND "${CMAKE_COMMAND}" -E rm -rf "${JAVA_INDEXER_LIB_DIR}"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${JAVA_INDEXER_SOURCE_DIR}/target/lib" "${JAVA_INDEXER_LIB_DIR}"
  COMMAND "${CMAKE_COMMAND}" -E copy "${JAVA_INDEXER_SOURCE_DIR}/target/java-indexer-1.0.jar" "${JAVA_INDEXER_JAR}"
  DEPENDS "${JAVA_INDEXER_SOURCE_DIR}/pom.xml" ${JAVA_INDEXER_SOURCES}
  COMMENT "Building java-indexer.jar with Maven"
  VERBATIM)

add_custom_target(java_indexer_jar DEPENDS "${JAVA_INDEXER_JAR}")
