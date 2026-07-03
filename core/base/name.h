// core/base/name.h — interned names with content-hashed, order-independent ids.
//
// The guarantee (relied on by journals, goldens, and every serialized id):
//   * Name::id() is XXH3-64 of the name's UTF-8 bytes — a pure content hash.
//     Ids are identical across runs, platforms, threads, and intern order;
//     they are NEVER sequential handout (insertion order must not leak into
//     any output — determinism contract, spec section 4.3).
//   * id 0 is reserved for the empty name (Name() == Name("")). A non-empty
//     text hashing to 0 or to another text's hash is a 64-bit collision:
//     interning detects it and aborts — that is a design-revisit event, not
//     a runtime condition to paper over.
//   * view() returns the interned text; storage lives for the process, so
//     views never dangle. Comparison and ordering use ids only: O(1),
//     deterministic, content-defined (not lexicographic).
//
// Consumers in this milestone: log subsystem/code identifiers (core/base/log.h);
// reflection class/property names and ECS component keys follow in M0.

#pragma once

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

namespace midday::base {

class Name {
public:
    constexpr Name() noexcept = default; // the empty name, id 0

    // Interns `text` (first use per content allocates; later uses are lookups).
    explicit Name(std::string_view text);

    [[nodiscard]] std::uint64_t id() const { return id_; }

    [[nodiscard]] std::string_view view() const {
        return str_ != nullptr ? std::string_view(*str_) : std::string_view();
    }

    [[nodiscard]] bool empty() const { return id_ == 0; }

    friend bool operator==(const Name& a, const Name& b) { return a.id_ == b.id_; }

    friend std::strong_ordering operator<=>(const Name& a, const Name& b) {
        return a.id_ <=> b.id_;
    }

    // The pure hash: XXH3-64 of the bytes; "" maps to 0 by definition, and a
    // nonzero-input hash of 0 is remapped to a fixed constant to keep 0 reserved.
    static std::uint64_t hash_of(std::string_view text);

private:
    std::uint64_t id_ = 0;
    const std::string* str_ = nullptr; // interned storage, process lifetime
};

} // namespace midday::base
