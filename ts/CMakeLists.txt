# ts/ — the embedded TypeScript tier (spec section 7): QuickJS runtime
# embedding and the vendored-compiler toolchain. ts/lib (the engine-side TS
# library) is data, not a build target; it grows with the binding surface.

add_subdirectory(runtime)
add_subdirectory(toolchain)
add_subdirectory(codegen)
