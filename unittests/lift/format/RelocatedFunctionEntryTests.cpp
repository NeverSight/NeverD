//===- RelocatedFunctionEntryTests.cpp - Shared callable evidence --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/FuncDetector.h"
#include "neverd/loader/COFF/ControlFlowEntries.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/IR/Verifier.h"

using namespace neverd;

namespace {

BinaryImage pointerImage(Arch Target, BinaryFormat Format) {
  BinaryImage Img;
  Img.Arch = Target;
  Img.Format = Format;
  Img.Bits = Target == Arch::X86 || Target == Arch::ARM ? Bitness::Bits32
                                                        : Bitness::Bits64;
  if (Target == Arch::ARM)
    Img.Mode = InstructionMode::Thumb;
  Img.Entry = 0x1000;
  Segment Code;
  Code.VA = 0x1000;
  Code.Size = 0x80;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Code.Data.assign(Code.Size, 0xcc);
  if (Target == Arch::X86 || Target == Arch::X64) {
    Code.Data[0] = 0xc3;
    // movss xmm0, [rdx]; ret: no stack frame or strict prologue byte.
    const uint8_t Leaf[] = {0xf3, 0x0f, 0x10, 0x02, 0xc3};
    std::copy(std::begin(Leaf), std::end(Leaf), Code.Data.begin() + 0x40);
  } else if (Target == Arch::AArch64) {
    writeLE<uint32_t>(Code.Data.data(), 0xd65f03c0);        // ret
    writeLE<uint32_t>(Code.Data.data() + 0x40, 0x52800040); // mov w0, #2
    writeLE<uint32_t>(Code.Data.data() + 0x44, 0xd65f03c0);
  } else {
    writeLE<uint16_t>(Code.Data.data(), 0x4770);        // bx lr
    writeLE<uint16_t>(Code.Data.data() + 0x40, 0x2002); // movs r0, #2
    writeLE<uint16_t>(Code.Data.data() + 0x42, 0x4770);
  }
  Img.Segments.push_back(std::move(Code));
  Segment Data;
  Data.VA = 0x2000;
  Data.Size = 0x10;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(Data.Size);
  const va_t Address = Target == Arch::ARM ? 0x1041 : 0x1040;
  if (Img.is64Bit())
    writeLE<uint64_t>(Data.Data.data(), Address);
  else
    writeLE<uint32_t>(Data.Data.data(), Address);
  Img.Segments.push_back(std::move(Data));
  Img.CodePtrRelocSlots.insert(0x2000);
  return Img;
}

MedFunc pointerSource(const BinaryImage &Img) {
  MedFunc F;
  F.Entry = 0x1000;
  F.Name = "pointer_source";
  const unsigned Width = Img.getPointerSize();
  F.ReturnType = NdType::makeInt(Width);
  F.Blocks.emplace_back();
  F.Blocks[0].Id = 0;
  F.Blocks[0].StartAddr = F.Entry;
  F.Blocks[0].EndAddr = F.Entry + 4;
  MedOp Copy;
  Copy.Opcode = NdOp::COPY;
  Copy.Output.Kind = MedVar::Reg;
  Copy.Output.TheArch = Img.Arch;
  Copy.Output.RegOff = getTargetRegInfo(Img.Arch).IntReturnReg;
  Copy.Output.Id = 1;
  Copy.Output.SSAVer = 1;
  Copy.Output.Size = Width;
  Copy.addInput(
      MedVar::makeConst(0x2000, Width, ConstantAddressProvenance::DataAddress));
  F.Blocks[0].Ops.push_back(Copy);
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.addInput(Copy.Output);
  F.Blocks[0].Ops.push_back(Return);
  return F;
}

void addEntryEvidence(BinaryImage &Img, va_t Address, unsigned Evidence) {
  switch (Evidence) {
  case 0:
    Img.RuntimeFunctionAddrs.insert(Address);
    break;
  case 1:
    Img.VerifiedFunctionEntries.insert(Address);
    break;
  case 2: {
    auto Sym = Symbol::makeFunc(Address, 8);
    Sym.Name = "metadata_leaf";
    Img.Symbols.push_back(std::move(Sym));
    break;
  }
  case 3:
    Img.KnownCodeRanges.emplace_back(Address, Address + 8);
    break;
  case 4:
    Img.ImportStubRanges.emplace_back(Address, Address + 8);
    break;
  }
}

TEST(FunctionEntryAdmission, ContinuationsCannotBeReauthenticatedByMetadata) {
  for (Arch Target : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      for (unsigned Evidence = 0; Evidence != 5; ++Evidence) {
        SCOPED_TRACE(::testing::Message()
                     << static_cast<int>(Target) << ':'
                     << static_cast<int>(Format) << ':' << Evidence);
        auto Img = pointerImage(Target, Format);
        addEntryEvidence(Img, 0x1040, Evidence);
        ASSERT_TRUE(Img.hasAuthenticatedFunctionEntryAt(0x1040));
        Img.ContinuationCodeStarts.insert(0x1040);
        EXPECT_FALSE(Img.hasAuthenticatedFunctionEntryAt(0x1040));
        EXPECT_FALSE(Img.hasKnownFunctionEntryAt(0x1040));
      }
    }
  }
}

TEST(FunctionEntryAdmission, DiscoveryCannotRepublishContinuationPointers) {
  for (Arch Target : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      SCOPED_TRACE(::testing::Message() << static_cast<int>(Target) << ':'
                                        << static_cast<int>(Format));
      auto Img = pointerImage(Target, Format);
      Img.ContinuationCodeStarts.insert(0x1040);
      Decoder Dec;
      ASSERT_TRUE(Dec.init(Img));
      const auto Found = FuncDetector().detect(Img, Dec);
      EXPECT_TRUE(std::none_of(Found.begin(), Found.end(), [](const auto &F) {
        return F.first == 0x1040;
      }));
      EXPECT_FALSE(Img.VerifiedFunctionEntries.count(0x1040));
    }
  }
}

TEST(FunctionEntryAdmission, DataObjectsCannotBeReauthenticatedByMetadata) {
  for (Arch Target : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      for (unsigned Evidence = 0; Evidence != 5; ++Evidence) {
        SCOPED_TRACE(::testing::Message()
                     << static_cast<int>(Target) << ':'
                     << static_cast<int>(Format) << ':' << Evidence);
        auto Img = pointerImage(Target, Format);
        addEntryEvidence(Img, 0x1040, Evidence);
        ASSERT_TRUE(Img.hasAuthenticatedFunctionEntryAt(0x1040));
        Img.DataInCode.insert(0x1040);
        EXPECT_FALSE(Img.hasAuthenticatedFunctionEntryAt(0x1040));
        EXPECT_FALSE(Img.hasKnownFunctionEntryAt(0x1040));
        EXPECT_FALSE(RelocatedFunctionEntryVerifier(Img).accepts(0x1040));
      }
    }
  }
}

TEST(FunctionEntryAdmission, LLVMRejectsNamedAndRuntimeContinuationEntries) {
  for (Arch Target : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      for (unsigned Evidence : {0u, 2u}) {
        SCOPED_TRACE(::testing::Message()
                     << static_cast<int>(Target) << ':'
                     << static_cast<int>(Format) << ':' << Evidence);
        auto Img = pointerImage(Target, Format);
        addEntryEvidence(Img, 0x1040, Evidence);
        Img.ContinuationCodeStarts.insert(0x1040);
        llvm::LLVMContext Context;
        auto Module = MedLLVMEmitter().emit({pointerSource(Img)}, Context,
                                            "named-continuation", Target, {},
                                            &Img, Format);
        EXPECT_EQ(Module, nullptr);
      }
    }
  }
}

TEST(FunctionEntryAdmission,
     MetadataDoesNotAuthorizeMisalignedInstructionStarts) {
  for (Arch Target : {Arch::ARM, Arch::AArch64}) {
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      for (unsigned Evidence = 0; Evidence != 5; ++Evidence) {
        SCOPED_TRACE(::testing::Message()
                     << static_cast<int>(Target) << ':'
                     << static_cast<int>(Format) << ':' << Evidence);
        auto Img = pointerImage(Target, Format);
        if (Target == Arch::ARM)
          Img.Mode = InstructionMode::ARM;
        addEntryEvidence(Img, 0x1042, Evidence);
        EXPECT_FALSE(Img.hasAuthenticatedFunctionEntryAt(0x1042));
        if (Evidence == 4)
          continue; // Import identity has a separate LLVM materialization path.
        if (Img.is64Bit())
          writeLE<uint64_t>(Img.Segments[1].Data.data(), 0x1042);
        else
          writeLE<uint32_t>(Img.Segments[1].Data.data(), 0x1042);
        llvm::LLVMContext Context;
        auto Module =
            MedLLVMEmitter().emit({pointerSource(Img)}, Context,
                                  "misaligned-entry", Target, {}, &Img, Format);
        EXPECT_EQ(Module, nullptr);
      }
    }
  }
}

TEST(FunctionEntryAdmission, NestedRangeStartsSupplyNoIndependentIdentity) {
  for (Arch Target : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      SCOPED_TRACE(::testing::Message() << static_cast<int>(Target) << ':'
                                        << static_cast<int>(Format));
      auto Img = pointerImage(Target, Format);
      Img.KnownCodeRanges = {{0x1020, 0x1070}, {0x1040, 0x1048}};
      EXPECT_FALSE(Img.hasKnownFunctionEntryAt(0x1040));
      EXPECT_FALSE(Img.hasAuthenticatedFunctionEntryAt(0x1040));
    }
  }
}

TEST(FunctionEntryAdmission,
     RestrictedPdataStillAuthenticatesIndependentEntries) {
  auto Img = pointerImage(Arch::X64, BinaryFormat::COFF);
  Img.COFFPDataRecords.push_back({0x1040, 0x1048, 0x3000, 0x4000});
  ASSERT_TRUE(Img.hasKnownFunctionEntryAt(0x1040));
  EXPECT_TRUE(Img.hasAuthenticatedFunctionEntryAt(0x1040));
  Img.ContinuationCodeStarts.insert(0x1040);
  EXPECT_FALSE(Img.hasKnownFunctionEntryAt(0x1040));
  EXPECT_FALSE(Img.hasAuthenticatedFunctionEntryAt(0x1040));
}

TEST(FunctionEntryAdmission,
     ThumbRuntimeAndContinuationQueriesShareOneIdentity) {
  for (BinaryFormat Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
    auto Img = pointerImage(Arch::ARM, Format);
    Img.CodePtrRelocSlots.clear();
    ASSERT_TRUE(Img.recordRuntimeFunction(0x1041));
    EXPECT_TRUE(Img.isRuntimeFunctionAt(0x1040));
    EXPECT_TRUE(Img.hasKnownFunctionEntryAt(0x1040));
    EXPECT_TRUE(Img.hasAuthenticatedFunctionEntryAt(0x1040));
    auto Sym = Symbol::makeFunc(0x1041, 4);
    Img.Symbols.push_back(std::move(Sym));
    Img.ContinuationCodeStarts.insert(0x1040);
    EXPECT_TRUE(Img.getFunctionSymbols().empty());
    EXPECT_FALSE(Img.hasAuthenticatedFunctionEntryAt(0x1041));
    EXPECT_FALSE(Img.hasKnownFunctionEntryAt(0x1041));
  }
}

TEST(FunctionEntryAdmission, UnwindFrameStartsAreNotAlwaysFunctionEntries) {
  for (RuntimeFunctionKind Kind :
       {RuntimeFunctionKind::Primary, RuntimeFunctionKind::Chained,
        RuntimeFunctionKind::Fragment}) {
    auto Img = pointerImage(Arch::X64, BinaryFormat::COFF);
    Img.CodePtrRelocSlots.clear();
    ExceptionFunction F;
    F.Kind = Kind;
    F.CodeRange = {0x1040, 0x1048};
    Img.ExceptionMetadata.Functions.push_back(F);
    Img.ExceptionMetadata.rebuildIndex();
    const bool Independent = Kind == RuntimeFunctionKind::Primary;
    EXPECT_EQ(Img.hasKnownFunctionEntryAt(0x1040), Independent);
    EXPECT_EQ(Img.hasAuthenticatedFunctionEntryAt(0x1040), Independent);
  }
  auto Img = pointerImage(Arch::AArch64, BinaryFormat::ELF);
  Img.CodePtrRelocSlots.clear();
  ExceptionFunction F;
  F.CodeRange = {0x1044, 0x1048};
  F.FunctionEntry = 0x1040; // A landing instruction precedes the unwind frame.
  Img.ExceptionMetadata.Functions.push_back(F);
  Img.ExceptionMetadata.rebuildIndex();
  EXPECT_TRUE(Img.hasKnownFunctionEntryAt(0x1040));
  EXPECT_TRUE(Img.hasAuthenticatedFunctionEntryAt(0x1040));
  EXPECT_FALSE(Img.hasKnownFunctionEntryAt(0x1044));
  EXPECT_FALSE(Img.hasAuthenticatedFunctionEntryAt(0x1044));
}

TEST(FunctionEntryAdmission, MalformedUnwindCannotSupplyEntryEvidence) {
  auto Img = pointerImage(Arch::X64, BinaryFormat::COFF);
  Img.CodePtrRelocSlots.clear();
  Img.Entry = 0;
  ExceptionFunction F;
  F.CodeRange = {0x1040, 0x1048};
  F.ParseStatus = ExceptionParseStatus::Malformed;
  Img.ExceptionMetadata.Functions.push_back(F);
  Img.ExceptionMetadata.rebuildIndex();
  EXPECT_FALSE(Img.hasKnownFunctionEntryAt(0x1040));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Img));
  const auto Found = FuncDetector().detect(Img, Dec);
  EXPECT_TRUE(std::none_of(Found.begin(), Found.end(), [](const auto &Entry) {
    return Entry.first == 0x1040;
  }));
}

TEST(RelocatedFunctionEntries, LLVMConsumesVerifiedEntriesWithoutSymbols) {
  for (Arch Target : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      SCOPED_TRACE(::testing::Message() << static_cast<int>(Target) << ':'
                                        << static_cast<int>(Format));
      auto Img = pointerImage(Target, Format);
      Img.VerifiedFunctionEntries.insert(0x1040);
      llvm::LLVMContext Context;
      auto Module = MedLLVMEmitter().emit({pointerSource(Img)}, Context,
                                          "entry", Target, {}, &Img, Format);
      ASSERT_NE(Module, nullptr);
      EXPECT_FALSE(llvm::verifyModule(*Module));
      const auto *Leaf = Module->getFunction("sub_1040");
      ASSERT_NE(Leaf, nullptr);
      EXPECT_TRUE(Leaf->isDeclaration());
      EXPECT_FALSE(Leaf->use_empty());
    }
  }
}

TEST(FunctionEntryAdmission, ImportVeneersCannotOverrideStructuralRejection) {
  for (Arch Target : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      for (bool Rejected : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << static_cast<int>(Target) << ':'
                     << static_cast<int>(Format) << ':' << Rejected);
        auto Img = pointerImage(Target, Format);
        Img.Imports.push_back({"fixture", "imported_leaf", 0, 0x2008});
        ASSERT_TRUE(Img.recordImportStub(0x1040, 0));
        if (Rejected)
          Img.ContinuationCodeStarts.insert(0x1040);
        EXPECT_EQ(Img.findImportStubAt(0x1040) != nullptr, !Rejected);
        const auto Names = Img.getImportAddressNames();
        std::vector<std::pair<va_t, std::string>> Imports(Names.begin(),
                                                          Names.end());
        llvm::LLVMContext Context;
        auto Module =
            MedLLVMEmitter().emit({pointerSource(Img)}, Context, "import-entry",
                                  Target, Imports, &Img, Format);
        if (Rejected) {
          EXPECT_EQ(Module, nullptr);
        } else {
          ASSERT_NE(Module, nullptr);
          EXPECT_FALSE(llvm::verifyModule(*Module));
          const auto *Imported = Module->getNamedValue("imported_leaf");
          ASSERT_NE(Imported, nullptr);
          EXPECT_TRUE(Imported->isDeclaration());
        }
      }
    }
  }
}

TEST(FunctionEntryAdmission, ThumbVeneerRegistrationUsesInstructionIdentity) {
  auto Img = pointerImage(Arch::ARM, BinaryFormat::COFF);
  Img.Imports.push_back({"fixture", "imported_leaf", 0, 0x2008});
  ASSERT_TRUE(Img.recordImportStub(0x1041, 0));
  EXPECT_NE(Img.findImportStubAt(0x1040), nullptr);
  EXPECT_NE(Img.findImportStubAt(0x1041), nullptr);
  EXPECT_TRUE(Img.hasAuthenticatedFunctionEntryAt(0x1040));
  EXPECT_TRUE(Img.getImportAddressNames().count(0x1040));
  Img.ContinuationCodeStarts.insert(0x1041);
  EXPECT_EQ(Img.findImportStubAt(0x1040), nullptr);
  EXPECT_FALSE(Img.getImportAddressNames().count(0x1040));
  EXPECT_FALSE(RelocatedFunctionEntryVerifier(Img).accepts(0x1040));
}

TEST(FunctionEntryAdmission, ThunkDecodeCannotCrossIntoAdjacentData) {
  auto Img = pointerImage(Arch::X64, BinaryFormat::COFF);
  Img.Imports.push_back({"fixture", "imported_leaf", 0, 0x2008});
  Img.Segments[0].Data[0x40] = 0xff;
  Img.Segments[0].Data[0x41] = 0x25;
  writeLE<uint32_t>(Img.Segments[0].Data.data() + 0x42, 0x2008 - 0x1046);
  ASSERT_NE(Img.findImportStubAt(0x1040), nullptr);
  Section Code;
  Code.VA = 0x1000;
  Code.Size = Code.FileSz = 0x43;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(Code);
  Section Data;
  Data.VA = 0x1043;
  Data.Size = Data.FileSz = 0x3d;
  Data.Flags = SegmentFlags::Readable;
  Img.Sections.push_back(Data);
  EXPECT_EQ(Img.findImportStubAt(0x1040), nullptr);
  EXPECT_FALSE(Img.hasAuthenticatedFunctionEntryAt(0x1040));
}

TEST(FunctionEntryAdmission, NamedContinuationJumpsRetainTheParentFrame) {
  auto Img = pointerImage(Arch::X64, BinaryFormat::COFF);
  Img.Segments[0].Data[0] = 0xe9; // jmp 0x1040
  writeLE<uint32_t>(Img.Segments[0].Data.data() + 1, 0x3b);
  Img.Symbols.push_back(Symbol::makeFunc(0x1040, 8));
  Img.ContinuationCodeStarts.insert(0x1040);
  const std::set<va_t> OldEntries{0x1000, 0x1040};
  CFGBuilder Builder;
  Builder.setKnownFuncEntries(&OldEntries);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Img));
  const LowFunc F = Builder.build(Img, Dec, 0x1000, "parent");
  bool SawContinuationReturn = false;
  for (const auto &Block : F.Blocks)
    for (const auto &Op : Block.Ops) {
      EXPECT_NE(Op.Opcode, NdOp::CALL);
      SawContinuationReturn |= Op.Opcode == NdOp::RETURN && Op.Addr == 0x1044;
    }
  EXPECT_TRUE(SawContinuationReturn);
}

TEST(FunctionEntryAdmission, PrimaryEntryIndexTracksEntryAndKindUpdates) {
  auto Img = pointerImage(Arch::AArch64, BinaryFormat::ELF);
  Img.CodePtrRelocSlots.clear();
  ExceptionFunction F;
  F.CodeRange = {0x1044, 0x1048};
  F.FunctionEntry = 0x1040;
  Img.ExceptionMetadata.Functions.push_back(F);
  Img.ExceptionMetadata.rebuildIndex();
  EXPECT_TRUE(Img.hasKnownFunctionEntryAt(0x1040));
  Img.ExceptionMetadata.Functions[0].FunctionEntry = 0;
  Img.ExceptionMetadata.rebuildIndex();
  EXPECT_FALSE(Img.hasKnownFunctionEntryAt(0x1040));
  EXPECT_TRUE(Img.hasKnownFunctionEntryAt(0x1044));
  Img.ExceptionMetadata.Functions[0].Kind = RuntimeFunctionKind::Fragment;
  Img.ExceptionMetadata.rebuildIndex();
  EXPECT_FALSE(Img.hasKnownFunctionEntryAt(0x1044));
  EXPECT_FALSE(Img.hasAuthenticatedFunctionEntryAt(0x1044));
}

TEST(FunctionEntryAdmission, NextEntryQueriesSkipThumbAliasesAndContinuations) {
  auto Img = pointerImage(Arch::ARM, BinaryFormat::ELF);
  Img.RuntimeFunctionAddrs = {0x1041, 0x1051, 0x1061};
  Img.ContinuationCodeStarts.insert(0x1050);
  EXPECT_EQ(Img.nextKnownFunctionEntryAfter(0x1040), 0x1060u);
  EXPECT_EQ(Img.nextKnownFunctionEntryAfter(0x1041), 0x1060u);
}

TEST(FunctionEntryAdmission, NextUnwindEntryUsesPrimaryInstructionAddresses) {
  auto Img = pointerImage(Arch::AArch64, BinaryFormat::ELF);
  ExceptionFunction First;
  First.CodeRange = {0x1044, 0x1048};
  First.FunctionEntry = 0x1040;
  ExceptionFunction Fragment;
  Fragment.CodeRange = {0x1050, 0x1058};
  Fragment.Kind = RuntimeFunctionKind::Fragment;
  ExceptionFunction Next;
  Next.CodeRange = {0x1060, 0x1068};
  Img.ExceptionMetadata.Functions = {First, Fragment, Next};
  Img.ExceptionMetadata.rebuildIndex();
  EXPECT_EQ(Img.nextKnownFunctionEntryAfter(0x1000), 0x1040u);
  EXPECT_EQ(Img.nextKnownFunctionEntryAfter(0x1040), 0x1060u);
}

TEST(FunctionEntryAdmission, NextCompactPdataEntrySkipsContinuations) {
  auto Img = pointerImage(Arch::X64, BinaryFormat::COFF);
  Img.COFFPDataRecords = {{0x1040, 0x1048, 0x3000, 0x4000},
                          {0x1050, 0x1058, 0x3010, 0x400c},
                          {0x1060, 0x1068, 0x3020, 0x4018}};
  Img.ContinuationCodeStarts.insert(0x1050);
  EXPECT_EQ(Img.nextKnownFunctionEntryAfter(0x1040), 0x1060u);
}

TEST(FunctionEntryAdmission, LLVMKeepsFrameAddressesRelativeToThePrimaryEntry) {
  for (BinaryFormat Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
    SCOPED_TRACE(static_cast<int>(Format));
    auto Img = pointerImage(Arch::AArch64, Format);
    writeLE<uint64_t>(Img.Segments[1].Data.data(), 0x1044);
    ExceptionFunction F;
    F.CodeRange = {0x1044, 0x1048};
    F.FunctionEntry = 0x1040;
    Img.ExceptionMetadata.Functions.push_back(F);
    Img.ExceptionMetadata.rebuildIndex();
    if (Format == BinaryFormat::COFF)
      Img.COFFPDataRecords.push_back({0x1044, 0x1048, 0x3000, 0x4000});
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({pointerSource(Img)}, Context, "frame-interior",
                              Img.Arch, {}, &Img, Img.Format);
    ASSERT_NE(Module, nullptr);
    EXPECT_FALSE(llvm::verifyModule(*Module));
    EXPECT_NE(Module->getFunction("sub_1040"), nullptr);
    EXPECT_EQ(Module->getFunction("sub_1044"), nullptr);
  }
}

TEST(FunctionEntryAdmission,
     LLVMCompactPdataCannotReauthenticateContinuations) {
  for (Arch Target : {Arch::X64, Arch::AArch64}) {
    for (bool Continuation : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << static_cast<int>(Target) << ':' << Continuation);
      auto Img = pointerImage(Target, BinaryFormat::COFF);
      Img.COFFPDataRecords.push_back({0x1040, 0x1048, 0x3000, 0x4000});
      if (Continuation)
        Img.ContinuationCodeStarts.insert(0x1040);
      llvm::LLVMContext Context;
      auto Module = MedLLVMEmitter().emit({pointerSource(Img)}, Context,
                                          "compact-frame-entry", Target, {},
                                          &Img, Img.Format);
      if (Continuation) {
        EXPECT_EQ(Module, nullptr);
      } else {
        ASSERT_NE(Module, nullptr);
        EXPECT_FALSE(llvm::verifyModule(*Module));
        EXPECT_NE(Module->getFunction("sub_1040"), nullptr);
      }
    }
  }
}

TEST(RelocatedFunctionEntries, RecoversRelocatedLeavesWithoutInstructionSweep) {
  for (Arch Target : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      for (bool EntryPoint : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << static_cast<int>(Target) << ':'
                     << static_cast<int>(Format) << ':' << EntryPoint);
        auto Img = pointerImage(Target, Format);
        if (!EntryPoint)
          Img.Entry = 0;
        const auto Before = Img.VerifiedFunctionEntries;
        RelocatedFunctionEntryVerifier Verifier(Img);
        EXPECT_TRUE(Verifier.accepts(0x1040));
        EXPECT_TRUE(Verifier.accepts(0x1040));
        EXPECT_EQ(Before, Img.VerifiedFunctionEntries);
        llvm::LLVMContext Context;
        auto Module =
            MedLLVMEmitter().emit({pointerSource(Img)}, Context, "discovered",
                                  Target, {}, &Img, Format);
        ASSERT_NE(Module, nullptr);
        EXPECT_FALSE(llvm::verifyModule(*Module));
      }
    }
  }
}

TEST(RelocatedFunctionEntries, RejectsUnprovedInvalidAndInteriorTargets) {
  for (unsigned Case = 0; Case != 6; ++Case) {
    SCOPED_TRACE(Case);
    auto Img = pointerImage(Arch::X64, BinaryFormat::COFF);
    switch (Case) {
    case 0:
      Img.CodePtrRelocSlots.clear(); // a numeric coincidence is no relocation
      break;
    case 1:
      Img.DataInCode.insert(0x1040);
      break;
    case 2:
      Img.Segments[0].Data[0x40] = 0x0f;
      Img.Segments[0].Data[0x41] = 0x04; // invalid opcode
      break;
    case 3: {
      auto Owner = Symbol::makeFunc(0x1020);
      Owner.Size = 0x30;
      Img.Symbols.push_back(Owner);
      break;
    }
    case 4: {
      // Printable UTF-16 bytes and traps alone do not prove a callable entry.
      const uint8_t Text[] = {'n', 0, 't', 0, 'f', 0, 's', 0, 0, 0};
      std::copy(std::begin(Text), std::end(Text),
                Img.Segments[0].Data.begin() + 0x40);
      break;
    }
    case 5:
      Img.KnownCodeRanges.emplace_back(0x1020, 0x1050);
      break;
    }
    EXPECT_FALSE(RelocatedFunctionEntryVerifier(Img).accepts(0x1040));
    EXPECT_FALSE(Img.VerifiedFunctionEntries.count(0x1040));
  }
  auto Img = pointerImage(Arch::AArch64, BinaryFormat::ELF);
  writeLE<uint64_t>(Img.Segments[1].Data.data(), 0x1041);
  EXPECT_FALSE(RelocatedFunctionEntryVerifier(Img).accepts(0x1041));
  EXPECT_FALSE(Img.VerifiedFunctionEntries.count(0x1041));
}

TEST(RelocatedFunctionEntries, CallableProofDoesNotTreatBudgetAsSuccess) {
  for (unsigned Count : {80u, 400u}) {
    auto Img = pointerImage(Arch::X64, BinaryFormat::COFF);
    Img.Segments[0].Size = 0x400;
    Img.Segments[0].Data.resize(0x400, 0xcc);
    std::fill_n(Img.Segments[0].Data.begin() + 0x40, Count, 0x90);
    Img.Segments[0].Data[0x40 + Count] = 0xc3;
    EXPECT_EQ(RelocatedFunctionEntryVerifier(Img).accepts(0x1040), Count == 80);
    EXPECT_TRUE(Img.VerifiedFunctionEntries.empty());
  }
}

TEST(RelocatedFunctionEntries, RelocationsDoNotAuthenticateContinuationStarts) {
  for (Arch Target : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      SCOPED_TRACE(::testing::Message() << static_cast<int>(Target) << ':'
                                        << static_cast<int>(Format));
      auto Img = pointerImage(Target, Format);
      Img.ContinuationCodeStarts.insert(0x1040);
      Img.KnownCodeRanges.emplace_back(0x1040, 0x1048);
      EXPECT_FALSE(RelocatedFunctionEntryVerifier(Img).accepts(0x1040));
      EXPECT_TRUE(Img.VerifiedFunctionEntries.empty());
      llvm::LLVMContext Context;
      auto Module =
          MedLLVMEmitter().emit({pointerSource(Img)}, Context, "continuation",
                                Target, {}, &Img, Format);
      EXPECT_EQ(Module, nullptr);
    }
  }
}

TEST(RelocatedFunctionEntries, EmitterDoesNotRetainProofsAcrossImageChanges) {
  auto Img = pointerImage(Arch::X64, BinaryFormat::COFF);
  MedLLVMEmitter Emitter;
  llvm::LLVMContext Context;
  auto First = Emitter.emit({pointerSource(Img)}, Context, "first", Arch::X64,
                            {}, &Img, Img.Format);
  ASSERT_NE(First, nullptr);
  Img.Segments[0].Data[0x40] = 0x0f;
  Img.Segments[0].Data[0x41] = 0x04;
  auto Second = Emitter.emit({pointerSource(Img)}, Context, "second", Arch::X64,
                             {}, &Img, Img.Format);
  EXPECT_EQ(Second, nullptr);
}

TEST(RelocatedFunctionEntries,
     SymbolIndexPreservesAliasesAndResetsPerEmission) {
  for (Arch Target : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    auto Img = pointerImage(Target, BinaryFormat::COFF);
    const va_t Entry = Target == Arch::ARM ? 0x1041 : 0x1040;
    for (const char *Name : {"first_alias", "last_alias"}) {
      auto Sym = Symbol::makeFunc(Entry, 8);
      Sym.Name = Name;
      Img.Symbols.push_back(std::move(Sym));
    }
    MedLLVMEmitter Emitter;
    llvm::LLVMContext Context;
    auto First = Emitter.emit({pointerSource(Img)}, Context, "first", Target,
                              {}, &Img, Img.Format);
    ASSERT_NE(First, nullptr);
    EXPECT_NE(First->getFunction("last_alias"), nullptr);
    EXPECT_EQ(First->getFunction("first_alias"), nullptr);
    Img.Symbols.back().Name = "renamed_alias";
    auto Second = Emitter.emit({pointerSource(Img)}, Context, "second", Target,
                               {}, &Img, Img.Format);
    ASSERT_NE(Second, nullptr);
    EXPECT_NE(Second->getFunction("renamed_alias"), nullptr);
    EXPECT_EQ(Second->getFunction("last_alias"), nullptr);
    EXPECT_FALSE(llvm::verifyModule(*Second, &llvm::errs()));
  }
}

BinaryImage guardedImage(Arch Target, unsigned Extra = 0) {
  auto Img = pointerImage(Target, BinaryFormat::COFF);
  if (Target == Arch::X86 || Target == Arch::X64) {
    // A valid function can forward through a register instead of returning.
    Img.Segments[0].Data[0x40] = 0xff; // jmp eax/rax
    Img.Segments[0].Data[0x41] = 0xe0;
  }
  Segment Metadata;
  Metadata.VA = 0x3002; // PE tables need not be four-byte aligned.
  Metadata.Size = Metadata.FileSz = 2 * (4 + Extra);
  Metadata.Flags = SegmentFlags::Readable;
  Metadata.Data.resize(Metadata.Size);
  writeLE<uint32_t>(Metadata.Data.data(), 0x1000);
  writeLE<uint32_t>(Metadata.Data.data() + 4 + Extra, 0x1040);
  Img.Segments.push_back(std::move(Metadata));
  for (auto &Seg : Img.Segments) {
    Seg.FileSz = Seg.Size;
    Section Sec;
    Sec.VA = Seg.VA;
    Sec.Size = Sec.FileSz = Seg.Size;
    Sec.Flags = Seg.Flags;
    Img.Sections.push_back(std::move(Sec));
  }
  Img.DynInfo.GuardFlags = 0x500 | (Extra << 28);
  Img.DynInfo.GuardCFFunctionTableRVA = 0x3002;
  Img.DynInfo.GuardCFFunctionCount = 2;
  return Img;
}

TEST(RelocatedFunctionEntries, CFGTargetsNeedUniqueCompleteFileStorage) {
  for (Arch Target : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    for (unsigned Case = 0; Case != 4; ++Case) {
      if (Case == 3 && Target != Arch::ARM && Target != Arch::AArch64)
        continue;
      SCOPED_TRACE(::testing::Message()
                   << static_cast<int>(Target) << ':' << Case);
      auto Img = guardedImage(Target);
      switch (Case) {
      case 0: // The target is in virtual storage, beyond the file bytes.
        Img.Segments[0].FileSz = Img.Sections[0].FileSz = 0x40;
        break;
      case 1:
        Img.Segments.push_back(Img.Segments[0]);
        break;
      case 2:
        Img.Sections.push_back(Img.Sections[0]);
        break;
      case 3: // Not enough bytes remain for even one instruction.
        Img.Segments[0].Size = Img.Segments[0].FileSz = 0x41;
        Img.Segments[0].Data.resize(0x41);
        Img.Sections[0].Size = Img.Sections[0].FileSz = 0x41;
        break;
      }
      auto Entries = readPEControlFlowEntries(Img);
      EXPECT_FALSE(bool(Entries));
      if (!Entries)
        EXPECT_FALSE(llvm::toString(Entries.takeError()).empty());
    }
  }
}

TEST(RelocatedFunctionEntries, CFGTargetIdentityDoesNotRequireImmutableCode) {
  for (auto Flags : {SegmentFlags::Executable, SegmentFlags::Readable |
                                                   SegmentFlags::Writable |
                                                   SegmentFlags::Executable}) {
    auto Img = guardedImage(Arch::X64);
    Img.Segments[0].Flags = Img.Sections[0].Flags = Flags;
    auto Entries = readPEControlFlowEntries(Img);
    ASSERT_TRUE(bool(Entries))
        << (Entries ? "" : llvm::toString(Entries.takeError()));
    EXPECT_EQ(*Entries, (std::vector<va_t>{0x1000, 0x1040}));
  }
}

TEST(RelocatedFunctionEntries, CFGUsesAddressSpecificARMInstructionUnits) {
  for (auto LocalMode : {InstructionMode::ARM, InstructionMode::Thumb}) {
    SCOPED_TRACE(static_cast<int>(LocalMode));
    auto Img = guardedImage(Arch::ARM);
    Img.Mode = InstructionMode::MixedARMThumb;
    Img.ARMCodeModeEntries = {{0x1000, InstructionMode::Thumb},
                              {0x1040, LocalMode}};
    // Only two target bytes are file-backed: enough for a Thumb instruction
    // unit, but not for an ARM unit in the same mixed-mode image.
    Img.Segments[0].FileSz = Img.Sections[0].FileSz = 0x42;
    auto Entries = readPEControlFlowEntries(Img);
    EXPECT_EQ(bool(Entries), LocalMode == InstructionMode::Thumb);
    if (!Entries)
      EXPECT_FALSE(llvm::toString(Entries.takeError()).empty());
  }
}

TEST(RelocatedFunctionEntries, CFGMetadataAuthenticatesIndirectTailEntries) {
  for (Arch Target : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    for (unsigned Extra : {0u, 1u}) {
      SCOPED_TRACE(::testing::Message()
                   << static_cast<int>(Target) << ':' << Extra);
      auto Img = guardedImage(Target, Extra);
      auto Entries = readPEControlFlowEntries(Img);
      ASSERT_TRUE(bool(Entries));
      EXPECT_EQ(*Entries, (std::vector<va_t>{0x1000, 0x1040}));
      EXPECT_TRUE(RelocatedFunctionEntryVerifier(Img).accepts(0x1040));
      EXPECT_TRUE(Img.VerifiedFunctionEntries.empty());
      llvm::LLVMContext Context;
      auto Module =
          MedLLVMEmitter().emit({pointerSource(Img)}, Context, "guarded",
                                Target, {}, &Img, Img.Format);
      ASSERT_NE(Module, nullptr);
      EXPECT_FALSE(llvm::verifyModule(*Module));
    }
  }
}

TEST(RelocatedFunctionEntries,
     CFGMetadataAuthenticatesEntriesWithoutRelocations) {
  for (Arch Target : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    for (bool HasMetadata : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << static_cast<int>(Target) << ':' << HasMetadata);
      auto Img = guardedImage(Target);
      Img.CodePtrRelocSlots.clear();
      if (!HasMetadata) {
        Img.DynInfo.GuardFlags = 0;
        Img.DynInfo.GuardCFFunctionTableRVA = 0;
        Img.DynInfo.GuardCFFunctionCount = 0;
      }
      EXPECT_EQ(RelocatedFunctionEntryVerifier(Img).accepts(0x1040),
                HasMetadata);
      auto Source = pointerSource(Img);
      // Keep the ABI return-register write. The observable return resolves
      // this completed address on every architecture.
      Source.Blocks[0].Ops[0].Inputs[0] = MedVar::makeConst(
          Target == Arch::ARM ? 0x1041 : 0x1040, Img.getPointerSize(),
          ConstantAddressProvenance::Address);
      llvm::LLVMContext Context;
      auto Module =
          MedLLVMEmitter().emit({Source}, Context, "cfg-no-relocations", Target,
                                {}, &Img, Img.Format);
      if (HasMetadata) {
        ASSERT_NE(Module, nullptr);
        EXPECT_FALSE(llvm::verifyModule(*Module));
        EXPECT_NE(Module->getFunction("sub_1040"), nullptr);
      } else {
        EXPECT_EQ(Module, nullptr);
      }
      EXPECT_TRUE(Img.VerifiedFunctionEntries.empty());
    }
  }
}

TEST(RelocatedFunctionEntries, CFGSuppressionDoesNotGrantCallableAuthority) {
  for (uint8_t Flag : {1, 2, 3}) {
    auto Img = guardedImage(Arch::X64, 1);
    Img.Segments.back().Data[9] = Flag;
    auto Entries = readPEControlFlowEntries(Img);
    ASSERT_TRUE(bool(Entries));
    EXPECT_EQ(*Entries, (std::vector<va_t>{0x1000}));
    EXPECT_FALSE(RelocatedFunctionEntryVerifier(Img).accepts(0x1040));
  }
}

TEST(RelocatedFunctionEntries,
     CFGMetadataDoesNotAuthenticateContinuationStarts) {
  for (Arch Target : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    SCOPED_TRACE(static_cast<int>(Target));
    auto Img = guardedImage(Target);
    Img.ContinuationCodeStarts.insert(0x1040);
    Img.KnownCodeRanges.emplace_back(0x1040, 0x1048);
    EXPECT_FALSE(RelocatedFunctionEntryVerifier(Img).accepts(0x1040));
    EXPECT_TRUE(Img.VerifiedFunctionEntries.empty());
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({pointerSource(Img)}, Context, "cfg-continuation",
                              Target, {}, &Img, Img.Format);
    EXPECT_EQ(Module, nullptr);
  }
}

TEST(RelocatedFunctionEntries, CFGMetadataRejectsMalformedWholeTables) {
  for (unsigned Case = 0; Case != 11; ++Case) {
    SCOPED_TRACE(Case);
    auto Img = guardedImage(Arch::X64, 1);
    switch (Case) {
    case 0: // duplicate
      writeLE<uint32_t>(Img.Segments.back().Data.data() + 5, 0x1000);
      break;
    case 1: // out of order: even the valid prefix supplies no proof
      writeLE<uint32_t>(Img.Segments.back().Data.data(), 0x1040);
      writeLE<uint32_t>(Img.Segments.back().Data.data() + 5, 0x1000);
      break;
    case 2:
      Img.Segments.back().FileSz--;
      break;
    case 3:
      Img.Segments.back().Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
      break;
    case 4:
      Img.DynInfo.GuardCFFunctionCount = UINT64_MAX;
      break;
    case 5:
      Img.DynInfo.GuardFlags = 0x500 | (2u << 28);
      break;
    case 6:
      Img.Segments.back().Data[9] = 4;
      break;
    case 7:
      writeLE<uint32_t>(Img.Segments.back().Data.data() + 5, 0x2000);
      break;
    case 8:
      Img.DynInfo.GuardFlags &= ~0x400u;
      break;
    case 9:
      Img.Base = InvalidVA - 0x100;
      break;
    case 10:
      Img.Sections.push_back(Img.Sections.back());
      break;
    }
    auto Entries = readPEControlFlowEntries(Img);
    EXPECT_FALSE(bool(Entries));
    if (!Entries)
      EXPECT_FALSE(llvm::toString(Entries.takeError()).empty());
  }
}

TEST(RelocatedFunctionEntries, CFGEntryRVAUsesTheLoadedImageBase) {
  auto Img = guardedImage(Arch::X64);
  Img.Base = 0x180000000;
  for (auto &Seg : Img.Segments)
    Seg.VA += Img.Base;
  for (auto &Sec : Img.Sections)
    Sec.VA += Img.Base;
  auto Entries = readPEControlFlowEntries(Img);
  ASSERT_TRUE(bool(Entries));
  EXPECT_EQ(*Entries,
            (std::vector<va_t>{Img.Base + 0x1000, Img.Base + 0x1040}));
}

TEST(RelocatedFunctionEntries,
     CFGEntryCannotOverrideDataOrInstructionAlignment) {
  for (Arch Target : {Arch::X64, Arch::AArch64}) {
    auto Img = guardedImage(Target);
    if (Target == Arch::X64)
      Img.DataInCode.insert(0x1040);
    else
      writeLE<uint32_t>(Img.Segments.back().Data.data() + 4, 0x1042);
    auto Entries = readPEControlFlowEntries(Img);
    EXPECT_FALSE(bool(Entries));
    if (!Entries)
      EXPECT_FALSE(llvm::toString(Entries.takeError()).empty());
  }
}

TEST(RelocatedFunctionEntries, CFGTableMayShareImmutableExecutableStorage) {
  auto Img = guardedImage(Arch::X64);
  const auto RX = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Segments.back().Flags = RX;
  Img.Sections.back().Flags = RX;
  auto Entries = readPEControlFlowEntries(Img);
  ASSERT_TRUE(bool(Entries));
  EXPECT_EQ(*Entries, (std::vector<va_t>{0x1000, 0x1040}));
  EXPECT_TRUE(RelocatedFunctionEntryVerifier(Img).accepts(0x1040));
}

} // namespace
