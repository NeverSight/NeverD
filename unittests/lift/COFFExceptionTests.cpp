//===- COFFExceptionTests.cpp - Windows exception metadata tests ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/COFF/COFFExceptionPatch.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/loader/COFF/COFFException.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

#include <limits>
#include <mutex>

namespace {

using namespace neverd;

void ensureCOFFCodegenTargets() {
  static std::once_flag Once;
  std::call_once(Once, [] {
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmParsers();
    llvm::InitializeAllAsmPrinters();
  });
}

BinaryImage makeX64ExceptionImage(size_t XDataSize = 0x100) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;

  Segment Text;
  Text.Name = ".text";
  Text.VA = Img.Base + 0x1000;
  Text.Size = 0x200;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(Text.Size, 0x90);
  Img.Segments.push_back(std::move(Text));

  Segment XData;
  XData.Name = ".xdata";
  XData.VA = Img.Base + 0x3000;
  XData.Size = XDataSize;
  XData.Flags = SegmentFlags::Readable;
  XData.Data.resize(XDataSize);
  Img.Segments.push_back(std::move(XData));
  return Img;
}

void addPersonalityImport(BinaryImage &Img, va_t StubVA, llvm::StringRef Name) {
  Import Imp;
  Imp.Module = "vcruntime-test.dll";
  Imp.Name = Name.str();
  Imp.IATAddr = Img.Base + 0x30f0;
  Img.Imports.push_back(std::move(Imp));
  ASSERT_TRUE(Img.recordImportStub(StubVA, Img.Imports.size() - 1));
}

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

TEST(COFFExceptionParser, ReconstructsCSpecificScopeTable) {
  BinaryImage Img = makeX64ExceptionImage(0x200);
  addPersonalityImport(Img, Img.Base + 0x1100, "__C_specific_handler");
  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<uint32_t>(X, 1);
  writeLE<uint32_t>(X + 4, 0x1000);
  writeLE<uint32_t>(X + 8, 0x1040);
  writeLE<uint32_t>(X + 12, 1); // catch-all filter
  writeLE<uint32_t>(X + 16, 0x1080);

  ExceptionFunction F;
  F.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1200};
  F.PersonalityVA = Img.Base + 0x1100;
  F.HandlerDataVA = Img.Base + 0x3000;
  F.Personality = ExceptionPersonality::Unknown;
  Img.ExceptionMetadata.Functions.push_back(std::move(F));

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::CSpecificHandler);
  ASSERT_TRUE(Decoded.SEH.has_value());
  ASSERT_EQ(Decoded.SEH->Scopes.size(), 1u);
  EXPECT_EQ(Decoded.SEH->Scopes[0].Kind, SEHScopeKind::CatchAll);
  EXPECT_EQ(Decoded.SEH->Scopes[0].GuardedRange.Begin, Img.Base + 0x1000);
  EXPECT_EQ(Decoded.SEH->Scopes[0].HandlerVA, Img.Base + 0x1080);
}

TEST(COFFExceptionParser, ResolvesAArch64PersonalityBranchVeneer) {
  BinaryImage Img = makeX64ExceptionImage();
  Img.Arch = Arch::AArch64;
  Img.Bits = Bitness::Bits64;
  uint8_t *Text = Img.Segments[0].Data.data();
  writeLE<uint32_t>(Text + 0x100, 0x14000008); // b +0x20

  Symbol Personality;
  Personality.Name = "__C_specific_handler";
  Personality.Addr = Img.Base + 0x1120;
  Personality.IsFunc = true;
  Img.Symbols.push_back(std::move(Personality));
  writeLE<uint32_t>(Img.Segments[1].Data.data(), 0); // empty scope table

  ExceptionFunction F;
  F.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1080};
  F.PersonalityVA = Img.Base + 0x1100;
  F.HandlerDataVA = Img.Base + 0x3000;
  Img.ExceptionMetadata.Functions.push_back(std::move(F));

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::CSpecificHandler);
  EXPECT_EQ(Decoded.PersonalityName, "__C_specific_handler");
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Complete);
  ASSERT_TRUE(Decoded.SEH.has_value());
  EXPECT_TRUE(Decoded.SEH->Scopes.empty());
}

TEST(COFFExceptionParser, DoesNotTreatAArch64CallAsBranchVeneer) {
  BinaryImage Img = makeX64ExceptionImage();
  Img.Arch = Arch::AArch64;
  Img.Bits = Bitness::Bits64;
  writeLE<uint32_t>(Img.Segments[0].Data.data() + 0x100,
                    0x94000008); // bl +0x20

  Symbol Personality;
  Personality.Name = "__C_specific_handler";
  Personality.Addr = Img.Base + 0x1120;
  Personality.IsFunc = true;
  Img.Symbols.push_back(std::move(Personality));

  ExceptionFunction F;
  F.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1080};
  F.PersonalityVA = Img.Base + 0x1100;
  F.HandlerDataVA = Img.Base + 0x3000;
  Img.ExceptionMetadata.Functions.push_back(std::move(F));

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::Unknown);
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Partial);
  EXPECT_FALSE(Decoded.SEH.has_value());
}

TEST(COFFExceptionParser, ReconstructsCxxFrameHandler3StateGraph) {
  BinaryImage Img = makeX64ExceptionImage(0x200);
  addPersonalityImport(Img, Img.Base + 0x1100, "__CxxFrameHandler3");
  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<uint32_t>(X, 0x3040); // handler data -> FuncInfo3

  uint8_t *FI = X + 0x40;
  writeLE<uint32_t>(FI, 0x19930522);
  writeLE<int32_t>(FI + 4, 2);
  writeLE<uint32_t>(FI + 8, 0x3080);
  writeLE<uint32_t>(FI + 12, 1);
  writeLE<uint32_t>(FI + 16, 0x3090);
  writeLE<uint32_t>(FI + 20, 2);
  writeLE<uint32_t>(FI + 24, 0x30d0);
  writeLE<int32_t>(FI + 28, -8);
  writeLE<uint32_t>(FI + 32, 0);
  writeLE<uint32_t>(FI + 36, 1);

  uint8_t *Unwind = X + 0x80;
  writeLE<int32_t>(Unwind, -1);
  writeLE<uint32_t>(Unwind + 4, 0x1120);
  writeLE<int32_t>(Unwind + 8, 0);
  writeLE<uint32_t>(Unwind + 12, 0x1130);

  uint8_t *Try = X + 0x90;
  writeLE<int32_t>(Try, 0);
  writeLE<int32_t>(Try + 4, 0);
  writeLE<int32_t>(Try + 8, 1);
  writeLE<uint32_t>(Try + 12, 1);
  writeLE<uint32_t>(Try + 16, 0x30b0);

  uint8_t *Catch = X + 0xb0;
  writeLE<uint32_t>(Catch, 0x40);
  writeLE<uint32_t>(Catch + 4, 0); // catch (...)
  writeLE<int32_t>(Catch + 8, 0);
  writeLE<uint32_t>(Catch + 12, 0x1150);
  writeLE<int32_t>(Catch + 16, 0x20);

  uint8_t *IPMap = X + 0xd0;
  writeLE<uint32_t>(IPMap, 0x1000);
  writeLE<int32_t>(IPMap + 4, -1);
  writeLE<uint32_t>(IPMap + 8, 0x1010);
  writeLE<int32_t>(IPMap + 12, 0);

  ExceptionFunction F;
  F.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1200};
  F.PersonalityVA = Img.Base + 0x1100;
  F.HandlerDataVA = Img.Base + 0x3000;
  F.Personality = ExceptionPersonality::Unknown;
  Img.ExceptionMetadata.Functions.push_back(std::move(F));

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Complete);
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::CxxFrameHandler3);
  ASSERT_TRUE(Decoded.Cxx.has_value());
  EXPECT_TRUE(Decoded.Cxx->hasValidStateGraph());
  ASSERT_EQ(Decoded.Cxx->TryBlocks.size(), 1u);
  ASSERT_EQ(Decoded.Cxx->TryBlocks[0].Handlers.size(), 1u);
  EXPECT_EQ(Decoded.Cxx->TryBlocks[0].Handlers[0].HandlerVA, Img.Base + 0x1150);
}

TEST(COFFExceptionParser, AcceptsFH3IPMapAcrossSharedFuncInfoGroup) {
  BinaryImage Img = makeX64ExceptionImage(0x200);
  addPersonalityImport(Img, Img.Base + 0x1100, "__CxxFrameHandler3");
  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<uint32_t>(X, 0x3040);
  writeLE<uint32_t>(X + 4, 0x3040);

  uint8_t *FI = X + 0x40;
  writeLE<uint32_t>(FI, 0x19930522);
  writeLE<int32_t>(FI + 4, 2);
  writeLE<uint32_t>(FI + 8, 0x3080);
  writeLE<uint32_t>(FI + 12, 1);
  writeLE<uint32_t>(FI + 16, 0x3090);
  writeLE<uint32_t>(FI + 20, 2);
  writeLE<uint32_t>(FI + 24, 0x30d0);
  writeLE<int32_t>(FI + 28, -8);
  writeLE<uint32_t>(FI + 32, 0);
  writeLE<uint32_t>(FI + 36, 1);

  uint8_t *Unwind = X + 0x80;
  writeLE<int32_t>(Unwind, -1);
  writeLE<uint32_t>(Unwind + 4, 0);
  writeLE<int32_t>(Unwind + 8, 0);
  writeLE<uint32_t>(Unwind + 12, 0);

  uint8_t *Try = X + 0x90;
  writeLE<int32_t>(Try, 0);
  writeLE<int32_t>(Try + 4, 0);
  writeLE<int32_t>(Try + 8, 1);
  writeLE<uint32_t>(Try + 12, 1);
  writeLE<uint32_t>(Try + 16, 0x30b0);

  uint8_t *Catch = X + 0xb0;
  writeLE<uint32_t>(Catch, 0x40);
  writeLE<uint32_t>(Catch + 4, 0);
  writeLE<int32_t>(Catch + 8, 0);
  writeLE<uint32_t>(Catch + 12, 0x1150);
  writeLE<int32_t>(Catch + 16, 0);

  uint8_t *IPMap = X + 0xd0;
  writeLE<uint32_t>(IPMap, 0x1000);
  writeLE<int32_t>(IPMap + 4, 0);
  writeLE<uint32_t>(IPMap + 8, 0x1150);
  writeLE<int32_t>(IPMap + 12, 1);

  ExceptionFunction Parent;
  Parent.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1080};
  Parent.PersonalityVA = Img.Base + 0x1100;
  Parent.HandlerDataVA = Img.Base + 0x3000;
  Img.ExceptionMetadata.Functions.push_back(std::move(Parent));

  ExceptionFunction CatchFunclet;
  CatchFunclet.CodeRange = {Img.Base + 0x1150, Img.Base + 0x1180};
  CatchFunclet.PersonalityVA = Img.Base + 0x1100;
  CatchFunclet.HandlerDataVA = Img.Base + 0x3004;
  Img.ExceptionMetadata.Functions.push_back(std::move(CatchFunclet));

  coff_loader::resolveExceptionHandlers(Img);
  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 2u);
  const ExceptionFunction &DecodedParent = Img.ExceptionMetadata.Functions[0];
  const ExceptionFunction &DecodedCatch = Img.ExceptionMetadata.Functions[1];
  EXPECT_EQ(DecodedParent.ParseStatus, ExceptionParseStatus::Complete);
  EXPECT_EQ(DecodedCatch.ParseStatus, ExceptionParseStatus::Complete);
  ASSERT_TRUE(DecodedParent.Cxx.has_value());
  ASSERT_TRUE(DecodedCatch.Cxx.has_value());
  EXPECT_TRUE(DecodedParent.Cxx->IsSeparated);
  EXPECT_FALSE(DecodedParent.Cxx->IsCatchFunclet);
  EXPECT_TRUE(DecodedCatch.Cxx->IsSeparated);
  EXPECT_TRUE(DecodedCatch.Cxx->IsCatchFunclet);
  EXPECT_TRUE(DecodedParent.Cxx->hasValidStateGraph());
  EXPECT_TRUE(DecodedCatch.Cxx->hasValidStateGraph());
}

TEST(COFFExceptionParser, StopsFH3AggregateGraphExpansionAtBudget) {
  BinaryImage Img = makeX64ExceptionImage(0x100);
  addPersonalityImport(Img, Img.Base + 0x1100, "__CxxFrameHandler3");
  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<uint32_t>(X, 0x3040); // handler data -> FuncInfo3

  uint8_t *FI = X + 0x40;
  writeLE<uint32_t>(FI, 0x19930522);
  writeLE<int32_t>(FI + 4, 1 << 16); // valid per-table maximum
  writeLE<uint32_t>(FI + 8, 0);
  writeLE<uint32_t>(FI + 12, 1); // individually valid, aggregate is too large
  writeLE<uint32_t>(FI + 16, 0);
  writeLE<uint32_t>(FI + 20, 0);
  writeLE<uint32_t>(FI + 24, 0);
  writeLE<int32_t>(FI + 28, 0);
  writeLE<uint32_t>(FI + 32, 0);
  writeLE<uint32_t>(FI + 36, 1);

  ExceptionFunction F;
  F.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1080};
  F.PersonalityVA = Img.Base + 0x1100;
  F.HandlerDataVA = Img.Base + 0x3000;
  Img.ExceptionMetadata.Functions.push_back(std::move(F));

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::CxxFrameHandler3);
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Partial);
  EXPECT_FALSE(Decoded.Cxx.has_value());
  ASSERT_FALSE(Decoded.Diagnostics.empty());
  EXPECT_NE(Decoded.Diagnostics.back().find("aggregate language graph"),
            std::string::npos);
}

TEST(COFFExceptionParser, ReconstructsCompressedCxxFrameHandler4Graph) {
  BinaryImage Img = makeX64ExceptionImage(0x300);
  addPersonalityImport(Img, Img.Base + 0x1100, "__CxxFrameHandler4");
  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<uint32_t>(X, 0x3040); // handler data -> FuncInfo4

  // FuncInfo4: unwind map + try map, followed by the mandatory IP map.
  X[0x40] = 0x18;
  writeLE<uint32_t>(X + 0x41, 0x3080);
  writeLE<uint32_t>(X + 0x45, 0x30a0);
  writeLE<uint32_t>(X + 0x49, 0x30e0);

  // Two unwind entries.  FH4 compressed integers below are all one byte
  // (value << 1).  Entry 1 points five bytes back to entry 0.
  X[0x80] = 4; // count = 2
  X[0x81] = 6; // direct action, terminal state
  writeLE<uint32_t>(X + 0x82, 0x1120);
  X[0x86] = 46; // direct action, predecessor byte distance = 5
  writeLE<uint32_t>(X + 0x87, 0x1130);

  X[0xa0] = 2; // one try block
  X[0xa1] = 0; // try low = 0
  X[0xa2] = 0; // try high = 0
  X[0xa3] = 2; // catch high = 1
  writeLE<uint32_t>(X + 0xa4, 0x30c0);

  X[0xc0] = 2;    // one handler
  X[0xc1] = 0x10; // one function-relative continuation
  writeLE<uint32_t>(X + 0xc2, 0x1150);
  X[0xc6] = 0x60; // continuation = function + 0x30

  X[0xe0] = 4;    // two IP-state entries
  X[0xe1] = 0;    // delta 0
  X[0xe2] = 0;    // encoded state -1
  X[0xe3] = 0x20; // delta 0x10
  X[0xe4] = 2;    // encoded state 0

  ExceptionFunction F;
  F.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1200};
  F.PersonalityVA = Img.Base + 0x1100;
  F.HandlerDataVA = Img.Base + 0x3000;
  Img.ExceptionMetadata.Functions.push_back(std::move(F));

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Complete);
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::CxxFrameHandler4);
  ASSERT_TRUE(Decoded.Cxx.has_value());
  EXPECT_EQ(Decoded.Cxx->NativeEncoding, CxxExceptionInfo::Encoding::FH4);
  EXPECT_TRUE(Decoded.Cxx->hasValidStateGraph());
  ASSERT_EQ(Decoded.Cxx->UnwindMap.size(), 2u);
  EXPECT_EQ(Decoded.Cxx->UnwindMap[1].ToState, 0);
  ASSERT_EQ(Decoded.Cxx->TryBlocks.size(), 1u);
  ASSERT_EQ(Decoded.Cxx->TryBlocks[0].Handlers.size(), 1u);
  ASSERT_EQ(Decoded.Cxx->TryBlocks[0].Handlers[0].ContinuationVAs.size(), 1u);
  EXPECT_EQ(Decoded.Cxx->TryBlocks[0].Handlers[0].ContinuationVAs[0],
            Img.Base + 0x1030);
  EXPECT_FALSE(Decoded.canRegenerateLanguageMetadata());
}

TEST(COFFExceptionParser, StopsFH4RepeatedHandlerExpansionAtBudget) {
  BinaryImage Img = makeX64ExceptionImage(0x100);
  addPersonalityImport(Img, Img.Base + 0x1100, "__CxxFrameHandler4");
  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<uint32_t>(X, 0x3040); // handler data -> FuncInfo4

  X[0x40] = 0x10; // try map plus mandatory IP map
  writeLE<uint32_t>(X + 0x41, 0x3080);
  writeLE<uint32_t>(X + 0x45, 0x30c0);

  // Canonical three-byte encoding of 65,536 try records.  Every record may
  // legally reference the same handler map, so a per-table limit alone would
  // permit quadratic normalized-graph expansion.
  X[0x80] = 3;
  X[0x81] = 0;
  X[0x82] = 8;
  X[0x83] = 0; // try low
  X[0x84] = 0; // try high
  X[0x85] = 0; // catch high
  writeLE<uint32_t>(X + 0x86, 0x30a0);
  X[0xa0] = 2; // one handler; aggregate budget is already exhausted
  X[0xc0] = 2; // one mandatory IP-state entry (not reached)
  X[0xc1] = 0;
  X[0xc2] = 0;

  ExceptionFunction F;
  F.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1080};
  F.PersonalityVA = Img.Base + 0x1100;
  F.HandlerDataVA = Img.Base + 0x3000;
  Img.ExceptionMetadata.Functions.push_back(std::move(F));

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::CxxFrameHandler4);
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Partial);
  EXPECT_FALSE(Decoded.Cxx.has_value());
  ASSERT_FALSE(Decoded.Diagnostics.empty());
  EXPECT_NE(Decoded.Diagnostics.back().find("aggregate language graph"),
            std::string::npos);
}

TEST(COFFExceptionParser, RejectsNonCanonicalFH4CompressedInteger) {
  BinaryImage Img = makeX64ExceptionImage(0x100);
  addPersonalityImport(Img, Img.Base + 0x1100, "__CxxFrameHandler4");
  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<uint32_t>(X, 0x3040);
  X[0x40] = 0;
  writeLE<uint32_t>(X + 0x41, 0x3080);
  X[0x80] = 5; // overlong two-byte encoding of the value one
  X[0x81] = 0;

  ExceptionFunction F;
  F.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1200};
  F.PersonalityVA = Img.Base + 0x1100;
  F.HandlerDataVA = Img.Base + 0x3000;
  Img.ExceptionMetadata.Functions.push_back(std::move(F));

  coff_loader::resolveExceptionHandlers(Img);
  EXPECT_EQ(Img.ExceptionMetadata.Functions[0].ParseStatus,
            ExceptionParseStatus::Malformed);
}

TEST(COFFExceptionParser, KeepsGSWrapperDistinctAndFailClosed) {
  BinaryImage Img = makeX64ExceptionImage();
  addPersonalityImport(Img, Img.Base + 0x1100, "__GSHandlerCheck_SEH");
  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<uint32_t>(X, 0);         // no SEH scopes
  writeLE<uint32_t>(X + 4, 0x25);  // EH + aligned, cookie offset 0x20
  writeLE<int32_t>(X + 8, -0x10);  // alignment base
  writeLE<uint32_t>(X + 12, 0x10); // alignment

  ExceptionFunction F;
  F.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1200};
  F.PersonalityVA = Img.Base + 0x1100;
  F.HandlerDataVA = Img.Base + 0x3000;
  F.Personality = ExceptionPersonality::Unknown;
  Img.ExceptionMetadata.Functions.push_back(std::move(F));

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::GSHandlerCheckSEH);
  ASSERT_TRUE(Decoded.SEH.has_value());
  ASSERT_TRUE(Decoded.GSCookie.has_value());
  EXPECT_FALSE(Decoded.GSCookie->Payload.empty());
  EXPECT_EQ(Decoded.GSCookie->CookieOffset, 0x20);
  EXPECT_TRUE(Decoded.GSCookie->HasAlignment);
  EXPECT_EQ(Decoded.GSCookie->Alignment, 0x10u);
  EXPECT_FALSE(Decoded.canRegenerateLanguageMetadata());
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Complete);
}

TEST(COFFExceptionIR, EmitsLosslessNamedMetadata) {
  MedFunc Func;
  Func.Entry = 0x140001000;
  Func.Name = "eh_metadata_test";
  MedBlock Block;
  Block.Id = 0;
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = Func.Entry;
  Block.Ops.push_back(Return);
  Func.Blocks.push_back(std::move(Block));

  ExceptionFunction EH;
  EH.CodeRange = {Func.Entry, Func.Entry + 0x40};
  EH.Kind = RuntimeFunctionKind::Chained;
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.UnwindInfoVA = 0x140003000;
  EH.PackedUnwindData = 0x12345678;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  EH.PersonalityName = "resolved!__C_specific_handler";
  EH.PersonalityVA = 0x140001100;
  EH.HandlerDataVA = 0x140003010;
  EH.NativeUnwindBytes = {0x09, 0x04, 0x00, 0x00};
  UnwindOperation UnwindOp;
  UnwindOp.Kind = UnwindOperationKind::AllocateLarge;
  UnwindOp.SlotCount = 2;
  EH.UnwindOperations.push_back(UnwindOp);
  EH.PrimaryFunctionIndex = 7;
  EH.ChainedPrimaryRange = ExceptionAddressRange{Func.Entry, Func.Entry + 0x20};
  EH.ChainedUnwindInfoRVA = 0x3010;
  SEHExceptionInfo SEH;
  SEHScopeRecord Scope;
  Scope.GuardedRange = EH.CodeRange;
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = 0x140001080;
  SEH.Scopes.push_back(Scope);
  EH.SEH = std::move(SEH);
  Func.ExceptionMetadata = std::move(EH);

  llvm::LLVMContext Ctx;
  auto Module = MedLLVMEmitter().emit({Func}, Ctx, "eh-metadata", Arch::X64, {},
                                      nullptr, BinaryFormat::COFF);
  ASSERT_NE(Module, nullptr);
  EXPECT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  llvm::NamedMDNode *Table =
      Module->getNamedMetadata("neverd.windows.eh.functions");
  ASSERT_NE(Table, nullptr);
  ASSERT_EQ(Table->getNumOperands(), 1u);
  llvm::Function *IRFunc = Module->getFunction("eh_metadata_test");
  ASSERT_NE(IRFunc, nullptr);
  llvm::MDNode *Payload = IRFunc->getMetadata("neverd.windows.eh");
  ASSERT_NE(Payload, nullptr);
  ASSERT_EQ(Payload->getNumOperands(), windows_eh_md::OperandCount);
  auto UIntAt = [](const llvm::MDNode &Node, unsigned Index) -> uint64_t {
    const auto *Constant =
        llvm::dyn_cast<llvm::ConstantAsMetadata>(Node.getOperand(Index).get());
    EXPECT_NE(Constant, nullptr);
    const auto *Integer =
        Constant ? llvm::dyn_cast<llvm::ConstantInt>(Constant->getValue())
                 : nullptr;
    EXPECT_NE(Integer, nullptr);
    return Integer ? Integer->getZExtValue() : 0;
  };
  EXPECT_EQ(UIntAt(*Payload, windows_eh_md::Version),
            windows_eh_md::SchemaVersion);
  EXPECT_EQ(UIntAt(*Payload, windows_eh_md::RuntimeKind),
            static_cast<uint8_t>(RuntimeFunctionKind::Chained));
  EXPECT_EQ(UIntAt(*Payload, windows_eh_md::PackedUnwindData), 0x12345678u);
  const auto *ResolvedName = llvm::dyn_cast<llvm::MDString>(
      Payload->getOperand(windows_eh_md::PersonalityName).get());
  ASSERT_NE(ResolvedName, nullptr);
  EXPECT_EQ(ResolvedName->getString(), "resolved!__C_specific_handler");
  const auto *Operations = llvm::dyn_cast<llvm::MDNode>(
      Payload->getOperand(windows_eh_md::UnwindOperations).get());
  ASSERT_NE(Operations, nullptr);
  ASSERT_EQ(Operations->getNumOperands(), 1u);
  const auto *FirstOperation =
      llvm::dyn_cast<llvm::MDNode>(Operations->getOperand(0).get());
  ASSERT_NE(FirstOperation, nullptr);
  EXPECT_EQ(UIntAt(*FirstOperation, 3), 2u);
  const auto *PrimaryIndex = llvm::dyn_cast<llvm::MDNode>(
      Payload->getOperand(windows_eh_md::PrimaryFunctionIndex).get());
  ASSERT_NE(PrimaryIndex, nullptr);
  ASSERT_EQ(PrimaryIndex->getNumOperands(), 1u);
  EXPECT_EQ(UIntAt(*PrimaryIndex, 0), 7u);
  const auto *PrimaryRange = llvm::dyn_cast<llvm::MDNode>(
      Payload->getOperand(windows_eh_md::ChainedPrimaryRange).get());
  ASSERT_NE(PrimaryRange, nullptr);
  ASSERT_EQ(PrimaryRange->getNumOperands(), 2u);
  EXPECT_EQ(UIntAt(*PrimaryRange, 0), Func.Entry);
  EXPECT_EQ(UIntAt(*PrimaryRange, 1), Func.Entry + 0x20);
  EXPECT_EQ(UIntAt(*Payload, windows_eh_md::ChainedUnwindInfoRVA), 0x3010u);
}

TEST(COFFExceptionIR, EmitsVerifierCleanNativeCatchAllSEH) {
  MedFunc Func;
  Func.Entry = 0x140001000;
  Func.Name = "native_seh_test";

  MedBlock Protected;
  Protected.Id = 0;
  Protected.StartAddr = Func.Entry;
  Protected.EndAddr = Func.Entry + 0x10;
  MedOp ProtectedReturn;
  ProtectedReturn.Opcode = NdOp::RETURN;
  ProtectedReturn.Addr = Func.Entry + 8;
  Protected.Ops.push_back(ProtectedReturn);

  MedBlock Handler;
  Handler.Id = 1;
  Handler.StartAddr = Func.Entry + 0x20;
  Handler.EndAddr = Func.Entry + 0x30;
  MedOp HandlerReturn;
  HandlerReturn.Opcode = NdOp::RETURN;
  HandlerReturn.Addr = Func.Entry + 0x28;
  Handler.Ops.push_back(HandlerReturn);

  Func.Blocks.push_back(std::move(Protected));
  Func.Blocks.push_back(std::move(Handler));

  ExceptionFunction EH;
  EH.CodeRange = {Func.Entry, Func.Entry + 0x40};
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  EH.PersonalityVA = Func.Entry + 0x100;
  SEHExceptionInfo SEH;
  SEHScopeRecord Scope;
  Scope.GuardedRange = {Func.Entry, Func.Entry + 0x10};
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = Func.Entry + 0x20;
  SEH.Scopes.push_back(Scope);
  EH.SEH = std::move(SEH);
  Func.ExceptionMetadata = std::move(EH);

  llvm::LLVMContext Ctx;
  MedLLVMEmitter Emitter;
  auto Mod = Emitter.emit({Func}, Ctx, "native_seh", Arch::X64, {}, nullptr,
                          BinaryFormat::COFF);
  ASSERT_NE(Mod, nullptr);
  std::string Verification;
  llvm::raw_string_ostream VerificationOS(Verification);
  EXPECT_FALSE(llvm::verifyModule(*Mod, &VerificationOS)) << Verification;

  llvm::Function *F = Mod->getFunction(Func.Name);
  ASSERT_NE(F, nullptr);
  EXPECT_TRUE(F->hasPersonalityFn());
  EXPECT_NE(F->getMetadata(windows_eh_md::NativeAttachment), nullptr);
  EXPECT_NE(Mod->getModuleFlag("eh-asynch"), nullptr);

  std::string IR;
  llvm::raw_string_ostream OS(IR);
  Mod->print(OS, nullptr);
  EXPECT_NE(IR.find("invoke void @llvm.seh.try.begin"), std::string::npos);
  EXPECT_NE(IR.find("invoke void @llvm.seh.try.end"), std::string::npos);
  EXPECT_NE(IR.find("catchswitch within none"), std::string::npos);
  EXPECT_NE(IR.find("catchpad within"), std::string::npos);
  EXPECT_NE(IR.find("catchret from"), std::string::npos);
}

TEST(COFFExceptionIR, EmitsVerifierCleanNativeSimpleFH3Catch) {
  MedFunc Func;
  Func.Entry = 0x140001000;
  Func.Name = "native_cxx_test";
  Func.ReturnType = NdType::makeVoid();
  constexpr va_t MayThrowVA = 0x140001100;

  MedBlock Protected;
  Protected.Id = 0;
  Protected.StartAddr = Func.Entry;
  Protected.EndAddr = Func.Entry + 0x10;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = Func.Entry + 4;
  Call.addInput(MedVar::makeConst(MayThrowVA, 8));
  Protected.Ops.push_back(Call);
  MedOp ProtectedReturn;
  ProtectedReturn.Opcode = NdOp::RETURN;
  ProtectedReturn.Addr = Func.Entry + 8;
  Protected.Ops.push_back(ProtectedReturn);

  MedBlock Handler;
  Handler.Id = 1;
  Handler.StartAddr = Func.Entry + 0x20;
  Handler.EndAddr = Func.Entry + 0x30;
  MedOp HandlerReturn;
  HandlerReturn.Opcode = NdOp::RETURN;
  HandlerReturn.Addr = Func.Entry + 0x28;
  Handler.Ops.push_back(HandlerReturn);

  Func.Blocks.push_back(std::move(Protected));
  Func.Blocks.push_back(std::move(Handler));

  ExceptionFunction EH;
  EH.CodeRange = {Func.Entry, Func.Entry + 0x40};
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CxxFrameHandler3;
  CxxExceptionInfo Cxx;
  Cxx.Flags = 1;
  Cxx.IsSynchronous = true;
  Cxx.MaxState = 2;
  CxxUnwindAction State0;
  State0.ToState = -1;
  State0.Kind = CxxUnwindAction::ActionKind::None;
  CxxUnwindAction State1;
  State1.ToState = 0;
  State1.Kind = CxxUnwindAction::ActionKind::None;
  Cxx.UnwindMap = {State0, State1};
  Cxx.IPMap = {{Func.Entry, 0}, {Func.Entry + 0x10, -1}};
  CxxTryBlock Try;
  Try.TryLow = 0;
  Try.TryHigh = 0;
  Try.CatchHigh = 1;
  CxxCatchHandler Catch;
  Catch.Adjectives = 0x40;
  Catch.HandlerVA = Func.Entry + 0x20;
  Try.Handlers.push_back(Catch);
  Cxx.TryBlocks.push_back(std::move(Try));
  ASSERT_TRUE(Cxx.hasValidStateGraph());
  EH.Cxx = std::move(Cxx);
  Func.ExceptionMetadata = EH;

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::COFF;
  Image.Base = 0x140000000;
  Image.DynInfo.GuardFlags =
      uint32_t(llvm::COFF::GuardFlags::EH_CONTINUATION_TABLE_PRESENT) |
      uint32_t(llvm::COFF::GuardFlags::CF_INSTRUMENTED) |
      uint32_t(llvm::COFF::GuardFlags::CF_FUNCTION_TABLE_PRESENT);
  Image.ExceptionMetadata.Functions.push_back(EH);
  Image.ExceptionMetadata.rebuildIndex();

  llvm::LLVMContext Ctx;
  MedLLVMEmitter Emitter;
  auto Mod =
      Emitter.emit({Func}, Ctx, "native_cxx", Arch::X64,
                   {{MayThrowVA, "may_throw"}}, &Image, BinaryFormat::COFF);
  ASSERT_NE(Mod, nullptr);
  std::string Verification;
  llvm::raw_string_ostream VerificationOS(Verification);
  EXPECT_FALSE(llvm::verifyModule(*Mod, &VerificationOS)) << Verification;

  llvm::Function *F = Mod->getFunction(Func.Name);
  ASSERT_NE(F, nullptr);
  EXPECT_TRUE(F->hasPersonalityFn());
  EXPECT_NE(F->getMetadata(windows_eh_md::NativeAttachment), nullptr);
  EXPECT_EQ(Mod->getModuleFlag("eh-asynch"), nullptr);
  EXPECT_NE(Mod->getModuleFlag("cfguard"), nullptr);
  EXPECT_NE(Mod->getModuleFlag("ehcontguard"), nullptr);

  std::string IR;
  llvm::raw_string_ostream OS(IR);
  Mod->print(OS, nullptr);
  EXPECT_NE(IR.find("personality ptr @__CxxFrameHandler3"), std::string::npos);
  EXPECT_NE(IR.find("invoke"), std::string::npos);
  EXPECT_NE(IR.find("catchswitch within none"), std::string::npos);
  EXPECT_NE(IR.find("catchpad within"), std::string::npos);
  EXPECT_NE(IR.find("i32 64"), std::string::npos);

  auto Plan = planCOFFExceptionPatch(*Mod, Image, Arch::X64);
  ASSERT_TRUE(static_cast<bool>(Plan)) << llvm::toString(Plan.takeError());

  ensureCOFFCodegenTargets();
  constexpr uint64_t GeneratedVA = 0x140004000;
  CompiledImage Compiled = compileImageForPatch(
      *Mod, Arch::X64, BinaryFormat::COFF, GeneratedVA,
      [&](llvm::StringRef Symbol, uint32_t) -> std::optional<uint64_t> {
        if (Symbol == "may_throw")
          return MayThrowVA;
        if (Symbol == "__CxxFrameHandler3")
          return Func.Entry + 0x180;
        return std::nullopt;
      },
      Image.Base);
  ASSERT_TRUE(Compiled.Success);
  EXPECT_TRUE(Compiled.Unresolved.empty());
  EXPECT_TRUE(Compiled.SymbolAddrs.count(Func.Name));

  const CompiledSection *EHCont = nullptr;
  const CompiledSection *Text = nullptr;
  for (const CompiledSection &Section : Compiled.Sections) {
    if (llvm::StringRef(Section.Name).starts_with(".gehcont"))
      EHCont = &Section;
    if (Section.Kind == llvm::mc_rewrite::RewriteSectionKind::Code)
      Text = &Section;
  }
  ASSERT_NE(Text, nullptr);
  ASSERT_NE(EHCont, nullptr);
  EXPECT_FALSE(EHCont->IsAllocated);
  EXPECT_EQ(EHCont->VA, 0u);
  ASSERT_FALSE(EHCont->SymbolIndexReferences.empty());
  for (const auto &Reference : EHCont->SymbolIndexReferences) {
    EXPECT_GE(Reference.TargetVA, Text->VA);
    EXPECT_LT(Reference.TargetVA, Text->VA + Text->Size);
  }

  auto *WrongPersonalityTy =
      llvm::FunctionType::get(llvm::Type::getInt32Ty(Ctx), {}, true);
  llvm::FunctionCallee WrongPersonality =
      Mod->getOrInsertFunction("__C_specific_handler", WrongPersonalityTy);
  F->setPersonalityFn(llvm::cast<llvm::Constant>(WrongPersonality.getCallee()));
  auto TamperedPlan = planCOFFExceptionPatch(*Mod, Image, Arch::X64);
  ASSERT_FALSE(static_cast<bool>(TamperedPlan));
  EXPECT_NE(llvm::toString(TamperedPlan.takeError())
                .find("native WinEH IR contract was altered"),
            std::string::npos);
}

TEST(COFFExceptionIR, EmitsVerifierCleanNestedFH3CatchRegions) {
  MedFunc Func;
  Func.Entry = 0x140001000;
  Func.Name = "native_nested_cxx_test";
  Func.ReturnType = NdType::makeVoid();
  constexpr va_t MayThrowVA = 0x140001200;

  auto AddCallBlock = [&](int Id, va_t Begin) {
    MedBlock Block;
    Block.Id = Id;
    Block.StartAddr = Begin;
    Block.EndAddr = Begin + 0x10;
    MedOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Addr = Begin + 4;
    Call.addInput(MedVar::makeConst(MayThrowVA, 8));
    Block.Ops.push_back(Call);
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = Begin + 8;
    Block.Ops.push_back(Return);
    Func.Blocks.push_back(std::move(Block));
  };
  auto AddHandlerBlock = [&](int Id, va_t Begin) {
    MedBlock Block;
    Block.Id = Id;
    Block.StartAddr = Begin;
    Block.EndAddr = Begin + 0x10;
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = Begin + 8;
    Block.Ops.push_back(Return);
    Func.Blocks.push_back(std::move(Block));
  };
  AddCallBlock(0, Func.Entry);
  AddCallBlock(1, Func.Entry + 0x10);
  AddHandlerBlock(2, Func.Entry + 0x40);
  AddHandlerBlock(3, Func.Entry + 0x50);

  ExceptionFunction EH;
  EH.CodeRange = {Func.Entry, Func.Entry + 0x60};
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CxxFrameHandler3;
  CxxExceptionInfo Cxx;
  Cxx.Flags = 1;
  Cxx.IsSynchronous = true;
  Cxx.MaxState = 4;
  for (int32_t State = 0; State < 4; ++State) {
    CxxUnwindAction Action;
    Action.ToState = State - 1;
    Action.Kind = CxxUnwindAction::ActionKind::None;
    Cxx.UnwindMap.push_back(Action);
  }
  Cxx.IPMap = {
      {Func.Entry, 0}, {Func.Entry + 0x10, 1}, {Func.Entry + 0x20, -1}};

  CxxTryBlock Outer;
  Outer.TryLow = 0;
  Outer.TryHigh = 1;
  Outer.CatchHigh = 3;
  CxxCatchHandler OuterCatch;
  OuterCatch.HandlerVA = Func.Entry + 0x40;
  Outer.Handlers.push_back(OuterCatch);
  Cxx.TryBlocks.push_back(std::move(Outer));

  CxxTryBlock Inner;
  Inner.TryLow = 1;
  Inner.TryHigh = 1;
  Inner.CatchHigh = 2;
  CxxCatchHandler InnerCatch;
  InnerCatch.HandlerVA = Func.Entry + 0x50;
  Inner.Handlers.push_back(InnerCatch);
  Cxx.TryBlocks.push_back(std::move(Inner));
  ASSERT_TRUE(Cxx.hasValidStateGraph());
  EH.Cxx = std::move(Cxx);
  Func.ExceptionMetadata = EH;

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::COFF;
  Image.Base = 0x140000000;
  Image.ExceptionMetadata.Functions.push_back(EH);
  Image.ExceptionMetadata.rebuildIndex();

  llvm::LLVMContext Ctx;
  MedLLVMEmitter Emitter;
  auto Mod =
      Emitter.emit({Func}, Ctx, "native_nested_cxx", Arch::X64,
                   {{MayThrowVA, "may_throw"}}, &Image, BinaryFormat::COFF);
  ASSERT_NE(Mod, nullptr);
  std::string Verification;
  llvm::raw_string_ostream VerificationOS(Verification);
  EXPECT_FALSE(llvm::verifyModule(*Mod, &VerificationOS)) << Verification;

  llvm::Function *F = Mod->getFunction(Func.Name);
  ASSERT_NE(F, nullptr);
  EXPECT_TRUE(F->hasPersonalityFn());
  EXPECT_NE(F->getMetadata(windows_eh_md::NativeAttachment), nullptr);

  std::string IR;
  llvm::raw_string_ostream OS(IR);
  Mod->print(OS, nullptr);
  EXPECT_NE(IR.find("cxx.catch.dispatch.0"), std::string::npos);
  EXPECT_NE(IR.find("cxx.catch.dispatch.1"), std::string::npos);
  EXPECT_NE(IR.find("unwind label %cxx.catch.dispatch.0"), std::string::npos);

  auto Plan = planCOFFExceptionPatch(*Mod, Image, Arch::X64);
  ASSERT_TRUE(static_cast<bool>(Plan)) << llvm::toString(Plan.takeError());
}

TEST(COFFExceptionIR, StructuresReducibleSEHAndCxxRegionsInHighIR) {
  auto MakeFunction = [] {
    MedFunc Func;
    Func.Entry = 0x140001000;
    Func.Name = "structured_eh";
    Func.ReturnType = NdType::makeVoid();

    MedBlock Protected;
    Protected.Id = 0;
    Protected.StartAddr = Func.Entry;
    Protected.EndAddr = Func.Entry + 0x10;
    MedOp ProtectedReturn;
    ProtectedReturn.Opcode = NdOp::RETURN;
    ProtectedReturn.Addr = Func.Entry + 8;
    Protected.Ops.push_back(ProtectedReturn);

    MedBlock Handler;
    Handler.Id = 1;
    Handler.StartAddr = Func.Entry + 0x20;
    Handler.EndAddr = Func.Entry + 0x30;
    MedOp HandlerReturn;
    HandlerReturn.Opcode = NdOp::RETURN;
    HandlerReturn.Addr = Func.Entry + 0x28;
    Handler.Ops.push_back(HandlerReturn);

    Func.Blocks.push_back(std::move(Protected));
    Func.Blocks.push_back(std::move(Handler));
    return Func;
  };

  MedFunc SEHFunc = MakeFunction();
  ExceptionFunction SEHMetadata;
  SEHMetadata.CodeRange = {SEHFunc.Entry, SEHFunc.Entry + 0x30};
  SEHMetadata.ParseStatus = ExceptionParseStatus::Complete;
  SEHMetadata.Personality = ExceptionPersonality::CSpecificHandler;
  SEHExceptionInfo SEH;
  SEHScopeRecord Scope;
  Scope.GuardedRange = {SEHFunc.Entry, SEHFunc.Entry + 0x10};
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = SEHFunc.Entry + 0x20;
  SEH.Scopes.push_back(Scope);
  SEHMetadata.SEH = std::move(SEH);
  SEHFunc.ExceptionMetadata = std::move(SEHMetadata);

  HighFunc HighSEH = MedToHighConverter().convert(SEHFunc, Arch::X64);
  ASSERT_EQ(HighSEH.StructuredExceptionRegions, 1u);
  ASSERT_EQ(HighSEH.UnstructuredExceptionRegions, 0u);
  ASSERT_FALSE(HighSEH.Body.empty());
  EXPECT_EQ(HighSEH.Body.front().Kind, StmtKind::SEHTry);
  ASSERT_EQ(HighSEH.Body.front().EHClauses.size(), 1u);
  EXPECT_EQ(HighSEH.Body.front().EHClauses.front().Kind,
            HighEHClauseKind::SEHExcept);

  MedFunc CxxFunc = MakeFunction();
  CxxFunc.Name = "structured_cxx";
  ExceptionFunction CxxMetadata;
  CxxMetadata.CodeRange = {CxxFunc.Entry, CxxFunc.Entry + 0x30};
  CxxMetadata.ParseStatus = ExceptionParseStatus::Complete;
  CxxMetadata.Personality = ExceptionPersonality::CxxFrameHandler3;
  CxxExceptionInfo Cxx;
  Cxx.MaxState = 2;
  Cxx.UnwindMap = {{-1, 0}, {0, 0}};
  Cxx.UnwindMap[0].ActionVA = CxxFunc.Entry + 0x100;
  Cxx.UnwindMap[0].Kind =
      CxxUnwindAction::ActionKind::DestructorWithObjectPointer;
  Cxx.UnwindMap[0].ObjectOffset = -0x20;
  Cxx.IPMap = {{CxxFunc.Entry, 0},
               {CxxFunc.Entry + 0x10, -1},
               {CxxFunc.Entry + 0x20, 1}};
  CxxTryBlock Try;
  Try.TryLow = 0;
  Try.TryHigh = 0;
  Try.CatchHigh = 1;
  CxxCatchHandler Catch;
  Catch.HandlerVA = CxxFunc.Entry + 0x20;
  Try.Handlers.push_back(Catch);
  Cxx.TryBlocks.push_back(std::move(Try));
  ASSERT_TRUE(Cxx.hasValidStateGraph());
  CxxMetadata.Cxx = std::move(Cxx);
  CxxFunc.ExceptionMetadata = std::move(CxxMetadata);

  HighFunc HighCxx = MedToHighConverter().convert(CxxFunc, Arch::X64);
  ASSERT_EQ(HighCxx.StructuredExceptionRegions, 1u);
  ASSERT_EQ(HighCxx.UnstructuredExceptionRegions, 0u);
  ASSERT_FALSE(HighCxx.Body.empty());
  EXPECT_EQ(HighCxx.Body.front().Kind, StmtKind::CxxTry);
  ASSERT_EQ(HighCxx.Body.front().EHClauses.size(), 2u);
  EXPECT_EQ(HighCxx.Body.front().EHClauses.front().Kind,
            HighEHClauseKind::CxxCatch);
  const HighEHClause &Cleanup = HighCxx.Body.front().EHClauses.back();
  EXPECT_EQ(Cleanup.Kind, HighEHClauseKind::CxxCleanup);
  EXPECT_EQ(Cleanup.UnwindActionKind,
            CxxUnwindAction::ActionKind::DestructorWithObjectPointer);
  EXPECT_EQ(Cleanup.UnwindObjectOffset, -0x20);
}

TEST(COFFExceptionPatch, AcceptsCompleteX64UnwindContract) {
  MedFunc Func;
  Func.Entry = 0x140001000;
  Func.Name = "sub_140001000";
  MedBlock Block;
  Block.Id = 0;
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = Func.Entry;
  Block.Ops.push_back(Return);
  Func.Blocks.push_back(std::move(Block));

  ExceptionFunction EH;
  EH.CodeRange = {Func.Entry, Func.Entry + 0x20};
  EH.Kind = RuntimeFunctionKind::Primary;
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  Func.ExceptionMetadata = EH;

  llvm::LLVMContext Ctx;
  auto Module = MedLLVMEmitter().emit({Func}, Ctx, "eh-patch-safe", Arch::X64,
                                      {}, nullptr, BinaryFormat::COFF);
  ASSERT_NE(Module, nullptr);
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Base = 0x140000000;
  Image.ExceptionMetadata.Functions.push_back(std::move(EH));
  Image.ExceptionMetadata.rebuildIndex();

  auto Plan = planCOFFExceptionPatch(*Module, Image, Arch::X64);
  ASSERT_TRUE(static_cast<bool>(Plan)) << llvm::toString(Plan.takeError());
  ASSERT_EQ(Plan->ExceptionFunctionEntries.size(), 1u);
  EXPECT_EQ(Plan->ExceptionFunctionEntries[0], Func.Entry);

  Image.DynInfo.GuardFlags = 0x00800000u; // IMAGE_GUARD_XFG_ENABLED
  auto UnsupportedGuardPlan = planCOFFExceptionPatch(*Module, Image, Arch::X64);
  ASSERT_FALSE(static_cast<bool>(UnsupportedGuardPlan));
  EXPECT_NE(llvm::toString(UnsupportedGuardPlan.takeError())
                .find("guard instrumentation mode"),
            std::string::npos);
}

TEST(COFFExceptionPatch, ResolvesExecutablePersonalityThunkInsteadOfIATData) {
  BinaryImage Image;
  Image.Base = 0x140000000;
  Segment Code;
  Code.VA = Image.Base + 0x1000;
  Code.Size = 1;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Code.Data = {0xc3};
  Image.Segments.push_back(std::move(Code));

  ExceptionFunction EH;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  EH.PersonalityVA = Image.Base + 0x1000;
  Image.ExceptionMetadata.Functions.push_back(EH);

  EXPECT_EQ(findCOFFExceptionPersonalityVA(Image, "__C_specific_handler"),
            EH.PersonalityVA);
  EXPECT_EQ(findCOFFExceptionPersonalityVA(Image, "\01__C_specific_handler"),
            EH.PersonalityVA);
  EXPECT_FALSE(findCOFFExceptionPersonalityVA(Image, "__CxxFrameHandler3"));

  Image.Segments.front().Flags = SegmentFlags::Readable;
  EXPECT_FALSE(findCOFFExceptionPersonalityVA(Image, "__C_specific_handler"));
}

TEST(COFFExceptionPatch, RejectsLanguageGraphWithoutNativeWinEH) {
  MedFunc Func;
  Func.Entry = 0x140001000;
  Func.Name = "sub_140001000";
  MedBlock Block;
  Block.Id = 0;
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = Func.Entry;
  Block.Ops.push_back(Return);
  Func.Blocks.push_back(std::move(Block));

  ExceptionFunction EH;
  EH.CodeRange = {Func.Entry, Func.Entry + 0x20};
  EH.Kind = RuntimeFunctionKind::Primary;
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  EH.SEH.emplace();
  Func.ExceptionMetadata = EH;

  llvm::LLVMContext Ctx;
  auto Module = MedLLVMEmitter().emit({Func}, Ctx, "eh-patch-reject", Arch::X64,
                                      {}, nullptr, BinaryFormat::COFF);
  ASSERT_NE(Module, nullptr);
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Base = 0x140000000;
  Image.ExceptionMetadata.Functions.push_back(std::move(EH));
  Image.ExceptionMetadata.rebuildIndex();

  auto Plan = planCOFFExceptionPatch(*Module, Image, Arch::X64);
  ASSERT_FALSE(static_cast<bool>(Plan));
  std::string Message = llvm::toString(Plan.takeError());
  EXPECT_NE(Message.find("native WinEH lowering is unavailable"),
            std::string::npos);
}

TEST(COFFExceptionIR, SplitsProtectedRangesAndKeepsEdgesSeparate) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  Segment Text;
  Text.VA = Img.Base + 0x1000;
  Text.Size = 4;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = {0x90, 0x90, 0x90, 0xc3}; // nop; nop; nop; ret
  Img.Segments.push_back(std::move(Text));

  ExceptionFunction EH;
  EH.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1004};
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  SEHExceptionInfo SEH;
  SEHScopeRecord Scope;
  Scope.GuardedRange = {Img.Base + 0x1001, Img.Base + 0x1003};
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = Img.Base + 0x1003;
  SEH.Scopes.push_back(Scope);
  EH.SEH = std::move(SEH);
  Img.ExceptionMetadata.Functions.push_back(std::move(EH));
  Img.ExceptionMetadata.rebuildIndex();

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  CFGBuilder Builder;
  LowFunc Func = Builder.build(Img, Dec, Img.Base + 0x1000, "seh_cfg");
  ASSERT_TRUE(Func.ExceptionMetadata.has_value());
  ASSERT_EQ(Func.Blocks.size(), 3u);
  EXPECT_EQ(Func.Blocks[0].StartAddr, Img.Base + 0x1000);
  EXPECT_EQ(Func.Blocks[1].StartAddr, Img.Base + 0x1001);
  EXPECT_EQ(Func.Blocks[2].StartAddr, Img.Base + 0x1003);
  ASSERT_EQ(Func.Blocks[1].ExceptionalSuccs.size(), 1u);
  EXPECT_EQ(Func.Blocks[1].ExceptionalSuccs[0].Kind,
            ExceptionalEdgeKind::SEHHandler);
  EXPECT_EQ(Func.Blocks[1].ExceptionalSuccs[0].BlockId, 2);
  EXPECT_EQ(Func.Blocks[1].Succs.size(), 1u);
  EXPECT_EQ(Func.Blocks[1].Succs[0], 2);
}

TEST(COFFExceptionIR, DecompileRetainsFaithfulCxxAnnotation) {
  HighFunc Func;
  Func.Entry = 0x140001000;
  Func.Name = "cxx_annotation_test";
  Func.ReturnType = NdType::makeInt(8);
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = HighExpr::makeConst(0, 8);
  Func.Body.push_back(std::move(Return));

  ExceptionFunction EH;
  EH.CodeRange = {Func.Entry, Func.Entry + 0x40};
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.Personality = ExceptionPersonality::CxxFrameHandler3;
  CxxExceptionInfo Cxx;
  Cxx.MaxState = 2;
  Cxx.UnwindMap.push_back({-1, Func.Entry + 0x30});
  Cxx.UnwindMap.push_back({0, Func.Entry + 0x34});
  Cxx.UnwindMap.back().Kind =
      CxxUnwindAction::ActionKind::DestructorWithObjectPointer;
  Cxx.UnwindMap.back().ObjectOffset = -16;
  CxxTryBlock Try;
  Try.TryLow = 0;
  Try.TryHigh = 0;
  Try.CatchHigh = 1;
  CxxCatchHandler Catch;
  Catch.Adjectives = 0x40;
  Catch.TypeDescriptorVA = 0x140003000;
  Catch.CatchObjectOffset = -32;
  Catch.HandlerVA = Func.Entry + 0x20;
  Catch.ParentFrameOffset = -8;
  Catch.ContinuationVAs.push_back(Func.Entry + 0x38);
  Try.Handlers.push_back(Catch);
  Cxx.TryBlocks.push_back(std::move(Try));
  EH.Cxx = std::move(Cxx);
  Func.ExceptionMetadata = std::move(EH);

  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(HighCEmitter().emit({Func}, OS));
  OS.flush();
  EXPECT_NE(Source.find("neverd.exception: encoding=x64-unwind-v1"),
            std::string::npos);
  EXPECT_NE(Source.find("personality=__CxxFrameHandler3"), std::string::npos);
  EXPECT_NE(Source.find("cxx.try[0]"), std::string::npos);
  EXPECT_NE(Source.find("handler=0x140001020"), std::string::npos);
  EXPECT_NE(Source.find("kind=destructor-object-pointer"), std::string::npos);
  EXPECT_NE(Source.find("object_offset=-16"), std::string::npos);
  EXPECT_NE(Source.find("adjectives=0x40"), std::string::npos);
  EXPECT_NE(Source.find("parent_frame_offset=-8"), std::string::npos);
  EXPECT_NE(Source.find("continuations=0x140001038"), std::string::npos);
}

} // namespace
