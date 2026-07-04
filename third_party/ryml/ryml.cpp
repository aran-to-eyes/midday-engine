// third_party/ryml/ryml.cpp — the ONE translation unit that instantiates the
// amalgamated rapidyaml definitions (upstream single-header convention:
// exactly one TU defines RYML_SINGLE_HDR_DEFINE_NOW). First-party code
// includes rapidyaml.hpp declaration-only through the `ryml` CMake target;
// the header itself stays byte-pristine (D-BUILD-060 vendoring ethos).
#define RYML_SINGLE_HDR_DEFINE_NOW
#include "rapidyaml.hpp"
