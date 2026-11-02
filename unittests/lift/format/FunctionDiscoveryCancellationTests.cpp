//===- FunctionDiscoveryCancellationTests.cpp - Stoppable native scans
//-----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Cancellation through real architecture scanners, publication and retry.
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/FuncDetector.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/ScopeExit.h"

#include <algorithm>
#include <atomic>
#include <gtest/gtest.h>
#include <iterator>
#include <vector>

using namespace neverd;
namespace {
BinaryImage image(Arch Architecture,
                  InstructionMode Mode = InstructionMode::Default) {
  BinaryImage Image;
  Image.Arch = Architecture;
  Image.Mode = Mode;
  Image.Bits = Architecture == Arch::X86 || Architecture == Arch::ARM
                   ? Bitness::Bits32
                   : Bitness::Bits64;
  Image.Format = BinaryFormat::Raw;
  Image.Base = Image.Entry = 0x1000;
  Segment Text;
  Text.VA = Image.Base;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  std::vector<uint8_t> Instruction;
  if (Architecture == Arch::AArch64)
    Instruction = {0xc0, 0x03, 0x5f, 0xd6}; // ret
  else if (Architecture == Arch::ARM)
    Instruction = Mode == InstructionMode::Thumb
                      ? std::vector<uint8_t>{0x70, 0x47} // bx lr
                      : std::vector<uint8_t>{0x1e, 0xff, 0x2f, 0xe1};
  else
    Instruction = {0xc3};
  for (unsigned I = 0; I < 65536; ++I)
    Text.Data.insert(Text.Data.end(), Instruction.begin(), Instruction.end());
  if (Architecture == Arch::X64 || Architecture == Arch::X86) {
    // call entry+16; ret, with an independent callable return at entry+16.
    const uint8_t Call[] = {0xe8, 0x0b, 0, 0, 0};
    std::copy(std::begin(Call), std::end(Call), Text.Data.begin());
  }
  Text.Size = Text.Data.size();
  Image.Segments.push_back(std::move(Text));
  return Image;
}

TEST(FunctionDiscoveryCancellation,
     StopsNativeWalksAndRetriesWithoutPublishingCandidates) {
  const auto PreviousWorkers = workerThreadOverride().load();
  auto RestoreWorkers =
      llvm::scope_exit([&] { setWorkerThreadCount(PreviousWorkers); });
  for (unsigned Workers : {1, 4}) {
    setWorkerThreadCount(Workers);
    for (auto Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
      for (auto Mode : {InstructionMode::Default, InstructionMode::Thumb}) {
        if (Mode == InstructionMode::Thumb && Architecture != Arch::ARM)
          continue;
        SCOPED_TRACE(::testing::Message()
                     << Workers << ":" << unsigned(Architecture) << ":"
                     << unsigned(Mode));
        auto Image = image(Architecture, Mode);
        auto Reference = Image;
        Decoder Dec, Baseline;
        ASSERT_TRUE(Dec.init(Image));
        ASSERT_TRUE(Baseline.init(Reference));
        Dec.setDetail(false);
        Dec.setText(false);
        const auto Verified = Image.VerifiedFunctionEntries;
        FuncDetector Detector;
        std::atomic<unsigned> Calls{0};
        const auto Stopped = Detector.detect(
            Image, Dec, [&] { return Calls.fetch_add(1) >= 64; });
        EXPECT_FALSE(Stopped);
        EXPECT_GE(Calls.load(), 65u);
        EXPECT_EQ(Image.VerifiedFunctionEntries, Verified);
        EXPECT_FALSE(Dec.detailEnabled());
        EXPECT_FALSE(Dec.textEnabled());
        // Reuse both the detector and decoder after stop; compare full results
        // and authoritative entry publication to a fresh uncontrolled scan.
        const auto Retry = Detector.detect(Image, Dec, [] { return false; });
        ASSERT_TRUE(Retry);
        EXPECT_EQ(*Retry, FuncDetector().detect(Reference, Baseline));
        EXPECT_EQ(Image.VerifiedFunctionEntries,
                  Reference.VerifiedFunctionEntries);
      }
    }
  }
}

TEST(FunctionDiscoveryCancellation,
     CancellationBeforeCommitPublishesNoVerifiedEntries) {
  const auto PreviousWorkers = workerThreadOverride().load();
  setWorkerThreadCount(1);
  auto RestoreWorkers =
      llvm::scope_exit([&] { setWorkerThreadCount(PreviousWorkers); });
  auto Image = image(Arch::X64);
  auto Reference = Image;
  Decoder Dec, Baseline;
  ASSERT_TRUE(Dec.init(Image));
  ASSERT_TRUE(Baseline.init(Reference));
  unsigned Calls = 0;
  const auto Expected = FuncDetector().detect(Reference, Baseline, [&] {
    ++Calls;
    return false;
  });
  ASSERT_TRUE(Expected);
  ASSERT_FALSE(Reference.VerifiedFunctionEntries.empty());
  ASSERT_GT(Calls, 1u);
  unsigned Seen = 0;
  EXPECT_FALSE(
      FuncDetector().detect(Image, Dec, [&] { return ++Seen == Calls; }));
  EXPECT_EQ(Seen, Calls);
  EXPECT_TRUE(Image.VerifiedFunctionEntries.empty());
  EXPECT_EQ(FuncDetector().detect(Image, Dec), *Expected);
}

TEST(FunctionDiscoveryCancellation, EmptyCompletionIsDistinctFromCancellation) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Image));
  FuncDetector Detector;
  EXPECT_FALSE(Detector.detect(Image, Dec, [] { return true; }));
  const auto Complete = Detector.detect(Image, Dec, [] { return false; });
  ASSERT_TRUE(Complete);
  EXPECT_TRUE(Complete->empty());
}
} // namespace
