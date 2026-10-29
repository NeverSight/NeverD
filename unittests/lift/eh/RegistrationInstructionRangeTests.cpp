//===- RegistrationInstructionRangeTests.cpp - PE32 instruction ownership
//--===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reject padding guesses inside instructions without swallowing real entries.
//===----------------------------------------------------------------------===//

#include "../../../lib/loader/COFF/eh/COFFRegistrationEHDetail.h"
#include "../core/MappedSectionFlags.h"
#include "gtest/gtest.h"

namespace {
using namespace neverd;
using namespace neverd::coff_loader::registration_detail;

BinaryImage codeImage(std::vector<uint8_t> Bytes) {
  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::COFF;
  Image.Entry = 0x1000;
  Segment Text;
  Text.VA = Image.Entry;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = std::move(Bytes);
  Text.Size = Text.FileSz = Text.Data.size();
  Section Code;
  Code.Name = ".text";
  Code.VA = Text.VA;
  Code.Size = Code.FileSz = Text.Size;
  Code.Flags = Text.Flags;
  Image.Sections.push_back(Code);
  Image.Segments.push_back(std::move(Text));
  return Image;
}

TEST(RegistrationInstructionRange,
     BranchesAndCallsCannotSplitAnInstructionOperand) {
  struct Case {
    const char *Name;
    std::vector<uint8_t> Bytes;
    unsigned Boundary;
    bool Disproved;
  };
  const Case Cases[] = {
      {"branch to call",
       {0x85, 0xc0, 0x74, 1, 0x90, 0xe8, 0xcc, 0xe9, 0xfe, 0xff, 0xc3},
       7,
       true},
      {"call then operand",
       {0xe8, 0x40, 0, 0, 0, 0xc7, 0x86, 0xcc, 0x33, 0, 0, 0, 0, 0, 0, 0xc3},
       8,
       true},
      {"branch past return",
       {0xeb, 1, 0xc3, 0xc7, 0x86, 0xcc, 0x33, 0, 0, 0, 0, 0, 0, 0xc3},
       6,
       true},
      {"loop before operand",
       {0x85, 0xc0, 0x75, 0xfc, 0xc7, 0x86, 0xcc, 0x33, 0, 0, 0, 0, 0, 0, 0xc3},
       7,
       true},
      {"return before operand",
       {0xc3, 0xc7, 0x86, 0xcc, 0x33, 0, 0, 0, 0, 0, 0, 0xc3},
       4,
       false},
      {"direct jump operand", {0xeb, 0x40, 0xc3}, 1, true},
      {"indirect jump operand", {0xff, 0xe0, 0xc3}, 1, false},
      {"indirect exit",
       {0xff, 0xe0, 0xc7, 0x86, 0xcc, 0x33, 0, 0, 0, 0, 0, 0, 0xc3},
       5,
       false},
      {"unreachable operand",
       {0xeb, 11, 0x90, 0xc7, 0x86, 0xcc, 0x33, 0, 0, 0, 0, 0, 0, 0xc3},
       6,
       false},
      {"exact entry after call",
       {0xe8, 0x40, 0, 0, 0, 0xc7, 0x86, 0xcc, 0x33, 0, 0, 0, 0, 0, 0, 0xc3},
       5,
       false},
      {"explicit conflicting entry",
       {0x75, 4, 0x90, 0xc7, 0x86, 0xcc, 0x33, 0, 0, 0, 0, 0, 0, 0xc3},
       6,
       false},
      {"overlapping instructions",
       {0x75, 2, 0x90, 0xc7, 0x86, 0xcc, 0x33, 0, 0, 0, 0, 0, 0, 0xc3},
       6,
       false},
  };
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Name);
    auto Image = codeImage(C.Bytes);
    const va_t Boundary = Image.Entry + C.Boundary;
    auto Guess = Symbol::makeFunc(Boundary);
    Guess.IsBoundaryGuess = true;
    Image.Symbols.push_back(Guess);
    size_t Work = 0;
    EXPECT_EQ(disprovesCallbackBoundary(Image, Image.Entry, Boundary, Work),
              C.Disproved);
    const size_t RequiredWork = Work;
    ASSERT_GT(RequiredWork, 0u);
    const auto Range = FunctionRangeMap(Image).find(Image, Image.Entry, true);
    ASSERT_TRUE(Range);
    EXPECT_EQ(Range->End,
              C.Disproved ? Image.Entry + C.Bytes.size() : Boundary);
    // Even an operand-looking address is retained when a symbol or export
    // states an independent entry. Loading cannot revoke that authority.
    Image.Symbols.back().Name = "stated_entry";
    const auto Stated = FunctionRangeMap(Image).find(Image, Image.Entry, true);
    ASSERT_TRUE(Stated);
    EXPECT_EQ(Stated->End, Boundary);
    Work = limits::kMaxRegistrationEHStateWork - RequiredWork + 1;
    EXPECT_FALSE(disprovesCallbackBoundary(Image, Image.Entry, Boundary, Work));
    EXPECT_LE(Work, limits::kMaxRegistrationEHStateWork);
  }
}

TEST(RegistrationInstructionRange,
     BoundedAbsoluteJumpTableDisprovesAnInteriorGuess) {
  // cmp eax, 1; ja default; jmp [eax*4+table]. Case 0's immediate covers the
  // guess. A slot that names the guess, or a jump with no bound, keeps it.
  std::vector<uint8_t> Bytes = {
      0x83, 0xf8, 0x01, 0x0f, 0x87, 0x16, 0x00, 0x00, 0x00, 0xff, 0x24, 0x85,
      0x28, 0x10, 0x00, 0x00, 0xc7, 0x86, 0xcc, 0x33, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0xc3, 0xcc, 0xcc, 0xcc, 0xcc, 0xc3, 0xcc, 0xcc, 0xcc, 0xcc,
      0xcc, 0xcc, 0xcc, 0xcc, 0x10, 0x10, 0x00, 0x00, 0x1f, 0x10, 0x00, 0x00};
  ASSERT_EQ(Bytes.size(), 48u);
  auto Image = codeImage(Bytes);
  const va_t Boundary = Image.Entry + 0x14;
  auto Guess = Symbol::makeFunc(Boundary);
  Guess.IsBoundaryGuess = true;
  Image.Symbols.push_back(Guess);
  size_t Work = 0;
  EXPECT_TRUE(disprovesCallbackBoundary(Image, Image.Entry, Boundary, Work));
  const auto Range = FunctionRangeMap(Image).find(Image, Image.Entry, true);
  ASSERT_TRUE(Range);
  EXPECT_EQ(Range->End, Image.Entry + Bytes.size());

  Bytes[40] = 0x14;
  auto NamesBoundary = codeImage(Bytes);
  NamesBoundary.Symbols.push_back(Guess);
  Work = 0;
  EXPECT_FALSE(disprovesCallbackBoundary(NamesBoundary, NamesBoundary.Entry,
                                         Boundary, Work));
  const auto Kept = FunctionRangeMap(NamesBoundary)
                        .find(NamesBoundary, NamesBoundary.Entry, true);
  ASSERT_TRUE(Kept);
  EXPECT_EQ(Kept->End, Boundary);

  Bytes[40] = 0x10;
  Bytes[0] = 0x90;
  Bytes[1] = 0x90;
  Bytes[2] = 0x90;
  auto Unbounded = codeImage(Bytes);
  Unbounded.Symbols.push_back(Guess);
  Work = 0;
  EXPECT_FALSE(
      disprovesCallbackBoundary(Unbounded, Unbounded.Entry, Boundary, Work));
}

TEST(RegistrationInstructionRange,
     OperandPaddingByteDisprovesTheFollowingPrologueGuess) {
  // je +0 crosses control. `mov eax, [ebp-0x34]` ends in CC, and `xor ebx, ebx`
  // is a prologue byte, so the padding scan guesses a function there. A call
  // whose displacement ends in CC still leaves the next entry in place, as
  // does `push ebp` after a real int3.
  auto Image = codeImage(
      {0x74, 0x00, 0x8b, 0x45, 0xcc, 0x33, 0xdb, 0xc3, 0xcc, 0x55, 0xc3});
  const va_t Xor = Image.Entry + 5;
  const va_t Next = Image.Entry + 9;
  for (va_t Boundary : {Xor, Next}) {
    auto Guess = Symbol::makeFunc(Boundary);
    Guess.IsBoundaryGuess = true;
    Image.Symbols.push_back(Guess);
  }
  size_t Work = 0;
  EXPECT_TRUE(disprovesCallbackBoundary(Image, Image.Entry, Xor, Work));
  EXPECT_FALSE(disprovesCallbackBoundary(Image, Image.Entry, Next, Work));
  const auto Range = FunctionRangeMap(Image).find(Image, Image.Entry, true);
  ASSERT_TRUE(Range);
  EXPECT_EQ(Range->End, Next);

  auto Call = codeImage({0xe8, 0x00, 0x00, 0x00, 0xcc, 0x55, 0xc3});
  auto CallGuess = Symbol::makeFunc(Call.Entry + 5);
  CallGuess.IsBoundaryGuess = true;
  Call.Symbols.push_back(CallGuess);
  Work = 0;
  EXPECT_FALSE(
      disprovesCallbackBoundary(Call, Call.Entry, CallGuess.Addr, Work));
  const auto CallRange = FunctionRangeMap(Call).find(Call, Call.Entry, true);
  ASSERT_TRUE(CallRange);
  EXPECT_EQ(CallRange->End, CallGuess.Addr);
}

TEST(RegistrationInstructionRange,
     ImageEntryCallAndTailJumpRetainsTheTargetBoundary) {
  for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
    SCOPED_TRACE(Mutation);
    // The image entry calls a helper, then transfers to an unnamed body.
    // A padding guess before that body cannot own its registration record.
    auto Image = codeImage({0xe8, 5, 0, 0, 0, 0xe9, 6, 0, 0, 0, 0xc3, 0xcc,
                            0xcc, 0xcc, 0xcc, 0xcc, 0xc3});
    constexpr va_t Target = 0x1010;
    auto Guess = Symbol::makeFunc(0x100e);
    Guess.IsBoundaryGuess = true;
    Image.Symbols.push_back(Guess);
    switch (Mutation) {
    case 1: {
      auto Ordinary = Symbol::makeFunc(Image.Entry);
      Ordinary.Name = "ordinary_call_then_jump";
      Image.Symbols.push_back(Ordinary);
      Image.Entry = 0x100a;
      break;
    }
    case 2:
      Image.Segments.front().Flags =
          Image.Segments.front().Flags | SegmentFlags::Writable;
      break;
    case 3:
      setMappedSectionFlags(Image, Image.Sections.front(),
                            Image.Sections.front().Flags |
                                SegmentFlags::Writable);
      break;
    case 4:
      Image.BaseRelocations.push_back({0x1001, 3});
      break;
    case 5:
      Image.Segments.front().FileSz = 9;
      break;
    case 6:
      Image.Segments.push_back(Image.Segments.front());
      break;
    case 7:
      Image.Segments.front().Data[5] = 0x90;
      break;
    case 8:
      Image.IsRelocatable = true;
      break;
    case 9:
      Image.Segments.front().Data[6] = 0xf6;
      Image.Segments.front().Data[7] = 0xef;
      Image.Segments.front().Data[8] = 0xff;
      Image.Segments.front().Data[9] = 0xff;
      break;
    }
    const auto Range = FunctionRangeMap(Image).find(Image, Target, true);
    ASSERT_TRUE(Range);
    EXPECT_EQ(Range->Begin, Mutation == 0 ? Target : Guess.Addr);
    EXPECT_FALSE(Image.hasAuthenticatedFunctionEntryAt(Target));
  }
}

TEST(RegistrationInstructionRange, BodyExtentIncludesEveryRuntimeEntry) {
  // A direct helper follows the parent's return with no padding or symbol.
  // The disconnected callback is also an owner, but the called helper is not.
  auto Image =
      codeImage({0xe8, 1, 0, 0, 0, 0xc3, 0x90, 0xc3, 0x90, 0xc3, 0x90, 0xc3});
  const ExceptionAddressRange Range{Image.Entry, Image.Entry + 12};
  size_t Work = 0;
  EXPECT_EQ(checkedRegistrationRangeEnd(Image, Range, {Image.Entry}, Work),
            Image.Entry + 6);
  Work = 0;
  EXPECT_EQ(checkedRegistrationRangeEnd(Image, Range,
                                        {Image.Entry, Image.Entry + 8}, Work),
            Image.Entry + 10);
  const size_t RequiredWork = Work;
  Work = limits::kMaxRegistrationEHStateWork - RequiredWork + 1;
  EXPECT_FALSE(checkedRegistrationRangeEnd(
      Image, Range, {Image.Entry, Image.Entry + 8}, Work));

  struct Case {
    std::vector<uint8_t> Bytes;
    std::vector<va_t> Roots;
  };
  for (const auto &C : {
           Case{{0xff, 0xe0, 0xc3}, {0x1000}}, // Unresolved jump.
           Case{{0xeb, 0x20, 0xc3}, {0x1000}}, // Escaping branch.
           Case{{0x90, 0x90}, {0x1000}},       // Unclosed fallthrough.
           Case{{0xe8, 0, 0, 0, 0, 0xc3}, {0x1000, 0x1001}},
           Case{{0xc3}, {0x1001}}, // Root outside the range.
           Case{{0xcb}, {0x1000}}, // Far return.
       }) {
    auto Bad = codeImage(C.Bytes);
    Work = 0;
    EXPECT_FALSE(checkedRegistrationRangeEnd(
        Bad, {Bad.Entry, Bad.Entry + C.Bytes.size()}, C.Roots, Work));
  }
}

TEST(RegistrationInstructionRange, ClosedCallbackOwnsItsLocalBranchTargets) {
  for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image =
        codeImage({0x75, 6, 0xc7, 0xc0, 0, 0, 0, 0, 0x83, 0xc0, 1, 0xc3});
    const va_t Boundary = Image.Entry + 8;
    auto Guess = Symbol::makeFunc(Boundary);
    Guess.IsBoundaryGuess = true;
    if (Mutation == 1)
      Guess.Name = "independent_function";
    Image.Symbols.push_back(Guess);
    if (Mutation == 2) {
      Export Export;
      Export.Addr = Boundary;
      Image.Exports.push_back(Export);
    }
    if (Mutation == 3)
      Image.Symbols.push_back(Symbol::makeFunc(Boundary));
    auto &Code = Image.Segments[0].Data;
    if (Mutation == 4)
      Code[1] = 30;
    if (Mutation == 5) {
      Code[8] = 0xff;
      Code[9] = 0xe0;
    }
    if (Mutation == 6) {
      Code[2] = 0xe8;
      Code[3] = 1;
      Code[7] = 0x90;
    }
    if (Mutation == 7)
      Code[1] = 5;
    if (Mutation == 9) {
      Code[8] = 0x0f;
      Code[9] = 0x0b;
    }
    const va_t Limit = Mutation == 8 ? Boundary : InvalidVA;
    const FunctionRangeMap Functions(Image);
    const auto Ordinary = Functions.find(Image, Image.Entry, true);
    ASSERT_TRUE(Ordinary);
    EXPECT_EQ(Ordinary->End, Boundary);
    const auto Callback = Functions.findCallback(Image, Image.Entry, Limit);
    ASSERT_TRUE(Callback);
    EXPECT_EQ(Callback->End,
              Mutation == 0 ? Image.Entry + Code.size() : Boundary);
    size_t Work = 0;
    const auto End = checkedCallbackRangeEnd(
        Image, Image.Entry, Boundary,
        std::min(Limit, Image.Entry + Code.size()), Work);
    EXPECT_EQ(End.has_value(), Mutation < 4);
    Work = limits::kMaxRegistrationEHStateWork;
    EXPECT_FALSE(checkedCallbackRangeEnd(Image, Image.Entry, Boundary,
                                         Image.Entry + Code.size(), Work));
  }
  auto Image =
      codeImage({0x75, 6, 0xc7, 0xc0, 0, 0, 0, 0, 0x83, 0xc0, 1, 0xc3});
  for (va_t Boundary : {Image.Entry + 8, Image.Entry + 11}) {
    size_t Work = 0;
    const auto End = checkedCallbackRangeEnd(Image, Image.Entry, Boundary,
                                             Image.Entry + 12, Work);
    ASSERT_TRUE(End);
    EXPECT_EQ(*End, Image.Entry + 12);
  }
}
} // namespace
