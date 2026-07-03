// testkit/doctest.h — THE way to include doctest in Midday test TUs.
//
// MSVC's <string_view> declares operator<< against an incomplete
// basic_ostream; doctest stringification instantiates it, so every test TU
// needs <ostream> completed BEFORE doctest is pulled in. libc++/libstdc++
// forgive the omission, MSVC does not — this wrapper ends the recurring
// Windows-lane failure class (4th occurrence; see LEDGER).
// Include this, never "doctest/doctest.h" directly.

#pragma once

#include "doctest/doctest.h" // IWYU pragma: export

#include <ostream> // IWYU pragma: keep
