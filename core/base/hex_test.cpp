// base.hex.* — the canonical hex spellings (hex.h).
//
// Under test: hex64's pinned 16-digit lowercase form, bytes_to_hex /
// hex_to_bytes as exact inverses, and the strict-parse contract — odd
// length, uppercase, or non-hex input refuses with nullopt (the journal
// embedding of canonical payload bytes rides these helpers, so the
// spelling must stay canonical in BOTH directions).

#include "core/base/hex.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

using midday::base::bytes_to_hex;
using midday::base::hex64;
using midday::base::hex_to_bytes;

TEST_CASE("base.hex: hex64 is 16 lowercase digits, zero-padded") {
    CHECK(hex64(0) == "0000000000000000");
    CHECK(hex64(0xF) == "000000000000000f");
    CHECK(hex64(0xDEADBEEFCAFE1234ULL) == "deadbeefcafe1234");
    CHECK(hex64(~0ULL) == "ffffffffffffffff");
}

TEST_CASE("base.hex: bytes_to_hex spells two lowercase digits per byte") {
    CHECK(bytes_to_hex({}).empty());
    const std::vector<std::byte> bytes{
        std::byte{0x00}, std::byte{0x01}, std::byte{0xAB}, std::byte{0xFF}};
    CHECK(bytes_to_hex(bytes) == "0001abff");
}

TEST_CASE("base.hex: hex_to_bytes is the strict inverse of bytes_to_hex") {
    std::vector<std::byte> bytes;
    bytes.reserve(256);
    for (unsigned value = 0; value < 256; ++value)
        bytes.push_back(static_cast<std::byte>(value));
    const std::string hex = bytes_to_hex(bytes);
    CHECK(hex.size() == 512);

    auto parsed = hex_to_bytes(hex);
    CHECK(midday::testkit::unwrap(parsed) == bytes);

    auto empty = hex_to_bytes("");
    CHECK(midday::testkit::unwrap(empty).empty());
}

TEST_CASE("base.hex: hex_to_bytes refuses every non-canonical spelling") {
    CHECK(!hex_to_bytes("abc").has_value());   // odd length
    CHECK(!hex_to_bytes("AB").has_value());    // uppercase is not canonical
    CHECK(!hex_to_bytes("0g").has_value());    // non-hex digit
    CHECK(!hex_to_bytes("0x41").has_value());  // no prefixes
    CHECK(!hex_to_bytes(" 4").has_value());    // no whitespace tolerance
    CHECK(!hex_to_bytes("4\n41").has_value()); // control characters refuse
}
