//===- X86_64_ExtensionInvariantTests.cpp - extension width regressions -===//

#include "NeverDLiftFixture.h"

#include <iterator>
#include <regex>

class X86_64_ExtensionInvariant : public NeverDLiftTest {};

namespace {

fs::path testObj() {
  return fs::path(TEST_OBJ_DIR) / "test_extension_widths.o";
}

} // namespace

TEST_F(X86_64_ExtensionInvariant, EveryIntegerExtensionStrictlyWidens) {
  ASSERT_TRUE(fs::exists(testObj())) << "test_extension_widths.o not built";
  verifyIntegerExtensionsStrictlyWiden(testObj());
}

TEST_F(X86_64_ExtensionInvariant, ByteCarryConsumersSnapshotIncomingCf) {
  auto Run = liftToLowIR(testObj());
  ASSERT_EQ(Run.exitCode, 0) << Run.err;

  const std::regex Snapshot(R"(COPY\s+tmp:0x[0-9A-Fa-f]+:1\s+reg:0xC8:1)");
  const size_t SnapshotCount = static_cast<size_t>(std::distance(
      std::sregex_iterator(Run.out.begin(), Run.out.end(), Snapshot),
      std::sregex_iterator()));
  EXPECT_GE(SnapshotCount, 4U) << Run.out;
}
