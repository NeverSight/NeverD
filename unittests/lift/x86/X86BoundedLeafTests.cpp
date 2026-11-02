//===- X86BoundedLeafTests.cpp - Proven SGX/SMX completion ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check CFG admission and complete state transport without privileged host
/// execution. Optional exports feed the independent native fault oracle.
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/c/render/X86/X86BoundedLeaf.h"
#include "../../../lib/backend/c/render/X86/X86NativeFeatures.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/lift/X86/X86Lifter.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/ModRef.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

#include <array>
#include <cstdlib>
#include <stdexcept>
#include <vector>

using namespace neverd;
namespace {
constexpr va_t Entry = 0x1000;
constexpr std::array Families = {Intrinsic::X86EncluBounded,
                                 Intrinsic::X86GetsecBounded};
std::vector<unsigned> leaves(Intrinsic Id) {
  return isX86EncluLeaf(Id) ? std::vector<unsigned>{0, 1, 5, 6, 7, 8, 9, 10, 11}
                            : std::vector<unsigned>{0, 6, 7, 8};
}
std::vector<uint8_t> encoding(Intrinsic Id) {
  return isX86EncluLeaf(Id) ? std::vector<uint8_t>{0x0f, 0x01, 0xd7}
                            : std::vector<uint8_t>{0x0f, 0x37};
}
void append(std::vector<uint8_t> &Bytes, const std::vector<uint8_t> &Tail) {
  Bytes.insert(Bytes.end(), Tail.begin(), Tail.end());
}
BinaryImage image(Arch A, std::vector<uint8_t> Bytes,
                  BinaryFormat Format = BinaryFormat::ELF) {
  BinaryImage Image;
  Image.Arch = A;
  Image.Bits = A == Arch::X64 ? Bitness::Bits64 : Bitness::Bits32;
  Image.Format = Format;
  Image.Entry = Image.Base = Entry;
  Segment Text;
  Text.VA = Entry;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = std::move(Bytes);
  Text.Size = Text.FileSz = Text.Data.size();
  auto Symbol = Symbol::makeFunc(Entry, Text.Size);
  Symbol.Name = "bounded_pipeline";
  Image.Symbols.push_back(std::move(Symbol));
  Image.Segments.push_back(std::move(Text));
  return Image;
}
LowFunc build(BinaryImage &Image) {
  Decoder D;
  EXPECT_TRUE(D.init(Image));
  return CFGBuilder().build(Image, D, Entry, "bounded");
}
const LowOp *operation(const LowFunc &F, bool Pending = false) {
  for (const auto &B : F.Blocks)
    for (const auto &O : B.Ops)
      if (O.Opcode == NdOp::INTRINSIC && O.NumInputs && O.Inputs[0].isConst()) {
        const auto Id = static_cast<Intrinsic>(O.Inputs[0].Offset);
        if (Pending ? isX86BoundedLeafPending(Id)
                    : isX86BoundedLeafIntrinsic(Id))
          return &O;
      }
  return nullptr;
}
MedVar temporary(int Id, uint16_t Size) {
  MedVar V;
  V.Id = Id;
  V.Size = Size;
  return V;
}

TEST(X86BoundedLeaf, LeafPolicyMatchesReturningInstructionSemantics) {
  // Independent expectations from SDM 093 Vol. 2D chapter 7 / Vol. 3D
  // chapter 41 and the extension reference's EREPORT2/EGETKEY256 leaves.
  // Bits name AX/BX/CX/DX. AX's constant selector is supplied separately.
  struct Expected {
    Intrinsic Id;
    unsigned Leaf, Reads, Writes;
    X86BoundedLeafFlags Flags;
  };
  using F = X86BoundedLeafFlags;
  const Expected Cases[] = {
      {Intrinsic::X86EncluBounded, 0, 14, 0, F::Preserve},
      {Intrinsic::X86EncluBounded, 1, 6, 1, F::Define},
      {Intrinsic::X86EncluBounded, 5, 6, 1, F::Define},
      {Intrinsic::X86EncluBounded, 6, 6, 0, F::Preserve},
      {Intrinsic::X86EncluBounded, 7, 14, 1, F::Define},
      {Intrinsic::X86EncluBounded, 8, 2, 1, F::Capture},
      {Intrinsic::X86EncluBounded, 9, 0, 0, F::Preserve},
      {Intrinsic::X86EncluBounded, 10, 14, 1, F::Define},
      {Intrinsic::X86EncluBounded, 11, 6, 1, F::Define},
      {Intrinsic::X86GetsecBounded, 0, 2, 1, F::Preserve},
      {Intrinsic::X86GetsecBounded, 6, 6, 7, F::Preserve},
      {Intrinsic::X86GetsecBounded, 7, 2, 0, F::Preserve},
      {Intrinsic::X86GetsecBounded, 8, 0, 0, F::Preserve}};
  for (const auto &E : Cases) {
    const auto C = x86BoundedLeafContract(E.Id, E.Leaf);
    ASSERT_TRUE(C);
    EXPECT_EQ(C->Reads, E.Reads);
    EXPECT_EQ(C->Writes, E.Writes);
    EXPECT_EQ(C->Flags, E.Flags);
  }
}

TEST(X86BoundedLeaf, DecoderRetainsUnprovenStateAndScalarEmulatorRefuses) {
  for (auto A : {Arch::X86, Arch::X64})
    for (auto Id : Families)
      for (uint8_t Prefix : {0, 0x67, 0x64, 0x65, 0x4f}) {
        if (A == Arch::X86 && Prefix == 0x4f)
          continue;
        auto Bytes = encoding(Id);
        if (Prefix)
          Bytes.insert(Bytes.begin(), Prefix);
        Decoder D;
        ASSERT_TRUE(D.init(A));
        DecodedInsn I{};
        ASSERT_EQ(D.decodeOneForLift(Bytes.data(), Bytes.size(), Entry, I),
                  Bytes.size());
        std::vector<LowOp> Ops;
        ASSERT_NO_THROW(D.liftToLow(I, Ops));
        unsigned Count = 0;
        for (const auto &O : Ops) {
          EXPECT_NE(O.Opcode, NdOp::NOP);
          if (O.Opcode != NdOp::INTRINSIC)
            continue;
          ++Count;
          const auto Pending = static_cast<Intrinsic>(O.Inputs[0].Offset);
          EXPECT_TRUE(isX86BoundedLeafPending(Pending));
          EXPECT_TRUE(x86BoundedLeafShapeIsValid(Pending,
                                                 x86BoundedLeafLowShape(O, A)));
          EXPECT_EQ(intrinsicOutputCount(Pending),
                    X86BoundedLeafAuxiliaryOutputs);
          EXPECT_TRUE(isSideeffectIntrinsic(Pending));
          auto Image = image(A, Bytes);
          NdOpEmulator Emu(Image);
          EXPECT_FALSE(Emu.step(O));
        }
        EXPECT_EQ(Count, 1u);
      }
}

TEST(X86BoundedLeaf, ProvesEveryBoundedLeafAndUsesOnlyItsArchitecturalInputs) {
  for (auto A : {Arch::X86, Arch::X64})
    for (auto Id : Families)
      for (unsigned Leaf : leaves(Id)) {
        std::vector<uint8_t> Bytes = {0xb8, uint8_t(Leaf), 0, 0, 0};
        append(Bytes, encoding(Id));
        Bytes.push_back(0xc3);
        auto Image = image(A, Bytes);
        const auto F = build(Image);
        ASSERT_TRUE(F.UnsupportedInstructionAddresses.empty()) << Leaf;
        const auto *O = operation(F);
        ASSERT_NE(O, nullptr) << Leaf;
        EXPECT_EQ(operation(F, true), nullptr);
        ASSERT_TRUE(O->Inputs[1].isConst());
        EXPECT_EQ(O->Inputs[1].Offset, Leaf);
        EXPECT_TRUE(
            x86BoundedLeafShapeIsValid(Id, x86BoundedLeafLowShape(*O, A)));
        const auto C = *x86BoundedLeafContract(Id, Leaf);
        const std::array<uint64_t, 4> Registers = {x86reg::RAX, x86reg::RBX,
                                                   x86reg::RCX, x86reg::RDX};
        for (unsigned I = 1; I < Registers.size(); ++I)
          EXPECT_EQ(O->Inputs[I + 1],
                    C.Reads & (1u << I)
                        ? NdVar::reg(Registers[I], A == Arch::X64 ? 8 : 4)
                        : NdVar::scalar(0, A == Arch::X64 ? 8 : 4));
        for (const auto &B : F.Blocks)
          for (const auto &Op : B.Ops)
            if (Op.Addr == O->Addr && Op.Output.isReg())
              for (unsigned I = 0; I < Registers.size(); ++I)
                if (Op.Output.Offset == Registers[I])
                  EXPECT_TRUE(C.Writes & (1u << I));
      }
}

TEST(X86BoundedLeaf, UnknownAndTransferringLeavesRemainUnsupported) {
  for (auto A : {Arch::X86, Arch::X64})
    for (auto Id : Families)
      for (int Leaf : {-1, 2, 3, 4, 12, 255}) {
        std::vector<uint8_t> Bytes;
        if (Leaf >= 0)
          Bytes = {0xb8, uint8_t(Leaf), 0, 0, 0};
        append(Bytes, encoding(Id));
        Bytes.push_back(0xc3);
        auto Image = image(A, Bytes);
        const auto F = build(Image);
        EXPECT_EQ(operation(F), nullptr);
        EXPECT_NE(operation(F, true), nullptr);
        EXPECT_FALSE(F.UnsupportedInstructionAddresses.empty());
        EXPECT_THROW(LowToMedConverter().convert(F, A),
                     LowToMedConversionError);
      }
}

TEST(X86BoundedLeaf, InvalidPrefixesAndForgedEncodingNeverPublishState) {
  for (auto A : {Arch::X86, Arch::X64})
    for (auto Id : Families) {
      Decoder D;
      ASSERT_TRUE(D.init(A));
      for (uint8_t Prefix : {0xf0, 0x66, 0xf2, 0xf3}) {
        auto Bytes = encoding(Id);
        Bytes.insert(Bytes.begin(), Prefix);
        DecodedInsn I{};
        if (D.decodeOneForLift(Bytes.data(), Bytes.size(), Entry, I)) {
          std::vector<LowOp> Ops;
          EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
          EXPECT_TRUE(Ops.empty());
        }
      }
      const auto Bytes = encoding(Id);
      DecodedInsn I{};
      ASSERT_EQ(D.decodeOneForLift(Bytes.data(), Bytes.size(), Entry, I),
                Bytes.size());
      for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
        cs_insn Raw = *I.Raw;
        cs_detail Detail = *Raw.detail;
        Raw.detail = &Detail;
        switch (Mutation) {
        case 0:
          Detail.x86.op_count = 1;
          break;
        case 1:
          Detail.x86.encoding.imm_size = 1;
          break;
        case 2:
          Detail.x86.encoding.disp_size = 1;
          break;
        case 3:
          Raw.bytes[Raw.size - 1] ^= 1;
          break;
        case 4:
          Detail.x86.opcode[0] = 0;
          break;
        }
        X86Lifter L(A);
        L.setStrict(true);
        std::vector<LowOp> Ops;
        EXPECT_THROW(L.lift(&Raw, Ops), UnliftedInstruction);
        EXPECT_TRUE(Ops.empty());
      }
    }
}

TEST(X86BoundedLeaf, ExceptionEntryCannotInheritAnOrdinaryPredecessorSelector) {
  std::vector<uint8_t> Bytes = {0xb8, 5, 0, 0, 0};
  append(Bytes, encoding(Intrinsic::X86EncluBounded));
  Bytes.push_back(0xc3);
  auto Image = image(Arch::X64, Bytes);
  ExceptionFunction EH;
  EH.CodeRange = {Entry, Entry + Bytes.size()};
  EH.Kind = RuntimeFunctionKind::Primary;
  EH.Encoding = ExceptionEncoding::DwarfFDE;
  EH.Personality = ExceptionPersonality::GxxPersonalityV0;
  ItaniumEHInfo Itanium;
  ItaniumCallSite Site;
  Site.GuardedRange = {Entry, Entry + 5};
  Site.LandingPadVA = Entry + 5;
  Itanium.CallSites.push_back(Site);
  EH.Itanium = std::move(Itanium);
  Image.ExceptionMetadata.Functions.push_back(std::move(EH));
  Image.ExceptionMetadata.rebuildIndex();
  const auto F = build(Image);
  EXPECT_EQ(operation(F), nullptr);
  EXPECT_FALSE(F.UnsupportedInstructionAddresses.empty());
}

TEST(X86BoundedLeaf, LowAndMedRequireConstantLeafCompleteStateAndCorrectMode) {
  for (auto A : {Arch::X86, Arch::X64})
    for (auto Id : Families) {
      const uint16_t Word = A == Arch::X64 ? 8 : 4;
      LowOp Low;
      Low.Addr = Entry;
      Low.Opcode = NdOp::INTRINSIC;
      Low.Output = NdVar::tmp(0, Word);
      Low.addInput(NdVar::cst(unsigned(Id), 2));
      for (unsigned I = 0; I < X86BoundedLeafRegisterCount; ++I)
        Low.addInput(NdVar::scalar(0, Word));
      Low.addInput(NdVar::scalar(0, X86ArithmeticFlagsBytes));
      auto BoundaryError = [](const LowOp &O) {
        LowBlock B;
        B.StartAddr = Entry;
        B.EndAddr = Entry + 3;
        B.Ops = {O};
        B.InstructionBoundaries.push_back({Entry, 3, 0, 1});
        return llvm::toString(validateLowInstructionBoundaries(B));
      };
      ASSERT_TRUE(BoundaryError(Low).empty());
      for (unsigned Mutation = 0; Mutation < 7; ++Mutation) {
        auto Bad = Low;
        switch (Mutation) {
        case 0:
          Bad.NumInputs--;
          break;
        case 1:
          Bad.Inputs[1] = NdVar::reg(x86reg::RAX, Word);
          break;
        case 2:
          Bad.Inputs[1].Offset = 2;
          break;
        case 3:
          Bad.Inputs[5].Size = 1;
          break;
        case 4:
          Bad.Output = NdVar::scalar(0, Word);
          break;
        case 5:
          Bad.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
          break;
        case 6:
          Bad.Inputs[1].Provenance = ConstantAddressProvenance::Address;
          break;
        }
        EXPECT_FALSE(BoundaryError(Bad).empty());
        EXPECT_FALSE(
            x86BoundedLeafShapeIsValid(Id, x86BoundedLeafLowShape(Bad, A)));
      }
      MedOp Med;
      Med.Opcode = NdOp::INTRINSIC;
      Med.Output = temporary(20, Word);
      for (unsigned I = 0; I < Low.NumInputs; ++I)
        Med.addInput(
            MedVar::makeConst(Low.Inputs[I].Offset, Low.Inputs[I].Size));
      for (unsigned I = 0; I < X86BoundedLeafAuxiliaryOutputs; ++I)
        Med.IntrinsicOutputs.push_back(temporary(21 + I, I < 3 ? Word : 2));
      ASSERT_TRUE(
          x86BoundedLeafShapeIsValid(Id, x86BoundedLeafMedShape(Med, A)));
      EXPECT_FALSE(x86BoundedLeafShapeIsValid(
          Id,
          x86BoundedLeafMedShape(Med, A == Arch::X64 ? Arch::X86 : Arch::X64)));
      for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
        auto Bad = Med;
        switch (Mutation) {
        case 0:
          Bad.IntrinsicOutputs.pop_back();
          break;
        case 1:
          Bad.IntrinsicOutputs[0].Id = Bad.Output.Id;
          break;
        case 2:
          Bad.IntrinsicOutputs[1].Id = Bad.IntrinsicOutputs[0].Id;
          break;
        case 3:
          Bad.IntrinsicOutputs.back().Size = Word;
          break;
        }
        EXPECT_FALSE(
            x86BoundedLeafShapeIsValid(Id, x86BoundedLeafMedShape(Bad, A)));
      }
    }
}

TEST(X86BoundedLeaf, OldFlagInputLivesOnlyWhenTheFlagResultIsObserved) {
  for (bool ReadFlags : {false, true}) {
    MedFunc F;
    MedBlock B;
    B.Id = 0;
    MedOp O;
    O.Opcode = NdOp::INTRINSIC;
    O.Output = temporary(10, 8);
    O.addInput(MedVar::makeConst(unsigned(Intrinsic::X86EncluBounded), 2));
    O.addInput(MedVar::makeConst(8, 8));
    for (unsigned I = 1; I < X86BoundedLeafRegisterCount; ++I)
      O.addInput(MedVar::makeConst(0, 8));
    O.addInput(temporary(0, 2));
    for (unsigned I = 0; I < X86BoundedLeafAuxiliaryOutputs; ++I)
      O.IntrinsicOutputs.push_back(temporary(11 + I, I < 3 ? 8 : 2));
    B.Ops.push_back(O);
    MedOp Result;
    Result.Opcode = ReadFlags ? NdOp::INT_ZEXT : NdOp::COPY;
    Result.Output = temporary(20, 8);
    Result.Output.Kind = MedVar::Reg;
    Result.Output.TheArch = Arch::X64;
    Result.Output.RegOff = x86reg::RAX;
    Result.addInput(ReadFlags ? O.IntrinsicOutputs.back() : O.Output);
    B.Ops.push_back(Result);
    MedOp Ret;
    Ret.Opcode = NdOp::RETURN;
    B.Ops.push_back(Ret);
    F.Blocks.push_back(std::move(B));
    LowToMedConverter().runRegisterDce(F, Arch::X64);
    ASSERT_FALSE(F.Blocks[0].Ops.empty());
    EXPECT_EQ(F.Blocks[0].Ops.front().Inputs[5].isConst(), !ReadFlags);
  }
}

TEST(X86BoundedLeaf, HighCRejectsMissingStateAddressSelectorsAndPendingLeaves) {
  for (unsigned Mutation = 0; Mutation < 9; ++Mutation) {
    auto E = HighExpr::makeCall("bounded", 0, {});
    E->IntrinsicId = Intrinsic::X86EncluBounded;
    E->Type = NdType::makeInt(8, false);
    E->Operands.push_back(HighExpr::makeConst(8, 8));
    for (unsigned I = 1; I < 5; ++I)
      E->Operands.push_back(HighExpr::makeVar(temporary(I, I == 4 ? 2 : 8)));
    for (unsigned I = 0; I < 4; ++I)
      E->IntrinsicOutputs.push_back(temporary(10 + I, I == 3 ? 2 : 8));
    switch (Mutation) {
    case 1:
      E->IntrinsicId = Intrinsic::X86EncluPending;
      break;
    case 2:
      E->Operands[0]->ConstProvenance = ConstantAddressProvenance::Address;
      break;
    case 3:
      E->IntrinsicOutputs.pop_back();
      break;
    case 4:
      E->IntrinsicOutputs.back().Id = E->IntrinsicOutputs.front().Id;
      break;
    case 5:
      E->Operands.back()->Type = NdType::makeInt(1, false);
      break;
    case 6:
      E->MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      break;
    case 7:
      E->DoesNotReturn = true;
      break;
    case 8:
      E->Operands[0]->ConstVal = 2;
      break;
    }
    const auto Render = [&] {
      return renderX86BoundedLeafExpression(
          Arch::X64, *E,
          [](const HighExpr &V) {
            return V.Kind == ExprKind::Const
                       ? std::to_string(V.ConstVal)
                       : "input_" + std::to_string(V.Var.Id);
          },
          [](const MedVar &V) { return "output_" + std::to_string(V.Id); },
          [](const MedVar &) { return true; });
    };
    if (Mutation)
      EXPECT_THROW(Render(), std::invalid_argument);
    else
      EXPECT_NO_THROW(Render());
  }
}

llvm::CallInst *stateWrapper(llvm::Module &M, Arch A, Intrinsic Id,
                             unsigned Leaf, bool Native) {
  auto &C = M.getContext();
  const unsigned Bits = A == Arch::X64 ? 64 : 32;
  auto *Word = llvm::IntegerType::get(C, Bits);
  auto *Pointer = llvm::PointerType::getUnqual(C);
  auto *F =
      llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(C),
                                                     {Pointer, Pointer}, false),
                             llvm::GlobalValue::ExternalLinkage, "probe", M);
  F->addFnAttr("target-features", isX86EncluLeaf(Id) ? "+sgx,+sahf" : "+sahf");
  llvm::IRBuilder<> B(llvm::BasicBlock::Create(C, "entry", F));
  const uint64_t Ax = Leaf | (A == Arch::X64 ? 0x8800000000000000ull : 0);
  std::vector<llvm::Value *> Arguments = {llvm::ConstantInt::get(Word, Ax)};
  for (unsigned I = 1; I < 5; ++I) {
    auto *V =
        B.CreateLoad(Word, B.CreateGEP(Word, F->getArg(0), B.getInt32(I)));
    Arguments.push_back(I == 4 ? B.CreateTrunc(V, B.getInt16Ty()) : V);
  }
  llvm::CallInst *Call;
  if (Native) {
    const auto ID =
        llvm::Intrinsic::lookupIntrinsicID(x86BoundedLeafNativeName(Id, A));
    Call = B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(&M, ID),
                        Arguments);
  } else {
    const std::vector<llvm::Type *> Types = {Word, Word, Word, Word,
                                             B.getInt16Ty()};
    auto *Signature =
        llvm::FunctionType::get(llvm::StructType::get(C, Types), Types, false);
    Call =
        B.CreateCall(llvm::InlineAsm::get(Signature, x86BoundedLeafAsm(Id, A),
                                          X86BoundedLeafAsmConstraints, true),
                     Arguments);
  }
  for (unsigned I = 0; I < 5; ++I) {
    auto *V = B.CreateExtractValue(Call, I);
    B.CreateStore(I == 4 ? B.CreateZExt(V, Word) : V,
                  B.CreateGEP(Word, F->getArg(1), B.getInt32(I)));
  }
  B.CreateRetVoid();
  return Call;
}

TEST(X86BoundedLeaf, LLVMToCRejectsForgedTypesSelectorsEffectsAndAssembly) {
  for (auto A : {Arch::X86, Arch::X64})
    for (auto Id : Families)
      for (bool Native : {false, true}) {
        if (Native && !llvm::Intrinsic::lookupIntrinsicID(
                          x86BoundedLeafNativeName(Id, A)))
          continue;
        for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
          llvm::LLVMContext C;
          llvm::Module M("invalid-bounded", C);
          auto *Call = stateWrapper(M, A, Id, 0, Native);
          ASSERT_EQ(x86BoundedLeafCallId(A, *Call), Id);
          switch (Mutation) {
          case 0:
            Call->setArgOperand(
                0, llvm::UndefValue::get(Call->getArgOperand(0)->getType()));
            break;
          case 1:
            Call->setArgOperand(0, llvm::ConstantInt::get(
                                       Call->getArgOperand(0)->getType(), 2));
            break;
          case 2:
            Call->setMemoryEffects(llvm::MemoryEffects::none());
            break;
          case 3:
            Call->setDoesNotReturn();
            break;
          case 4:
            if (Native) {
              EXPECT_THROW(x86BoundedLeafCallId(
                               A == Arch::X64 ? Arch::X86 : Arch::X64, *Call),
                           std::invalid_argument);
              continue;
            }
            Call->setCalledOperand(llvm::InlineAsm::get(
                Call->getFunctionType(), x86BoundedLeafAsm(Id, A) + "\n\tnop",
                X86BoundedLeafAsmConstraints, true));
            break;
          }
          EXPECT_THROW(x86BoundedLeafCallId(A, *Call), std::invalid_argument);
        }
      }
}

TEST(X86BoundedLeaf, ExportCompleteStateForIndependentOracle) {
  const char *Directory = std::getenv("NEVERD_BOUNDED_LEAF_EXPORT_DIR");
  if (!Directory)
    return;
  ASSERT_FALSE(
      llvm::sys::fs::create_directories(std::string(Directory) + "/state"));
  for (auto A : {Arch::X86, Arch::X64})
    for (auto Id : Families)
      for (unsigned Leaf : leaves(Id)) {
        llvm::LLVMContext C;
        llvm::Module M("bounded-state", C);
        M.setTargetTriple(llvm::Triple(A == Arch::X64
                                           ? "x86_64-unknown-linux-gnu"
                                           : "i386-unknown-linux-gnu"));
        const auto *Call =
            stateWrapper(M, A, Id, Leaf,
                         llvm::Intrinsic::lookupIntrinsicID(
                             x86BoundedLeafNativeName(Id, A)) != 0);
        ASSERT_FALSE(llvm::verifyModule(M, &llvm::errs()));
        const uint16_t Word = A == Arch::X64 ? 8 : 4;
        auto E = HighExpr::makeCall("bounded", 0, {});
        E->IntrinsicId = Id;
        E->Type = NdType::makeInt(Word, false);
        E->Operands.push_back(HighExpr::makeConst(
            Leaf | (Word == 8 ? 0x8800000000000000ull : 0), Word));
        for (unsigned I = 1; I < 5; ++I)
          E->Operands.push_back(
              HighExpr::makeVar(temporary(I, I == 4 ? 2 : Word)));
        for (unsigned I = 1; I < 5; ++I)
          E->IntrinsicOutputs.push_back(temporary(I, I == 4 ? 2 : Word));
        const auto Type = std::string(Word == 8 ? "uint64_t" : "uint32_t");
        std::string High = "#include <stdint.h>\n" +
                           x86NativeFunctionPreamble(*Call->getFunction(), A) +
                           "void probe(const " + Type + " *input, " + Type +
                           " *output) { output[0] = ";
        High += renderX86BoundedLeafExpression(
            A, *E,
            [](const HighExpr &V) {
              return V.Kind == ExprKind::Const
                         ? std::to_string(V.ConstVal) + "ULL"
                         : "input[" + std::to_string(V.Var.Id) + "]";
            },
            [](const MedVar &V) {
              return "output[" + std::to_string(V.Id) + "]";
            },
            [](const MedVar &) { return true; });
        High += "; }\n";
        auto Object = Codegen().compile(M, A, BinaryFormat::ELF);
        ASSERT_TRUE(Object.Success);
        CEmitterOptions Options;
        Options.TheArch = A;
        Options.Format = BinaryFormat::ELF;
        std::string LLVM, IR;
        llvm::raw_string_ostream LLVMOS(LLVM), IROS(IR);
        ASSERT_TRUE(LLVMCEmitter().emit(M, LLVMOS, Options));
        M.print(IROS, nullptr);
        const auto Stem = std::string(Directory) + "/state/" +
                          (A == Arch::X64 ? "64-" : "32-") +
                          (isX86EncluLeaf(Id) ? "enclu-" : "getsec-") +
                          std::to_string(Leaf);
        {
          std::error_code Error;
          llvm::raw_fd_ostream File(Stem + ".o", Error);
          ASSERT_FALSE(Error);
          File.write(reinterpret_cast<const char *>(Object.ObjectData.data()),
                     Object.ObjectData.size());
        }
        for (const auto &[Extension, Text] :
             {std::pair{"-high.c", High}, {"-llvm.c", LLVM}, {".ll", IR}}) {
          std::error_code Error;
          llvm::raw_fd_ostream File(Stem + Extension, Error);
          ASSERT_FALSE(Error);
          File << Text;
        }
      }
}

TEST(X86BoundedLeaf, MeetsEveryBranchAndLoopPredecessor) {
  for (auto A : {Arch::X86, Arch::X64})
    for (bool Same : {false, true}) {
      std::vector<uint8_t> Bytes = {0x85,
                                    0xff,
                                    0x74,
                                    7,
                                    0xb8,
                                    5,
                                    0,
                                    0,
                                    0,
                                    0xeb,
                                    5,
                                    0xb8,
                                    uint8_t(Same ? 5 : 7),
                                    0,
                                    0,
                                    0};
      append(Bytes, encoding(Intrinsic::X86EncluBounded));
      Bytes.push_back(0xc3);
      auto Image = image(A, Bytes);
      const auto F = build(Image);
      EXPECT_EQ(operation(F) != nullptr, Same);
      EXPECT_EQ(F.UnsupportedInstructionAddresses.empty(), Same);
    }
  for (bool ChangesAx : {false, true}) {
    std::vector<uint8_t> Bytes = {0xb8,
                                  5,
                                  0,
                                  0,
                                  0,
                                  0x85,
                                  0xff,
                                  0x74,
                                  4,
                                  0xff,
                                  uint8_t(ChangesAx ? 0xc0 : 0xcf),
                                  0xeb,
                                  0xf8};
    append(Bytes, encoding(Intrinsic::X86EncluBounded));
    Bytes.push_back(0xc3);
    auto Image = image(Arch::X64, Bytes);
    const auto F = build(Image);
    EXPECT_EQ(operation(F) != nullptr, !ChangesAx);
    EXPECT_EQ(F.UnsupportedInstructionAddresses.empty(), !ChangesAx);
  }
}

TEST(X86BoundedLeaf,
     PartialWritesNeedKnownHighBytesAndIndependentRootsRevokeProof) {
  for (bool Whole : {false, true}) {
    std::vector<uint8_t> Bytes;
    if (Whole)
      Bytes = {0x48, 0xb8, 1, 0, 0, 0, 0x44, 0x33, 0x22, 0x11};
    append(Bytes, {0xb0, 5});
    append(Bytes, encoding(Intrinsic::X86EncluBounded));
    Bytes.push_back(0xc3);
    auto Image = image(Arch::X64, Bytes);
    const auto F = build(Image);
    EXPECT_EQ(operation(F) != nullptr, Whole);
    if (Whole) {
      ASSERT_NE(operation(F), nullptr);
      EXPECT_EQ(operation(F)->Inputs[1].Offset, 0x1122334400000005ull);
    }
  }
  std::vector<uint8_t> Bytes = {0xb8, 5, 0, 0, 0};
  append(Bytes, encoding(Intrinsic::X86EncluBounded));
  Bytes.push_back(0xc3);
  auto Image = image(Arch::X64, Bytes);
  Image.CodeRefTargets.insert(Entry + 5);
  const auto F = build(Image);
  EXPECT_EQ(operation(F), nullptr);
  EXPECT_FALSE(F.UnsupportedInstructionAddresses.empty());
}

TEST(X86BoundedLeaf, PreservedAxCanSelectTheNextLeafButCallsCannot) {
  std::vector<uint8_t> Bytes = {0xb8, 9, 0, 0, 0};
  append(Bytes, encoding(Intrinsic::X86EncluBounded));
  append(Bytes, encoding(Intrinsic::X86EncluBounded));
  Bytes.push_back(0xc3);
  auto Image = image(Arch::X64, Bytes);
  const auto F = build(Image);
  EXPECT_TRUE(F.UnsupportedInstructionAddresses.empty());
  EXPECT_EQ(operation(F, true), nullptr);
  Bytes = {0xb8, 9, 0, 0, 0, 0xe8, 0xf6, 0x0f, 0, 0};
  append(Bytes, encoding(Intrinsic::X86EncluBounded));
  Bytes.push_back(0xc3);
  auto CallImage = image(Arch::X64, Bytes);
  const auto CallF = build(CallImage);
  EXPECT_EQ(operation(CallF), nullptr);
  EXPECT_FALSE(CallF.UnsupportedInstructionAddresses.empty());
}

TEST(X86BoundedLeaf, PipelineEmitsNativeOrOwnedStateInEveryFormatAndMode) {
  const char *Directory = std::getenv("NEVERD_BOUNDED_LEAF_EXPORT_DIR");
  if (Directory)
    ASSERT_FALSE(llvm::sys::fs::create_directories(Directory));
  for (auto A : {Arch::X86, Arch::X64})
    for (auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (auto Id : Families)
        for (unsigned Leaf : leaves(Id))
          for (bool NoOpt : {false, true}) {
            SCOPED_TRACE(std::to_string(unsigned(A)) + "/" +
                         std::to_string(unsigned(Format)) + "/" +
                         std::to_string(unsigned(Id)) + "/" +
                         std::to_string(Leaf) + "/" + std::to_string(NoOpt));
            std::vector<uint8_t> Bytes = {
                0xbb, 0x20, 0, 0, 0,    0xb9,          0x40, 0, 0, 0,    0xba,
                0x60, 0,    0, 0, 0xb8, uint8_t(Leaf), 0,    0, 0, 0x39, 0xc0};
            append(Bytes, encoding(Id));
            append(Bytes, {0x9f, 0xc3});
            auto Image = image(A, Bytes, Format);
            PipelineOptions O;
            O.EmitDumpOutput = false;
            O.NoOpt = NoOpt;
            O.OnlyFunctionEntries = {Entry};
            llvm::LLVMContext Context;
            auto R = Pipeline().run(Image, Context, O);
            ASSERT_TRUE(R.Success) << R.Error;
            auto Module = MedLLVMEmitter().emit(R.MedFuncs, Context, "bounded",
                                                A, {}, &Image, Format);
            ASSERT_TRUE(Module);
            ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
            CEmitterOptions Options;
            Options.TheArch = A;
            Options.Format = Format;
            Options.Image = &Image;
            std::string High, LLVM, IR;
            llvm::raw_string_ostream HighOS(High), LLVMOS(LLVM), IROS(IR);
            Module->print(IROS, nullptr);
            ASSERT_TRUE(HighCEmitter().emit(R.HighFuncs, HighOS, Options));
            Pipeline::promoteScaffoldingAllocas(*Module);
            ASSERT_TRUE(LLVMCEmitter().emit(*Module, LLVMOS, Options));
            EXPECT_NE(High.find(X86BoundedLeafAsmMarker), std::string::npos)
                << High;
            EXPECT_NE(LLVM.find(X86BoundedLeafAsmMarker), std::string::npos)
                << LLVM;
            auto Object = Codegen().compile(*Module, A, Format);
            ASSERT_TRUE(Object.Success) << IR;
            ASSERT_FALSE(Object.ObjectData.empty());
            if (Directory) {
              const auto Stem = std::string(Directory) + "/" +
                                (A == Arch::X64 ? "64-" : "32-") +
                                std::to_string(unsigned(Format)) + "-" +
                                (isX86EncluLeaf(Id) ? "enclu-" : "getsec-") +
                                std::to_string(Leaf) +
                                (NoOpt ? "-raw" : "-opt");
              for (const auto &[Extension, Text] : {std::pair{"-high.c", High},
                                                    {"-llvm.c", LLVM},
                                                    {".ll", IR}}) {
                std::error_code Error;
                llvm::raw_fd_ostream File(Stem + Extension, Error);
                ASSERT_FALSE(Error);
                File << Text;
              }
            }
          }
}
} // namespace
