# cli/verbs

One file per verb. A verb file contains (a) an internal run function taking framework-validated `VerbArgs` — typed access only, no argv parsing — and (b) a public `<name>_spec()` returning its declarative `VerbSpec` (name, summary, flag/positional schema in reflect TypeDesc spellings). Register the spec with one line in `registry.cpp` (the manifest; declaration order = canonical output order, D-BUILD-037) and one source entry in `cli/CMakeLists.txt` — the framework itself never changes.

Every verb emits the JSON envelope defined by `formats/cli_envelope.schema.json` with exit codes 0 ok / 1 failure / 2 usage / 3 validation. Usage errors (unknown flag, missing required, type mismatch) are produced by the framework before the verb runs; help (human and `--json`) is generated from the metadata by `cli/help.h`.
