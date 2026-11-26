//===- ELFIntegerExtendedFixture.inc - Assembled extended scalar forms ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent Clang encodings for the physical decoder and semantic tests.
///
//===----------------------------------------------------------------------===//

namespace ppc_fixture {
constexpr uint32_t AddPrefix = 0x06000000;
constexpr uint32_t AddR3Suffix = 0x38600000;
constexpr uint32_t ReservedPrefixes[] = {0x06040000, 0x06400000, 0x04000000};
constexpr uint32_t AddR3R4One = 0x38640001;
} // namespace ppc_fixture
