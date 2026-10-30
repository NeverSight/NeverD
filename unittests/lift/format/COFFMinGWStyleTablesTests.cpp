//===- COFFMinGWStyleTablesTests.cpp - tables among the code --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Lifts a linked i686 PE whose linker layout matches MinGW's: a callable
/// dispatch table read through call [base+idx*s], and a dense switch whose
/// jump table the linker places in .rdata.  Every table entry is
/// relocation-covered, so the lift must resolve the tables-among-code
/// handling instead of demanding a per-slot callable identity.
///
//===----------------------------------------------------------------------===//

#include "../NeverDLiftFixture.h"

#include <cstring>
#include <string>

#ifndef NEVERD_MINGW_STYLE_TABLES_FIXTURE
#define NEVERD_MINGW_STYLE_TABLES_FIXTURE ""
#endif

namespace {

size_t countDefines(const std::string &IR) {
  size_t Count = 0;
  for (size_t P = IR.find("define"); P != std::string::npos;
       P = IR.find("define", P + 1))
    ++Count;
  return Count;
}

} // namespace

TEST_F(NeverDLiftTest, LiftsMinGWStyleDispatchAndJumpTables) {
  if (NEVERD_MINGW_STYLE_TABLES_FIXTURE[0] == '\0')
    GTEST_SKIP() << "lld-link unavailable";

  const fs::path Exe = NEVERD_MINGW_STYLE_TABLES_FIXTURE;
  ASSERT_TRUE(fs::exists(Exe)) << "missing fixture: " << Exe;

  const RunResult R = liftToLLVMIR(Exe);
  ASSERT_TRUE(R.ok()) << R.err;
  EXPECT_FALSE(R.errContains("error:")) << R.err;

  // The dispatch table's entries stay functions, the switch's cases stay
  // their function's labels: exactly the five real functions, no
  // case-block pseudo-functions.
  EXPECT_EQ(countDefines(R.out), 5u) << R.out;
}
