# testkit/goldens/m0 — M0 render goldens (m0-rhi-vulkan)

Decoded-pixel golden hashes for the three pinned M0 scenes
(`core/rhi/scenes.h`: clear, triangle, textured_quad), minted on the
golden-software CI lane's pinned Mesa lavapipe.

Files once minted:

| file | content |
|---|---|
| `clear.hash`, `triangle.hash`, `textured_quad.hash` | 16-digit lowercase hex, one line: `base::hex64(rhi::pixel_hash(image))` — XXH3-64 over (width LE u32, height LE u32, RGBA bytes). DECODED pixels, never PNG file bytes (Aurora D-14). |
| `DRIVER_PIN.txt` | the exact `DeviceCaps::driver_info` string the hashes were minted on. Hash-equality is only claimed within this driver class (spec section 5 two-tier comparison semantics); every other driver asserts structure, not hashes. |

## Bootstrap / re-mint protocol

1. The `golden-software` CI lane renders the scenes headless on lavapipe and
   compares via `midday rhi render --software --goldens testkit/goldens/m0`.
   With this directory empty it exits 1 (`rhi.golden_missing`) — the lane is
   RED until goldens exist. Never stub it green.
2. Every lane run uploads `m0-goldens-candidate` (PNGs + `.hash` files +
   `driver.txt` + probe/render reports).
3. INSPECT the candidate PNGs with human/agent eyes (AGENTS.md rule 7:
   goldens are born only from inspected images): clear = uniform
   (51,102,153); triangle = red apex top-center, green bottom-right, blue
   bottom-left on that background; textured quad = centered 8x8 red/blue
   checkerboard, top-left cell red-ish (204,51,51).
4. Commit the candidate's `<scene>.hash` files here plus `driver.txt` as
   `DRIVER_PIN.txt`. Re-run the lane: green.
5. Mesa/driver bump on the lane => `rhi.golden_mismatch` or
   `rhi.golden_driver_mismatch` => repeat from step 2. Scene-constant edits
   in `core/rhi/scenes.h` invalidate goldens the same way, by construction.

Local smoke renders (e.g. MoltenVK on a dev Mac) are perceptual evidence
only and must NEVER mint these files — `--software` on the lane plus the
driver pin make that mistake structurally impossible.
