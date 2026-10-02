//===- COFFExceptionTests.cpp - Windows exception metadata tests ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFExceptionTestsDetail.h"
#include "COFFUnwindDetail.h"
#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFException.h"
#include "neverd/loader/ExceptionInfo.h"
#include "neverd/loader/ExecutableCodeOwnerIndex.h"
#include "neverd/support/BinaryEncoding.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using namespace neverd;
using namespace neverd::coff_eh_test;

TEST(COFFExceptionModel, BuildsCheckedHalfOpenRanges) {
  auto R = ExceptionAddressRange::fromStartAndSize(0x1000, 0x40);
  ASSERT_TRUE(R.has_value());
  EXPECT_EQ(R->Begin, 0x1000u);
  EXPECT_EQ(R->End, 0x1040u);
  EXPECT_TRUE(R->contains(0x1000));
  EXPECT_TRUE(R->contains(0x103f));
  EXPECT_FALSE(R->contains(0x1040));
  EXPECT_TRUE(R->contains(*R));

  EXPECT_FALSE(ExceptionAddressRange::fromStartAndSize(0x1000, 0));
  EXPECT_FALSE(ExceptionAddressRange::fromStartAndSize(
      std::numeric_limits<va_t>::max() - 3, 8));
  EXPECT_FALSE((ExceptionAddressRange{0x2000, 0x2000}.isValid()));
}

TEST(COFFExceptionModel, CombinesParseStatusConservatively) {
  EXPECT_EQ(mergeExceptionParseStatus(ExceptionParseStatus::Complete,
                                      ExceptionParseStatus::Complete),
            ExceptionParseStatus::Complete);
  EXPECT_EQ(mergeExceptionParseStatus(ExceptionParseStatus::Complete,
                                      ExceptionParseStatus::Partial),
            ExceptionParseStatus::Partial);
  EXPECT_EQ(mergeExceptionParseStatus(ExceptionParseStatus::Malformed,
                                      ExceptionParseStatus::Partial),
            ExceptionParseStatus::Malformed);
}

TEST(COFFExceptionModel, ValidatesCxxStateMaps) {
  CxxExceptionInfo Info;
  Info.MaxState = 3;
  Info.UnwindMap = {{-1, 0x5000}, {0, 0x5010}, {1, 0x5020}};
  Info.IPMap = {{0x1000, -1}, {0x1010, 0}, {0x1020, 2}};
  EXPECT_TRUE(Info.hasValidStateGraph());

  Info.UnwindMap[1].ToState = 1;
  EXPECT_FALSE(Info.hasValidStateGraph());
  Info.UnwindMap[1].ToState = 0;
  Info.IPMap[2].IP = 0x1008;
  EXPECT_FALSE(Info.hasValidStateGraph());
}

TEST(COFFExceptionModel, KeepsPersonalityIdentityAndGSProvenance) {
  EXPECT_TRUE(isSEHPersonality(ExceptionPersonality::CSpecificHandler));
  EXPECT_TRUE(isSEHPersonality(ExceptionPersonality::GSHandlerCheckSEH));
  EXPECT_TRUE(isCxxPersonality(ExceptionPersonality::CxxFrameHandler3));
  EXPECT_TRUE(isCxxPersonality(ExceptionPersonality::GSHandlerCheckEH4));
  EXPECT_TRUE(isGSWrappedPersonality(ExceptionPersonality::GSHandlerCheckEH));
  EXPECT_FALSE(isGSWrappedPersonality(ExceptionPersonality::CxxFrameHandler3));
}

TEST(COFFExceptionModel, QueriesOwningRuntimeFunction) {
  ExceptionInfo EI;
  ExceptionFunction First;
  First.CodeRange = {0x1000, 0x1040};
  First.ParseStatus = ExceptionParseStatus::Complete;
  EI.Functions.push_back(First);

  ExceptionFunction Second;
  Second.CodeRange = {0x2000, 0x2080};
  Second.ParseStatus = ExceptionParseStatus::Partial;
  EI.Functions.push_back(Second);
  EI.rebuildIndex();

  ASSERT_NE(EI.findFunction(0x1020), nullptr);
  EXPECT_EQ(EI.findFunction(0x1020)->CodeRange.Begin, 0x1000u);
  EXPECT_EQ(EI.findFunction(0x207f)->ParseStatus,
            ExceptionParseStatus::Partial);
  EXPECT_EQ(EI.findFunction(0x1040), nullptr);
  EXPECT_EQ(EI.findFunction(0x3000), nullptr);

  ExceptionFunction Parent;
  Parent.CodeRange = {0x1000, 0x3000};
  Parent.ParseStatus = ExceptionParseStatus::Complete;
  EI.Functions.push_back(Parent);
  EI.rebuildIndex();
  ASSERT_NE(EI.findFunction(0x1020), nullptr);
  EXPECT_EQ(EI.findFunction(0x1020)->CodeRange.End, 0x1040u);
  ASSERT_NE(EI.findFunction(0x1800), nullptr);
  EXPECT_EQ(EI.findFunction(0x1800)->CodeRange.Begin, 0x1000u);
  EXPECT_EQ(EI.findFunction(0x1800)->CodeRange.End, 0x3000u);
}

TEST(COFFExceptionParser, ChainedFragmentsBelongToTheirChainsPrimary) {
  // ExIsResourceAcquiredSharedLite: the record at +0x7f chains to the one at
  // +0x79, which chains to the function's primary record.  Code in either
  // fragment is part of the function, so a jump into it is not a tail call.
  BinaryImage Image;
  Image.Format = BinaryFormat::COFF;
  Image.Arch = Arch::X64;
  ExceptionInfo &Info = Image.ExceptionMetadata;
  auto Add = [&](RuntimeFunctionKind Kind, va_t Begin, va_t End,
                 uint32_t UnwindInfoRVA) {
    ExceptionFunction Function;
    Function.Kind = Kind;
    Function.CodeRange = {Begin, End};
    Function.UnwindInfoRVA = UnwindInfoRVA;
    Info.Functions.push_back(std::move(Function));
  };
  Add(RuntimeFunctionKind::Primary, 0x140001000, 0x140001058, 0x3000);
  Add(RuntimeFunctionKind::Chained, 0x140001079, 0x14000107f, 0x3010);
  Add(RuntimeFunctionKind::Chained, 0x14000107f, 0x140001123, 0x3020);
  for (size_t I = 1; I != 3; ++I) {
    Info.Functions[I].ChainedPrimaryRange = Info.Functions[I - 1].CodeRange;
    Info.Functions[I].ChainedUnwindInfoRVA =
        Info.Functions[I - 1].UnwindInfoRVA;
  }

  coff_loader::unwind_detail::resolveX64UnwindChains(Info);
  ASSERT_EQ(Info.ParseStatus, ExceptionParseStatus::Complete);

  const ExecutableCodeOwnerIndex Owners(Image);
  for (const ExecutableCodeOwnerIndex *Index :
       {static_cast<const ExecutableCodeOwnerIndex *>(nullptr), &Owners}) {
    for (va_t Target : {0x140001079ULL, 0x14000107fULL, 0x140001122ULL})
      EXPECT_TRUE(
          isExplicitlyOwnedFunctionFragment(Image, 0x140001000, Target, Index))
          << std::hex << Target;
    EXPECT_FALSE(isExplicitlyOwnedFunctionFragment(Image, 0x140001000,
                                                   0x140001123, Index));
    EXPECT_FALSE(isExplicitlyOwnedFunctionFragment(Image, 0x140001079,
                                                   0x14000107f, Index));
  }
}

TEST(COFFExceptionParser, AcceptsAcyclicX64UnwindChainBeyondLegacyDepth) {
  constexpr size_t ChainedRecordCount = 40;
  ExceptionInfo Info;
  Info.Functions.reserve(ChainedRecordCount + 1);
  for (size_t I = 0; I <= ChainedRecordCount; ++I) {
    ExceptionFunction Function;
    Function.CodeRange = {0x140001000 + I * 0x20, 0x140001010 + I * 0x20};
    Function.UnwindInfoRVA = static_cast<uint32_t>(0x3000 + I * 0x20);
    Function.Kind = I == ChainedRecordCount ? RuntimeFunctionKind::Primary
                                            : RuntimeFunctionKind::Chained;
    Info.Functions.push_back(std::move(Function));
  }
  for (size_t I = 0; I < ChainedRecordCount; ++I) {
    Info.Functions[I].ChainedPrimaryRange = Info.Functions[I + 1].CodeRange;
    Info.Functions[I].ChainedUnwindInfoRVA =
        Info.Functions[I + 1].UnwindInfoRVA;
  }

  coff_loader::unwind_detail::resolveX64UnwindChains(Info);

  EXPECT_EQ(Info.ParseStatus, ExceptionParseStatus::Complete);
  for (size_t I = 0; I < ChainedRecordCount; ++I) {
    SCOPED_TRACE(I);
    ASSERT_TRUE(Info.Functions[I].PrimaryFunctionIndex.has_value());
    EXPECT_EQ(*Info.Functions[I].PrimaryFunctionIndex, I + 1);
    EXPECT_EQ(Info.Functions[I].ParseStatus, ExceptionParseStatus::Complete);
    EXPECT_TRUE(Info.Functions[I].Diagnostics.empty());
  }
}

TEST(COFFExceptionParser, RecordsChainedFragmentsOnTheirPrimary) {
  // A fragment chained through another fragment still belongs to the primary
  // at the end of the chain; one whose frame contract differs does not.
  ExceptionInfo Info;
  auto Add = [&](va_t Begin, uint32_t UnwindRVA, RuntimeFunctionKind Kind) {
    ExceptionFunction Function;
    Function.CodeRange = {Begin, Begin + 0x10};
    Function.UnwindInfoRVA = UnwindRVA;
    Function.Kind = Kind;
    Info.Functions.push_back(std::move(Function));
  };
  Add(0x140003000, 0x3020, RuntimeFunctionKind::Chained); // through [2]
  Add(0x140001000, 0x3000, RuntimeFunctionKind::Primary);
  Add(0x140002000, 0x3010, RuntimeFunctionKind::Chained); // to [1]
  Add(0x140004000, 0x3030, RuntimeFunctionKind::Primary);
  Add(0x140005000, 0x3040, RuntimeFunctionKind::Chained); // to [1], bad frame
  Info.Functions[0].ChainedPrimaryRange = Info.Functions[2].CodeRange;
  Info.Functions[0].ChainedUnwindInfoRVA = 0x3010;
  Info.Functions[2].ChainedPrimaryRange = Info.Functions[1].CodeRange;
  Info.Functions[2].ChainedUnwindInfoRVA = 0x3000;
  Info.Functions[4].ChainedPrimaryRange = Info.Functions[1].CodeRange;
  Info.Functions[4].ChainedUnwindInfoRVA = 0x3000;
  Info.Functions[4].FrameRegister = 5;

  for (int Pass = 0; Pass < 2; ++Pass) {
    SCOPED_TRACE(Pass);
    coff_loader::unwind_detail::resolveX64UnwindChains(Info);
    const std::vector<ExceptionAddressRange> &Fragments =
        Info.Functions[1].FragmentRanges;
    ASSERT_EQ(Fragments.size(), 2u);
    EXPECT_EQ(Fragments[0].Begin, 0x140002000u);
    EXPECT_EQ(Fragments[1].Begin, 0x140003000u);
    EXPECT_TRUE(Info.Functions[1].ownsCode(0x14000300f));
    EXPECT_FALSE(Info.Functions[1].ownsCode(0x140003010));
    EXPECT_FALSE(Info.Functions[1].ownsCode(0x140005000));
    EXPECT_TRUE(Info.Functions[3].FragmentRanges.empty());
  }
}

TEST(COFFExceptionParser, RestrictedLoadSelectsTheRequestedFunctionsFragments) {
  constexpr va_t Base = 0x140000000;
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = Base;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Base + 0x1000;
  Text.Size = Text.FileSz = 0x4000;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xc3);
  Img.Segments.push_back(std::move(Text));
  // UNWIND_INFO: version 1, no codes; a chained one carries its parent's
  // RUNTIME_FUNCTION right after the header.
  Segment RData;
  RData.Name = ".rdata";
  RData.VA = Base + 0x6000;
  RData.Size = RData.FileSz = 0x60;
  RData.Flags = SegmentFlags::Readable;
  RData.Data.assign(RData.Size, 0);
  auto PutUnwind = [&](uint32_t Offset,
                       std::optional<std::array<uint32_t, 3>> Parent) {
    RData.Data[Offset] = Parent ? 0x21 : 0x01;
    if (Parent)
      for (size_t Word = 0; Word < 3; ++Word)
        for (unsigned Byte = 0; Byte < 4; ++Byte)
          RData.Data[Offset + 4 + Word * 4 + Byte] =
              static_cast<uint8_t>((*Parent)[Word] >> (8 * Byte));
  };
  PutUnwind(0x00, std::nullopt);                                    // P
  PutUnwind(0x10, std::array<uint32_t, 3>{0x1000, 0x1100, 0x6000}); // F1 -> P
  PutUnwind(0x20, std::array<uint32_t, 3>{0x2000, 0x2010, 0x6010}); // F2 -> F1
  PutUnwind(0x30, std::nullopt);                                    // Q
  PutUnwind(0x40, std::array<uint32_t, 3>{0x4000, 0x4100, 0x6030}); // FQ -> Q
  Img.Segments.push_back(std::move(RData));
  Img.LoadOnlyFunctionEntries.insert(Base + 0x1000);

  const uint32_t Records[][3] = {{0x1000, 0x1100, 0x6000},
                                 {0x2000, 0x2010, 0x6010},
                                 {0x3000, 0x3010, 0x6020},
                                 {0x4000, 0x4100, 0x6030},
                                 {0x3100, 0x3110, 0x6040}};
  std::vector<uint8_t> Bytes;
  for (const auto &Record : Records)
    for (uint32_t Word : Record)
      for (unsigned Byte = 0; Byte < 4; ++Byte)
        Bytes.push_back(static_cast<uint8_t>(Word >> (8 * Byte)));

  std::unordered_map<size_t, ExceptionFunction> Decoded;
  const std::vector<bool> Needed =
      coff_loader::unwind_detail::selectRestrictedX64Records(
          Img, Base, Bytes.data(), std::size(Records), 0x7000, Decoded);
  EXPECT_EQ(Needed, (std::vector<bool>{true, true, true, false, false}));
  EXPECT_EQ(Decoded.size(), 3u);
  ASSERT_TRUE(Decoded.count(2));
  ASSERT_TRUE(Decoded.at(2).ChainedPrimaryRange.has_value());
  EXPECT_EQ(Decoded.at(2).ChainedPrimaryRange->Begin, Base + 0x2000);
}

TEST(COFFExceptionParser, RejectsCyclicX64UnwindChainExplicitly) {
  ExceptionInfo Info;
  for (size_t I = 0; I < 3; ++I) {
    ExceptionFunction Function;
    Function.CodeRange = {0x140001000 + I * 0x20, 0x140001010 + I * 0x20};
    Function.UnwindInfoRVA = static_cast<uint32_t>(0x3000 + I * 0x20);
    Function.Kind = RuntimeFunctionKind::Chained;
    Info.Functions.push_back(std::move(Function));
  }
  for (size_t I = 0; I < Info.Functions.size(); ++I) {
    const size_t Next = (I + 1) % Info.Functions.size();
    Info.Functions[I].ChainedPrimaryRange = Info.Functions[Next].CodeRange;
    Info.Functions[I].ChainedUnwindInfoRVA = Info.Functions[Next].UnwindInfoRVA;
  }

  coff_loader::unwind_detail::resolveX64UnwindChains(Info);

  EXPECT_EQ(Info.ParseStatus, ExceptionParseStatus::Malformed);
  for (const ExceptionFunction &Function : Info.Functions) {
    EXPECT_EQ(Function.ParseStatus, ExceptionParseStatus::Malformed);
    EXPECT_TRUE(
        std::any_of(Function.Diagnostics.begin(), Function.Diagnostics.end(),
                    [](const std::string &Message) {
                      return Message.find("cyclic") != std::string::npos;
                    }));
  }
}

TEST(COFFExceptionParser,
     RejectsX64UnwindChainWithDifferentFrameRegister) {
  ExceptionInfo Info;

  ExceptionFunction Chained;
  Chained.CodeRange = {0x140001000, 0x140001010};
  Chained.UnwindInfoRVA = 0x3000;
  Chained.Kind = RuntimeFunctionKind::Chained;
  Chained.FrameRegister = 5;
  Chained.FrameOffset = 32;
  Chained.ChainedPrimaryRange =
      ExceptionAddressRange{0x140001020, 0x140001030};
  Chained.ChainedUnwindInfoRVA = 0x3020;
  Info.Functions.push_back(std::move(Chained));

  ExceptionFunction Primary;
  Primary.CodeRange = {0x140001020, 0x140001030};
  Primary.UnwindInfoRVA = 0x3020;
  Primary.Kind = RuntimeFunctionKind::Primary;
  Primary.FrameRegister = 13;
  Primary.FrameOffset = 32;
  Info.Functions.push_back(std::move(Primary));

  coff_loader::unwind_detail::resolveX64UnwindChains(Info);

  EXPECT_EQ(Info.ParseStatus, ExceptionParseStatus::Malformed);
  EXPECT_EQ(Info.Functions[0].ParseStatus,
            ExceptionParseStatus::Malformed);
  ASSERT_TRUE(Info.Functions[0].PrimaryFunctionIndex.has_value());
  EXPECT_EQ(*Info.Functions[0].PrimaryFunctionIndex, 1u);
  EXPECT_TRUE(std::any_of(
      Info.Functions[0].Diagnostics.begin(),
      Info.Functions[0].Diagnostics.end(), [](const std::string &Message) {
        return Message.find("frame register") != std::string::npos;
      }));
  EXPECT_EQ(Info.Functions[1].ParseStatus, ExceptionParseStatus::Complete);
}

TEST(COFFExceptionParser, RejectsX64UnwindChainWithDifferentFrameOffset) {
  ExceptionInfo Info;

  ExceptionFunction Chained;
  Chained.CodeRange = {0x140001000, 0x140001010};
  Chained.UnwindInfoRVA = 0x3000;
  Chained.Kind = RuntimeFunctionKind::Chained;
  Chained.FrameRegister = 5;
  Chained.FrameOffset = 32;
  Chained.ChainedPrimaryRange =
      ExceptionAddressRange{0x140001020, 0x140001030};
  Chained.ChainedUnwindInfoRVA = 0x3020;
  Info.Functions.push_back(std::move(Chained));

  ExceptionFunction Primary;
  Primary.CodeRange = {0x140001020, 0x140001030};
  Primary.UnwindInfoRVA = 0x3020;
  Primary.Kind = RuntimeFunctionKind::Primary;
  Primary.FrameRegister = 5;
  Primary.FrameOffset = 48;
  Info.Functions.push_back(std::move(Primary));

  coff_loader::unwind_detail::resolveX64UnwindChains(Info);

  EXPECT_EQ(Info.ParseStatus, ExceptionParseStatus::Malformed);
  EXPECT_EQ(Info.Functions[0].ParseStatus,
            ExceptionParseStatus::Malformed);
  ASSERT_TRUE(Info.Functions[0].PrimaryFunctionIndex.has_value());
  EXPECT_EQ(*Info.Functions[0].PrimaryFunctionIndex, 1u);
  EXPECT_TRUE(std::any_of(
      Info.Functions[0].Diagnostics.begin(),
      Info.Functions[0].Diagnostics.end(), [](const std::string &Message) {
        return Message.find("frame offset") != std::string::npos;
      }));
  EXPECT_EQ(Info.Functions[1].ParseStatus, ExceptionParseStatus::Complete);
}

TEST(COFFExceptionParser,
     RejectsEveryX64UnwindChainNodeWithDifferentTerminalFrame) {
  ExceptionInfo Info;
  for (size_t I = 0; I < 3; ++I) {
    ExceptionFunction Function;
    Function.CodeRange = {0x140001000 + I * 0x20,
                          0x140001010 + I * 0x20};
    Function.UnwindInfoRVA = static_cast<uint32_t>(0x3000 + I * 0x20);
    Function.Kind = I == 2 ? RuntimeFunctionKind::Primary
                           : RuntimeFunctionKind::Chained;
    Function.FrameRegister = I == 2 ? 13 : 5;
    Function.FrameOffset = I == 2 ? 48 : 32;
    Info.Functions.push_back(std::move(Function));
  }
  for (size_t I = 0; I < 2; ++I) {
    Info.Functions[I].ChainedPrimaryRange = Info.Functions[I + 1].CodeRange;
    Info.Functions[I].ChainedUnwindInfoRVA =
        Info.Functions[I + 1].UnwindInfoRVA;
  }

  coff_loader::unwind_detail::resolveX64UnwindChains(Info);

  EXPECT_EQ(Info.ParseStatus, ExceptionParseStatus::Malformed);
  for (size_t I = 0; I < 2; ++I) {
    SCOPED_TRACE(I);
    EXPECT_EQ(Info.Functions[I].ParseStatus,
              ExceptionParseStatus::Malformed);
    EXPECT_TRUE(std::any_of(
        Info.Functions[I].Diagnostics.begin(),
        Info.Functions[I].Diagnostics.end(), [](const std::string &Message) {
          return Message.find("frame register") != std::string::npos;
        }));
    EXPECT_TRUE(std::any_of(
        Info.Functions[I].Diagnostics.begin(),
        Info.Functions[I].Diagnostics.end(), [](const std::string &Message) {
          return Message.find("frame offset") != std::string::npos;
        }));
  }
  EXPECT_EQ(Info.Functions[2].ParseStatus, ExceptionParseStatus::Complete);
}

TEST(COFFExceptionParser, DecodesX64V1OperationsAndHandlerLocation) {
  BinaryImage Img = makeX64ExceptionImage();
  uint8_t *X = Img.Segments[1].Data.data();
  X[0] = 1 | (1 << 3); // version 1, exception handler
  X[1] = 5;            // prologue size
  X[2] = 2;            // two unwind slots
  X[3] = 0;
  X[4] = 4;
  X[5] = (3 << 4) | 2; // allocate 32 bytes
  X[6] = 1;
  X[7] = (5 << 4) | 0; // push rbp
  writeLE<uint32_t>(X + 8, 0x1100);

  ExceptionFunction F = coff_loader::decodeX64ExceptionFunction(
      Img, Img.Base, 0x2000, 0x1000, 0x1040, 0x3000);
  EXPECT_EQ(F.ParseStatus, ExceptionParseStatus::Complete);
  EXPECT_EQ(F.Encoding, ExceptionEncoding::X64UnwindV1);
  ASSERT_EQ(F.UnwindOperations.size(), 2u);
  EXPECT_EQ(F.UnwindOperations[0].Kind, UnwindOperationKind::AllocateSmall);
  EXPECT_EQ(F.UnwindOperations[0].StackOffset, 32u);
  EXPECT_EQ(F.UnwindOperations[1].Kind, UnwindOperationKind::PushNonVolatile);
  EXPECT_EQ(F.UnwindOperations[1].Register, 5u);
  EXPECT_EQ(F.PersonalityVA, Img.Base + 0x1100);
  EXPECT_EQ(F.HandlerDataVA, Img.Base + 0x300c);
}

TEST(COFFExceptionParser, RejectsTruncatedX64UnwindSlots) {
  BinaryImage Img = makeX64ExceptionImage(4);
  uint8_t *X = Img.Segments[1].Data.data();
  X[0] = 1;
  X[1] = 4;
  X[2] = 3;
  X[3] = 0;

  ExceptionFunction F = coff_loader::decodeX64ExceptionFunction(
      Img, Img.Base, 0x2000, 0x1000, 0x1040, 0x3000);
  EXPECT_EQ(F.ParseStatus, ExceptionParseStatus::Malformed);
  EXPECT_FALSE(F.Diagnostics.empty());
}

TEST(COFFExceptionParser, AcceptsZeroX64UnwindSlotsAtSectionEnd) {
  BinaryImage Img = makeX64ExceptionImage(4);
  uint8_t *X = Img.Segments[1].Data.data();
  X[0] = 1;
  X[1] = 0;
  X[2] = 0;
  X[3] = 0;

  ExceptionFunction F = coff_loader::decodeX64ExceptionFunction(
      Img, Img.Base, 0x2000, 0x1000, 0x1040, 0x3000);
  EXPECT_EQ(F.ParseStatus, ExceptionParseStatus::Complete);
  EXPECT_TRUE(F.UnwindOperations.empty());
}

TEST(COFFExceptionParser, RejectsInvalidX64V1CodeOrdering) {
  BinaryImage Img = makeX64ExceptionImage();
  uint8_t *X = Img.Segments[1].Data.data();
  X[0] = 1;
  X[1] = 5;
  X[2] = 2;
  X[4] = 1;
  X[5] = (5 << 4) | 0;
  X[6] = 4; // Later slot must describe an earlier prologue instruction.
  X[7] = (3 << 4) | 2;

  ExceptionFunction F = coff_loader::decodeX64ExceptionFunction(
      Img, Img.Base, 0x2000, 0x1000, 0x1040, 0x3000);
  EXPECT_EQ(F.ParseStatus, ExceptionParseStatus::Malformed);
}

TEST(COFFExceptionParser, AcceptsX64V1OperationsAtTheSameCodeOffset) {
  BinaryImage Img = makeX64ExceptionImage();
  uint8_t *X = Img.Segments[1].Data.data();
  X[0] = 1;
  X[1] = 4;
  X[2] = 2;
  X[3] = 5; // rbp is the frame register
  X[4] = 4;
  X[5] = 3; // set frame pointer
  X[6] = 4;
  X[7] = (3 << 4) | 2; // allocate 32 bytes at the same prologue offset

  ExceptionFunction F = coff_loader::decodeX64ExceptionFunction(
      Img, Img.Base, 0x2000, 0x1000, 0x1040, 0x3000);
  EXPECT_EQ(F.ParseStatus, ExceptionParseStatus::Complete);
  ASSERT_EQ(F.UnwindOperations.size(), 2u);
  EXPECT_EQ(F.UnwindOperations[0].Kind, UnwindOperationKind::SetFramePointer);
  EXPECT_EQ(F.UnwindOperations[1].Kind, UnwindOperationKind::AllocateSmall);
}

TEST(COFFExceptionParser, IgnoresTheReservedSetFramePointerInfo) {
  // MSVC writes the frame register or the scaled offset into the reserved
  // operation info of UWOP_SET_FPREG; the header alone defines the frame.
  for (uint8_t Info : {uint8_t{5}, uint8_t{2}}) {
    SCOPED_TRACE(static_cast<int>(Info));
    BinaryImage Img = makeX64ExceptionImage();
    uint8_t *X = Img.Segments[1].Data.data();
    X[0] = 1;
    X[1] = 4;
    X[2] = 2;
    X[3] = 5 | (2 << 4); // rbp = rsp + 32
    X[4] = 4;
    X[5] = static_cast<uint8_t>(3 | (Info << 4)); // set frame pointer
    X[6] = 4;
    X[7] = (3 << 4) | 2; // allocate 32 bytes

    ExceptionFunction F = coff_loader::decodeX64ExceptionFunction(
        Img, Img.Base, 0x2000, 0x1000, 0x1040, 0x3000);
    EXPECT_EQ(F.ParseStatus, ExceptionParseStatus::Complete);
    EXPECT_EQ(F.FrameRegister, 5u);
    EXPECT_EQ(F.FrameOffset, 32u);
  }
}

TEST(COFFExceptionParser, DecodesX64V3PayloadAndWODPool) {
  BinaryImage Img = makeX64ExceptionImage();
  uint8_t *X = Img.Segments[1].Data.data();
  X[0] = 3;
  X[1] = 8;
  X[2] = 6;            // twelve payload bytes
  X[3] = 2 | (1 << 5); // two prolog ops, one epilog
  X[4] = 4;
  X[5] = 1; // prolog IP offsets
  X[6] = 2 << 3;
  writeLE<uint16_t>(X + 7, 0x30); // epilog descriptor
  writeLE<uint16_t>(X + 9, 0);    // first WOD
  X[11] = 5;                      // last instruction offset
  X[12] = 1;
  X[13] = 4; // epilog IP offsets
  X[14] = 0x38;
  X[15] = 0x2c; // alloc_small 32, push rbp

  ExceptionFunction F = coff_loader::decodeX64ExceptionFunction(
      Img, Img.Base, 0x2000, 0x1000, 0x1080, 0x3000);
  EXPECT_EQ(F.ParseStatus, ExceptionParseStatus::Complete);
  EXPECT_EQ(F.Encoding, ExceptionEncoding::X64UnwindV3);
  ASSERT_EQ(F.UnwindOperations.size(), 2u);
  EXPECT_EQ(F.UnwindOperations[0].Kind, UnwindOperationKind::AllocateSmall);
  EXPECT_EQ(F.UnwindOperations[1].Kind, UnwindOperationKind::PushNonVolatile);
  ASSERT_EQ(F.Epilogs.size(), 1u);
  EXPECT_EQ(F.Epilogs[0].StartOffset, 0x30);
  ASSERT_EQ(F.Epilogs[0].Operations.size(), 2u);
}

TEST(COFFExceptionParser, RejectsTruncatedX64V3Payload) {
  BinaryImage Img = makeX64ExceptionImage(8);
  uint8_t *X = Img.Segments[1].Data.data();
  X[0] = 3;
  X[2] = 6;

  ExceptionFunction F = coff_loader::decodeX64ExceptionFunction(
      Img, Img.Base, 0x2000, 0x1000, 0x1040, 0x3000);
  EXPECT_EQ(F.ParseStatus, ExceptionParseStatus::Malformed);
}

TEST(COFFExceptionParser, RejectsInvalidX64V3OrderingAndInheritance) {
  {
    BinaryImage Img = makeX64ExceptionImage();
    uint8_t *X = Img.Segments[1].Data.data();
    X[0] = 3;
    X[1] = 8;
    X[2] = 2;
    X[3] = 2;
    X[4] = 1;
    X[5] = 4; // Prologue operation offsets must be descending.
    X[6] = 0x38;
    X[7] = 0x2c;

    ExceptionFunction F = coff_loader::decodeX64ExceptionFunction(
        Img, Img.Base, 0x2000, 0x1000, 0x1080, 0x3000);
    EXPECT_EQ(F.ParseStatus, ExceptionParseStatus::Malformed);
  }

  {
    BinaryImage Img = makeX64ExceptionImage();
    uint8_t *X = Img.Segments[1].Data.data();
    X[0] = 3;
    X[1] = 0;
    X[2] = 6;
    X[3] = 2 << 5;
    X[4] = 1 << 3;                 // One-operation full descriptor.
    writeLE<int16_t>(X + 5, 0x20); // Ascending from function start.
    writeLE<uint16_t>(X + 7, 0);   // First WOD.
    X[9] = 2;                      // Last instruction offset.
    X[10] = 1;                     // Operation IP offset.
    X[11] = 1;                     // Inherited descriptor changes flags.
    writeLE<int16_t>(X + 12, 0x10);
    X[14] = 0x2c;

    ExceptionFunction F = coff_loader::decodeX64ExceptionFunction(
        Img, Img.Base, 0x2000, 0x1000, 0x1080, 0x3000);
    EXPECT_EQ(F.ParseStatus, ExceptionParseStatus::Malformed);
  }
}

} // namespace
