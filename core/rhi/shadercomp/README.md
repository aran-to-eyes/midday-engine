# core/rhi/shadercomp

Shader compilation seam: GLSL -> SPIR-V via vendored glslang (the ONLY
glslang consumer in the tree). Deterministic: no optimizer, no debug info,
compiled under the deterministic-FP contract — same source, same SPIR-V
words, everywhere (rhi.shadercomp selftests pin it). SPIRV-Cross
(SPIR-V -> MSL) joins HERE at m0-rhi-metal so both backends share one
front end.
