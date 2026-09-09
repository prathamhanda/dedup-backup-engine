cmake_minimum_required(VERSION 3.16)
project(dedup-backup CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

find_package(OpenSSL REQUIRED)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Core library: every piece of logic except main(). main.cpp is kept separate
# so the CLI, the test binaries, and the tools/ binaries can all link against
# the same compiled objects instead of recompiling them per target.
# ---------------------------------------------------------------------------
add_library(dedupbackup_core STATIC
    src/chunker/fastcdc_chunker.cpp
    src/chunker/fixed_chunker.cpp
    src/hasher/sha256_hasher.cpp
    src/store/chunk_store.cpp
    src/index/chunk_index.cpp
    src/wal/wal.cpp
    src/manifest/manifest.cpp
    src/repo_meta/repo_meta.cpp
    src/pipeline/repository.cpp
    src/pipeline/backup_pipeline.cpp
    src/pipeline/restore_pipeline.cpp
    src/pipeline/verify.cpp
    src/stats/stats.cpp
)
target_include_directories(dedupbackup_core PUBLIC include)
target_link_libraries(dedupbackup_core PUBLIC OpenSSL::Crypto Threads::Threads)

add_executable(dedup-backup src/main.cpp)
target_link_libraries(dedup-backup PRIVATE dedupbackup_core)

add_executable(chunk_stats tools/chunk_stats.cpp)
target_link_libraries(chunk_stats PRIVATE dedupbackup_core)

add_executable(chunk_identity tools/chunk_identity.cpp)
target_link_libraries(chunk_identity PRIVATE dedupbackup_core)

add_executable(store_check tools/store_check.cpp)
target_link_libraries(store_check PRIVATE dedupbackup_core)

add_executable(index_check tools/index_check.cpp)
target_link_libraries(index_check PRIVATE dedupbackup_core)

add_executable(wal_check tools/wal_check.cpp)
target_link_libraries(wal_check PRIVATE dedupbackup_core)

add_executable(manifest_check tools/manifest_check.cpp)
target_link_libraries(manifest_check PRIVATE dedupbackup_core)

enable_testing()
add_subdirectory(tests)
