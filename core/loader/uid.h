// core/loader/uid.h — engine-assigned asset identity (m1-uid-system, spec
// section 8 / MIDDAY_ENGINE_SPEC.md lines 363-368): asset references
// dual-write {uid, path} — the uid is authoritative (rename-safe, survives
// moves), the path keeps files greppable. A uid is NEVER hand-minted: it is
// born exactly once, by mint_uid(), into a committed `.uid` sidecar next to
// its asset (format: 1, spec lines 373-375). core/loader/uid_registry.h
// rebuilds the uid<->path map by scanning every sidecar under a project
// root; core/loader/asset_ref.h is the {uid, path} YAML ref shape a uid is
// spelled into.
//
// AUTHORING-TIME TOOLING, NOT THE SIM: minting draws real entropy
// (std::random_device) and is only ever reached from `midday check --fix`
// (core/loader/asset_ref.h) — never from core/tick or anything it drives.
// The engine's no-wall-clock/no-unseeded-RNG rule governs sim code; it does
// not bind here. What IS required is that a minted uid, once written, is
// STABLE forever after (the committed sidecar is the source of truth; the
// cache only ever regenerates FROM sidecars) — mint_uid() has no side
// effects of its own, callers commit the value by writing the sidecar.
//
// Textual form: "uid://" + 13 lowercase base-36 digits (0-9a-z), a fixed-
// width, zero-padded encoding of an up-to-64-bit unsigned integer. Godot's
// uid:// alphabet is off by one ("these constants are off by 1, causing the
// 'z' and '9' characters never to be used... cannot be fixed without
// breaking compatibility", resource_uid.cpp) — this scheme has no legacy
// constraint, so it uses the full, uncorrupted 36-symbol alphabet.
// parse_uid_text() is permissive on width (a hand-typed "uid://1" parses
// fine) since textual well-formedness and REGISTRY membership are different
// questions — a well-formed but unregistered uid is exactly the "hand-
// minted" refusal core/loader/asset_ref.h's `midday check` reports.

#pragma once

#include "core/base/error.h"
#include "core/loader/yaml.h"

#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_set>

namespace midday::loader {

// An opaque, engine-assigned asset identity. Default-constructed is
// INVALID (value 0 never mints — see mint_uid).
class Uid {
public:
    Uid() = default;

    [[nodiscard]] static Uid from_value(std::uint64_t value) { return Uid(value); }

    [[nodiscard]] std::uint64_t value() const { return value_; }

    [[nodiscard]] bool valid() const { return value_ != 0; }

    // "uid://" + 13 lowercase base-36 digits, zero-padded.
    [[nodiscard]] std::string text() const;

    friend bool operator==(const Uid&, const Uid&) = default;

private:
    explicit Uid(std::uint64_t value) : value_(value) {}

    std::uint64_t value_ = 0;
};

// Strict "uid://[0-9a-z]+" parse: rejects a missing/mistyped prefix, an
// empty or out-of-alphabet body (uppercase included — the alphabet is
// exactly [0-9a-z]), and a body whose decoded value overflows 64 bits.
// Width is NOT checked here (see the file header) and a decoded value of
// zero is accepted syntactically (Uid::valid() is the separate "is this a
// real, non-placeholder id" question).
[[nodiscard]] std::optional<Uid> parse_uid_text(std::string_view text);

// Non-deterministic entropy source for mint_uid, seeded from
// std::random_device.
using UidRng = std::mt19937_64;
[[nodiscard]] UidRng make_uid_rng();

// Draws a fresh 63-bit id (top bit clear, so it always fits the positive
// range of a signed 64-bit integer too) and retries while it collides with
// `taken` — the same shape as Godot's proven ResourceUID::create_id(),
// minus its documented off-by-one alphabet bug. Uniqueness is NOT merely
// "63 bits is a lot of space": the retry loop checks the actual live
// registry before accepting, so a colliding draw is provably impossible to
// return, however many assets a project accumulates.
[[nodiscard]] Uid mint_uid(const std::unordered_set<std::uint64_t>& taken, UidRng& rng);

// ---- .uid sidecars (committed; spec lines 366-368) -------------------------
// `<asset path>.uid`, e.g. "sprite.png" -> "sprite.png.uid".
[[nodiscard]] std::string sidecar_path_for(std::string_view asset_path);

struct UidSidecar {
    Uid uid;
};

struct SidecarLoadResult {
    std::optional<UidSidecar> sidecar;
    std::optional<base::Error> error; // loader.io / loader.bad_format / loader.unknown_key /
                                      // loader.bad_value / "uid.malformed"
};

// Strict-YAML parse of `{format: 1, uid: "uid://..."}` — reuses the SAME
// loader.* refusal codes and file:line diagnostics every other text format
// uses (core/loader/parse_util.h's check_format/check_keys), plus
// "uid.malformed" when `uid:` parses as a string but not as uid:// syntax.
SidecarLoadResult load_uid_sidecar(const std::string& path);

// Canonical emit (core/loader/yaml_emit.h) -> write_file. Overwrites any
// existing sidecar at `path` (the mint/attach repair path's job).
std::optional<base::Error> write_uid_sidecar(const std::string& path, Uid uid);

} // namespace midday::loader
