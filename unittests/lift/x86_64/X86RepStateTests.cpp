//===- X86RepStateTests.cpp - REP completion contracts and source --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise decoded controls, complete register returns, malformed interfaces
/// and both source routes. Optional exports feed an independent hardware
/// oracle.
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/c/render/X86/X86NativeFeatures.h"
#include "../../../lib/backend/c/render/X86/X86RepState.h"
#include "../core/SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/X86/X86AmxState.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/X86/X86RepAddresses.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/ModRef.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

using namespace neverd;

namespace {

constexpr std::array Families = {
#define NEVERD_X86_REP_STATE(Id, ...) Intrinsic::Id,
#include "neverd/ir/intrinsics/X86/X86RepState.def"
#undef NEVERD_X86_REP_STATE
};
constexpr va_t Entry = 0x1000;

MedVar temporary(int Id, uint16_t Size) {
  MedVar V;
  V.Id = Id;
  V.Size = Size;
  return V;
}

std::vector<X86RepAsm> forms(Arch A) {
  std::vector<X86RepAsm> Plans;
  for (auto Id : Families) {
    const auto C = *x86RepContract(Id);
    for (unsigned Width : {1, 2, 4, 8})
      for (unsigned Address : {2, 4, 8})
        for (unsigned Segment : {0, 0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65})
          for (bool NotEqual : {false, true})
            if (auto D =
                    x86RepDescriptor(Id, A, Width, Address, Segment, NotEqual))
              Plans.push_back(*x86RepAsm(Id, A, *D));
  }
  return Plans;
}

std::vector<uint8_t> encoding(const X86RepAsm &P) {
  std::vector<uint8_t> Bytes;
  if (P.Control.AddressBytes != P.WordBytes)
    Bytes.push_back(0x67);
  if (P.Control.Segment)
    Bytes.push_back(P.Control.Segment);
  Bytes.push_back(P.Control.NotEqual ? 0xf2 : 0xf3);
  if (P.Control.ElementBytes == 2)
    Bytes.push_back(0x66);
  if (P.Control.ElementBytes == 8)
    Bytes.push_back(0x48);
  const uint8_t Opcodes[] = {0xa4, 0xaa, 0xac, 0xa6, 0xae, 0x6c, 0x6e};
  const auto Index =
      std::find(Families.begin(), Families.end(), P.Id) - Families.begin();
  Bytes.push_back(Opcodes[Index] + (P.Control.ElementBytes != 1));
  return Bytes;
}

std::string name(const X86RepAsm &P) {
  return "rep_" + std::string(P.Contract.Name) + '_' +
         std::to_string(P.Control.ElementBytes) + '_' +
         std::to_string(P.Control.AddressBytes * 8) + '_' +
         std::to_string(P.Control.Segment) + '_' +
         std::to_string(P.Control.NotEqual);
}

llvm::CallInst *call(llvm::Module &M, llvm::IRBuilder<> &B, const X86RepAsm &P,
                     bool Native, std::vector<llvm::Value *> Inputs) {
  if (Native) {
    Inputs[3] = B.CreateTrunc(Inputs[3], B.getInt8Ty());
    if (P.Contract.Compare)
      Inputs[4] = B.CreateTrunc(Inputs[4], B.getInt16Ty());
    for (unsigned V : {P.Control.ElementBytes, P.Control.AddressBytes * 8})
      Inputs.push_back(B.getInt32(V));
    if (P.Contract.Source)
      Inputs.push_back(B.getInt32(P.Control.Segment));
    if (P.Contract.Compare)
      Inputs.push_back(B.getInt32(P.Control.NotEqual));
    const auto ID = llvm::Intrinsic::lookupIntrinsicID(
        x86RepNativeName(P.Id, P.WordBytes == 8 ? Arch::X64 : Arch::X86));
    return B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(&M, ID),
                        Inputs);
  }
  std::vector<llvm::Type *> Types;
  for (auto *V : Inputs)
    Types.push_back(V->getType());
  auto *Word = B.getIntNTy(P.WordBytes * 8);
  auto *Result = llvm::StructType::get(
      M.getContext(),
      std::vector<llvm::Type *>(P.OutputConstraints.size(), Word));
  auto *Signature = llvm::FunctionType::get(Result, Types, false);
  return B.CreateCall(
      llvm::InlineAsm::get(Signature, P.Text, P.Constraints, true), Inputs);
}

llvm::CallInst *wrapper(llvm::Module &M, const X86RepAsm &P, bool Native) {
  llvm::IRBuilder<> B(M.getContext());
  auto *Word = B.getIntNTy(P.WordBytes * 8);
  auto *Pointer = B.getPtrTy();
  auto *F = llvm::Function::Create(
      llvm::FunctionType::get(B.getVoidTy(), {Pointer, Pointer}, false),
      llvm::GlobalValue::ExternalLinkage, name(P), M);
  if (P.Contract.Compare && P.WordBytes == 8)
    F->addFnAttr("target-features", "+sahf");
  B.SetInsertPoint(llvm::BasicBlock::Create(M.getContext(), "entry", F));
  std::vector<llvm::Value *> Inputs;
  for (unsigned I = 0; I < (P.Contract.Compare ? 5u : 4u); ++I) {
    auto *V = B.CreateLoad(Word, B.CreateConstGEP1_32(Word, F->getArg(0), I));
    Inputs.push_back(I == 3 ? B.CreateZExtOrTrunc(V, B.getInt32Ty())
                     : I == 2 && P.Contract.Port
                         ? B.CreateTrunc(V, B.getInt16Ty())
                         : V);
  }
  auto *Call = call(M, B, P, Native, Inputs);
  for (unsigned I = 0; I < P.Contract.WordResults + P.Contract.Compare; ++I) {
    auto *V = B.CreateZExtOrTrunc(B.CreateExtractValue(Call, I), Word);
    const unsigned Index = I == P.Contract.WordResults ? 3 : I;
    B.CreateStore(V, B.CreateConstGEP1_32(Word, F->getArg(1), Index));
  }
  B.CreateRetVoid();
  return Call;
}

TEST(X86RepState, DecodedControlsKeepFullInputsAndActualOutputs) {
  for (const auto A : {Arch::X86, Arch::X64}) {
    Decoder D;
    ASSERT_TRUE(D.init(A));
    D.setStrict(true);
    for (const auto &P : forms(A)) {
      SCOPED_TRACE(name(P));
      const auto Bytes = encoding(P);
      DecodedInsn I{};
      ASSERT_EQ(D.decodeOneForLift(Bytes.data(), Bytes.size(), Entry, I),
                int(Bytes.size()));
      std::vector<LowOp> Ops;
      D.liftToLow(I, Ops);
      const auto It = std::find_if(Ops.begin(), Ops.end(), [](const LowOp &Op) {
        return Op.Opcode == NdOp::INTRINSIC;
      });
      ASSERT_NE(It, Ops.end());
      EXPECT_EQ(It->Inputs[0].Offset, static_cast<uint16_t>(P.Id));
      EXPECT_EQ(It->Inputs[5].Offset, P.Descriptor);
      EXPECT_TRUE(x86RepShapeIsValid(P.Id, x86RepLowShape(*It, A)));
      EXPECT_EQ(It->Inputs[1].Size, P.WordBytes);
      EXPECT_EQ(It->Inputs[2].Size, P.WordBytes);
      EXPECT_EQ(It->Inputs[3].Size, P.Contract.Port ? 2 : P.WordBytes);
      ASSERT_GE(Ops.end() - It, 1 + P.Contract.auxiliaryOutputs());
      for (unsigned J = 1; J <= P.Contract.auxiliaryOutputs(); ++J) {
        EXPECT_EQ(It[J].Opcode, NdOp::COPY);
        EXPECT_TRUE(It[J].Inputs[0].isTemp());
        EXPECT_EQ(It[J].Inputs[0].Size,
                  J < P.Contract.WordResults ? P.WordBytes : X86RepFlagBytes);
      }
      EXPECT_EQ(std::count_if(It + 1, Ops.end(),
                              [](const LowOp &Op) {
                                return Op.Opcode == NdOp::INT_ADD ||
                                       Op.Opcode == NdOp::INT_MULT;
                              }),
                0);
    }
  }
}

TEST(X86RepState, LiftedStatusNormalizesEachArchitecturalFlag) {
  const uint64_t Flags[] = {x86reg::CF, x86reg::PF, x86reg::AF,
                            x86reg::ZF, x86reg::SF, x86reg::OF};
  const unsigned Bits[] = {8, 10, 12, 14, 15, 0};
  const uint64_t Registers[][3] = {{x86reg::RSI, x86reg::RDI, x86reg::RCX},
                                   {x86reg::RDI, x86reg::RCX, x86reg::RAX},
                                   {x86reg::RSI, x86reg::RCX, x86reg::RAX},
                                   {x86reg::RSI, x86reg::RDI, x86reg::RCX},
                                   {x86reg::RDI, x86reg::RCX, x86reg::RAX},
                                   {x86reg::RDI, x86reg::RCX, x86reg::RDX},
                                   {x86reg::RSI, x86reg::RCX, x86reg::RDX}};
  for (auto A : {Arch::X86, Arch::X64}) {
    Decoder D;
    ASSERT_TRUE(D.init(A));
    for (unsigned Family = 0; Family < Families.size(); ++Family) {
      const auto Id = Families[Family];
      const auto P = *x86RepAsm(
          Id, A, *x86RepDescriptor(Id, A, 1, A == Arch::X64 ? 8 : 4, 0, false));
      const auto Bytes = encoding(P);
      DecodedInsn I{};
      ASSERT_EQ(D.decodeOneForLift(Bytes.data(), Bytes.size(), Entry, I),
                int(Bytes.size()));
      std::vector<LowOp> Ops;
      D.liftToLow(I, Ops);
      const auto It = std::find_if(Ops.begin(), Ops.end(), [](const LowOp &Op) {
        return Op.Opcode == NdOp::INTRINSIC;
      });
      ASSERT_NE(It, Ops.end());
      for (unsigned J = 0; J < 3; ++J)
        EXPECT_EQ(It->Inputs[J + 1].Offset, Registers[Family][J]);
      for (unsigned Selected = 0; Selected != 7; ++Selected) {
        BinaryImage Image;
        Image.Arch = A;
        NdOpEmulator Emulator(Image);
        Emulator.setStrictMode(true);
        Emulator.setRegister(x86reg::DF, 0xff);
        for (unsigned J = 0; J < 6; ++J)
          Emulator.setRegister(Flags[J], J == Selected ? 0x80 : 0);
        for (auto Before = Ops.begin(); Before != It; ++Before)
          ASSERT_TRUE(Emulator.step(*Before));
        const uint64_t Expected =
            0x10000 |
            (P.Contract.Compare && Selected < 6 ? 1u << Bits[Selected] : 0);
        EXPECT_EQ(Emulator.getRegister(It->Inputs[4].Offset),
                  std::optional<uint64_t>(Expected));
      }
    }
  }
}

TEST(X86RepState, LowAndMedRejectPartialOrAmbiguousState) {
  for (auto A : {Arch::X86, Arch::X64})
    for (auto Id : Families) {
      const auto C = *x86RepContract(Id);
      const unsigned Word = A == Arch::X64 ? 8 : 4;
      LowOp Low;
      Low.Opcode = NdOp::INTRINSIC;
      Low.Addr = Entry;
      Low.Output = NdVar::tmp(256, Word);
      Low.addInput(NdVar::scalar(static_cast<unsigned>(Id), 2));
      for (unsigned I = 0; I < 3; ++I)
        Low.addInput(NdVar::reg(I * 8, I == 2 && C.Port ? 2 : Word));
      Low.addInput(NdVar::scalar(0, X86RepStatusBytes));
      Low.addInput(
          NdVar::scalar(*x86RepDescriptor(Id, A, 1, Word, 0, false), 4));
      ASSERT_TRUE(x86RepShapeIsValid(Id, x86RepLowShape(Low, A)));
      auto BoundaryError = [](const LowOp &Op) {
        LowBlock Block;
        Block.StartAddr = Entry;
        Block.EndAddr = Entry + 2;
        Block.Ops.push_back(Op);
        Block.InstructionBoundaries.push_back({Entry, 2, 0, 1});
        return llvm::toString(validateLowInstructionBoundaries(Block));
      };
      EXPECT_TRUE(BoundaryError(Low).empty());
      for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
        auto Bad = Low;
        switch (Mutation) {
        case 0:
          Bad.NumInputs = 5;
          break;
        case 1:
          Bad.Inputs[1].Size = 2;
          break;
        case 2:
          Bad.Inputs[4].Size = 1;
          break;
        case 3:
          Bad.Inputs[5].Offset |= 128;
          break;
        case 4:
          Bad.Inputs[5] = NdVar::reg(32, 4);
          break;
        case 5:
          Bad.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
          break;
        case 6:
          Bad.Output = NdVar::scalar(0, Word);
          break;
        }
        EXPECT_FALSE(x86RepShapeIsValid(Id, x86RepLowShape(Bad, A)));
        EXPECT_NE(BoundaryError(Bad).find("invalid operand/output shape"),
                  std::string::npos);
      }
      MedOp Med;
      Med.Opcode = NdOp::INTRINSIC;
      Med.Output = temporary(20, Word);
      for (unsigned I = 0; I < Low.NumInputs; ++I)
        Med.addInput(
            Low.Inputs[I].isConst()
                ? MedVar::makeConst(Low.Inputs[I].Offset, Low.Inputs[I].Size)
                : temporary(I, Low.Inputs[I].Size));
      for (unsigned I = 0; I < C.auxiliaryOutputs(); ++I)
        Med.IntrinsicOutputs.push_back(
            temporary(21 + I, I + 1 < C.WordResults ? Word : 2));
      ASSERT_TRUE(x86RepShapeIsValid(Id, x86RepMedShape(Med, A)));
      for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
        auto Bad = Med;
        switch (Mutation) {
        case 0:
          Bad.IntrinsicOutputs.pop_back();
          break;
        case 1:
          Bad.IntrinsicOutputs.back().Size = 1;
          break;
        case 2:
          Bad.IntrinsicOutputs.back().Id = Bad.Output.Id;
          break;
        case 3:
          Bad.IntrinsicOutputs.back().Kind = MedVar::Param;
          break;
        }
        EXPECT_FALSE(x86RepShapeIsValid(Id, x86RepMedShape(Bad, A)));
      }
    }
}

TEST(X86RepState, HighCRejectsLostStateAndEvaluatesPackedStatusOnce) {
  for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
    auto E = HighExpr::makeCall("rep", 0, {});
    E->IntrinsicId = Intrinsic::X86RepCmpsState;
    E->Type = NdType::makeInt(8, false);
    for (unsigned I = 0; I != 4; ++I)
      E->Operands.push_back(HighExpr::makeVar(temporary(I, I == 3 ? 4 : 8)));
    E->Operands.push_back(HighExpr::makeConst(
        *x86RepDescriptor(E->IntrinsicId, Arch::X64, 1, 4, 0x65, true), 4));
    E->IntrinsicOutputs = {temporary(10, 8), temporary(11, 8),
                           temporary(12, 2)};
    switch (Mutation) {
    case 1:
      E->IntrinsicOutputs.pop_back();
      break;
    case 2:
      E->IntrinsicOutputs[1].Id = E->IntrinsicOutputs[0].Id;
      break;
    case 3:
      E->IntrinsicOutputs[2].Size = 8;
      break;
    case 4:
      E->Operands[4]->ConstVal |= 128;
      break;
    case 5:
      E->Operands[3]->Type = NdType::makeInt(1, false);
      break;
    case 6:
      E->DoesNotReturn = true;
      break;
    case 7:
      E->IsIndirectCall = true;
      break;
    case 8:
      E->MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
      break;
    }
    const auto Render = [&] {
      return renderX86RepExpression(
          Arch::X64, *E, "uint64_t",
          [](const HighExpr &V) {
            return V.Kind == ExprKind::Const ? std::to_string(V.ConstVal)
                   : V.Var.Id == 3           ? std::string("status_provider()")
                                             : "input";
          },
          [](const MedVar &V) { return "out" + std::to_string(V.Id); },
          [](const MedVar &) { return true; });
    };
    if (Mutation)
      EXPECT_THROW(Render(), std::invalid_argument);
    else {
      const auto Text = Render();
      const auto At = Text.find("status_provider()");
      ASSERT_NE(At, std::string::npos);
      EXPECT_EQ(Text.find("status_provider()", At + 1), std::string::npos);
    }
  }
}

TEST(X86RepState, OwnedAssemblyRejectsLostEffectsAndAlteredControls) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto Id : Families)
      for (unsigned Mutation = 0; Mutation < 12; ++Mutation) {
        SCOPED_TRACE(static_cast<unsigned>(Id));
        SCOPED_TRACE(Mutation);
        const auto P = *x86RepAsm(
            Id, A,
            *x86RepDescriptor(Id, A, 1, A == Arch::X64 ? 8 : 4, 0, false));
        llvm::LLVMContext Context;
        llvm::Module M("rep-contract", Context);
        auto *Call = wrapper(M, P, false);
        auto *Asm = llvm::cast<llvm::InlineAsm>(Call->getCalledOperand());
        std::string Text = P.Text, Constraints = P.Constraints;
        if (Mutation == 1)
          Text += "\n\tnop";
        if (Mutation == 2)
          Constraints.erase(Constraints.find(",~{memory}"));
        if (Mutation == 3)
          Text = "# neverd-rep-state-v1:broken";
        if (Mutation == 9)
          Constraints.erase(Constraints.find(",~{flags}"));
        if (Mutation == 10)
          Constraints.replace(Constraints.find("~{flags}"), 8, "~{cc}");
        Call->setCalledOperand(llvm::InlineAsm::get(
            Asm->getFunctionType(), Text, Constraints, Mutation != 4));
        if (Mutation == 5)
          Call->setOnlyReadsMemory();
        if (Mutation == 6)
          Call->setOnlyWritesMemory();
        if (Mutation == 7)
          Call->setDoesNotReturn();
        if (Mutation == 11)
          Call->setMemoryEffects(llvm::MemoryEffects::argMemOnly());
        const auto Mode =
            Mutation == 8 ? (A == Arch::X64 ? Arch::X86 : Arch::X64) : A;
        if (Mutation)
          EXPECT_THROW(x86RepCallPlan(Mode, *Call), std::invalid_argument);
        else {
          EXPECT_TRUE(x86RepCallPlan(Mode, *Call));
          EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));
        }
      }
}

TEST(X86RepState, OwnedScasCapturesAccumulatorOncePerCompilerRoute) {
  const auto P = *x86RepAsm(
      Intrinsic::X86RepScasState, Arch::X64,
      *x86RepDescriptor(Intrinsic::X86RepScasState, Arch::X64, 1, 8, 0, false));
  llvm::LLVMContext Context;
  llvm::Module M("rep-scas-input", Context);
  auto *Call = wrapper(M, P, false);
  const auto Text = renderX86RepCall(
      Arch::X64, *Call, "result", true,
      {"destination", "count", "accumulator_provider()", "direction", "flags"});
  ASSERT_TRUE(Text);
  size_t At = 0;
  unsigned Occurrences = 0;
  while ((At = Text->find("accumulator_provider()", At)) != std::string::npos) {
    ++Occurrences;
    ++At;
  }
  // Each compile-time alternative evaluates the original input once.
  EXPECT_EQ(Occurrences, 2u) << *Text;
}

TEST(X86RepState, NativeContractsRejectWrongModeEffectsAndControls) {
  if (!llvm::Intrinsic::lookupIntrinsicID("llvm.x86.rep.movs.state"))
    GTEST_SKIP() << "native REP interfaces require the qualified LLVM fork";
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto Id : Families)
      for (unsigned Mutation = 0; Mutation < 10; ++Mutation) {
        SCOPED_TRACE(static_cast<unsigned>(Id));
        SCOPED_TRACE(Mutation);
        const auto P = *x86RepAsm(
            Id, A,
            *x86RepDescriptor(Id, A, 1, A == Arch::X64 ? 8 : 4, 0, false));
        llvm::LLVMContext Context;
        llvm::Module M("rep-native-contract", Context);
        auto *Call = wrapper(M, P, true);
        const unsigned Control = P.Contract.Compare ? 5 : 4;
        if (Mutation == 1)
          Call->setOnlyReadsMemory();
        if (Mutation == 2)
          Call->setOnlyWritesMemory();
        if (Mutation == 3)
          Call->setDoesNotReturn();
        if (Mutation == 9)
          Call->setMemoryEffects(llvm::MemoryEffects::argMemOnly());
        if (Mutation == 4)
          Call->setArgOperand(Control, llvm::ConstantInt::get(
                                           llvm::Type::getInt32Ty(Context), 3));
        if (Mutation == 5)
          Call->setArgOperand(
              Control + 1,
              llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), 33));
        if (Mutation == 6)
          llvm::BasicBlock::Create(Context, "body", Call->getCalledFunction());
        if (Mutation == 7)
          Call->setArgOperand(
              Control, llvm::PoisonValue::get(llvm::Type::getInt32Ty(Context)));
        const auto Mode =
            Mutation == 8 ? (A == Arch::X64 ? Arch::X86 : Arch::X64) : A;
        if (Mutation)
          EXPECT_THROW(x86RepCallPlan(Mode, *Call), std::invalid_argument);
        else
          EXPECT_TRUE(x86RepCallPlan(Mode, *Call));
      }
}

TEST(X86RepState, PipelinePreservesAllFamiliesAcrossModesAndFormats) {
  const char *Directory = std::getenv("NEVERD_REP_EXPORT_DIR");
  if (Directory)
    ASSERT_FALSE(llvm::sys::fs::create_directories(std::string(Directory) +
                                                   "/pipeline"));
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (const auto Id : Families)
        for (bool NoOpt : {false, true}) {
          SCOPED_TRACE(static_cast<unsigned>(A));
          SCOPED_TRACE(static_cast<unsigned>(Format));
          SCOPED_TRACE(static_cast<unsigned>(Id));
          SCOPED_TRACE(NoOpt);
          const auto P = *x86RepAsm(
              Id, A,
              *x86RepDescriptor(Id, A, 1, A == Arch::X64 ? 4 : 2, 0, false));
          BinaryImage Image;
          Image.Arch = A;
          Image.Bits = A == Arch::X64 ? Bitness::Bits64 : Bitness::Bits32;
          Image.Format = Format;
          Image.Entry = Image.Base = Entry;
          Segment Text;
          Text.VA = Entry;
          Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
          // CMP defines AF before LAHF observes it; XOR alone leaves AF
          // undefined and deliberately trips HighC's unknown-state guard.
          Text.Data = {0x31, 0xc0, 0x31, 0xc9, 0x31, 0xd2, 0xbe, 0x10, 0,   0,
                       0,    0xbf, 0x20, 0,    0,    0,    0x39, 0xc0, 0xfc};
          const auto Bytes = encoding(P);
          Text.Data.insert(Text.Data.end(), Bytes.begin(), Bytes.end());
          Text.Data.insert(Text.Data.end(), {0x9f, 0xc3});
          Text.Size = Text.FileSz = Text.Data.size();
          auto Symbol = Symbol::makeFunc(Entry, Text.Size);
          Symbol.Name = "rep_pipeline";
          Image.Symbols.push_back(std::move(Symbol));
          Image.Segments.push_back(std::move(Text));
          PipelineOptions O;
          O.EmitDumpOutput = false;
          O.NoOpt = NoOpt;
          O.OnlyFunctionEntries = {Entry};
          llvm::LLVMContext Context;
          auto R = Pipeline().run(Image, Context, O);
          ASSERT_TRUE(R.Success) << R.Error;
          ASSERT_EQ(R.MedFuncs.size(), 1u);
          auto Module = MedLLVMEmitter().emit(R.MedFuncs, Context, "rep", A, {},
                                              &Image, Format);
          ASSERT_TRUE(Module);
          ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
          std::string IR;
          llvm::raw_string_ostream IROS(IR);
          Module->print(IROS, nullptr);
          EXPECT_NE(IR.find(llvm::Intrinsic::lookupIntrinsicID(
                                x86RepNativeName(Id, A))
                                ? "@" + x86RepNativeName(Id, A)
                                : "# neverd-rep-state-v1:"),
                    std::string::npos)
              << IR;
          CEmitterOptions Options;
          Options.TheArch = A;
          Options.Format = Format;
          Options.Image = &Image;
          std::string High, LLVM;
          llvm::raw_string_ostream HighOS(High), LLVMOS(LLVM);
          ASSERT_TRUE(HighCEmitter().emit(R.HighFuncs, HighOS, Options));
          Pipeline::promoteScaffoldingAllocas(*Module);
          ASSERT_TRUE(LLVMCEmitter().emit(*Module, LLVMOS, Options));
          EXPECT_NE(High.find("# neverd-rep-state-v1:"), std::string::npos)
              << High;
          EXPECT_NE(LLVM.find("# neverd-rep-state-v1:"), std::string::npos)
              << LLVM;
          if (Directory) {
            const auto Stem = std::string(Directory) + "/pipeline/" +
                              (A == Arch::X64 ? "64-" : "32-") +
                              (Format == BinaryFormat::ELF    ? "elf-"
                               : Format == BinaryFormat::COFF ? "coff-"
                                                              : "macho-") +
                              P.Contract.Name + (NoOpt ? "-raw-" : "-opt-");
            std::error_code Error;
            llvm::raw_fd_ostream HighFile(Stem + "high.c", Error);
            ASSERT_FALSE(Error);
            HighFile << High;
            llvm::raw_fd_ostream LLVMFile(Stem + "llvm.c", Error);
            ASSERT_FALSE(Error);
            LLVMFile << LLVM;
          }
          auto Object = Codegen().compile(*Module, A, Format);
          ASSERT_TRUE(Object.Success) << IR;
          ASSERT_FALSE(Object.ObjectData.empty());
        }
}

TEST(X86RepState, MixedNativeFeaturesRetainComparisonFlagSupport) {
  HighFunc F;
  for (auto Id : {Intrinsic::X86RepCmpsState, Intrinsic::X86Encls,
                  Intrinsic::X86AMXState}) {
    HighStmt Statement;
    Statement.Kind = StmtKind::Call;
    Statement.CallExpr = HighExpr::makeCall("instruction", 0, {});
    Statement.CallExpr->IntrinsicId = Id;
    if (Id == Intrinsic::X86AMXState)
      Statement.CallExpr->Operands = {
          HighExpr::makeConst(0, 8),
          HighExpr::makeConst(X86AmxStateVersion | unsigned(X86AmxOp::Release),
                              4)};
    F.Body.push_back(Statement);
  }
  const auto Text = x86NativeFunctionPreamble(F, Arch::X64);
  for (const char *Features :
       {"amx-tile,sgx,sahf", "amx-tile,sahf", "sgx,sahf", "sahf"})
    EXPECT_NE(Text.find(std::string("target(\"") + Features + "\")"),
              std::string::npos)
        << Text;
}

TEST(X86RepState, ExportFullMatrixForIndependentOracle) {
  const char *Directory = std::getenv("NEVERD_REP_EXPORT_DIR");
  if (!Directory)
    return;
  ASSERT_FALSE(llvm::sys::fs::create_directories(Directory));
  for (auto A : {Arch::X86, Arch::X64}) {
    llvm::LLVMContext Context;
    llvm::Module M("rep-exports", Context);
    M.setDataLayout(A == Arch::X64 ? "e-p:64:64" : "e-p:32:32");
    M.setTargetTriple(llvm::Triple(A == Arch::X64 ? "x86_64-unknown-linux-gnu"
                                                  : "i386-unknown-linux-gnu"));
    std::string High = "#include <stdint.h>\n";
    for (const auto &P : forms(A)) {
      auto *Call = wrapper(
          M, P,
          llvm::Intrinsic::lookupIntrinsicID(x86RepNativeName(P.Id, A)) != 0);
      auto E = HighExpr::makeCall("rep", 0, {});
      E->IntrinsicId = P.Id;
      E->Type = NdType::makeInt(P.WordBytes, false);
      for (unsigned I = 0; I < 4; ++I)
        E->Operands.push_back(HighExpr::makeVar(
            temporary(I, I == 3                      ? 4
                         : I == 2 && P.Contract.Port ? 2
                                                     : P.WordBytes)));
      E->Operands.push_back(HighExpr::makeConst(P.Descriptor, 4));
      for (unsigned I = 1; I < P.Contract.WordResults + P.Contract.Compare; ++I)
        E->IntrinsicOutputs.push_back(
            temporary(I == P.Contract.WordResults ? 3 : I,
                      I == P.Contract.WordResults ? 2 : P.WordBytes));
      const auto Word = "uint" + std::to_string(P.WordBytes * 8) + "_t";
      High += x86NativeFunctionPreamble(*Call->getFunction(), A);
      High +=
          "void " + name(P) + "(const " + Word + " *in, " + Word + " *out) {\n";
      High += "uint32_t status = ((in[3] != 0) << 16) | (in[4] & "
              "54529);\nout[0] = ";
      High += renderX86RepExpression(
          A, *E, Word,
          [](const HighExpr &V) {
            return V.Kind == ExprKind::Const ? std::to_string(V.ConstVal)
                   : V.Var.Id == 3 ? std::string("status")
                                   : "in[" + std::to_string(V.Var.Id) + "]";
          },
          [](const MedVar &V) { return "out[" + std::to_string(V.Id) + "]"; },
          [](const MedVar &) { return true; });
      High += ";\n}\n";
    }
    ASSERT_FALSE(llvm::verifyModule(M, &llvm::errs()));
    // Use the production target's complete ABI layout before projecting C.
    // A pointer-width-only layout gives i64 aggregates the wrong tail padding.
    auto NativeModule = llvm::CloneModule(M);
    auto Object = Codegen().compile(*NativeModule, A, BinaryFormat::ELF);
    ASSERT_TRUE(Object.Success);
    ASSERT_FALSE(Object.ObjectData.empty());
    M.setDataLayout(NativeModule->getDataLayout());
    std::error_code Error;
    const auto Stem =
        std::string(Directory) + "/rep-" + (A == Arch::X64 ? "64" : "32");
    llvm::raw_fd_ostream IR(Stem + ".ll", Error);
    ASSERT_FALSE(Error);
    M.print(IR, nullptr);
    llvm::raw_fd_ostream HighOS(Stem + "-high.c", Error);
    ASSERT_FALSE(Error);
    HighOS << High;
    llvm::raw_fd_ostream LLVMOS(Stem + "-llvm.c", Error);
    ASSERT_FALSE(Error);
    CEmitterOptions Options;
    Options.TheArch = A;
    Options.Format = BinaryFormat::ELF;
    ASSERT_TRUE(LLVMCEmitter().emit(M, LLVMOS, Options));
    llvm::raw_fd_ostream ObjectOS(Stem + "-object.o", Error);
    ASSERT_FALSE(Error);
    ObjectOS.write(reinterpret_cast<const char *>(Object.ObjectData.data()),
                   Object.ObjectData.size());
  }
}

} // namespace

TEST(X86RepState, ImageAccessSpanRequiresEveryByteInsideTheMapping) {
  for (unsigned Width : {1, 2, 4, 8})
    for (bool Backward : {false, true}) {
      const X86RepControl Control{Width, 8, 0, false};
      const uint64_t Address = 0x2000 + (Backward ? 24 - Width : 0);
      auto Span =
          x86RepAccessSpan(Address, 24 / Width, Control, Backward, 0x2000, 24);
      ASSERT_TRUE(Span);
      EXPECT_EQ(Span->Begin, 0x2000u);
      EXPECT_EQ(Span->Bytes, 24u);
      EXPECT_FALSE(x86RepAccessSpan(Address, 24 / Width + 1, Control, Backward,
                                    0x2000, 24));
      EXPECT_FALSE(x86RepAccessSpan(Address, 24 / Width, Control, Backward,
                                    Backward ? 0x2001 : 0x2000, 23));
      EXPECT_FALSE(
          x86RepAccessSpan(Address, UINT64_MAX, Control, Backward, 0x2000, 24));
      EXPECT_FALSE(x86RepAccessSpan(Address, 0, Control, Backward, 0x2000, 24));
    }
}

TEST(X86RepState, ZeroCountAndOffsetFormsKeepTheirNumericInputs) {
  constexpr va_t CodeVA = 0x140001000, DataVA = 0x140002000;
  for (auto Id : {Intrinsic::X86RepMovsState, Intrinsic::X86RepStosState})
    for (unsigned Mode : {0, 1, 2}) {
      if (Mode == 2 && Id == Intrinsic::X86RepStosState)
        continue;
      SCOPED_TRACE(static_cast<unsigned>(Id));
      SCOPED_TRACE(Mode);
      BinaryImage Image;
      Image.Arch = Arch::X64;
      Image.Bits = Bitness::Bits64;
      Image.Format = BinaryFormat::ELF;
      Image.Entry = Image.Base = CodeVA;
      Segment Data;
      Data.VA = DataVA;
      Data.Size = Data.FileSz = 40;
      Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      Data.Data.resize(40);
      for (unsigned I = 0; I < 8; ++I)
        Data.Data[32 + I] = static_cast<uint8_t>(CodeVA >> (8 * I));
      Image.Segments.push_back(std::move(Data));
      Image.CodePtrRelocSlots.insert(DataVA + 32);
      Image.CodeRefTargets.insert(CodeVA);
      MedFunc Function;
      Function.Entry = CodeVA;
      Function.Name = "rep_address_bits";
      Function.CC = CallingConv::SysV_AMD64;
      Function.ReturnType = NdType::makeInt(8, false);
      MedOp Op;
      Op.Opcode = NdOp::INTRINSIC;
      Op.Output = temporary(10, 8);
      Op.addInput(MedVar::makeConst(static_cast<unsigned>(Id), 2));
      Op.addInput(
          MedVar::makeConst(DataVA, 8, ConstantAddressProvenance::Address));
      const bool Copy = Id == Intrinsic::X86RepMovsState;
      const uint64_t Count = Mode == 0 ? 0 : 3;
      Op.addInput(MedVar::makeConst(Copy ? DataVA + 8 : Count, 8,
                                    Copy ? ConstantAddressProvenance::Address
                                         : ConstantAddressProvenance::Scalar));
      Op.addInput(MedVar::makeConst(Copy ? Count : DataVA + 8, 8,
                                    ConstantAddressProvenance::Scalar));
      Op.addInput(MedVar::makeConst(0, 4));
      Op.addInput(MedVar::makeConst(
          *x86RepDescriptor(Id, Arch::X64, 8, Mode == 1 ? 4 : 8,
                            Mode == 2 ? 0x65 : 0, false),
          4));
      for (unsigned I = 0; I < x86RepContract(Id)->auxiliaryOutputs(); ++I)
        Op.IntrinsicOutputs.push_back(temporary(11 + I, 8));
      MedOp Return;
      Return.Opcode = NdOp::RETURN;
      Return.addInput(Op.Output);
      MedBlock Block;
      Block.Id = 0;
      Block.Ops = {Op, Return};
      Function.Blocks.push_back(std::move(Block));
      llvm::LLVMContext Context;
      auto Module =
          MedLLVMEmitter().emit({Function}, Context, "rep-inputs", Arch::X64,
                                {}, &Image, BinaryFormat::ELF);
      ASSERT_TRUE(Module);
      ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
      unsigned Calls = 0;
      for (const auto &F : *Module)
        for (const auto &B : F)
          for (const auto &I : B)
            if (const auto *Call = llvm::dyn_cast<llvm::CallInst>(&I)) {
              ++Calls;
              for (unsigned J = 0; J < 3; ++J) {
                const auto *Value =
                    llvm::dyn_cast<llvm::ConstantInt>(Call->getArgOperand(J));
                ASSERT_TRUE(Value);
                EXPECT_EQ(Value->getZExtValue(), Op.Inputs[J + 1].ConstVal);
              }
            }
      EXPECT_EQ(Calls, 1u);
#if defined(__x86_64__) || defined(_M_X64)
      // Only the zero-count native-width form is safe to execute on the host.
      if (Mode == 0) {
        std::string Source;
        llvm::raw_string_ostream Out(Source);
        CEmitterOptions Options;
        Options.TheArch = Arch::X64;
        Options.Format = BinaryFormat::ELF;
        ASSERT_TRUE(LLVMCEmitter().emit(*Module, Out, Options));
        Out.flush();
        Source += "\nint main(void) { return rep_address_bits() != " +
                  std::to_string(DataVA) + "ULL; }\n";
        for (const auto Level : {"-O0", "-O2"})
          source_call_execution_test::compileAndRun(
              Source, {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
      }
#endif
    }
}

TEST(X86RepState, ConstantAddressFactsExcludeUnprovedAndNonAddressResults) {
  for (auto A : {Arch::X86, Arch::X64}) {
    const unsigned Word = A == Arch::X64 ? 8 : 4;
    for (auto Id : {Intrinsic::X86RepMovsState, Intrinsic::X86RepStosState}) {
      const bool Copy = Id == Intrinsic::X86RepMovsState;
      MedOp Op;
      Op.Opcode = NdOp::INTRINSIC;
      Op.Output = temporary(10, Word);
      Op.addInput(MedVar::makeConst(static_cast<unsigned>(Id), 2));
      Op.addInput(MedVar::makeConst(0x1000, Word));
      Op.addInput(MedVar::makeConst(Copy ? 0x2000 : 3, Word));
      Op.addInput(MedVar::makeConst(Copy ? 3 : 0x55, Word));
      Op.addInput(MedVar::makeConst(0, 4));
      Op.addInput(
          MedVar::makeConst(*x86RepDescriptor(Id, A, 4, Word, 0, false), 4));
      for (unsigned I = 0; I < x86RepContract(Id)->auxiliaryOutputs(); ++I)
        Op.IntrinsicOutputs.push_back(temporary(11 + I, Word));
      for (bool Backward : {false, true}) {
        Op.Inputs[4].ConstVal = uint64_t(Backward) << X86RepDirectionShift;
        EXPECT_EQ(x86RepConstantAddressOutput(Op, Op.Output, A),
                  Backward ? 0xff4u : 0x100cu);
        if (Copy)
          EXPECT_EQ(
              x86RepConstantAddressOutput(Op, Op.IntrinsicOutputs.front(), A),
              Backward ? 0x1ff4u : 0x200cu);
        EXPECT_FALSE(
            x86RepConstantAddressOutput(Op, Op.IntrinsicOutputs.back(), A));
        EXPECT_FALSE(x86RepConstantAddressOutput(Op, temporary(99, Word), A));
      }
      auto Dynamic = Op;
      Dynamic.Inputs[Copy ? 3 : 2] = temporary(20, Word);
      EXPECT_FALSE(x86RepConstantAddressOutput(Dynamic, Op.Output, A));
      Dynamic = Op;
      Dynamic.Inputs[4] = temporary(21, 4);
      EXPECT_FALSE(x86RepConstantAddressOutput(Dynamic, Op.Output, A));
      Dynamic = Op;
      Dynamic.Inputs[1] = temporary(22, Word);
      EXPECT_FALSE(x86RepConstantAddressOutput(Dynamic, Op.Output, A));
      auto Reduced = Op;
      Reduced.Inputs[5].ConstVal =
          *x86RepDescriptor(Id, A, 4, Word / 2, 0, false);
      EXPECT_FALSE(x86RepConstantAddressOutput(Reduced, Op.Output, A));
      if (Copy) {
        auto Segmented = Op;
        Segmented.Inputs[5].ConstVal =
            *x86RepDescriptor(Id, A, 4, Word, 0x65, false);
        EXPECT_FALSE(x86RepConstantAddressOutput(Segmented, Op.Output, A));
      }
      auto Malformed = Op;
      Malformed.IntrinsicOutputs.clear();
      EXPECT_FALSE(x86RepConstantAddressOutput(Malformed, Op.Output, A));
    }
  }
}

TEST(X86RepState, ImageCopiesRetainEveryAccessedByte) {
#if !defined(__x86_64__) && !defined(_M_X64)
  GTEST_SKIP() << "executing x64 REP source requires an x64 host";
#endif
  constexpr va_t CodeVA = 0x140001000, DataVA = 0x140002000;
  for (auto Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
    for (bool PointerMirror : {false, true})
      for (bool ImageDestination : {false, true})
        for (bool Fill : {false, true})
          for (bool Backward : {false, true}) {
            SCOPED_TRACE(static_cast<unsigned>(Format));
            SCOPED_TRACE(Backward);
            SCOPED_TRACE(Fill);
            SCOPED_TRACE(PointerMirror);
            SCOPED_TRACE(ImageDestination);
            BinaryImage Image;
            Image.Arch = Arch::X64;
            Image.Bits = Bitness::Bits64;
            Image.Format = Format;
            Image.Base = Image.Entry = CodeVA;
            Segment Data;
            Data.Name = ".rdata";
            Data.VA = DataVA;
            Data.Flags = SegmentFlags::Readable;
            if (Fill || ImageDestination)
              Data.Flags = Data.Flags | SegmentFlags::Writable;
            Data.Data = {0x41, 0x42, 0,    0x43, 0xff, 0x12, 0x80, 0x26,
                         0x8a, 0xf5, 0,    0x7d, 0x31, 0x99, 0x02, 0xe4,
                         0xee, 0x81, 0x21, 0,    0x43, 0x90, 0xfe, 0x72};
            Data.Size = Data.FileSz = Data.Data.size();
            uint64_t Expected = 0;
            for (unsigned I = 0; I < Data.Data.size(); ++I)
              Expected ^= uint64_t(Data.Data[I]) << (8 * (I % 8));
            uint64_t RevisitedWord = 0;
            for (unsigned I = 0; I < 8; ++I)
              RevisitedWord |= uint64_t(Data.Data[(Backward ? 0 : 16) + I])
                               << (8 * I);
            // An unrelated function pointer in the same data owner must not
            // keep the REP operand at its original unmapped numeric address.
            if (PointerMirror) {
              Data.Data.resize(40);
              for (unsigned I = 0; I < 8; ++I)
                Data.Data[32 + I] = static_cast<uint8_t>(CodeVA >> (8 * I));
              Data.Size = Data.FileSz = Data.Data.size();
              Image.CodePtrRelocSlots.insert(DataVA + 32);
              Image.CodeRefTargets.insert(CodeVA);
            }
            if (ImageDestination) {
              Data.Data.resize(96);
              Data.Size = Data.FileSz = Data.Data.size();
            }
            const uint64_t FillValue =
                PointerMirror ? DataVA + 8 : 0x17365879BADCF05A;
            if (Fill)
              Expected = RevisitedWord = FillValue;
            Segment Text;
            Text.Name = ".text";
            Text.VA = CodeVA;
            Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
            // Preserve the string registers, copy three complete words to a
            // local object and observe every byte independently of source
            // declarations.
            Text.Data = {0x56, 0x57, 0x48, 0x83, 0xec, 0x28};
            if (Fill) {
              Text.Data.insert(Text.Data.end(), {0x48, 0x8d, 0x3d});
              const uint32_t FillDisplacement = DataVA + (Backward ? 16 : 0) -
                                                (CodeVA + Text.Data.size() + 4);
              for (unsigned I = 0; I < 4; ++I)
                Text.Data.push_back(
                    static_cast<uint8_t>(FillDisplacement >> (8 * I)));
              Text.Data.insert(Text.Data.end(), {0x48, 0xb8});
              for (unsigned I = 0; I < 8; ++I)
                Text.Data.push_back(static_cast<uint8_t>(FillValue >> (8 * I)));
              Text.Data.insert(Text.Data.end(),
                               {0xb9, 3, 0, 0, 0,
                                static_cast<uint8_t>(Backward ? 0xfd : 0xfc),
                                0xf3, 0x48, 0xab, 0xfc});
            }
            if (ImageDestination) {
              Text.Data.insert(Text.Data.end(), {0x48, 0x8d, 0x3d});
              const uint32_t DestinationDisplacement =
                  DataVA + 64 + (Backward ? 16 : 0) -
                  (CodeVA + Text.Data.size() + 4);
              for (unsigned I = 0; I < 4; ++I)
                Text.Data.push_back(
                    static_cast<uint8_t>(DestinationDisplacement >> (8 * I)));
            } else {
              Text.Data.insert(Text.Data.end(),
                               {0x48, 0x8d, 0x7c, 0x24,
                                static_cast<uint8_t>(Backward ? 0x18 : 0x08)});
            }
            Text.Data.insert(Text.Data.end(), {0x48, 0x8d, 0x35});
            const uint32_t Displacement =
                DataVA + (Backward ? 16 : 0) - (CodeVA + Text.Data.size() + 4);
            for (unsigned I = 0; I < 4; ++I)
              Text.Data.push_back(
                  static_cast<uint8_t>(Displacement >> (8 * I)));
            Text.Data.insert(Text.Data.end(),
                             {0xb9, 3, 0, 0, 0,
                              static_cast<uint8_t>(Backward ? 0xfd : 0xfc),
                              0xf3, 0x48, 0xa5, 0xfc});
            if (ImageDestination) {
              // Observe the auxiliary destination result through three reads.
              for (unsigned I = 0; I < 3; ++I)
                Text.Data.insert(
                    Text.Data.end(),
                    {0x48, static_cast<uint8_t>(I ? 0x33 : 0x8b), 0x47,
                     static_cast<uint8_t>((Backward ? 8 : -24) + I * 8)});
            } else {
              Text.Data.insert(Text.Data.end(),
                               {0x48, 0x8b, 0x44, 0x24, 0x08, 0x48, 0x33, 0x44,
                                0x24, 0x10, 0x48, 0x33, 0x44, 0x24, 0x18});
            }
            // A subsequent memory use must resolve the completed address too.
            Text.Data.insert(
                Text.Data.end(),
                {0x48, 0x03, 0x46, static_cast<uint8_t>(Backward ? 8 : -8)});
            Expected += RevisitedWord;
            // Observe the completed source register in the original coordinate
            // model as well as every copied byte.
            Text.Data.insert(Text.Data.end(), {0x48, 0x8d, 0x15});
            const uint32_t SourceDisplacement =
                DataVA + (Backward ? 16 : 0) - (CodeVA + Text.Data.size() + 4);
            for (unsigned I = 0; I < 4; ++I)
              Text.Data.push_back(
                  static_cast<uint8_t>(SourceDisplacement >> (8 * I)));
            Text.Data.insert(Text.Data.end(),
                             {0x48, 0x29, 0xd6, 0x48, 0x31, 0xf0, 0x48, 0x83,
                              0xc4, 0x28, 0x5f, 0x5e, 0xc3});
            Expected ^= Backward ? uint64_t(-24) : uint64_t(24);
            Text.Size = Text.FileSz = Text.Data.size();
            auto Symbol = Symbol::makeFunc(CodeVA, Text.Size);
            Symbol.Name = "copy_image_words";
            Image.Symbols.push_back(std::move(Symbol));
            Image.Segments.push_back(std::move(Text));
            Image.Segments.push_back(std::move(Data));
            PipelineOptions Options;
            Options.EmitDumpOutput = false;
            Options.OnlyFunctionEntries = {CodeVA};
            llvm::LLVMContext Context;
            auto Result = Pipeline().run(Image, Context, Options);
            ASSERT_TRUE(Result.Success) << Result.Error;
            auto Module =
                MedLLVMEmitter().emit(Result.MedFuncs, Context, "copy",
                                      Arch::X64, {}, &Image, Format);
            ASSERT_TRUE(Module);
            CEmitterOptions COptions;
            COptions.TheArch = Arch::X64;
            COptions.Format = Format;
            COptions.Image = &Image;
            for (bool LLVMRoute : {false, true}) {
              SCOPED_TRACE(LLVMRoute);
              std::string Source;
              llvm::raw_string_ostream Out(Source);
              ASSERT_TRUE(
                  LLVMRoute
                      ? LLVMCEmitter().emit(*Module, Out, COptions, nullptr,
                                            &Image)
                      : HighCEmitter().emit(Result.HighFuncs, Out, COptions));
              Out.flush();
              const auto Arguments = Format == BinaryFormat::COFF ? "" : "0, 0";
              const auto Oracle =
                  "\nint main(void) { return copy_image_words(" +
                  std::string(Arguments) + ") != " + std::to_string(Expected) +
                  "ULL; }\n";
              for (const auto Level : {"-O0", "-O2"})
                source_call_execution_test::compileAndRun(
                    Source + Oracle,
                    {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
            }
            // A complete image span is required even when its starting address
            // remains mapped. Truncate the far end in either direction.
            auto &Truncated = Image.Segments.back();
            if (Backward) {
              Truncated.VA += 8;
              Truncated.Data.erase(Truncated.Data.begin(),
                                   Truncated.Data.begin() + 8);
            }
            // File padding beyond the mapping is not accessible image storage.
            Truncated.Size = 16;
            Truncated.FileSz = Truncated.Data.size();
            std::string Refused;
            llvm::raw_string_ostream RefusedOut(Refused);
            EXPECT_THROW(
                HighCEmitter().emit(Result.HighFuncs, RefusedOut, COptions),
                std::invalid_argument);
            EXPECT_THROW(MedLLVMEmitter().emit(Result.MedFuncs, Context,
                                               "short", Arch::X64, {}, &Image,
                                               Format),
                         std::invalid_argument);
          }
}
