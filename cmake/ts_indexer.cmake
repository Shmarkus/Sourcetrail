# Stages tools/ts-indexer with its runtime dependencies (npm ci --omit=dev) into bin/app/data/ts-indexer, where the
# TypeScript source group runs it from (build/app/data links to bin/app/data). Node.js runs it at indexing time.
# Without npm nothing is staged: TypeScript source groups then fail to run, and their tests skip.
find_program(NPM_EXECUTABLE npm)
if(NOT NPM_EXECUTABLE)
  message(WARNING "npm not found: the TypeScript indexer is not staged and TypeScript source groups cannot index")
  return()
endif()

set(TS_INDEXER_SOURCE_DIR "${CMAKE_SOURCE_DIR}/tools/ts-indexer")
set(TS_INDEXER_STAGE_DIR "${CMAKE_SOURCE_DIR}/bin/app/data/ts-indexer")
set(TS_INDEXER_SCRIPTS
    convert.mjs
    http.mjs
    index.mjs
    sqlite.mjs)
set(TS_INDEXER_MANIFESTS package.json package-lock.json)

# The scripts are copied one by one; the dependencies are installed again only when the manifests change.
set(TS_INDEXER_OUTPUTS)
foreach(file IN LISTS TS_INDEXER_SCRIPTS)
  add_custom_command(
    OUTPUT "${TS_INDEXER_STAGE_DIR}/${file}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${TS_INDEXER_STAGE_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy "${TS_INDEXER_SOURCE_DIR}/${file}" "${TS_INDEXER_STAGE_DIR}/${file}"
    DEPENDS "${TS_INDEXER_SOURCE_DIR}/${file}"
    VERBATIM)
  list(APPEND TS_INDEXER_OUTPUTS "${TS_INDEXER_STAGE_DIR}/${file}")
endforeach()

set(TS_INDEXER_MANIFEST_SOURCES)
foreach(file IN LISTS TS_INDEXER_MANIFESTS)
  list(APPEND TS_INDEXER_MANIFEST_SOURCES "${TS_INDEXER_SOURCE_DIR}/${file}")
endforeach()
set(TS_INDEXER_STAMP "${TS_INDEXER_STAGE_DIR}/node_modules/.package-lock.json")
add_custom_command(
  OUTPUT "${TS_INDEXER_STAMP}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${TS_INDEXER_STAGE_DIR}"
  COMMAND "${CMAKE_COMMAND}" -E copy ${TS_INDEXER_MANIFEST_SOURCES} "${TS_INDEXER_STAGE_DIR}/"
  COMMAND "${NPM_EXECUTABLE}" ci --prefix "${TS_INDEXER_STAGE_DIR}" --omit=dev --prefer-offline --no-audit --no-fund
          --loglevel=error
  DEPENDS ${TS_INDEXER_MANIFEST_SOURCES}
  COMMENT "Installing the TypeScript indexer's dependencies with npm"
  VERBATIM)
list(APPEND TS_INDEXER_OUTPUTS "${TS_INDEXER_STAMP}")

add_custom_target(ts_indexer DEPENDS ${TS_INDEXER_OUTPUTS})
