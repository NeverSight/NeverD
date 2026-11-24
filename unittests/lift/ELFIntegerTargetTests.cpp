//===- ELFIntegerTargetTests.cpp - RISC-V and PowerPC pipeline tests --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercise linked, stripped ELF inputs through identification, strict lifting,
/// ABI recovery, C emission and LLVM verification.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ArchSupport.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/SourceParameterPlacement.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/lift/IntegerRegisters.h"
#include "neverd/loader/ELF/ELFLoader.h"
#include "neverd/loader/ELF/ELFLoaderUtils.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <vector>

using namespace neverd;
namespace {
struct LinkedImage {
  static constexpr va_t Entry = 0x10100;
  bool Wide, Little;
  std::vector<uint8_t> Bytes = std::vector<uint8_t>(0x108);
  void put(size_t Offset, uint64_t V, unsigned N) {
    for (unsigned I = 0; I < N; ++I)
      Bytes[Offset + (Little ? I : N - 1 - I)] = V >> (8 * I);
  }
  explicit LinkedImage(Arch A)
      : Wide(A == Arch::RISCV64 || isPowerPC64(A)),
        Little(archLittleEndian(A)) {
    using namespace llvm::ELF;
    std::copy_n(ElfMagic, 4, Bytes.begin());
    Bytes[EI_CLASS] = Wide ? ELFCLASS64 : ELFCLASS32;
    Bytes[EI_DATA] = Little ? ELFDATA2LSB : ELFDATA2MSB;
    Bytes[EI_VERSION] = EV_CURRENT;
    unsigned Ptr = Wide ? 8 : 4, Header = Wide ? 64 : 52;
    put(16, ET_EXEC, 2);
    put(18, isRISCV(A) ? EM_RISCV : Wide ? EM_PPC64 : EM_PPC, 2);
    put(20, EV_CURRENT, 4);
    put(24, Entry, Ptr);
    put(24 + Ptr, Header, Ptr);
    put(Wide ? 48 : 36, isPowerPC64(A) ? 2 : 0, 4);
    put(Wide ? 52 : 40, Header, 2);
    put(Wide ? 54 : 42, Wide ? 56 : 32, 2);
    put(Wide ? 56 : 44, 1, 2);
    put(Wide ? 58 : 46, Wide ? 64 : 40, 2);
    put(Header, PT_LOAD, 4);
    put(Header + (Wide ? 4 : 24), PF_R | PF_X, 4);
    put(Header + (Wide ? 8 : 4), 0x100, Ptr);
    put(Header + (Wide ? 16 : 8), Entry, Ptr);
    put(Header + (Wide ? 24 : 12), Entry, Ptr);
    put(Header + (Wide ? 32 : 16), 8, Ptr);
    put(Header + (Wide ? 40 : 20), 8, Ptr);
    put(Header + (Wide ? 48 : 28), 0x100, Ptr);
    put(0x100, isRISCV(A) ? 0x00b50533 : 0x7c632214,
        4); // add a0,a0,a1 / add r3,r3,r4
    put(0x104, isRISCV(A) ? 0x00008067 : 0x4e800020, 4); // ret / blr
  }
  void riscvAttributes(llvm::StringRef ISA, uint8_t StackAlign = 0) {
    // ELF vendor section 'A', "riscv", Tag_File, Tag_RISCV_arch.
    const size_t Attr = Bytes.size(),
                 Size = 18 + ISA.size() + (StackAlign ? 2 : 0);
    Bytes.resize(Attr + Size);
    Bytes[Attr] = 'A';
    put(Attr + 1, Size - 1, 4);
    std::copy_n("riscv", 6, Bytes.begin() + Attr + 5);
    Bytes[Attr + 11] = 1;
    put(Attr + 12, Size - 11, 4);
    size_t Next = Attr + 16;
    if (StackAlign) {
      Bytes[Next++] = 4;
      Bytes[Next++] = StackAlign;
    }
    Bytes[Next++] = 5;
    std::copy(ISA.begin(), ISA.end(), Bytes.begin() + Next);
    addSection(".riscv.attributes", llvm::ELF::SHT_RISCV_ATTRIBUTES, Attr,
               Size);
  }
  void addSection(llvm::StringRef Name, unsigned Kind, size_t Offset,
                  size_t Size, uint64_t Flags = 0, uint64_t VA = 0,
                  unsigned EntrySize = 0) {
    std::string Names("\0.shstrtab\0", 11);
    Names += Name.str();
    Names.push_back('\0');
    const size_t Strings = Bytes.size();
    Bytes.insert(Bytes.end(), Names.begin(), Names.end());
    const size_t Table = (Bytes.size() + 7) & ~size_t(7);
    const unsigned Ent = Wide ? 64 : 40, Ptr = Wide ? 8 : 4;
    Bytes.resize(Table + 3 * Ent);
    put(Wide ? 40 : 32, Table, Ptr);
    put(Wide ? 60 : 48, 3, 2);
    put(Wide ? 62 : 50, 1, 2);
    auto Section = [&](unsigned Index, unsigned Name, unsigned Type,
                       size_t Offset, size_t Length) {
      const size_t S = Table + Ent * Index;
      put(S, Name, 4);
      put(S + 4, Type, 4);
      put(S + (Wide ? 24 : 16), Offset, Ptr);
      put(S + (Wide ? 32 : 20), Length, Ptr);
      put(S + (Wide ? 48 : 32), 1, Ptr);
    };
    Section(1, 1, llvm::ELF::SHT_STRTAB, Strings, Names.size());
    Section(2, 11, Kind, Offset, Size);
    const size_t S = Table + 2 * Ent;
    put(S + 8, Flags, Ptr);
    put(S + (Wide ? 16 : 12), VA, Ptr);
    put(S + (Wide ? 56 : 36), EntrySize, Ptr);
  }
  llvm::MemoryBufferRef buffer() const {
    return {llvm::StringRef(reinterpret_cast<const char *>(Bytes.data()),
                            Bytes.size()),
            "integer-add.elf"};
  }
};

class ELFIntegerTarget : public ::testing::TestWithParam<Arch> {};

TEST_P(ELFIntegerTarget, StrippedELFToCAndVerifiedLLVM) {
  const Arch A = GetParam();
  LinkedImage File(A);
  std::vector<LoadCandidate> Rows;
  ELFLoader::identify(File.buffer(), Rows);
  ASSERT_EQ(Rows.size(), 1u);
  ASSERT_TRUE(Rows[0].Loadable) << Rows[0].Reason;
  ASSERT_EQ(Rows[0].TheArch, A);
  auto Img = ELFLoader().loadBuffer(File.buffer());
  ASSERT_TRUE(bool(Img)) << llvm::toString(Img.takeError());
  ASSERT_EQ(Img->Arch, A);
  ASSERT_EQ(Img->Entry, File.Entry);
  EXPECT_EQ(Img->vaToFileOffset(Img->Entry), 0x100u);
  EXPECT_FALSE(Img->isFunctionEntryCandidateAt(Img->Entry + 1));
  EXPECT_TRUE(archLiftSupported(A));
  EXPECT_FALSE(archCodegenSupported(A));
  for (bool LLVM : {false, true}) {
    PipelineOptions Options;
    Options.OnlyFunctionEntries = {Img->Entry};
    Options.LiftMode = LLVM;
    Options.EmitDumpOutput = false;
    llvm::LLVMContext Ctx;
    auto Result = Pipeline().run(*Img, Ctx, Options);
    ASSERT_TRUE(Result.Success) << Result.Error;
    ASSERT_EQ(Result.FunctionAudits.size(), 1u);
    EXPECT_TRUE(Result.FunctionAudits.front().UnsupportedInstructions.empty());
    EXPECT_TRUE(Result.FunctionAudits.front().DecodeFailures.empty());
    EXPECT_TRUE(Result.FunctionAudits.front().TruncatedPaths.empty());
    ASSERT_EQ(Result.MedFuncs.size(), 1u);
    const auto &Med = Result.MedFuncs.front();
    EXPECT_EQ(Result.MedIRVerifierFailures, 0u);
    ASSERT_EQ(Med.Params.size(), 2u);
    unsigned First = isRISCV(A) ? 10 : 3;
    EXPECT_EQ(Med.Params[0].RegOff, integerreg::gpr(First));
    EXPECT_EQ(Med.Params[1].RegOff, integerreg::gpr(First + 1));
    if (LLVM) {
      ASSERT_NE(Result.LlvmModule, nullptr);
      EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
      const auto &DL = Result.LlvmModule->getDataLayout();
      EXPECT_EQ(DL.isLittleEndian(), File.Little);
      EXPECT_EQ(DL.getPointerSize(), File.Wide ? 8u : 4u);
      auto *Body = Result.LlvmModule->getFunction(Med.Name);
      ASSERT_NE(Body, nullptr);
      if (isRISCV(A)) {
        const auto *ABI = llvm::dyn_cast_or_null<llvm::MDString>(
            Result.LlvmModule->getModuleFlag("target-abi"));
        ASSERT_NE(ABI, nullptr);
        EXPECT_EQ(ABI->getString(), File.Wide ? "lp64" : "ilp32");
      }
      EXPECT_EQ(Body->arg_size(), 2u);
      EXPECT_TRUE(Body->getReturnType()->isIntegerTy(File.Wide ? 64 : 32));
    } else {
      ASSERT_EQ(Result.HighFuncs.size(), 1u);
      ASSERT_TRUE(Result.HighFuncs.front().ReturnType);
      EXPECT_EQ(Result.HighFuncs.front().ReturnType->Size, File.Wide ? 8u : 4u);
      CEmitterOptions Emit;
      Emit.TheArch = A;
      Emit.Format = BinaryFormat::ELF;
      Emit.Image = &*Img;
      std::string Text;
      llvm::raw_string_ostream Out(Text);
      ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Out, Emit));
      EXPECT_NE(Text.find("return "), std::string::npos) << Text;
      EXPECT_NE(Text.find(" + "), std::string::npos) << Text;
    }
  }
}

TEST_P(ELFIntegerTarget, DirectHighIRUsesTheIntegerReturnRegister) {
  LinkedImage File(GetParam());
  auto Img = ELFLoader().loadBuffer(File.buffer());
  ASSERT_TRUE(bool(Img)) << llvm::toString(Img.takeError());
  Decoder D;
  ASSERT_TRUE(D.init(*Img));
  auto Low = CFGBuilder().build(*Img, D, Img->Entry, "sum");
  ASSERT_TRUE(Low.hasCompleteLiftCoverage());
  LowToMedConverter Lower;
  Lower.setBinaryImage(&*Img);
  auto Med = Lower.convert(Low, Img->Arch, BinaryFormat::ELF);
  auto High = MedToHighConverter().convert(Med, Img->Arch);
  ASSERT_TRUE(High.ReturnType);
  EXPECT_EQ(High.ReturnType->Size, File.Wide ? 8u : 4u);
}

TEST_P(ELFIntegerTarget, StackAllocationUsesPointerWidthDisplacements) {
  const Arch A = GetParam();
  LinkedImage File(A);
  // addi sp,sp,-16 / stwu or stdu r1,-16(r1), followed by stack release.
  File.Bytes.resize(0x10c);
  File.put(0x100,
           isRISCV(A)       ? 0xff010113
           : isPowerPC64(A) ? 0xf821fff1
                            : 0x9421fff0,
           4);
  File.put(0x104, isRISCV(A) ? 0x01010113 : 0x38210010, 4);
  File.put(0x108, isRISCV(A) ? 0x00008067 : 0x4e800020, 4);
  File.put(File.Wide ? 96 : 68, 12, File.Wide ? 8 : 4);
  File.put(File.Wide ? 104 : 72, 12, File.Wide ? 8 : 4);
  auto Img = ELFLoader().loadBuffer(File.buffer());
  ASSERT_TRUE(bool(Img)) << llvm::toString(Img.takeError());
  Decoder D;
  ASSERT_TRUE(D.init(*Img));
  auto Low = CFGBuilder().build(*Img, D, Img->Entry, "frame");
  ASSERT_TRUE(Low.hasCompleteLiftCoverage());
  LowToMedConverter Convert;
  Convert.setBinaryImage(&*Img);
  auto Med = Convert.convert(Low, A, BinaryFormat::ELF);
  EXPECT_EQ(Med.FrameSize, 16);
}

TEST_P(ELFIntegerTarget, RejectsUnknownABIInBothPublicEntrypoints) {
  const Arch A = GetParam();
  auto Reject = [](const LinkedImage &File) {
    std::vector<LoadCandidate> Rows;
    ELFLoader::identify(File.buffer(), Rows);
    ASSERT_EQ(Rows.size(), 1u);
    EXPECT_FALSE(Rows[0].Loadable);
    auto Img = ELFLoader().loadBuffer(File.buffer());
    ASSERT_FALSE(bool(Img));
    EXPECT_EQ(llvm::toString(Img.takeError()), Rows[0].Reason);
  };
  LinkedImage File(A);
  for (unsigned Flags : isRISCV(A) ? std::vector<unsigned>{2, 4, 8, 0x80000000u}
                        : isPowerPC64(A) ? std::vector<unsigned>{0, 1, 3}
                                         : std::vector<unsigned>{0x80000000u}) {
    File.put(File.Wide ? 48 : 36, Flags, 4);
    Reject(File);
  }
  if (isPowerPC(A)) {
    LinkedImage VLE(A);
    VLE.put(VLE.Wide ? 68 : 76, 0x10000005, 4); // executable VLE PT_LOAD
    Reject(VLE);
  }
}

TEST_P(ELFIntegerTarget, PhysicalWidthsStrictFailureAndDirectCalls) {
  const Arch A = GetParam();
  LinkedImage File(A);
  Decoder D;
  ASSERT_TRUE(D.init(A));
  DecodedInsn I;
  EXPECT_EQ(D.decodeOne(File.Bytes.data() + 0x100, 3, File.Entry, I), 0);
  EXPECT_EQ(D.decodeOne(File.Bytes.data() + 0x100, 4, File.Entry + 2, I), 0);
  File.put(0x100, isRISCV(A) ? 0x008000ef : 0x48000009, 4);
  for (bool Text : {true, false}) {
    D.setText(Text);
    D.setDetail(Text);
    ASSERT_EQ(D.decodeOneLight(File.Bytes.data() + 0x100, 4, File.Entry, I), 4);
    EXPECT_EQ(D.directCallTarget(I), File.Entry + 8);
    EXPECT_FALSE(D.isFunctionTerminator(I));
  }
  D.setText(true);
  File.put(0x100, isRISCV(A) ? 0x00000073 : 0x44000002, 4); // ecall / sc
  ASSERT_EQ(D.decodeOneForLift(File.Bytes.data() + 0x100, 4, File.Entry, I), 4);
  std::vector<LowOp> Ops(1);
  EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
  EXPECT_EQ(Ops.size(), 1u);
  if (isPowerPC64(A)) {
    File.put(0x100, 0x7c6102a6,
             4); // mfxer r3: ISA level determines CA32 and OV32.
    ASSERT_EQ(D.decodeOneForLift(File.Bytes.data() + 0x100, 4, File.Entry, I),
              4);
    EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
    EXPECT_EQ(Ops.size(), 1u);
  }
}

TEST_P(ELFIntegerTarget, LinkedCallHasARelocatableContinuation) {
  const Arch A = GetParam();
  LinkedImage File(A);
  const unsigned Header = File.Wide ? 64 : 52, Ptr = File.Wide ? 8 : 4;
  const uint32_t RV[] = {0x00008293, 0x00c000ef, 0x00028093,
                         0x00008067, 0x00b50533, 0x00008067};
  const uint32_t PPC[] = {0x7d6802a6, 0x4800000d, 0x7d6803a6,
                          0x4e800020, 0x7c632214, 0x4e800020};
  // Save the entry link, call the sum at +16, restore the link, return.
  File.Bytes.resize(0x100 + sizeof(RV));
  File.put(Header + (File.Wide ? 32 : 16), sizeof(RV), Ptr);
  File.put(Header + (File.Wide ? 40 : 20), sizeof(RV), Ptr);
  for (unsigned I = 0; I < std::size(RV); ++I)
    File.put(0x100 + 4 * I, isRISCV(A) ? RV[I] : PPC[I], 4);
  auto Img = ELFLoader().loadBuffer(File.buffer());
  ASSERT_TRUE(bool(Img)) << llvm::toString(Img.takeError());
  Decoder D;
  ASSERT_TRUE(D.init(*Img));
  auto Low = CFGBuilder().build(*Img, D, File.Entry, "caller");
  ASSERT_TRUE(Low.hasCompleteLiftCoverage());
  ASSERT_FALSE(Low.Blocks.empty());
  const auto &First = Low.Blocks.front();
  ASSERT_EQ(First.Succs.size(), 1u);
  EXPECT_EQ(Low.Blocks.at(First.Succs[0]).StartAddr, File.Entry + 8);
}

TEST_P(ELFIntegerTarget, DistinguishesPointerRelocationsFromCode) {
  using namespace llvm::ELF;
  const Arch A = GetParam();
  const uint32_t Absolute = isRISCV(A)
                                ? (A == Arch::RISCV32 ? R_RISCV_32 : R_RISCV_64)
                            : isPowerPC64(A) ? uint32_t(R_PPC64_ADDR64)
                                             : uint32_t(R_PPC_ADDR32);
  EXPECT_EQ(elf_loader::symbolRelocationValue(A, Absolute, 0x8000, -8),
            0x7ff8u);
  if (isRISCV(A)) {
    EXPECT_TRUE(elf_loader::isELFSlotBinding(A, R_RISCV_JUMP_SLOT));
    EXPECT_EQ(
        elf_loader::symbolRelocationValue(A, R_RISCV_JUMP_SLOT, 0x8000, 8),
        0x8000u);
    EXPECT_FALSE(
        elf_loader::symbolRelocationValue(A, R_RISCV_BRANCH, 0x8000, -8));
  } else {
    EXPECT_TRUE(elf_loader::isELFSlotBinding(A, R_PPC_GLOB_DAT));
    EXPECT_EQ(elf_loader::isELFSlotBinding(A, R_PPC_JMP_SLOT), isPowerPC64(A));
    EXPECT_FALSE(elf_loader::symbolRelocationValue(A, R_PPC_REL24, 0x8000, -8));
  }
}

TEST_P(ELFIntegerTarget, RelativeRelocationPatchesTargetEndianWord) {
  using namespace llvm::ELF;
  const Arch A = GetParam();
  LinkedImage File(A);
  const unsigned Ptr = File.Wide ? 8 : 4, EntrySize = 3 * Ptr;
  // The second PT_LOAD holds a pointer followed by its allocated RELA record.
  const size_t Data = 0x200, Rela = Data + Ptr;
  const va_t Slot = File.Entry + 0x100, Target = File.Entry + 4;
  File.Bytes.resize(Rela + EntrySize);
  const unsigned PH = File.Wide ? 120 : 84;
  File.put(File.Wide ? 56 : 44, 2, 2);
  File.put(PH, PT_LOAD, 4);
  File.put(PH + (File.Wide ? 4 : 24), PF_R | PF_W, 4);
  File.put(PH + (File.Wide ? 8 : 4), Data, Ptr);
  File.put(PH + (File.Wide ? 16 : 8), Slot, Ptr);
  File.put(PH + (File.Wide ? 24 : 12), Slot, Ptr);
  File.put(PH + (File.Wide ? 32 : 16), 4 * Ptr, Ptr);
  File.put(PH + (File.Wide ? 40 : 20), 4 * Ptr, Ptr);
  File.put(PH + (File.Wide ? 48 : 28), 0x100, Ptr);
  File.put(Rela, Slot, Ptr);
  File.put(Rela + Ptr,
           isRISCV(A)  ? uint32_t(R_RISCV_RELATIVE)
           : File.Wide ? uint32_t(R_PPC64_RELATIVE)
                       : uint32_t(R_PPC_RELATIVE),
           Ptr);
  File.put(Rela + Ptr * 2, Target, Ptr);
  File.addSection(".rela.dyn", SHT_RELA, Rela, EntrySize, SHF_ALLOC, Slot + Ptr,
                  EntrySize);
  auto Img = ELFLoader().loadBuffer(File.buffer());
  ASSERT_TRUE(bool(Img)) << llvm::toString(Img.takeError());
  const uint8_t *P = Img->readVA(Slot, Ptr);
  ASSERT_NE(P, nullptr);
  for (unsigned I = 0; I < Ptr; ++I)
    EXPECT_EQ(P[File.Little ? I : Ptr - 1 - I], uint8_t(Target >> (8 * I)));
  EXPECT_TRUE(
      llvm::any_of(Img->BaseRelocations, [Slot](const BaseRelocation &R) {
        return R.Address == Slot;
      }));
}

TEST_P(ELFIntegerTarget, IntegerRegisterAndStackParameters) {
  const Arch A = GetParam();
  const auto &Info = getTargetRegInfo(A);
  if (isPowerPC(A)) {
    for (unsigned Bit = 0; Bit < 32; ++Bit)
      EXPECT_EQ(Info.isCallPreserved(integerreg::ppc::crBit(Bit), 1),
                Bit >= 8 && Bit < 20);
  } else {
    EXPECT_TRUE(Info.isCallPreserved(integerreg::gpr(3), Info.PointerSize));
    EXPECT_TRUE(Info.isCallPreserved(integerreg::gpr(4), Info.PointerSize));
  }
  const unsigned Word = Info.PointerSize;
  auto Integer = NdType::makeInt(Word);
  std::vector<TypeRef> Params(9, Integer);
  Params.push_back(NdType::makeInt(1));
  auto P = placeSourceParameters(A, BinaryFormat::ELF, Integer, Params);
  ASSERT_TRUE(P);
  ASSERT_EQ(P->Parameters.size(), 10u);
  for (unsigned I = 0; I < 8; ++I) {
    ASSERT_EQ(P->Parameters[I].size(), 1u);
    EXPECT_EQ(P->Parameters[I][0].Location.RegisterOffset,
              integerreg::gpr((isRISCV(A) ? 10 : 3) + I));
  }
  const unsigned Base = isRISCV(A) ? 0 : isPowerPC64(A) ? 96 : 8;
  EXPECT_EQ(P->Parameters[8][0].Location.EntryStackOffset, Base);
  EXPECT_EQ(P->Parameters[9][0].Location.EntryStackOffset,
            Base + Word + (archLittleEndian(A) ? 0 : Word - 1));
}

TEST_P(ELFIntegerTarget, OrderingSurvivesBothSourceRoutes) {
  const Arch A = GetParam();
  LinkedImage File(A);
  // amoadd.w.aqrl a0,a1,(a0) or lwarx r3,0,r3. Both return the old word.
  File.put(0x100, isRISCV(A) ? 0x06b5252f : 0x7c601828, 4);
  auto Img = ELFLoader().loadBuffer(File.buffer());
  ASSERT_TRUE(bool(Img)) << llvm::toString(Img.takeError());
  if (isRISCV(A))
    Img->RISCV.Atomic = true;
  for (bool LLVM : {false, true}) {
    PipelineOptions Options;
    Options.OnlyFunctionEntries = {Img->Entry};
    Options.LiftMode = LLVM;
    Options.EmitDumpOutput = false;
    llvm::LLVMContext Ctx;
    auto R = Pipeline().run(*Img, Ctx, Options);
    ASSERT_TRUE(R.Success) << R.Error;
    ASSERT_EQ(R.FunctionAudits.size(), 1u);
    ASSERT_TRUE(R.FunctionAudits.front().UnsupportedInstructions.empty());
    ASSERT_EQ(R.MedFuncs.size(), 1u);
    EXPECT_FALSE(R.MedFuncs.front().ReturnsNoValue);
    CEmitterOptions Emit;
    Emit.Image = &*Img;
    Emit.TheArch = A;
    Emit.Format = BinaryFormat::ELF;
    std::string Text;
    llvm::raw_string_ostream Out(Text);
    if (LLVM) {
      ASSERT_TRUE(R.LlvmModule);
      ASSERT_FALSE(llvm::verifyModule(*R.LlvmModule, &llvm::errs()));
      ASSERT_TRUE(LLVMCEmitter().emit(*R.LlvmModule, Out, Emit));
    } else {
      ASSERT_TRUE(HighCEmitter().emit(R.HighFuncs, Out, Emit));
    }
    EXPECT_NE(Text.find(isRISCV(A) ? "amoadd.w.aqrl" : "lwarx"),
              std::string::npos)
        << Text;
    EXPECT_NE(Text.find("\"memory\""), std::string::npos) << Text;
    EXPECT_NE(Text.find("return "), std::string::npos) << Text;
  }
}

TEST(RISCVProfile, ELFAttributesAdmitEachExtensionIndependently) {
  for (Arch A : {Arch::RISCV32, Arch::RISCV64}) {
    const std::string Base = A == Arch::RISCV64 ? "rv64i2p1" : "rv32i2p1";
    for (unsigned Extensions = 0; Extensions < 8; ++Extensions) {
      LinkedImage File(A);
      std::string ISA = Base;
      if (Extensions & 1)
        ISA += "_m2p0";
      if (Extensions & 2)
        ISA += "_a2p1";
      if (Extensions & 4)
        ISA += "_c2p0";
      File.riscvAttributes(ISA, 16);
      auto Img = ELFLoader().loadBuffer(File.buffer());
      ASSERT_TRUE(bool(Img)) << ISA << ": " << llvm::toString(Img.takeError());
      EXPECT_EQ(Img->RISCV.Multiply, bool(Extensions & 1));
      EXPECT_EQ(Img->RISCV.Atomic, bool(Extensions & 2));
      EXPECT_EQ(Img->RISCV.Compressed, bool(Extensions & 4));
    }
    LinkedImage ReducedStack(A);
    ReducedStack.riscvAttributes(Base, 8);
    auto Stack = ELFLoader().loadBuffer(ReducedStack.buffer());
    ASSERT_FALSE(bool(Stack));
    EXPECT_NE(llvm::toString(Stack.takeError()).find("stack alignment"),
              std::string::npos);
    LinkedImage Conflict(A);
    Conflict.riscvAttributes(A == Arch::RISCV64 ? "rv32i2p1" : "rv64i2p1");
    std::vector<LoadCandidate> Rows;
    ELFLoader::identify(Conflict.buffer(), Rows);
    ASSERT_EQ(Rows.size(), 1u);
    EXPECT_FALSE(Rows[0].Loadable);
    auto Img = ELFLoader().loadBuffer(Conflict.buffer());
    ASSERT_FALSE(bool(Img));
    EXPECT_EQ(llvm::toString(Img.takeError()), Rows[0].Reason);
  }
}

TEST(RISCVProfile, ReusedDecoderTracksExtensionEvidenceAndHalfwordBoundaries) {
  const uint8_t Addi[] = {0x05, 0x05}; // c.addi a0,1
  const uint8_t Mul[] = {0x33, 0x05, 0xb5, 0x02};
  BinaryImage Img;
  Img.Arch = Arch::RISCV64;
  Img.Bits = Bitness::Bits64;
  Decoder D;
  DecodedInsn I;
  for (bool Extended : {true, false, true}) {
    Img.RISCV = {.Multiply = Extended, .Compressed = Extended};
    ASSERT_TRUE(D.selectMode(Img, 0x1000));
    EXPECT_EQ(D.decodeOne(Addi, 2, 0x1002, I), Extended ? 2 : 0);
    ASSERT_EQ(D.decodeOneForLift(Mul, 4, 0x1000, I), 4);
    std::vector<LowOp> Ops;
    if (Extended)
      EXPECT_NO_THROW(D.liftToLow(I, Ops));
    else
      EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
  }
}
INSTANTIATE_TEST_SUITE_P(Profiles, ELFIntegerTarget,
                         ::testing::Values(Arch::RISCV32, Arch::RISCV64,
                                           Arch::PPC32, Arch::PPC32LE,
                                           Arch::PPC64, Arch::PPC64LE),
                         [](const ::testing::TestParamInfo<Arch> &P) {
                           return getArchName(P.param);
                         });
} // namespace
