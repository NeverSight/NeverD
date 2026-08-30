//===- LowInstructionBoundaryTests.cpp - LowIR instruction provenance ----===//

#include "../../../lib/ir/low/jumptable/JumpTableResolverDetail.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/lift/ARMRegs.h"
#include "neverd/lift/LiftCommon.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t kEntry = 0x1000;

testing::AssertionResult validLLVMModule(llvm::Module &Module) {
  std::string Error;
  llvm::raw_string_ostream OS(Error);
  if (llvm::verifyModule(Module, &OS)) {
    OS.flush();
    return testing::AssertionFailure() << Error;
  }
  return testing::AssertionSuccess();
}

testing::AssertionResult validHighC(llvm::StringRef Source) {
#ifdef NEVERD_TEST_CLANG
  std::string CompilerPath = NEVERD_TEST_CLANG;
#else
  std::string CompilerPath;
#endif
  if (CompilerPath.empty()) {
    auto Compiler = llvm::sys::findProgramByName("clang");
    if (!Compiler)
      return testing::AssertionFailure() << Compiler.getError().message();
    CompilerPath = *Compiler;
  }
  llvm::SmallString<128> IncludeDir;
  std::error_code EC =
      llvm::sys::fs::createUniqueDirectory("neverd-segment-include", IncludeDir);
  if (EC)
    return testing::AssertionFailure() << EC.message();
  llvm::FileRemover RemoveIncludeDir(IncludeDir);
  llvm::SmallString<128> StringHeader = IncludeDir;
  llvm::sys::path::append(StringHeader, "string.h");
  {
    llvm::raw_fd_ostream Header(StringHeader, EC);
    if (EC)
      return testing::AssertionFailure() << EC.message();
    Header << "void *memcpy(void *, const void *, __SIZE_TYPE__);\n"
              "void *memset(void *, int, __SIZE_TYPE__);\n"
              "int memcmp(const void *, const void *, __SIZE_TYPE__);\n";
  }
  llvm::FileRemover RemoveStringHeader(StringHeader);
  llvm::SmallString<128> SourcePath;
  llvm::SmallString<128> StdoutPath;
  llvm::SmallString<128> StderrPath;
  llvm::SmallString<128> ObjectPath;
  EC = llvm::sys::fs::createTemporaryFile("neverd-segment-memory", "c",
                                          SourcePath);
  if (EC)
    return testing::AssertionFailure() << EC.message();
  llvm::FileRemover RemoveSource(SourcePath);
  EC = llvm::sys::fs::createTemporaryFile("neverd-segment-memory", "out",
                                          StdoutPath);
  if (EC)
    return testing::AssertionFailure() << EC.message();
  llvm::FileRemover RemoveStdout(StdoutPath);
  EC = llvm::sys::fs::createTemporaryFile("neverd-segment-memory", "err",
                                          StderrPath);
  if (EC)
    return testing::AssertionFailure() << EC.message();
  llvm::FileRemover RemoveStderr(StderrPath);
  EC = llvm::sys::fs::createTemporaryFile("neverd-segment-memory", "o",
                                          ObjectPath);
  if (EC)
    return testing::AssertionFailure() << EC.message();
  llvm::FileRemover RemoveObject(ObjectPath);
  {
    llvm::raw_fd_ostream Out(SourcePath, EC);
    if (EC)
      return testing::AssertionFailure() << EC.message();
    Out << Source;
  }
  llvm::SmallVector<llvm::StringRef, 14> Args{
      CompilerPath, "-target", "x86_64-none-elf", "-ffreestanding",
      "-std=gnu11", "-mavx2",  "-I",              IncludeDir,
      "-c",         SourcePath, "-o",              ObjectPath};
  std::optional<llvm::StringRef> Redirects[] = {std::nullopt, StdoutPath.str(),
                                                StderrPath.str()};
  std::string ExecuteError;
  const int RC = llvm::sys::ExecuteAndWait(
      CompilerPath, Args, std::nullopt, Redirects,
      /*SecondsToWait=*/30, /*MemoryLimit=*/0, &ExecuteError);
  if (RC == 0)
    return testing::AssertionSuccess();
  auto ErrorBuffer = llvm::MemoryBuffer::getFile(StderrPath);
  return testing::AssertionFailure()
         << ExecuteError
         << (ErrorBuffer ? (*ErrorBuffer)->getBuffer().str() : std::string{});
}

TEST(LowInstructionBoundary, JumpTableCaseLabelsUseModularArithmetic) {
  EXPECT_EQ(recoverCaseLabelBitPattern(/*EntryIndex=*/0,
                                       /*Stride=*/1, /*NormShift=*/0,
                                       std::numeric_limits<int64_t>::min()),
            UINT64_C(0x8000000000000000));
  EXPECT_EQ(recoverCaseLabelBitPattern(UINT64_MAX, /*Stride=*/2,
                                       /*NormShift=*/0, /*NormBase=*/0),
            UINT64_C(0xfffffffffffffffe));
  EXPECT_EQ(recoverCaseLabelBitPattern(/*EntryIndex=*/1,
                                       /*Stride=*/1, /*NormShift=*/63,
                                       std::numeric_limits<int64_t>::min()),
            UINT64_C(0));
  EXPECT_FALSE(recoverCaseLabelBitPattern(/*EntryIndex=*/1,
                                          /*Stride=*/1, /*NormShift=*/64,
                                          /*NormBase=*/0));
}

TEST(LowInstructionBoundary, GuardEvaluatorDistinguishesNotFromTwosComplement) {
  const std::array<uint64_t, 1> Zero{0};
  const std::array<uint16_t, 1> W32{4};
  EXPECT_EQ(evaluateJumpTableGuardPrimitive(NdOp::INT_NEGATE, 4, Zero, W32),
            UINT64_C(0xffffffff));
  EXPECT_EQ(evaluateJumpTableGuardPrimitive(NdOp::INT_NEG2, 4, Zero, W32),
            UINT64_C(0));
}

TEST(LowInstructionBoundary, GuardEvaluatorCoercesMixedWidthComparisons) {
  const std::array<uint64_t, 2> Values{UINT64_C(0xffffffff),
                                       UINT64_C(0x100000004)};
  const std::array<uint16_t, 2> Widths{4, 8};
  EXPECT_EQ(evaluateJumpTableGuardPrimitive(NdOp::INT_LESS, 1, Values, Widths),
            UINT64_C(1));
}

TEST(LowInstructionBoundary, GuardEvaluatorRejectsValuesWiderThanU64) {
  const std::array<uint64_t, 1> Value{0};
  const std::array<uint16_t, 1> W128{16};
  EXPECT_FALSE(
      evaluateJumpTableGuardPrimitive(NdOp::INT_NEGATE, 16, Value, W128));
}

TEST(LowInstructionBoundary,
     RelativeTableTargetsRequireCompletePointerWidthTransforms) {
  EXPECT_TRUE(relativeTargetTransformUsesPointerWidth(
      NdOp::INT_ZEXT, /*DynamicInputSize=*/4, /*OtherInputSize=*/0,
      /*OutputSize=*/8, /*PointerSize=*/8));
  EXPECT_FALSE(relativeTargetTransformUsesPointerWidth(
      NdOp::INT_ZEXT, /*DynamicInputSize=*/1, /*OtherInputSize=*/0,
      /*OutputSize=*/4, /*PointerSize=*/8));

  EXPECT_TRUE(relativeTargetTransformUsesPointerWidth(
      NdOp::INT_LEFT, /*DynamicInputSize=*/8, /*OtherInputSize=*/1,
      /*OutputSize=*/8, /*PointerSize=*/8));
  EXPECT_FALSE(relativeTargetTransformUsesPointerWidth(
      NdOp::INT_LEFT, /*DynamicInputSize=*/4, /*OtherInputSize=*/1,
      /*OutputSize=*/4, /*PointerSize=*/8));

  EXPECT_TRUE(relativeTargetTransformUsesPointerWidth(
      NdOp::INT_ADD, /*DynamicInputSize=*/8, /*OtherInputSize=*/8,
      /*OutputSize=*/8, /*PointerSize=*/8));
  EXPECT_FALSE(relativeTargetTransformUsesPointerWidth(
      NdOp::INT_ADD, /*DynamicInputSize=*/4, /*OtherInputSize=*/4,
      /*OutputSize=*/4, /*PointerSize=*/8));
  EXPECT_FALSE(relativeTargetTransformUsesPointerWidth(
      NdOp::INT_ADD, /*DynamicInputSize=*/8, /*OtherInputSize=*/4,
      /*OutputSize=*/8, /*PointerSize=*/8));
}

TEST(LowInstructionBoundary, AbsoluteARMTableTargetsUseUniformImageMode) {
  BinaryImage Thumb;
  Thumb.Arch = Arch::ARM;
  Thumb.Mode = InstructionMode::Thumb;
  EXPECT_EQ(canonicalizeAbsoluteTableCodeTarget(Thumb, 0x1001), 0x1000u);
  EXPECT_FALSE(canonicalizeAbsoluteTableCodeTarget(Thumb, 0x1000));

  BinaryImage ARM;
  ARM.Arch = Arch::ARM;
  ARM.Mode = InstructionMode::ARM;
  EXPECT_EQ(canonicalizeAbsoluteTableCodeTarget(ARM, 0x2000), 0x2000u);
  EXPECT_FALSE(canonicalizeAbsoluteTableCodeTarget(ARM, 0x2001));

  BinaryImage A64;
  A64.Arch = Arch::AArch64;
  A64.Mode = InstructionMode::Default;
  EXPECT_EQ(canonicalizeAbsoluteTableCodeTarget(A64, 0x3001), 0x3001u);
}

TEST(LowInstructionBoundary, JumpTableTargetBasePresenceAllowsZeroAnchor) {
  const uint8_t Entry = 3;
  EXPECT_EQ(decodeTableEntry(&Entry, 1, /*IsRelative=*/true,
                             /*IsSigned=*/false, /*BaseAddr=*/0x1000,
                             /*HasTargetBase=*/true, /*TargetBase=*/0,
                             /*Scale=*/4, /*AddressBytes=*/8),
            12u);
  EXPECT_EQ(decodeTableEntry(&Entry, 1, /*IsRelative=*/true,
                             /*IsSigned=*/false, /*BaseAddr=*/0x1000,
                             /*HasTargetBase=*/false, /*TargetBase=*/0,
                             /*Scale=*/4, /*AddressBytes=*/8),
            0x1003u);
}

TEST(LowInstructionBoundary,
     RelocatedOwnerUsesMachOSectionCodeAuthorityForOnePast) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Mode = InstructionMode::Default;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::MachO;

  Segment Text;
  Text.Name = "__TEXT";
  Text.VA = 0x1000;
  Text.Size = 0x200;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(Text.Size);
  Image.Segments.push_back(std::move(Text));

  Section Code;
  Code.Name = "__text";
  Code.SegmentName = "__TEXT";
  Code.VA = 0x1000;
  Code.Size = 0x40;
  Code.FileSz = Code.Size;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Code.Type = llvm::MachO::S_REGULAR | llvm::MachO::S_ATTR_PURE_INSTRUCTIONS |
              llvm::MachO::S_ATTR_SOME_INSTRUCTIONS;
  Image.Sections.push_back(Code);

  Section CString;
  CString.Name = "__cstring";
  CString.SegmentName = "__TEXT";
  CString.VA = 0x1100;
  CString.Size = 0x10;
  CString.FileSz = CString.Size;
  CString.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  CString.Type = llvm::MachO::S_CSTRING_LITERALS;
  Image.Sections.push_back(CString);

  ASSERT_TRUE(Image.hasExecutableCodeOwnerAt(Code.VA));
  ASSERT_FALSE(Image.hasExecutableCodeOwnerAt(CString.VA));
  EXPECT_FALSE(
      Image.relocatedTargetBelongsToOwner(Code.VA + Code.Size, Code.VA));
  EXPECT_TRUE(Image.relocatedTargetBelongsToOwner(CString.VA + CString.Size,
                                                  CString.VA));
}

TEST(LowInstructionBoundary,
     I386GOTOFFOwnerRequiresTheExactRodataAnchorAssociation) {
  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Mode = InstructionMode::Default;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::ELF;

  Segment RodataSegment;
  RodataSegment.Name = ".rodata.load";
  RodataSegment.VA = 0x4000;
  RodataSegment.Size = 0x200;
  RodataSegment.Flags = SegmentFlags::Readable;
  RodataSegment.Data.resize(RodataSegment.Size);
  Image.Segments.push_back(std::move(RodataSegment));

  Section Rodata;
  Rodata.Name = ".rodata";
  Rodata.VA = 0x4080;
  Rodata.Size = 0x40;
  Rodata.FileSz = Rodata.Size;
  Rodata.Flags = SegmentFlags::Readable;
  Image.Sections.push_back(Rodata);

  constexpr va_t FoldedBeforeOwner = 0x3ff0;
  ASSERT_FALSE(
      Image.relocatedTargetBelongsToOwner(FoldedBeforeOwner, Rodata.VA));
  EXPECT_FALSE(Image.relocatedI386GOTOFFTargetBelongsToOwner(FoldedBeforeOwner,
                                                             Rodata.VA));

  Image.RodataAnchorSeg[FoldedBeforeOwner] = 0x5000;
  EXPECT_FALSE(Image.relocatedI386GOTOFFTargetBelongsToOwner(FoldedBeforeOwner,
                                                             Rodata.VA))
      << "an unrelated anchor segment must not authenticate the folded VA";

  Image.RodataAnchorSeg[FoldedBeforeOwner] = 0x4000;
  EXPECT_TRUE(Image.relocatedI386GOTOFFTargetBelongsToOwner(FoldedBeforeOwner,
                                                            Rodata.VA));
  EXPECT_FALSE(Image.relocatedI386GOTOFFTargetBelongsToOwner(FoldedBeforeOwner,
                                                             Rodata.VA + 1))
      << "the anchor must retain the loader's exact section owner";

  // Relocatable i386 objects commonly place their rodata owner at guest VA
  // zero.  A -16 GOTOFF bias is then encoded as 0xfffffff0; owner checking
  // must use guest-width modular arithmetic without accepting an arbitrary
  // large or foreign wrapped address.
  Image.Segments.front().VA = 0;
  Image.Sections.front().VA = 0;
  constexpr va_t WrappedBeforeOwner = 0xfffffff0u;
  Image.RodataAnchorSeg.clear();
  Image.RodataAnchorSeg[WrappedBeforeOwner] = 0;
  EXPECT_TRUE(
      Image.relocatedI386GOTOFFTargetBelongsToOwner(WrappedBeforeOwner, 0));
  Image.RodataAnchorSeg[WrappedBeforeOwner] = 0x1000;
  EXPECT_FALSE(
      Image.relocatedI386GOTOFFTargetBelongsToOwner(WrappedBeforeOwner, 0))
      << "a wrapped target still requires the exact loader owner segment";
  Image.RodataAnchorSeg[0xff000000u] = 0;
  EXPECT_FALSE(Image.relocatedI386GOTOFFTargetBelongsToOwner(0xff000000u, 0))
      << "a modular distance beyond the bounded selector bias is not an anchor";

  Image.Format = BinaryFormat::MachO;
  EXPECT_FALSE(Image.relocatedI386GOTOFFTargetBelongsToOwner(FoldedBeforeOwner,
                                                             Rodata.VA))
      << "synthetic anchor metadata cannot grant GOTOFF semantics cross-format";
}

TEST(LowInstructionBoundary,
     StackTableSourceCannotSuppressAnAdjustedAdjacentAnchor) {
  constexpr va_t TableA = 0x4000;
  constexpr va_t TableB = TableA + 16;
  constexpr va_t FieldVA = 0x1010;
  constexpr va_t OwnerVA = TableA;

  BinaryImage Image;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = 0x100;
  Text.FileSz = Text.Size;
  Text.Data.resize(Text.Size);
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Image.Segments.push_back(std::move(Text));
  for (va_t Slot : {TableA, TableA + 8, TableB, TableB + 8})
    Image.CodePtrRelocSlots.insert(Slot);
  Image.DataAddressRelocOperands[FieldVA] = {
      TableB, TableB, 8, OwnerVA, /*PCRelativeFromInstructionEnd=*/false};

  // The exact field names B, but scalar arithmetic derives A and the LOAD
  // consumes A's two entries.  B remains an independent boundary anchor.
  const AuthenticatedSourceAnchorExemption Adjusted{TableA,  FieldVA, TableB,
                                                    OwnerVA, TableA,  16};
  EXPECT_FALSE(authenticatedSourceAnchorExemptionMatches(
      Adjusted, TableA, 8, 4, FieldVA,
      Image.DataAddressRelocOperands.at(FieldVA)));
  EXPECT_EQ(boundCodePtrRunByNextAnchor(Image, TableA, 8, 4, {},
                                        {{FieldVA, Adjusted}}),
            2u);

  // A direct occurrence for an interior source chunk may be ignored only
  // under the same exact field/target/owner and candidate-local byte span.
  Image.DataAddressRelocOperands[FieldVA].EncodedValue = TableA + 8;
  Image.DataAddressRelocOperands[FieldVA].TargetVA = TableA + 8;
  const AuthenticatedSourceAnchorExemption Direct{
      TableA, FieldVA, TableA + 8, OwnerVA, TableA + 8, 8};
  EXPECT_TRUE(authenticatedSourceAnchorExemptionMatches(
      Direct, TableA, 8, 4, FieldVA,
      Image.DataAddressRelocOperands.at(FieldVA)));
  EXPECT_EQ(
      boundCodePtrRunByNextAnchor(Image, TableA, 8, 4, {}, {{FieldVA, Direct}}),
      4u);

  auto EscapesRun = Direct;
  EscapesRun.SourceByteCount = 32;
  EXPECT_FALSE(authenticatedSourceAnchorExemptionMatches(
      EscapesRun, TableA, 8, 4, FieldVA,
      Image.DataAddressRelocOperands.at(FieldVA)));
}

TEST(LowInstructionBoundary,
     RelocationFreeStackSourceRequiresExactOccurrenceAuthority) {
  constexpr va_t Table = 0x4000;
  constexpr va_t InteriorSource = Table + 16;
  constexpr va_t Owner = Table;

  const AuthenticatedSourceAnchorExemption Exemption{
      Table, InvalidVA, InteriorSource, Owner, InteriorSource, 8};
  RelocatedInstructionAddressOccurrence Occurrence;
  Occurrence.FieldVA = InvalidVA;
  Occurrence.InstructionAddr = 0x1014;
  Occurrence.OpSeq = 2;
  Occurrence.TargetVA = InteriorSource;
  Occurrence.TargetOwnerVA = Owner;
  Occurrence.Width = 8;
  Occurrence.Provenance = ConstantAddressProvenance::DataAddress;
  Occurrence.DefinesOutput = true;
  Occurrence.OutputOpcode = NdOp::INT_ADD;
  Occurrence.OutputWitness = NdVar::tmp(7, 8);
  Occurrence.Authority = RelocatedInstructionAddressProofKind::
      AArch64RelocationFreeDataDereference;
  Occurrence.SeedInstructionAddr = 0x1010;
  Occurrence.SeedOpSeq = 0;
  Occurrence.SeedOpcode = NdOp::COPY;
  Occurrence.SeedInputWitness = NdVar::addressFragment(Table, 8);
  Occurrence.SeedOutputWitness = NdVar::reg(8, 8);
  Occurrence.ArithmeticProof.push_back(
      {Occurrence.InstructionAddr, Occurrence.OpSeq, NdOp::INT_ADD, 0,
       Occurrence.SeedOutputWitness, NdVar::scalar(16, 8),
       Occurrence.OutputWitness});
  Occurrence.DereferenceInstructionAddr = 0x1018;
  Occurrence.DereferenceOpSeq = 0;
  Occurrence.DereferenceOpcode = NdOp::LOAD;
  Occurrence.DereferenceAddressWitness = Occurrence.OutputWitness;
  Occurrence.DereferenceAccessSize = 8;

  EXPECT_TRUE(authenticatedSourceAnchorExemptionMatches(Exemption, Table, 8, 5,
                                                        Occurrence));

  auto MayDepend = Occurrence;
  MayDepend.OutputMayDepend = true;
  EXPECT_FALSE(authenticatedSourceAnchorExemptionMatches(Exemption, Table, 8, 5,
                                                         MayDepend));

  auto FakeLoaderField = Occurrence;
  FakeLoaderField.Authority = RelocatedInstructionAddressProofKind::LoaderField;
  EXPECT_FALSE(authenticatedSourceAnchorExemptionMatches(Exemption, Table, 8, 5,
                                                         FakeLoaderField))
      << "LoaderField authority cannot exist without a real loader field";

  auto EscapesRun = Occurrence;
  const AuthenticatedSourceAnchorExemption TooWide{
      Table, InvalidVA, InteriorSource, Owner, InteriorSource, 32};
  EXPECT_FALSE(authenticatedSourceAnchorExemptionMatches(TooWide, Table, 8, 5,
                                                         EscapesRun));
}

TEST(LowInstructionBoundary, StackTableMutationArithmeticFailsClosed) {
  constexpr int64_t Min = std::numeric_limits<int64_t>::min();
  constexpr int64_t Max = std::numeric_limits<int64_t>::max();

  EXPECT_FALSE(stackCheckedOffset(Max, 1));
  EXPECT_FALSE(stackCheckedOffset(Min, -1));
  EXPECT_FALSE(stackCheckedOffset(Min, 1, /*Subtract=*/true));
  EXPECT_FALSE(stackCheckedOffset(Max, -1, /*Subtract=*/true));
  EXPECT_EQ(stackCheckedOffset(-8, 16), 8);
  EXPECT_EQ(stackCheckedOffset(8, 16, /*Subtract=*/true), -8);

  EXPECT_FALSE(checkedVAOffset(InvalidVA - 1, 2));
  EXPECT_FALSE(checkedVAOffset(0, -1));
  EXPECT_EQ(checkedVAOffset(InvalidVA - 1, 1), InvalidVA);
  EXPECT_EQ(checkedVAOffset(1, -1), 0u);
}

TEST(LowInstructionBoundary, JumpTableTargetsUseGuestWidthModularArithmetic) {
  const uint32_t PositiveOffset = 0x30;
  EXPECT_EQ(decodeTableEntry(reinterpret_cast<const uint8_t *>(&PositiveOffset),
                             4,
                             /*IsRelative=*/true, /*IsSigned=*/true,
                             /*BaseAddr=*/0xfffffffffffffff0ULL,
                             /*HasTargetBase=*/false, /*TargetBase=*/0,
                             /*Scale=*/1, /*AddressBytes=*/8),
            0x20u);

  const uint8_t WrappedOffset = 0x10;
  EXPECT_EQ(decodeTableEntry(&WrappedOffset, 1, /*IsRelative=*/true,
                             /*IsSigned=*/false, /*BaseAddr=*/0xfffffff8u,
                             /*HasTargetBase=*/false, /*TargetBase=*/0,
                             /*Scale=*/1, /*AddressBytes=*/4),
            8u);

  const uint8_t CompactEntry = 2;
  EXPECT_EQ(decodeTableEntry(&CompactEntry, 1, /*IsRelative=*/true,
                             /*IsSigned=*/false, /*BaseAddr=*/0x200,
                             /*HasTargetBase=*/true,
                             /*TargetBase=*/0xfffffffcu,
                             /*Scale=*/4, /*AddressBytes=*/4),
            4u);
}

TEST(LowInstructionBoundary, JumpTableKeepsPhysicalSlotsSeparateFromLabels) {
  JumpTable Table;
  Table.BaseAddr = 0x2000;
  Table.HasBaseAddr = true;
  Table.EntrySize = 8;
  Table.EntryStride = 16;
  Table.StorageRanges = {JumpTableStorageRange{0x2000, 8, 16, 3},
                         JumpTableStorageRange{0x3000, 4, 4, 2}};
  Table.Targets = {0x3000, 0x3020};
  Table.SlotIndices = {0, 2};
  Table.HasDispatchSlotMap = true;
  Table.CaseLabels = {-7, 41};

  EXPECT_EQ(Table.targetPositionForPhysicalSlot(0), 0u);
  EXPECT_FALSE(Table.targetPositionForPhysicalSlot(1).has_value());
  EXPECT_EQ(Table.targetPositionForPhysicalSlot(2), 1u);
  EXPECT_TRUE(Table.ownsStorageAddress(0x2000));
  EXPECT_FALSE(
      Table.ownsStorageAddress(0x2008)); // strided padding is not owned
  EXPECT_TRUE(Table.ownsStorageAddress(0x2010));
  EXPECT_FALSE(Table.ownsStorageAddress(0x2018));
  EXPECT_TRUE(Table.ownsStorageAddress(0x2027)); // end byte of trailing slot
  EXPECT_FALSE(Table.ownsStorageAddress(0x2028));
  EXPECT_FALSE(Table.ownsStorageAddress(0x2800)); // disjoint gap remains free
  EXPECT_TRUE(Table.ownsStorageAddress(0x3007));
  EXPECT_FALSE(Table.ownsStorageAddress(0x3008));
  EXPECT_FALSE(Table.storageEnd().has_value());
}

TEST(LowInstructionBoundary, JumpTableRejectsUnsupportedEntryWidths) {
  const uint8_t Bytes[16] = {};
  EXPECT_FALSE(decodeTableEntry(Bytes, 3, /*IsRelative=*/true,
                                /*IsSigned=*/false, /*BaseAddr=*/0,
                                /*HasTargetBase=*/true,
                                /*TargetBase=*/0x1000, /*Scale=*/4,
                                /*AddressBytes=*/8));
  EXPECT_FALSE(decodeTableEntry(Bytes, 16, /*IsRelative=*/false,
                                /*IsSigned=*/false, /*BaseAddr=*/0));
}

TEST(LowInstructionBoundary, FrameSlotDeltasFollowArithmeticWidthCoercion) {
  auto resolveOffset = [&](Arch Architecture, NdOp Opcode, NdVar Constant,
                           uint16_t OutputSize) -> std::optional<int64_t> {
    const TargetRegInfo &TRI = getTargetRegInfo(Architecture);
    EXPECT_EQ(TRI.PointerSize, OutputSize);
    LowOp Arithmetic;
    Arithmetic.Opcode = Opcode;
    Arithmetic.Output = NdVar::tmp(0x100, OutputSize);
    Arithmetic.addInput(NdVar::reg(TRI.StackPointer, OutputSize));
    Arithmetic.addInput(Constant);
    std::vector<LowOp> Ops{Arithmetic};
    uint64_t BaseReg = InvalidVA;
    int64_t Offset = 0;
    if (!frameSlotKey(Ops, 0, Arithmetic.Output, TRI, BaseReg, Offset))
      return std::nullopt;
    EXPECT_EQ(BaseReg, TRI.StackPointer);
    return Offset;
  };

  // Integer operands are zero-extended to the operation width.  Treating the
  // high bit of the i8 literal as a sign bit would analyze a different stack
  // address than the emitted 32-bit ADD/SUB executes.
  EXPECT_EQ(resolveOffset(Arch::X86, NdOp::INT_ADD, NdVar::scalar(0xf0, 1), 4),
            240);
  EXPECT_EQ(resolveOffset(Arch::X86, NdOp::INT_SUB, NdVar::scalar(0xf0, 1), 4),
            -240);

  // A genuine pointer-width two's-complement displacement remains negative.
  EXPECT_EQ(resolveOffset(Arch::X86, NdOp::INT_ADD,
                          NdVar::scalar(uint32_t{0xfffffff0}, 4), 4),
            -16);
  EXPECT_EQ(resolveOffset(Arch::X86, NdOp::INT_SUB,
                          NdVar::scalar(uint32_t{0xfffffff0}, 4), 4),
            16);

  // Exercise the full-width mask/sign path as well; it must make the same
  // distinction without shifting by 64 or relying on narrower truncation.
  EXPECT_EQ(resolveOffset(Arch::X64, NdOp::INT_ADD, NdVar::scalar(0xf0, 1), 8),
            240);
  EXPECT_EQ(resolveOffset(Arch::X64, NdOp::INT_SUB, NdVar::scalar(0xf0, 1), 8),
            -240);
  EXPECT_EQ(resolveOffset(Arch::X64, NdOp::INT_ADD,
                          NdVar::scalar(uint64_t{0xfffffffffffffff0}, 8), 8),
            -16);
  EXPECT_EQ(resolveOffset(Arch::X64, NdOp::INT_SUB,
                          NdVar::scalar(uint64_t{0xfffffffffffffff0}, 8), 8),
            16);
}

TEST(LowInstructionBoundary, IntegerAndMaskUsesOutputAndDynamicWidths) {
  // A wider encoded immediate is truncated to the operation result.  Using
  // the immediate's own width would invent a 512-value domain for an i8 AND.
  EXPECT_EQ(effectiveIntegerAndMask(/*EncodedMask=*/0x1ff,
                                    /*MaskSize=*/2,
                                    /*DynamicSize=*/1,
                                    /*OutputSize=*/1),
            0xffu);

  // A narrow dynamic input is zero-extended before a wider AND, so its newly
  // introduced high output bits remain unreachable even when the mask sets
  // them.
  EXPECT_EQ(effectiveIntegerAndMask(/*EncodedMask=*/0x1ff,
                                    /*MaskSize=*/2,
                                    /*DynamicSize=*/1,
                                    /*OutputSize=*/2),
            0xffu);

  // With a genuinely wide dynamic operand the ninth bit is reachable.
  EXPECT_EQ(effectiveIntegerAndMask(/*EncodedMask=*/0x1ff,
                                    /*MaskSize=*/2,
                                    /*DynamicSize=*/2,
                                    /*OutputSize=*/2),
            0x1ffu);
  EXPECT_FALSE(effectiveIntegerAndMask(/*EncodedMask=*/~uint64_t{0},
                                       /*MaskSize=*/16,
                                       /*DynamicSize=*/8,
                                       /*OutputSize=*/8));
}

LowFunc buildFunction(Arch Architecture, InstructionMode Mode,
                      std::vector<uint8_t> Bytes,
                      std::vector<Symbol> Symbols = {}) {
  BinaryImage Image;
  Image.Arch = Architecture;
  Image.Mode = Mode;
  Image.Bits = Architecture == Arch::X64 || Architecture == Arch::AArch64
                   ? Bitness::Bits64
                   : Bitness::Bits32;
  Image.Format = BinaryFormat::ELF;
  Image.Base = kEntry;
  Image.Symbols = std::move(Symbols);

  Segment Text;
  Text.VA = kEntry;
  Text.Size = Bytes.size();
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = std::move(Bytes);
  Image.Segments.push_back(std::move(Text));

  Decoder Dec;
  if (!Dec.init(Architecture, Mode)) {
    ADD_FAILURE() << "decoder initialization failed";
    return {};
  }
  CFGBuilder Builder;
  return Builder.build(Image, Dec, kEntry, "instruction_boundaries");
}

const LowBlock *findBlock(const LowFunc &Function, va_t Address) {
  for (const LowBlock &Block : Function.Blocks)
    if (Block.StartAddr == Address)
      return &Block;
  return nullptr;
}

TEST(LowInstructionBoundary,
     X86FSGSOverridesSurviveLiftingAsTargetMemoryAddressSpaces) {
  // mov rax, qword ptr fs:[0x28]
  // mov qword ptr gs:[0x30], rdx
  // mov r8, qword ptr gs:[0x30]
  // lea rcx, qword ptr fs:[rbx + 0x20]  (the segment prefix is ignored)
  // xor rax, r8
  // ret
  const std::vector<uint8_t> Bytes = {
      0x64, 0x48, 0x8b, 0x04, 0x25, 0x28, 0x00, 0x00, 0x00, 0x65, 0x48, 0x89,
      0x14, 0x25, 0x30, 0x00, 0x00, 0x00, 0x65, 0x4c, 0x8b, 0x04, 0x25, 0x30,
      0x00, 0x00, 0x00, 0x64, 0x48, 0x8d, 0x4b, 0x20, 0x4c, 0x31, 0xc0, 0xc3,
  };
  LowFunc Low = buildFunction(Arch::X64, InstructionMode::Default, Bytes);
  ASSERT_EQ(Low.Blocks.size(), 1u);

  const LowOp *FSLoad = nullptr;
  const LowOp *GSStore = nullptr;
  bool FSOffsetIsScalar = false;
  bool GSOffsetIsScalar = false;
  for (const LowOp &Op : Low.Blocks.front().Ops) {
    if (Op.Addr == kEntry && Op.Opcode == NdOp::LOAD)
      FSLoad = &Op;
    if (Op.Addr == kEntry + 9 && Op.Opcode == NdOp::STORE)
      GSStore = &Op;
    if (Op.Addr == kEntry && Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
        Op.Inputs[0].isConst() && Op.Inputs[0].Offset == 0x28)
      FSOffsetIsScalar =
          Op.Inputs[0].Provenance == ConstantAddressProvenance::Scalar;
    if (Op.Addr == kEntry + 9 && Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
        Op.Inputs[0].isConst() && Op.Inputs[0].Offset == 0x30)
      GSOffsetIsScalar =
          Op.Inputs[0].Provenance == ConstantAddressProvenance::Scalar;
    if (Op.Addr == kEntry + 27)
      EXPECT_EQ(Op.MemoryAddressSpace, NdMemoryAddressSpace::Default)
          << "LEA must ignore FS/GS segment bases";
  }
  ASSERT_NE(FSLoad, nullptr);
  ASSERT_NE(GSStore, nullptr);
  EXPECT_EQ(FSLoad->MemoryAddressSpace, NdMemoryAddressSpace::X86FS);
  EXPECT_EQ(GSStore->MemoryAddressSpace, NdMemoryAddressSpace::X86GS);
  EXPECT_TRUE(FSOffsetIsScalar);
  EXPECT_TRUE(GSOffsetIsScalar);

  constexpr uint64_t FSBase = 0x2000;
  constexpr uint64_t GSBase = 0x3000;
  constexpr uint64_t FSValue = UINT64_C(0x1122334455667788);
  constexpr uint64_t StoredValue = UINT64_C(0x8877665544332211);
  BinaryImage SemanticImage;
  SemanticImage.Arch = Arch::X64;
  SemanticImage.Bits = Bitness::Bits64;
  SemanticImage.Format = BinaryFormat::ELF;
  Segment FSData;
  FSData.VA = FSBase;
  FSData.Size = 0x100;
  FSData.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  FSData.Data.resize(FSData.Size);
  for (unsigned I = 0; I != sizeof(FSValue); ++I)
    FSData.Data[0x28 + I] = static_cast<uint8_t>(FSValue >> (I * 8));
  SemanticImage.Segments.push_back(std::move(FSData));
  Segment GSData;
  GSData.VA = GSBase;
  GSData.Size = 0x100;
  GSData.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  GSData.Data.resize(GSData.Size);
  SemanticImage.Segments.push_back(std::move(GSData));

  NdOpEmulator MissingBases(SemanticImage);
  EXPECT_LT(MissingBases.run(Low.Blocks.front()),
            Low.Blocks.front().Ops.size());
  EXPECT_FALSE(MissingBases.getRegister(x86reg::RAX));

  NdOpEmulator Emulator(SemanticImage);
  ASSERT_TRUE(
      Emulator.setMemoryAddressSpaceBase(NdMemoryAddressSpace::X86FS, FSBase));
  ASSERT_TRUE(
      Emulator.setMemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS, GSBase));
  Emulator.setRegister(x86reg::RDX, StoredValue);
  Emulator.setRegister(x86reg::RBX, 0);
  Emulator.run(Low.Blocks.front());
  ASSERT_TRUE(Emulator.getRegister(x86reg::RAX));
  ASSERT_TRUE(Emulator.getRegister(x86reg::R8));
  ASSERT_TRUE(Emulator.getRegister(x86reg::RCX));
  EXPECT_EQ(*Emulator.getRegister(x86reg::RAX), FSValue ^ StoredValue);
  EXPECT_EQ(*Emulator.getRegister(x86reg::R8), StoredValue);
  EXPECT_EQ(*Emulator.getRegister(x86reg::RCX), 0x20u);

  MedFunc Med = LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::ELF);
  const MedOp *MedFSLoad = nullptr;
  const MedOp *MedGSStore = nullptr;
  const MedOp *MedGSLoad = nullptr;
  for (const MedBlock &Block : Med.Blocks)
    for (const MedOp &Op : Block.Ops) {
      if (Op.Addr == kEntry && Op.Opcode == NdOp::LOAD)
        MedFSLoad = &Op;
      if (Op.Addr == kEntry + 9 && Op.Opcode == NdOp::STORE)
        MedGSStore = &Op;
      if (Op.Addr == kEntry + 18 && Op.Opcode == NdOp::LOAD)
        MedGSLoad = &Op;
    }
  ASSERT_NE(MedFSLoad, nullptr);
  ASSERT_NE(MedGSStore, nullptr);
  ASSERT_NE(MedGSLoad, nullptr);
  EXPECT_EQ(MedFSLoad->MemoryAddressSpace, NdMemoryAddressSpace::X86FS);
  EXPECT_EQ(MedGSStore->MemoryAddressSpace, NdMemoryAddressSpace::X86GS);
  EXPECT_EQ(MedGSLoad->MemoryAddressSpace, NdMemoryAddressSpace::X86GS);

  // OUT consumes AL/AX/EAX but never defines the accumulator.  Treating RAX
  // as an intrinsic result creates a false architectural register version
  // that can hide its real value from subsequent instructions.
  LowFunc OutLow = buildFunction(Arch::X64, InstructionMode::Default,
                                 {0xee, 0x48, 0x89, 0xc3, 0xc3});
  const LowOp *Out = nullptr;
  for (const LowOp &Op : OutLow.Blocks.front().Ops)
    if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs != 0 &&
        Op.Inputs[0].isConst() &&
        static_cast<Intrinsic>(Op.Inputs[0].Offset) == Intrinsic::Out)
      Out = &Op;
  ASSERT_NE(Out, nullptr);
  EXPECT_EQ(Out->Output.Size, 0u);
  ASSERT_GE(Out->NumInputs, 3u);
  EXPECT_EQ(Out->Inputs[2], NdVar::reg(x86reg::RAX, 1));

  LowFunc CrossDomainFrameLow = buildFunction(
      Arch::X64, InstructionMode::Default,
      {0x64, 0x48, 0x89, 0x44, 0x24, 0x08, // mov fs:[rsp+8], rax
       0x48, 0x8b, 0x44, 0x24, 0x08,       // mov rax, [rsp+8]
       0x48, 0x89, 0x03,                   // mov [rbx], rax
       0xc3});
  MedFunc CrossDomainFrameMed = LowToMedConverter().convert(
      CrossDomainFrameLow, Arch::X64, BinaryFormat::ELF);
  unsigned CrossDomainFSStores = 0;
  const MedOp *CrossDomainFSStore = nullptr;
  const MedOp *CrossDomainDefaultSink = nullptr;
  for (const MedBlock &Block : CrossDomainFrameMed.Blocks)
    for (const MedOp &Op : Block.Ops) {
      if (Op.Opcode == NdOp::STORE &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::X86FS) {
        ++CrossDomainFSStores;
        CrossDomainFSStore = &Op;
      }
      if (Op.Addr == kEntry + 11 && Op.Opcode == NdOp::STORE &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default)
        CrossDomainDefaultSink = &Op;
    }
  EXPECT_EQ(CrossDomainFSStores, 1u);
  ASSERT_NE(CrossDomainFSStore, nullptr);
  ASSERT_NE(CrossDomainDefaultSink, nullptr);
  ASSERT_GE(CrossDomainFSStore->NumInputs, 2u);
  ASSERT_GE(CrossDomainDefaultSink->NumInputs, 2u);
  EXPECT_NE(CrossDomainDefaultSink->Inputs[1],
            CrossDomainFSStore->Inputs[1])
      << "an FS frame store cannot define a default stack reload";
  EXPECT_EQ(CrossDomainDefaultSink->Inputs[1].Kind, MedVar::Param)
      << "the default stack value remains the incoming stack parameter";

  llvm::LLVMContext Context;
  auto Module =
      MedLLVMEmitter().emit({Med}, Context, "segment-memory", Arch::X64);
  ASSERT_NE(Module, nullptr);
  EXPECT_TRUE(validLLVMModule(*Module));
  unsigned FSLoads = 0;
  unsigned GSLoads = 0;
  unsigned GSStores = 0;
  for (const llvm::Function &Function : *Module)
    for (const llvm::BasicBlock &Block : Function)
      for (const llvm::Instruction &Instruction : Block) {
        if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction))
          if (unsigned AddressSpace = Load->getPointerOperand()
                                          ->getType()
                                          ->getPointerAddressSpace();
              AddressSpace == 257)
            ++FSLoads;
          else if (AddressSpace == 256)
            ++GSLoads;
        if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction))
          GSStores +=
              Store->getPointerOperand()->getType()->getPointerAddressSpace() ==
              256;
      }
  EXPECT_EQ(FSLoads, 1u);
  EXPECT_EQ(GSLoads, 1u);
  EXPECT_EQ(GSStores, 1u);

  auto LiftStringInstruction = [](std::initializer_list<uint8_t> Bytes) {
    Decoder Dec;
    std::vector<LowOp> Ops;
    if (!Dec.init(Arch::X64)) {
      ADD_FAILURE() << "string decoder initialization failed";
      return Ops;
    }
    DecodedInsn Insn{};
    if (Dec.decodeOneForLift(Bytes.begin(), Bytes.size(), kEntry, Insn) !=
        static_cast<int>(Bytes.size())) {
      ADD_FAILURE() << "string instruction decode failed";
      return Ops;
    }
    Dec.liftToLow(Insn, Ops);
    return Ops;
  };
  auto CountOps = [](const std::vector<LowOp> &Ops, NdOp Opcode,
                     NdMemoryAddressSpace AddressSpace) {
    return std::count_if(Ops.begin(), Ops.end(), [&](const LowOp &Op) {
      return Op.Opcode == Opcode && Op.MemoryAddressSpace == AddressSpace;
    });
  };
  auto FindIntrinsic = [](const std::vector<LowOp> &Ops, Intrinsic Id) {
    return std::find_if(Ops.begin(), Ops.end(), [&](const LowOp &Op) {
      return Op.Opcode == NdOp::INTRINSIC && Op.NumInputs >= 1 &&
             Op.Inputs[0].isConst() &&
             Op.Inputs[0].Offset == static_cast<uint64_t>(Id);
    });
  };

  // Real instruction bytes cover the implicit byte-masked store path.  The
  // address-size override selects EDI, the FS/GS prefix survives on the
  // intrinsic, and no unconditional STORE may remain.
  const std::vector<LowOp> FSMaskMov =
      LiftStringInstruction({0x64, 0x67, 0x66, 0x0f, 0xf7, 0xc1});
  auto FSMaskMovOp = FindIntrinsic(FSMaskMov, Intrinsic::MaskedStoreB);
  ASSERT_NE(FSMaskMovOp, FSMaskMov.end());
  ASSERT_GE(FSMaskMovOp->NumInputs, 4u);
  EXPECT_EQ(FSMaskMovOp->MemoryAddressSpace, NdMemoryAddressSpace::X86FS);
  EXPECT_EQ(FSMaskMovOp->Inputs[1].Size, 8u);
  EXPECT_EQ(FSMaskMovOp->Inputs[2].Size, 16u);
  EXPECT_EQ(FSMaskMovOp->Inputs[3].Size, 16u);
  EXPECT_EQ(std::count_if(FSMaskMov.begin(), FSMaskMov.end(),
                          [](const LowOp &Op) {
                            return Op.Opcode == NdOp::STORE;
                          }),
            0);
  EXPECT_TRUE(std::any_of(FSMaskMov.begin(), FSMaskMov.end(),
                          [](const LowOp &Op) {
                            return Op.Opcode == NdOp::INT_ZEXT &&
                                   Op.Output.Size == 8 && Op.NumInputs == 1 &&
                                   Op.Inputs[0].Size == 4;
                          }));

  const std::vector<LowOp> GSVMASKMov =
      LiftStringInstruction({0x65, 0x67, 0xc5, 0xf9, 0xf7, 0xc1});
  auto GSVMASKMovOp = FindIntrinsic(GSVMASKMov, Intrinsic::MaskedStoreB);
  ASSERT_NE(GSVMASKMovOp, GSVMASKMov.end());
  EXPECT_EQ(GSVMASKMovOp->MemoryAddressSpace, NdMemoryAddressSpace::X86GS);

  // addr32 VSIB gather performs every address calculation at 32 bits, then
  // zero-extends the wrapped offset.  Each lane uses MaskedLoadD, so a clear
  // mask cannot fault and the old destination lane is selected as passthrough.
  const std::vector<LowOp> GSGather = LiftStringInstruction(
      {0x65, 0x67, 0xc4, 0xe2, 0x6d, 0x92, 0x44, 0x88, 0x20});
  unsigned GatherMaskedLoads = 0;
  unsigned GatherPlainLoads = 0;
  unsigned GatherSelects = 0;
  unsigned GatherRawMasks = 0;
  for (const LowOp &Op : GSGather) {
    GatherPlainLoads += Op.Opcode == NdOp::LOAD;
    GatherSelects += Op.Opcode == NdOp::SELECT;
    if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs >= 3 &&
        Op.Inputs[0].isConst() &&
        Op.Inputs[0].Offset ==
            static_cast<uint64_t>(Intrinsic::MaskedLoadD)) {
      ++GatherMaskedLoads;
      EXPECT_EQ(Op.MemoryAddressSpace, NdMemoryAddressSpace::X86GS);
      EXPECT_EQ(Op.Inputs[1].Size, 8u);
      EXPECT_EQ(Op.Inputs[2].Size, 16u);
      const NdVar Mask = Op.Inputs[2];
      GatherRawMasks += std::any_of(
          GSGather.begin(), GSGather.end(), [&](const LowOp &Def) {
            return Def.Opcode == NdOp::INT_ZEXT && Def.Output == Mask &&
                   Def.Output.Size == 16 && Def.NumInputs == 1 &&
                   Def.Inputs[0].Size == 4;
          });
    }
  }
  EXPECT_EQ(GatherPlainLoads, 0u);
  EXPECT_EQ(GatherMaskedLoads, 8u);
  EXPECT_EQ(GatherRawMasks, 8u);
  EXPECT_GE(GatherSelects, 8u);
  EXPECT_TRUE(std::any_of(GSGather.begin(), GSGather.end(),
                          [](const LowOp &Op) {
                            return Op.Opcode == NdOp::INT_ADD &&
                                   Op.Output.Size == 4;
                          }));
  EXPECT_TRUE(std::any_of(GSGather.begin(), GSGather.end(),
                          [](const LowOp &Op) {
                            return Op.Opcode == NdOp::INT_ZEXT &&
                                   Op.Output.Size == 8 && Op.NumInputs == 1 &&
                                   Op.Inputs[0].Size == 4;
                          }));

  auto DwordVector = [](const std::array<uint32_t, 8> &Values) {
    std::vector<uint8_t> Bytes(32);
    std::memcpy(Bytes.data(), Values.data(), Bytes.size());
    return Bytes;
  };
  std::vector<uint64_t> GatherVectorRegs;
  for (const LowOp &Op : GSGather)
    if (Op.Opcode == NdOp::SUBBYTES && Op.Output.Size == 4 &&
        Op.NumInputs >= 1 && Op.Inputs[0].isReg() &&
        Op.Inputs[0].Size == 32 &&
        std::find(GatherVectorRegs.begin(), GatherVectorRegs.end(),
                  Op.Inputs[0].Offset) == GatherVectorRegs.end())
      GatherVectorRegs.push_back(Op.Inputs[0].Offset);
  ASSERT_EQ(GatherVectorRegs.size(), 3u);
  const uint64_t GatherIndexReg = GatherVectorRegs[0];
  const uint64_t GatherMaskReg = GatherVectorRegs[1];
  const uint64_t GatherDestReg = GatherVectorRegs[2];
  const std::array<uint32_t, 8> GatherIndices{0, 1, 2, 3, 4, 5, 6, 7};
  const std::array<uint32_t, 8> GatherMasks{
      UINT32_C(0x80000000), 0, UINT32_C(0x80000000), 0,
      0, 0, 0, UINT32_C(0x80000000)};
  const std::array<uint32_t, 8> GatherOld{
      0xaaaa0000, 0xaaaa0001, 0xaaaa0002, 0xaaaa0003,
      0xaaaa0004, 0xaaaa0005, 0xaaaa0006, 0xaaaa0007};
  const std::array<uint32_t, 8> GatherMemory{
      0x12340000, 0x12340001, 0x12340002, 0x12340003,
      0x12340004, 0x12340005, 0x12340006, 0x12340007};
  BinaryImage GatherSemanticImage = SemanticImage;
  auto GatherGSSegment = std::find_if(
      GatherSemanticImage.Segments.begin(), GatherSemanticImage.Segments.end(),
      [&](const Segment &Segment) { return Segment.VA == GSBase; });
  ASSERT_NE(GatherGSSegment, GatherSemanticImage.Segments.end());
  std::memcpy(GatherGSSegment->Data.data() + 0x20, GatherMemory.data(),
              sizeof(GatherMemory));

  NdOpEmulator GatherEmulator(GatherSemanticImage);
  ASSERT_TRUE(GatherEmulator.setMemoryAddressSpaceBase(
      NdMemoryAddressSpace::X86GS, GSBase));
  GatherEmulator.setLoadCollect(true);
  GatherEmulator.setRegister(x86reg::RAX, 0);
  GatherEmulator.setRegisterBytes(GatherIndexReg, DwordVector(GatherIndices));
  GatherEmulator.setRegisterBytes(GatherMaskReg, DwordVector(GatherMasks));
  GatherEmulator.setRegisterBytes(GatherDestReg, DwordVector(GatherOld));
  EXPECT_EQ(GatherEmulator.run(GSGather), GSGather.size());
  const auto GatherResult = GatherEmulator.getRegisterBytes(GatherDestReg);
  ASSERT_TRUE(GatherResult);
  ASSERT_EQ(GatherResult->size(), 32u);
  std::array<uint32_t, 8> GatherResultWords{};
  std::memcpy(GatherResultWords.data(), GatherResult->data(),
              GatherResult->size());
  for (size_t I = 0; I < GatherResultWords.size(); ++I)
    EXPECT_EQ(GatherResultWords[I],
              (GatherMasks[I] & UINT32_C(0x80000000)) ? GatherMemory[I]
                                                       : GatherOld[I]);
  const auto ClearedGatherMask =
      GatherEmulator.getRegisterBytes(GatherMaskReg);
  ASSERT_TRUE(ClearedGatherMask);
  EXPECT_TRUE(std::all_of(ClearedGatherMask->begin(), ClearedGatherMask->end(),
                          [](uint8_t Byte) { return Byte == 0; }));
  ASSERT_EQ(GatherEmulator.getLoadRecords().size(), 3u);
  EXPECT_EQ(GatherEmulator.getLoadRecords()[0].Addr, GSBase + 0x20);
  EXPECT_EQ(GatherEmulator.getLoadRecords()[1].Addr, GSBase + 0x28);
  EXPECT_EQ(GatherEmulator.getLoadRecords()[2].Addr, GSBase + 0x3c);

  // All mask sign bits clear: no lane may touch even an unmapped address, the
  // destination is preserved, and the architectural mask is still cleared.
  NdOpEmulator SuppressedGather(GatherSemanticImage);
  ASSERT_TRUE(SuppressedGather.setMemoryAddressSpaceBase(
      NdMemoryAddressSpace::X86GS, GSBase));
  SuppressedGather.setLoadCollect(true);
  SuppressedGather.setRegister(x86reg::RAX, UINT32_C(0x100000));
  SuppressedGather.setRegisterBytes(GatherIndexReg,
                                    DwordVector(GatherIndices));
  SuppressedGather.setRegisterBytes(
      GatherMaskReg, DwordVector(std::array<uint32_t, 8>{}));
  SuppressedGather.setRegisterBytes(GatherDestReg, DwordVector(GatherOld));
  EXPECT_EQ(SuppressedGather.run(GSGather), GSGather.size());
  EXPECT_TRUE(SuppressedGather.getLoadRecords().empty());
  const auto SuppressedResult =
      SuppressedGather.getRegisterBytes(GatherDestReg);
  ASSERT_TRUE(SuppressedResult);
  EXPECT_EQ(*SuppressedResult, DwordVector(GatherOld));

  // Gather faults preserve completed architectural progress.  This model uses
  // the permitted deterministic low-to-high order: lane 0 succeeds and is
  // committed/cleared, lane 1 faults on an unmapped address, and all later
  // destination/mask lanes retain their pre-instruction values.
  BinaryImage PartialGatherImage;
  PartialGatherImage.Arch = Arch::X64;
  PartialGatherImage.Bits = Bitness::Bits64;
  PartialGatherImage.Format = BinaryFormat::ELF;
  Segment PartialGatherData;
  PartialGatherData.VA = GSBase + 0x20;
  PartialGatherData.Size = sizeof(uint32_t);
  PartialGatherData.Flags = SegmentFlags::Readable;
  PartialGatherData.Data.resize(sizeof(uint32_t));
  const uint32_t PartialLane0 = UINT32_C(0xdec0ad01);
  std::memcpy(PartialGatherData.Data.data(), &PartialLane0,
              sizeof(PartialLane0));
  PartialGatherImage.Segments.push_back(std::move(PartialGatherData));
  std::array<uint32_t, 8> PartialMasks{};
  PartialMasks[0] = UINT32_C(0x80000000);
  PartialMasks[1] = UINT32_C(0x80000000);
  NdOpEmulator PartialGather(PartialGatherImage);
  ASSERT_TRUE(PartialGather.setMemoryAddressSpaceBase(
      NdMemoryAddressSpace::X86GS, GSBase));
  PartialGather.setLoadCollect(true);
  PartialGather.setRegister(x86reg::RAX, 0);
  PartialGather.setRegisterBytes(GatherIndexReg,
                                 DwordVector(GatherIndices));
  PartialGather.setRegisterBytes(GatherMaskReg,
                                 DwordVector(PartialMasks));
  PartialGather.setRegisterBytes(GatherDestReg, DwordVector(GatherOld));
  EXPECT_LT(PartialGather.run(GSGather), GSGather.size());
  const auto PartialResult = PartialGather.getRegisterBytes(GatherDestReg);
  const auto PartialMaskResult =
      PartialGather.getRegisterBytes(GatherMaskReg);
  ASSERT_TRUE(PartialResult);
  ASSERT_TRUE(PartialMaskResult);
  std::array<uint32_t, 8> PartialResultWords{};
  std::array<uint32_t, 8> PartialMaskWords{};
  std::memcpy(PartialResultWords.data(), PartialResult->data(),
              PartialResult->size());
  std::memcpy(PartialMaskWords.data(), PartialMaskResult->data(),
              PartialMaskResult->size());
  EXPECT_EQ(PartialResultWords[0], PartialLane0);
  EXPECT_EQ(PartialMaskWords[0], 0u);
  EXPECT_EQ(PartialResultWords[1], GatherOld[1]);
  EXPECT_EQ(PartialMaskWords[1], UINT32_C(0x80000000));
  for (size_t I = 2; I < PartialResultWords.size(); ++I) {
    EXPECT_EQ(PartialResultWords[I], GatherOld[I]);
    EXPECT_EQ(PartialMaskWords[I], 0u);
  }
  ASSERT_EQ(PartialGather.getLoadRecords().size(), 1u);
  EXPECT_EQ(PartialGather.getLoadRecords().front().Addr, GSBase + 0x20);

  std::vector<uint8_t> MaskMovData(16);
  std::vector<uint8_t> MaskMovMask(16, 0);
  for (size_t I = 0; I < MaskMovData.size(); ++I)
    MaskMovData[I] = static_cast<uint8_t>(0x40 + I);
  MaskMovMask[0] = 0x80;
  MaskMovMask[3] = 0x80;
  MaskMovMask[15] = 0x80;
  NdOpEmulator MaskMovEmulator(SemanticImage);
  ASSERT_TRUE(MaskMovEmulator.setMemoryAddressSpaceBase(
      NdMemoryAddressSpace::X86FS, FSBase));
  MaskMovEmulator.setRegister(x86reg::RDI, 0x60);
  MaskMovEmulator.setRegisterBytes(FSMaskMovOp->Inputs[2].Offset,
                                   MaskMovMask);
  MaskMovEmulator.setRegisterBytes(FSMaskMovOp->Inputs[3].Offset,
                                   MaskMovData);
  EXPECT_EQ(MaskMovEmulator.run(FSMaskMov), FSMaskMov.size());
  for (size_t I = 0; I < MaskMovData.size(); ++I) {
    LowOp Probe;
    Probe.Opcode = NdOp::LOAD;
    Probe.Output = NdVar::tmp(0x700 + I, 1);
    Probe.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
    Probe.addInput(NdVar::scalar(0x60 + I, 8));
    ASSERT_TRUE(MaskMovEmulator.step(Probe));
    ASSERT_TRUE(MaskMovEmulator.getRegister(Probe.Output.Offset));
    EXPECT_EQ(*MaskMovEmulator.getRegister(Probe.Output.Offset),
              (MaskMovMask[I] & 0x80) ? MaskMovData[I] : 0u);
  }

  LowFunc GatherLow = buildFunction(
      Arch::X64, InstructionMode::Default,
      {0x65, 0x67, 0xc4, 0xe2, 0x6d, 0x92, 0x44, 0x88, 0x20, 0xc3});
  MedFunc GatherMed =
      LowToMedConverter().convert(GatherLow, Arch::X64, BinaryFormat::ELF);
  unsigned LiveGatherDstCommits = 0;
  unsigned LiveGatherMaskCommits = 0;
  for (const MedBlock &Block : GatherMed.Blocks)
    for (const MedOp &Op : Block.Ops) {
      if (Op.Dead || Op.Opcode != NdOp::COPY ||
          Op.Output.Kind != MedVar::Reg)
        continue;
      LiveGatherDstCommits += Op.Output.RegOff == GatherDestReg;
      LiveGatherMaskCommits += Op.Output.RegOff == GatherMaskReg;
    }
  EXPECT_GE(LiveGatherDstCommits, 7u);
  EXPECT_GE(LiveGatherMaskCommits, 7u)
      << "each pre-final lane commit must feed the next faulting lane after "
         "Low-to-Med DCE";
  llvm::LLVMContext GatherContext;
  auto GatherModule = MedLLVMEmitter().emit({GatherMed}, GatherContext,
                                            "segment-gather", Arch::X64);
  ASSERT_NE(GatherModule, nullptr);
  EXPECT_TRUE(validLLVMModule(*GatherModule));
  unsigned LLVMGatherMaskedLoads = 0;
  for (const llvm::Function &Function : *GatherModule)
    for (const llvm::BasicBlock &Block : Function)
      for (const llvm::Instruction &Instruction : Block)
        if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction))
          if (Call->getCalledFunction() &&
              Call->getCalledFunction()->getName().starts_with(
                  "llvm.masked.load") &&
              Call->getArgOperand(0)->getType()->getPointerAddressSpace() ==
                  256)
            ++LLVMGatherMaskedLoads;
  EXPECT_EQ(LLVMGatherMaskedLoads, 8u);
  HighFunc GatherHigh = MedToHighConverter().convert(GatherMed, Arch::X64);
  std::string GatherHighC;
  llvm::raw_string_ostream GatherHighCOS(GatherHighC);
  CEmitterOptions GatherCOptions;
  GatherCOptions.TheArch = Arch::X64;
  ASSERT_TRUE(
      HighCEmitter().emit({GatherHigh}, GatherHighCOS, GatherCOptions));
  GatherHighCOS.flush();
  EXPECT_NE(GatherHighC.find("vmaskmovps %%gs:(%[address])"),
            std::string::npos);
  EXPECT_TRUE(validHighC(GatherHighC));

  // The full 64-bit bit index selects a chunk before the byte offset is
  // truncated into addr32 and added to the wrapped FS-relative base.
  const std::vector<LowOp> FSAddr32BT = LiftStringInstruction(
      {0x64, 0x67, 0x48, 0x0f, 0xa3, 0x48, 0x10});
  EXPECT_EQ(CountOps(FSAddr32BT, NdOp::LOAD, NdMemoryAddressSpace::X86FS), 1);
  EXPECT_TRUE(std::any_of(FSAddr32BT.begin(), FSAddr32BT.end(),
                          [](const LowOp &Op) {
                            return Op.Opcode == NdOp::INT_ASHR &&
                                   Op.Output.Size == 8;
                          }));
  EXPECT_TRUE(std::any_of(FSAddr32BT.begin(), FSAddr32BT.end(),
                          [](const LowOp &Op) {
                            return Op.Opcode == NdOp::SUBBYTES &&
                                   Op.Output.Size == 4 && Op.NumInputs >= 1 &&
                                   Op.Inputs[0].Size == 8;
                          }));

  const std::vector<LowOp> FSRipCall = LiftStringInstruction(
      {0x64, 0xff, 0x15, 0x00, 0x00, 0x00, 0x00});
  const LowOp *FSCallLoad = nullptr;
  const LowOp *FSIndirectCall = nullptr;
  for (const LowOp &Op : FSRipCall) {
    if (Op.Opcode == NdOp::LOAD &&
        Op.MemoryAddressSpace == NdMemoryAddressSpace::X86FS)
      FSCallLoad = &Op;
    if (Op.Opcode == NdOp::INDIR_CALL)
      FSIndirectCall = &Op;
  }
  ASSERT_NE(FSCallLoad, nullptr);
  ASSERT_NE(FSIndirectCall, nullptr);
  ASSERT_GE(FSIndirectCall->NumInputs, 1u);
  EXPECT_TRUE(FSIndirectCall->Inputs[0] == FSCallLoad->Output);

  const std::vector<LowOp> GSRipJump = LiftStringInstruction(
      {0x65, 0xff, 0x25, 0x00, 0x00, 0x00, 0x00});
  EXPECT_EQ(CountOps(GSRipJump, NdOp::LOAD, NdMemoryAddressSpace::X86GS), 1);
  EXPECT_TRUE(std::any_of(GSRipJump.begin(), GSRipJump.end(),
                          [](const LowOp &Op) {
                            return Op.Opcode == NdOp::INDIR_BR;
                          }));

  LowOp SegmentTableLoad;
  SegmentTableLoad.Addr = 0x1400;
  SegmentTableLoad.Seq = 3;
  SegmentTableLoad.Opcode = NdOp::LOAD;
  SegmentTableLoad.Output = NdVar::tmp(77, 8);
  SegmentTableLoad.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
  SegmentTableLoad.addInput(NdVar::scalar(0x4000, 8));
  LowOp SegmentTableBranch;
  SegmentTableBranch.Addr = 0x1400;
  SegmentTableBranch.Seq = 4;
  SegmentTableBranch.Opcode = NdOp::INDIR_BR;
  SegmentTableBranch.addInput(SegmentTableLoad.Output);
  const std::array<LowOp, 2> SegmentTableOps{SegmentTableLoad,
                                             SegmentTableBranch};
  EXPECT_FALSE(jumpTableTargetLoadUsesDefaultAddressSpace(
      SegmentTableOps, SegmentTableLoad.Addr, SegmentTableLoad.Seq,
      SegmentTableLoad.Output))
      << "an FS offset equal to an image table VA must publish no targets";
  SegmentTableLoad.MemoryAddressSpace = NdMemoryAddressSpace::Default;
  const std::array<LowOp, 2> DefaultTableOps{SegmentTableLoad,
                                             SegmentTableBranch};
  EXPECT_TRUE(jumpTableTargetLoadUsesDefaultAddressSpace(
      DefaultTableOps, SegmentTableLoad.Addr, SegmentTableLoad.Seq,
      SegmentTableLoad.Output));

  // Exercise the real resolver publication path as well.  The default form is
  // a valid six-way COFF image-relative table; adding FS or GS to the exact
  // target LOAD changes its address domain and must suppress the entire table,
  // even though the numeric offset still points into the flat image bytes.
  constexpr va_t ResolverImageBase = UINT64_C(0x140000000);
  constexpr va_t ResolverFunctionVA = ResolverImageBase + 0x1000;
  constexpr va_t ResolverTableVA = ResolverImageBase + 0x1100;
  auto MakeResolverImage = [&](uint8_t SegmentPrefix) {
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    Image.Format = BinaryFormat::COFF;
    Image.Base = ResolverImageBase;
    Image.Entry = ResolverFunctionVA;

    Segment Text;
    Text.Name = ".text";
    Text.VA = ResolverFunctionVA;
    Text.Size = (ResolverTableVA - ResolverFunctionVA) +
                6 * sizeof(uint32_t);
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.assign(Text.Size, 0x90);
    std::vector<uint8_t> Dispatch;
    auto AppendU32 = [&](uint32_t Value) {
      for (unsigned Byte = 0; Byte < sizeof(Value); ++Byte)
        Dispatch.push_back(static_cast<uint8_t>(Value >> (Byte * 8)));
    };
    auto AppendImageBaseLEA = [&](uint8_t REX, uint8_t ModRM) {
      const va_t NextInstruction =
          ResolverFunctionVA + Dispatch.size() + 7;
      const int64_t Displacement =
          static_cast<int64_t>(ResolverImageBase) -
          static_cast<int64_t>(NextInstruction);
      Dispatch.insert(Dispatch.end(), {REX, 0x8d, ModRM});
      AppendU32(static_cast<uint32_t>(Displacement));
    };

    Dispatch.insert(Dispatch.end(), {0x89, 0xc9,       // mov ecx, ecx
                                     0x83, 0xf9, 0x05, // cmp ecx, 5
                                     0x0f, 0x87});     // ja default
    const size_t GuardNext = Dispatch.size() + sizeof(uint32_t);
    AppendU32(static_cast<uint32_t>(0x40 - GuardNext));
    AppendImageBaseLEA(0x48, 0x15); // lea rdx, image base
    if (SegmentPrefix != 0)
      Dispatch.push_back(SegmentPrefix);
    Dispatch.insert(Dispatch.end(),
                    {0x8b, 0x8c, 0x8a, 0x00, 0x11, 0x00,
                     0x00}); // mov ecx,[rdx+rcx*4+1100h]
    AppendImageBaseLEA(0x4c, 0x05); // lea r8, image base
    Dispatch.insert(Dispatch.end(), {0x49, 0x03, 0xc8, // add rcx, r8
                                     0xff, 0xe1});     // jmp rcx
    EXPECT_LE(Dispatch.size(), size_t{0x30});
    std::copy(Dispatch.begin(), Dispatch.end(), Text.Data.begin());

    const std::array<uint32_t, 6> TargetRVAs{
        0x1030, 0x1032, 0x1034, 0x1036, 0x1038, 0x103a};
    for (size_t I = 0; I < TargetRVAs.size(); ++I) {
      Text.Data[0x30 + I * 2] = 0xc3;
      for (unsigned Byte = 0; Byte < sizeof(uint32_t); ++Byte)
        Text.Data[(ResolverTableVA - ResolverFunctionVA) +
                  I * sizeof(uint32_t) + Byte] =
            static_cast<uint8_t>(TargetRVAs[I] >> (Byte * 8));
    }
    Text.Data[0x40] = 0xc3;
    Image.Segments.push_back(std::move(Text));

    Section TextSection;
    TextSection.Name = ".text";
    TextSection.VA = ResolverFunctionVA;
    TextSection.Size = (ResolverTableVA - ResolverFunctionVA) +
                       6 * sizeof(uint32_t);
    TextSection.Flags =
        SegmentFlags::Readable | SegmentFlags::Executable;
    Image.Sections.push_back(std::move(TextSection));
    Symbol Function = Symbol::makeFunc(ResolverFunctionVA, 0x41);
    Function.Name = "segment_table_publication";
    Image.Symbols.push_back(std::move(Function));
    Image.KnownCodeRanges.emplace_back(ResolverFunctionVA,
                                       ResolverFunctionVA + 0x41);
    return Image;
  };
  auto ResolveTable = [&](uint8_t SegmentPrefix) {
    BinaryImage Image = MakeResolverImage(SegmentPrefix);
    Decoder TableDecoder;
    EXPECT_TRUE(TableDecoder.init(Image.Arch, Image.Mode));
    CFGBuilder TableBuilder;
    const std::set<va_t> FunctionEntries{ResolverFunctionVA};
    TableBuilder.setKnownFuncEntries(&FunctionEntries);
    return TableBuilder.build(Image, TableDecoder, ResolverFunctionVA,
                              "segment_table_publication");
  };
  const LowFunc DefaultResolvedTable = ResolveTable(/*SegmentPrefix=*/0);
  ASSERT_EQ(DefaultResolvedTable.JumpTables.size(), 1u);
  EXPECT_EQ(DefaultResolvedTable.JumpTables.front().Targets.size(), 6u);
  for (uint8_t SegmentPrefix : {uint8_t{0x64}, uint8_t{0x65}}) {
    SCOPED_TRACE(SegmentPrefix == 0x64 ? "FS table load" : "GS table load");
    const LowFunc SegmentedResolvedTable = ResolveTable(SegmentPrefix);
    EXPECT_TRUE(SegmentedResolvedTable.JumpTables.empty());
    EXPECT_TRUE(std::any_of(
        SegmentedResolvedTable.Blocks.begin(),
        SegmentedResolvedTable.Blocks.end(), [&](const LowBlock &Block) {
          return std::any_of(Block.Ops.begin(), Block.Ops.end(),
                             [&](const LowOp &Op) {
                               return Op.Opcode == NdOp::LOAD &&
                                      Op.MemoryAddressSpace ==
                                          (SegmentPrefix == 0x64
                                               ? NdMemoryAddressSpace::X86FS
                                               : NdMemoryAddressSpace::X86GS);
                             });
        }));
  }

  const std::vector<LowOp> FSMovs = LiftStringInstruction({0x64, 0xa4});
  EXPECT_EQ(CountOps(FSMovs, NdOp::LOAD, NdMemoryAddressSpace::X86FS), 1);
  EXPECT_EQ(CountOps(FSMovs, NdOp::STORE, NdMemoryAddressSpace::Default), 1)
      << "MOVS destination remains fixed ES/default";

  const std::vector<LowOp> GSRepMovs =
      LiftStringInstruction({0xf3, 0x65, 0xa4});
  auto RepMovs = FindIntrinsic(GSRepMovs, Intrinsic::Movsb);
  ASSERT_NE(RepMovs, GSRepMovs.end());
  EXPECT_EQ(RepMovs->MemoryAddressSpace, NdMemoryAddressSpace::X86GS);

  const std::vector<LowOp> FSLods = LiftStringInstruction({0x64, 0xac});
  EXPECT_EQ(CountOps(FSLods, NdOp::LOAD, NdMemoryAddressSpace::X86FS), 1);

  const std::vector<LowOp> GSRepLods =
      LiftStringInstruction({0xf3, 0x65, 0xac});
  auto RepLods = FindIntrinsic(GSRepLods, Intrinsic::Lodsb);
  ASSERT_NE(RepLods, GSRepLods.end());
  EXPECT_EQ(RepLods->MemoryAddressSpace, NdMemoryAddressSpace::X86GS);
  EXPECT_EQ(CountOps(GSRepLods, NdOp::LOAD, NdMemoryAddressSpace::Default), 0);
  EXPECT_EQ(CountOps(GSRepLods, NdOp::LOAD, NdMemoryAddressSpace::X86GS), 0)
      << "REP LODS must not speculate a load when RCX is zero";

  Decoder X86StringDecoder;
  ASSERT_TRUE(X86StringDecoder.init(Arch::X86));
  const std::array<uint8_t, 2> X86RepLodsBytes{0xf3, 0xac};
  DecodedInsn X86RepLodsInsn{};
  ASSERT_EQ(X86StringDecoder.decodeOneForLift(
                X86RepLodsBytes.data(), X86RepLodsBytes.size(), kEntry,
                X86RepLodsInsn),
            static_cast<int>(X86RepLodsBytes.size()));
  std::vector<LowOp> X86RepLods;
  X86StringDecoder.liftToLow(X86RepLodsInsn, X86RepLods);
  auto X86RepLodsOp = FindIntrinsic(X86RepLods, Intrinsic::Lodsb);
  ASSERT_NE(X86RepLodsOp, X86RepLods.end());
  ASSERT_GE(X86RepLodsOp->NumInputs, 4u);
  EXPECT_EQ(X86RepLodsOp->Output.Size, 4u);
  EXPECT_EQ(X86RepLodsOp->Inputs[3].Size, 4u)
      << "i386 REP LODS must carry EAX, not an eight-byte accumulator";

  const std::vector<LowOp> FSCmps = LiftStringInstruction({0x64, 0xa6});
  EXPECT_EQ(CountOps(FSCmps, NdOp::LOAD, NdMemoryAddressSpace::X86FS), 1);
  EXPECT_EQ(CountOps(FSCmps, NdOp::LOAD, NdMemoryAddressSpace::Default), 1)
      << "CMPS destination remains fixed ES/default";

  const std::vector<LowOp> GSRepCmps =
      LiftStringInstruction({0xf3, 0x65, 0xa6});
  auto RepCmps = FindIntrinsic(GSRepCmps, Intrinsic::Cmpsb);
  ASSERT_NE(RepCmps, GSRepCmps.end());
  EXPECT_EQ(RepCmps->MemoryAddressSpace, NdMemoryAddressSpace::X86GS);
  EXPECT_EQ(
      std::count_if(GSRepCmps.begin(), GSRepCmps.end(),
                    [](const LowOp &Op) { return Op.Opcode == NdOp::LOAD; }),
      0)
      << "zero-count REP CMPS must have no unconditional reconstruction load";

  const std::vector<LowOp> GSXlat = LiftStringInstruction({0x65, 0xd7});
  EXPECT_EQ(CountOps(GSXlat, NdOp::LOAD, NdMemoryAddressSpace::X86GS), 1);

  // The REP instructions execute in hardware, so the segment override must be
  // present in LLVM inline asm rather than represented only as metadata.
  LowFunc StringLow = buildFunction(Arch::X64, InstructionMode::Default,
                                    {0xf3, 0x64, 0xa4, 0xf3, 0x65, 0xac, 0xf3,
                                     0x65, 0xa6, 0x0f, 0x94, 0xc0, 0xc3});
  MedFunc StringMed =
      LowToMedConverter().convert(StringLow, Arch::X64, BinaryFormat::ELF);
  llvm::LLVMContext StringContext;
  auto StringModule = MedLLVMEmitter().emit({StringMed}, StringContext,
                                            "segment-strings", Arch::X64);
  ASSERT_NE(StringModule, nullptr);
  EXPECT_TRUE(validLLVMModule(*StringModule));
  bool SawFSMovs = false;
  bool SawGSLods = false;
  bool SawGSCmpsWithFlags = false;
  for (const llvm::Function &Function : *StringModule)
    for (const llvm::BasicBlock &Block : Function)
      for (const llvm::Instruction &Instruction : Block) {
        const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction);
        if (!Call)
          continue;
        const auto *Asm =
            llvm::dyn_cast<llvm::InlineAsm>(Call->getCalledOperand());
        if (!Asm)
          continue;
        llvm::StringRef Text = Asm->getAsmString();
        SawFSMovs |= Text.contains("fs rep movsb");
        SawGSLods |= Text.contains("gs rep lodsb");
        SawGSCmpsWithFlags |= Text.contains("gs repz cmpsb") &&
                              Text.contains("lahf") &&
                              Text.contains("seto %al");
      }
  EXPECT_TRUE(SawFSMovs);
  EXPECT_TRUE(SawGSLods);
  EXPECT_TRUE(SawGSCmpsWithFlags);

  // Address-size overrides select ESI/EDI/ECX (or SI/DI/CX on i386), not the
  // full native registers.  This is observable even when RCX's low 32 bits
  // are zero: addr32 REP must perform no access regardless of RCX's high half.
  // OUTS is another implicit string source and follows the same FS/GS policy.
  LowFunc Addr32StringLow =
      buildFunction(Arch::X64, InstructionMode::Default,
                    {0x67, 0xf3, 0x65, 0xa6, 0x67, 0xf3, 0x64, 0x6e, 0xc3});
  const LowOp *Addr32Cmps = nullptr;
  const LowOp *Addr32Outs = nullptr;
  for (const LowOp &Op : Addr32StringLow.Blocks.front().Ops) {
    if (Op.Opcode != NdOp::INTRINSIC || Op.NumInputs == 0 ||
        !Op.Inputs[0].isConst())
      continue;
    const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
    if (Id == Intrinsic::Cmpsb)
      Addr32Cmps = &Op;
    if (Id == Intrinsic::Outsb)
      Addr32Outs = &Op;
  }
  ASSERT_NE(Addr32Cmps, nullptr);
  ASSERT_GE(Addr32Cmps->NumInputs, 4u);
  EXPECT_EQ(Addr32Cmps->Output.Size, 4u);
  EXPECT_EQ(Addr32Cmps->Inputs[1].Size, 4u);
  EXPECT_EQ(Addr32Cmps->Inputs[2].Size, 4u);
  EXPECT_EQ(Addr32Cmps->Inputs[3].Size, 4u);
  EXPECT_EQ(Addr32Cmps->MemoryAddressSpace, NdMemoryAddressSpace::X86GS);
  ASSERT_NE(Addr32Outs, nullptr);
  ASSERT_GE(Addr32Outs->NumInputs, 5u);
  EXPECT_EQ(Addr32Outs->Output.Size, 0u);
  EXPECT_EQ(Addr32Outs->Inputs[1].Size, 4u);
  EXPECT_EQ(Addr32Outs->Inputs[2].Size, 4u);
  EXPECT_EQ(Addr32Outs->MemoryAddressSpace, NdMemoryAddressSpace::X86FS);

  MedFunc Addr32StringMed = LowToMedConverter().convert(
      Addr32StringLow, Arch::X64, BinaryFormat::ELF);
  llvm::LLVMContext Addr32StringContext;
  auto Addr32StringModule =
      MedLLVMEmitter().emit({Addr32StringMed}, Addr32StringContext,
                            "segment-addr32-strings", Arch::X64);
  ASSERT_NE(Addr32StringModule, nullptr);
  EXPECT_TRUE(validLLVMModule(*Addr32StringModule));
  bool SawAddr32GSCmps = false;
  bool SawAddr32FSOuts = false;
  for (const llvm::Function &Function : *Addr32StringModule)
    for (const llvm::BasicBlock &Block : Function)
      for (const llvm::Instruction &Instruction : Block) {
        const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction);
        if (!Call)
          continue;
        const auto *Asm =
            llvm::dyn_cast<llvm::InlineAsm>(Call->getCalledOperand());
        if (!Asm)
          continue;
        SawAddr32GSCmps |= Asm->getAsmString().contains("addr32 gs repz cmpsb");
        SawAddr32FSOuts |= Asm->getAsmString().contains("addr32 fs rep outsb");
      }
  EXPECT_TRUE(SawAddr32GSCmps);
  EXPECT_TRUE(SawAddr32FSOuts);

  // Med -> High must preserve the address space on both ordinary memory and
  // memory-effect expressions.  High-C renders scalar accesses through
  // target address-space-qualified helpers instead of flattening them into
  // ordinary process pointers.
  HighFunc High = MedToHighConverter().convert(Med, Arch::X64);
  unsigned HighFSLoads = 0;
  unsigned HighGSLoads = 0;
  unsigned HighGSStores = 0;
  bool SawFSQualifierInHighIR = false;
  std::set<const HighExpr *> SeenHighExprs;
  std::function<void(const ExprPtr &)> VisitHighExpr =
      [&](const ExprPtr &Expr) {
        if (!Expr || !SeenHighExprs.insert(Expr.get()).second)
          return;
        if (Expr->Kind == ExprKind::Load &&
            Expr->MemoryAddressSpace == NdMemoryAddressSpace::X86FS) {
          ++HighFSLoads;
          SawFSQualifierInHighIR |= Expr->str().starts_with("fs:*");
        }
        if (Expr->Kind == ExprKind::Load &&
            Expr->MemoryAddressSpace == NdMemoryAddressSpace::X86GS)
          ++HighGSLoads;
        for (const ExprPtr &Operand : Expr->Operands)
          VisitHighExpr(Operand);
      };
  walkStmts(High.Body, [&](const HighStmt &Stmt) {
    if (Stmt.Kind == StmtKind::Store &&
        Stmt.MemoryAddressSpace == NdMemoryAddressSpace::X86GS)
      ++HighGSStores;
    forEachExpr(Stmt, VisitHighExpr);
  });
  EXPECT_GE(HighFSLoads, 1u);
  EXPECT_GE(HighGSLoads, 1u);
  EXPECT_GE(HighGSStores, 1u);
  EXPECT_TRUE(SawFSQualifierInHighIR);

  std::string HighC;
  llvm::raw_string_ostream HighCOS(HighC);
  CEmitterOptions HighCOptions;
  HighCOptions.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit({High}, HighCOS, HighCOptions));
  HighCOS.flush();
  EXPECT_NE(HighC.find("neverd_mem_load_fs_"), std::string::npos);
  EXPECT_NE(HighC.find("neverd_mem_load_gs_"), std::string::npos);
  EXPECT_NE(HighC.find("neverd_mem_store_gs_"), std::string::npos);
  EXPECT_NE(HighC.find("address_space(257)"), std::string::npos);
  EXPECT_NE(HighC.find("address_space(256)"), std::string::npos);

  HighFunc StringHigh = MedToHighConverter().convert(StringMed, Arch::X64);
  bool SawHighSegmentedString = false;
  bool SawHighCmpsFlags = false;
  walkStmts(StringHigh.Body, [&](const HighStmt &Stmt) {
    forEachExpr(Stmt, [&](const ExprPtr &Expr) {
      if (Expr && Expr->Kind == ExprKind::Call &&
          Expr->MemoryAddressSpace != NdMemoryAddressSpace::Default)
        SawHighSegmentedString = true;
      if (Expr && Expr->IntrinsicId == Intrinsic::Cmpsb &&
          Expr->MemoryAddressSpace == NdMemoryAddressSpace::X86GS)
        SawHighCmpsFlags = Expr->IntrinsicOutputs.size() == 1;
    });
  });
  EXPECT_TRUE(SawHighSegmentedString);
  EXPECT_TRUE(SawHighCmpsFlags);

  std::string StringHighC;
  llvm::raw_string_ostream StringHighCOS(StringHighC);
  ASSERT_TRUE(HighCEmitter().emit({StringHigh}, StringHighCOS, HighCOptions));
  StringHighCOS.flush();
  EXPECT_NE(StringHighC.find("__asm__ volatile"), std::string::npos);
  EXPECT_NE(StringHighC.find("fs rep movsb"), std::string::npos);
  EXPECT_NE(StringHighC.find("gs rep lodsb"), std::string::npos);
  EXPECT_NE(StringHighC.find("gs repz cmpsb"), std::string::npos);
  EXPECT_NE(StringHighC.find("lahf"), std::string::npos);
  EXPECT_NE(StringHighC.find("seto %%al"), std::string::npos);
  EXPECT_EQ(StringHighC.find("__movsb("), std::string::npos);
  EXPECT_TRUE(validHighC(StringHighC));

  // Keep one reconstructed flag observably live so the High-C renderer must
  // bind CMPS's auxiliary LAHF/SETO output, rather than legitimately deleting
  // it after a zero-count/unused-flags sequence.
  LowFunc LiveFlagsLow = buildFunction(
      Arch::X64, InstructionMode::Default,
      {0xf3, 0x65, 0xa6,             // repe gs:cmpsb
       0x0f, 0x94, 0xc0,             // sete al
       0x0f, 0xb6, 0xc0,             // movzx eax, al
       0xc3});
  MedFunc LiveFlagsMed =
      LowToMedConverter().convert(LiveFlagsLow, Arch::X64, BinaryFormat::ELF);
  HighFunc LiveFlagsHigh =
      MedToHighConverter().convert(LiveFlagsMed, Arch::X64);
  std::optional<VarKey> LiveFlagsOutputKey;
  unsigned LiveFlagsOutputCount = 0;
  walkStmts(LiveFlagsHigh.Body, [&](const HighStmt &Stmt) {
    forEachExpr(Stmt, [&](const ExprPtr &Expr) {
      if (!Expr || Expr->Kind != ExprKind::Call ||
          Expr->IntrinsicId != Intrinsic::Cmpsb)
        return;
      ASSERT_EQ(Expr->IntrinsicOutputs.size(), 1u);
      if (Expr->IntrinsicOutputs.size() == 1) {
        LiveFlagsOutputKey = varKey(Expr->IntrinsicOutputs.front());
        ++LiveFlagsOutputCount;
      }
    });
  });
  ASSERT_EQ(LiveFlagsOutputCount, 1u);
  ASSERT_TRUE(LiveFlagsOutputKey.has_value());
  bool LiveFlagsReturnUsesOutput = false;
  std::set<const HighExpr *> SeenLiveFlagsExprs;
  std::function<void(const HighExpr &)> FindLiveFlagsOutput =
      [&](const HighExpr &Expr) {
        if (!SeenLiveFlagsExprs.insert(&Expr).second)
          return;
        if (Expr.Kind == ExprKind::Var &&
            varKey(Expr.Var) == *LiveFlagsOutputKey)
          LiveFlagsReturnUsesOutput = true;
        for (const ExprPtr &Operand : Expr.Operands)
          if (Operand)
            FindLiveFlagsOutput(*Operand);
      };
  walkStmts(LiveFlagsHigh.Body, [&](const HighStmt &Stmt) {
    if (Stmt.Kind == StmtKind::Return && Stmt.RetVal)
      FindLiveFlagsOutput(*Stmt.RetVal);
  });
  EXPECT_TRUE(LiveFlagsReturnUsesOutput);
  std::string LiveFlagsHighC;
  llvm::raw_string_ostream LiveFlagsHighCOS(LiveFlagsHighC);
  ASSERT_TRUE(
      HighCEmitter().emit({LiveFlagsHigh}, LiveFlagsHighCOS, HighCOptions));
  LiveFlagsHighCOS.flush();
  EXPECT_NE(LiveFlagsHighC.find("gs repz cmpsb"), std::string::npos);
  EXPECT_NE(LiveFlagsHighC.find("= (uint16_t)neverd_flags"), std::string::npos)
      << LiveFlagsHighC;

  HighFunc Addr32StringHigh =
      MedToHighConverter().convert(Addr32StringMed, Arch::X64);
  std::string Addr32StringHighC;
  llvm::raw_string_ostream Addr32StringHighCOS(Addr32StringHighC);
  ASSERT_TRUE(HighCEmitter().emit({Addr32StringHigh}, Addr32StringHighCOS,
                                  HighCOptions));
  Addr32StringHighCOS.flush();
  EXPECT_NE(Addr32StringHighC.find("addr32 gs repz cmpsb"), std::string::npos);
  EXPECT_NE(Addr32StringHighC.find("addr32 fs rep outsb"), std::string::npos);
  EXPECT_TRUE(validHighC(Addr32StringHighC));

  // The same statement renderer owns the default-address-space REP families.
  // This keeps DF, address-size and auxiliary flags explicit instead of
  // falling back to C intrinsics with incompatible calling conventions.
  LowFunc DefaultStringLow = buildFunction(
      Arch::X64, InstructionMode::Default,
      {0xfd,                         // std
       0xf3, 0xa4,                   // rep movsb
       0xf3, 0xaa,                   // rep stosb
       0xf3, 0xac,                   // rep lodsb
       0xf3, 0xa6, 0x31, 0xc0,       // unused-flags rep cmpsb; xor eax,eax
       0xf3, 0xa6, 0x0f, 0x94, 0xc0, // live-flags rep cmpsb; sete al
       0xf2, 0xae, 0x0f, 0x94, 0xc3, // live-flags repne scasb; sete bl
       0xf3, 0x6e,                   // rep outsb
       0xfc,                         // cld
       0x67, 0xf3, 0x6c,             // addr32 rep insb
       0x67, 0xf3, 0x6d,             // addr32 rep insd
       0xc3});

  // Default-address-space memory intrinsics obey the same operand contract as
  // FS/GS forms.  Incomplete REP state is rejected at both public IR layers;
  // it must never become a bare host REP instruction using ambient registers.
  LowFunc MalformedDefaultStringLow = DefaultStringLow;
  bool TruncatedDefaultMovs = false;
  for (LowOp &Op : MalformedDefaultStringLow.Blocks.front().Ops)
    if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs > 0 &&
        Op.Inputs[0].isConst() &&
        Op.Inputs[0].Offset == static_cast<uint64_t>(Intrinsic::Movsb)) {
      Op.NumInputs = 4;
      TruncatedDefaultMovs = true;
      break;
    }
  ASSERT_TRUE(TruncatedDefaultMovs);
  const std::string MalformedDefaultLowError = llvm::toString(
      validateLowInstructionBoundaries(MalformedDefaultStringLow));
  EXPECT_NE(MalformedDefaultLowError.find("invalid operand/output shape"),
            std::string::npos);

  auto TruncatedDefaultStringError = [&](Intrinsic Id,
                                         uint8_t TruncatedInputs) {
    LowFunc BadShape = DefaultStringLow;
    bool Truncated = false;
    for (LowOp &Op : BadShape.Blocks.front().Ops)
      if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs > 0 &&
          Op.Inputs[0].isConst() &&
          Op.Inputs[0].Offset == static_cast<uint64_t>(Id)) {
        Op.NumInputs = TruncatedInputs;
        Truncated = true;
        break;
      }
    EXPECT_TRUE(Truncated);
    return Truncated
               ? llvm::toString(validateLowInstructionBoundaries(BadShape))
               : std::string{};
  };
  const std::array<std::pair<Intrinsic, uint8_t>, 3> FixedSegmentStrings{{
      {Intrinsic::Stosb, 4},
      {Intrinsic::Scasb, 5},
      {Intrinsic::Insb, 4},
  }};
  for (const auto &[Id, TruncatedInputs] : FixedSegmentStrings)
    EXPECT_NE(TruncatedDefaultStringError(Id, TruncatedInputs)
                  .find("invalid operand/output shape"),
              std::string::npos);

  MedFunc DefaultStringMed = LowToMedConverter().convert(
      DefaultStringLow, Arch::X64, BinaryFormat::ELF);
  MedFunc MalformedDefaultStringMed = DefaultStringMed;
  bool TruncatedDefaultMedMovs = false;
  for (MedOp &Op : MalformedDefaultStringMed.Blocks.front().Ops)
    if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs > 0 &&
        Op.Inputs[0].isConst() &&
        Op.Inputs[0].ConstVal == static_cast<uint64_t>(Intrinsic::Movsb)) {
      Op.NumInputs = 4;
      TruncatedDefaultMedMovs = true;
      break;
    }
  ASSERT_TRUE(TruncatedDefaultMedMovs);
  EXPECT_FALSE(verifyMedFunc(MalformedDefaultStringMed,
                             "malformed-default-rep"));

  llvm::LLVMContext DefaultStringContext;
  auto DefaultStringModule = MedLLVMEmitter().emit(
      {DefaultStringMed}, DefaultStringContext, "default-string-semantics",
      Arch::X64);
  ASSERT_NE(DefaultStringModule, nullptr);
  EXPECT_TRUE(validLLVMModule(*DefaultStringModule));
  bool SawDefaultMovs = false;
  bool SawDefaultStos = false;
  bool SawDefaultLods = false;
  bool SawDefaultCmps = false;
  bool SawDefaultScas = false;
  bool SawDefaultOuts = false;
  bool SawAddr32Insb = false;
  bool SawAddr32Insd = false;
  for (const llvm::Function &Function : *DefaultStringModule)
    for (const llvm::BasicBlock &Block : Function)
      for (const llvm::Instruction &Instruction : Block) {
        const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction);
        const auto *Asm =
            Call ? llvm::dyn_cast<llvm::InlineAsm>(Call->getCalledOperand())
                 : nullptr;
        if (!Asm)
          continue;
        const llvm::StringRef Text = Asm->getAsmString();
        SawDefaultMovs |= Text.contains("rep movsb");
        SawDefaultStos |= Text.contains("rep stosb");
        SawDefaultLods |= Text.contains("rep lodsb");
        SawDefaultCmps |= Text.contains("repz cmpsb") &&
                          Text.contains("lahf");
        SawDefaultScas |= Text.contains("repnz scasb") &&
                          Text.contains("lahf");
        SawDefaultOuts |= Text.contains("rep outsb");
        SawAddr32Insb |= Text.contains("addr32 rep insb");
        SawAddr32Insd |= Text.contains("addr32 rep insd");
      }
  EXPECT_TRUE(SawDefaultMovs);
  EXPECT_TRUE(SawDefaultStos);
  EXPECT_TRUE(SawDefaultLods);
  EXPECT_TRUE(SawDefaultCmps);
  EXPECT_TRUE(SawDefaultScas);
  EXPECT_TRUE(SawDefaultOuts);
  EXPECT_TRUE(SawAddr32Insb);
  EXPECT_TRUE(SawAddr32Insd);

  HighFunc DefaultStringHigh =
      MedToHighConverter().convert(DefaultStringMed, Arch::X64);
  std::string DefaultStringHighC;
  llvm::raw_string_ostream DefaultStringHighCOS(DefaultStringHighC);
  ASSERT_TRUE(HighCEmitter().emit({DefaultStringHigh}, DefaultStringHighCOS,
                                  HighCOptions));
  DefaultStringHighCOS.flush();
  EXPECT_NE(DefaultStringHighC.find("rep movsb"), std::string::npos);
  EXPECT_NE(DefaultStringHighC.find("rep stosb"), std::string::npos);
  EXPECT_NE(DefaultStringHighC.find("rep lodsb"), std::string::npos);
  EXPECT_NE(DefaultStringHighC.find("repz cmpsb"), std::string::npos);
  EXPECT_NE(DefaultStringHighC.find("repnz scasb"), std::string::npos);
  EXPECT_NE(DefaultStringHighC.find("rep outsb"), std::string::npos);
  EXPECT_NE(DefaultStringHighC.find("addr32 rep insb"), std::string::npos);
  EXPECT_NE(DefaultStringHighC.find("addr32 rep insl"), std::string::npos);
  EXPECT_EQ(DefaultStringHighC.find("%%:("), std::string::npos);
  EXPECT_TRUE(validHighC(DefaultStringHighC));

  // Cache and architectural-state instructions own a real memory operand.
  // Their effective-address offset is input 1, FS/GS stays on the intrinsic,
  // and side-effect-only forms never manufacture an integer result.
  auto CheckMemoryIntrinsic =
      [&](std::initializer_list<uint8_t> Bytes, Intrinsic Id,
          NdMemoryAddressSpace AddressSpace) {
        std::vector<LowOp> Ops = LiftStringInstruction(Bytes);
        auto It = FindIntrinsic(Ops, Id);
        EXPECT_NE(It, Ops.end());
        if (It != Ops.end()) {
          EXPECT_EQ(It->MemoryAddressSpace, AddressSpace);
          EXPECT_EQ(It->Output.Size, 0u);
          EXPECT_GE(It->NumInputs, 2u);
          if (It->NumInputs >= 2)
            EXPECT_EQ(It->Inputs[1].Size, 8u);
        }
        return Ops;
      };
  const std::vector<LowOp> FSLdmxcsr = CheckMemoryIntrinsic(
      {0x64, 0x0f, 0xae, 0x50, 0x20}, Intrinsic::Ldmxcsr,
      NdMemoryAddressSpace::X86FS);
  const std::vector<LowOp> GSStmxcsr = CheckMemoryIntrinsic(
      {0x65, 0x0f, 0xae, 0x58, 0x24}, Intrinsic::Stmxcsr,
      NdMemoryAddressSpace::X86GS);
  const std::vector<LowOp> GSLdmxcsr = CheckMemoryIntrinsic(
      {0x65, 0x0f, 0xae, 0x50, 0x24}, Intrinsic::Ldmxcsr,
      NdMemoryAddressSpace::X86GS);
  const std::vector<LowOp> FSClflush = CheckMemoryIntrinsic(
      {0x64, 0x0f, 0xae, 0x78, 0x28}, Intrinsic::Clflush,
      NdMemoryAddressSpace::X86FS);
  CheckMemoryIntrinsic({0x65, 0x66, 0x0f, 0xae, 0x78, 0x30},
                       Intrinsic::Clflushopt,
                       NdMemoryAddressSpace::X86GS);
  CheckMemoryIntrinsic({0x64, 0x66, 0x0f, 0xae, 0x70, 0x38},
                       Intrinsic::Clwb, NdMemoryAddressSpace::X86FS);
  CheckMemoryIntrinsic({0x64, 0x0f, 0x18, 0x48, 0x40},
                       Intrinsic::PrefetchT0,
                       NdMemoryAddressSpace::X86FS);
  const std::vector<LowOp> GSPrefetchW = CheckMemoryIntrinsic(
      {0x65, 0x0f, 0x0d, 0x48, 0x48}, Intrinsic::PrefetchW,
      NdMemoryAddressSpace::X86GS);
  CheckMemoryIntrinsic({0x64, 0x0f, 0x0d, 0x50, 0x50},
                       Intrinsic::PrefetchWT1,
                       NdMemoryAddressSpace::X86FS);
  CheckMemoryIntrinsic({0x64, 0x0f, 0xae, 0x40, 0x58}, Intrinsic::Fxsave,
                       NdMemoryAddressSpace::X86FS);
  CheckMemoryIntrinsic({0x65, 0x0f, 0xae, 0x48, 0x60}, Intrinsic::Fxrstor,
                       NdMemoryAddressSpace::X86GS);
  CheckMemoryIntrinsic({0x64, 0xd9, 0x70, 0x68}, Intrinsic::X87Fnstenv,
                       NdMemoryAddressSpace::X86FS);
  CheckMemoryIntrinsic({0x65, 0xd9, 0x60, 0x70}, Intrinsic::X87Fldenv,
                       NdMemoryAddressSpace::X86GS);
  CheckMemoryIntrinsic({0x64, 0xdd, 0x70, 0x78}, Intrinsic::X87Fnsave,
                       NdMemoryAddressSpace::X86FS);
  CheckMemoryIntrinsic({0x65, 0xdd, 0x60, 0x7c}, Intrinsic::X87Frstor,
                       NdMemoryAddressSpace::X86GS);
  const std::vector<LowOp> FSXsave = CheckMemoryIntrinsic(
      {0x64, 0x0f, 0xae, 0x60, 0x20}, Intrinsic::Xsave,
      NdMemoryAddressSpace::X86FS);
  auto FSXsaveOp = FindIntrinsic(FSXsave, Intrinsic::Xsave);
  ASSERT_NE(FSXsaveOp, FSXsave.end());
  ASSERT_GE(FSXsaveOp->NumInputs, 4u);
  EXPECT_EQ(FSXsaveOp->Inputs[2].Size, 4u);
  EXPECT_EQ(FSXsaveOp->Inputs[3].Size, 4u);
  CheckMemoryIntrinsic({0x65, 0x0f, 0xae, 0x68, 0x28}, Intrinsic::Xrstor,
                       NdMemoryAddressSpace::X86GS);

  // State snapshots need an explicit bridge between the lifted XMM/x87/MXCSR
  // variables and the architectural memory layout.  Until that representation
  // exists, every consumer fails closed instead of reading/writing incidental
  // host registers through raw inline asm.
  NdOpEmulator UnsupportedStateEmulator(SemanticImage);
  UnsupportedStateEmulator.setStrictMode(true);
  ASSERT_TRUE(UnsupportedStateEmulator.setMemoryAddressSpaceBase(
      NdMemoryAddressSpace::X86FS, FSBase));
  UnsupportedStateEmulator.setRegister(x86reg::RAX, 0);
  UnsupportedStateEmulator.setRegister(x86reg::RDX, 0);
  EXPECT_EQ(UnsupportedStateEmulator.run(FSXsave),
            static_cast<size_t>(std::distance(FSXsave.begin(), FSXsaveOp)));

  LowFunc UnsupportedStateLow = buildFunction(
      Arch::X64, InstructionMode::Default,
      {0x64, 0x0f, 0xae, 0x60, 0x20, 0xc3});
  MedFunc UnsupportedStateMed = LowToMedConverter().convert(
      UnsupportedStateLow, Arch::X64, BinaryFormat::ELF);
  ASSERT_TRUE(verifyMedFunc(UnsupportedStateMed, "state-snapshot-shape"));

  // The lightweight emulator models the exact observable state for MXCSR and
  // cache hints.  A prefetched unmapped offset is non-faulting; a cache flush
  // validates the resolved target; STMXCSR followed by LDMXCSR round-trips the
  // architectural value through target memory.
  BinaryImage StateSemanticImage = SemanticImage;
  auto StateFSSegment = std::find_if(
      StateSemanticImage.Segments.begin(), StateSemanticImage.Segments.end(),
      [&](const Segment &S) { return S.VA == FSBase; });
  ASSERT_NE(StateFSSegment, StateSemanticImage.Segments.end());
  const uint32_t LoadedMXCSR = 0x5f80;
  std::memcpy(StateFSSegment->Data.data() + 0x20, &LoadedMXCSR,
              sizeof(LoadedMXCSR));
  NdOpEmulator StateEmulator(StateSemanticImage);
  ASSERT_TRUE(StateEmulator.setMemoryAddressSpaceBase(
      NdMemoryAddressSpace::X86FS, FSBase));
  ASSERT_TRUE(StateEmulator.setMemoryAddressSpaceBase(
      NdMemoryAddressSpace::X86GS, GSBase));
  StateEmulator.setRegister(x86reg::RAX, 0);
  EXPECT_EQ(StateEmulator.run(FSLdmxcsr), FSLdmxcsr.size());
  EXPECT_EQ(StateEmulator.getMXCSR(), LoadedMXCSR);
  BinaryImage InvalidMXCSRImage = StateSemanticImage;
  auto InvalidMXCSRSegment = std::find_if(
      InvalidMXCSRImage.Segments.begin(), InvalidMXCSRImage.Segments.end(),
      [&](const Segment &S) { return S.VA == FSBase; });
  ASSERT_NE(InvalidMXCSRSegment, InvalidMXCSRImage.Segments.end());
  const uint32_t InvalidMXCSR = UINT32_C(0x00010000);
  std::memcpy(InvalidMXCSRSegment->Data.data() + 0x20, &InvalidMXCSR,
              sizeof(InvalidMXCSR));
  NdOpEmulator InvalidMXCSREmulator(InvalidMXCSRImage);
  ASSERT_TRUE(InvalidMXCSREmulator.setMemoryAddressSpaceBase(
      NdMemoryAddressSpace::X86FS, FSBase));
  InvalidMXCSREmulator.setRegister(x86reg::RAX, 0);
  InvalidMXCSREmulator.setMXCSR(0x1f80);
  EXPECT_LT(InvalidMXCSREmulator.run(FSLdmxcsr), FSLdmxcsr.size());
  EXPECT_EQ(InvalidMXCSREmulator.getMXCSR(), 0x1f80u);
  constexpr uint32_t StoredMXCSR = 0x1f40;
  StateEmulator.setMXCSR(StoredMXCSR);
  EXPECT_EQ(StateEmulator.run(GSStmxcsr), GSStmxcsr.size());
  StateEmulator.setMXCSR(0);
  EXPECT_EQ(StateEmulator.run(GSLdmxcsr), GSLdmxcsr.size());
  EXPECT_EQ(StateEmulator.getMXCSR(), StoredMXCSR);
  StateEmulator.setLoadCollect(true);
  EXPECT_EQ(StateEmulator.run(FSClflush), FSClflush.size());
  EXPECT_TRUE(StateEmulator.getLoadRecords().empty())
      << "cache maintenance must not masquerade as jump-table data evidence";
  const size_t CacheLoads = StateEmulator.getLoadRecords().size();
  StateEmulator.setRegister(x86reg::RAX, UINT64_C(0x100000));
  EXPECT_EQ(StateEmulator.run(GSPrefetchW), GSPrefetchW.size());
  EXPECT_EQ(StateEmulator.getLoadRecords().size(), CacheLoads)
      << "prefetch remains a non-faulting hint and performs no data read";

  LowFunc MemoryStateLow = buildFunction(
      Arch::X64, InstructionMode::Default,
      {0x64, 0x0f, 0xae, 0x50, 0x20,       // ldmxcsr fs:[rax+20h]
       0x65, 0x0f, 0xae, 0x58, 0x24,       // stmxcsr gs:[rax+24h]
       0x65, 0x66, 0x0f, 0xae, 0x78, 0x30, // clflushopt gs:[rax+30h]
       0x64, 0x0f, 0x0d, 0x48, 0x40,       // prefetchw fs:[rax+40h]
       0xc3});
  MedFunc MemoryStateMed = LowToMedConverter().convert(
      MemoryStateLow, Arch::X64, BinaryFormat::ELF);
  ASSERT_TRUE(verifyMedFunc(MemoryStateMed, "segment-memory-state"));
  llvm::LLVMContext MemoryStateContext;
  auto MemoryStateModule = MedLLVMEmitter().emit(
      {MemoryStateMed}, MemoryStateContext, "segment-memory-state", Arch::X64);
  ASSERT_NE(MemoryStateModule, nullptr);
  EXPECT_TRUE(validLLVMModule(*MemoryStateModule));
  bool SawLLVMFSLdmxcsr = false;
  bool SawLLVMGSStmxcsr = false;
  bool SawLLVMGSClflushopt = false;
  bool SawLLVMFSPrefetchW = false;
  for (const llvm::Function &Function : *MemoryStateModule)
    for (const llvm::BasicBlock &Block : Function)
      for (const llvm::Instruction &Instruction : Block) {
        const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction);
        const auto *Asm =
            Call ? llvm::dyn_cast<llvm::InlineAsm>(Call->getCalledOperand())
                 : nullptr;
        if (!Asm)
          continue;
        const llvm::StringRef Text = Asm->getAsmString();
        SawLLVMFSLdmxcsr |= Text.contains("ldmxcsr %fs:($0)");
        SawLLVMGSStmxcsr |= Text.contains("stmxcsr %gs:($0)");
        SawLLVMGSClflushopt |= Text.contains("clflushopt %gs:($0)");
        SawLLVMFSPrefetchW |= Text.contains("prefetchw %fs:($0)");
      }
  EXPECT_TRUE(SawLLVMFSLdmxcsr);
  EXPECT_TRUE(SawLLVMGSStmxcsr);
  EXPECT_TRUE(SawLLVMGSClflushopt);
  EXPECT_TRUE(SawLLVMFSPrefetchW);

  HighFunc MemoryStateHigh =
      MedToHighConverter().convert(MemoryStateMed, Arch::X64);
  std::string MemoryStateHighC;
  llvm::raw_string_ostream MemoryStateHighCOS(MemoryStateHighC);
  ASSERT_TRUE(HighCEmitter().emit({MemoryStateHigh}, MemoryStateHighCOS,
                                  HighCOptions));
  MemoryStateHighCOS.flush();
  EXPECT_NE(MemoryStateHighC.find("ldmxcsr %%fs:(%[address])"),
            std::string::npos);
  EXPECT_NE(MemoryStateHighC.find("stmxcsr %%gs:(%[address])"),
            std::string::npos);
  EXPECT_NE(MemoryStateHighC.find("clflushopt %%gs:(%[address])"),
            std::string::npos);
  EXPECT_NE(MemoryStateHighC.find("prefetchw %%fs:(%[address])"),
            std::string::npos);
  EXPECT_TRUE(validHighC(MemoryStateHighC));

  // System-register memory stores are observable even with no SSA result;
  // the converter's DCE must retain them.  Conversely LLDT's register form
  // only reads r/m16 and must not manufacture an RAX definition.
  const std::vector<LowOp> FSSldt = CheckMemoryIntrinsic(
      {0x64, 0x0f, 0x00, 0x00}, Intrinsic::Sldt,
      NdMemoryAddressSpace::X86FS);
  LowFunc FSSldtLow = buildFunction(
      Arch::X64, InstructionMode::Default,
      {0x64, 0x0f, 0x00, 0x00, 0xc3});
  MedFunc FSSldtMed = LowToMedConverter().convert(
      FSSldtLow, Arch::X64, BinaryFormat::ELF);
  EXPECT_TRUE(std::any_of(
      FSSldtMed.Blocks.begin(), FSSldtMed.Blocks.end(),
      [](const MedBlock &Block) {
        return std::any_of(Block.Ops.begin(), Block.Ops.end(),
                           [](const MedOp &Op) {
                             return Op.Opcode == NdOp::INTRINSIC &&
                                    Op.NumInputs >= 1 &&
                                    Op.Inputs[0].isConst() &&
                                    Op.Inputs[0].ConstVal ==
                                        static_cast<uint64_t>(Intrinsic::Sldt);
                           });
      }));
  const std::vector<LowOp> RegisterLldt =
      LiftStringInstruction({0x0f, 0x00, 0xd0});
  auto RegisterLldtOp = FindIntrinsic(RegisterLldt, Intrinsic::Lldt);
  ASSERT_NE(RegisterLldtOp, RegisterLldt.end());
  EXPECT_EQ(RegisterLldtOp->Output.Size, 0u);
  ASSERT_GE(RegisterLldtOp->NumInputs, 2u);
  EXPECT_EQ(RegisterLldtOp->Inputs[1].Size, 2u);

  LowFunc RegisterSystemLow = buildFunction(
      Arch::X64, InstructionMode::Default,
      {0x0f, 0x00, 0xd0,             // lldt ax
       0x48, 0x0f, 0x00, 0xc0,       // sldt rax
       0xc3});
  EXPECT_TRUE(
      llvm::toString(validateLowInstructionBoundaries(RegisterSystemLow))
          .empty());
  MedFunc RegisterSystemMed = LowToMedConverter().convert(
      RegisterSystemLow, Arch::X64, BinaryFormat::ELF);
  EXPECT_TRUE(verifyMedFunc(RegisterSystemMed, "register-system-forms"));

  LowFunc I386SldtLow = buildFunction(
      Arch::X86, InstructionMode::Default,
      {0x0f, 0x00, 0x00, 0xc3});
  MedFunc I386SldtMed = LowToMedConverter().convert(
      I386SldtLow, Arch::X86, BinaryFormat::ELF);
  HighFunc I386SldtHigh =
      MedToHighConverter().convert(I386SldtMed, Arch::X86);
  CEmitterOptions I386COptions;
  I386COptions.TheArch = Arch::X86;
  std::string I386SldtHighC;
  llvm::raw_string_ostream I386SldtHighCOS(I386SldtHighC);
  ASSERT_TRUE(
      HighCEmitter().emit({I386SldtHigh}, I386SldtHighCOS, I386COptions));
  I386SldtHighCOS.flush();
  EXPECT_NE(I386SldtHighC.find("sldt (%[address])"), std::string::npos);
  llvm::LLVMContext I386SldtContext;
  auto I386SldtModule = MedLLVMEmitter().emit(
      {I386SldtMed}, I386SldtContext, "i386-system-memory", Arch::X86);
  ASSERT_NE(I386SldtModule, nullptr);
  EXPECT_TRUE(validLLVMModule(*I386SldtModule));
  bool SawI386NativeAddressOperand = false;
  for (const llvm::Function &Function : *I386SldtModule)
    for (const llvm::BasicBlock &Block : Function)
      for (const llvm::Instruction &Instruction : Block) {
        const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction);
        const auto *Asm =
            Call ? llvm::dyn_cast<llvm::InlineAsm>(Call->getCalledOperand())
                 : nullptr;
        if (Asm && Asm->getAsmString().contains("sldt ($0)")) {
          ASSERT_GE(Call->arg_size(), 1u);
          SawI386NativeAddressOperand =
              Call->getArgOperand(0)->getType()->isIntegerTy(32);
        }
      }
  EXPECT_TRUE(SawI386NativeAddressOperand)
      << "i386 inline asm must receive one native-width address register";

  // Fault-suppressing masked memory operations cannot be flattened to the
  // ordinary intrinsics: their implicit memory operand must retain FS/GS in
  // the actual instruction emitted by High-C.
  auto MaskedTemp = [](int Id, uint16_t Size) {
    MedVar Var;
    Var.Kind = MedVar::Temp;
    Var.TheArch = Arch::X64;
    Var.Id = Id;
    Var.SSAVer = 1;
    Var.Size = Size;
    return Var;
  };
  MedFunc MaskedMed;
  MaskedMed.Entry = 0x1800;
  MaskedMed.Name = "segment_masked_memory_semantics";
  MaskedMed.ReturnType = NdType::makeVoid();
  MedBlock MaskedBlock;
  MaskedBlock.Id = 0;
  MaskedBlock.StartAddr = MaskedMed.Entry;
  MaskedBlock.EndAddr = MaskedMed.Entry + 12;
  MedOp MaskedLoad;
  MaskedLoad.Opcode = NdOp::INTRINSIC;
  MaskedLoad.Addr = MaskedMed.Entry;
  MaskedLoad.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
  MaskedLoad.Output = MaskedTemp(1, 16);
  MaskedLoad.addInput(
      MedVar::makeConst(static_cast<uint64_t>(Intrinsic::MaskedLoadD), 2,
                        ConstantAddressProvenance::Scalar));
  MaskedLoad.addInput(
      MedVar::makeConst(0x60, 8, ConstantAddressProvenance::Scalar));
  MaskedLoad.addInput(MedVar::makeConst(UINT64_C(0x8000000080000000), 16,
                                        ConstantAddressProvenance::Scalar));
  MaskedBlock.Ops.push_back(MaskedLoad);
  MedOp WideMaskedLoad = MaskedLoad;
  WideMaskedLoad.Addr = MaskedMed.Entry + 1;
  WideMaskedLoad.Output = MaskedTemp(2, 32);
  WideMaskedLoad.Inputs[1] =
      MedVar::makeConst(0x80, 8, ConstantAddressProvenance::Scalar);
  WideMaskedLoad.Inputs[2] = MedVar::makeConst(
      UINT64_C(0x8000000080000000), 32,
      ConstantAddressProvenance::Scalar);
  MaskedBlock.Ops.push_back(WideMaskedLoad);
  MedOp DefaultMaskedLoad = MaskedLoad;
  DefaultMaskedLoad.Addr = MaskedMed.Entry + 2;
  DefaultMaskedLoad.Output = MaskedTemp(3, 16);
  DefaultMaskedLoad.MemoryAddressSpace = NdMemoryAddressSpace::Default;
  DefaultMaskedLoad.Inputs[1] =
      MedVar::makeConst(0x88, 8, ConstantAddressProvenance::Scalar);
  MaskedBlock.Ops.push_back(DefaultMaskedLoad);
  MedOp MaskedStore;
  MaskedStore.Opcode = NdOp::INTRINSIC;
  MaskedStore.Addr = MaskedMed.Entry + 4;
  MaskedStore.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
  MaskedStore.addInput(
      MedVar::makeConst(static_cast<uint64_t>(Intrinsic::MaskedStoreQ), 2,
                        ConstantAddressProvenance::Scalar));
  MaskedStore.addInput(
      MedVar::makeConst(0x70, 8, ConstantAddressProvenance::Scalar));
  MaskedStore.addInput(MedVar::makeConst(UINT64_C(0x8000000000000000), 16,
                                         ConstantAddressProvenance::Scalar));
  MaskedStore.addInput(MaskedLoad.Output);
  MaskedBlock.Ops.push_back(MaskedStore);
  MedOp WideMaskedStore;
  WideMaskedStore.Opcode = NdOp::INTRINSIC;
  WideMaskedStore.Addr = MaskedMed.Entry + 5;
  WideMaskedStore.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
  WideMaskedStore.addInput(MedVar::makeConst(
      static_cast<uint64_t>(Intrinsic::MaskedStoreQ), 2,
      ConstantAddressProvenance::Scalar));
  WideMaskedStore.addInput(
      MedVar::makeConst(0x90, 8, ConstantAddressProvenance::Scalar));
  WideMaskedStore.addInput(MedVar::makeConst(
      UINT64_C(0x8000000000000000), 32,
      ConstantAddressProvenance::Scalar));
  WideMaskedStore.addInput(WideMaskedLoad.Output);
  MaskedBlock.Ops.push_back(WideMaskedStore);
  MedOp DefaultMaskedStore;
  DefaultMaskedStore.Opcode = NdOp::INTRINSIC;
  DefaultMaskedStore.Addr = MaskedMed.Entry + 5;
  DefaultMaskedStore.addInput(MedVar::makeConst(
      static_cast<uint64_t>(Intrinsic::MaskedStoreD), 2,
      ConstantAddressProvenance::Scalar));
  DefaultMaskedStore.addInput(
      MedVar::makeConst(0x98, 8, ConstantAddressProvenance::Scalar));
  DefaultMaskedStore.addInput(MedVar::makeConst(
      UINT64_C(0x8000000080000000), 16,
      ConstantAddressProvenance::Scalar));
  DefaultMaskedStore.addInput(DefaultMaskedLoad.Output);
  MaskedBlock.Ops.push_back(DefaultMaskedStore);
  MedOp ByteMaskedStore;
  ByteMaskedStore.Opcode = NdOp::INTRINSIC;
  ByteMaskedStore.Addr = MaskedMed.Entry + 6;
  ByteMaskedStore.addInput(MedVar::makeConst(
      static_cast<uint64_t>(Intrinsic::MaskedStoreB), 2,
      ConstantAddressProvenance::Scalar));
  ByteMaskedStore.addInput(
      MedVar::makeConst(0xa0, 8, ConstantAddressProvenance::Scalar));
  ByteMaskedStore.addInput(MedVar::makeConst(
      UINT64_C(0x8000000000000080), 8,
      ConstantAddressProvenance::Scalar));
  ByteMaskedStore.addInput(MedVar::makeConst(
      UINT64_C(0x8877665544332211), 8,
      ConstantAddressProvenance::Scalar));
  MaskedBlock.Ops.push_back(ByteMaskedStore);
  MedOp FSByteMaskedStore = ByteMaskedStore;
  FSByteMaskedStore.Addr = MaskedMed.Entry + 7;
  FSByteMaskedStore.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
  FSByteMaskedStore.Inputs[1] =
      MedVar::makeConst(0xb0, 8, ConstantAddressProvenance::Scalar);
  FSByteMaskedStore.Inputs[2] = MedVar::makeConst(
      UINT64_C(0x8000000000000080), 16,
      ConstantAddressProvenance::Scalar);
  FSByteMaskedStore.Inputs[3] = MedVar::makeConst(
      UINT64_C(0x8877665544332211), 16,
      ConstantAddressProvenance::Scalar);
  MaskedBlock.Ops.push_back(FSByteMaskedStore);
  MedOp MaskedReturn;
  MaskedReturn.Opcode = NdOp::RETURN;
  MaskedReturn.Addr = MaskedMed.Entry + 8;
  MaskedBlock.Ops.push_back(MaskedReturn);
  MaskedMed.Blocks.push_back(std::move(MaskedBlock));
  ASSERT_TRUE(verifyMedFunc(MaskedMed, "segment-masked-memory"));

  llvm::LLVMContext MaskedContext;
  auto MaskedModule = MedLLVMEmitter().emit({MaskedMed}, MaskedContext,
                                            "segment-masked-memory", Arch::X64);
  ASSERT_NE(MaskedModule, nullptr);
  EXPECT_TRUE(validLLVMModule(*MaskedModule));
  unsigned FSMaskedLoads = 0;
  unsigned GSMaskedStores = 0;
  unsigned WideMaskedLoads = 0;
  unsigned WideMaskedStores = 0;
  unsigned DefaultDwordMaskedLoads = 0;
  unsigned DefaultDwordMaskedStores = 0;
  unsigned DefaultByteMaskedStores = 0;
  unsigned FSByteMaskedStores = 0;
  for (const llvm::Function &Function : *MaskedModule)
    for (const llvm::BasicBlock &Block : Function)
      for (const llvm::Instruction &Instruction : Block) {
        const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction);
        if (!Call || !Call->getCalledFunction())
          continue;
        const llvm::StringRef Name = Call->getCalledFunction()->getName();
        if (Name.starts_with("llvm.masked.load")) {
          ASSERT_GE(Call->arg_size(), 1u);
          FSMaskedLoads +=
              Call->getArgOperand(0)->getType()->getPointerAddressSpace() ==
              257;
          WideMaskedLoads +=
              Call->getType()->getPrimitiveSizeInBits() == 256;
          auto *ResultVector =
              llvm::dyn_cast<llvm::FixedVectorType>(Call->getType());
          ASSERT_NE(ResultVector, nullptr);
          DefaultDwordMaskedLoads +=
              Call->getArgOperand(0)->getType()->getPointerAddressSpace() == 0 &&
              ResultVector->getElementType()->isIntegerTy(32);
        }
        if (Name.starts_with("llvm.masked.store")) {
          ASSERT_GE(Call->arg_size(), 2u);
          const unsigned AS =
              Call->getArgOperand(1)->getType()->getPointerAddressSpace();
          GSMaskedStores += AS == 256;
          auto *VectorTy = llvm::dyn_cast<llvm::FixedVectorType>(
              Call->getArgOperand(0)->getType());
          ASSERT_NE(VectorTy, nullptr);
          WideMaskedStores +=
              VectorTy->getPrimitiveSizeInBits() == 256;
          if (VectorTy->getElementType()->isIntegerTy(8)) {
            DefaultByteMaskedStores += AS == 0;
            FSByteMaskedStores += AS == 257;
          }
          DefaultDwordMaskedStores +=
              AS == 0 && VectorTy->getElementType()->isIntegerTy(32);
        }
      }
  EXPECT_EQ(FSMaskedLoads, 2u);
  EXPECT_EQ(GSMaskedStores, 2u)
      << "void segmented masked stores must not be dropped before SIMD "
         "dispatch";
  EXPECT_EQ(WideMaskedLoads, 1u);
  EXPECT_EQ(WideMaskedStores, 1u);
  EXPECT_EQ(DefaultDwordMaskedLoads, 1u);
  EXPECT_EQ(DefaultDwordMaskedStores, 1u);
  EXPECT_EQ(DefaultByteMaskedStores, 1u);
  EXPECT_EQ(FSByteMaskedStores, 1u);

  HighFunc MaskedHigh = MedToHighConverter().convert(MaskedMed, Arch::X64);
  std::string MaskedHighC;
  llvm::raw_string_ostream MaskedHighCOS(MaskedHighC);
  ASSERT_TRUE(HighCEmitter().emit({MaskedHigh}, MaskedHighCOS, HighCOptions));
  MaskedHighCOS.flush();
  EXPECT_NE(MaskedHighC.find("vmaskmovps %%fs:(%[address])"),
            std::string::npos);
  EXPECT_NE(MaskedHighC.find("vmaskmovpd %[data], %[mask], "
                             "%%gs:(%[address])"),
            std::string::npos);
  EXPECT_NE(MaskedHighC.find("vmaskmovps (%[address])"), std::string::npos);
  EXPECT_NE(MaskedHighC.find(
                "vmaskmovps %[data], %[mask], (%[address])"),
            std::string::npos);
  EXPECT_EQ(MaskedHighC.find("_mm_maskload"), std::string::npos);
  EXPECT_EQ(MaskedHighC.find("_mm_maskstore"), std::string::npos);
  EXPECT_NE(MaskedHighC.find("typedef unsigned _BitInt(256) uint256_t;"),
            std::string::npos);
  EXPECT_NE(MaskedHighC.find("__m256i"), std::string::npos);
  EXPECT_NE(MaskedHighC.find("__builtin_memcpy"), std::string::npos);
  EXPECT_NE(MaskedHighC.find(
                "if (((neverd_mask >> (neverd_i * 8)) & 0x80u) != 0)"),
            std::string::npos);
  EXPECT_NE(MaskedHighC.find("address_space(257)"), std::string::npos);
  EXPECT_TRUE(validHighC(MaskedHighC));

  // Generic atomic opcodes share the same pointer-resolution owner as scalar
  // loads/stores.  A segmented atomic must bypass image-global resolution and
  // reach LLVM and High-C with its target address space intact.
  auto AtomicTemp = [](int Id) {
    MedVar Var;
    Var.Kind = MedVar::Temp;
    Var.TheArch = Arch::X64;
    Var.Id = Id;
    Var.SSAVer = 1;
    Var.Size = 8;
    return Var;
  };
  MedFunc AtomicMed;
  AtomicMed.Entry = 0x2000;
  AtomicMed.Name = "segment_atomic_semantics";
  AtomicMed.ReturnType = NdType::makeVoid();
  MedBlock AtomicBlock;
  AtomicBlock.Id = 0;
  AtomicBlock.StartAddr = AtomicMed.Entry;
  AtomicBlock.EndAddr = AtomicMed.Entry + 12;
  MedOp AtomicAdd;
  AtomicAdd.Opcode = NdOp::ATOMIC_ADD;
  AtomicAdd.Addr = AtomicMed.Entry;
  AtomicAdd.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
  AtomicAdd.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
  AtomicAdd.Output = AtomicTemp(1);
  AtomicAdd.addInput(
      MedVar::makeConst(0x40, 8, ConstantAddressProvenance::Scalar));
  AtomicAdd.addInput(
      MedVar::makeConst(3, 8, ConstantAddressProvenance::Scalar));
  AtomicBlock.Ops.push_back(AtomicAdd);
  MedOp AtomicCmpXchg;
  AtomicCmpXchg.Opcode = NdOp::ATOMIC_CMPXCHG;
  AtomicCmpXchg.Addr = AtomicMed.Entry + 4;
  AtomicCmpXchg.MemoryOrdering = NdMemoryOrdering::AcquireRelease;
  AtomicCmpXchg.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
  AtomicCmpXchg.Output = AtomicTemp(2);
  AtomicCmpXchg.addInput(
      MedVar::makeConst(0x48, 8, ConstantAddressProvenance::Scalar));
  AtomicCmpXchg.addInput(
      MedVar::makeConst(1, 8, ConstantAddressProvenance::Scalar));
  AtomicCmpXchg.addInput(
      MedVar::makeConst(2, 8, ConstantAddressProvenance::Scalar));
  AtomicBlock.Ops.push_back(AtomicCmpXchg);
  MedOp AtomicReturn;
  AtomicReturn.Opcode = NdOp::RETURN;
  AtomicReturn.Addr = AtomicMed.Entry + 8;
  AtomicBlock.Ops.push_back(AtomicReturn);
  AtomicMed.Blocks.push_back(std::move(AtomicBlock));
  ASSERT_TRUE(verifyMedFunc(AtomicMed, "segment-atomics"));

  llvm::LLVMContext AtomicContext;
  auto AtomicModule = MedLLVMEmitter().emit({AtomicMed}, AtomicContext,
                                            "segment-atomics", Arch::X64);
  ASSERT_NE(AtomicModule, nullptr);
  EXPECT_TRUE(validLLVMModule(*AtomicModule));
  unsigned FSAtomicRMW = 0;
  unsigned GSAtomicCmpXchg = 0;
  for (const llvm::Function &Function : *AtomicModule)
    for (const llvm::BasicBlock &Block : Function)
      for (const llvm::Instruction &Instruction : Block) {
        if (const auto *RMW = llvm::dyn_cast<llvm::AtomicRMWInst>(&Instruction))
          FSAtomicRMW +=
              RMW->getPointerOperand()->getType()->getPointerAddressSpace() ==
              257;
        if (const auto *CmpXchg =
                llvm::dyn_cast<llvm::AtomicCmpXchgInst>(&Instruction))
          GSAtomicCmpXchg += CmpXchg->getPointerOperand()
                                 ->getType()
                                 ->getPointerAddressSpace() == 256;
      }
  EXPECT_EQ(FSAtomicRMW, 1u);
  EXPECT_EQ(GSAtomicCmpXchg, 1u);

  HighFunc AtomicHigh = MedToHighConverter().convert(AtomicMed, Arch::X64);
  bool HighFSAtomic = false;
  bool HighGSAtomic = false;
  walkStmts(AtomicHigh.Body, [&](const HighStmt &Stmt) {
    forEachExpr(Stmt, [&](const ExprPtr &Expr) {
      if (!Expr)
        return;
      HighFSAtomic |= Expr->Op == NdOp::ATOMIC_ADD &&
                      Expr->MemoryAddressSpace == NdMemoryAddressSpace::X86FS;
      HighGSAtomic |= Expr->Op == NdOp::ATOMIC_CMPXCHG &&
                      Expr->MemoryAddressSpace == NdMemoryAddressSpace::X86GS;
    });
  });
  EXPECT_TRUE(HighFSAtomic);
  EXPECT_TRUE(HighGSAtomic);

  std::string AtomicHighC;
  llvm::raw_string_ostream AtomicHighCOS(AtomicHighC);
  ASSERT_TRUE(HighCEmitter().emit({AtomicHigh}, AtomicHighCOS, HighCOptions));
  AtomicHighCOS.flush();
  EXPECT_NE(AtomicHighC.find("__atomic_fetch_add"), std::string::npos);
  EXPECT_NE(AtomicHighC.find("__atomic_compare_exchange_n"), std::string::npos);
  EXPECT_NE(AtomicHighC.find("address_space(257)"), std::string::npos);
  EXPECT_NE(AtomicHighC.find("address_space(256)"), std::string::npos);
  EXPECT_TRUE(validHighC(AtomicHighC));

  // A segment offset remains a raw integer even when its bit pattern and
  // provenance collide with a mapped image object.  Exercise the scalar,
  // generic-atomic and masked-memory pointer owners through one shared
  // arithmetic definition and verify their actual LLVM pointer operands.
  constexpr uint64_t CollisionVA = UINT64_C(0x100000);
  constexpr uint64_t CollisionOffset = CollisionVA + 0x20;
  constexpr uint64_t CollisionAbsoluteSlot = CollisionVA + 0x40;
  constexpr uint64_t CollisionRelativeSlot = CollisionVA + 0x48;
  constexpr uint64_t CollisionTarget = CollisionVA + 0x80;
  BinaryImage CollisionImage;
  CollisionImage.Arch = Arch::X64;
  CollisionImage.Bits = Bitness::Bits64;
  CollisionImage.Format = BinaryFormat::ELF;
  Segment CollisionData;
  CollisionData.VA = CollisionVA;
  CollisionData.Size = 0x100;
  CollisionData.Flags = SegmentFlags::Readable;
  CollisionData.Data.resize(CollisionData.Size);
  std::memcpy(CollisionData.Data.data() +
                  (CollisionAbsoluteSlot - CollisionVA),
              &CollisionTarget, sizeof(CollisionTarget));
  const int32_t RelativeTarget =
      static_cast<int32_t>(CollisionTarget - CollisionRelativeSlot);
  std::memcpy(CollisionData.Data.data() +
                  (CollisionRelativeSlot - CollisionVA),
              &RelativeTarget, sizeof(RelativeTarget));
  CollisionImage.Segments.push_back(std::move(CollisionData));
  CollisionImage.RelocDataAddrs.insert(CollisionVA);
  CollisionImage.RelocDataAddrs.insert(CollisionTarget);
  CollisionImage.DataPtrRelocSlots.insert(CollisionAbsoluteSlot);
  CollisionImage.DataPtrRelocTargetOwners[CollisionAbsoluteSlot] =
      CollisionTarget;
  CollisionImage.RelDataPtrRelocSlots.insert(CollisionRelativeSlot);

  MedFunc RawOffsetMed;
  RawOffsetMed.Entry = 0x2800;
  RawOffsetMed.Name = "segment_raw_numeric_offsets";
  RawOffsetMed.ReturnType = NdType::makeVoid();
  MedBlock RawOffsetBlock;
  RawOffsetBlock.Id = 0;
  RawOffsetBlock.StartAddr = RawOffsetMed.Entry;
  RawOffsetBlock.EndAddr = RawOffsetMed.Entry + 20;
  MedVar RawAddress = MaskedTemp(40, 8);
  MedOp RawAddressAdd;
  RawAddressAdd.Opcode = NdOp::INT_ADD;
  RawAddressAdd.Addr = RawOffsetMed.Entry;
  RawAddressAdd.Output = RawAddress;
  RawAddressAdd.addInput(MedVar::makeConst(
      CollisionVA, 8, ConstantAddressProvenance::DataAddress));
  RawAddressAdd.addInput(
      MedVar::makeConst(0x20, 8, ConstantAddressProvenance::Scalar));
  RawOffsetBlock.Ops.push_back(RawAddressAdd);
  MedOp RawFSLoad;
  RawFSLoad.Opcode = NdOp::LOAD;
  RawFSLoad.Addr = RawOffsetMed.Entry + 4;
  RawFSLoad.Output = MaskedTemp(41, 8);
  RawFSLoad.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
  RawFSLoad.addInput(RawAddress);
  RawOffsetBlock.Ops.push_back(RawFSLoad);
  MedOp RawGSAtomic;
  RawGSAtomic.Opcode = NdOp::ATOMIC_ADD;
  RawGSAtomic.Addr = RawOffsetMed.Entry + 8;
  RawGSAtomic.Output = MaskedTemp(42, 8);
  RawGSAtomic.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
  RawGSAtomic.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
  RawGSAtomic.addInput(RawAddress);
  RawGSAtomic.addInput(
      MedVar::makeConst(1, 8, ConstantAddressProvenance::Scalar));
  RawOffsetBlock.Ops.push_back(RawGSAtomic);
  MedOp RawFSMasked;
  RawFSMasked.Opcode = NdOp::INTRINSIC;
  RawFSMasked.Addr = RawOffsetMed.Entry + 12;
  RawFSMasked.Output = MaskedTemp(43, 16);
  RawFSMasked.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
  RawFSMasked.addInput(MedVar::makeConst(
      static_cast<uint64_t>(Intrinsic::MaskedLoadD), 2,
      ConstantAddressProvenance::Scalar));
  RawFSMasked.addInput(RawAddress);
  RawFSMasked.addInput(MedVar::makeConst(
      UINT64_C(0x8000000080000000), 16,
      ConstantAddressProvenance::Scalar));
  RawOffsetBlock.Ops.push_back(RawFSMasked);
  MedOp RawReturn;
  RawReturn.Opcode = NdOp::RETURN;
  RawReturn.Addr = RawOffsetMed.Entry + 16;
  RawOffsetBlock.Ops.push_back(RawReturn);
  RawOffsetMed.Blocks.push_back(std::move(RawOffsetBlock));
  ASSERT_TRUE(verifyMedFunc(RawOffsetMed, "segment-raw-offsets"));

  llvm::LLVMContext RawOffsetContext;
  auto RawOffsetModule = MedLLVMEmitter().emit(
      {RawOffsetMed}, RawOffsetContext, "segment-raw-offsets", Arch::X64, {},
      &CollisionImage);
  ASSERT_NE(RawOffsetModule, nullptr);
  EXPECT_TRUE(validLLVMModule(*RawOffsetModule));
  auto IsRawSegmentPointer = [&](const llvm::Value *Pointer) {
    const llvm::Value *Numeric = nullptr;
    if (const auto *Cast = llvm::dyn_cast<llvm::IntToPtrInst>(Pointer))
      Numeric = Cast->getOperand(0);
    else if (const auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(Pointer);
             CE && CE->getOpcode() == llvm::Instruction::IntToPtr)
      Numeric = CE->getOperand(0);
    const auto *CI = llvm::dyn_cast_or_null<llvm::ConstantInt>(Numeric);
    return CI && CI->getZExtValue() == CollisionOffset;
  };
  unsigned RawScalarPointers = 0;
  unsigned RawAtomicPointers = 0;
  unsigned RawMaskedPointers = 0;
  for (const llvm::Function &Function : *RawOffsetModule)
    for (const llvm::BasicBlock &Block : Function)
      for (const llvm::Instruction &Instruction : Block) {
        if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction))
          if (Load->getPointerAddressSpace() == 257)
            RawScalarPointers += IsRawSegmentPointer(Load->getPointerOperand());
        if (const auto *RMW =
                llvm::dyn_cast<llvm::AtomicRMWInst>(&Instruction))
          if (RMW->getPointerOperand()->getType()->getPointerAddressSpace() ==
              256)
            RawAtomicPointers += IsRawSegmentPointer(RMW->getPointerOperand());
        if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction))
          if (Call->getCalledFunction() &&
              Call->getCalledFunction()->getName().starts_with(
                  "llvm.masked.load") &&
              Call->getArgOperand(0)->getType()->getPointerAddressSpace() ==
                  257)
            RawMaskedPointers += IsRawSegmentPointer(Call->getArgOperand(0));
      }
  EXPECT_EQ(RawScalarPointers, 1u);
  EXPECT_EQ(RawAtomicPointers, 1u);
  EXPECT_EQ(RawMaskedPointers, 1u);

  // A segmented LOAD whose numeric offset collides with an absolute or
  // relative image pointer-table slot remains a runtime segment read.  Its
  // result must feed the following ordinary dereference instead of being
  // reconstructed from the flat image bytes.
  MedFunc CollisionLoadMed;
  CollisionLoadMed.Entry = 0x2900;
  CollisionLoadMed.Name = "segment_pointer_table_collision";
  CollisionLoadMed.ReturnType = NdType::makeVoid();
  MedBlock CollisionLoadBlock;
  CollisionLoadBlock.Id = 0;
  CollisionLoadBlock.StartAddr = CollisionLoadMed.Entry;
  CollisionLoadBlock.EndAddr = CollisionLoadMed.Entry + 28;

  MedOp CollisionAbsoluteLoad;
  CollisionAbsoluteLoad.Opcode = NdOp::LOAD;
  CollisionAbsoluteLoad.Addr = CollisionLoadMed.Entry;
  CollisionAbsoluteLoad.Output = MaskedTemp(44, 8);
  CollisionAbsoluteLoad.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
  CollisionAbsoluteLoad.addInput(MedVar::makeConst(
      CollisionAbsoluteSlot, 8, ConstantAddressProvenance::Scalar));
  CollisionLoadBlock.Ops.push_back(CollisionAbsoluteLoad);
  MedOp CollisionAbsoluteDeref;
  CollisionAbsoluteDeref.Opcode = NdOp::LOAD;
  CollisionAbsoluteDeref.Addr = CollisionLoadMed.Entry + 4;
  CollisionAbsoluteDeref.Output = MaskedTemp(45, 8);
  CollisionAbsoluteDeref.addInput(CollisionAbsoluteLoad.Output);
  CollisionLoadBlock.Ops.push_back(CollisionAbsoluteDeref);

  MedOp CollisionRelativeLoad;
  CollisionRelativeLoad.Opcode = NdOp::LOAD;
  CollisionRelativeLoad.Addr = CollisionLoadMed.Entry + 8;
  CollisionRelativeLoad.Output = MaskedTemp(46, 4);
  CollisionRelativeLoad.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
  CollisionRelativeLoad.addInput(MedVar::makeConst(
      CollisionRelativeSlot, 8, ConstantAddressProvenance::Scalar));
  CollisionLoadBlock.Ops.push_back(CollisionRelativeLoad);
  MedOp CollisionRelativeSext;
  CollisionRelativeSext.Opcode = NdOp::INT_SEXT;
  CollisionRelativeSext.Addr = CollisionLoadMed.Entry + 12;
  CollisionRelativeSext.Output = MaskedTemp(47, 8);
  CollisionRelativeSext.addInput(CollisionRelativeLoad.Output);
  CollisionLoadBlock.Ops.push_back(CollisionRelativeSext);
  MedOp CollisionRelativeAddress;
  CollisionRelativeAddress.Opcode = NdOp::INT_ADD;
  CollisionRelativeAddress.Addr = CollisionLoadMed.Entry + 16;
  CollisionRelativeAddress.Output = MaskedTemp(48, 8);
  CollisionRelativeAddress.addInput(MedVar::makeConst(
      CollisionRelativeSlot, 8,
      ConstantAddressProvenance::DataAddress));
  CollisionRelativeAddress.addInput(CollisionRelativeSext.Output);
  CollisionLoadBlock.Ops.push_back(CollisionRelativeAddress);
  MedOp CollisionRelativeDeref;
  CollisionRelativeDeref.Opcode = NdOp::LOAD;
  CollisionRelativeDeref.Addr = CollisionLoadMed.Entry + 20;
  CollisionRelativeDeref.Output = MaskedTemp(49, 8);
  CollisionRelativeDeref.addInput(CollisionRelativeAddress.Output);
  CollisionLoadBlock.Ops.push_back(CollisionRelativeDeref);
  MedOp CollisionLoadReturn;
  CollisionLoadReturn.Opcode = NdOp::RETURN;
  CollisionLoadReturn.Addr = CollisionLoadMed.Entry + 24;
  CollisionLoadBlock.Ops.push_back(CollisionLoadReturn);
  CollisionLoadMed.Blocks.push_back(std::move(CollisionLoadBlock));
  ASSERT_TRUE(verifyMedFunc(CollisionLoadMed,
                            "segment-pointer-table-collision"));

  llvm::LLVMContext CollisionLoadContext;
  auto CollisionLoadModule = MedLLVMEmitter().emit(
      {CollisionLoadMed}, CollisionLoadContext,
      "segment-pointer-table-collision", Arch::X64, {}, &CollisionImage);
  ASSERT_NE(CollisionLoadModule, nullptr);
  EXPECT_TRUE(validLLVMModule(*CollisionLoadModule));
  std::vector<const llvm::LoadInst *> RuntimeAbsoluteLoads;
  std::vector<const llvm::LoadInst *> RuntimeRelativeLoads;
  std::vector<const llvm::LoadInst *> FlatDereferences;
  auto IsAllocaBackedLoad = [](const llvm::LoadInst *Load) {
    return llvm::isa<llvm::AllocaInst>(
        Load->getPointerOperand()->stripPointerCasts());
  };
  for (const llvm::Function &Function : *CollisionLoadModule)
    for (const llvm::BasicBlock &Block : Function)
      for (const llvm::Instruction &Instruction : Block)
        if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction)) {
          if (Load->getPointerAddressSpace() == 257 &&
              Load->getType()->isIntegerTy(64))
            RuntimeAbsoluteLoads.push_back(Load);
          else if (Load->getPointerAddressSpace() == 256 &&
                   Load->getType()->isIntegerTy(32))
            RuntimeRelativeLoads.push_back(Load);
          else if (Load->getPointerAddressSpace() == 0 &&
                   !IsAllocaBackedLoad(Load))
            FlatDereferences.push_back(Load);
        }
  ASSERT_EQ(RuntimeAbsoluteLoads.size(), 1u);
  ASSERT_EQ(RuntimeRelativeLoads.size(), 1u);
  ASSERT_EQ(FlatDereferences.size(), 2u);
  const llvm::LoadInst *RuntimeAbsoluteLoad = RuntimeAbsoluteLoads.front();
  const llvm::LoadInst *RuntimeRelativeLoad = RuntimeRelativeLoads.front();
  auto DependsOn = [](const llvm::Value *Root,
                      const llvm::Value *Needle) {
    std::vector<const llvm::Value *> Work{Root};
    std::set<const llvm::Value *> Seen;
    while (!Work.empty()) {
      const llvm::Value *Value = Work.back();
      Work.pop_back();
      if (Value == Needle)
        return true;
      if (!Seen.insert(Value).second)
        continue;
      // Med variables are deliberately materialized through allocas before
      // mem2reg.  Recover the local SSA dependency through the nearest store
      // that precedes this load in the same basic block; later stores and
      // stores on other control-flow paths are not reaching definitions here.
      if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Value)) {
        const llvm::Value *Pointer =
            Load->getPointerOperand()->stripPointerCasts();
        if (llvm::isa<llvm::AllocaInst>(Pointer)) {
          const llvm::StoreInst *ReachingStore = nullptr;
          for (const llvm::Instruction &Candidate : *Load->getParent()) {
            if (&Candidate == Load)
              break;
            const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Candidate);
            if (Store &&
                Store->getPointerOperand()->stripPointerCasts() == Pointer)
              ReachingStore = Store;
          }
          if (ReachingStore)
            Work.push_back(ReachingStore->getValueOperand());
        }
      }
      if (const auto *User = llvm::dyn_cast<llvm::User>(Value))
        for (const llvm::Use &Operand : User->operands())
          Work.push_back(Operand.get());
    }
    return false;
  };
  EXPECT_TRUE(std::any_of(
      FlatDereferences.begin(), FlatDereferences.end(),
      [&](const llvm::LoadInst *Load) {
        return DependsOn(Load->getPointerOperand(), RuntimeAbsoluteLoad);
      }));
  EXPECT_TRUE(std::any_of(
      FlatDereferences.begin(), FlatDereferences.end(),
      [&](const llvm::LoadInst *Load) {
        return DependsOn(Load->getPointerOperand(), RuntimeRelativeLoad);
      }));

  // A runtime segmented pointer-table read can merge with an already
  // symbolized writable-data address.  The merged pointer is Mixed: only the
  // flat-image arm may be rebased into the writable global, while the FS arm
  // must remain the value produced by the runtime segment load.
  constexpr uint64_t WritableTableVA = UINT64_C(0x300000);
  constexpr uint64_t WritableTableSlot = WritableTableVA + 0x20;
  constexpr uint64_t WritableRunVA = UINT64_C(0x400000);
  constexpr uint64_t WritableTarget = WritableRunVA + 0x20;
  BinaryImage WritableCollisionImage;
  WritableCollisionImage.Arch = Arch::X64;
  WritableCollisionImage.Bits = Bitness::Bits64;
  WritableCollisionImage.Format = BinaryFormat::ELF;
  WritableCollisionImage.Base = WritableTableVA;
  Segment WritablePointerTable;
  WritablePointerTable.Name = ".rodata.ptr";
  WritablePointerTable.VA = WritableTableVA;
  WritablePointerTable.Size = 0x100;
  WritablePointerTable.Flags = SegmentFlags::Readable;
  WritablePointerTable.Data.resize(WritablePointerTable.Size);
  std::memcpy(WritablePointerTable.Data.data() +
                  (WritableTableSlot - WritableTableVA),
              &WritableTarget, sizeof(WritableTarget));
  WritableCollisionImage.Segments.push_back(std::move(WritablePointerTable));
  Segment WritableRun;
  WritableRun.Name = ".data";
  WritableRun.VA = WritableRunVA;
  WritableRun.Size = 0x100;
  WritableRun.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  WritableRun.Data.resize(WritableRun.Size);
  WritableCollisionImage.Segments.push_back(std::move(WritableRun));
  Section WritableSection;
  WritableSection.Name = ".data";
  WritableSection.VA = WritableRunVA;
  WritableSection.Size = 0x100;
  WritableSection.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  WritableCollisionImage.Sections.push_back(std::move(WritableSection));
  WritableCollisionImage.DataPtrRelocSlots.insert(WritableTableSlot);
  WritableCollisionImage.DataPtrRelocTargetOwners[WritableTableSlot] =
      WritableTarget;
  WritableCollisionImage.RelocDataAddrs.insert(WritableTarget);
  WritableCollisionImage.WritableRelocDataAddrs.insert(WritableTarget);

  MedFunc WritableMergeMed;
  WritableMergeMed.Entry = 0x2980;
  WritableMergeMed.Name = "segment_writable_pointer_merge";
  WritableMergeMed.ReturnType = NdType::makeVoid();
  MedVar WritableCondition;
  WritableCondition.Kind = MedVar::Reg;
  WritableCondition.TheArch = Arch::X64;
  WritableCondition.Id = 70;
  WritableCondition.SSAVer = 0;
  WritableCondition.RegOff = x86reg::RCX;
  WritableCondition.Size = 1;
  WritableMergeMed.Params.push_back(WritableCondition);
  MedBlock WritableMergeBlock;
  WritableMergeBlock.Id = 0;
  WritableMergeBlock.StartAddr = WritableMergeMed.Entry;
  WritableMergeBlock.EndAddr = WritableMergeMed.Entry + 20;
  MedOp WritableSlotAddress;
  WritableSlotAddress.Opcode = NdOp::INT_ADD;
  WritableSlotAddress.Addr = WritableMergeMed.Entry;
  WritableSlotAddress.Output = MaskedTemp(71, 8);
  WritableSlotAddress.addInput(MedVar::makeConst(
      WritableTableVA, 8, ConstantAddressProvenance::DataAddress,
      WritableTableVA));
  WritableSlotAddress.addInput(MedVar::makeConst(
      WritableTableSlot - WritableTableVA, 8,
      ConstantAddressProvenance::Scalar));
  WritableMergeBlock.Ops.push_back(WritableSlotAddress);
  MedOp WritableSegmentLoad;
  WritableSegmentLoad.Opcode = NdOp::LOAD;
  WritableSegmentLoad.Addr = WritableMergeMed.Entry + 4;
  WritableSegmentLoad.Output = MaskedTemp(72, 8);
  WritableSegmentLoad.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
  WritableSegmentLoad.addInput(WritableSlotAddress.Output);
  WritableMergeBlock.Ops.push_back(WritableSegmentLoad);
  MedOp WritableSelect;
  WritableSelect.Opcode = NdOp::SELECT;
  WritableSelect.Addr = WritableMergeMed.Entry + 8;
  WritableSelect.Output = MaskedTemp(73, 8);
  WritableSelect.addInput(WritableCondition);
  WritableSelect.addInput(WritableSegmentLoad.Output);
  WritableSelect.addInput(MedVar::makeConst(
      WritableTarget, 8, ConstantAddressProvenance::DataAddress,
      WritableTarget));
  WritableMergeBlock.Ops.push_back(WritableSelect);
  MedOp WritableMergedDeref;
  WritableMergedDeref.Opcode = NdOp::LOAD;
  WritableMergedDeref.Addr = WritableMergeMed.Entry + 12;
  WritableMergedDeref.Output = MaskedTemp(74, 8);
  WritableMergedDeref.addInput(WritableSelect.Output);
  WritableMergeBlock.Ops.push_back(WritableMergedDeref);
  MedOp WritableMergeReturn;
  WritableMergeReturn.Opcode = NdOp::RETURN;
  WritableMergeReturn.Addr = WritableMergeMed.Entry + 16;
  WritableMergeBlock.Ops.push_back(WritableMergeReturn);
  WritableMergeMed.Blocks.push_back(std::move(WritableMergeBlock));
  ASSERT_TRUE(
      verifyMedFunc(WritableMergeMed, "segment-writable-pointer-merge"));

  llvm::LLVMContext WritableMergeContext;
  auto WritableMergeModule = MedLLVMEmitter().emit(
      {WritableMergeMed}, WritableMergeContext,
      "segment-writable-pointer-merge", Arch::X64, {},
      &WritableCollisionImage);
  ASSERT_NE(WritableMergeModule, nullptr);
  EXPECT_TRUE(validLLVMModule(*WritableMergeModule));
  std::vector<const llvm::LoadInst *> WritableRuntimeSegmentLoads;
  std::vector<const llvm::SelectInst *> WritableMixedPointers;
  std::vector<const llvm::LoadInst *> WritableFlatDerefs;
  for (const llvm::Function &Function : *WritableMergeModule)
    for (const llvm::BasicBlock &Block : Function)
      for (const llvm::Instruction &Instruction : Block) {
        if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction)) {
          if (Load->getPointerAddressSpace() == 257)
            WritableRuntimeSegmentLoads.push_back(Load);
          else if (Load->getPointerAddressSpace() == 0 &&
                   Load->getType()->isIntegerTy(64) &&
                   !IsAllocaBackedLoad(Load))
            WritableFlatDerefs.push_back(Load);
        }
        if (const auto *Select =
                llvm::dyn_cast<llvm::SelectInst>(&Instruction))
          if (Select->getName() == "wrptr.mixed")
            WritableMixedPointers.push_back(Select);
      }
  ASSERT_EQ(WritableRuntimeSegmentLoads.size(), 1u);
  ASSERT_EQ(WritableMixedPointers.size(), 1u);
  ASSERT_EQ(WritableFlatDerefs.size(), 1u);
  const llvm::LoadInst *WritableRuntimeSegmentLoad =
      WritableRuntimeSegmentLoads.front();
  const llvm::SelectInst *WritableMixedPointer =
      WritableMixedPointers.front();
  const llvm::LoadInst *WritableFlatDeref = WritableFlatDerefs.front();
  EXPECT_TRUE(DependsOn(WritableMixedPointer, WritableRuntimeSegmentLoad));
  EXPECT_TRUE(
      DependsOn(WritableFlatDeref->getPointerOperand(), WritableMixedPointer));

  // FS/GS offsets are integer ABI values, not ordinary host-process pointers.
  MedFunc OffsetParamMed;
  OffsetParamMed.Entry = 0x2a00;
  OffsetParamMed.Name = "segment_offset_parameter";
  OffsetParamMed.ReturnType = NdType::makeVoid();
  MedVar OffsetParam;
  OffsetParam.Kind = MedVar::Reg;
  OffsetParam.TheArch = Arch::X64;
  OffsetParam.Id = 1;
  OffsetParam.SSAVer = 0;
  OffsetParam.RegOff = x86reg::RBX;
  OffsetParam.Size = 8;
  OffsetParamMed.Params.push_back(OffsetParam);
  MedBlock OffsetParamBlock;
  OffsetParamBlock.Id = 0;
  OffsetParamBlock.StartAddr = OffsetParamMed.Entry;
  OffsetParamBlock.EndAddr = OffsetParamMed.Entry + 8;
  MedOp OffsetParamLoad;
  OffsetParamLoad.Opcode = NdOp::LOAD;
  OffsetParamLoad.Addr = OffsetParamMed.Entry;
  OffsetParamLoad.Output = MaskedTemp(50, 8);
  OffsetParamLoad.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
  OffsetParamLoad.addInput(OffsetParam);
  OffsetParamBlock.Ops.push_back(OffsetParamLoad);
  MedOp OffsetParamReturn;
  OffsetParamReturn.Opcode = NdOp::RETURN;
  OffsetParamReturn.Addr = OffsetParamMed.Entry + 4;
  OffsetParamBlock.Ops.push_back(OffsetParamReturn);
  OffsetParamMed.Blocks.push_back(std::move(OffsetParamBlock));
  inferMedTypes(OffsetParamMed, Arch::X64);
  ASSERT_EQ(OffsetParamMed.TypedParams.size(), 1u);
  ASSERT_TRUE(OffsetParamMed.TypedParams.front().Type);
  EXPECT_EQ(OffsetParamMed.TypedParams.front().Type->Kind, NdTypeKind::Int);
  HighFunc OffsetParamHigh =
      MedToHighConverter().convert(OffsetParamMed, Arch::X64);
  ASSERT_EQ(OffsetParamHigh.Params.size(), 1u);
  ASSERT_TRUE(OffsetParamHigh.Params.front().Type);
  EXPECT_EQ(OffsetParamHigh.Params.front().Type->Kind, NdTypeKind::Int);
  unsigned ObservableSegmentLoads = 0;
  walkStmts(OffsetParamHigh.Body, [&](const HighStmt &Stmt) {
    forEachExpr(Stmt, [&](const ExprPtr &Expr) {
      ObservableSegmentLoads +=
          Expr && Expr->Kind == ExprKind::Load &&
          Expr->MemoryAddressSpace == NdMemoryAddressSpace::X86FS;
    });
  });
  EXPECT_EQ(ObservableSegmentLoads, 1u)
      << "nondefault loads remain observable even when their value is unused";

  // A single live-in may be both a flat pointer and an FS/GS numeric offset.
  // Its ABI representation must remain an integer so neither backend casts the
  // segmented use through a host pointer or rejects raw-offset reconstruction.
  MedFunc MixedRoleMed;
  MixedRoleMed.Entry = 0x2b00;
  MixedRoleMed.Name = "mixed_memory_address_parameter";
  MixedRoleMed.ReturnType = NdType::makeInt(8, false);
  MedVar MixedRoleParam = OffsetParam;
  MixedRoleParam.RegOff = x86reg::RCX;
  MixedRoleMed.Params.push_back(MixedRoleParam);
  MedBlock MixedRoleBlock;
  MixedRoleBlock.Id = 0;
  MixedRoleBlock.StartAddr = MixedRoleMed.Entry;
  MixedRoleBlock.EndAddr = MixedRoleMed.Entry + 16;
  MedOp MixedDefaultLoad;
  MixedDefaultLoad.Opcode = NdOp::LOAD;
  MixedDefaultLoad.Addr = MixedRoleMed.Entry;
  MixedDefaultLoad.Output = MaskedTemp(60, 8);
  MixedDefaultLoad.addInput(MixedRoleParam);
  MixedRoleBlock.Ops.push_back(MixedDefaultLoad);
  MedOp MixedSegmentLoad = MixedDefaultLoad;
  MixedSegmentLoad.Addr = MixedRoleMed.Entry + 4;
  MixedSegmentLoad.Output = MaskedTemp(61, 8);
  MixedSegmentLoad.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
  MixedRoleBlock.Ops.push_back(MixedSegmentLoad);
  MedOp MixedSum;
  MixedSum.Opcode = NdOp::INT_XOR;
  MixedSum.Addr = MixedRoleMed.Entry + 8;
  MixedSum.Output = MaskedTemp(62, 8);
  MixedSum.addInput(MixedDefaultLoad.Output);
  MixedSum.addInput(MixedSegmentLoad.Output);
  MixedRoleBlock.Ops.push_back(MixedSum);
  MedOp MixedReturn;
  MixedReturn.Opcode = NdOp::RETURN;
  MixedReturn.Addr = MixedRoleMed.Entry + 12;
  MixedReturn.addInput(MixedSum.Output);
  MixedRoleBlock.Ops.push_back(MixedReturn);
  MixedRoleMed.Blocks.push_back(std::move(MixedRoleBlock));
  inferMedTypes(MixedRoleMed, Arch::X64);
  ASSERT_EQ(MixedRoleMed.TypedParams.size(), 1u);
  ASSERT_TRUE(MixedRoleMed.TypedParams.front().Type);
  EXPECT_EQ(MixedRoleMed.TypedParams.front().Type->Kind, NdTypeKind::Int);

  llvm::LLVMContext MixedRoleContext;
  auto MixedRoleModule = MedLLVMEmitter().emit(
      {MixedRoleMed}, MixedRoleContext, "mixed-memory-address-parameter",
      Arch::X64);
  ASSERT_NE(MixedRoleModule, nullptr);
  EXPECT_TRUE(validLLVMModule(*MixedRoleModule));
  const llvm::Function *MixedRoleFunction = nullptr;
  for (const llvm::Function &Function : *MixedRoleModule)
    if (!Function.isDeclaration())
      MixedRoleFunction = &Function;
  ASSERT_NE(MixedRoleFunction, nullptr);
  ASSERT_EQ(MixedRoleFunction->arg_size(), 1u);
  const llvm::Argument *MixedRoleArgument =
      MixedRoleFunction->arg_begin();
  EXPECT_TRUE(MixedRoleArgument->getType()->isIntegerTy(64));
  std::vector<const llvm::LoadInst *> MixedDefaultLoads;
  std::vector<const llvm::LoadInst *> MixedSegmentLoads;
  for (const llvm::BasicBlock &Block : *MixedRoleFunction)
    for (const llvm::Instruction &Instruction : Block)
      if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction)) {
        if (Load->getPointerAddressSpace() == 0 &&
            !IsAllocaBackedLoad(Load))
          MixedDefaultLoads.push_back(Load);
        if (Load->getPointerAddressSpace() == 257)
          MixedSegmentLoads.push_back(Load);
      }
  ASSERT_EQ(MixedDefaultLoads.size(), 1u);
  ASSERT_EQ(MixedSegmentLoads.size(), 1u);
  const llvm::LoadInst *MixedDefaultLLVM = MixedDefaultLoads.front();
  const llvm::LoadInst *MixedSegmentLLVM = MixedSegmentLoads.front();
  EXPECT_TRUE(
      DependsOn(MixedDefaultLLVM->getPointerOperand(), MixedRoleArgument));
  EXPECT_TRUE(
      DependsOn(MixedSegmentLLVM->getPointerOperand(), MixedRoleArgument));

  HighFunc MixedRoleHigh =
      MedToHighConverter().convert(MixedRoleMed, Arch::X64);
  ASSERT_EQ(MixedRoleHigh.Params.size(), 1u);
  ASSERT_TRUE(MixedRoleHigh.Params.front().Type);
  EXPECT_EQ(MixedRoleHigh.Params.front().Type->Kind, NdTypeKind::Int);
  std::string MixedRoleHighC;
  llvm::raw_string_ostream MixedRoleHighCOS(MixedRoleHighC);
  ASSERT_TRUE(HighCEmitter().emit({MixedRoleHigh}, MixedRoleHighCOS,
                                  HighCOptions));
  MixedRoleHighCOS.flush();
  EXPECT_NE(MixedRoleHighC.find("neverd_mem_load_fs_"), std::string::npos);
  EXPECT_TRUE(validHighC(MixedRoleHighC));

  // Invalid enum values, non-memory opcodes, and intrinsic IDs without an
  // address-space-aware implementation are rejected at the public IR
  // boundaries rather than being silently treated as ordinary memory.
  LowFunc BadLowOpcode = Low;
  auto BadCopy =
      std::find_if(BadLowOpcode.Blocks.front().Ops.begin(),
                   BadLowOpcode.Blocks.front().Ops.end(),
                   [](const LowOp &Op) { return Op.Opcode == NdOp::COPY; });
  ASSERT_NE(BadCopy, BadLowOpcode.Blocks.front().Ops.end());
  BadCopy->MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
  std::string BadOpcodeError =
      llvm::toString(validateLowInstructionBoundaries(BadLowOpcode));
  EXPECT_NE(BadOpcodeError.find("non-memory opcode"), std::string::npos);

  LowFunc BadLowEnum = Low;
  auto BadEnumLoad =
      std::find_if(BadLowEnum.Blocks.front().Ops.begin(),
                   BadLowEnum.Blocks.front().Ops.end(),
                   [](const LowOp &Op) { return Op.Opcode == NdOp::LOAD; });
  ASSERT_NE(BadEnumLoad, BadLowEnum.Blocks.front().Ops.end());
  BadEnumLoad->MemoryAddressSpace = static_cast<NdMemoryAddressSpace>(0xff);
  std::string BadEnumError =
      llvm::toString(validateLowInstructionBoundaries(BadLowEnum));
  EXPECT_NE(BadEnumError.find("unknown memory address space"),
            std::string::npos);

  auto MalformedSegmentedString = [&](Intrinsic ID,
                                      uint8_t TruncatedInputs) {
    LowFunc BadShape = StringLow;
    bool Truncated = false;
    for (LowOp &Op : BadShape.Blocks.front().Ops)
      if (Op.Opcode == NdOp::INTRINSIC &&
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default &&
          Op.NumInputs >= 1 && Op.Inputs[0].isConst() &&
          Op.Inputs[0].Offset == static_cast<uint64_t>(ID)) {
        Op.NumInputs = TruncatedInputs;
        Truncated = true;
        break;
      }
    EXPECT_TRUE(Truncated);
    if (!Truncated)
      return std::string{};
    return llvm::toString(validateLowInstructionBoundaries(BadShape));
  };
  const std::string BadMovsShape =
      MalformedSegmentedString(Intrinsic::Movsb, /*TruncatedInputs=*/4);
  EXPECT_NE(BadMovsShape.find("invalid operand/output shape"),
            std::string::npos);
  const std::string BadCmpsShape =
      MalformedSegmentedString(Intrinsic::Cmpsb, /*TruncatedInputs=*/5);
  EXPECT_NE(BadCmpsShape.find("invalid operand/output shape"),
            std::string::npos);

  LowOp InvalidConsumerOp;
  InvalidConsumerOp.Opcode = NdOp::COPY;
  InvalidConsumerOp.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
  InvalidConsumerOp.Output = NdVar::reg(x86reg::RAX, 8);
  InvalidConsumerOp.addInput(NdVar::scalar(7, 8));
  NdOpEmulator InvalidConsumer(SemanticImage);
  EXPECT_FALSE(InvalidConsumer.step(InvalidConsumerOp));
  EXPECT_FALSE(InvalidConsumer.getRegister(x86reg::RAX));

  LowFunc BadLowIntrinsic = StringLow;
  bool ReplacedIntrinsic = false;
  for (LowOp &Op : BadLowIntrinsic.Blocks.front().Ops)
    if (Op.Opcode == NdOp::INTRINSIC &&
        Op.MemoryAddressSpace != NdMemoryAddressSpace::Default &&
        Op.NumInputs >= 1 && Op.Inputs[0].isConst()) {
      Op.Inputs[0] = NdVar::scalar(static_cast<uint64_t>(Intrinsic::Stosb), 2);
      ReplacedIntrinsic = true;
      break;
    }
  ASSERT_TRUE(ReplacedIntrinsic);
  std::string BadIntrinsicError =
      llvm::toString(validateLowInstructionBoundaries(BadLowIntrinsic));
  EXPECT_NE(BadIntrinsicError.find("does not support"), std::string::npos);

  MedFunc BadMedOpcode = AtomicMed;
  BadMedOpcode.Blocks.front().Ops.front().Opcode = NdOp::COPY;
  EXPECT_FALSE(verifyMedFunc(BadMedOpcode, "bad-segment-opcode"));
  MedFunc BadMedIntrinsic = StringMed;
  bool ReplacedMedIntrinsic = false;
  for (MedOp &Op : BadMedIntrinsic.Blocks.front().Ops)
    if (Op.Opcode == NdOp::INTRINSIC &&
        Op.MemoryAddressSpace != NdMemoryAddressSpace::Default &&
        Op.NumInputs >= 1 && Op.Inputs[0].isConst()) {
      Op.Inputs[0] = MedVar::makeConst(static_cast<uint64_t>(Intrinsic::Stosb),
                                       2, ConstantAddressProvenance::Scalar);
      ReplacedMedIntrinsic = true;
      break;
    }
  ASSERT_TRUE(ReplacedMedIntrinsic);
  EXPECT_FALSE(verifyMedFunc(BadMedIntrinsic, "bad-segment-intrinsic"));
}

std::string errorText(llvm::Error Error) {
  return llvm::toString(std::move(Error));
}

std::vector<LowOp> liftARMInstruction(InstructionMode Mode,
                                      const std::vector<uint8_t> &Bytes,
                                      va_t Address = kEntry) {
  Decoder Dec;
  if (!Dec.init(Arch::ARM, Mode)) {
    ADD_FAILURE() << "decoder initialization failed";
    return {};
  }

  DecodedInsn Insn{};
  int Size = Dec.decodeOneForLift(Bytes.data(), Bytes.size(), Address, Insn);
  if (Size <= 0) {
    ADD_FAILURE() << "instruction decode failed";
    return {};
  }
  EXPECT_EQ(static_cast<size_t>(Size), Bytes.size());

  std::vector<LowOp> Ops;
  Dec.liftToLow(Insn, Ops);
  return Ops;
}

TEST(LowInstructionBoundary,
     ARMPredicatedRegisterEffectsHaveUniqueOccurrenceSequences) {
  // movhi r0, #0.  The ARM lifter represents the predicated register write as
  // a SELECT after the instruction's architectural-PC seed.  Every emitted
  // LowOp must retain a unique (Addr, Seq) occurrence so exact provenance
  // consumers cannot confuse the SELECT with the PC COPY.
  const std::vector<uint8_t> MovHiR0Zero = {0x00, 0x00, 0xa0, 0x83};
  const std::vector<LowOp> Ops =
      liftARMInstruction(InstructionMode::ARM, MovHiR0Zero);
  ASSERT_FALSE(Ops.empty());
  ASSERT_TRUE(std::any_of(Ops.begin(), Ops.end(), [](const LowOp &Op) {
    return Op.Opcode == NdOp::SELECT;
  }));

  std::set<std::pair<va_t, int>> Occurrences;
  for (const LowOp &Op : Ops)
    EXPECT_TRUE(Occurrences.emplace(Op.Addr, Op.Seq).second)
        << "duplicate LowIR occurrence at 0x" << std::hex << Op.Addr << "."
        << std::dec << Op.Seq;
}

std::vector<LowOp> liftAArch64Instruction(const std::vector<uint8_t> &Bytes,
                                          va_t Address = kEntry) {
  Decoder Dec;
  if (!Dec.init(Arch::AArch64, InstructionMode::Default)) {
    ADD_FAILURE() << "decoder initialization failed";
    return {};
  }

  DecodedInsn Insn{};
  int Size = Dec.decodeOneForLift(Bytes.data(), Bytes.size(), Address, Insn);
  if (Size <= 0) {
    ADD_FAILURE() << "instruction decode failed";
    return {};
  }
  EXPECT_EQ(static_cast<size_t>(Size), Bytes.size());

  std::vector<LowOp> Ops;
  Dec.liftToLow(Insn, Ops);
  return Ops;
}

std::vector<LowOp> liftX86Instruction(Arch Architecture,
                                      const std::vector<uint8_t> &Bytes,
                                      va_t Address = kEntry) {
  Decoder Dec;
  if (!Dec.init(Architecture, InstructionMode::Default)) {
    ADD_FAILURE() << "decoder initialization failed";
    return {};
  }

  DecodedInsn Insn{};
  int Size = Dec.decodeOneForLift(Bytes.data(), Bytes.size(), Address, Insn);
  if (Size <= 0) {
    ADD_FAILURE() << "instruction decode failed";
    return {};
  }
  EXPECT_EQ(static_cast<size_t>(Size), Bytes.size());

  std::vector<LowOp> Ops;
  Dec.liftToLow(Insn, Ops);
  return Ops;
}

TEST(LowInstructionBoundary, X86GetPcPairCannotCrossDisjointDecodeRoots) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));

  // call $+5
  const std::array<uint8_t, 5> CallNext = {0xe8, 0x00, 0x00, 0x00, 0x00};
  DecodedInsn Call{};
  ASSERT_EQ(
      Dec.decodeOneForLift(CallNext.data(), CallNext.size(), kEntry, Call),
      static_cast<int>(CallNext.size()));
  std::vector<LowOp> Ops;
  Dec.liftToLow(Call, Ops);
  EXPECT_FALSE(Dec.getX86GetPcOccurrence());

  // pop %esi at an independently explored address.  The decoder may visit
  // disconnected CFG roots in this order, but the machine instructions are
  // not adjacent and therefore do not form a get-PC thunk.
  const std::array<uint8_t, 1> PopESI = {0x5e};
  DecodedInsn Pop{};
  ASSERT_EQ(
      Dec.decodeOneForLift(PopESI.data(), PopESI.size(), kEntry + 0x100, Pop),
      static_cast<int>(PopESI.size()));
  Dec.liftToLow(Pop, Ops);
  EXPECT_FALSE(Dec.getX86GetPcOccurrence());
}

LowFunc buildAArch64PageBaseUse(std::vector<uint8_t> Bytes) {
  constexpr va_t DataPage = 0x9000;

  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Mode = InstructionMode::Default;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::MachO;
  Image.Base = kEntry;
  Image.Entry = kEntry;

  Segment Text;
  Text.Name = "__TEXT";
  Text.VA = kEntry;
  Text.Size = Bytes.size();
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = std::move(Bytes);
  Image.Segments.push_back(std::move(Text));

  Segment Data;
  Data.Name = "__DATA";
  Data.VA = DataPage;
  Data.Size = 0x100;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(Data.Size);
  Image.Segments.push_back(std::move(Data));

  // Deliberately give the page start exact loader ownership in both tests.
  // The negative case therefore cannot pass by merely noticing that the ADRP
  // result numerically equals a known pointer slot.
  Image.CodePtrRelocSlots.insert(DataPage);

  Decoder Dec;
  if (!Dec.init(Arch::AArch64, InstructionMode::Default)) {
    ADD_FAILURE() << "decoder initialization failed";
    return {};
  }
  CFGBuilder Builder;
  return Builder.build(Image, Dec, kEntry, "aarch64_page_base_use");
}

LowFunc buildAArch64RelocationFreeOffsetLoad(bool WritableData = false) {
  constexpr va_t DataPage = 0x9000;
  constexpr va_t Target = DataPage + 0x10;
  // adrp x8,0x9000; ldr x8,[x8,#0x10]; ret
  std::vector<uint8_t> Bytes = {0x48, 0x00, 0x00, 0x90, 0x08, 0x09,
                                0x40, 0xf9, 0xc0, 0x03, 0x5f, 0xd6};

  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Mode = InstructionMode::Default;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::MachO;
  Image.Base = kEntry;
  Image.Entry = kEntry;

  Segment Text;
  Text.Name = "__TEXT";
  Text.VA = kEntry;
  Text.Size = Bytes.size();
  Text.FileSz = Text.Size;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = std::move(Bytes);
  Image.Segments.push_back(std::move(Text));

  Segment Data;
  Data.Name = "__DATA_CONST";
  Data.VA = DataPage;
  Data.Size = 0x100;
  Data.FileSz = Data.Size;
  Data.Flags = SegmentFlags::Readable;
  if (WritableData)
    Data.Flags = Data.Flags | SegmentFlags::Writable;
  Data.Data.resize(Data.Size);
  Image.Segments.push_back(std::move(Data));
  Image.CodePtrRelocSlots.insert(Target);

  Decoder Dec;
  if (!Dec.init(Arch::AArch64, InstructionMode::Default)) {
    ADD_FAILURE() << "decoder initialization failed";
    return {};
  }
  CFGBuilder Builder;
  return Builder.build(Image, Dec, kEntry,
                       "aarch64_relocation_free_offset_load");
}

const LowOp *findAddressMaterialization(const LowFunc &Function,
                                        va_t InstructionAddress) {
  for (const LowBlock &Block : Function.Blocks)
    for (const LowOp &Op : Block.Ops)
      if (Op.Addr == InstructionAddress && Op.Opcode == NdOp::COPY &&
          Op.NumInputs == 1 && Op.Output.isReg() && Op.Inputs[0].isConst())
        return &Op;
  return nullptr;
}

bool containsOp(const std::vector<LowOp> &Ops, NdOp Opcode) {
  return std::any_of(Ops.begin(), Ops.end(),
                     [Opcode](const LowOp &Op) { return Op.Opcode == Opcode; });
}

TEST(LowInstructionBoundary,
     DisabledMpxHintNopsPreserveLengthAndHaveNoEffects) {
  struct TailCase {
    const char *Name;
    std::vector<uint8_t> Bytes;
    uint16_t InstructionSize;
  };
  const std::array<TailCase, 9> Tails = {{
      {"register", {0xde}, 3},
      {"mod00", {0x00}, 3},
      {"mod00_sib", {0x04, 0x08}, 4},
      {"mod00_sib_disp32", {0x04, 0x0d, 0x78, 0x56, 0x34, 0x12}, 8},
      {"mod00_disp32", {0x05, 0x78, 0x56, 0x34, 0x12}, 7},
      {"mod01_disp8", {0x40, 0x7f}, 4},
      {"mod01_sib_disp8", {0x44, 0x08, 0x7f}, 5},
      {"mod10_disp32", {0x80, 0x78, 0x56, 0x34, 0x12}, 7},
      {"mod10_sib_disp32", {0x84, 0x08, 0x78, 0x56, 0x34, 0x12}, 8},
  }};

  for (Arch Architecture : {Arch::X86, Arch::X64}) {
    Decoder Full;
    ASSERT_TRUE(Full.init(Architecture));
    Decoder Light;
    ASSERT_TRUE(Light.init(Architecture));
    Light.setDetail(false);

    for (uint8_t Opcode : {uint8_t{0x1a}, uint8_t{0x1b}}) {
      for (const TailCase &Tail : Tails) {
        SCOPED_TRACE(::testing::Message()
                     << "arch=" << static_cast<unsigned>(Architecture)
                     << " opcode=0x" << std::hex
                     << static_cast<unsigned>(Opcode) << " " << Tail.Name);
        std::vector<uint8_t> Bytes = {0x0f, Opcode};
        Bytes.insert(Bytes.end(), Tail.Bytes.begin(), Tail.Bytes.end());

        DecodedInsn Insn{};
        ASSERT_EQ(Full.decodeOne(Bytes.data(), Bytes.size(), kEntry, Insn),
                  Tail.InstructionSize);
        EXPECT_EQ(Insn.Id, X86_INS_NOP);
        EXPECT_EQ(Insn.Size, Tail.InstructionSize);

        std::vector<LowOp> Ops;
        Full.liftToLow(Insn, Ops);
        ASSERT_EQ(Ops.size(), 1u);
        EXPECT_EQ(Ops.front().Opcode, NdOp::NOP);

        DecodedInsn LightInsn{};
        ASSERT_EQ(
            Light.decodeOneLight(Bytes.data(), Bytes.size(), kEntry, LightInsn),
            Tail.InstructionSize);
        EXPECT_EQ(LightInsn.Id, X86_INS_NOP);
        EXPECT_EQ(LightInsn.Size, Tail.InstructionSize);
      }
    }
  }
}

TEST(LowInstructionBoundary, MandatoryMpxPrefixesRemainDistinct) {
  struct PrefixCase {
    const char *Name;
    std::vector<uint8_t> Bytes;
    uint32_t ExpectedId;
  };
  const std::array<PrefixCase, 6> Cases = {{
      {"bndcl", {0xf3, 0x0f, 0x1a, 0xc0}, X86_INS_BNDCL},
      {"bndcu", {0xf2, 0x0f, 0x1a, 0xc0}, X86_INS_BNDCU},
      {"bndmov_load", {0x66, 0x0f, 0x1a, 0xc0}, X86_INS_BNDMOV},
      {"bndmk", {0xf3, 0x0f, 0x1b, 0x00}, X86_INS_BNDMK},
      {"bndcn", {0xf2, 0x0f, 0x1b, 0xc0}, X86_INS_BNDCN},
      {"bndmov_store", {0x66, 0x0f, 0x1b, 0xc0}, X86_INS_BNDMOV},
  }};

  for (Arch Architecture : {Arch::X86, Arch::X64}) {
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Architecture));
    for (const PrefixCase &Test : Cases) {
      SCOPED_TRACE(::testing::Message()
                   << "arch=" << static_cast<unsigned>(Architecture) << " "
                   << Test.Name);
      DecodedInsn Insn{};
      ASSERT_EQ(
          Dec.decodeOne(Test.Bytes.data(), Test.Bytes.size(), kEntry, Insn),
          static_cast<int>(Test.Bytes.size()));
      EXPECT_EQ(Insn.Id, Test.ExpectedId);
    }
  }
}

TEST(LowInstructionBoundary, ReachableMpxHintNopsPreserveCfgBoundaries) {
  const std::vector<uint8_t> Bytes = {
      0x0f, 0x1a, 0x84, 0x08, 0x78, 0x56, 0x34, 0x12, 0x0f, 0x1b, 0xde, 0xc3,
  };

  for (Arch Architecture : {Arch::X86, Arch::X64}) {
    SCOPED_TRACE(::testing::Message()
                 << "arch=" << static_cast<unsigned>(Architecture));
    LowFunc Function =
        buildFunction(Architecture, InstructionMode::Default, Bytes);
    EXPECT_TRUE(Function.hasCompleteLiftCoverage());
    EXPECT_EQ(Function.DecodedInstructionCount, 3u);
    EXPECT_EQ(Function.LiftedInstructionCount, 3u);

    const LowBlock *Entry = findBlock(Function, kEntry);
    ASSERT_NE(Entry, nullptr);
    ASSERT_EQ(Entry->InstructionBoundaries.size(), 3u);
    EXPECT_EQ(Entry->InstructionBoundaries[0].Address, kEntry);
    EXPECT_EQ(Entry->InstructionBoundaries[0].Size, 8u);
    EXPECT_EQ(Entry->InstructionBoundaries[1].Address, kEntry + 8);
    EXPECT_EQ(Entry->InstructionBoundaries[1].Size, 3u);
    EXPECT_EQ(Entry->InstructionBoundaries[2].Address, kEntry + 11);
    EXPECT_EQ(Entry->InstructionBoundaries[2].Size, 1u);

    for (size_t I = 0; I < 2; ++I) {
      const LowInstructionBoundary &Boundary = Entry->InstructionBoundaries[I];
      ASSERT_EQ(Boundary.OpCount, 1u);
      ASSERT_LT(Boundary.FirstOp, Entry->Ops.size());
      EXPECT_EQ(Entry->Ops[Boundary.FirstOp].Opcode, NdOp::NOP);
    }
    EXPECT_FALSE(containsOp(Entry->Ops, NdOp::LOAD));
    EXPECT_FALSE(containsOp(Entry->Ops, NdOp::STORE));
  }
}

TEST(LowInstructionBoundary, UnreachableMpxHintBytesAreNotDecoded) {
  // jmp +3; unreachable 0F 1A /r register form; ret
  const std::vector<uint8_t> Bytes = {0xeb, 0x03, 0x0f, 0x1a, 0xde, 0xc3};
  for (Arch Architecture : {Arch::X86, Arch::X64}) {
    SCOPED_TRACE(::testing::Message()
                 << "arch=" << static_cast<unsigned>(Architecture));
    LowFunc Function =
        buildFunction(Architecture, InstructionMode::Default, Bytes);
    EXPECT_TRUE(Function.hasCompleteLiftCoverage());
    EXPECT_EQ(Function.DecodedInstructionCount, 2u);
    EXPECT_EQ(Function.LiftedInstructionCount, 2u);
    EXPECT_TRUE(Function.DecodeFailureAddresses.empty());

    const LowBlock *Entry = findBlock(Function, kEntry);
    const LowBlock *Target = findBlock(Function, kEntry + 5);
    ASSERT_NE(Entry, nullptr);
    ASSERT_NE(Target, nullptr);
    ASSERT_EQ(Entry->InstructionBoundaries.size(), 1u);
    ASSERT_EQ(Target->InstructionBoundaries.size(), 1u);
    EXPECT_EQ(Entry->InstructionBoundaries.front().Address, kEntry);
    EXPECT_EQ(Target->InstructionBoundaries.front().Address, kEntry + 5);
  }
}

TEST(LowInstructionBoundary, PreservesInstructionSlicesAcrossBlockSplits) {
  // xor eax, eax; je target; nop; target: ret
  LowFunc Function = buildFunction(Arch::X64, InstructionMode::Default,
                                   {0x31, 0xc0, 0x74, 0x01, 0x90, 0xc3});

  const LowBlock *Entry = findBlock(Function, kEntry);
  ASSERT_NE(Entry, nullptr);
  ASSERT_EQ(Entry->InstructionBoundaries.size(), 2u);

  const LowInstructionBoundary &Xor = Entry->InstructionBoundaries[0];
  EXPECT_EQ(Xor.Address, kEntry);
  EXPECT_EQ(Xor.Size, 2u);
  EXPECT_EQ(Xor.FirstOp, 0u);
  EXPECT_GT(Xor.OpCount, 0u);
  EXPECT_EQ(Xor.Mode, InstructionMode::Default);

  const LowInstructionBoundary &Branch = Entry->InstructionBoundaries[1];
  EXPECT_EQ(Branch.Address, kEntry + 2);
  EXPECT_EQ(Branch.Size, 2u);
  EXPECT_EQ(Branch.FirstOp, Xor.FirstOp + Xor.OpCount);
  EXPECT_EQ(Branch.FirstOp + Branch.OpCount, Entry->Ops.size());
  EXPECT_EQ(Branch.Control, LowInstructionControl::Branch);
  EXPECT_TRUE(hasLowInstructionControlFlag(
      Branch.ControlFlags, LowInstructionControlFlag::Conditional));
  ASSERT_TRUE(Branch.Immediate.has_value());
  EXPECT_EQ(*Branch.Immediate, kEntry + 5);

  EXPECT_FALSE(static_cast<bool>(validateLowInstructionBoundaries(
      Function, LowInstructionBoundaryRequirement::Required)));
}

TEST(LowInstructionBoundary, PreservesDirectCallAndEncodedReturnImmediates) {
  // call 0x1008; ret 0.  The return's explicit zero must not collapse into the
  // absence used by the one-byte `ret` encoding.
  LowFunc Function =
      buildFunction(Arch::X64, InstructionMode::Default,
                    {0xe8, 0x03, 0x00, 0x00, 0x00, 0xc2, 0x00, 0x00});

  const LowBlock *Entry = findBlock(Function, kEntry);
  ASSERT_NE(Entry, nullptr);
  ASSERT_EQ(Entry->InstructionBoundaries.size(), 2u);

  const LowInstructionBoundary &Call = Entry->InstructionBoundaries[0];
  EXPECT_EQ(Call.Control, LowInstructionControl::Call);
  EXPECT_TRUE(hasLowInstructionControlFlag(Call.ControlFlags,
                                           LowInstructionControlFlag::Call));
  ASSERT_TRUE(Call.Immediate.has_value());
  EXPECT_EQ(*Call.Immediate, kEntry + 8);

  const LowInstructionBoundary &Return = Entry->InstructionBoundaries[1];
  EXPECT_EQ(Return.Control, LowInstructionControl::Return);
  EXPECT_TRUE(hasLowInstructionControlFlag(Return.ControlFlags,
                                           LowInstructionControlFlag::Return));
  ASSERT_TRUE(Return.Immediate.has_value());
  EXPECT_EQ(*Return.Immediate, 0u);
  EXPECT_FALSE(static_cast<bool>(validateLowInstructionBoundaries(
      Function, LowInstructionBoundaryRequirement::Required)));

  LowFunc Ordinary = buildFunction(Arch::X64, InstructionMode::Default, {0xc3});
  const LowBlock *OrdinaryEntry = findBlock(Ordinary, kEntry);
  ASSERT_NE(OrdinaryEntry, nullptr);
  ASSERT_EQ(OrdinaryEntry->InstructionBoundaries.size(), 1u);
  EXPECT_FALSE(OrdinaryEntry->InstructionBoundaries[0].Immediate.has_value());
}

TEST(LowInstructionBoundary, RetainsTheDecoderModeAcrossSupportedISAs) {
  struct Case {
    Arch Architecture;
    InstructionMode Mode;
    InstructionMode ExpectedMode;
    std::vector<uint8_t> Bytes;
    std::vector<uint16_t> Sizes;
  };
  const std::vector<Case> Cases = {
      {Arch::X64,
       InstructionMode::Default,
       InstructionMode::Default,
       {0x90, 0xc3},
       {1, 1}},
      {Arch::ARM,
       InstructionMode::ARM,
       InstructionMode::ARM,
       {0x00, 0x00, 0xa0, 0xe1, 0x1e, 0xff, 0x2f, 0xe1},
       {4, 4}},
      {Arch::ARM,
       InstructionMode::Default,
       InstructionMode::ARM,
       {0x00, 0x00, 0xa0, 0xe1, 0x1e, 0xff, 0x2f, 0xe1},
       {4, 4}},
      {Arch::ARM,
       InstructionMode::Thumb,
       InstructionMode::Thumb,
       {0x00, 0xbf, 0x70, 0x47},
       {2, 2}},
      {Arch::AArch64,
       InstructionMode::Default,
       InstructionMode::Default,
       {0x1f, 0x20, 0x03, 0xd5, 0xc0, 0x03, 0x5f, 0xd6},
       {4, 4}},
  };

  for (const Case &Test : Cases) {
    LowFunc Function = buildFunction(Test.Architecture, Test.Mode, Test.Bytes);
    const LowBlock *Entry = findBlock(Function, kEntry);
    ASSERT_NE(Entry, nullptr);
    ASSERT_EQ(Entry->InstructionBoundaries.size(), Test.Sizes.size());
    for (size_t I = 0; I < Test.Sizes.size(); ++I) {
      EXPECT_EQ(Entry->InstructionBoundaries[I].Mode, Test.ExpectedMode);
      EXPECT_EQ(Entry->InstructionBoundaries[I].Size, Test.Sizes[I]);
    }
    EXPECT_FALSE(static_cast<bool>(validateLowInstructionBoundaries(
        Function, LowInstructionBoundaryRequirement::Required)));
  }
}

TEST(LowInstructionBoundary, RejectsMixedInstructionModesWithinABlock) {
  LowFunc Function = buildFunction(Arch::X64, InstructionMode::Default,
                                   {0x90, 0xc3}); // nop; ret
  LowBlock *Entry = Function.blockFor(kEntry);
  ASSERT_NE(Entry, nullptr);
  ASSERT_EQ(Entry->InstructionBoundaries.size(), 2u);

  Entry->InstructionBoundaries.back().Mode = InstructionMode::Thumb;
  EXPECT_NE(
      errorText(validateLowInstructionBoundaries(
                    Function, LowInstructionBoundaryRequirement::Required))
          .find("mix instruction modes"),
      std::string::npos);
}

TEST(LowInstructionBoundary, PreservesARMInterworkingTargetMode) {
  struct Case {
    InstructionMode Mode;
    std::vector<uint8_t> Bytes;
    LowInstructionTargetMode TargetMode;
  };
  const std::vector<Case> Cases = {
      {InstructionMode::Default,
       {0x00, 0x00, 0x00, 0xea},
       LowInstructionTargetMode::Preserve},
      {InstructionMode::ARM,
       {0x00, 0x00, 0x00, 0xeb},
       LowInstructionTargetMode::Preserve},
      {InstructionMode::ARM,
       {0x00, 0x00, 0x00, 0xfa},
       LowInstructionTargetMode::Thumb},
      {InstructionMode::ARM,
       {0x10, 0xff, 0x2f, 0xe1},
       LowInstructionTargetMode::FromTargetBit0},
      {InstructionMode::ARM,
       {0x1e, 0xff, 0x2f, 0xe1},
       LowInstructionTargetMode::FromTargetBit0},
      {InstructionMode::ARM,
       {0x30, 0xff, 0x2f, 0xe1},
       LowInstructionTargetMode::FromTargetBit0},
      {InstructionMode::Thumb,
       {0x00, 0xe0},
       LowInstructionTargetMode::Preserve},
      {InstructionMode::Thumb,
       {0x00, 0xf0, 0x00, 0xf8},
       LowInstructionTargetMode::Preserve},
      {InstructionMode::Thumb,
       {0x00, 0xf0, 0x00, 0xe8},
       LowInstructionTargetMode::ARM},
      {InstructionMode::Thumb,
       {0x00, 0x47},
       LowInstructionTargetMode::FromTargetBit0},
      {InstructionMode::Thumb,
       {0x70, 0x47},
       LowInstructionTargetMode::FromTargetBit0},
      {InstructionMode::Thumb,
       {0x80, 0x47},
       LowInstructionTargetMode::FromTargetBit0},
      {InstructionMode::ARM,
       {0x00, 0xf0, 0x90, 0xe5},
       LowInstructionTargetMode::FromTargetBit0},
      {InstructionMode::Thumb,
       {0xd0, 0xf8, 0x00, 0xf0},
       LowInstructionTargetMode::FromTargetBit0},
      {InstructionMode::ARM,
       {0x04, 0xf0, 0x9d, 0xe4},
       LowInstructionTargetMode::FromTargetBit0},
      {InstructionMode::Thumb,
       {0x00, 0xbd},
       LowInstructionTargetMode::FromTargetBit0},
      {InstructionMode::ARM,
       {0x10, 0x80, 0xb0, 0xe8},
       LowInstructionTargetMode::FromTargetBit0},
      {InstructionMode::ARM,
       {0x00, 0xf0, 0xa0, 0xe1},
       LowInstructionTargetMode::FromTargetBit0},
      {InstructionMode::Thumb,
       {0x87, 0x46},
       LowInstructionTargetMode::Preserve},
  };

  for (const Case &Test : Cases) {
    LowFunc Function = buildFunction(Arch::ARM, Test.Mode, Test.Bytes);
    const LowBlock *Entry = findBlock(Function, kEntry);
    ASSERT_NE(Entry, nullptr);
    ASSERT_EQ(Entry->InstructionBoundaries.size(), 1u);
    const LowInstructionBoundary &Boundary =
        Entry->InstructionBoundaries.front();
    EXPECT_EQ(Boundary.Mode, Test.Mode == InstructionMode::Default
                                 ? InstructionMode::ARM
                                 : Test.Mode);
    EXPECT_EQ(Boundary.TargetMode, Test.TargetMode);
    EXPECT_FALSE(static_cast<bool>(validateLowInstructionBoundaries(
        Function, LowInstructionBoundaryRequirement::Required)));
  }
}

TEST(LowInstructionBoundary, ARMPCWritesEmitExplicitControlOps) {
  struct Case {
    InstructionMode Mode;
    std::vector<uint8_t> Bytes;
    NdOp Terminator;
  };
  const std::vector<Case> Cases = {
      {InstructionMode::ARM, {0x00, 0xf0, 0x90, 0xe5}, NdOp::INDIR_BR},
      {InstructionMode::Thumb, {0xd0, 0xf8, 0x00, 0xf0}, NdOp::INDIR_BR},
      {InstructionMode::ARM, {0x10, 0x80, 0xb0, 0xe8}, NdOp::INDIR_BR},
      {InstructionMode::ARM, {0x00, 0xf0, 0xa0, 0xe1}, NdOp::INDIR_BR},
      {InstructionMode::Thumb, {0x87, 0x46}, NdOp::INDIR_BR},
      {InstructionMode::ARM, {0x00, 0xf0, 0xe0, 0xe1}, NdOp::INDIR_BR},
      {InstructionMode::ARM, {0x01, 0xf0, 0x80, 0xe0}, NdOp::INDIR_BR},
      {InstructionMode::ARM, {0x0e, 0xf0, 0xa0, 0xe1}, NdOp::RETURN},
      {InstructionMode::ARM, {0x04, 0xf0, 0x9d, 0xe4}, NdOp::RETURN},
      {InstructionMode::Thumb, {0x00, 0xbd}, NdOp::RETURN},
      {InstructionMode::ARM, {0x00, 0x80, 0xbd, 0xe8}, NdOp::RETURN},
  };

  for (const Case &Test : Cases) {
    const std::vector<LowOp> Ops = liftARMInstruction(Test.Mode, Test.Bytes);
    EXPECT_TRUE(containsOp(Ops, Test.Terminator));
    EXPECT_FALSE(containsOp(
        Ops, Test.Terminator == NdOp::RETURN ? NdOp::INDIR_BR : NdOp::RETURN));
  }
}

TEST(LowInstructionBoundary, ARMPCReadUsesModeSpecificRelocatableAddress) {
  struct Case {
    InstructionMode Mode;
    va_t Address;
    std::vector<uint8_t> Bytes;
    uint64_t ExpectedPC;
  };
  const std::vector<Case> Cases = {
      {InstructionMode::Thumb, 0x1000, {0x00, 0x48}, 0x1004},
      {InstructionMode::Thumb, 0x1002, {0x00, 0x48}, 0x1004},
      {InstructionMode::ARM, 0x1000, {0x00, 0x00, 0x9f, 0xe5}, 0x1008},
  };

  for (const Case &Test : Cases) {
    SCOPED_TRACE(::testing::Message()
                 << (Test.Mode == InstructionMode::Thumb ? "Thumb" : "ARM")
                 << " at 0x" << std::hex << Test.Address);
    const std::vector<LowOp> Ops =
        liftARMInstruction(Test.Mode, Test.Bytes, Test.Address);
    auto PCWrite = std::find_if(Ops.begin(), Ops.end(), [](const LowOp &Op) {
      return Op.Opcode == NdOp::COPY &&
             Op.Output == NdVar::reg(armreg::PC, 4) && Op.NumInputs == 1 &&
             Op.Inputs[0].isConst();
    });
    ASSERT_NE(PCWrite, Ops.end());
    EXPECT_EQ(PCWrite->Inputs[0].Offset, Test.ExpectedPC);
    EXPECT_EQ(PCWrite->Inputs[0].Provenance,
              ConstantAddressProvenance::Address);
  }
}

TEST(LowInstructionBoundary, ARMTableBranchUsesRawPCAndScalarScale) {
  struct Case {
    const char *Name;
    std::vector<uint8_t> Bytes;
    va_t Address;
  };
  const std::vector<Case> Cases = {
      {"tbb", {0xdf, 0xe8, 0x00, 0xf0}, 0x1002},
      {"tbh", {0xdf, 0xe8, 0x10, 0xf0}, 0x1002},
  };

  for (const Case &Test : Cases) {
    SCOPED_TRACE(Test.Name);
    const std::vector<LowOp> Ops =
        liftARMInstruction(InstructionMode::Thumb, Test.Bytes, Test.Address);

    auto PCWrite = std::find_if(Ops.begin(), Ops.end(), [](const LowOp &Op) {
      return Op.Opcode == NdOp::COPY &&
             Op.Output == NdVar::reg(armreg::PC, 4) && Op.NumInputs == 1;
    });
    PCWrite = std::find_if(PCWrite, Ops.end(), [&](const LowOp &Op) {
      return Op.Opcode == NdOp::COPY &&
             Op.Output == NdVar::reg(armreg::PC, 4) && Op.NumInputs == 1 &&
             Op.Inputs[0].isConst() && Op.Inputs[0].Offset == Test.Address + 4;
    });
    ASSERT_NE(PCWrite, Ops.end());
    ASSERT_TRUE(PCWrite->Inputs[0].isConst());
    EXPECT_EQ(PCWrite->Inputs[0].Offset, 0x1006u);
    EXPECT_EQ(PCWrite->Inputs[0].Provenance,
              ConstantAddressProvenance::Address);

    auto Scale = std::find_if(Ops.begin(), Ops.end(), [](const LowOp &Op) {
      return Op.Opcode == NdOp::INT_LEFT && Op.NumInputs == 2 &&
             Op.Inputs[1].isConst() && Op.Inputs[1].Offset == 1 &&
             Op.Inputs[1].Size == 4;
    });
    ASSERT_NE(Scale, Ops.end());
    EXPECT_EQ(Scale->Inputs[1].Provenance, ConstantAddressProvenance::Scalar);
  }
}

TEST(LowInstructionBoundary, X86EIPRelativeAddressWrapsBeforeZeroExtension) {
  constexpr va_t HighAddress = 0x100001000ULL;
  struct Case {
    std::vector<uint8_t> Bytes;
    va_t ExpectedTarget;
    uint64_t ExpectedTaggedLeaf;
    ConstantAddressProvenance ExpectedProvenance;
  };
  const std::vector<Case> Cases = {
      {{0x67, 0x48, 0x8d, 0x05, 0xf9, 0xff, 0xff, 0xff},
       0x1001,
       0x1001,
       ConstantAddressProvenance::Address},
      {{0x48, 0x8d, 0x05, 0xf9, 0xff, 0xff, 0xff},
       HighAddress,
       HighAddress + 7,
       ConstantAddressProvenance::Address},
  };

  for (const Case &Test : Cases) {
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X64, InstructionMode::Default));
    DecodedInsn Insn{};
    ASSERT_EQ(Dec.decodeOneForLift(Test.Bytes.data(), Test.Bytes.size(),
                                   HighAddress, Insn),
              static_cast<int>(Test.Bytes.size()));
    EXPECT_EQ(Dec.pcRelCodeRefTarget(Insn), Test.ExpectedTarget);

    std::vector<LowOp> Ops;
    Dec.liftToLow(Insn, Ops);
    bool SawTaggedLeaf = false;
    for (const LowOp &Op : Ops)
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        SawTaggedLeaf |= Op.Inputs[I].isConst() &&
                         Op.Inputs[I].Provenance == Test.ExpectedProvenance &&
                         Op.Inputs[I].Offset == Test.ExpectedTaggedLeaf;
    EXPECT_TRUE(SawTaggedLeaf);
  }
}

TEST(LowInstructionBoundary, X86I386EffectiveAddressWrapsAtGuestPointerWidth) {
  // mov ecx, dword ptr [eax + 1]
  const std::vector<LowOp> Ops =
      liftX86Instruction(Arch::X86, {0x8b, 0x48, 0x01});

  auto BaseCopy = std::find_if(Ops.begin(), Ops.end(), [](const LowOp &Op) {
    return Op.Opcode == NdOp::COPY && Op.Output.Size == 4 &&
           Op.NumInputs == 1 && Op.Inputs[0] == NdVar::reg(x86reg::RAX, 4);
  });
  ASSERT_NE(BaseCopy, Ops.end());

  auto Add = std::find_if(Ops.begin(), Ops.end(), [](const LowOp &Op) {
    return Op.Opcode == NdOp::INT_ADD && Op.Output.Size == 4 &&
           Op.NumInputs == 2 && Op.Inputs[1].isConst() &&
           Op.Inputs[1].Offset == 1 && Op.Inputs[1].Size == 4;
  });
  ASSERT_NE(Add, Ops.end());

  auto Extend = std::find_if(Ops.begin(), Ops.end(), [&](const LowOp &Op) {
    return Op.Opcode == NdOp::INT_ZEXT && Op.Output.Size == 8 &&
           Op.NumInputs == 1 && Op.Inputs[0] == Add->Output;
  });
  ASSERT_NE(Extend, Ops.end());
  EXPECT_TRUE(std::any_of(Ops.begin(), Ops.end(), [&](const LowOp &Op) {
    return Op.Opcode == NdOp::LOAD && Op.NumInputs != 0 &&
           Op.Inputs[0] == Extend->Output;
  }));

  // In particular, EAX=0xffffffff plus one is computed by the i32 ADD and
  // wraps to zero before the internal 8-byte VA representation is formed.
  EXPECT_FALSE(std::any_of(Ops.begin(), Ops.end(), [](const LowOp &Op) {
    for (uint8_t I = 0; I < Op.NumInputs; ++I)
      if (Op.Inputs[I] == NdVar::reg(x86reg::RAX, 8))
        return true;
    return false;
  }));
}

TEST(LowInstructionBoundary, X86AddressOverrideIgnoresTheHighHalfOfRAX) {
  // addr32 mov ecx, dword ptr [eax + 1]
  const std::vector<LowOp> Ops =
      liftX86Instruction(Arch::X64, {0x67, 0x8b, 0x48, 0x01});

  auto Add = std::find_if(Ops.begin(), Ops.end(), [](const LowOp &Op) {
    return Op.Opcode == NdOp::INT_ADD && Op.Output.Size == 4 &&
           Op.NumInputs == 2 && Op.Inputs[1].isConst() &&
           Op.Inputs[1].Offset == 1 && Op.Inputs[1].Size == 4;
  });
  ASSERT_NE(Add, Ops.end());
  EXPECT_TRUE(std::any_of(Ops.begin(), Ops.end(), [](const LowOp &Op) {
    return Op.Opcode == NdOp::COPY && Op.Output.Size == 4 &&
           Op.NumInputs == 1 && Op.Inputs[0] == NdVar::reg(x86reg::RAX, 4);
  }));
  EXPECT_TRUE(std::any_of(Ops.begin(), Ops.end(), [&](const LowOp &Op) {
    return Op.Opcode == NdOp::INT_ZEXT && Op.Output.Size == 8 &&
           Op.NumInputs == 1 && Op.Inputs[0] == Add->Output;
  }));
  EXPECT_FALSE(std::any_of(Ops.begin(), Ops.end(), [](const LowOp &Op) {
    for (uint8_t I = 0; I < Op.NumInputs; ++I)
      if (Op.Inputs[I] == NdVar::reg(x86reg::RAX, 8))
        return true;
    return false;
  }));
}

TEST(LowInstructionBoundary, X86Native64EffectiveAddressKeepsRAXWidth) {
  // mov ecx, dword ptr [rax + 1]
  const std::vector<LowOp> Ops =
      liftX86Instruction(Arch::X64, {0x8b, 0x48, 0x01});

  EXPECT_TRUE(std::any_of(Ops.begin(), Ops.end(), [](const LowOp &Op) {
    return Op.Opcode == NdOp::COPY && Op.Output.Size == 8 &&
           Op.NumInputs == 1 && Op.Inputs[0] == NdVar::reg(x86reg::RAX, 8);
  }));
  EXPECT_TRUE(std::any_of(Ops.begin(), Ops.end(), [](const LowOp &Op) {
    return Op.Opcode == NdOp::INT_ADD && Op.Output.Size == 8 &&
           Op.NumInputs == 2 && Op.Inputs[1].isConst() &&
           Op.Inputs[1].Offset == 1 && Op.Inputs[1].Size == 8;
  }));
}

TEST(LowInstructionBoundary,
     X86RelocatedDisplacementDoesNotTagEqualStoredImmediate) {
  constexpr va_t DataVA = 0x250;
  // movl $0x250, 0x250(%ebx); ret
  //
  // The disp32 and imm32 deliberately have the same bits.  Only the disp32
  // relocation occurrence forms the destination address; the stored value is
  // an ordinary integer and must not inherit DataAddress provenance merely
  // because it is numerically equal to the relocation target.
  const std::vector<uint8_t> Bytes = {0xc7, 0x83, 0x50, 0x02, 0x00, 0x00,
                                      0x50, 0x02, 0x00, 0x00, 0xc3};

  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Mode = InstructionMode::Default;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::ELF;
  Image.Base = 0;
  Image.Entry = kEntry;

  Segment Data;
  Data.VA = 0x200;
  Data.Size = 0x100;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(Data.Size);
  Image.Segments.push_back(std::move(Data));

  Segment Text;
  Text.VA = kEntry;
  Text.Size = Bytes.size();
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = Bytes;
  Image.Segments.push_back(std::move(Text));

  // c7 /0 uses a ModR/M byte at +1, disp32 at +2, and imm32 at +6.
  Image.DataAddressRelocOperands[kEntry + 2] = {DataVA, DataVA, 4, 0x200};

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  CFGBuilder Builder;
  LowFunc Function =
      Builder.build(Image, Dec, kEntry, "relocated_displacement_occurrence");

  const LowBlock *Entry = findBlock(Function, kEntry);
  ASSERT_NE(Entry, nullptr);
  const LowOp *Store = nullptr;
  unsigned TaggedTargetOccurrences = 0;
  for (const LowOp &Op : Entry->Ops) {
    if (Op.Opcode == NdOp::STORE)
      Store = &Op;
    for (uint8_t I = 0; I < Op.NumInputs; ++I)
      if (Op.Inputs[I].isConst() &&
          (Op.Inputs[I].Offset & uint64_t{0xffffffff}) == DataVA &&
          Op.Inputs[I].Provenance == ConstantAddressProvenance::DataAddress)
        ++TaggedTargetOccurrences;
  }

  ASSERT_NE(Store, nullptr);
  ASSERT_GE(Store->NumInputs, 2u);
  EXPECT_TRUE(Store->Inputs[1].isConst());
  EXPECT_EQ(Store->Inputs[1].Offset, DataVA);
  EXPECT_EQ(Store->Inputs[1].Provenance, ConstantAddressProvenance::Unknown);
  EXPECT_EQ(TaggedTargetOccurrences, 1u);
}

TEST(LowInstructionBoundary,
     X86BaseLessIndexedDisplacementIsScalarWithoutExactRelocation) {
  // lea 0x2b(,%r8,4), %esi
  //
  // The displacement is one arithmetic component, not a complete address
  // occurrence.  In a low-VA object 0x2b may also be a lifted block start;
  // tagging this leaf Address would let numeric equality turn an index formula
  // into a BlockAddress.  A loader descriptor still overrides this default for
  // a genuine absolute table/global relocation.
  const std::vector<uint8_t> Bytes = {0x42, 0x8d, 0x34, 0x85,
                                      0x2b, 0x00, 0x00, 0x00};
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kEntry, Insn),
            static_cast<int>(Bytes.size()));

  auto displacementProvenance =
      [&](llvm::ArrayRef<RelocatedAddressOperand> Relocs) {
        std::vector<LowOp> Ops;
        Dec.liftToLow(Insn, Ops, Relocs);
        std::optional<ConstantAddressProvenance> Result;
        for (const LowOp &Op : Ops)
          for (uint8_t I = 0; I < Op.NumInputs; ++I)
            if (Op.Inputs[I].isConst() && Op.Inputs[I].Offset == 0x2b &&
                Op.Inputs[I].Size == 8)
              Result = Op.Inputs[I].Provenance;
        return Result;
      };

  auto Plain = displacementProvenance({});
  ASSERT_TRUE(Plain.has_value());
  EXPECT_EQ(*Plain, ConstantAddressProvenance::Scalar);

  RelocatedAddressOperand Reloc;
  Reloc.FieldVA = kEntry + 4;
  Reloc.EncodedValue = 0x2b;
  Reloc.TargetVA = 0x2b;
  Reloc.Width = 4;
  Reloc.Provenance = ConstantAddressProvenance::DataAddress;
  auto Relocated = displacementProvenance(
      llvm::ArrayRef<RelocatedAddressOperand>(&Reloc, 1));
  ASSERT_TRUE(Relocated.has_value());
  EXPECT_EQ(*Relocated, ConstantAddressProvenance::DataAddress);
}

TEST(LowInstructionBoundary,
     X86RelocatedDisplacementPreservesZeroAndUnsignedHighBitTargets) {
  struct Case {
    uint32_t Encoded;
    std::vector<uint8_t> Bytes;
  };
  const std::vector<Case> Cases = {
      {0, {0x8b, 0x83, 0x00, 0x00, 0x00, 0x00}},
      {0x80000000u, {0x8b, 0x83, 0x00, 0x00, 0x00, 0x80}},
  };

  for (const Case &C : Cases) {
    SCOPED_TRACE(C.Encoded);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X86));
    DecodedInsn Insn{};
    ASSERT_EQ(
        Dec.decodeOneForLift(C.Bytes.data(), C.Bytes.size(), kEntry, Insn),
        static_cast<int>(C.Bytes.size()));

    RelocatedAddressOperand Reloc;
    Reloc.FieldVA = kEntry + 2;
    Reloc.EncodedValue = C.Encoded;
    Reloc.TargetVA = C.Encoded;
    Reloc.Width = 4;
    Reloc.Provenance = ConstantAddressProvenance::DataAddress;
    std::vector<LowOp> Ops;
    Dec.liftToLow(Insn, Ops,
                  llvm::ArrayRef<RelocatedAddressOperand>(&Reloc, 1));

    unsigned Matches = 0;
    for (const LowOp &Op : Ops)
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        if (Op.Inputs[I].isConst() &&
            Op.Inputs[I].Provenance == ConstantAddressProvenance::DataAddress) {
          EXPECT_EQ(Op.Inputs[I].Offset, uint64_t(C.Encoded));
          ++Matches;
        }
    EXPECT_EQ(Matches, 1u);
  }
}

TEST(LowInstructionBoundary, X86ConflictingRelocationOwnersFailClosed) {
  // mov 0x20(%rbx), %eax
  const std::vector<uint8_t> Bytes = {0x8b, 0x43, 0x20};
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kEntry, Insn),
            static_cast<int>(Bytes.size()));

  const RelocatedAddressOperand Relocs[] = {
      {kEntry + 2, 0x20, 0x20, 1, ConstantAddressProvenance::DataAddress},
      {kEntry + 2, 0x20, 0x20, 1, ConstantAddressProvenance::CodeAddress},
  };
  std::vector<LowOp> Ops;
  EXPECT_THROW(Dec.liftToLow(Insn, Ops, Relocs), UnliftedInstruction);
  EXPECT_TRUE(Ops.empty());
}

TEST(LowInstructionBoundary, X86GeneratedShiftMaskIsAlwaysScalar) {
  struct Case {
    const char *Name;
    std::vector<uint8_t> Bytes;
  };
  const std::vector<Case> Cases = {
      {"shl", {0x48, 0xd3, 0xe0}},
      {"rol", {0x48, 0xd3, 0xc0}},
      {"shld", {0x48, 0x0f, 0xa5, 0xd8}},
      {"shrd", {0x48, 0x0f, 0xad, 0xd8}},
      {"rcr", {0x48, 0xd3, 0xd8}},
      {"rcl", {0x48, 0xd3, 0xd0}},
      {"rorx", {0xc4, 0xe3, 0xfb, 0xf0, 0xc3, 0x07}},
      {"shrx", {0xc4, 0xe2, 0xf3, 0xf7, 0xc3}},
  };

  for (const Case &C : Cases) {
    SCOPED_TRACE(C.Name);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X64));
    DecodedInsn Insn{};
    ASSERT_EQ(
        Dec.decodeOneForLift(C.Bytes.data(), C.Bytes.size(), kEntry, Insn),
        static_cast<int>(C.Bytes.size()));

    std::vector<LowOp> Ops;
    Dec.liftToLow(Insn, Ops);
    bool SawCountMask = false;
    for (const LowOp &Op : Ops)
      for (uint8_t I = 0; I < Op.NumInputs; ++I) {
        const NdVar &Input = Op.Inputs[I];
        if (!Input.isConst())
          continue;
        EXPECT_EQ(Input.Provenance, ConstantAddressProvenance::Scalar);
        if (Op.Opcode == NdOp::INT_AND && Input.Offset == 0x3f &&
            Input.Size == 8)
          SawCountMask = true;
      }
    EXPECT_TRUE(SawCountMask);
  }
}

TEST(LowInstructionBoundary, X86GeneratedArithmeticConstantsAreScalar) {
  // incq %rax synthesizes a pointer-width `1` for the add and its flags.  In a
  // low-VA object another function may genuinely live at VA 1; the generated
  // increment must not inherit that function's value-global CodeRef identity.
  const std::vector<uint8_t> Bytes = {0x48, 0xff, 0xc0};
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kEntry, Insn),
            static_cast<int>(Bytes.size()));

  std::vector<LowOp> Ops;
  Dec.liftToLow(Insn, Ops);
  bool SawPointerWidthOne = false;
  for (const LowOp &Op : Ops)
    for (uint8_t I = 0; I < Op.NumInputs; ++I) {
      const NdVar &Input = Op.Inputs[I];
      if (!Input.isConst())
        continue;
      EXPECT_EQ(Input.Provenance, ConstantAddressProvenance::Scalar);
      SawPointerWidthOne |= Input.Offset == 1 && Input.Size == 8;
    }
  EXPECT_TRUE(SawPointerWidthOne);
}

TEST(LowInstructionBoundary,
     NumericLowIROperandsDefaultToScalarWithoutErasingExactProvenance) {
  std::vector<LowOp> Ops;
  LiftStateBase State(kEntry, 4, Ops);

  State.emit(NdOp::INT_AND, NdVar::tmp(0, 4),
             {NdVar::reg(0, 4), NdVar::cst(0xff, 4)});
  State.emit(NdOp::INT_LEFT, NdVar::tmp(1, 4),
             {NdVar::reg(4, 4), NdVar::cst(31, 4)});
  State.emit(NdOp::INT_ADD, NdVar::tmp(2, 8),
             {NdVar::reg(8, 8), NdVar::dataAddress(0x2000, 8)});
  State.emit(NdOp::COPY, NdVar::tmp(3, 8), {NdVar::cst(0x3000, 8)});

  ASSERT_EQ(Ops.size(), 4u);
  EXPECT_EQ(Ops[0].Inputs[1].Provenance, ConstantAddressProvenance::Scalar);
  EXPECT_EQ(Ops[1].Inputs[1].Provenance, ConstantAddressProvenance::Scalar);
  EXPECT_EQ(Ops[2].Inputs[1].Provenance,
            ConstantAddressProvenance::DataAddress);
  EXPECT_EQ(Ops[3].Inputs[0].Provenance, ConstantAddressProvenance::Unknown);
}

TEST(LowInstructionBoundary,
     CopyPropagatedImmediatesAcquireTheNumericUseRoleOnlyAtThatUse) {
  LowFunc Low;
  Low.Entry = kEntry;
  Low.Name = "copy_propagated_numeric_occurrence";

  LowBlock Block;
  Block.Id = 0;
  Block.StartAddr = kEntry;
  Block.EndAddr = kEntry + 0x10;

  const NdVar UnknownValue = NdVar::tmp(0, 4);
  LowOp MaterializeUnknown;
  MaterializeUnknown.Opcode = NdOp::COPY;
  MaterializeUnknown.Addr = kEntry;
  MaterializeUnknown.Output = UnknownValue;
  MaterializeUnknown.addInput(NdVar::cst(0x83, 4));
  Block.Ops.push_back(std::move(MaterializeUnknown));

  LowOp NumericUse;
  NumericUse.Opcode = NdOp::INT_ADD;
  NumericUse.Addr = kEntry + 4;
  NumericUse.Output = NdVar::tmp(8, 4);
  NumericUse.addInput(NdVar::reg(0, 4));
  NumericUse.addInput(UnknownValue);
  Block.Ops.push_back(std::move(NumericUse));

  const NdVar ExactAddress = NdVar::tmp(16, 4);
  LowOp MaterializeAddress;
  MaterializeAddress.Opcode = NdOp::COPY;
  MaterializeAddress.Addr = kEntry + 8;
  MaterializeAddress.Output = ExactAddress;
  MaterializeAddress.addInput(NdVar::dataAddress(0x83, 4, 0x80));
  Block.Ops.push_back(std::move(MaterializeAddress));

  LowOp AddressUse;
  AddressUse.Opcode = NdOp::INT_ADD;
  AddressUse.Addr = kEntry + 0xc;
  AddressUse.Output = NdVar::tmp(24, 4);
  AddressUse.addInput(NdVar::reg(4, 4));
  AddressUse.addInput(ExactAddress);
  Block.Ops.push_back(std::move(AddressUse));
  Low.Blocks.push_back(std::move(Block));

  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Format = BinaryFormat::ELF;
  Image.Bits = Bitness::Bits32;
  Segment Data;
  Data.Name = ".rodata";
  Data.VA = 0x80;
  Data.Size = 0x20;
  Data.FileSz = Data.Size;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(Data.Size);
  Image.Segments.push_back(std::move(Data));
  Image.RelocDataAddrs.insert(0x83);

  LowToMedConverter Converter;
  Converter.setBinaryImage(&Image);
  MedFunc Med = Converter.convert(Low, Arch::X86, BinaryFormat::ELF);
  ASSERT_EQ(Med.Blocks.size(), 1u);

  auto inputAt = [&](va_t Addr) -> const MedVar * {
    for (const MedOp &Op : Med.Blocks.front().Ops)
      if (Op.Addr == Addr && Op.Opcode == NdOp::INT_ADD && Op.NumInputs == 2)
        return &Op.Inputs[1];
    return nullptr;
  };
  const MedVar *ScalarUse = inputAt(kEntry + 4);
  const MedVar *RelocatableUse = inputAt(kEntry + 0xc);
  ASSERT_NE(ScalarUse, nullptr);
  ASSERT_NE(RelocatableUse, nullptr);
  ASSERT_TRUE(ScalarUse->isConst());
  ASSERT_TRUE(RelocatableUse->isConst());
  EXPECT_EQ(ScalarUse->Provenance, ConstantAddressProvenance::Scalar);
  EXPECT_EQ(RelocatableUse->Provenance, ConstantAddressProvenance::DataAddress);
}

TEST(LowInstructionBoundary, X86GeneratedStringAndLoopConstantsAreScalar) {
  struct Case {
    const char *Name;
    std::vector<uint8_t> Bytes;
    NdOp Opcode;
    uint64_t Value;
  };
  const std::vector<Case> Cases = {
      // rep lodsq: the hardware intrinsic owns the final load; RCX*8 advances
      // RSI and its generated element-size scale must remain scalar.
      {"rep-lods", {0xf3, 0x48, 0xad}, NdOp::INT_MULT, 8},
      // loop $-2: the encoded branch target remains a control-flow address,
      // but the lifter-generated RCX decrement must be a scalar occurrence.
      {"loop", {0xe2, 0xfe}, NdOp::INT_SUB, 1},
  };

  for (const Case &C : Cases) {
    SCOPED_TRACE(C.Name);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X64));
    DecodedInsn Insn{};
    ASSERT_EQ(
        Dec.decodeOneForLift(C.Bytes.data(), C.Bytes.size(), kEntry, Insn),
        static_cast<int>(C.Bytes.size()));

    std::vector<LowOp> Ops;
    Dec.liftToLow(Insn, Ops);
    bool SawGeneratedScalar = false;
    for (const LowOp &Op : Ops) {
      if (Op.Opcode != C.Opcode)
        continue;
      for (uint8_t I = 0; I < Op.NumInputs; ++I) {
        const NdVar &Input = Op.Inputs[I];
        if (!Input.isConst() || Input.Offset != C.Value || Input.Size != 8)
          continue;
        EXPECT_EQ(Input.Provenance, ConstantAddressProvenance::Scalar);
        SawGeneratedScalar = true;
      }
    }
    EXPECT_TRUE(SawGeneratedScalar);
  }
}

TEST(LowInstructionBoundary,
     X86VSIBGatherConsumesTheExactRelocatedDisplacement) {
  // vpgatherdd %ymm2,0x11223344(,%ymm1,4),%ymm0.  The gather constructs its
  // lane addresses directly instead of calling computeEA, but it must consume
  // the same exact relocation descriptor (including a relocated zero) as an
  // ordinary memory operand.  Its lane scale remains scalar.
  const std::vector<uint8_t> Bytes = {0xc4, 0xe2, 0x6d, 0x90, 0x04,
                                      0x8d, 0x44, 0x33, 0x22, 0x11};
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kEntry, Insn),
            static_cast<int>(Bytes.size()));

  RelocatedAddressOperand Reloc;
  Reloc.FieldVA = kEntry + 6;
  Reloc.EncodedValue = 0x11223344;
  Reloc.TargetVA = 0x11223344;
  Reloc.Width = 4;
  Reloc.Provenance = ConstantAddressProvenance::DataAddress;
  std::vector<LowOp> Ops;
  Dec.liftToLow(Insn, Ops, llvm::ArrayRef<RelocatedAddressOperand>(&Reloc, 1));

  unsigned AddressMatches = 0;
  bool SawScalarScale = false;
  for (const LowOp &Op : Ops)
    for (uint8_t I = 0; I < Op.NumInputs; ++I) {
      const NdVar &Input = Op.Inputs[I];
      if (!Input.isConst())
        continue;
      if (Input.Offset == Reloc.TargetVA && Input.Size == 8) {
        EXPECT_EQ(Input.Provenance, ConstantAddressProvenance::DataAddress);
        ++AddressMatches;
      }
      if (Op.Opcode == NdOp::INT_MULT && Input.Offset == 4 && Input.Size == 8) {
        EXPECT_EQ(Input.Provenance, ConstantAddressProvenance::Scalar);
        SawScalarScale = true;
      }
    }
  EXPECT_EQ(AddressMatches, 1u);
  EXPECT_TRUE(SawScalarScale);
}

TEST(LowInstructionBoundary,
     ArithmeticImmediatesAreScalarUnlessTheirExactFieldRelocates) {
  {
    // cmp rax,0x2000.  A large low-VA object can contain address 0x2000, but
    // this unrelocated encoded bound is still a scalar occurrence.
    const std::vector<uint8_t> Bytes = {0x48, 0x3d, 0x00, 0x20, 0x00, 0x00};
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X64));
    DecodedInsn Insn{};
    ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kEntry, Insn),
              static_cast<int>(Bytes.size()));

    std::vector<LowOp> Ops;
    Dec.liftToLow(Insn, Ops);
    bool SawScalarBound = false;
    for (const LowOp &Op : Ops)
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        if (Op.Inputs[I].isConst() && Op.Inputs[I].Offset == 0x2000) {
          EXPECT_EQ(Op.Inputs[I].Provenance, ConstantAddressProvenance::Scalar);
          SawScalarBound = true;
        }
    EXPECT_TRUE(SawScalarBound);

    // The same bytes with a loader-authenticated relocation on the exact imm32
    // field must retain address identity.  A value-equal constant elsewhere in
    // the instruction would not match FieldVA and therefore cannot inherit it.
    RelocatedAddressOperand Reloc;
    Reloc.FieldVA = kEntry + 2;
    Reloc.EncodedValue = 0x2000;
    Reloc.TargetVA = 0x2000;
    Reloc.Width = 4;
    Reloc.Provenance = ConstantAddressProvenance::DataAddress;
    Ops.clear();
    Dec.liftToLow(Insn, Ops,
                  llvm::ArrayRef<RelocatedAddressOperand>(&Reloc, 1));
    bool SawRelocatedBound = false;
    for (const LowOp &Op : Ops)
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        if (Op.Inputs[I].isConst() && Op.Inputs[I].Offset == 0x2000) {
          EXPECT_EQ(Op.Inputs[I].Provenance,
                    ConstantAddressProvenance::DataAddress);
          SawRelocatedBound = true;
        }
    EXPECT_TRUE(SawRelocatedBound);
  }

  {
    // cmp r0,#0x100.  In a relocatable ARM object 0x100 can simultaneously be
    // the .bss base; the compare occurrence must remain a scalar bound.
    const std::vector<LowOp> Ops =
        liftARMInstruction(InstructionMode::ARM, {0x01, 0x0c, 0x50, 0xe3});
    bool SawScalarBound = false;
    for (const LowOp &Op : Ops)
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        if (Op.Inputs[I].isConst() && Op.Inputs[I].Offset == 0x100) {
          EXPECT_EQ(Op.Inputs[I].Provenance, ConstantAddressProvenance::Scalar);
          SawScalarBound = true;
        }
    EXPECT_TRUE(SawScalarBound);
  }
}

TEST(LowInstructionBoundary, AArch64AddressDisplacementsCarryScalarProvenance) {
  struct Case {
    std::vector<uint8_t> Bytes;
    uint64_t Displacement;
  };
  // add x12,x12,#0x248; str x11,[sp,#16]. The first value deliberately
  // matches the low-VA table_b address in the issue fixture: its instruction
  // role, not numeric segment membership, proves that it is a displacement.
  const std::vector<Case> Cases = {
      {{0x8c, 0x21, 0x09, 0x91}, 0x248},
      {{0xeb, 0x0b, 0x00, 0xf9}, 16},
  };

  for (const Case &Test : Cases) {
    const std::vector<LowOp> Ops = liftAArch64Instruction(Test.Bytes);
    bool SawScalarDisplacement = false;
    for (const LowOp &Op : Ops)
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        SawScalarDisplacement |=
            Op.Inputs[I].isConst() &&
            Op.Inputs[I].Offset == Test.Displacement &&
            Op.Inputs[I].Provenance == ConstantAddressProvenance::Scalar;
    EXPECT_TRUE(SawScalarDisplacement);
  }
}

TEST(LowInstructionBoundary,
     AArch64BarePageBaseCompletesOnlyForExactDereference) {
  // adrp x8,0x9000; str x8,[sp,#0x18]; ldr x8,[x8]; ret
  //
  // The page base is itself the complete address of the pointer table.  It is
  // spilled as a first-class pointer and dereferenced before x8 is redefined,
  // so retaining AddressFragment would later reject the stack reload even
  // though this exact instruction occurrence proved the zero page offset.
  LowFunc Function =
      buildAArch64PageBaseUse({0x48, 0x00, 0x00, 0x90, 0xe8, 0x0f, 0x00, 0xf9,
                               0x08, 0x01, 0x40, 0xf9, 0xc0, 0x03, 0x5f, 0xd6});

  const LowOp *Materialization = findAddressMaterialization(Function, kEntry);
  ASSERT_NE(Materialization, nullptr);
  ASSERT_EQ(Materialization->Inputs[0].Offset, 0x9000u);
  EXPECT_EQ(Materialization->Inputs[0].Provenance,
            ConstantAddressProvenance::DataAddress);
  EXPECT_EQ(Materialization->Inputs[0].AddressOwnerVA, 0x9000u);
}

TEST(LowInstructionBoundary,
     AArch64UnsignedOffsetLoadPublishesExactRelocationFreeOccurrence) {
  const LowFunc Function = buildAArch64RelocationFreeOffsetLoad();
  std::vector<const RelocatedInstructionAddressOccurrence *> Found;
  for (const RelocatedInstructionAddressOccurrence &Occurrence :
       Function.RelocatedInstructionAddressOccurrences)
    if (Occurrence.TargetVA == 0x9010)
      Found.push_back(&Occurrence);

  ASSERT_EQ(Found.size(), 1u);
  const RelocatedInstructionAddressOccurrence &Occurrence = *Found.front();
  EXPECT_EQ(Occurrence.Authority, RelocatedInstructionAddressProofKind::
                                      AArch64RelocationFreeDataDereference);
  EXPECT_EQ(Occurrence.FieldVA, InvalidVA);
  EXPECT_TRUE(Occurrence.DefinesOutput);
  EXPECT_FALSE(Occurrence.OutputMayDepend);
  EXPECT_EQ(Occurrence.Provenance, ConstantAddressProvenance::DataAddress);
  EXPECT_EQ(Occurrence.TargetOwnerVA, 0x9000u);
  EXPECT_EQ(Occurrence.InstructionAddr, kEntry + 4);
  EXPECT_EQ(Occurrence.DereferenceInstructionAddr, kEntry + 4);
  EXPECT_EQ(Occurrence.DereferenceOpcode, NdOp::LOAD);
  EXPECT_EQ(Occurrence.DereferenceAccessSize, 8u);
}

TEST(LowInstructionBoundary,
     AArch64UnsignedOffsetLoadIntoWritableDataPublishesNoOccurrence) {
  const LowFunc Function = buildAArch64RelocationFreeOffsetLoad(
      /*WritableData=*/true);
  EXPECT_TRUE(
      std::none_of(Function.RelocatedInstructionAddressOccurrences.begin(),
                   Function.RelocatedInstructionAddressOccurrences.end(),
                   [](const RelocatedInstructionAddressOccurrence &Occurrence) {
                     return Occurrence.TargetVA == 0x9010;
                   }));
}

TEST(LowInstructionBoundary,
     AArch64PageStartCollisionDoesNotCompleteOffsetAddress) {
  // adrp x8,0x9000; add x8,x8,#0x20; ldr x8,[x8]; ret
  //
  // A real pointer slot also begins at 0x9000, but this occurrence addresses
  // 0x9020.  Numeric equality with the unrelated page-start slot must not turn
  // the incomplete ADRP result into that slot's relocatable identity.
  LowFunc Function =
      buildAArch64PageBaseUse({0x48, 0x00, 0x00, 0x90, 0x08, 0x81, 0x00, 0x91,
                               0x08, 0x01, 0x40, 0xf9, 0xc0, 0x03, 0x5f, 0xd6});

  const LowOp *Materialization = findAddressMaterialization(Function, kEntry);
  ASSERT_NE(Materialization, nullptr);
  ASSERT_EQ(Materialization->Inputs[0].Offset, 0x9000u);
  EXPECT_EQ(Materialization->Inputs[0].Provenance,
            ConstantAddressProvenance::AddressFragment);
  EXPECT_EQ(Materialization->Inputs[0].AddressOwnerVA, InvalidVA);
}

TEST(LowInstructionBoundary, ARMExceptionReturnsFailClosed) {
  const std::vector<std::vector<uint8_t>> Cases = {
      {0x04, 0xf0, 0x5e, 0xe2}, // subs pc, lr, #4
      {0x0e, 0xf0, 0xb0, 0xe1}, // movs pc, lr
      {0x00, 0x80, 0xfd, 0xe8}, // ldm sp!, {pc} ^
      {0x00, 0x0a, 0xbd, 0xf8}, // rfeia sp!
      {0x6e, 0x00, 0x60, 0xe1}, // eret
  };

  for (size_t CaseIndex = 0; CaseIndex < Cases.size(); ++CaseIndex) {
    SCOPED_TRACE(::testing::Message() << "case " << CaseIndex);
    const std::vector<uint8_t> &Bytes = Cases[CaseIndex];
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::ARM, InstructionMode::ARM));
    DecodedInsn Insn{};
    ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kEntry, Insn),
              static_cast<int>(Bytes.size()));
    std::vector<LowOp> Ops;
    EXPECT_THROW(Dec.liftToLow(Insn, Ops), UnliftedInstruction);
  }
}

TEST(LowInstructionBoundary, ARMTerminatorClassificationUsesOperands) {
  struct Case {
    InstructionMode Mode;
    std::vector<uint8_t> Bytes;
    bool IsTerminator;
  };
  const std::vector<Case> Cases = {
      {InstructionMode::Thumb, {0x10, 0xbc}, false}, // pop {r4}
      {InstructionMode::Thumb, {0x00, 0xbd}, true},  // pop {pc}
      {InstructionMode::ARM, {0x10, 0x00, 0xbd, 0xe8}, false},
      {InstructionMode::ARM, {0x10, 0x80, 0xb0, 0xe8}, true},
      {InstructionMode::ARM, {0x00, 0x80, 0xbd, 0x08}, false},
  };

  for (const Case &Test : Cases) {
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::ARM, Test.Mode));
    DecodedInsn Insn{};
    ASSERT_EQ(Dec.decodeOneForLift(Test.Bytes.data(), Test.Bytes.size(), kEntry,
                                   Insn),
              static_cast<int>(Test.Bytes.size()));
    EXPECT_EQ(Dec.isFunctionTerminator(Insn), Test.IsTerminator);
  }
}

TEST(LowInstructionBoundary, ARMNonPCPopDoesNotTerminateDecode) {
  // pop {r4}; bx lr
  LowFunc Function = buildFunction(Arch::ARM, InstructionMode::Thumb,
                                   {0x10, 0xbc, 0x70, 0x47});

  const LowBlock *Entry = findBlock(Function, kEntry);
  ASSERT_NE(Entry, nullptr);
  ASSERT_EQ(Entry->InstructionBoundaries.size(), 2u);
  EXPECT_EQ(Entry->InstructionBoundaries[0].Control,
            LowInstructionControl::None);
  EXPECT_EQ(Entry->InstructionBoundaries[1].Control,
            LowInstructionControl::Return);
  EXPECT_FALSE(static_cast<bool>(validateLowInstructionBoundaries(
      Function, LowInstructionBoundaryRequirement::Required)));
}

TEST(LowInstructionBoundary, ARMConditionalPCWritesKeepFalseFallthrough) {
  struct Case {
    std::vector<uint8_t> Bytes;
    LowInstructionControl Control;
    bool IsIndirect;
  };
  const std::vector<Case> Cases = {
      {{0x00, 0xf0, 0x90, 0x05}, LowInstructionControl::Branch, true},
      {{0x00, 0x80, 0xbd, 0x08},
       LowInstructionControl::ConditionalReturn,
       false},
      {{0x0e, 0xf0, 0xa0, 0x01},
       LowInstructionControl::ConditionalReturn,
       false},
      {{0x01, 0xf0, 0x80, 0x00}, LowInstructionControl::Branch, true},
  };

  for (const Case &Test : Cases) {
    std::vector<uint8_t> Bytes = Test.Bytes;
    Bytes.insert(Bytes.end(), {0x1e, 0xff, 0x2f, 0xe1}); // bx lr
    LowFunc Function =
        buildFunction(Arch::ARM, InstructionMode::ARM, std::move(Bytes));

    const LowBlock *Entry = findBlock(Function, kEntry);
    const LowBlock *Fallthrough = findBlock(Function, kEntry + 4);
    ASSERT_NE(Entry, nullptr);
    ASSERT_NE(Fallthrough, nullptr);
    ASSERT_EQ(Entry->InstructionBoundaries.size(), 1u);
    const LowInstructionBoundary &Boundary =
        Entry->InstructionBoundaries.front();
    EXPECT_EQ(Boundary.Control, Test.Control);
    EXPECT_TRUE(hasLowInstructionControlFlag(
        Boundary.ControlFlags, LowInstructionControlFlag::Conditional));
    EXPECT_TRUE(hasLowInstructionControlFlag(
        Boundary.ControlFlags, LowInstructionControlFlag::InstructionGuard));
    EXPECT_EQ(hasLowInstructionControlFlag(Boundary.ControlFlags,
                                           LowInstructionControlFlag::Indirect),
              Test.IsIndirect);
    EXPECT_EQ(
        std::count(Entry->Succs.begin(), Entry->Succs.end(), Fallthrough->Id),
        1);
    EXPECT_FALSE(static_cast<bool>(validateLowInstructionBoundaries(
        Function, LowInstructionBoundaryRequirement::Required)));
  }
}

TEST(LowInstructionBoundary, ARMConditionalDirectBranchIsNotLocalGuard) {
  // bne +0 (to 0x1008); bx lr; bx lr.  The lifter spells the branch as
  // `COND_BR next,!NE; BRANCH target`, but that pair is the guest control flow
  // itself rather than a same-instruction guard around a memory/call effect.
  LowFunc Function = buildFunction(
      Arch::ARM, InstructionMode::ARM,
      {0x00, 0x00, 0x00, 0x1a, 0x1e, 0xff, 0x2f, 0xe1, 0x1e, 0xff, 0x2f, 0xe1});

  const LowBlock *Entry = findBlock(Function, kEntry);
  ASSERT_NE(Entry, nullptr);
  ASSERT_EQ(Entry->InstructionBoundaries.size(), 1u);
  const LowInstructionBoundary &Boundary = Entry->InstructionBoundaries.front();
  EXPECT_EQ(Boundary.Control, LowInstructionControl::Branch);
  EXPECT_TRUE(hasLowInstructionControlFlag(
      Boundary.ControlFlags, LowInstructionControlFlag::Conditional));
  EXPECT_FALSE(hasLowInstructionControlFlag(
      Boundary.ControlFlags, LowInstructionControlFlag::InstructionGuard));
  EXPECT_EQ(Entry->Succs.size(), 2u);
  EXPECT_FALSE(static_cast<bool>(validateLowInstructionBoundaries(
      Function, LowInstructionBoundaryRequirement::Required)));
}

TEST(LowInstructionBoundary, ARMConditionalMemoryUsesOneInstructionLocalGuard) {
  struct Case {
    std::vector<uint8_t> Bytes;
    NdOp Effect;
  };
  const std::vector<Case> Cases = {
      {{0x00, 0x10, 0x90, 0x05}, NdOp::LOAD},  // ldreq r1, [r0]
      {{0x00, 0x10, 0x80, 0x05}, NdOp::STORE}, // streq r1, [r0]
  };

  for (const Case &Test : Cases) {
    std::vector<uint8_t> Bytes = Test.Bytes;
    Bytes.insert(Bytes.end(), {0x1e, 0xff, 0x2f, 0xe1}); // bx lr
    LowFunc Function =
        buildFunction(Arch::ARM, InstructionMode::ARM, std::move(Bytes));

    const LowBlock *Entry = findBlock(Function, kEntry);
    const LowBlock *Continuation = findBlock(Function, kEntry + 4);
    ASSERT_NE(Entry, nullptr);
    ASSERT_NE(Continuation, nullptr);
    ASSERT_EQ(Entry->InstructionBoundaries.size(), 1u);
    EXPECT_TRUE(hasLowInstructionControlFlag(
        Entry->InstructionBoundaries.front().ControlFlags,
        LowInstructionControlFlag::InstructionGuard));
    EXPECT_TRUE(containsOp(Entry->Ops, NdOp::COND_BR));
    EXPECT_TRUE(containsOp(Entry->Ops, Test.Effect));
    EXPECT_FALSE(containsOp(Entry->Ops, NdOp::SELECT));
    if (Test.Effect == NdOp::STORE)
      EXPECT_FALSE(containsOp(Entry->Ops, NdOp::LOAD))
          << "a skipped store must not use a read-modify-write fallback";

    auto Guard =
        std::find_if(Entry->Ops.begin(), Entry->Ops.end(), [](const LowOp &Op) {
          return Op.Opcode == NdOp::COND_BR;
        });
    auto Effect =
        std::find_if(Entry->Ops.begin(), Entry->Ops.end(),
                     [&](const LowOp &Op) { return Op.Opcode == Test.Effect; });
    ASSERT_NE(Guard, Entry->Ops.end());
    ASSERT_NE(Effect, Entry->Ops.end());
    EXPECT_LT(Guard, Effect);

    ASSERT_EQ(Entry->Succs.size(), 1u);
    EXPECT_EQ(Entry->Succs.front(), Continuation->Id);
    EXPECT_FALSE(static_cast<bool>(validateLowInstructionBoundaries(
        Function, LowInstructionBoundaryRequirement::Required)));
  }
}

TEST(LowInstructionBoundary, RejectsForgedOrMissingInstructionGuardMetadata) {
  LowFunc Function = buildFunction(
      Arch::ARM, InstructionMode::ARM,
      {0x00, 0x10, 0x90, 0x05, 0x1e, 0xff, 0x2f, 0xe1}); // ldreq; bx lr
  LowBlock *Entry = Function.blockFor(kEntry);
  ASSERT_NE(Entry, nullptr);
  ASSERT_EQ(Entry->InstructionBoundaries.size(), 1u);

  LowInstructionBoundary &Boundary = Entry->InstructionBoundaries.front();
  const uint16_t WithoutGuard =
      static_cast<uint16_t>(Boundary.ControlFlags) &
      ~static_cast<uint16_t>(LowInstructionControlFlag::InstructionGuard);
  Boundary.ControlFlags = static_cast<LowInstructionControlFlag>(WithoutGuard);
  EXPECT_NE(errorText(validateLowInstructionBoundaries(
                Function, LowInstructionBoundaryRequirement::Required)),
            "");

  LowFunc Ordinary = buildFunction(
      Arch::ARM, InstructionMode::ARM,
      {0x00, 0x00, 0x00, 0xea, 0x1e, 0xff, 0x2f, 0xe1}); // b; bx lr
  LowBlock *OrdinaryEntry = Ordinary.blockFor(kEntry);
  ASSERT_NE(OrdinaryEntry, nullptr);
  ASSERT_EQ(OrdinaryEntry->InstructionBoundaries.size(), 1u);
  OrdinaryEntry->InstructionBoundaries.front().ControlFlags |=
      LowInstructionControlFlag::InstructionGuard;
  EXPECT_NE(errorText(validateLowInstructionBoundaries(
                Ordinary, LowInstructionBoundaryRequirement::Required)),
            "");
}

TEST(LowInstructionBoundary, ARMPredicatedMemoryIsMaterializedBeforeMedSSA) {
  // ldreq r1, [r0]; mov r0, r1; bx lr.  The use of r1 keeps the effect and its
  // join phi live through the complete LowToMed pipeline.
  LowFunc Low = buildFunction(
      Arch::ARM, InstructionMode::ARM,
      {0x00, 0x10, 0x90, 0x05, 0x01, 0x00, 0xa0, 0xe1, 0x1e, 0xff, 0x2f, 0xe1});
  LowBlock *LowGuard = Low.blockFor(kEntry);
  LowBlock *LowHandler = Low.blockFor(kEntry + 4);
  ASSERT_NE(LowGuard, nullptr);
  ASSERT_NE(LowHandler, nullptr);
  ExceptionalEdge ToHandler;
  ToHandler.BlockId = LowHandler->Id;
  ToHandler.TargetVA = LowHandler->StartAddr;
  ToHandler.Kind = ExceptionalEdgeKind::ItaniumCatchPad;
  LowGuard->ExceptionalSuccs.push_back(ToHandler);
  ExceptionalEdge FromGuard = ToHandler;
  FromGuard.BlockId = LowGuard->Id;
  LowHandler->ExceptionalPreds.push_back(FromGuard);
  MedFunc Med = LowToMedConverter().convert(Low, Arch::ARM, BinaryFormat::ELF);

  const MedBlock *Guard = nullptr;
  for (const MedBlock &Block : Med.Blocks)
    if (std::any_of(Block.Ops.begin(), Block.Ops.end(), [](const MedOp &Op) {
          return Op.Opcode == NdOp::COND_BR;
        })) {
      Guard = &Block;
      break;
    }
  ASSERT_NE(Guard, nullptr);
  ASSERT_EQ(Guard->Succs.size(), 2u);

  const MedBlock &Skip = Med.Blocks[Guard->Succs[0]];
  const MedBlock &Effect = Med.Blocks[Guard->Succs[1]];
  EXPECT_TRUE(Guard->ExceptionalSuccs.empty())
      << "the skipped path must not retain the effect's exception edge";
  ASSERT_EQ(Effect.ExceptionalSuccs.size(), 1u);
  EXPECT_EQ(Effect.ExceptionalSuccs.front().BlockId, LowHandler->Id);
  EXPECT_NE(std::find_if(Skip.ExceptionalPreds.begin(),
                         Skip.ExceptionalPreds.end(),
                         [&](const ExceptionalEdge &Edge) {
                           return Edge.BlockId == Effect.Id;
                         }),
            Skip.ExceptionalPreds.end());
  EXPECT_EQ(std::find_if(Skip.ExceptionalPreds.begin(),
                         Skip.ExceptionalPreds.end(),
                         [&](const ExceptionalEdge &Edge) {
                           return Edge.BlockId == Guard->Id;
                         }),
            Skip.ExceptionalPreds.end());
  EXPECT_TRUE(
      std::any_of(Effect.Ops.begin(), Effect.Ops.end(),
                  [](const MedOp &Op) { return Op.Opcode == NdOp::LOAD; }));
  ASSERT_EQ(Effect.Succs.size(), 1u);
  EXPECT_EQ(Effect.Succs.front(), Skip.Id);
  EXPECT_NE(std::find(Skip.Preds.begin(), Skip.Preds.end(), Guard->Id),
            Skip.Preds.end());
  EXPECT_NE(std::find(Skip.Preds.begin(), Skip.Preds.end(), Effect.Id),
            Skip.Preds.end());

  EXPECT_TRUE(std::any_of(Skip.Phis.begin(), Skip.Phis.end(),
                          [](const PhiNode &Phi) {
                            return Phi.Output.Kind == MedVar::Reg &&
                                   Phi.Output.RegOff == armreg::R1;
                          }))
      << "the skip edge must retain old r1 while the effect edge defines it";
  EXPECT_TRUE(verifyMedFunc(Med, "test-predicated-memory-materialization"));
}

TEST(LowInstructionBoundary, ConditionalIndirectBranchKeepsFallthrough) {
  // bxeq r0; bx lr
  LowFunc Function =
      buildFunction(Arch::ARM, InstructionMode::Default,
                    {0x10, 0xff, 0x2f, 0x01, 0x1e, 0xff, 0x2f, 0xe1});

  const LowBlock *Entry = findBlock(Function, kEntry);
  const LowBlock *Fallthrough = findBlock(Function, kEntry + 4);
  ASSERT_NE(Entry, nullptr);
  ASSERT_NE(Fallthrough, nullptr);
  EXPECT_TRUE(Entry->hasSucc(Fallthrough->Id));
  ASSERT_EQ(Entry->InstructionBoundaries.size(), 1u);
  EXPECT_EQ(Entry->InstructionBoundaries.front().TargetMode,
            LowInstructionTargetMode::FromTargetBit0);
  EXPECT_FALSE(static_cast<bool>(validateLowInstructionBoundaries(
      Function, LowInstructionBoundaryRequirement::Required)));
}

TEST(LowInstructionBoundary,
     MemoryIndirectTailCallKeepsInstructionLocalTargetSlice) {
  // jmpq *(%rcx,%rax,8).  An unresolved terminal indirect jump is modeled as
  // an indirect tail call.  Its target LOAD is part of this same instruction;
  // dropping that definition leaves a reusable instruction-local temp whose
  // next occurrence can silently become the call target during SSA renaming.
  LowFunc Function =
      buildFunction(Arch::X64, InstructionMode::Default, {0xff, 0x24, 0xc1});

  const LowBlock *Entry = findBlock(Function, kEntry);
  ASSERT_NE(Entry, nullptr);
  auto Call =
      std::find_if(Entry->Ops.begin(), Entry->Ops.end(), [](const LowOp &Op) {
        return Op.Opcode == NdOp::INDIR_CALL;
      });
  ASSERT_NE(Call, Entry->Ops.end());
  ASSERT_EQ(Call->NumInputs, 1u);
  EXPECT_TRUE(Call->Inputs[0].isTemp());

  auto TargetLoad =
      std::find_if(Entry->Ops.begin(), Call, [&](const LowOp &Op) {
        return Op.Opcode == NdOp::LOAD &&
               Op.Output.Space == Call->Inputs[0].Space &&
               Op.Output.Offset == Call->Inputs[0].Offset &&
               Op.Output.Size == Call->Inputs[0].Size;
      });
  ASSERT_NE(TargetLoad, Call)
      << "the tail-call target must retain its same-instruction LOAD";
  EXPECT_NE(
      std::find_if(std::next(Call), Entry->Ops.end(),
                   [](const LowOp &Op) { return Op.Opcode == NdOp::RETURN; }),
      Entry->Ops.end());
  EXPECT_FALSE(static_cast<bool>(validateLowInstructionBoundaries(
      Function, LowInstructionBoundaryRequirement::Required)));

  MedFunc Med =
      LowToMedConverter().convert(Function, Arch::X64, BinaryFormat::ELF);
  ASSERT_EQ(Med.Blocks.size(), 1u);
  auto MedCall = std::find_if(
      Med.Blocks[0].Ops.begin(), Med.Blocks[0].Ops.end(),
      [](const MedOp &Op) { return Op.Opcode == NdOp::INDIR_CALL; });
  ASSERT_NE(MedCall, Med.Blocks[0].Ops.end());
  ASSERT_EQ(MedCall->NumInputs, 1u);
  EXPECT_NE(std::find_if(Med.Blocks[0].Ops.begin(), MedCall,
                         [&](const MedOp &Op) {
                           return Op.Opcode == NdOp::LOAD &&
                                  Op.Output.Kind == MedCall->Inputs[0].Kind &&
                                  Op.Output.Id == MedCall->Inputs[0].Id &&
                                  Op.Output.SSAVer == MedCall->Inputs[0].SSAVer;
                         }),
            MedCall);
  EXPECT_TRUE(verifyMedFunc(Med, "memory-indirect-tail-call"));
}

TEST(LowInstructionBoundary,
     RelocationAddressTakenTrivialBlockSurvivesMedSimplification) {
  constexpr va_t TrivialAddress = kEntry + 0x10;
  constexpr va_t TargetAddress = kEntry + 0x20;
  constexpr va_t PointerSlot = 0x2000;

  LowFunc Low;
  Low.Entry = kEntry;
  Low.Name = "address_taken_trivial_block";

  auto makeBranchBlock = [](int Id, va_t Address, int Succ, va_t Target) {
    LowBlock Block;
    Block.Id = Id;
    Block.StartAddr = Address;
    Block.EndAddr = Address + 2;
    Block.Succs = {Succ};
    LowOp Branch;
    Branch.Opcode = NdOp::BRANCH;
    Branch.Addr = Address;
    Branch.addInput(NdVar::cst(Target, 8));
    Block.Ops.push_back(std::move(Branch));
    return Block;
  };
  Low.Blocks.push_back(makeBranchBlock(0, kEntry, 1, TrivialAddress));
  Low.Blocks.push_back(makeBranchBlock(1, TrivialAddress, 2, TargetAddress));
  Low.Blocks[1].Preds = {0};
  LowBlock Target;
  Target.Id = 2;
  Target.StartAddr = TargetAddress;
  Target.EndAddr = TargetAddress + 1;
  Target.Preds = {1};
  LowOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = TargetAddress;
  Target.Ops.push_back(std::move(Return));
  Low.Blocks.push_back(std::move(Target));

  auto convert = [&](bool AddressTaken) {
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Format = BinaryFormat::ELF;
    Image.Bits = Bitness::Bits64;
    Segment Text;
    Text.Name = ".text";
    Text.VA = kEntry;
    Text.Size = 0x40;
    Text.FileSz = Text.Size;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.assign(Text.Size, 0);
    Image.Segments.push_back(std::move(Text));
    Segment Data;
    Data.Name = ".data.rel.ro";
    Data.VA = PointerSlot;
    Data.Size = 8;
    Data.FileSz = Data.Size;
    Data.Flags = SegmentFlags::Readable;
    Data.Data.assign(Data.Size, 0);
    for (unsigned I = 0; I < 8; ++I)
      Data.Data[I] = static_cast<uint8_t>(TrivialAddress >> (I * 8));
    Image.Segments.push_back(std::move(Data));
    if (AddressTaken)
      Image.CodePtrRelocSlots.insert(PointerSlot);

    LowToMedConverter Converter;
    Converter.setBinaryImage(&Image);
    return Converter.convert(Low, Arch::X64, BinaryFormat::ELF);
  };
  auto hasBlockAt = [](const MedFunc &Func, va_t Address) {
    return std::any_of(
        Func.Blocks.begin(), Func.Blocks.end(),
        [&](const MedBlock &Block) { return Block.StartAddr == Address; });
  };

  const MedFunc Unreferenced = convert(false);
  const MedFunc AddressTaken = convert(true);
  EXPECT_FALSE(hasBlockAt(Unreferenced, TrivialAddress));
  EXPECT_TRUE(hasBlockAt(AddressTaken, TrivialAddress));
  EXPECT_TRUE(verifyMedFunc(AddressTaken, "address-taken-trivial-block"));
}

TEST(LowInstructionBoundary, ConditionalNoReturnCallKeepsFalsePath) {
  Symbol Abort = Symbol::makeFunc(kEntry + 0x10);
  Abort.Name = "abort";

  // bleq abort; bx lr
  LowFunc Function = buildFunction(
      Arch::ARM, InstructionMode::Default,
      {0x02, 0x00, 0x00, 0x0b, 0x1e, 0xff, 0x2f, 0xe1}, {std::move(Abort)});

  const LowBlock *Entry = findBlock(Function, kEntry);
  const LowBlock *Fallthrough = findBlock(Function, kEntry + 4);
  ASSERT_NE(Entry, nullptr);
  ASSERT_NE(Fallthrough, nullptr);
  EXPECT_TRUE(Entry->hasSucc(Fallthrough->Id));
  ASSERT_EQ(Entry->InstructionBoundaries.size(), 1u);
  const LowInstructionBoundary &Call = Entry->InstructionBoundaries.front();
  EXPECT_EQ(Call.Control, LowInstructionControl::ConditionalCall);
  EXPECT_TRUE(hasLowInstructionControlFlag(
      Call.ControlFlags, LowInstructionControlFlag::NoReturn));
  EXPECT_FALSE(static_cast<bool>(validateLowInstructionBoundaries(
      Function, LowInstructionBoundaryRequirement::Required)));

  MedFunc Med =
      LowToMedConverter().convert(Function, Arch::ARM, BinaryFormat::ELF);
  const MedBlock *Guard = nullptr;
  for (const MedBlock &Block : Med.Blocks)
    if (std::any_of(Block.Ops.begin(), Block.Ops.end(), [](const MedOp &Op) {
          return Op.Opcode == NdOp::COND_BR;
        })) {
      Guard = &Block;
      break;
    }
  ASSERT_NE(Guard, nullptr);
  ASSERT_EQ(Guard->Succs.size(), 2u);
  const MedBlock &Taken = Med.Blocks[Guard->Succs[1]];
  EXPECT_TRUE(Taken.Succs.empty())
      << "a selected no-return call must not rejoin its false continuation";
  EXPECT_TRUE(
      std::any_of(Taken.Ops.begin(), Taken.Ops.end(), [](const MedOp &Op) {
        return Op.Opcode == NdOp::CALL && Op.DoesNotReturn;
      }));
  EXPECT_TRUE(verifyMedFunc(Med, "test-predicated-no-return-call"));
}

TEST(LowInstructionBoundary, CrossChecksControlAgainstLowOpSlice) {
  LowFunc Function = buildFunction(Arch::X64, InstructionMode::Default,
                                   {0xe8, 0x03, 0x00, 0x00, 0x00, 0xc3});
  LowBlock *Entry = Function.blockFor(kEntry);
  ASSERT_NE(Entry, nullptr);
  ASSERT_FALSE(Entry->InstructionBoundaries.empty());
  LowInstructionBoundary &Call = Entry->InstructionBoundaries.front();
  ASSERT_TRUE(Call.Immediate.has_value());

  ++*Call.Immediate;
  EXPECT_NE(
      errorText(validateLowInstructionBoundaries(
                    Function, LowInstructionBoundaryRequirement::Required))
          .find("direct target disagrees"),
      std::string::npos);
  --*Call.Immediate;

  Call.TargetMode = LowInstructionTargetMode::Thumb;
  EXPECT_NE(
      errorText(validateLowInstructionBoundaries(
                    Function, LowInstructionBoundaryRequirement::Required))
          .find("fixed target mode requires"),
      std::string::npos);
  Call.TargetMode = LowInstructionTargetMode::Preserve;

  auto CallOp =
      std::find_if(Entry->Ops.begin(), Entry->Ops.end(),
                   [](const LowOp &Op) { return Op.Opcode == NdOp::CALL; });
  ASSERT_NE(CallOp, Entry->Ops.end());
  CallOp->Opcode = NdOp::INDIR_CALL;
  EXPECT_NE(
      errorText(validateLowInstructionBoundaries(
                    Function, LowInstructionBoundaryRequirement::Required))
          .find("control flags disagree"),
      std::string::npos);

  LowFunc Return = buildFunction(Arch::X64, InstructionMode::Default, {0xc3});
  LowBlock *ReturnEntry = Return.blockFor(kEntry);
  ASSERT_NE(ReturnEntry, nullptr);
  LowInstructionBoundary &ReturnBoundary =
      ReturnEntry->InstructionBoundaries.front();
  ReturnBoundary.Control = LowInstructionControl::None;
  ReturnBoundary.ControlFlags = LowInstructionControlFlag::None;
  EXPECT_NE(errorText(validateLowInstructionBoundaries(
                          Return, LowInstructionBoundaryRequirement::Required))
                .find("control class disagrees"),
            std::string::npos);

  LowFunc Trap =
      buildFunction(Arch::X64, InstructionMode::Default, {0x0f, 0x0b});
  const LowBlock *TrapEntry = findBlock(Trap, kEntry);
  ASSERT_NE(TrapEntry, nullptr);
  ASSERT_EQ(TrapEntry->InstructionBoundaries.size(), 1u);
  EXPECT_EQ(TrapEntry->InstructionBoundaries.front().Control,
            LowInstructionControl::Terminator);
  EXPECT_FALSE(static_cast<bool>(validateLowInstructionBoundaries(
      Trap, LowInstructionBoundaryRequirement::Required)));
}

TEST(LowInstructionBoundary, AllowsZeroOpInstructionsAndDetectsAbsence) {
  LowBlock ZeroOp;
  ZeroOp.Id = 0;
  ZeroOp.StartAddr = kEntry;
  ZeroOp.EndAddr = kEntry + 2;
  ZeroOp.InstructionBoundaries = {
      {kEntry, 1, 0, 0, InstructionMode::Default, LowInstructionControl::None,
       LowInstructionControlFlag::None, LowInstructionTargetMode::Preserve,
       std::nullopt},
      {kEntry + 1, 1, 0, 0, InstructionMode::Default,
       LowInstructionControl::None, LowInstructionControlFlag::None,
       LowInstructionTargetMode::Preserve, std::nullopt},
  };
  EXPECT_TRUE(ZeroOp.hasInstructionBoundaries());
  EXPECT_FALSE(static_cast<bool>(validateLowInstructionBoundaries(
      ZeroOp, LowInstructionBoundaryRequirement::Required)));

  LowBlock Legacy;
  Legacy.Id = 1;
  Legacy.StartAddr = 0x2000;
  Legacy.EndAddr = 0x2001;
  LowOp Nop;
  Nop.Opcode = NdOp::NOP;
  Nop.Addr = Legacy.StartAddr;
  Legacy.Ops.push_back(Nop);

  EXPECT_FALSE(Legacy.hasInstructionBoundaries());
  EXPECT_FALSE(static_cast<bool>(validateLowInstructionBoundaries(
      Legacy, LowInstructionBoundaryRequirement::Optional)));
  EXPECT_NE(errorText(validateLowInstructionBoundaries(
                          Legacy, LowInstructionBoundaryRequirement::Required))
                .find("metadata is missing"),
            std::string::npos);
}

TEST(LowInstructionBoundary, RejectsOverflowAndNonCanonicalOpCoverage) {
  LowBlock Overflow;
  Overflow.Id = 0;
  Overflow.StartAddr = std::numeric_limits<va_t>::max() - 1;
  Overflow.EndAddr = std::numeric_limits<va_t>::max();
  Overflow.InstructionBoundaries.push_back(
      {Overflow.StartAddr, 2, 0, 0, InstructionMode::Default,
       LowInstructionControl::None, LowInstructionControlFlag::None,
       LowInstructionTargetMode::Preserve, std::nullopt});
  EXPECT_NE(
      errorText(validateLowInstructionBoundaries(
                    Overflow, LowInstructionBoundaryRequirement::Required))
          .find("overflows"),
      std::string::npos);

  LowBlock Gap;
  Gap.Id = 1;
  Gap.StartAddr = 0x3000;
  Gap.EndAddr = 0x3001;
  LowOp Nop;
  Nop.Opcode = NdOp::NOP;
  Nop.Addr = Gap.StartAddr;
  Gap.Ops.push_back(Nop);
  Gap.InstructionBoundaries.push_back(
      {Gap.StartAddr, 1, 1, 0, InstructionMode::Default,
       LowInstructionControl::None, LowInstructionControlFlag::None,
       LowInstructionTargetMode::Preserve, std::nullopt});
  EXPECT_NE(errorText(validateLowInstructionBoundaries(
                          Gap, LowInstructionBoundaryRequirement::Required))
                .find("non-canonical op slice"),
            std::string::npos);

  LowBlock Complete;
  Complete.Id = 2;
  Complete.StartAddr = 0x4000;
  Complete.EndAddr = 0x4001;
  Complete.InstructionBoundaries.push_back(
      {Complete.StartAddr, 1, 0, 0, InstructionMode::Default,
       LowInstructionControl::None, LowInstructionControlFlag::None,
       LowInstructionTargetMode::Preserve, std::nullopt});
  LowBlock Missing;
  Missing.Id = 3;
  Missing.StartAddr = 0x5000;
  Missing.EndAddr = 0x5001;
  LowFunc Partial;
  Partial.Blocks = {Complete, Missing};
  EXPECT_NE(errorText(validateLowInstructionBoundaries(
                          Partial, LowInstructionBoundaryRequirement::Optional))
                .find("metadata is missing"),
            std::string::npos);
}

} // namespace
