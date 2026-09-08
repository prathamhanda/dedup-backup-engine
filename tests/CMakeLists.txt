# Component 10: the four required tests, plus the existing tools/*_check
# binaries registered as additional regression coverage (they were built
# incrementally as their own components landed, and already do real
# verification work -- no reason to leave them unregistered).

# --- new integration tests, through the real pipeline ---
add_executable(roundtrip_test roundtrip_test.cpp)
target_link_libraries(roundtrip_test PRIVATE dedupbackup_core)
add_test(NAME roundtrip COMMAND roundtrip_test)

add_executable(insertion_identity_test insertion_identity_test.cpp)
target_link_libraries(insertion_identity_test PRIVATE dedupbackup_core)
add_test(NAME insertion_identity COMMAND insertion_identity_test)
set_tests_properties(insertion_identity PROPERTIES TIMEOUT 120)

add_test(NAME crash_recovery
         COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/crash_recovery_test.sh"
                 "$<TARGET_FILE:dedup-backup>")
set_tests_properties(crash_recovery PROPERTIES TIMEOUT 120)

# --- WAL torn-write test: tools/wal_check.cpp already does exactly this
#     (see its own header comment and handoff §4.8) -- registering it
#     directly rather than duplicating the logic. ---
add_test(NAME wal_torn_write
         COMMAND wal_check "${CMAKE_CURRENT_BINARY_DIR}/wal_check_scratch")
set_tests_properties(wal_torn_write PROPERTIES TIMEOUT 60)

# --- bonus regression coverage: the other per-component check tools,
#     each already independently verified on real Linux (handoff §4.6-
#     §4.9), now made idempotent (self-cleaning) and wired into CTest so
#     a future change that regresses any of them fails the suite. ---
# (Single-level scratch paths directly under CMAKE_CURRENT_BINARY_DIR,
# deliberately not nested under a "scratch/" subdirectory: each tool's
# own directory setup is a single POSIX mkdir(), not mkdir -p, so it can
# create the leaf directory but not a missing parent -- CMAKE_CURRENT_
# BINARY_DIR itself is guaranteed to already exist.)
add_test(NAME store_roundtrip
         COMMAND store_check "${CMAKE_CURRENT_BINARY_DIR}/store_check_scratch")
add_test(NAME index_dedup
         COMMAND index_check "${CMAKE_CURRENT_BINARY_DIR}/index_check_scratch")
add_test(NAME manifest_format
         COMMAND manifest_check "${CMAKE_CURRENT_BINARY_DIR}/manifest_check_scratch")
