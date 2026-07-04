/* third_party/stb/stb_image_impl.c -- the single stb_image implementation
   TU (upstream-documented usage: define STB_IMAGE_IMPLEMENTATION in exactly
   one translation unit; stb_image_write_impl.c precedent). Kept in
   third_party/ so the vendored header body stays out of first-party lint;
   recorded in LICENSES/manifest.json.

   Configuration (m0-golden-compare): PNG is the only decoded format --
   golden/compare transport is PNG by contract, and compiling the other
   decoders out removes their entire attack/maintenance surface. STBI_NO_STDIO
   keeps core/base/file_io.h the tree's single file seam (decode happens
   from memory via stbi_load_from_memory). */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"
