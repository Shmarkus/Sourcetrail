# Stages tools/ts-indexer with its runtime dependencies (npm ci --omit=dev) into bin/app/data/ts-indexer, where the
# TypeScript source group runs it from (build/app/data links to bin/app/data). Node.js runs it at indexing time.
find_program(NPM_EXECUTABLE npm REQUIRED)

set(TS_INDEXER_SOURCE_DIR "${CMAKE_SOURCE_DIR}/tools/ts-indexer")
set(TS_INDEXER_STAGE_DIR "${CMAKE_SOURCE_DIR}/bin/app/data/ts-indexer")
set(TS_INDEXER_FILES
    convert.mjs
    http.mjs
    index.mjs
    package.json
    package-lock.json
    sqlite.mjs)
set(TS_INDEXER_STAMP "${TS_INDEXER_STAGE_DIR}/node_modules/.package-lock.json")

set(TS_INDEXER_SOURCES)
set(TS_INDEXER_COPY_COMMANDS)
foreach(file IN LISTS TS_INDEXER_FILES)
  list(APPEND TS_INDEXER_SOURCES "${TS_INDEXER_SOURCE_DIR}/${file}")
  list(
    APPEND
    TS_INDEXER_COPY_COMMANDS
    COMMAND
    "${CMAKE_COMMAND}"
    -E
    copy
    "${TS_INDEXER_SOURCE_DIR}/${file}"
    "${TS_INDEXER_STAGE_DIR}/${file}")
endforeach()

add_custom_command(
  OUTPUT "${TS_INDEXER_STAMP}"
  COMMAND "${CMAKE_COMMAND}" -E rm -rf "${TS_INDEXER_STAGE_DIR}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${TS_INDEXER_STAGE_DIR}" ${TS_INDEXER_COPY_COMMANDS}
  COMMAND "${NPM_EXECUTABLE}" ci --prefix "${TS_INDEXER_STAGE_DIR}" --omit=dev --prefer-offline --no-audit --no-fund
          --loglevel=error
  DEPENDS ${TS_INDEXER_SOURCES}
  COMMENT "Staging the TypeScript indexer with npm"
  VERBATIM)

add_custom_target(ts_indexer DEPENDS "${TS_INDEXER_STAMP}")
