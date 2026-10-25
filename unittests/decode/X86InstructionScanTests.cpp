//===- X86InstructionScanTests.cpp - Bounded scan consumers -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve call targets, byte resynchronization and code ownership in batches.
//===----------------------------------------------------------------------===//

#include "FuncDetectorDetail.h"
#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/decode/X86InstructionScan.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <set>
#include <utility>
#include <vector>

namespace neverd {
namespace {

BinaryImage makeScanImage(Arch A) {
  BinaryImage Img;
  Img.Arch = A;
  Img.Bits = A == Arch::X64 ? Bitness::Bits64 : Bitness::Bits32;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x400000;
  Segment Seg;
  Seg.VA = 0x401000;
  Seg.Size = 160;
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Seg.Data.assign(Seg.Size, 0x90);
  const auto Call = [&](size_t From, size_t To) {
    Seg.Data[From] = 0xe8;
    const uint32_t Disp = static_cast<uint32_t>(To - From - 5);
    for (unsigned I = 0; I != 4; ++I)
      Seg.Data[From + 1 + I] = static_cast<uint8_t>(Disp >> (I * 8));
  };
  Call(4, 100);
  Call(30, 110); // Crosses from code into data.
  Call(35, 120); // Entirely in data.
  Call(60, 130);
  const uint8_t Fallback[] = {0x66, 0x48, 0xd9, 0x30, 0xf0, 0x90};
  std::copy(std::begin(Fallback), std::end(Fallback), Seg.Data.begin() + 45);
  Img.Segments.push_back(std::move(Seg));
  const auto SectionAt = [&](uint64_t Start, uint64_t End, bool Code) {
    Section Sec;
    Sec.VA = Img.Segments[0].VA + Start;
    Sec.Size = End - Start;
    Sec.Flags = Code ? SegmentFlags::Readable | SegmentFlags::Executable
                     : SegmentFlags::Readable;
    Sec.Type = Code ? 0x60000020 : 0x40000040;
    Img.Sections.push_back(std::move(Sec));
  };
  SectionAt(0, 33, true);
  SectionAt(33, 40, false);
  SectionAt(40, 160, true);
  return Img;
}

TEST(X86InstructionScan, CallWalkMatchesSingleStepsAtEveryStartAndEnd) {
  for (Arch A : {Arch::X86, Arch::X64}) {
    const BinaryImage Img = makeScanImage(A);
    const Segment &Seg = Img.Segments[0];
    Decoder Single, Batched;
    ASSERT_TRUE(Single.init(A));
    ASSERT_TRUE(Batched.init(A));
    for (Decoder *Dec : {&Single, &Batched}) {
      Dec->setText(false);
      Dec->setDetail(false);
    }
    for (size_t End = 1; End <= Seg.Data.size(); ++End) {
      for (size_t Start = 0; Start < End; ++Start) {
        X86InstructionScan Scan(
            Batched, llvm::ArrayRef<uint8_t>(Seg.Data).take_front(End), Seg.VA);
        auto Known =
            func_detect_detail::codeIntervalAround(Img, Seg.VA + Start);
        auto ExpectedKnown = Known;
        for (va_t At = Seg.VA + Start; At < Seg.VA + End;) {
          const auto Expected = func_detect_detail::stepCallsX86(
              Img, Single, &Seg, At, Seg.VA + End, ExpectedKnown);
          const auto Actual = func_detect_detail::stepCallsX86(
              Img, Batched, &Seg, At, Seg.VA + End, Known, &Scan);
          ASSERT_TRUE(Expected);
          ASSERT_TRUE(Actual);
          EXPECT_EQ(Actual->Addr, Expected->Addr);
          EXPECT_EQ(Actual->Next, Expected->Next);
          EXPECT_EQ(Actual->Target, Expected->Target);
          ASSERT_GT(Actual->Next, At);
          At = Actual->Next;
        }
      }
    }
  }
}

TEST(X86InstructionScan, WiderCacheCannotExtendTheCallSweepBound) {
  for (Arch A : {Arch::X86, Arch::X64}) {
    const BinaryImage Img = makeScanImage(A);
    const Segment &Seg = Img.Segments[0];
    Decoder Dec;
    ASSERT_TRUE(Dec.init(A));
    Dec.setText(false);
    Dec.setDetail(false);
    X86InstructionScan Scan(Dec, Seg.Data, Seg.VA);
    auto Known = func_detect_detail::codeIntervalAround(Img, Seg.VA + 4);
    // The cache can see the complete CALL, but this sweep lacks its last byte.
    const auto Step = func_detect_detail::stepCallsX86(
        Img, Dec, &Seg, Seg.VA + 4, Seg.VA + 8, Known, &Scan);
    ASSERT_TRUE(Step);
    EXPECT_EQ(Step->Next, Seg.VA + 5);
    EXPECT_EQ(Step->Target, InvalidVA);
  }
}

TEST(X86InstructionScan, CallTargetsStillRequireCompleteCodeOwnership) {
  for (Arch A : {Arch::X86, Arch::X64}) {
    const BinaryImage Img = makeScanImage(A);
    const Segment &Seg = Img.Segments[0];
    for (bool Detail : {false, true}) {
      Decoder Dec;
      ASSERT_TRUE(Dec.init(A));
      Dec.setText(Detail);
      Dec.setDetail(Detail);
      std::set<va_t> Targets;
      func_detect_detail::scanSegmentCallsX86(Img, Dec, &Seg, Seg.VA,
                                              Seg.VA + Seg.Size, Targets);
      EXPECT_EQ(Targets, (std::set<va_t>{Seg.VA + 100, Seg.VA + 130}));
      Targets.clear();
      func_detect_detail::scanSegmentCallsX86(Img, Dec, &Seg, Seg.VA,
                                              Seg.VA + 8, Targets);
      EXPECT_TRUE(Targets.empty());
    }
  }
}

} // namespace
} // namespace neverd
