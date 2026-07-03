# core/bus — the keyed, journaled, immediate event bus (m0-event-bus):
# registration-order dispatch, key-scoped privacy, cascade depth cap,
# cause-id journal records, ECS component adaptor.

add_library(midday_bus STATIC
    bus.cpp
)
add_library(midday::bus ALIAS midday_bus)

target_link_libraries(midday_bus
    PUBLIC midday::options midday::base midday::ecs midday::journal midday::reflect)

# The bus is THE transition mechanism of the sim: the deterministic-FP
# contract applies.
midday_apply_deterministic_fp(midday_bus)

# Tests as an OBJECT library so doctest self-registration survives linking
# into the midday binary (D-BUILD-014 pattern).
add_library(midday_bus_tests OBJECT
    bus_journal_test.cpp
    bus_test.cpp
)
target_link_libraries(midday_bus_tests PRIVATE midday::bus doctest::doctest)
midday_apply_deterministic_fp(midday_bus_tests)
