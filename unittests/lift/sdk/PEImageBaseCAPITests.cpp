//===- PEImageBaseCAPITests.cpp - Session C of PE header walkers ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A session's single-function HighC and LLVM C, whose LLVM module refers to
/// the original image storage, reach a PE image's own headers through the
/// linker's `__ImageBase` too.  An incomplete AArch64 page address of the
/// headers no longer refuses the LLVM route.
//===----------------------------------------------------------------------===//

#include "../../loader/PEHeaderFixture.h"
#include "NeverDLiftFixture.h"

#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/ADT/StringExtras.h"

#include <fstream>
#include <regex>
#include <string>

namespace {

struct SessionOwner {
  neverd_session_t Value = neverd_session_create();
  ~SessionOwner() { neverd_session_destroy(Value); }
};

std::string takeString(const char *Value) {
  if (!Value)
    return {};
  std::string Result(Value);
  neverd_free_string(Value);
  return Result;
}

class PEImageBaseCAPITest : public NeverDLiftTest {};

TEST_F(PEImageBaseCAPITest, SessionRoutesNameTheHeadersThroughImageBase) {
  using namespace llvm::COFF;
  for (const uint16_t Machine :
       {uint16_t(IMAGE_FILE_MACHINE_I386), uint16_t(IMAGE_FILE_MACHINE_AMD64),
        uint16_t(IMAGE_FILE_MACHINE_ARM64)})
    for (bool Unmapped : {false, true}) {
      SCOPED_TRACE(::testing::Message() << Machine << ':' << Unmapped);
      const auto W = neverd::test::peImageBaseWalker(Machine, Unmapped);
      const auto Image = tmpFile("walker-" + std::to_string(Machine) +
                                 (Unmapped ? "-unmapped" : "-mapped") + ".exe");
      std::ofstream(Image, std::ios::binary) << W.Bytes;
      SessionOwner Session;
      ASSERT_NE(Session.Value, nullptr);
      ASSERT_EQ(neverd_session_load(Session.Value, Image.string().c_str()), 1)
          << neverd_last_error(Session.Value);
      for (bool LLVM : {false, true}) {
        SCOPED_TRACE(LLVM ? "llvm" : "high");
        const std::string C =
            takeString(LLVM ? neverd_decompile_llvm(Session.Value, W.Entry)
                            : neverd_decompile(Session.Value, W.Entry));
        ASSERT_FALSE(C.empty()) << neverd_last_error(Session.Value);
        const std::string Declaration = "extern unsigned char __ImageBase[];";
        EXPECT_NE(C.find(Declaration), std::string::npos) << C;
        EXPECT_EQ(C.find(Declaration), C.rfind(Declaration)) << C;
        for (const uint64_t Stale : {W.Base, W.Base + 0x3c, W.Base + 0x80})
          for (bool Lower : {false, true})
            EXPECT_EQ(C.find("0x" + llvm::utohexstr(Stale, Lower)),
                      std::string::npos)
                << C;
        // The header reads survive: neither "MZ" nor e_lfanew (0x80) folds.
        EXPECT_NE(C.find("0x5A4D"), std::string::npos) << C;
        EXPECT_FALSE(std::regex_search(C, std::regex("\\b0[xX]80\\b"))) << C;
        EXPECT_EQ(C.find("\"MZ"), std::string::npos) << C;
        EXPECT_EQ(C.find("neverd.not-decompiled"), std::string::npos) << C;
      }
    }
}

} // namespace
