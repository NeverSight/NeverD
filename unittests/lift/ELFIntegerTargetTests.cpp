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

#include "../../lib/pipeline/PipelineWideIntReturnDetail.h"
#include "gtest/gtest.h"

#include "neverd/ArchSupport.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/SourceParameterPlacement.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/FuncDetector.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/lift/IntegerRegisters.h"
#include "neverd/loader/ELF/ELFLoader.h"
#include "neverd/loader/ELF/ELFLoaderUtils.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <cstddef>
#include <tuple>
#include <vector>

using namespace neverd;
namespace {
#include "fixtures/RISCVConditionalZero.inc"

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
  void setWords(llvm::ArrayRef<uint32_t> Words) {
    const unsigned Header = Wide ? 64 : 52, Pointer = Wide ? 8 : 4;
    Bytes.resize(0x100 + Words.size() * 4);
    put(Header + (Wide ? 32 : 16), Words.size() * 4, Pointer);
    put(Header + (Wide ? 40 : 20), Words.size() * 4, Pointer);
    for (size_t I = 0; I != Words.size(); ++I)
      put(0x100 + I * 4, Words[I], 4);
  }
};

class ELFIntegerTarget : public ::testing::TestWithParam<Arch> {};

TEST(RegisterPairShift, OnlyTheZeroCountEdgeCanForwardTheInputPair) {
  for (Arch A : {Arch::RISCV32, Arch::PPC32, Arch::PPC32LE}) {
    const auto &TRI = getTargetRegInfo(A);
    auto Argument = [&](int I) {
      MedVar V;
      V.Kind = MedVar::Reg;
      V.TheArch = A;
      V.Size = 4;
      V.Id = I;
      V.RegOff = TRI.IntParamRegs[I];
      return V;
    };
    MedVar First = Argument(0), Second = Argument(1), Count = Argument(2);
    MedVar Condition;
    Condition.Kind = MedVar::Temp;
    Condition.Size = 1;
    MedOp Cmp;
    Cmp.Opcode = NdOp::INT_EQUAL;
    Cmp.Output = Condition;
    Cmp.addInput(Count);
    Cmp.addInput(MedVar::makeConst(0, 4));
    MedOp Branch;
    Branch.Opcode = NdOp::COND_BR;
    Branch.addInput(MedVar::makeConst(0x1010, 4));
    Branch.addInput(Condition);
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    MedFunc Func;
    Func.Entry = 0x1000;
    Func.Blocks.resize(3);
    auto &Entry = Func.Blocks[0], &Shift = Func.Blocks[1],
         &Join = Func.Blocks[2];
    Entry.Id = 0;
    Entry.StartAddr = Func.Entry;
    Entry.Succs = {2, 1};
    Entry.Ops = {Cmp, Branch};
    Shift.Id = 1;
    Shift.StartAddr = 0x1008;
    Shift.Preds = {0};
    Shift.Succs = {2};
    Join.Id = 2;
    Join.StartAddr = 0x1010;
    Join.Preds = {0, 1};
    Join.Ops = {Return};
    for (MedVar V : {First, Second}) {
      MedOp Op;
      Op.Opcode = NdOp::INT_LEFT;
      Op.addInput(V);
      Op.addInput(Count);
      Op.Output = V;
      Op.Output.SSAVer = 1;
      Shift.Ops.push_back(Op);
      PhiNode Phi;
      Phi.Output = V;
      Phi.Output.SSAVer = 2;
      Phi.Args = {{1, Op.Output}, {0, V}};
      Join.Phis.push_back(Phi);
    }
    auto Accepts = [&] {
      return returnsRegisterPairI64(
          Func, TRI, [](const MedOp &) { return false; },
          /*ShiftReturnsInputOnZero=*/true);
    };
    EXPECT_FALSE(returnsRegisterPairI64(Func, TRI));
    EXPECT_TRUE(Accepts());
    Entry.Ops[0].Opcode = NdOp::INT_NOTEQUAL;
    EXPECT_FALSE(Accepts());
    Entry.Ops[0] = Cmp;
    Entry.Ops[0].Inputs[0] = First;
    EXPECT_FALSE(Accepts());
    Entry.Ops[0] = Cmp;
    auto &FirstEdge = Join.Phis.front().Args[1].second;
    FirstEdge = MedVar::makeConst(0, 4);
    EXPECT_FALSE(Accepts());
    FirstEdge = First;
    Entry.Ops.insert(Entry.Ops.begin(), Shift.Ops.front());
    Entry.Ops.front().Output.SSAVer = 3;
    FirstEdge = Entry.Ops.front().Output;
    EXPECT_FALSE(Accepts());
    FirstEdge = First;
    Entry.Ops.erase(Entry.Ops.begin());
    // The same PHI does not justify an untouched pair if the common exit
    // overwrites its first word.
    Join.Ops.insert(Join.Ops.begin(), Shift.Ops.front());
    Join.Ops.front().Inputs[0] = Join.Phis.front().Output;
    Join.Ops.front().Output.SSAVer = 4;
    EXPECT_FALSE(Accepts());

    // A conditional return in the block takes the fallthrough edge. The
    // branch to the shifting block must therefore test for a nonzero count.
    Func.Blocks.pop_back();
    Entry.Ops = {Cmp, Branch, Return};
    Entry.Succs = {Shift.Id};
    Entry.Ops[0].Opcode = NdOp::INT_NOTEQUAL;
    Entry.Ops[1].Inputs[0] = MedVar::makeConst(Shift.StartAddr, 4);
    Shift.Succs.clear();
    Shift.Ops.push_back(Return);
    EXPECT_TRUE(Accepts());
    Entry.Ops[0].Opcode = NdOp::INT_EQUAL;
    EXPECT_FALSE(Accepts());
  }
}

void checkLeafReturnWidth(Arch A, llvm::ArrayRef<uint32_t> Words,
                          unsigned ResultBytes, bool Compressed = false) {
  LinkedImage File(A);
  File.setWords(Words);
  if (Compressed)
    File.put(offsetof(llvm::ELF::Elf32_Ehdr, e_flags), llvm::ELF::EF_RISCV_RVC,
             sizeof(llvm::ELF::Elf32_Word));
  auto Img = ELFLoader().loadBuffer(File.buffer());
  ASSERT_TRUE(bool(Img)) << llvm::toString(Img.takeError());
  for (bool LLVM : {false, true}) {
    PipelineOptions Options;
    Options.LiftMode = LLVM;
    Options.EmitDumpOutput = false;
    llvm::LLVMContext Ctx;
    auto R = Pipeline().run(*Img, Ctx, Options);
    ASSERT_TRUE(R.Success) << R.Error;
    ASSERT_EQ(R.MedFuncs.size(), 1u);
    if (LLVM) {
      ASSERT_TRUE(R.LlvmModule);
      auto *F = R.LlvmModule->getFunction(R.MedFuncs.front().Name);
      ASSERT_TRUE(F);
      EXPECT_TRUE(F->getReturnType()->isIntegerTy(ResultBytes * 8));
      EXPECT_FALSE(llvm::verifyModule(*R.LlvmModule, &llvm::errs()));
    } else {
      ASSERT_EQ(R.HighFuncs.size(), 1u);
      ASSERT_TRUE(R.HighFuncs.front().ReturnType);
      EXPECT_EQ(R.HighFuncs.front().ReturnType->Size, ResultBytes);
    }
  }
}

#include "DoubleWordSumFixture.inc"

TEST(ELF32DoubleWordSum, RecoversBothWordsOnlyForTheABISum) {
  using namespace doubleword;
  struct Case {
    Arch A;
    llvm::ArrayRef<uint32_t> Words;
    unsigned ResultBytes;
  };
  const Case Cases[] = {
      {Arch::RISCV32, RVSum, 8},           {Arch::RISCV32, RVWrongCarry, 4},
      {Arch::RISCV32, RVSpilled, 8},       {Arch::RISCV32, RVPartialSpill, 4},
      {Arch::RISCV32, RVStored, 4},        {Arch::PPC32, BESum, 8},
      {Arch::PPC32, BEIncomingCarry, 4},   {Arch::PPC32, LESum, 4},
      {Arch::PPC32, BEStored, 4},          {Arch::PPC32LE, LESum, 8},
      {Arch::PPC32LE, LEIncomingCarry, 4}, {Arch::PPC32LE, BESum, 4},
      {Arch::PPC32LE, LEStored, 4}};
  for (const Case &C : Cases) {
    SCOPED_TRACE(::testing::Message() << getArchName(C.A) << " " << C.Words[0]
                                      << " " << C.Words.size());
    checkLeafReturnWidth(C.A, C.Words, C.ResultBytes);
  }
}

#include "DoubleWordShiftFixture.inc"

TEST(RegisterPairShift, OnlyTheCompleteShiftQualifiesWithoutACaller) {
  using namespace wordshift;
  checkLeafReturnWidth(Arch::RISCV32, RVLeft, 8, /*Compressed=*/true);
  checkLeafReturnWidth(Arch::RISCV32, RVBaseLeft, 8);
  checkLeafReturnWidth(Arch::RISCV32, RVWrongAt17, 4);
  checkLeafReturnWidth(Arch::PPC32, BELeft, 8);
  checkLeafReturnWidth(Arch::PPC32LE, BELeft, 4);

  // Each near-miss still defines both carriers and keeps the zero-count exit.
  std::vector<uint32_t> RV(std::begin(RVLeft), std::end(RVLeft));
  RV[7] = ShiftHighAgain;
  checkLeafReturnWidth(Arch::RISCV32, RV, 4, /*Compressed=*/true);
  std::vector<uint32_t> PPC(std::begin(BELeft), std::end(BELeft));
  PPC[6] = CrossFromHigh;
  checkLeafReturnWidth(Arch::PPC32, PPC, 4);
  PPC[6] = BELeft[6];
  PPC[13] = FillOne;
  checkLeafReturnWidth(Arch::PPC32, PPC, 4);
}

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
  for (unsigned Flags : isRISCV(A) ? std::vector<unsigned>{2, 6, 8, 0x80000000u}
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

TEST(RISCVProfile, DoubleABIFollowsFileFlagsAndRejectsIncompatibleISA) {
  for (const Arch Base : {Arch::RISCV32, Arch::RISCV64}) {
    LinkedImage File(Base);
    const Arch Hard = isRISCV32(Base) ? Arch::RISCV32D : Arch::RISCV64D;
    File.put(File.Wide ? 48 : 36, llvm::ELF::EF_RISCV_FLOAT_ABI_DOUBLE, 4);
    auto Image = ELFLoader().loadBuffer(File.buffer());
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    EXPECT_EQ(Image->Arch, Hard);
    EXPECT_TRUE(Image->RISCV.SingleFloat && Image->RISCV.DoubleFloat);
    std::vector<LoadCandidate> Rows;
    ELFLoader::identify(File.buffer(), Rows);
    ASSERT_EQ(Rows.size(), 1u);
    EXPECT_TRUE(Rows[0].Loadable);
    EXPECT_EQ(Rows[0].TheArch, Hard);
    const auto &Info = getTargetRegInfo(Hard);
    EXPECT_EQ(Info.PointerSize, isRISCV32(Base) ? 4 : 8);
    EXPECT_EQ(Info.FPParamRegs.size(), 8u);
    EXPECT_EQ(Info.FPReturnReg, integerreg::riscv::fpr(10));
    EXPECT_TRUE(Info.isCallPreserved(integerreg::riscv::fpr(8), 8));
    EXPECT_FALSE(Info.isCallPreserved(integerreg::riscv::fpr(10), 8));
    EXPECT_TRUE(getTargetRegInfo(Base).FPParamRegs.empty());

    File.riscvAttributes(isRISCV32(Base) ? "rv32i2p1" : "rv64i2p1");
    auto Conflict = ELFLoader().loadBuffer(File.buffer());
    ASSERT_FALSE(bool(Conflict));
    const auto Message = llvm::toString(Conflict.takeError());
    EXPECT_NE(Message.find("double-float"), std::string::npos);
    Rows.clear();
    ELFLoader::identify(File.buffer(), Rows);
    ASSERT_EQ(Rows.size(), 1u);
    EXPECT_FALSE(Rows[0].Loadable);
    EXPECT_EQ(Rows[0].Reason, Message);
  }
}

TEST(RISCVProfile, DoubleABIPlacesNamedAndVariadicArgumentsInIndependentBanks) {
  using K = SourceABICarrierKind;
  using integerreg::gpr;
  using integerreg::riscv::fpr;
  const auto Double = NdType::makeFloat(8);
  for (Arch A : {Arch::RISCV32D, Arch::RISCV64D}) {
    const auto &TRI = getTargetRegInfo(A);
    const auto Word = NdType::makeInt(TRI.PointerSize);
    std::vector<TypeRef> Args{Word};
    Args.insert(Args.end(), 9, Double);
    auto Params = placeSourceParameters(A, BinaryFormat::ELF, Double, Args);
    ASSERT_TRUE(Params);
    ASSERT_EQ(Params->Parameters.size(), Args.size());
    EXPECT_EQ(Params->Parameters[0][0].Location.RegisterOffset, gpr(10));
    for (unsigned I = 0; I < 8; ++I) {
      const auto &Lane = Params->Parameters[I + 1].front().Location;
      EXPECT_EQ(Lane.Kind, K::FloatingRegister);
      EXPECT_EQ(Lane.RegisterOffset, fpr(10 + I));
      EXPECT_EQ(Lane.ValueBytes, 8u);
    }
    EXPECT_EQ(Params->Parameters.back().front().Location.RegisterOffset,
              gpr(11));
    auto Half = placeSourceParameters(A, BinaryFormat::ELF, Double,
                                      {Word, NdType::makeFloat(2)});
    ASSERT_TRUE(Half);
    ASSERT_EQ(Half->Parameters.size(), 2u);
    EXPECT_EQ(Half->Parameters[1][0].Location.RegisterOffset, fpr(10));
    EXPECT_EQ(Half->Parameters[1][0].Location.ValueBytes, 2u);
    auto Mixed = NdType::makeStruct({Double, Word}, TRI.PointerSize);
    ASSERT_TRUE(Mixed);
    Mixed->Passing = NdRecordPassing::ByValue;
    auto MixedArgs =
        placeSourceParameters(A, BinaryFormat::ELF, Double, {Mixed});
    ASSERT_TRUE(MixedArgs);
    ASSERT_EQ(MixedArgs->Parameters.size(), 1u);
    ASSERT_EQ(MixedArgs->Parameters[0].size(), 2u);
    EXPECT_EQ(MixedArgs->Parameters[0][0].Location.RegisterOffset, fpr(10));
    EXPECT_EQ(MixedArgs->Parameters[0][1].Location.RegisterOffset, gpr(10));
    EXPECT_EQ(MixedArgs->Parameters[0][1].Offset, 8u);
    auto Union = NdType::makeStruct({NdType::makeFloat(4)}, TRI.PointerSize);
    ASSERT_TRUE(Union);
    Union->IsUnion = true;
    auto Nested = NdType::makeStruct({Union}, TRI.PointerSize);
    ASSERT_TRUE(Nested);
    Nested->Passing = NdRecordPassing::ByValue;
    auto NestedArgs =
        placeSourceParameters(A, BinaryFormat::ELF, Double, {Nested});
    ASSERT_TRUE(NestedArgs);
    ASSERT_EQ(NestedArgs->Parameters.size(), 1u);
    EXPECT_EQ(NestedArgs->Parameters[0][0].Location.RegisterOffset, gpr(10));
    auto Pair = NdType::makeComplex(8);
    auto ReturnPair = placeSourceParameters(A, BinaryFormat::ELF, Pair, {Word});
    ASSERT_TRUE(ReturnPair);
    EXPECT_FALSE(ReturnPair->ResultPointer);
    EXPECT_EQ(ReturnPair->Parameters[0][0].Location.RegisterOffset, gpr(10));

    auto Var = placeSourceParameters(A, BinaryFormat::ELF, Double,
                                     {Word, Double, Double}, true, 2);
    ASSERT_TRUE(Var);
    ASSERT_EQ(Var->Parameters.size(), 3u);
    EXPECT_EQ(Var->Parameters[1][0].Location.RegisterOffset, fpr(10));
    EXPECT_EQ(Var->Parameters[2][0].Location.Kind, K::IntegerRegister);
    EXPECT_EQ(Var->Parameters[2][0].Location.RegisterOffset,
              gpr(isRISCV32(A) ? 12 : 11));
  }
}

TEST(RISCVProfile, FloatingTransfersPreservePayloadsAndValidateNaNBoxes) {
  using integerreg::riscv::fpr;
  for (Arch A : {Arch::RISCV32D, Arch::RISCV64D}) {
    BinaryImage Image;
    Image.Arch = A;
    Image.Bits = isRISCV32(A) ? Bitness::Bits32 : Bitness::Bits64;
    Image.RISCV.SingleFloat = Image.RISCV.DoubleFloat = true;
    Decoder Decoder;
    ASSERT_TRUE(Decoder.selectMode(Image, 0x1000));
    NdOpEmulator Emu(Image);
    Emu.setStrictMode(true);
    const auto Run = [&](uint32_t Word) {
      uint8_t Bytes[4];
      llvm::support::endian::write32le(Bytes, Word);
      DecodedInsn Insn;
      ASSERT_EQ(Decoder.decodeOneForLift(Bytes, 4, 0x1000, Insn), 4);
      std::vector<LowOp> Ops;
      Decoder.liftToLow(Insn, Ops);
      for (const auto &Op : Ops)
        ASSERT_TRUE(Emu.step(Op));
    };
    Emu.setRegister(fpr(11), UINT64_C(0x7ff0000000000042));
    Emu.setRegister(fpr(12), UINT64_C(0x8000000000000000));
    Run(0x22c58553); // fsgnj.d fa0,fa1,fa2
    EXPECT_EQ(Emu.getRegister(fpr(10)), UINT64_C(0xfff0000000000042));
    Emu.setRegister(fpr(11), UINT64_C(0x00000000bf800000));
    Run(0x20b58553); // fsgnj.s fa0,fa1,fa1
    EXPECT_EQ(Emu.getRegister(fpr(10)), UINT64_C(0xffffffff7fc00000));
    Emu.setRegister(fpr(11), UINT64_C(0xffffffffbf800000));
    Run(0x20b58553);
    EXPECT_EQ(Emu.getRegister(fpr(10)), UINT64_C(0xffffffffbf800000));
    Run(0xe0050553); // fmv.x.w a0,fa0
    EXPECT_EQ(Emu.getRegister(integerreg::gpr(10)),
              isRISCV64(A) ? UINT64_C(0xffffffffbf800000)
                           : UINT64_C(0xbf800000));
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

TEST(RISCVProfile, SplitExtensionsControlLiftingAndLLVMFeatures) {
  struct AttributeCase {
    const char *Attribute;
    const char *Features;
    bool Multiply;
    bool Divide;
    bool Amo;
    bool Reservation;
    bool Compressed;
    bool Zba = false;
    bool Zbs = false;
    bool Zbb = false;
    bool Zicond = false;
  };
  const AttributeCase Cases[] = {
      {.Attribute = "", .Features = "+i"},
      {.Attribute = "_zicond1p0", .Features = "+i,+zicond", .Zicond = true},
      {.Attribute = "_c2p0_zicond1p0",
       .Features = "+i,+c,+zicond",
       .Compressed = true,
       .Zicond = true},
      {"_zba1p0", "+i,+zba", false, false, false, false, false, true},
      {"_zbs1p0", "+i,+zbs", false, false, false, false, false, false, true},
      {"_zba1p0_zbs1p0", "+i,+zba,+zbs", false, false, false, false, false,
       true, true},
      {"_zbb1p0", "+i,+zbb", false, false, false, false, false, false, false,
       true},
      {"_zba1p0_zbb1p0_zbs1p0", "+i,+zba,+zbs,+zbb", false, false, false, false,
       false, true, true, true},
      {"_b1p0_zba1p0_zbb1p0_zbs1p0", "+i,+zba,+zbs,+zbb", false, false, false,
       false, false, true, true, true},
      {"_zmmul1p0", "+i,+zmmul", true, false, false, false, false},
      {"_zaamo1p0", "+i,+zaamo", false, false, true, false, false},
      {"_zalrsc1p0", "+i,+zalrsc", false, false, false, true, false},
      {"_zca1p0", "+i,+zca", false, false, false, false, true},
      {"_m2p0_a2p1_c2p0_zaamo1p0_zalrsc1p0_zca1p0_zmmul1p0", "+i,+m,+a,+c",
       true, true, true, true, true}};
  for (Arch A : {Arch::RISCV32, Arch::RISCV64}) {
    const std::string Base = A == Arch::RISCV64 ? "rv64i2p1" : "rv32i2p1";
    Decoder D;
    for (const auto &C : Cases) {
      SCOPED_TRACE(Base + C.Attribute);
      LinkedImage File(A);
      if (C.Compressed)
        File.put(File.Wide ? 48 : 36, llvm::ELF::EF_RISCV_RVC, 4);
      File.riscvAttributes(Base + C.Attribute);
      auto Img = ELFLoader().loadBuffer(File.buffer());
      ASSERT_TRUE(bool(Img)) << llvm::toString(Img.takeError());
      ASSERT_TRUE(D.init(*Img));
      // A 32-bit instruction may start after a compressed instruction.
      const va_t PC = File.Entry + (C.Compressed ? 2 : 0);
      const auto Check = [&](uint32_t Word, bool Expected) {
        uint8_t Code[4];
        llvm::support::endian::write32le(Code, Word);
        DecodedInsn I;
        ASSERT_EQ(D.decodeOneForLift(Code, sizeof(Code), PC, I), 4);
        std::vector<LowOp> Ops;
        if (Expected)
          EXPECT_NO_THROW(D.liftToLow(I, Ops));
        else
          EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
      };
      Check(zicond_fixture::Eqz, C.Zicond);
      Check(zicond_fixture::Nez, C.Zicond);
      Check(0x20b52533, C.Zba);         // sh1add a0,a0,a1
      Check(0x60b51533, C.Zbb);         // rol a0,a0,a1
      Check(0x28b51533, C.Zbs);         // bset a0,a0,a1
      Check(0x02b50533, C.Multiply);    // mul a0,a0,a1
      Check(0x02b54533, C.Divide);      // div a0,a0,a1
      Check(0x00b5252f, C.Amo);         // amoadd.w a0,a1,(a0)
      Check(0x1005252f, C.Reservation); // lr.w a0,(a0)
      Check(0x18b5252f, C.Reservation); // sc.w a0,a1,(a0)
      EXPECT_EQ(Img->isFunctionEntryCandidateAt(File.Entry + 2), C.Compressed);

      PipelineOptions Options;
      Options.OnlyFunctionEntries = {File.Entry};
      Options.LiftMode = true;
      Options.EmitDumpOutput = false;
      llvm::LLVMContext Ctx;
      auto R = Pipeline().run(*Img, Ctx, Options);
      ASSERT_TRUE(R.Success) << R.Error;
      ASSERT_TRUE(R.LlvmModule);
      ASSERT_EQ(R.MedFuncs.size(), 1u);
      auto *Body = R.LlvmModule->getFunction(R.MedFuncs[0].Name);
      ASSERT_TRUE(Body);
      EXPECT_EQ(Body->getFnAttribute("target-features").getValueAsString(),
                C.Features);
    }
    for (const char *Version : {"_zmmul1p1", "_zba1p1", "_zbb1p1", "_zbs2p0",
                                "_zicond1p1", "_zicond2p0"}) {
      LinkedImage Unsupported(A);
      Unsupported.riscvAttributes(Base + Version);
      auto Bad = ELFLoader().loadBuffer(Unsupported.buffer());
      ASSERT_FALSE(bool(Bad)) << Version;
      llvm::consumeError(Bad.takeError());
    }
  }
}

namespace conditional_return_fixture {
#include "PowerPCConditionalReturnFixture.inc"
} // namespace conditional_return_fixture

TEST(PowerPCControl, ConditionalReturnRequiresTheCallersLinkOnEveryPath) {
  using namespace conditional_return_fixture;
  for (Arch A : {Arch::PPC32, Arch::PPC32LE, Arch::PPC64, Arch::PPC64LE}) {
    for (bool Clobber : {false, true}) {
      LinkedImage File(A);
      // The second path either preserves LR or replaces it with r4.
      File.setWords({Compare, Skip, Clobber ? SetLR : Compare, BeqLR, Ret});
      auto Img = ELFLoader().loadBuffer(File.buffer());
      ASSERT_TRUE(bool(Img)) << llvm::toString(Img.takeError());
      Decoder D;
      ASSERT_TRUE(D.init(*Img));
      DecodedInsn Insn;
      ASSERT_EQ(D.decodeOneForLift(File.Bytes.data() + 0x10c, 4,
                                   Img->Entry + 12, Insn),
                4);
      EXPECT_EQ(D.returnsToCaller(Insn), true);
      EXPECT_FALSE(D.isFunctionTerminator(Insn));
      auto Low = CFGBuilder().build(*Img, D, Img->Entry, "guarded");
      ASSERT_TRUE(Low.hasCompleteLiftCoverage());
      const auto *Block = Low.blockFor(Img->Entry + 12);
      ASSERT_TRUE(Block && !Block->InstructionBoundaries.empty());
      EXPECT_EQ(Block->InstructionBoundaries.back().Control,
                LowInstructionControl::ConditionalReturn);
      ASSERT_EQ(Block->Succs.size(), 1u);
      EXPECT_EQ(Low.Blocks[Block->Succs.front()].StartAddr, Img->Entry + 16);
      LowToMedConverter Converter;
      Converter.setBinaryImage(&*Img);
      if (Clobber)
        EXPECT_THROW(Converter.convert(Low, A, BinaryFormat::ELF),
                     LowToMedConversionError);
      else
        EXPECT_NO_THROW(Converter.convert(Low, A, BinaryFormat::ELF));
    }
    for (uint32_t Word : Unsupported) {
      LinkedImage File(A);
      File.setWords({Word, Ret});
      auto Img = ELFLoader().loadBuffer(File.buffer());
      ASSERT_TRUE(bool(Img)) << llvm::toString(Img.takeError());
      Decoder D;
      ASSERT_TRUE(D.init(*Img));
      auto Low = CFGBuilder().build(*Img, D, Img->Entry, "unmodeled");
      EXPECT_FALSE(Low.hasCompleteLiftCoverage());
    }
  }
}

TEST(PowerPCControl, ConditionalCTRPreservesThePhysicalIndirectEdge) {
  using namespace conditional_return_fixture;
  for (Arch A : {Arch::PPC32, Arch::PPC32LE, Arch::PPC64, Arch::PPC64LE}) {
    for (bool Taken : {false, true}) {
      LinkedImage File(A);
      File.setWords({Taken ? EQ : Zero, SetCTR, SetCR, BeqCTR, Ret});
      auto Img = ELFLoader().loadBuffer(File.buffer());
      ASSERT_TRUE(bool(Img)) << llvm::toString(Img.takeError());
      Decoder D;
      ASSERT_TRUE(D.init(*Img));
      auto Low = CFGBuilder().build(*Img, D, Img->Entry, "unknown_target");
      ASSERT_TRUE(Low.hasCompleteLiftCoverage());
      const auto *Block = Low.blockFor(Img->Entry);
      ASSERT_TRUE(Block && !Block->InstructionBoundaries.empty());
      const auto &Boundary = Block->InstructionBoundaries.back();
      EXPECT_EQ(Boundary.Control, LowInstructionControl::Branch);
      EXPECT_FALSE(Boundary.Immediate);
      EXPECT_TRUE(hasLowInstructionControlFlag(
          Boundary.ControlFlags, LowInstructionControlFlag::InstructionGuard));
      EXPECT_TRUE(hasLowInstructionControlFlag(
          Boundary.ControlFlags, LowInstructionControlFlag::Indirect));
      ASSERT_EQ(Block->Succs.size(), 1u);
      EXPECT_EQ(Low.Blocks[Block->Succs.front()].StartAddr, Img->Entry + 16);
      const va_t Target = Img->Entry + 24;
      NdOpEmulator Emu(*Img);
      Emu.setStrictMode(true);
      auto Reached = Emu.computeTarget(*Block, integerreg::gpr(4), Target + 3);
      EXPECT_EQ(bool(Reached), Taken);
      if (Reached)
        EXPECT_EQ(Reached->Address, Target);
      EXPECT_EQ(Emu.getRegister(integerreg::ppc::CTR), Target + 3);
      LowToMedConverter Converter;
      Converter.setBinaryImage(&*Img);
      EXPECT_THROW(Converter.convert(Low, A, BinaryFormat::ELF),
                   LowToMedConversionError);
    }
  }
}

TEST(PowerPCControl, LinkedDisplacementsUseTheGuestAddressWidth) {
  using namespace conditional_return_fixture;
  for (Arch A : {Arch::PPC32, Arch::PPC32LE, Arch::PPC64, Arch::PPC64LE}) {
    LinkedImage File(A);
    File.setWords({LinkNext, LinkBack, AbsoluteBack});
    auto Img = ELFLoader().loadBuffer(File.buffer());
    ASSERT_TRUE(bool(Img)) << llvm::toString(Img.takeError());
    Decoder D;
    ASSERT_TRUE(D.init(*Img));
    const va_t Mask = isPowerPC64(A) ? UINT64_MAX : UINT32_MAX;
    for (auto [Offset, PC, Target] :
         {std::tuple<unsigned, va_t, va_t>{0, Mask - 3, 0},
          {4, 4, Mask - 3},
          {8, 0x1000, Mask - 7}}) {
      DecodedInsn I;
      ASSERT_EQ(
          D.decodeOneForLift(File.Bytes.data() + 0x100 + Offset, 4, PC, I), 4);
      std::vector<LowOp> Ops;
      ASSERT_NO_THROW(D.liftToLow(I, Ops));
      ASSERT_FALSE(Ops.empty());
      EXPECT_EQ(D.directCallTarget(I), Offset ? Target : InvalidVA);
      EXPECT_EQ(Ops.back().Opcode, Offset ? NdOp::CALL : NdOp::COPY);
      EXPECT_EQ(Ops.back().Inputs[0].Offset, Target);
    }
  }
}

TEST(RISCVControl, ZeroBaseJalrHasAnAbsoluteAlignedTarget) {
  for (Arch A : {Arch::RISCV32, Arch::RISCV64}) {
    BinaryImage Img;
    Img.Arch = A;
    Img.Bits = A == Arch::RISCV32 ? Bitness::Bits32 : Bitness::Bits64;
    for (bool Compressed : {false, true}) {
      Img.RISCV.Compressed = Compressed;
      Decoder D;
      ASSERT_TRUE(D.init(Img));
      for (int Disp : {1, 3, 0x7fd, -3}) {
        for (unsigned Rd : {0u, 1u, 7u}) {
          uint8_t Code[4];
          llvm::support::endian::write32le(Code, (uint32_t(Disp) << 20) |
                                                     Rd << 7 | 0x67);
          DecodedInsn I;
          ASSERT_EQ(D.decodeOneForLift(Code, sizeof(Code), 0x1000, I), 4);
          va_t Target = va_t(Disp) & ~va_t(1);
          if (A == Arch::RISCV32)
            Target = uint32_t(Target);
          const bool Aligned = Compressed || !(Target & 3);
          EXPECT_EQ(D.directCallTarget(I),
                    Rd == 1 && Aligned ? Target : InvalidVA);
          std::vector<LowOp> Ops;
          if (!Aligned) {
            EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
            continue;
          }
          ASSERT_NO_THROW(D.liftToLow(I, Ops));
          ASSERT_FALSE(Ops.empty());
          EXPECT_EQ(Ops.back().Opcode, Rd == 1 ? NdOp::CALL : NdOp::BRANCH);
          EXPECT_EQ(Ops.back().Inputs[0].Provenance,
                    ConstantAddressProvenance::CodeAddress);
          EXPECT_EQ(Ops.back().Inputs[0].Offset, Target);
        }
      }
    }
  }
}

#include "RISCVIndexedLoadFixture.inc"

TEST(RISCVIndexedLoad, OnlyTheBoundedReadObservesTheRebasedAddress) {
  using namespace indexed_load;
  for (Arch A : {Arch::RISCV32, Arch::RISCV64})
    for (bool Sections : {false, true})
      for (const auto &[Index, Add, Load, Writable, Rebase] :
           {std::tuple{Mask31, BaseFirst, LoadByte, false, true},
            std::tuple{A == Arch::RISCV32 ? Shift : ShiftWord, BaseLast,
                       LoadByte, false, true},
            std::tuple{Mask63, BaseLast, LoadByte, false, false},
            std::tuple{Mask31, BaseFirst, LoadHalfword, false, false},
            std::tuple{SignedShift, BaseFirst, LoadByte, false, false},
            std::tuple{Argument, BaseLast, LoadByte, false, false},
            std::tuple{Mask31, BaseLast, LoadByte, true, false}}) {
        SCOPED_TRACE(::testing::Message()
                     << getArchName(A) << " " << Sections << " " << std::hex
                     << Index << " " << Load << " " << Writable);
        auto File = file(A, Index, Add, Load, Sections, Writable);
        auto Image = ELFLoader().loadBuffer(File.buffer());
        ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
        PipelineOptions Options;
        Options.OnlyFunctionEntries = {Image->Entry};
        Options.EmitDumpOutput = false;
        llvm::LLVMContext Context;
        auto R = Pipeline().run(*Image, Context, Options);
        ASSERT_TRUE(R.Success) << R.Error;
        ASSERT_EQ(R.MedFuncs.size(), 1u);
        ASSERT_EQ(R.MedIRVerifierFailures, 0u);
        const auto &Ops = R.MedFuncs.front().Blocks.front().Ops;
        const auto Read = llvm::find_if(
            Ops, [](const MedOp &Op) { return Op.Opcode == NdOp::LOAD; });
        const auto Xor = llvm::find_if(
            Ops, [](const MedOp &Op) { return Op.Opcode == NdOp::INT_XOR; });
        ASSERT_NE(Read, Ops.end());
        ASSERT_NE(Xor, Ops.end());
        const auto Sum = llvm::find_if(Ops, [&](const MedOp &Op) {
          return Op.Opcode == NdOp::INT_ADD && Op.Output == Read->Inputs[0];
        });
        ASSERT_NE(Sum, Ops.end());
        const bool Owned = llvm::any_of(Sum->Inputs, [](const MedVar &V) {
          return V.isConst() && V.ConstVal == TableVA &&
                 V.Provenance == ConstantAddressProvenance::DataAddress &&
                 V.AddressOwnerVA == TableVA;
        });
        EXPECT_EQ(Owned, Rebase);
        // The XOR observes the same sum as an integer, even where the read
        // independently establishes which mapped bytes it addresses.
        if (Rebase)
          EXPECT_NE(Xor->Inputs[1], Read->Inputs[0]);
      }
}

TEST(RISCVControl, UnrelaxedCalleeRequiresAReachableAuipcDefinition) {
  for (Arch A : {Arch::RISCV32, Arch::RISCV64}) {
    // An entry jump can bypass AUIPC and enter JALR with a caller-supplied
    // base. Likewise, a different base register does not name this callee.
    for (const auto &[First, Call, Expected] :
         {std::tuple{0x00000013u, 0x00c280e7u, true},     // nop; jalr ra,12(t0)
          std::tuple{0x0080006fu, 0x00c280e7u, false},    // j +8
          std::tuple{0x00000013u, 0x00c300e7u, false},    // jalr ra,12(t1)
          std::tuple{0x00050463u, 0x00c280e7u, false}}) { // beqz a0,+8
      SCOPED_TRACE(::testing::Message() << std::hex << First << " " << Call);
      LinkedImage File(A);
      File.setWords({First, 0x00000297, Call,
                     0x00008067,               // auipc t0,0; ...; ret
                     0x00b50533, 0x00008067}); // add a0,a0,a1; ret
      auto Img = ELFLoader().loadBuffer(File.buffer());
      ASSERT_TRUE(bool(Img)) << llvm::toString(Img.takeError());
      Decoder D;
      ASSERT_TRUE(D.init(*Img));
      auto Entries = FuncDetector().detect(*Img, D);
      EXPECT_EQ(llvm::any_of(Entries,
                             [&](const auto &Entry) {
                               return Entry.first == File.Entry + 16;
                             }),
                Expected);
    }
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
