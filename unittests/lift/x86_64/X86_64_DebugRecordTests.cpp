//===- X86_64_DebugRecordTests.cpp - Debug records C cannot spell ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// DWARF can describe a record C has no layout for, such as one with bit
/// fields. Pointers retain the debug name; by-value parameters and results
/// retain their machine carriers until a complete C layout is available.
///
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"

#include <fstream>
#include <iterator>

class X86_64_DebugRecords : public NeverDLiftTest {};

static fs::path debugRecordsObj() {
  return fs::path(TEST_OBJ_DIR) / "test_debug_records.o";
}

TEST_F(X86_64_DebugRecords, RecordsCCannotSpellKeepMachineTypes) {
  ASSERT_TRUE(fs::exists(debugRecordsObj())) << debugRecordsObj();
  const auto CFile = tmpFile("debug_records.c");
  const auto R = exec(
      ndBin(), {"decompile", "-o", CFile.string(), debugRecordsObj().string()});
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::ifstream Ifs(CFile);
  ASSERT_TRUE(Ifs.good()) << CFile;
  const std::string Source((std::istreambuf_iterator<char>(Ifs)),
                           std::istreambuf_iterator<char>());
  // The record argument arrives in two registers; each is named for the
  // offset of the bytes it holds.
  for (const char *Definition :
       {"flags_sum(Flags* f)", "flags_mode(int64_t f, int64_t f_8)",
        "flags_walk("})
    EXPECT_NE(Source.find(Definition), std::string::npos) << Source;
  // The returned record keeps the recovered result type, and says why.
  EXPECT_NE(Source.find("return=Flags"), std::string::npos) << Source;
  EXPECT_NE(Source.find("__int128 flags_make(int32_t value, uint32_t kind)"),
            std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("__fastcall"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("Flags* result"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("nd_record"), std::string::npos) << Source;

  for (const char *Function : {"flags_walk", "flags_make", "flags_mode"}) {
    SCOPED_TRACE(Function);
    const auto One = exec(
        ndBin(), {"decompile", "--func", Function, debugRecordsObj().string()});
    EXPECT_EQ(One.exitCode, 0) << One.err;
    EXPECT_NE(One.out.find(Function), std::string::npos) << One.out;
  }

#if defined(__x86_64__) && !defined(_WIN32)
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "no C compiler to check the recovered return carriers";
  // The high return register contains value and both bit fields. Losing it
  // produces compilable C but changes every result of this source oracle.
  std::ofstream(CFile, std::ios::app)
      << "\nint main(void) { for (int x = -17; x != 20; ++x) "
         "if (flags_walk(x) != 6 * x + 15) return 1; return 0; }\n";
  for (const char *Optimization : {"-O0", "-O2"}) {
    const auto Executable = tmpFile("debug_records_oracle");
    const auto Built = exec(NEVERD_TEST_CLANG,
                            {"-std=c11", Optimization, "-fno-strict-aliasing",
                             CFile.string(), "-o", Executable.string()});
    ASSERT_TRUE(Built.ok()) << Built.err << Source;
    const auto Run = exec(Executable.string(), {});
    EXPECT_TRUE(Run.ok()) << "exit " << Run.exitCode << Source;
  }
#endif
}
