// third_party/vma/vma_impl.cpp -- the single VMA implementation TU
// (upstream-documented usage: define VMA_IMPLEMENTATION in exactly one
// translation unit). Kept inside third_party/ so first-party clang-tidy
// never walks the vendored header body; the deviation from byte-pristine
// vendoring is this 4-line TU, recorded in LICENSES/manifest.json.
#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h"
