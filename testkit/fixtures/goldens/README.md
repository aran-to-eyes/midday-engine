# Golden-compare fixture triplet (m0-golden-compare)

Four tiny 24x24 PNGs, one base + three counterparts — one per comparison
verdict of the two-tier contract (`midday shot compare`, core/rhi/compare.h):

| pair                | tier 1 (decoded-pixel hash) | tier 2 (tolerance) | exit |
|---------------------|-----------------------------|--------------------|------|
| base vs `identical` | PASS                        | PASS               | 0    |
| base vs `noise`     | FAIL                        | PASS (max Δ = 1)   | 0    |
| base vs `shifted`   | FAIL                        | FAIL               | 1    |

`identical.png` holds the SAME pixels as `base.png` encoded through a pinned
alternate encoder configuration (forced None row filter, deflate level 3):
**different file bytes, identical decoded pixels** — the committed proof of
Aurora D-14 (hashes cover decoded pixels, never encodings). `noise.png` flips
the LSB of every third byte (the driver-noise class). `shifted.png` moves the
8x8 square by (+3,+2) (structural damage).

These files are GENERATED, never hand-edited: the single source of truth is
`testkit/compare_fixtures.h`. The verify gate regenerates them
(`MIDDAY_COMPARE_FIXTURE_DIR=<dir> midday selftest --filter compare.fixtures`)
and byte-compares against the committed files, then drives all three pairs
through the real CLI (journal `greppable.mrj` precedent). To re-mint after a
deliberate generator change, run the same command pointed at this directory
and commit the result together with the generator edit.
