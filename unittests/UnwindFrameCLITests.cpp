//===- UnwindFrameCLITests.cpp - Exact-PC CLI parity ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The CLI must print the complete public C API result at the supplied PC,
/// including null, source-specific states and a present zero signing address.
//===----------------------------------------------------------------------===//

#include "UnwindFrameTestsDetail.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Program.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {
using namespace neverd;
using namespace neverd::aarch64_dwarf_test;
using namespace neverd::unwind_frame_test;

#define NEVERD_UNWIND_CLI_TEST_TEXT(Name, Text) constexpr char Name[] = Text;
#define NEVERD_UNWIND_CLI_TEST_VALUE(Type, Name, Value)                        \
  constexpr Type Name = Value;
#include "UnwindFrameCLIFixtures.def"

std::string readFile(const std::filesystem::path &Path) {
  std::ifstream Stream(Path, std::ios::binary);
  return {std::istreambuf_iterator<char>(Stream), {}};
}

class UnwindFrameCLI : public UnwindFrameTest {
protected:
  int execute(llvm::StringRef Address, bool JSON = true) {
    const std::string Argument =
        (llvm::Twine(kAddressArgument) + Address).str();
    const std::vector<std::string> Arguments = {
        NEVERD_TEST_CLI, kInfo, (Directory / kImageFile).string(), kNoDebug,
        Argument};
    std::vector<llvm::StringRef> Args(Arguments.begin(), Arguments.end());
    if (JSON)
      Args.push_back(kJSON);
    const std::string Output = (Directory / kOutputFile).string();
    const std::string Error = (Directory / kErrorFile).string();
    // ExecuteAndWait may reopen an existing redirect without truncating it.
    // A shorter response must not inherit bytes from the preceding query.
    for (const auto &Path : {Output, Error}) {
      std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
      if (!Stream) {
        ADD_FAILURE() << Path;
        return EXIT_FAILURE;
      }
    }
    const std::array<std::optional<llvm::StringRef>, 3> Redirects = {
        std::nullopt, Output, Error};
    return llvm::sys::ExecuteAndWait(Args.front(), Args, std::nullopt,
                                     Redirects, kQueryTimeoutSeconds);
  }

  void compare(uint64_t Address) {
    const std::string Expected =
        take(neverd_unwind_frame_json(Session, Address));
    const std::string Input = readFile(Directory / kImageFile);
    const std::string Hex = llvm::utohexstr(Address);
    for (const std::string &Spelling : {Hex, kHexPrefix + Hex}) {
      ASSERT_EQ(execute(Spelling), 0) << readFile(Directory / kErrorFile);
      EXPECT_EQ(readFile(Directory / kOutputFile), Expected + '\n');
      EXPECT_EQ(readFile(Directory / kImageFile), Input);
    }
  }
};

TEST_F(UnwindFrameCLI, ExactPCNullAndZeroSigningAddressMatchThePublicQuery) {
  ASSERT_NO_FATAL_FAILURE(load());
  for (uint64_t Address : {uint64_t(0), kImageBase + kCodeOffset,
                           kImageBase + kCodeOffset + kCodeAlign,
                           kImageBase + kCodeOffset + kCodeAlign * 2,
                           kImageBase + kCodeOffset + kRange,
                           std::numeric_limits<uint64_t>::max()})
    ASSERT_NO_FATAL_FAILURE(compare(Address));
  const auto Zero = absoluteState(
      kSignedWithPC, -int64_t((kImageBase + kCodeOffset) / kCodeAlign));
  ASSERT_NO_FATAL_FAILURE(load(Zero));
  ASSERT_NO_FATAL_FAILURE(compare(kImageBase + kCodeOffset));
}

TEST_F(UnwindFrameCLI, AllUnavailableStatusesAndAbsentSourcesRemainUnchanged) {
  const std::vector<uint8_t> Deep(kRememberDepthOverflow, kRememberState);
  for (llvm::ArrayRef<uint8_t> Program :
       {llvm::ArrayRef<uint8_t>(kDynamic), llvm::ArrayRef<uint8_t>(kReserved),
        llvm::ArrayRef<uint8_t>(kLateUnderflow),
        llvm::ArrayRef<uint8_t>(Deep)}) {
    ASSERT_NO_FATAL_FAILURE(load(Program));
    ASSERT_NO_FATAL_FAILURE(compare(kImageBase + kCodeOffset));
  }
  ImageOptions Short;
  Short.DwarfRange = kShortDwarfRange;
  ASSERT_NO_FATAL_FAILURE(load(kStates, Short));
  ASSERT_NO_FATAL_FAILURE(compare(kImageBase + kCodeOffset + kShortDwarfRange));
  ImageOptions Other;
  Other.AArch64 = false;
  Other.DwarfFallback = true;
  ASSERT_NO_FATAL_FAILURE(load({}, Other));
  ASSERT_NO_FATAL_FAILURE(compare(kImageBase + kCodeOffset));
  ImageOptions Ordinary;
  Ordinary.Dwarf = false;
  Ordinary.CompactEncoding = kOrdinaryCompact;
  ASSERT_NO_FATAL_FAILURE(load({}, Ordinary));
  ASSERT_NO_FATAL_FAILURE(compare(kImageBase + kCodeOffset));
  ImageOptions Empty;
  Empty.Dwarf = Empty.Compact = false;
  ASSERT_NO_FATAL_FAILURE(load({}, Empty));
  ASSERT_NO_FATAL_FAILURE(compare(kImageBase + kCodeOffset));
}

TEST_F(UnwindFrameCLI, RejectsMalformedAddressesAndRequiresJSON) {
  ASSERT_NO_FATAL_FAILURE(load());
  const std::string Input = readFile(Directory / kImageFile);
#define NEVERD_UNWIND_CLI_TEST_INVALID(Text) EXPECT_NE(execute(Text), 0);
#include "UnwindFrameCLIFixtures.def"
  EXPECT_NE(execute(kEntryAddress, false), 0);
  EXPECT_EQ(readFile(Directory / kImageFile), Input);
}

} // namespace
