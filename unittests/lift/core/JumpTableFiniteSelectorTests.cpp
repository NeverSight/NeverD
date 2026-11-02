//===- JumpTableFiniteSelectorTests.cpp - Exact finite selectors ----------===//
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

namespace {

class JumpTableFiniteSelector : public JumpTableSourceExecution {
protected:
  void SetUp() override {
    NeverDLiftTest::SetUp();
    if (!hasCrossTargetClang())
      GTEST_SKIP() << "cross-target Clang is unavailable";
  }

  fs::path compileFixture(const std::string &Arch, unsigned Case) {
    const fs::path Source =
        fs::path(TEST_SOURCE_DIR) /
        (Arch == "aarch64" ? "aarch64/test_jumptable_finite_selector_a64.S"
                           : "x86_64/test_jumptable_finite_selector.S");
    const fs::path Object = tmpFile(Arch + "-" + std::to_string(Case) + ".o");
    const auto Built =
        exec(NEVERD_TEST_CLANG, {"-target", Arch + "-linux-gnu",
                                 "-DTEST_CASE=" + std::to_string(Case), "-c",
                                 Source.string(), "-o", Object.string()});
    EXPECT_TRUE(Built.ok()) << Built.err;
    return Object;
  }

  fs::path compileReloadFixture(bool I386) {
    const std::string Stem = I386 ? "i386" : "x64";
    const auto Source = tmpFile(Stem + ".s");
    const auto Object = tmpFile(Stem + ".o");
    std::ofstream Out(Source);
    Out << ".text\n.globl finite_reload\n"
           ".type finite_reload,@function\nfinite_reload:\n"
           "  test %esi,%esi\n  jne .Lbounded\n";
    if (I386)
      Out << "  mov 4(%esp),%edx\n";
    const char *Address = I386 ? "(%edx)" : "(%rdi)";
    Out << "  cmpl $6," << Address << "\n  jne .Ldefault\n";
    Out << "  mov " << Address << ",%eax\n";
    auto Dispatch = [&] {
      if (I386)
        Out << "  jmp *finite_reload_table(,%eax,4)\n";
      else
        Out << "  lea finite_reload_table(%rip),%r8\n"
               "  jmp *(%r8,%rax,8)\n";
    };
    Dispatch();
    Out << ".Lbounded:\n";
    if (I386)
      Out << "  mov 4(%esp),%edx\n";
    Out << "  mov " << Address << ",%eax\n"
        << "  cmp $6,%eax\n  ja .Ldefault\n";
    Dispatch();
    for (unsigned I = 0; I < 7; ++I) {
      Out << ".Lcase" << I << ":\n";
      if (I == 6)
        Out << "  call finite_reload_callee\n";
      Out << "  mov $" << I + 10 << ",%eax\n  ret\n";
    }
    Out << ".Ldefault:\n  mov $99,%eax\n  ret\n"
           ".size finite_reload,.-finite_reload\n"
           ".section .rodata\n.p2align 3\n"
           ".type finite_reload_table,@object\n"
           "finite_reload_table:\n";
    for (unsigned I = 0; I < 7; ++I)
      Out << (I386 ? "  .long " : "  .quad ") << ".Lcase" << I << '\n';
    Out << ".size finite_reload_table,.-finite_reload_table\n"
           ".text\n.globl finite_reload_callee\n"
           ".type finite_reload_callee,@function\n"
           "finite_reload_callee:\n  ret\n"
           ".size finite_reload_callee,.-finite_reload_callee\n"
           ".section .note.GNU-stack,\"\",@progbits\n";
    Out.close();
    const auto Built =
        exec(NEVERD_TEST_CLANG,
             {"-target", I386 ? "i386-linux-gnu" : "x86_64-linux-gnu", "-c",
              Source.string(), "-o", Object.string()});
    EXPECT_TRUE(Built.ok()) << Built.err;
    return Object;
  }

  static neverd::LowFunc build(const neverd::BinaryImage &Image,
                               const std::string &Name,
                               std::optional<size_t> Budget = std::nullopt) {
    const auto *Function = Image.findSymbol(Name);
    EXPECT_NE(Function, nullptr);
    if (!Function)
      return {};
    neverd::Decoder Decoder;
    EXPECT_TRUE(Decoder.init(Image.Arch, Image.Mode));
    neverd::CFGBuilder Builder;
    if (Budget)
      Builder.setMaskFixedPointEvidenceBudgetForTesting(*Budget);
    return Builder.build(Image, Decoder, Function->Addr, Function->Name);
  }

  static bool hasOpcode(const neverd::LowFunc &Function, neverd::NdOp Opcode) {
    for (const auto &Block : Function.Blocks)
      for (const auto &Op : Block.Ops)
        if (Op.Opcode == Opcode)
          return true;
    return false;
  }
};

TEST_F(JumpTableFiniteSelector, ExactSingletonReplayPreservesAProvedSibling) {
  for (bool I386 : {false, true}) {
    SCOPED_TRACE(I386);
    auto Image = neverd::loadBinary(compileReloadFixture(I386));
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    for (bool Exhausted : {false, true}) {
      SCOPED_TRACE(Exhausted);
      const auto Low =
          build(*Image, "finite_reload",
                Exhausted ? std::optional<size_t>{1} : std::nullopt);
      EXPECT_TRUE(hasOpcode(Low, neverd::NdOp::INDIR_BR));
      if (Exhausted) {
        EXPECT_TRUE(Low.JumpTables.empty());
        EXPECT_TRUE(Low.EverPublishedJumpTableBranchAddresses.empty());
        continue;
      }
      // Discarding the singleton's exact replay can poison the shared graph
      // and lose its independently bounded sibling. The unmodelled call in
      // case 6 still prevents publication of the singleton itself.
      ASSERT_EQ(Low.JumpTables.size(), 1u);
      EXPECT_EQ(Low.JumpTables.front().SlotIndices,
                (std::vector<uint32_t>{0, 1, 2, 3, 4, 5, 6}));
      EXPECT_EQ(Low.JumpTables.front().Targets.size(), 7u);
      EXPECT_EQ(Low.UnsafeIndirectBranchAddresses.size(), 1u);
    }
  }
}

TEST_F(JumpTableFiniteSelector, SparseSelectionPublishesOnlyFeasibleSlots) {
  for (const std::string Arch : {"aarch64", "x86_64"})
    for (unsigned Case : {0u, 1u}) {
      SCOPED_TRACE(Arch + ":" + std::to_string(Case));
      auto Image = neverd::loadBinary(compileFixture(Arch, Case).string());
      ASSERT_TRUE(static_cast<bool>(Image))
          << llvm::toString(Image.takeError());
      const std::string Name = Case == 0 ? "finite_ro" : "finite_rw";
      const auto Low = build(*Image, Name);
      ASSERT_EQ(Low.JumpTables.size(), 1u);
      const auto &Table = Low.JumpTables.front();
      EXPECT_EQ(Table.CaseLabels, (std::vector<int64_t>{2, 3}));
      EXPECT_TRUE(Table.HasDispatchSlotMap);
      EXPECT_EQ(Table.SlotIndices, (std::vector<uint32_t>{2, 3}));
      const auto *Two = Image->findSymbol(Name + "_case2");
      const auto *Three = Image->findSymbol(Name + "_case3");
      ASSERT_NE(Two, nullptr);
      ASSERT_NE(Three, nullptr);
      EXPECT_EQ(Table.Targets,
                (std::vector<neverd::va_t>{Two->Addr, Three->Addr}));
      EXPECT_FALSE(hasOpcode(Low, neverd::NdOp::INDIR_CALL));
    }
}

TEST_F(JumpTableFiniteSelector,
       PhysicalCapacityDoesNotLimitExactSelectorProof) {
  for (const std::string Arch : {"aarch64", "x86_64"})
    for (unsigned Case : {7u, 10u}) {
      const std::string Name = Case == 7 ? "finite_large" : "finite_large_rw";
      SCOPED_TRACE(Arch + ":" + Name);
      auto Image = neverd::loadBinary(compileFixture(Arch, Case).string());
      ASSERT_TRUE(static_cast<bool>(Image))
          << llvm::toString(Image.takeError());
      const auto *Storage = Image->findSymbol(Name + "_table");
      ASSERT_NE(Storage, nullptr);
      ASSERT_EQ(Storage->Size, 96u * 8);
      const auto Low = build(*Image, Name);
      ASSERT_EQ(Low.JumpTables.size(), 1u);
      EXPECT_EQ(Low.JumpTables.front().CaseLabels,
                (std::vector<int64_t>{2, 3}));
      EXPECT_EQ(Low.JumpTables.front().SlotIndices,
                (std::vector<uint32_t>{2, 3}));
    }
}

TEST_F(JumpTableFiniteSelector,
       UnselectedForeignPrefixDoesNotOwnTheDispatchDomain) {
  for (const std::string Arch : {"aarch64", "x86_64"}) {
    SCOPED_TRACE(Arch);
    auto Image = neverd::loadBinary(compileFixture(Arch, 11).string());
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    const auto Low = build(*Image, "finite_foreign_prefix");
    ASSERT_EQ(Low.JumpTables.size(), 1u);
    const auto &Table = Low.JumpTables.front();
    EXPECT_EQ(Table.SlotIndices, (std::vector<uint32_t>{2, 3}));
    const auto *Callback = Image->findSymbol("finite_callback");
    ASSERT_NE(Callback, nullptr);
    EXPECT_EQ(
        std::find(Table.Targets.begin(), Table.Targets.end(), Callback->Addr),
        Table.Targets.end());
    EXPECT_FALSE(hasOpcode(Low, neverd::NdOp::INDIR_CALL));
  }
}

TEST_F(JumpTableFiniteSelector,
       FeasibleForeignTargetsCannotBecomeLocalSwitchCases) {
  for (const std::string Arch : {"aarch64", "x86_64"}) {
    SCOPED_TRACE(Arch);
    auto Image = neverd::loadBinary(compileFixture(Arch, 12).string());
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    const auto Low = build(*Image, "finite_foreign_selected");
    EXPECT_TRUE(Low.JumpTables.empty());
    EXPECT_TRUE(hasOpcode(Low, neverd::NdOp::INDIR_BR));
    EXPECT_FALSE(hasOpcode(Low, neverd::NdOp::INDIR_CALL));
  }
  auto Image = neverd::loadBinary(compileFixture("aarch64", 13).string());
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  const auto Low = build(*Image, "finite_foreign_recurrent");
  EXPECT_TRUE(Low.JumpTables.empty());
  EXPECT_TRUE(hasOpcode(Low, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(hasOpcode(Low, neverd::NdOp::INDIR_CALL));
}

TEST_F(JumpTableFiniteSelector,
       AFeasibleSlotAboveTheQueryCeilingCannotDisappear) {
  for (const std::string Arch : {"aarch64", "x86_64"}) {
    SCOPED_TRACE(Arch);
    auto Image = neverd::loadBinary(compileFixture(Arch, 9).string());
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    const auto Low = build(*Image, "finite_above_ceiling");
    EXPECT_FALSE(hasOpcode(Low, neverd::NdOp::INDIR_CALL));
    if (Low.JumpTables.empty()) {
      EXPECT_TRUE(hasOpcode(Low, neverd::NdOp::INDIR_BR));
      continue;
    }
    // Another complete proof may recover the larger domain. A bounded query
    // may never truncate it to the single low slot that fits its bit mask.
    ASSERT_EQ(Low.JumpTables.size(), 1u);
    EXPECT_EQ(Low.JumpTables.front().CaseLabels, (std::vector<int64_t>{2, 66}));
    EXPECT_EQ(Low.JumpTables.front().SlotIndices,
              (std::vector<uint32_t>{2, 66}));
  }
}

TEST_F(JumpTableFiniteSelector,
       AQueryCeilingDoesNotBoundAnUnknownLargeSelector) {
  for (const std::string Arch : {"aarch64", "x86_64"}) {
    auto Image = neverd::loadBinary(compileFixture(Arch, 8).string());
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    const auto Low = build(*Image, "finite_large_unknown");
    EXPECT_TRUE(Low.JumpTables.empty());
    EXPECT_TRUE(hasOpcode(Low, neverd::NdOp::INDIR_BR));
    EXPECT_FALSE(hasOpcode(Low, neverd::NdOp::INDIR_CALL));
  }
}

TEST_F(JumpTableFiniteSelector, UnknownOrChangedSelectorsDoNotPublish) {
  const char *Names[] = {"finite_unknown_arm", "finite_bypassed",
                         "finite_clobbered"};
  for (const std::string Arch : {"aarch64", "x86_64"})
    for (unsigned I = 0; I < std::size(Names); ++I) {
      SCOPED_TRACE(Arch + ":" + Names[I]);
      auto Image = neverd::loadBinary(compileFixture(Arch, I + 2).string());
      ASSERT_TRUE(static_cast<bool>(Image))
          << llvm::toString(Image.takeError());
      const auto Low = build(*Image, Names[I]);
      EXPECT_TRUE(Low.JumpTables.empty());
      EXPECT_TRUE(hasOpcode(Low, neverd::NdOp::INDIR_BR));
      EXPECT_FALSE(hasOpcode(Low, neverd::NdOp::INDIR_CALL));
    }
}

TEST_F(JumpTableFiniteSelector,
       WritableDestinationMutationCannotPublishSource) {
  // The module-wide writer audit runs after the candidate-local CFG builder.
  // Exercise the publication boundary, including both C routes.
  for (const std::string Arch : {"aarch64", "x86_64"}) {
    const fs::path Object = compileFixture(Arch, 5);
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(Arch + (LLVM ? ":llvm" : ":high"));
      const auto File = tmpFile("mutated.c");
      std::vector<std::string> Args = {"decompile", Object.string(),
                                       "--func",    "finite_mutated",
                                       "-o",        File.string()};
      if (LLVM)
        Args.push_back("--llvm");
      const auto Recovered = exec(ndBin(), Args);
      if (Recovered.ok()) {
        std::ifstream Input(File);
        const std::string Source((std::istreambuf_iterator<char>(Input)), {});
        EXPECT_NE(Source.find("__builtin_trap"), std::string::npos);
      }
    }
  }
}

TEST_F(JumpTableFiniteSelector, ReachedBackedgeCannotReuseTheEntryDomain) {
  auto Image = neverd::loadBinary(compileFixture("aarch64", 6).string());
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  const auto Low = build(*Image, "finite_recurrent");
  EXPECT_TRUE(Low.JumpTables.empty());
  EXPECT_TRUE(hasOpcode(Low, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(hasOpcode(Low, neverd::NdOp::INDIR_CALL));
}

TEST_F(JumpTableFiniteSelector, ExhaustedEvidenceDoesNotPublish) {
  for (const std::string Arch : {"aarch64", "x86_64"}) {
    auto Image = neverd::loadBinary(compileFixture(Arch, 0).string());
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    for (size_t Budget : {size_t{0}, size_t{64}}) {
      SCOPED_TRACE(Arch + ":" + std::to_string(Budget));
      const auto Low = build(*Image, "finite_ro", Budget);
      EXPECT_TRUE(Low.JumpTables.empty());
      EXPECT_TRUE(hasOpcode(Low, neverd::NdOp::INDIR_BR));
      EXPECT_FALSE(hasOpcode(Low, neverd::NdOp::INDIR_CALL));
    }
  }
}

class JumpTableFiniteSelectorSource
    : public JumpTableFiniteSelector,
      public ::testing::WithParamInterface<const char *> {};

TEST_P(JumpTableFiniteSelectorSource,
       BothSourceRoutesExecuteTheOriginalSelection) {
  const std::string Arch = GetParam();
  const auto Emulator = executor(Arch);
  if (!Emulator)
    GTEST_SKIP() << "Execution of " << Arch << " source requires qemu-" << Arch;
  for (unsigned Case : {0u, 1u, 7u, 10u, 11u}) {
    const fs::path Object = compileFixture(Arch, Case);
    const std::string Name = Case == 0    ? "finite_ro"
                             : Case == 1  ? "finite_rw"
                             : Case == 7  ? "finite_large"
                             : Case == 10 ? "finite_large_rw"
                                          : "finite_foreign_prefix";
    const std::string Main =
        "\nint main(void) {\n"
        "const uint32_t inputs[] = {0, 1, 2, 99, UINT32_MAX, "
        "UINT32_C(0x80000000)};\n"
        "for (unsigned i = 0; i < sizeof(inputs)/sizeof(inputs[0]); ++i)"
        "if (" +
        Name +
        "(inputs[i]) != (inputs[i] == 0 ? 99 : 41)) return 1;\n"
        "return 0; }\n";
    const auto Original = tmpFile("original-selection.c");
    std::ofstream(Original)
        << "#include <stdint.h>\nuint32_t " << Name << "(uint32_t);\n"
        << Main;
    ASSERT_NO_FATAL_FAILURE(
        expectSourceRuns(Original, Arch, *Emulator, {Object.string()}));
    for (unsigned Route : {0u, 1u, 2u}) {
      SCOPED_TRACE(Arch + ":" + Name + ":" + std::to_string(Route));
      const auto File = tmpFile("selection.c");
      std::vector<std::string> Args = {
          "decompile", Object.string(), "--func", Name, "-o", File.string()};
      if (Route != 0)
        Args.push_back("--llvm");
      if (Route == 2)
        Args.push_back("--no-opt");
      const auto Recovered = exec(ndBin(), Args);
      ASSERT_TRUE(Recovered.ok()) << Recovered.err;
      std::ifstream Input(File);
      const std::string Source((std::istreambuf_iterator<char>(Input)), {});
      ASSERT_EQ(Source.find("unresolved indirect branch"), std::string::npos);
      std::ofstream Out(File, std::ios::app);
      Out << Main;
      Out.close();
      ASSERT_NO_FATAL_FAILURE(expectSourceRuns(File, Arch, *Emulator));
    }
  }
}

INSTANTIATE_TEST_SUITE_P(NativeTargets, JumpTableFiniteSelectorSource,
                         ::testing::Values("aarch64", "x86_64"));

} // namespace
