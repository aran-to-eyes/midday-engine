# core/ — the engine runtime, one subdirectory per subsystem (spec section 4).
# Subsystems arrive with their plan nodes; only what exists is listed.

add_subdirectory(base)
add_subdirectory(math)
add_subdirectory(reflect)
add_subdirectory(expr)
add_subdirectory(ecs)
add_subdirectory(hierarchy)
add_subdirectory(journal)
add_subdirectory(bus)
add_subdirectory(tick)
add_subdirectory(statechart)
add_subdirectory(physics)
add_subdirectory(loader)
add_subdirectory(input)
add_subdirectory(rhi)
