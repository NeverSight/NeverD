//===- JumpTableCountLaneTests.cpp - Bit-count selector lanes -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Regressions for bit-count selector lanes and generated jump-table code.
///
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"

#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/support/BinaryLoading.h"

namespace {

class JumpTableCountLane : public NeverDLiftTest {};

fs::path countLaneObject() {
  return fs::path(TEST_OBJ_DIR) / "test_jumptable_count_lane.o";
}

TEST_F(JumpTableCountLane, ByteGuardsRecoverOnlyUnchangedScalarCounts) {
  auto ImageOrErr = neverd::loadBinary(countLaneObject());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const auto &Image = *ImageOrErr;
  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  for (const char *Operation : {"ctz", "pop", "lz"}) {
    for (const char *Guard : {"byte", "full"}) {
      for (unsigned Width : {32u, 64u}) {
        const std::string Name = std::string("jt_") + Operation + "_" + Guard +
                                 std::to_string(Width);
        SCOPED_TRACE(Name);
        const auto *Function = Image.findSymbol(Name);
        ASSERT_NE(Function, nullptr);
        neverd::CFGBuilder Builder;
        const auto Low = Builder.build(Image, Decoder, Function->Addr, Name);
        EXPECT_EQ(Low.JumpTables.size(), 1u);
        if (Low.JumpTables.size() != 1)
          continue;
        EXPECT_EQ(Low.JumpTables.front().Targets.size(), 4u);
        EXPECT_EQ(Low.JumpTables.front().SlotIndices,
                  (std::vector<uint32_t>{0, 1, 2, 3}));
        EXPECT_TRUE(Low.UnsafeIndirectBranchAddresses.empty());

        neverd::CFGBuilder Exhausted;
        Exhausted.setMaskFixedPointEvidenceBudgetForTesting(0);
        EXPECT_TRUE(Exhausted.build(Image, Decoder, Function->Addr, Name)
                        .JumpTables.empty());
      }
    }
  }
  for (const char *Name : {"jt_count_or_high32", "jt_count_or_high64",
                           "jt_count_write_high32", "jt_count_write_high64"}) {
    SCOPED_TRACE(Name);
    const auto *Function = Image.findSymbol(Name);
    ASSERT_NE(Function, nullptr);
    neverd::CFGBuilder Builder;
    EXPECT_TRUE(
        Builder.build(Image, Decoder, Function->Addr, Name).JumpTables.empty());
  }
}

TEST_F(JumpTableCountLane, GeneratedCExecutesZeroAndAllBitPositions) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Clang is required to execute generated C";
  for (unsigned Operation = 0; Operation != 3; ++Operation) {
    for (unsigned Width : {32u, 64u}) {
      const char *Names[] = {"ctz", "pop", "lz"};
      // The relative-table HighC route and all three absolute-table source
      // routes share the same independently checked selector formula.
      for (unsigned Route : {0u, 1u, 2u, 3u}) {
        const std::string Name = std::string("jt_") + Names[Operation] +
                                 "_byte" + std::to_string(Width) +
                                 (Route ? "_absolute" : "");
        SCOPED_TRACE(Name + ":" + std::to_string(Route));
        const auto File = tmpFile(Name + "-" + std::to_string(Route) + ".c");
        std::vector<std::string> Args = {
            "decompile",  countLaneObject().string(), "--func", Name, "-o",
            File.string()};
        if (Route >= 2)
          Args.push_back("--llvm");
        if (Route == 3)
          Args.push_back("--no-opt");
        const auto Recovered = exec(ndBin(), Args);
        ASSERT_TRUE(Recovered.ok()) << Recovered.err;
        std::ofstream Out(File, std::ios::app);
        Out << R"(
static unsigned reference_count(uint64_t x) {
  unsigned result = 0;
)";
        if (Width == 32)
          Out << "x &= UINT32_MAX;\n";
        if (Operation == 0)
          Out << "while (result < " << Width
              << " && ((x >> result) & 1) == 0) ++result;\n";
        else if (Operation == 1)
          Out << "while (x) { result += x & 1; x >>= 1; }\n";
        else
          Out << "while (result < " << Width << " && ((x >> (" << Width - 1
              << " - result)) & 1) == 0) ++result;\n";
        Out << "return result; }\n"
               "static int check(uint64_t x) {\n"
               "unsigned count = reference_count(x);\n"
               "return "
            << Name << "(x) != (count <= 3 ? 8100 + count : 8999); }\n"
            << R"(
int main(void) {
  if (check(0) || check(UINT64_MAX)) return 1;
  for (unsigned i = 0; i < 64; ++i)
    if (check(UINT64_C(1) << i) || check(~(UINT64_C(1) << i))) return 2;
  for (uint64_t x = 0; x < 256; ++x)
    if (check(x) || check(x << 56)) return 3;
  uint64_t x = 1;
  for (unsigned i = 0; i < 1024; ++i) {
    x = x * UINT64_C(6364136223846793005) + 1;
    if (check(x)) return 4;
  }
  return 0;
}
)";
        Out.close();
        for (const char *Optimization : {"-O0", "-O2"}) {
          SCOPED_TRACE(Optimization);
          const auto Exe =
              tmpFile(Name + "-" + std::to_string(Route) + Optimization +
                      neverd::test::executableSuffix());
          const auto Built = exec(
              NEVERD_TEST_CLANG,
              {"-std=c11", Optimization, "-fsanitize=undefined",
               "-fsanitize-trap=undefined", File.string(), "-o", Exe.string()});
          ASSERT_TRUE(Built.ok()) << Built.err;
          const auto Run = exec(Exe.string(), {});
          EXPECT_TRUE(Run.ok()) << Run.err;
        }
      }
    }
  }
}

} // namespace
