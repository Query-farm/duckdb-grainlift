cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME grainlift)

set(CMAKE_CXX_STANDARD 17 CACHE STRING "C++ standard to enforce" FORCE)
set(CMAKE_CXX_STANDARD_REQUIRED True)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

include_directories(src/include third_party/adbc/include third_party/nanoarrow/include)

# ---------------------------------------------------------------------------
# grainlift ADBC driver (Rust staticlib)
# ---------------------------------------------------------------------------
include(${CMAKE_CURRENT_LIST_DIR}/cmake/grainlift_rust.cmake)

set(EXTENSION_SOURCES
    src/grainlift_extension.cpp
    src/grainlift_host_http.cpp
    src/adbc_static_driver.cpp
    src/adbc_connection.cpp
    src/adbc_functions.cpp
    src/adbc_scan.cpp
    src/adbc_arrow_types.cpp
    src/adbc_execute.cpp
    src/adbc_insert.cpp
    src/adbc_catalog.cpp
    src/adbc_secrets.cpp
    src/adbc_filter_pushdown.cpp
    src/storage/adbc_catalog.cpp
    src/storage/adbc_connection_pool.cpp
    src/storage/adbc_insert.cpp
    src/storage/adbc_catalog_set.cpp
    src/storage/adbc_schema_set.cpp
    src/storage/adbc_schema_entry.cpp
    src/storage/adbc_table_set.cpp
    src/storage/adbc_table_entry.cpp
    src/storage/adbc_transaction.cpp
    src/storage/adbc_transaction_manager.cpp
    src/storage/adbc_storage.cpp
    src/storage/adbc_clear_cache.cpp
    third_party/nanoarrow/src/nanoarrow.c
)

# Test hook: exercise the thread-free insert path (used by WASM) natively.
option(GRAINLIFT_NO_INSERT_THREAD "Consume inserts synchronously at finalize" OFF)
if(GRAINLIFT_NO_INSERT_THREAD)
    add_compile_definitions(GRAINLIFT_NO_INSERT_THREAD)
endif()

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# The driver is linked into both targets. For WASM the loadable target is a
# static archive and the side module is produced by a post-build emcc step that
# only sees LINKED_LIBS (see extension_config.cmake), so the archive must exist
# before that step runs.
# The WASM side module is relinked only when the extension archive changes;
# tie one object to the Rust archive so a driver change forces that relink.
set_source_files_properties(src/grainlift_extension.cpp PROPERTIES OBJECT_DEPENDS "${GRAINLIFT_RUST_LIB}")
add_dependencies(${EXTENSION_NAME} grainlift_rust_build)
add_dependencies(${LOADABLE_EXTENSION_NAME} grainlift_rust_build)
target_link_libraries(${EXTENSION_NAME} grainlift_rust)
if(NOT EMSCRIPTEN)
    target_link_libraries(${LOADABLE_EXTENSION_NAME} grainlift_rust)
endif()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
