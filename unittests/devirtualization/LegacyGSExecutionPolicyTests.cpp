//===- LegacyGSExecutionPolicyTests.cpp - Concrete GS boundary ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Raw recovery declarations do not enable unmodeled concrete GS state.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "os/windows/driver/WindowsX64ExecutionPolicy.h"

#include <utility>
#include <vector>

using namespace neverd::emulation;

namespace {
TEST(LegacyGSExecutionPolicy, RemainsUnmodeled) {
  const X64BranchModel Models[] = {
#define NEVERD_X64_BRANCH_MODEL(Name, Mode) X64BranchModel::Name,
#include "neverd/emulation/arch/x86_64/X64BranchModels.def"
#undef NEVERD_X64_BRANCH_MODEL
  };
  for (const auto Model : Models) {
    SCOPED_TRACE(static_cast<unsigned>(Model));
    WindowsX64ExecutionPolicy Policy;
    auto Error = Policy.initialize(Model);
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    for (const std::vector<uint8_t> Bytes :
         {std::vector<uint8_t>{0x0f, 0x01, 0xf8},
          std::vector<uint8_t>{0x48, 0x0f, 0x01, 0xf8}}) {
      // Reuse the inspection-cache slot with different live bytes. Ordinary
      // register movement stays admitted; each exchange must still refuse.
      for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
        Error = Policy.validate({0x48, 0x89, 0xc8}, 0x1000);
        ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
        Error = Policy.validate(Bytes, 0x1000);
        ASSERT_TRUE(bool(Error));
        EXPECT_EQ(llvm::toString(std::move(Error)),
                  "unmodeled CPU environment instruction: swapgs");
      }
    }
  }
}
} // namespace
