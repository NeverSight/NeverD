//===- JumpTableSharedAbsoluteTests.cpp - Joint absolute dispatch
//----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "JumpTableSourceExecution.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/Support/Error.h"

#include <iterator>
#include <optional>
#include <set>

namespace {

class JumpTableSharedAbsolute : public JumpTableSourceExecution {
protected:
  void SetUp() override {
    NeverDLiftTest::SetUp();
    if (!hasCrossTargetClang())
      GTEST_SKIP() << "cross-target Clang is unavailable";
  }

  fs::path compileFixture(unsigned Case = 0) {
    const auto Source = fs::path(TEST_SOURCE_DIR) /
                        "aarch64/test_jumptable_shared_absolute_a64.S";
    const auto Object = tmpFile("shared-" + std::to_string(Case) + ".o");
    const auto Built =
        exec(NEVERD_TEST_CLANG, {"-target", "aarch64-linux-gnu",
                                 "-DTEST_CASE=" + std::to_string(Case), "-c",
                                 Source.string(), "-o", Object.string()});
    EXPECT_TRUE(Built.ok()) << Built.err;
    return Object;
  }

  static neverd::LowFunc
  build(const neverd::BinaryImage &Image,
        std::optional<size_t> Budget = std::nullopt,
        const std::set<neverd::va_t> *Entries = nullptr,
        const std::set<neverd::va_t> *Protected = nullptr) {
    const auto *Function = Image.findSymbol("shared_absolute");
    EXPECT_NE(Function, nullptr);
    if (!Function)
      return {};
    neverd::Decoder Decoder;
    EXPECT_TRUE(Decoder.init(Image.Arch, Image.Mode));
    neverd::CFGBuilder Builder;
    if (Budget)
      Builder.setMaskFixedPointEvidenceBudgetForTesting(*Budget);
    Builder.setKnownFuncEntries(Entries);
    Builder.setProtectedJumpTableRelocationSlots(Protected);
    return Builder.build(Image, Decoder, Function->Addr, Function->Name);
  }

  static bool hasOpcode(const neverd::LowFunc &Function, neverd::NdOp Opcode) {
    for (const auto &Block : Function.Blocks)
      for (const auto &Op : Block.Ops)
        if (Op.Opcode == Opcode)
          return true;
    return false;
  }

  static void expectUnresolved(const neverd::LowFunc &Low) {
    EXPECT_TRUE(Low.JumpTables.empty());
    EXPECT_TRUE(hasOpcode(Low, neverd::NdOp::INDIR_BR));
    EXPECT_FALSE(hasOpcode(Low, neverd::NdOp::INDIR_CALL));
  }
};

TEST_F(JumpTableSharedAbsolute, PublishesEveryMemberWithItsExactCoordinates) {
  auto Image = neverd::loadBinary(compileFixture().string());
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  const auto *Object = Image->findSymbol("shared_absolute_table");
  ASSERT_NE(Object, nullptr);
  const auto Low = build(*Image);
  ASSERT_EQ(Low.JumpTables.size(), 3u);
  std::set<std::vector<uint32_t>> Domains;
  std::set<neverd::va_t> Suppressed;
  for (const auto &Table : Low.JumpTables) {
    Domains.insert(Table.SlotIndices);
    EXPECT_EQ(Table.BaseAddr, Object->Addr);
    ASSERT_EQ(Table.Targets.size(), 2u);
    EXPECT_EQ(Table.AuthenticatedTableLoads.size(), 1u);
    for (unsigned I = 0; I < Table.SlotIndices.size(); ++I) {
      const auto *Target = Image->findSymbol(
          "shared_absolute_case" + std::to_string(Table.SlotIndices[I]));
      ASSERT_NE(Target, nullptr);
      EXPECT_EQ(Table.Targets[I], Target->Addr);
    }
    Suppressed.insert(Table.SuppressibleRelocationSlots.begin(),
                      Table.SuppressibleRelocationSlots.end());
  }
  EXPECT_EQ(Domains, (std::set<std::vector<uint32_t>>{{2, 3}, {4, 5}, {6, 7}}));
  EXPECT_EQ(Suppressed.size(), 6u);
  EXPECT_EQ(Suppressed.count(Object->Addr), 0u);
  EXPECT_EQ(Suppressed.count(Object->Addr + 8), 0u);
  for (unsigned I : {0u, 1u}) {
    const auto *Target =
        Image->findSymbol("shared_absolute_case" + std::to_string(I));
    ASSERT_NE(Target, nullptr);
    EXPECT_TRUE(Low.ModuleAnalysisRoots.count(Target->Addr));
  }
  EXPECT_TRUE(Low.hasCompleteInstructionLift());
  EXPECT_FALSE(hasOpcode(Low, neverd::NdOp::INDIR_CALL));
}

TEST_F(JumpTableSharedAbsolute, UnknownMutatedOrEscapedMembersPublishNoGroup) {
  for (unsigned Case : {1u, 2u, 3u, 4u, 5u, 6u}) {
    SCOPED_TRACE(Case);
    auto Image = neverd::loadBinary(compileFixture(Case).string());
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    expectUnresolved(build(*Image));
  }
}

TEST_F(JumpTableSharedAbsolute, MissingRelocationCannotSupplyAnEdge) {
  auto Image = neverd::loadBinary(compileFixture().string());
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  const auto *Object = Image->findSymbol("shared_absolute_table");
  ASSERT_NE(Object, nullptr);
  ASSERT_EQ(Image->CodePtrRelocSlots.erase(Object->Addr + 6 * 8), 1u);
  expectUnresolved(build(*Image));
}

TEST_F(JumpTableSharedAbsolute, CallableOrProtectedRootsRetainTheirAuthority) {
  auto Image = neverd::loadBinary(compileFixture().string());
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  const auto *Function = Image->findSymbol("shared_absolute");
  const auto *Case = Image->findSymbol("shared_absolute_case2");
  const auto *Object = Image->findSymbol("shared_absolute_table");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Case, nullptr);
  ASSERT_NE(Object, nullptr);
  const std::set<neverd::va_t> Callable{Function->Addr, Case->Addr};
  expectUnresolved(build(*Image, std::nullopt, &Callable));
  const std::set<neverd::va_t> Protected{Object->Addr + 2 * 8};
  expectUnresolved(build(*Image, std::nullopt, nullptr, &Protected));
}

TEST_F(JumpTableSharedAbsolute, ExhaustedEvidencePublishesNoPartialGroup) {
  auto Image = neverd::loadBinary(compileFixture().string());
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  for (size_t Budget : {size_t{0}, size_t{64}, size_t{4096}}) {
    SCOPED_TRACE(Budget);
    expectUnresolved(build(*Image, Budget));
  }
}

TEST_F(JumpTableSharedAbsolute, BothSourceRoutesExecuteEverySelection) {
  const auto Emulator = executor("aarch64");
  if (!Emulator)
    GTEST_SKIP() << "Execution of AArch64 source requires qemu-aarch64";
  const auto Object = compileFixture();
  const std::string Main = R"C(
int main(void) {
  const uint32_t inputs[] = {0, 1, 2, UINT32_MAX, UINT32_C(0x80000000)};
  for (unsigned i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i)
    for (unsigned j = 0; j < sizeof(inputs) / sizeof(inputs[0]); ++j)
      for (unsigned k = 0; k < sizeof(inputs) / sizeof(inputs[0]); ++k) {
        const uint32_t expected = inputs[i] == 0
            ? (inputs[k] == 0 ? 71 : 53) : (inputs[j] == 0 ? 37 : 19);
        if (shared_absolute(inputs[i], inputs[j], inputs[k]) != expected)
          return 1;
      }
  return 0;
}
)C";
  const auto Original = tmpFile("original-shared-selection.c");
  std::ofstream(Original)
      << "#include <stdint.h>\n"
         "uint32_t shared_absolute(uint32_t, uint32_t, uint32_t);\n"
      << Main;
  ASSERT_NO_FATAL_FAILURE(
      expectSourceRuns(Original, "aarch64", *Emulator, {Object.string()}));
  for (unsigned Route : {0u, 1u, 2u}) {
    SCOPED_TRACE(Route);
    const auto File = tmpFile("shared-selection.c");
    std::vector<std::string> Args = {"decompile", Object.string(),
                                     "--func",    "shared_absolute",
                                     "-o",        File.string()};
    if (Route)
      Args.push_back("--llvm");
    if (Route == 2)
      Args.push_back("--no-opt");
    const auto Recovered = exec(ndBin(), Args);
    ASSERT_TRUE(Recovered.ok()) << Recovered.err;
    std::ifstream Input(File);
    const std::string Source((std::istreambuf_iterator<char>(Input)), {});
    ASSERT_EQ(Source.find("unresolved indirect branch"), std::string::npos)
        << Source;
    std::ofstream Out(File, std::ios::app);
    Out << Main;
    Out.close();
    ASSERT_NO_FATAL_FAILURE(expectSourceRuns(File, "aarch64", *Emulator));
  }
}

} // namespace
