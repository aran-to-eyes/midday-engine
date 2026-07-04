# core/rhi/shadercomp

The ONE shader compilation seam, two outputs: GLSL -> SPIR-V via vendored
glslang (`shader_compiler.cpp`, the only glslang consumer in the tree) and
SPIR-V -> MSL via vendored SPIRV-Cross (`msl_translator.cpp`, the only
SPIRV-Cross consumer — m0-rhi-metal). Deterministic both ways: no optimizer,
no debug info, deterministic-FP contract — same source, same SPIR-V words,
same MSL text, everywhere (rhi.shadercomp selftests pin it). The MSL path
owns the seam's coordinate adaptation: vertex stages get one exact
clip-space y negation (flip_vert_y) so the pinned y-DOWN contract holds
under Metal's y-up NDC.
