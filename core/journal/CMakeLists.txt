# core/journal — the run.mrj flight recorder (m0-journal): record model,
# deterministic zstd-compressed JSONL writer, validating reader with index
# seek. Format contract: formats/run_mrj.md.

add_library(midday_journal STATIC
    bundle.cpp
    reader.cpp
    record.cpp
    writer.cpp
)
add_library(midday::journal ALIAS midday_journal)

target_link_libraries(midday_journal
    PUBLIC midday::options midday::base
    PRIVATE zstd::zstd xxhash::xxhash)

# Journals are THE determinism byte-compare artifact: the FP contract applies.
midday_apply_deterministic_fp(midday_journal)

# Tests as an OBJECT library so doctest self-registration survives linking
# into the midday binary (D-BUILD-014 pattern).
add_library(midday_journal_tests OBJECT
    reader_test.cpp
    record_test.cpp
    writer_test.cpp
)
target_link_libraries(midday_journal_tests PRIVATE midday::journal doctest::doctest)
midday_apply_deterministic_fp(midday_journal_tests)
