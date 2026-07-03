# tools/ — native developer tools behind CI lanes. Each tool is a thin main
# over a library so its logic is selftest-covered inside the midday binary.

# Explicit binary dirs (<name>.build): the exit-test contract addresses each
# tool binary as build/<preset>/tools/<name>, which would otherwise collide
# with CMake's mirrored object directory of the same name.
add_subdirectory(codegen_bootstrap codegen_bootstrap.build)
