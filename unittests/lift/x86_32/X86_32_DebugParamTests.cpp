//===- X86_32_DebugParamTests.cpp - i386 debug parameters by location -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A pointer of an i386 DWARF unit is four bytes, its address size, so the
/// parameter after it arrives in the next stack slot.  Each recovered
/// parameter is named and typed after the source parameter there; read as
/// eight bytes, a pointer took two slots and every later name moved.
///
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"

#include <fstream>
#include <iterator>

class X86_32_DebugParams : public NeverDLiftTest {};

TEST_F(X86_32_DebugParams, ParametersAfterAPointerTakeTheNextSlot) {
  const fs::path Object = fs::path(TEST_OBJ_DIR) / "test_debug_params32.o";
  ASSERT_TRUE(fs::exists(Object)) << Object;
  const auto CFile = tmpFile("debug_params32.c");
  const auto R =
      exec(ndBin(), {"decompile", "-o", CFile.string(), Object.string()});
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::ifstream Ifs(CFile);
  ASSERT_TRUE(Ifs.good()) << CFile;
  const std::string Source((std::istreambuf_iterator<char>(Ifs)),
                           std::istreambuf_iterator<char>());
  for (const char *Definition :
       {"int32_t dp32_sum(uint8_t* buf, int32_t len, int32_t bias)",
        "int32_t dp32_area(Window* w, int32_t scale)"})
    EXPECT_NE(Source.find(Definition), std::string::npos) << Definition << "\n"
                                                          << Source;
  EXPECT_EQ(Source.find("buf_4"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("w_4"), std::string::npos) << Source;
}
