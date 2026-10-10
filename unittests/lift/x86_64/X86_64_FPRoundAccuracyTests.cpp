//===- X86_64_FPRoundAccuracyTests.cpp - ROUND numerical/state tests
//-------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "X86FPStateAccuracyFixture.h"

#include "neverd/ir/high/X86FPStateShape.h"
#include "neverd/loader/ExceptionInfo.h"

namespace {

LowOp roundStateOp(unsigned Control, unsigned Bytes, uint8_t Immediate,
                   uint32_t State) {
  LowOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.Output = NdVar::tmp(0, Bytes + 4);
  Op.addInput(NdVar::cst(static_cast<unsigned>(Intrinsic::X86FPRoundState), 2));
  Op.addInput(NdVar::cst(Control, 1));
  Op.addInput(NdVar::reg(x86reg::XMM0, Bytes));
  Op.addInput(NdVar::cst(Immediate, 1));
  Op.addInput(NdVar::cst(State, 4));
  return Op;
}

LowOp roundMemoryStateOp(
    unsigned Control, unsigned Bytes, uint64_t Address, uint32_t State,
    NdMemoryAddressSpace Space = NdMemoryAddressSpace::Default) {
  LowOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.Output = NdVar::tmp(0, Bytes + 4);
  Op.MemoryAddressSpace = Space;
  Op.addInput(NdVar::cst(unsigned(Intrinsic::X86FPRoundMemoryState), 2));
  Op.addInput(NdVar::cst(Address, 8));
  Op.addInput(NdVar::cst(Control, 1));
  Op.addInput(NdVar::cst(4, 1));
  Op.addInput(NdVar::cst(State, 4));
  return Op;
}

void addRoundSource(BinaryImage &Image, uint64_t Address, unsigned Size = 32) {
  Segment Memory;
  Memory.VA = Address;
  Memory.Size = Memory.FileSz = Size;
  Memory.Flags = SegmentFlags::Readable;
  Memory.Data.resize(Size, 0);
  Image.Segments.push_back(Memory);
}

#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
template <unsigned Control, unsigned Immediate>
__attribute__((target("sse4.1"))) std::pair<std::vector<uint8_t>, uint32_t>
nativeRound(const std::vector<uint8_t> &Source, uint32_t State) {
  const uint32_t Saved = _mm_getcsr();
  std::vector<uint8_t> Result(Source.size());
  if constexpr (Control == 4) {
    float Input, Output;
    std::memcpy(&Input, Source.data(), 4);
    __asm__ volatile("ldmxcsr %1\n\troundss %3,%2,%0\n\tstmxcsr %1"
                     : "=&x"(Output), "+m"(State)
                     : "x"(Input), "i"(Immediate)
                     : "memory");
    std::memcpy(Result.data(), &Output, 4);
  } else if constexpr (Control == 6) {
    double Input, Output;
    std::memcpy(&Input, Source.data(), 8);
    __asm__ volatile("ldmxcsr %1\n\troundsd %3,%2,%0\n\tstmxcsr %1"
                     : "=&x"(Output), "+m"(State)
                     : "x"(Input), "i"(Immediate)
                     : "memory");
    std::memcpy(Result.data(), &Output, 8);
  } else if constexpr (Control == 0) {
    using Vector = float __attribute__((vector_size(16)));
    Vector Input, Output;
    std::memcpy(&Input, Source.data(), 16);
    __asm__ volatile("ldmxcsr %1\n\troundps %3,%2,%0\n\tstmxcsr %1"
                     : "=&x"(Output), "+m"(State)
                     : "x"(Input), "i"(Immediate)
                     : "memory");
    std::memcpy(Result.data(), &Output, 16);
  } else {
    using Vector = double __attribute__((vector_size(16)));
    Vector Input, Output;
    std::memcpy(&Input, Source.data(), 16);
    __asm__ volatile("ldmxcsr %1\n\troundpd %3,%2,%0\n\tstmxcsr %1"
                     : "=&x"(Output), "+m"(State)
                     : "x"(Input), "i"(Immediate)
                     : "memory");
    std::memcpy(Result.data(), &Output, 16);
  }
  _mm_setcsr(Saved);
  return {Result, State};
}

template <unsigned Control, unsigned Immediate> void checkConcreteRound() {
  constexpr unsigned Element = x86FPRoundStateElementBytes(Control);
  constexpr unsigned Bytes = x86FPRoundStateIsScalar(Control) ? Element : 16;
  const std::vector<uint64_t> Values =
      Element == 8 ? std::vector<uint64_t>{0,
                                           UINT64_C(0x8000000000000000),
                                           UINT64_C(0x3ff4000000000000),
                                           UINT64_C(0xbff4000000000000),
                                           UINT64_C(0x4004000000000000),
                                           UINT64_C(0x400c000000000000),
                                           1,
                                           UINT64_C(0x000fffffffffffff),
                                           UINT64_C(0x7fefffffffffffff),
                                           UINT64_C(0x7ff0000000000000),
                                           UINT64_C(0xfff0000000000000),
                                           UINT64_C(0x7ff8000000000031),
                                           UINT64_C(0xfff0000000000071)}
                   : std::vector<uint64_t>{0,          0x80000000, 0x3fa00000,
                                           0xbfa00000, 0x40200000, 0x40600000,
                                           1,          0x007fffff, 0x7f7fffff,
                                           0x7f800000, 0xff800000, 0x7fc00031,
                                           0xff800071};
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  for (unsigned RC = 0; RC < 4; ++RC)
    for (unsigned Environment = 0; Environment < 4; ++Environment)
      for (bool Sticky : {false, true})
        for (unsigned Start = 0; Start < Values.size(); ++Start) {
          const uint32_t State =
              0x1f80 | (RC << 13) | ((Environment & 1) ? 0x40 : 0) |
              ((Environment & 2) ? 0x8000 : 0) | (Sticky ? 0x25 : 0);
          std::vector<uint8_t> Source(Bytes);
          for (unsigned Lane = 0; Lane < Bytes / Element; ++Lane)
            std::memcpy(Source.data() + Lane * Element,
                        &Values[(Start + Lane * 3) % Values.size()], Element);
          const auto Expected = nativeRound<Control, Immediate>(Source, State);
          for (uint8_t Imm : {uint8_t(Immediate), uint8_t(Immediate | 0xe0)}) {
            SCOPED_TRACE(testing::Message()
                         << "control=" << Control
                         << " immediate=" << unsigned(Imm) << " state=" << State
                         << " start=" << Start);
            NdOpEmulator Emulator(Image);
            Emulator.setStrictMode(true);
            Emulator.setRegisterBytes(x86reg::XMM0, Source);
            const auto Op = roundStateOp(Control, Bytes, Imm, State);
            ASSERT_TRUE(Emulator.step(Op));
            const auto Actual = Emulator.getRegisterBytes(0);
            ASSERT_TRUE(Actual);
            ASSERT_EQ(Actual->size(), Bytes + 4);
            EXPECT_EQ(
                std::vector<uint8_t>(Actual->begin(), Actual->begin() + Bytes),
                Expected.first);
            uint32_t Outgoing = 0;
            std::memcpy(&Outgoing, Actual->data() + Bytes, 4);
            EXPECT_EQ(Outgoing, Expected.second);
          }
        }
}

template <unsigned Immediate> void checkConcreteRoundImmediate() {
  checkConcreteRound<0, Immediate>();
  checkConcreteRound<2, Immediate>();
  checkConcreteRound<4, Immediate>();
  checkConcreteRound<6, Immediate>();
}
#endif

TEST(X86FPRoundContract, ConcreteNumericalAndStateMatchNativeAllControls) {
#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
  if (!llvm::sys::getHostCPUFeatures().lookup("sse4.1"))
    GTEST_SKIP() << "native SSE4.1 oracle required";
  checkConcreteRoundImmediate<0>();
  checkConcreteRoundImmediate<1>();
  checkConcreteRoundImmediate<2>();
  checkConcreteRoundImmediate<3>();
  checkConcreteRoundImmediate<4>();
  checkConcreteRoundImmediate<5>();
  checkConcreteRoundImmediate<6>();
  checkConcreteRoundImmediate<7>();
  checkConcreteRoundImmediate<8>();
  checkConcreteRoundImmediate<9>();
  checkConcreteRoundImmediate<10>();
  checkConcreteRoundImmediate<11>();
  checkConcreteRoundImmediate<12>();
  checkConcreteRoundImmediate<13>();
  checkConcreteRoundImmediate<14>();
  checkConcreteRoundImmediate<15>();
#else
  GTEST_SKIP() << "native x64 GCC/Clang oracle required";
#endif
}

TEST(X86FPRoundContract, ShapesAndScalarProvenanceRejectMalformedInputs) {
  constexpr auto Id = Intrinsic::X86FPRoundState;
  for (unsigned Control : {0U, 2U, 4U, 6U})
    for (unsigned Bytes : {4U, 8U, 16U, 32U}) {
      const auto Op = roundStateOp(Control, Bytes, 0xff, 0x1f80);
      const auto Shape = x86FPStateLowShape(Op, Arch::X64);
      const bool Scalar = x86FPRoundStateIsScalar(Control);
      const bool Valid =
          Scalar ? Bytes == x86FPRoundStateElementBytes(Control) : Bytes >= 16;
      EXPECT_EQ(x86FPStateShapeIsValid(Id, Shape), Valid);
      if (!Valid)
        continue;
      EXPECT_EQ(x86FPStateNumericalSliceSize(Id, Shape, 0, Bytes),
                Scalar ? Bytes : 0U);
      EXPECT_EQ(x86FPStateNumericalSliceSize(Id, Shape, Bytes, 4), 0U);
      EXPECT_EQ(x86FPStateNumericalSliceSize(Id, Shape, 0, Bytes + 4), 0U);
      for (unsigned Mutation = 0; Mutation < 9; ++Mutation) {
        auto Bad = Shape;
        switch (Mutation) {
        case 0:
          Bad.Control = Control | 1;
          break;
        case 1:
          Bad.Control = Control | 8;
          break;
        case 2:
          Bad.ControlIsConst = false;
          break;
        case 3:
          Bad.ImmediateIsConst = false;
          break;
        case 4:
          Bad.Immediate = 0x100;
          break;
        case 5:
          Bad.StateSize = 8;
          break;
        case 6:
          Bad.OutputSize = Bytes;
          break;
        case 7:
          Bad.HasAuxiliaryOutputs = true;
          break;
        case 8:
          Bad.TargetArch = Arch::AArch64;
          break;
        }
        EXPECT_FALSE(x86FPStateShapeIsValid(Id, Bad));
      }
    }
}

TEST(X86FPRoundContract, UnmaskedExceptionsAndUnknownStatePublishNoResult) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  for (unsigned Bytes : {4U, 8U})
    for (bool Invalid : {false, true})
      for (bool SuppressPrecision : {false, true}) {
        const unsigned Control = Bytes == 8 ? 6 : 4;
        const uint64_t Bits = Invalid
                                  ? (Bytes == 8 ? UINT64_C(0x7ff0000000000071)
                                                : UINT64_C(0x7f800071))
                                  : (Bytes == 8 ? UINT64_C(0x3ff4000000000000)
                                                : UINT64_C(0x3fa00000));
        const uint32_t State = Invalid ? 0x1f00 : 0x0f80;
        NdOpEmulator Emulator(Image);
        Emulator.setStrictMode(true);
        auto Op =
            roundStateOp(Control, Bytes, SuppressPrecision ? 12 : 4, State);
        Op.Inputs[2] = NdVar::cst(Bits, Bytes);
        const bool Complete = !Invalid && SuppressPrecision;
        EXPECT_EQ(Emulator.step(Op), Complete);
        EXPECT_EQ(bool(Emulator.getRegisterBytes(0)), Complete);
        EXPECT_EQ(Emulator.getMXCSR() & 0x3fU, Complete  ? 0U
                                               : Invalid ? 1U
                                                         : 32U);
        Op.Inputs[4] = NdVar::tmp(100, 4);
        NdOpEmulator Unknown(Image);
        Unknown.setStrictMode(true);
        EXPECT_FALSE(Unknown.step(Op));
        EXPECT_FALSE(Unknown.getRegisterBytes(0));
      }
}

TEST(X86FPRoundContract, HighControlsRequireIntegerConstants) {
  auto Call = HighExpr::makeCall(
      "", 0,
      {HighExpr::makeConst(4, 1), HighExpr::makeConst(0x3fa00000, 4),
       HighExpr::makeConst(4, 1), HighExpr::makeConst(0x1f80, 4)});
  Call->IntrinsicId = Intrinsic::X86FPRoundState;
  Call->Type = NdType::makeInt(8, false);
  ASSERT_TRUE(x86FPStateShapeIsValid(Call->IntrinsicId,
                                     x86FPStateHighShape(*Call, Arch::X64)));
  for (unsigned Index : {0U, 2U}) {
    Call->Operands[Index]->Type = NdType::makeFloat(1);
    EXPECT_FALSE(x86FPStateShapeIsValid(Call->IntrinsicId,
                                        x86FPStateHighShape(*Call, Arch::X64)));
    Call->Operands[Index]->Type = NdType::makeInt(1, false);
  }
}

TEST(X86FPRoundContract, PackedFaultPriorityDoesNotDependOnLaneOrder) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  for (unsigned Element : {4U, 8U})
    for (unsigned Bytes : {16U, 32U})
      for (bool InvalidMasked : {false, true})
        for (bool Swap : {false, true}) {
          const uint64_t SignalNaN = Element == 8 ? UINT64_C(0x7ff0000000000071)
                                                  : UINT64_C(0x7f800071);
          const uint64_t Fraction = Element == 8 ? UINT64_C(0x3ff4000000000000)
                                                 : UINT64_C(0x3fa00000);
          std::vector<uint8_t> Source(Bytes, 0);
          std::memcpy(Source.data() + (Swap ? Element : 0), &SignalNaN,
                      Element);
          std::memcpy(Source.data() + (Swap ? 0 : Element), &Fraction, Element);
          NdOpEmulator Emulator(Image);
          Emulator.setStrictMode(true);
          Emulator.setRegisterBytes(x86reg::XMM0, Source);
          const auto Op = roundStateOp(Element == 8 ? 2 : 0, Bytes, 4,
                                       InvalidMasked ? 0x0f80 : 0x1f00);
          EXPECT_FALSE(Emulator.step(Op));
          EXPECT_FALSE(Emulator.getRegisterBytes(0));
          EXPECT_EQ(Emulator.getMXCSR() & 0x3fU, InvalidMasked ? 33U : 1U)
              << "element=" << Element << " bytes=" << Bytes << " swap=" << Swap
              << " invalid masked=" << InvalidMasked;
        }
}

TEST(X86FPRoundContract, RawAddressImmediateAndRegisterDescriptorsAgree) {
  for (Arch Target : {Arch::X86, Arch::X64})
    for (bool Strict : {false, true})
      for (bool Vex : {false, true})
        for (bool Memory : {false, true})
          for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
            Decoder Decode;
            ASSERT_TRUE(Decode.init(Target));
            Decode.setStrict(Strict);
            std::vector<uint8_t> Bytes =
                Vex ? std::vector<uint8_t>{0xc4, 0xe3, 0x69, 0x0a}
                    : std::vector<uint8_t>{0x66, 0x0f, 0x3a, 0x0a};
            Bytes.push_back(Memory ? 0x00 : 0xc1);
            Bytes.push_back(4);
            DecodedInsn Insn;
            ASSERT_EQ(Decode.decodeOne(Bytes.data(), Bytes.size(), Entry, Insn),
                      Bytes.size());
            auto &Detail = Insn.Raw->detail->x86;
            const unsigned Source = Vex ? 2 : 1;
            SCOPED_TRACE(testing::Message()
                         << "target=" << unsigned(Target)
                         << " strict=" << Strict << " vex=" << Vex
                         << " memory=" << Memory << " mutation=" << Mutation);
            if (Mutation == 0) {
              std::vector<LowOp> Ops;
              ASSERT_NO_THROW(Decode.liftToLow(Insn, Ops));
              unsigned Found = 0;
              for (const auto &Op : Ops)
                if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
                    isX86FPRoundStateIntrinsic(
                        static_cast<Intrinsic>(Op.Inputs[0].Offset))) {
                  ++Found;
                  EXPECT_TRUE(x86FPStateShapeIsValid(
                      static_cast<Intrinsic>(Op.Inputs[0].Offset),
                      x86FPStateLowShape(Op, Target)));
                }
              EXPECT_EQ(Found, 1U);
              continue;
            }
            if (Mutation == 1)
              Detail.operands[Detail.op_count - 1].imm = 8;
            else if (Mutation == 2)
              Detail.operands[0].reg = X86_REG_XMM3;
            else if (Mutation == 3)
              Detail.operands[Source].size = 8;
            else if (Mutation == 4)
              Detail.addr_size = 2;
            else {
              if (Memory)
                Detail.operands[Source].mem.base = X86_REG_EBX;
              else {
                Insn.Raw->bytes[Insn.Raw->size++] = 0x90;
                Insn.Size = Insn.Raw->size;
              }
            }
            std::vector<LowOp> Ops;
            EXPECT_THROW(Decode.liftToLow(Insn, Ops), UnliftedInstruction);
            EXPECT_TRUE(Ops.empty());
          }
}

TEST(X86FPRoundContract, MedVerifierAndHighRendererEnforceTheSameShape) {
  for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
    MedOp Op;
    Op.Opcode = NdOp::INTRINSIC;
    Op.Output =
        MedVar{.Kind = MedVar::Temp, .TheArch = Arch::X64, .Id = 1, .Size = 8};
    Op.addInput(MedVar::makeConst(unsigned(Intrinsic::X86FPRoundState), 2));
    Op.addInput(MedVar::makeConst(4, 1));
    Op.addInput(MedVar::makeConst(0x3fa00000, 4));
    Op.addInput(MedVar::makeConst(4, 1));
    Op.addInput(MedVar::makeConst(0x1f80, 4));
    MedFunc Function;
    MedBlock Block;
    Block.Id = 0;
    Block.Ops.push_back(Op);
    Function.Blocks.push_back(Block);
    ASSERT_TRUE(verifyMedFunc(Function, "valid ROUND state"));
    auto &Bad = Function.Blocks[0].Ops[0];
    if (Mutation == 0)
      Bad.IntrinsicOutputs.push_back(
          MedVar{.Kind = MedVar::Temp, .Id = 2, .Size = 4});
    else if (Mutation == 1)
      Bad.Inputs[1] = MedVar::makeConst(4, 4);
    else if (Mutation == 2)
      Bad.Inputs[3] = MedVar::makeConst(4, 4);
    else if (Mutation == 3)
      Bad.MemoryOrdering = NdMemoryOrdering::Acquire;
    else
      Bad.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
    EXPECT_FALSE(verifyMedFunc(Function, "invalid ROUND state"));

    auto Call = HighExpr::makeCall(
        "", 0,
        {HighExpr::makeConst(4, 1), HighExpr::makeConst(0x3fa00000, 4),
         HighExpr::makeConst(4, 1), HighExpr::makeConst(0x1f80, 4)});
    Call->IntrinsicId = Intrinsic::X86FPRoundState;
    Call->Type = NdType::makeInt(8, false);
    if (Mutation == 0)
      Call->IntrinsicOutputs.push_back(
          MedVar{.Kind = MedVar::Temp, .Id = 2, .Size = 4});
    else if (Mutation == 1 || Mutation == 2)
      Call->Operands[Mutation == 1 ? 0 : 2]->Type = NdType::makeFloat(1);
    else if (Mutation == 3)
      Call->MemoryOrdering = NdMemoryOrdering::Acquire;
    else
      Call->MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
    bool HasCIntrinsics = false;
    EXPECT_DEATH(renderX86TypedIntrinsicCall(
                     Arch::X64, *Call,
                     [](const HighExpr &) { return "unused"; }, HasCIntrinsics,
                     true),
                 "invalid x86 FP state C contract");
  }
}

TEST(X86FPRoundContract, OwnedAssemblyRejectsWrongTypesConstraintsAndEffects) {
  for (unsigned Mutation = 0; Mutation < 3; ++Mutation) {
    llvm::LLVMContext Context;
    llvm::Module Module("invalid-round", Context);
    llvm::IRBuilder<> Builder(Context);
    auto *Pointer = Builder.getPtrTy();
    auto *Function = llvm::Function::Create(
        llvm::FunctionType::get(Builder.getFloatTy(), {Pointer}, false),
        llvm::Function::ExternalLinkage, "invalid_round", Module);
    Builder.SetInsertPoint(
        llvm::BasicBlock::Create(Context, "entry", Function));
    auto *Asm = llvm::InlineAsm::get(
        llvm::FunctionType::get(Builder.getFloatTy(),
                                {Builder.getFloatTy(), Pointer}, false),
        x86FPRoundStateAsm(x86FPRoundStateLayout(Mutation == 0 ? 8 : 4,
                                                 Mutation == 0 ? 6 : 4, 4)),
        Mutation == 1 ? "=x,x,r,~{memory}" : X86FPStateRoundConstraints,
        Mutation != 2);
    auto *Call = Builder.CreateCall(
        Asm, {llvm::ConstantFP::get(Builder.getFloatTy(), 1.25),
              Function->getArg(0)});
    Call->setMetadata(X86FPStateAsmMetadata, llvm::MDNode::get(Context, {}));
    Builder.CreateRet(Call);
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    EXPECT_DEATH(LLVMCEmitter().emit(Module, Out, Options),
                 "invalid x86 FP state assembly");
  }
}

TEST(X86FPRoundContract, SegmentedMemoryHelpersSurviveExceptionMetadata) {
  for (auto Space : {NdMemoryAddressSpace::Default, NdMemoryAddressSpace::X86FS,
                     NdMemoryAddressSpace::X86GS})
    for (bool HasExceptions : {false, true})
      for (bool EffectOnly : {false, true}) {
        EXPECT_EXIT(
            {
              auto Call = HighExpr::makeCall(
                  "", 0,
                  {HighExpr::makeConst(0x1000, 8), HighExpr::makeConst(4, 1),
                   HighExpr::makeConst(4, 1), HighExpr::makeConst(0x1f80, 4)});
              Call->IntrinsicId = Intrinsic::X86FPRoundMemoryState;
              Call->MemoryAddressSpace = Space;
              Call->Type = NdType::makeInt(8, false);
              HighFunc Function;
              Function.Name = "segmented_round";
              Function.ReturnType =
                  EffectOnly ? NdType::makeVoid() : Call->Type;
              if (HasExceptions)
                Function.ExceptionMetadata = ExceptionFunction{};
              HighStmt Return;
              Return.Kind = EffectOnly ? StmtKind::Call : StmtKind::Return;
              if (EffectOnly)
                Return.CallExpr = Call;
              else
                Return.RetVal = Call;
              Function.Body.push_back(Return);
              std::string Source;
              llvm::raw_string_ostream Out(Source);
              CEmitterOptions Options;
              Options.TheArch = Arch::X64;
              const bool Emitted =
                  HighCEmitter().emit({Function}, Out, Options);
              const char *Name =
                  Space == NdMemoryAddressSpace::X86FS   ? "memory_fs"
                  : Space == NdMemoryAddressSpace::X86GS ? "memory_gs"
                                                         : "memory";
              const auto Body = Source.rfind("segmented_round(");
              std::exit(Emitted && Body != std::string::npos &&
                                Source.find(Name, Body) != std::string::npos
                            ? 0
                            : 1);
            },
            ::testing::ExitedWithCode(0), "");
      }
}

TEST(X86FPRoundContract, OwnedMemoryAssemblyAuthenticatesBothPointerSpaces) {
  for (unsigned Mutation = 0; Mutation < 3; ++Mutation) {
    llvm::LLVMContext Context;
    llvm::Module Module("invalid-memory-round", Context);
    llvm::IRBuilder<> Builder(Context);
    auto *Address = Builder.getPtrTy(Mutation == 0 ? 0 : 257);
    auto *State = Builder.getPtrTy(Mutation == 1 ? 257 : 0);
    auto *Function = llvm::Function::Create(
        llvm::FunctionType::get(Builder.getFloatTy(), {Address, State}, false),
        llvm::Function::ExternalLinkage, "invalid_round", Module);
    Builder.SetInsertPoint(
        llvm::BasicBlock::Create(Context, "entry", Function));
    auto *Asm = llvm::InlineAsm::get(
        llvm::FunctionType::get(Builder.getFloatTy(), {Address, State}, false),
        x86FPRoundMemoryStateAsm(
            x86FPRoundStateLayout(4, 4, 4, NdMemoryAddressSpace::X86FS)),
        Mutation == 2 ? "=x,r,r,~{memory}" : X86FPStateRoundMemoryConstraints,
        true);
    auto *Call =
        Builder.CreateCall(Asm, {Function->getArg(0), Function->getArg(1)});
    Call->setMetadata(X86FPStateAsmMetadata, llvm::MDNode::get(Context, {}));
    Builder.CreateRet(Call);
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    EXPECT_DEATH(LLVMCEmitter().emit(Module, Out, Options),
                 "invalid x86 FP state assembly");
  }
}

class X86FPRoundAccuracy : public X86FPStateFixture {
protected:
  unsigned SourceBytes = 4;
  bool PrecisionFault = false;
  bool MixedFault = false;
  bool SwapFaultLanes = false;
  bool SeedPrecision = false;
  bool AlignmentFault = false;
  bool PageFault = false;
  bool CrossPage = false;
  bool UnalignedSource = false;
  NdMemoryAddressSpace SourceSegment = NdMemoryAddressSpace::Default;
  unsigned OutputBytes = 16;

  void SetUp() override {
    X86FPStateFixture::SetUp();
    if (!nativeX64() || !llvm::sys::getHostCPUFeatures().lookup("sse4.1"))
      GTEST_SKIP() << "native x64 SSE4.1 ROUND oracle required";
  }

  static bool hasAVX() { return llvm::sys::getHostCPUFeatures().lookup("avx"); }

  std::string comparisonDriver(bool IsDouble, bool DeclareProbe, bool Fault,
                               bool NativeCall = false) override {
    if (Fault) {
      auto Text = faultDriver(IsDouble, DeclareProbe, NativeCall);
      const auto Replace = [&](const std::string &Old, const std::string &New) {
        const auto Position = Text.find(Old);
        if (Position != std::string::npos)
          Text.replace(Position, Old.size(), New);
      };
      const unsigned FlagsValue = (AlignmentFault || PageFault ? 0U
                                   : PrecisionFault ? (MixedFault ? 33U : 32U)
                                                    : 1U) |
                                  (SeedPrecision ? 32U : 0U);
      const std::string Flags = std::to_string(FlagsValue);
      Replace("#include <stdint.h>", "#include <stdint.h>\n#include <stdio.h>");
      Replace("static unsigned char actual[80]",
              "static _Alignas(64) unsigned char actual[80]");
      Replace("#include <unistd.h>",
              "#include <unistd.h>\n#include <ucontext.h>");
      Replace("i < 64", "i < " + std::to_string(48 + OutputBytes));
      Replace("EXCEPTION_FLT_DIVIDE_BY_ZERO",
              AlignmentFault || PageFault ? "EXCEPTION_ACCESS_VIOLATION"
              : PrecisionFault            ? "EXCEPTION_FLT_INEXACT_RESULT"
                                          : "EXCEPTION_FLT_INVALID_OPERATION");
      Replace("numerical_output_untouched() ? 0 : 1",
              "numerical_output_untouched() && "
              "(info->ContextRecord->MxCsr & 0x3f) == " +
                  Flags + " ? 0 : 1");
      Replace(
          "static void on_fault(int signal_number) {\n"
          "  _exit(signal_number == SIGFPE && numerical_output_untouched() "
          "? 0 : 1);\n}",
          "static void on_fault(int signal_number, siginfo_t *info, "
          "void *context) {\n"
          "  ucontext_t *saved = (ucontext_t *)context;\n"
          "#if defined(__APPLE__)\n"
          "  uint32_t trapped_state = saved->uc_mcontext->__fs.__fpu_mxcsr;\n"
          "#else\n"
          "  uint32_t trapped_state = saved->uc_mcontext.fpregs->mxcsr;\n"
          "#endif\n"
          "  _exit(signal_number == SIGFPE && info->si_code == " +
              std::string(PrecisionFault ? "FPE_FLTRES" : "FPE_FLTINV") +
              " && numerical_output_untouched() && "
              "(trapped_state & 0x3f) == " +
              Flags + " ? 0 : 1);\n}");
      Replace(
          "if (signal(SIGFPE, on_fault) == SIG_ERR) return 77;",
          "struct sigaction action; memset(&action, 0, sizeof(action));\n"
          "  action.sa_sigaction = on_fault; action.sa_flags = SA_SIGINFO;\n"
          "  sigemptyset(&action.sa_mask);\n"
          "  if (sigaction(SIGFPE, &action, 0)) return 77;");
      const std::string Fraction =
          IsDouble ? "UINT64_C(0x3ff4000000000000)" : "0x3fa00000";
      const std::string SignalNaN =
          IsDouble ? "UINT64_C(0x7ff0000000000071)" : "0x7f800071";
      Replace("zero = 0;",
              "zero = " +
                  (PrecisionFault && !MixedFault ? Fraction : SignalNaN) + ";");
      Replace(
          "memcpy(actual + 16, &zero, sizeof(zero));",
          "memset(actual + 16, 0, " + std::to_string(SourceBytes) +
              ");\n"
              "  memcpy(actual + 16, &zero, sizeof(zero));" +
              (MixedFault
                   ? "\n  " + std::string(IsDouble ? "uint64_t" : "uint32_t") +
                         " fraction = " + Fraction +
                         ";\n"
                         "  memcpy(actual + 16 + sizeof(zero), &fraction, "
                         "sizeof(fraction));" +
                         (SwapFaultLanes ? "\n  memcpy(actual + 16, &fraction, "
                                           "sizeof(fraction));\n"
                                           "  memcpy(actual + 16 + "
                                           "sizeof(zero), &zero, sizeof(zero));"
                                         : "")
                   : ""));
      Replace("0x1d80", PrecisionFault ? (SeedPrecision ? "0x0fa0" : "0x0f80")
                                       : (SeedPrecision ? "0x1f20" : "0x1f00"));
      if (AlignmentFault || PageFault) {
        Replace("0x1f00", "0x1f80");
        Replace("signal_number == SIGFPE && info->si_code == FPE_FLTINV",
                "signal_number == SIGSEGV");
        Replace("sigaction(SIGFPE, &action, 0)",
                "sigaction(SIGSEGV, &action, 0)");
        if (AlignmentFault)
          Replace("memcpy(actual + 16, &zero, sizeof(zero))",
                  "memcpy(actual + 17, &zero, sizeof(zero))");
      }
      if (PageFault) {
        Replace("static _Alignas(64) unsigned char actual[80];", R"(
#if !defined(_WIN32)
#include <sys/mman.h>
#endif
static unsigned char *actual, *guard_page;
static size_t page_size;
static int protect_source(int protect) {
#if defined(_WIN32)
  DWORD old; return !VirtualProtect(guard_page, page_size,
      protect ? PAGE_NOACCESS : PAGE_READWRITE, &old);
#else
  return mprotect(guard_page, page_size, protect ? PROT_NONE : PROT_READ | PROT_WRITE);
#endif
}


)");
        Replace("static int numerical_output_untouched(void) {",
                "static int numerical_output_untouched(void) {\n"
                "  if (protect_source(0)) return 0;");
        Replace("int main(void) {", R"(
int main(void) {
#if defined(_WIN32)
  SYSTEM_INFO system; GetSystemInfo(&system); page_size = system.dwPageSize;
  unsigned char *mapping = VirtualAlloc(0, page_size * 2,
      MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
  page_size = (size_t)sysconf(_SC_PAGESIZE);
  unsigned char *mapping = mmap(0, page_size * 2, PROT_READ | PROT_WRITE,
      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mapping == MAP_FAILED) return 77;
#endif
  if (!mapping) return 77;
  guard_page = mapping + page_size;
)");
        Replace("for (unsigned i = 0; i < sizeof(actual); ++i)",
                "actual = guard_page - " + std::to_string(CrossPage ? 18 : 16) +
                    ";\n  for (unsigned i = 0; i < 80; ++i)");
        Replace("_mm_setcsr(0x1f80); /* divide-by-zero unmasked */",
                "if (protect_source(1)) return 77;\n"
                "  _mm_setcsr(0x1f80);");
      }
      return Text;
    }
    auto Text = floatingDriver(IsDouble, DeclareProbe);
    const std::string Needle =
        IsDouble ? "UINT64_C(0x4000000000000000)" : "0x40000000";
    const std::string Extra =
        IsDouble
            ? ", UINT64_C(0x3ff4000000000000), UINT64_C(0xbff4000000000000),"
              " UINT64_C(0x4004000000000000), UINT64_C(0x400c000000000000),"
              " UINT64_C(0x3fe0000000000000), UINT64_C(0xbfe0000000000000)"
            : ", 0x3fa00000, 0xbfa00000, 0x40200000, 0x40600000,"
              " 0x3f000000, 0xbf000000";
    const auto Position = Text.find(Needle);
    if (Position != std::string::npos)
      Text.insert(Position + Needle.size(), Extra);
    const std::string Copy =
        "memcpy(expected + 16, &values[b], sizeof(values[0]));";
    const auto CopyPosition = Text.find(Copy);
    if (CopyPosition != std::string::npos)
      Text.replace(
          CopyPosition, Copy.size(),
          "for (unsigned lane = 0; lane < " +
              std::to_string(SourceBytes / (IsDouble ? 8 : 4)) +
              "; ++lane) memcpy(expected + 16 + lane * sizeof(values[0]),"
              " &values[(b + lane * 3) % (sizeof(values)/sizeof(values[0]))],"
              " sizeof(values[0]));");
    if (UnalignedSource) {
      const std::string Needle = "expected + 16 + lane";
      const auto Position = Text.find(Needle);
      if (Position != std::string::npos)
        Text.replace(Position, Needle.size(), "expected + 17 + lane");
    }
    if (SourceSegment == NdMemoryAddressSpace::X86GS) {
#if defined(__linux__)
      const auto Replace = [&](const std::string &Old, const std::string &New) {
        const auto Position = Text.find(Old);
        if (Position != std::string::npos)
          Text.replace(Position, Old.size(), New);
      };
      Replace("#include <stdint.h>",
              "#include <stdint.h>\n#include <unistd.h>\n"
              "#include <sys/syscall.h>\n#include <asm/prctl.h>");
      Replace("unsigned char expected[80], actual[80];",
              "_Alignas(64) unsigned char expected[80], actual[80];\n"
              "            unsigned long saved_gs;\n"
              "            if (syscall(SYS_arch_prctl, ARCH_GET_GS, "
              "&saved_gs)) return 77;");
      Replace("native_probe((uintptr_t)expected);",
              "if (syscall(SYS_arch_prctl, ARCH_SET_GS, expected)) return 77;\n"
              "            native_probe((uintptr_t)expected);");
      Replace("isa_probe((uintptr_t)actual);",
              "if (syscall(SYS_arch_prctl, ARCH_SET_GS, actual)) return 77;\n"
              "            isa_probe((uintptr_t)actual);");
      Replace("uint32_t actual_state = _mm_getcsr();",
              "uint32_t actual_state = _mm_getcsr();\n"
              "            if (syscall(SYS_arch_prctl, ARCH_SET_GS, saved_gs)) "
              "return 77;");
#endif
    }
    return Text;
  }

  void check(bool IsDouble, bool Scalar, bool Vex, unsigned Width,
             uint8_t Immediate, bool Memory = false, bool KeepResult = true,
             bool Fault = false) {
    if (Vex && !hasAVX())
      GTEST_SKIP() << "native AVX and OS vector state required";
    SourceBytes = Scalar ? (IsDouble ? 8 : 4) : Width;
    OutputBytes = Vex ? 32 : 16;
    const auto Arg = memoryModRM();
    const uint8_t Opcode = (Scalar ? 0x0a : 0x08) + (IsDouble ? 1 : 0);
    std::vector<uint8_t> Bytes;
    if (Vex && !PageFault)
      Bytes = {0xc5, 0xfe, 0x6f, Arg}; // Seed all YMM0 bits from the input.
    else
      Bytes = {0xf3, 0x0f, 0x6f, Arg};
    if (!Memory) {
      if (Width == 32)
        Bytes.insert(Bytes.end(),
                     {0xc5, 0xfe, 0x6f, static_cast<uint8_t>(0x48 | Arg), 16});
      else
        Bytes.insert(Bytes.end(),
                     {0xf3, 0x0f, 0x6f, static_cast<uint8_t>(0x48 | Arg), 16});
    }
    if (Vex && Scalar) {
      // Different destination and upper-lane source expose src1 mistakes.
      Bytes.insert(Bytes.end(),
                   {0xf3, 0x0f, 0x6f, static_cast<uint8_t>(0x10 | Arg)});
    }
    if (SourceSegment != NdMemoryAddressSpace::Default)
      Bytes.push_back(SourceSegment == NdMemoryAddressSpace::X86FS ? 0x64
                                                                   : 0x65);
    if (Vex)
      Bytes.insert(Bytes.end(), {0xc4, 0xe3,
                                 static_cast<uint8_t>(Scalar        ? 0x69
                                                      : Width == 32 ? 0x7d
                                                                    : 0x79),
                                 Opcode});
    else
      Bytes.insert(Bytes.end(), {0x66, 0x0f, 0x3a, Opcode});
    if (Memory && SourceSegment != NdMemoryAddressSpace::Default) {
      // Absolute segment offsets avoid adding the host base in an earlier EA.
#if defined(_WIN32)
      constexpr uint8_t Offset = 0x30; // GS TEB self pointer.
#else
      const uint8_t Offset =
          SourceSegment == NdMemoryAddressSpace::X86FS ? 0 : 16;
#endif
      Bytes.insert(Bytes.end(), {0x04, 0x25, Offset, 0, 0, 0});
    } else if (Memory)
      Bytes.insert(Bytes.end(),
                   {static_cast<uint8_t>(0x40 | Arg),
                    uint8_t(AlignmentFault || UnalignedSource ? 17 : 16)});
    else
      Bytes.push_back(0xc1);
    Bytes.push_back(Immediate);
    if (KeepResult) {
      if (Vex)
        Bytes.insert(Bytes.end(),
                     {0xc5, 0xfe, 0x7f, static_cast<uint8_t>(0x40 | Arg), 48});
      else
        Bytes.insert(Bytes.end(),
                     {0xf3, 0x0f, 0x7f, static_cast<uint8_t>(0x40 | Arg), 48});
    }
    Bytes.insert(Bytes.end(), {0x31, 0xc0, 0xc3});
    SCOPED_TRACE(testing::Message()
                 << "f64=" << IsDouble << " scalar=" << Scalar << " vex=" << Vex
                 << " width=" << Width << " immediate=" << unsigned(Immediate)
                 << " memory=" << Memory << " result=" << KeepResult);
    compareFloating(Bytes, IsDouble, Fault);
  }
};

TEST_F(X86FPRoundAccuracy, LLVMCWideStateTransportPreservesAllBits) {
  llvm::LLVMContext Context;
  llvm::Module Module("wide-state", Context);
  llvm::IRBuilder<> Builder(Context);
  auto *Pointer = Builder.getPtrTy();
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(Builder.getVoidTy(), {Pointer, Pointer}, false),
      llvm::Function::ExternalLinkage, "wide_transport", Module);
  Builder.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Function));
  auto *Source =
      Builder.CreateLoad(Builder.getIntNTy(256), Function->getArg(0));
  auto *Wide = Builder.CreateZExt(Source, Builder.getIntNTy(512));
  llvm::APInt Constant(512, 0);
  Constant.setBit(400);
  auto *Combined =
      Builder.CreateOr(Wide, llvm::ConstantInt::get(Context, Constant));
  Builder.CreateStore(Combined, Function->getArg(1));
  auto *Second = Builder.CreateGEP(Builder.getInt8Ty(), Function->getArg(1),
                                   Builder.getInt64(64));
  Builder.CreateStore(Builder.CreateSExt(Source, Builder.getIntNTy(512)),
                      Second);
  auto *Third = Builder.CreateGEP(Builder.getInt8Ty(), Function->getArg(1),
                                  Builder.getInt64(128));
  auto *Narrow = Builder.CreateTrunc(Source, Builder.getIntNTy(160));
  Builder.CreateStore(Builder.CreateZExt(Narrow, Builder.getIntNTy(288)),
                      Third);
  Builder.CreateRetVoid();
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.Format = hostFormat();
  std::string SourceText;
  llvm::raw_string_ostream Out(SourceText);
  ASSERT_TRUE(LLVMCEmitter().emit(Module, Out, Options));
  executeSource(SourceText, R"(
#include <string.h>
int main(void) {
  _Alignas(64) unsigned char input[32], output[164], expected[164] = {0};
  for (unsigned i = 0; i < 32; ++i) input[i] = (unsigned char)(0x80 + i * 5);
  input[31] |= 0x80;
  memcpy(expected, input, 32); expected[50] = 1;
  memcpy(expected + 64, input, 32); memset(expected + 96, 0xff, 32);
  memcpy(expected + 128, input, 20);
  memset(output, 0x59, sizeof(output));
  wide_transport(input, output);
  return memcmp(output, expected, sizeof(output)) != 0;
}
)");
}

TEST_F(X86FPRoundAccuracy, ScalarReturnBitsAndMxcsrKeepTheirCallingConvention) {
  for (bool IsDouble : {false, true})
    for (uint8_t Immediate : {4, 12}) {
      const std::string Scalar = IsDouble ? "double" : "float";
      const std::vector<uint8_t> Bytes = {
          0x66, 0x0f,      0x3a, uint8_t(IsDouble ? 0x0b : 0x0a),
          0xc0, Immediate, 0xc3};
      auto Text = assembly(Bytes);
      for (size_t Position = 0;
           (Position = Text.find("isa_probe", Position)) != std::string::npos;)
        Text.replace(Position, 9, "native_probe");
      const auto Native = file("return-native.s");
      const auto Object = file("return-native.o");
      write(Native, Text);
      auto Built = command(Compiler, {"-c", Native, "-o", Object});
      ASSERT_EQ(Built.Status, 0) << Built.Error;
      std::string Driver = "\n#include <stdint.h>\n#include <string.h>\n"
                           "#include <immintrin.h>\nextern " +
                           Scalar + " native_probe(" + Scalar +
                           ");\n"
                           "int main(void) {\nuint32_t saved = _mm_getcsr();\n"
                           "uint64_t values[] = {";
      Driver +=
          IsDouble
              ? "UINT64_C(0x3ff4000000000000), UINT64_C(0xbff4000000000000), "
                "UINT64_C(0x7ff0000000000071), UINT64_C(0x8000000000000000)"
              : "0x3fa00000, 0xbfa00000, 0x7f800071, 0x80000000";
      Driver +=
          "};\nfor(unsigned rc=0; rc<4; ++rc)\n"
          "for(unsigned a=0; a<4; ++a) {\nuint32_t state=0x1f80 | (rc<<13);\n" +
          Scalar +
          " input; __builtin_memcpy(&input, &values[a], sizeof(input));\n"
          "_mm_setcsr(state);\n" +
          Scalar +
          " expected = native_probe(input);\n"
          "uint32_t expected_state = _mm_getcsr();\n_mm_setcsr(state);\n" +
          Scalar +
          " actual = isa_probe(input);\n"
          "uint32_t actual_state = _mm_getcsr();\n_mm_setcsr(saved);\n"
          "if (expected_state != actual_state || "
          "memcmp(&expected,&actual,sizeof(actual)))"
          " return 1;\n}\nreturn 0;\n}\n";
      for (bool NoOpt : {false, true})
        for (bool LLVM : {false, true}) {
          SCOPED_TRACE(testing::Message()
                       << Scalar << " imm=" << unsigned(Immediate)
                       << " NoOpt=" << NoOpt << " LLVMC=" << LLVM);
          auto Image = image(Bytes);
          llvm::LLVMContext Context;
          PipelineOptions Options;
          Options.LiftMode = LLVM;
          Options.SourceProjection = LLVM;
          Options.NoOpt = NoOpt;
          Options.EmitDumpOutput = false;
          Options.OnlyFunctionEntries = {Entry};
          auto Result = Pipeline().run(Image, Context, Options);
          ASSERT_TRUE(Result.Success) << Result.Error;
          CEmitterOptions Emission;
          Emission.TheArch = Arch::X64;
          Emission.Format = hostFormat();
          Emission.PreserveLLVMFunctionTypes = true;
          std::string Source, TypedDriver = Driver;
          llvm::raw_string_ostream Out(Source);
          if (LLVM) {
            ASSERT_NE(Result.LlvmModule, nullptr);
            ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Out, Emission,
                                            nullptr, &Image));
            const auto *Function = Result.LlvmModule->getFunction("isa_probe");
            ASSERT_NE(Function, nullptr);
            ASSERT_EQ(Function->arg_size(), 1U);
            if (Function->getArg(0)->getType()->isVectorTy()) {
              ASSERT_TRUE(Function->getReturnType()->isVectorTy());
              const std::string Call = Scalar + " actual = isa_probe(input);";
              const auto Position = TypedDriver.find(Call);
              ASSERT_NE(Position, std::string::npos);
              TypedDriver.replace(
                  Position, Call.size(),
                  "uint64_t __attribute__((vector_size(16))) incoming = "
                  "{0,0};\n"
                  "__builtin_memcpy(&incoming,&input,sizeof(input));\n"
                  "uint64_t __attribute__((vector_size(16))) returned = "
                  "isa_probe(incoming);\n" +
                      Scalar +
                      " actual; "
                      "__builtin_memcpy(&actual,&returned,sizeof(actual));");
            }
          } else {
            ASSERT_EQ(Result.HighFuncs.size(), 1U);
            ASSERT_EQ(Result.HighFuncs.front().ReturnType->Kind,
                      NdTypeKind::Float);
            ASSERT_EQ(Result.HighFuncs.front().ReturnType->Size,
                      IsDouble ? 8 : 4);
            ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Out, Emission));
          }
          const auto C = file("return.c");
          write(C, Source + TypedDriver);
          for (const char *Optimization : {"-O0", "-O2"}) {
            const auto Executable = file("return.exe");
            Built =
                command(Compiler, {Optimization, Object, C, "-o", Executable});
            ASSERT_EQ(Built.Status, 0) << Built.Error << Source;
            const auto Actual = command(Executable, {});
            EXPECT_EQ(Actual.Status, 0) << Actual.Out << Actual.Error << Source;
          }
        }
    }
}

TEST_F(X86FPRoundAccuracy, ScalarMxcsrRoundingAndPrecisionSuppression) {
  for (bool IsDouble : {false, true})
    for (uint8_t Immediate : {0x04, 0x06, 0x08, 0x7e})
      check(IsDouble, true, false, 16, Immediate);
}

TEST_F(X86FPRoundAccuracy, VexScalarPreservesSourceUpperAndClearsYmmUpper) {
  for (bool IsDouble : {false, true})
    for (uint8_t Immediate : {0x04, 0x08, 0x0e})
      check(IsDouble, true, true, 16, Immediate);
}

TEST_F(X86FPRoundAccuracy, PackedOneInstructionNumericalAndStateCompletion) {
  for (bool IsDouble : {false, true})
    for (uint8_t Immediate : {0x04, 0x08, 0x0e}) {
      check(IsDouble, false, false, 16, Immediate);
      if (hasAVX()) {
        check(IsDouble, false, true, 16, Immediate);
        check(IsDouble, false, true, 32, Immediate);
      }
    }
}

TEST_F(X86FPRoundAccuracy, ScalarAndPackedMemoryForms) {
  for (bool IsDouble : {false, true}) {
    check(IsDouble, true, false, 16, 0x04, true);
    check(IsDouble, false, false, 16, 0x0e, true);
    if (hasAVX()) {
      check(IsDouble, true, true, 16, 0x04, true);
      check(IsDouble, false, true, 32, 0x0e, true);
    }
  }
}

TEST_F(X86FPRoundAccuracy, DiscardedNumericalResultRetainsExceptionEffects) {
  for (bool IsDouble : {false, true}) {
    check(IsDouble, true, false, 16, 0x04, false, false);
    check(IsDouble, false, false, 16, 0x08, false, false);
    if (hasAVX())
      check(IsDouble, false, true, 32, 0x04, false, false);
  }
}

TEST_F(X86FPRoundAccuracy, UnmaskedPrecisionCommitsNoNumericalResult) {
  PrecisionFault = true;
  for (bool IsDouble : {false, true}) {
    check(IsDouble, true, false, 16, 4, false, true, true);
    check(IsDouble, false, false, 16, 4, false, true, true);
    if (hasAVX())
      check(IsDouble, false, true, 32, 4, false, true, true);
  }
}

TEST_F(X86FPRoundAccuracy, PrecisionSuppressionDoesNotSuppressInvalid) {
  for (bool IsDouble : {false, true})
    for (uint8_t Immediate : {4, 12}) {
      check(IsDouble, true, false, 16, Immediate, false, true, true);
      check(IsDouble, false, false, 16, Immediate, false, true, true);
    }
}

TEST_F(X86FPRoundAccuracy, PackedInvalidFaultPrecedesOtherLanePrecision) {
  MixedFault = true;
  for (bool IsDouble : {false, true})
    for (bool InvalidMasked : {false, true})
      for (bool Swap : {false, true}) {
        PrecisionFault = InvalidMasked;
        SwapFaultLanes = Swap;
        check(IsDouble, false, false, 16, 4, false, true, true);
        if (hasAVX())
          check(IsDouble, false, true, 32, 4, false, true, true);
      }
}

TEST_F(X86FPRoundAccuracy, StickyPrecisionSurvivesUnmaskedInvalidFault) {
  SeedPrecision = true;
  for (bool IsDouble : {false, true})
    for (uint8_t Immediate : {4, 12}) {
      check(IsDouble, true, false, 16, Immediate, false, true, true);
      MixedFault = true;
      check(IsDouble, false, false, 16, Immediate, false, true, true);
      MixedFault = false;
    }
}

TEST(X86FPRoundContract, MemoryShapesKeepAddressAndNumericalWidthsIndependent) {
  constexpr auto Id = Intrinsic::X86FPRoundMemoryState;
  for (unsigned Control = 0; Control < 16; ++Control)
    for (unsigned Bytes : {4U, 8U, 16U, 32U}) {
      const auto Op = roundMemoryStateOp(Control, Bytes, 0x3000, 0x1f80);
      const bool Valid = Control == 4                   ? Bytes == 4
                         : Control == 6                 ? Bytes == 8
                         : Control == 0 || Control == 2 ? Bytes == 16
                         : Control == 8 || Control == 10
                             ? Bytes == 16 || Bytes == 32
                             : false;
      const auto Shape = x86FPStateLowShape(Op, Arch::X64);
      EXPECT_EQ(x86FPStateShapeIsValid(Id, Shape), Valid);
      if (!Valid)
        continue;
      EXPECT_EQ(x86FPStateSourceBytes(x86FPStateHelperLayout(Id, Shape)),
                Bytes);
      EXPECT_EQ(x86FPStateNumericalSliceSize(Id, Shape, 0, Bytes),
                x86FPRoundStateIsScalar(Control) ? Bytes : 0U);
      EXPECT_EQ(x86FPStateNumericalSliceSize(Id, Shape, Bytes, 4), 0U);
      auto Bad = Op;
      Bad.Inputs[1].Size = 4;
      EXPECT_FALSE(
          x86FPStateShapeIsValid(Id, x86FPStateLowShape(Bad, Arch::X64)));
    }
  for (Arch Target : {Arch::X86, Arch::X64}) {
    auto Address = HighExpr::makeVar(
        MedVar{.Kind = MedVar::Param,
               .TheArch = Target,
               .Id = 0,
               .Size = static_cast<uint16_t>(Target == Arch::X86 ? 4 : 8)},
        NdType::makePtr());
    Address->Type->Size = Target == Arch::X86 ? 4 : 8;
    auto Call = HighExpr::makeCall("", 0,
                                   {Address, HighExpr::makeConst(4, 1),
                                    HighExpr::makeConst(4, 1),
                                    HighExpr::makeConst(0x1f80, 4)});
    Call->IntrinsicId = Id;
    Call->Type = NdType::makeInt(8, false);
    ASSERT_TRUE(x86FPStateShapeIsValid(Id, x86FPStateHighShape(*Call, Target)));
    Address->Type = NdType::makeFloat(8);
    EXPECT_FALSE(
        x86FPStateShapeIsValid(Id, x86FPStateHighShape(*Call, Target)));
  }
}

TEST(X86FPRoundContract,
     MemoryFaultsRetainIncomingStateWithoutNumericalCompletion) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  addRoundSource(Image, 0x3000);
  for (unsigned Control : {0U, 2U, 4U, 6U, 8U, 10U}) {
    const unsigned Bytes = x86FPRoundStateIsScalar(Control)
                               ? x86FPRoundStateElementBytes(Control)
                               : 16;
    for (uint64_t Address : {UINT64_C(0x3001), UINT64_C(0x9000)}) {
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(true);
      ASSERT_TRUE(Emulator.setX86LinearAddressBits(48));
      const auto Op = roundMemoryStateOp(Control, Bytes, Address, 0x5fa0);
      const bool Complete = Address == 0x3001 && (Control & 12);
      EXPECT_EQ(Emulator.step(Op), Complete);
      EXPECT_EQ(bool(Emulator.getRegisterBytes(0)), Complete);
      EXPECT_EQ(Emulator.getMXCSR(), 0x5fa0U);
    }
  }
  NdOpEmulator Unknown(Image);
  Unknown.setStrictMode(true);
  EXPECT_FALSE(Unknown.step(roundMemoryStateOp(0, 16, 0x3000, 0x5fa0)));
  EXPECT_EQ(Unknown.getMXCSR(), 0x1f80U);
}

TEST(X86FPRoundContract,
     MemoryUsesAuthenticatedCanonicalWidthAndResolvedSegmentAlignment) {
  for (Arch Target : {Arch::X86, Arch::X64})
    for (bool OffsetAligned : {false, true}) {
      BinaryImage Image;
      Image.Arch = Target;
      Image.Bits = Target == Arch::X64 ? Bitness::Bits64 : Bitness::Bits32;
      const uint64_t Base = OffsetAligned ? 0x2001 : 0x1fff;
      const uint64_t Offset = OffsetAligned ? 0x1000 : 0x1001;
      addRoundSource(Image, Base + Offset);
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(true);
      if (Target == Arch::X64)
        ASSERT_TRUE(Emulator.setX86LinearAddressBits(48));
      ASSERT_TRUE(Emulator.setMemoryAddressSpaceBase(
          NdMemoryAddressSpace::X86FS, Base));
      const auto Op = roundMemoryStateOp(
          0, 16, Offset | (Target == Arch::X86 ? UINT64_C(1) << 32 : 0), 0x5fa0,
          NdMemoryAddressSpace::X86FS);
      EXPECT_EQ(Emulator.step(Op), !OffsetAligned);
      EXPECT_EQ(Emulator.getMXCSR(), 0x5fa0U);
    }
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  constexpr uint64_t Address = UINT64_C(1) << 47;
  addRoundSource(Image, Address);
  for (uint8_t Bits : {48, 57}) {
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    ASSERT_TRUE(Emulator.setX86LinearAddressBits(Bits));
    EXPECT_EQ(Emulator.step(roundMemoryStateOp(0, 16, Address, 0x5fa0)),
              Bits == 57);
    EXPECT_EQ(Emulator.getLoadRecords().size(), Bits == 57 ? 1U : 0U);
    EXPECT_EQ(Emulator.getMXCSR(), 0x5fa0U);
  }
  NdOpEmulator UnknownSegment(Image);
  ASSERT_TRUE(UnknownSegment.setX86LinearAddressBits(48));
  EXPECT_FALSE(UnknownSegment.step(
      roundMemoryStateOp(0, 16, 0, 0x5fa0, NdMemoryAddressSpace::X86GS)));
  EXPECT_EQ(UnknownSegment.getMXCSR(), 0x1f80U);
}

TEST_F(X86FPRoundAccuracy, LegacyPackedMisalignmentFaultsBeforeInvalidOperand) {
  AlignmentFault = true;
  for (bool IsDouble : {false, true})
    check(IsDouble, false, false, 16, 4, true, true, true);
}

TEST_F(X86FPRoundAccuracy, ScalarAndVexPackedAllowUnalignedSources) {
  UnalignedSource = true;
  for (bool IsDouble : {false, true}) {
    check(IsDouble, true, false, 16, 4, true);
    if (hasAVX()) {
      check(IsDouble, true, true, 16, 4, true);
      check(IsDouble, false, true, 16, 4, true);
      check(IsDouble, false, true, 32, 4, true);
    }
  }
}

TEST_F(X86FPRoundAccuracy, ProtectedMemoryFaultsBeforeFloatingPointEvaluation) {
  PageFault = true;
  for (bool IsDouble : {false, true}) {
    check(IsDouble, true, false, 16, 4, true, true, true);
    check(IsDouble, false, false, 16, 4, true, true, true);
    if (hasAVX()) {
      check(IsDouble, true, true, 16, 4, true, true, true);
      check(IsDouble, false, true, 16, 4, true, true, true);
      check(IsDouble, false, true, 32, 4, true, true, true);
    }
  }
}

TEST_F(X86FPRoundAccuracy, CrossPageSourcesRequireTheEntireAccess) {
  PageFault = CrossPage = true;
  for (bool IsDouble : {false, true}) {
    check(IsDouble, true, false, 16, 4, true, true, true);
    if (hasAVX()) {
      check(IsDouble, true, true, 16, 4, true, true, true);
      check(IsDouble, false, true, 16, 4, true, true, true);
      check(IsDouble, false, true, 32, 4, true, true, true);
    }
  }
}

TEST(X86FPRoundContract, WritebackBytesCannotAuthorizeAnUnreadableSource) {
  for (bool Readable : {false, true}) {
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    addRoundSource(Image, 0x3000, 16);
    Image.Segments[0].Flags =
        Readable ? SegmentFlags::Readable | SegmentFlags::Writable
                 : SegmentFlags::Writable;
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    ASSERT_TRUE(Emulator.setX86LinearAddressBits(48));
    for (unsigned Offset : {0U, 8U}) {
      LowOp Store;
      Store.Opcode = NdOp::STORE;
      Store.addInput(NdVar::cst(0x3000 + Offset, 8));
      Store.addInput(NdVar::cst(0, 8));
      ASSERT_TRUE(Emulator.step(Store));
    }
    Emulator.setLoadCollect(true);
    EXPECT_EQ(Emulator.step(roundMemoryStateOp(0, 16, 0x3000, 0x5fa0)),
              Readable);
    EXPECT_EQ(bool(Emulator.getRegisterBytes(0)), Readable);
    EXPECT_EQ(Emulator.getLoadRecords().size(), Readable ? 1U : 0U);
    EXPECT_EQ(Emulator.getMXCSR(), 0x5fa0U);
  }
}

TEST_F(X86FPRoundAccuracy, NativeGsMemoryUsesTheArchitecturalBaseOnce) {
#if defined(_WIN32) || defined(__linux__)
  SourceSegment = NdMemoryAddressSpace::X86GS;
  for (bool IsDouble : {false, true}) {
    check(IsDouble, true, false, 16, 4, true);
    if (hasAVX())
      check(IsDouble, true, true, 16, 4, true);
  }
#else
  GTEST_SKIP() << "native GS source fixture requires Windows or Linux";
#endif
}

TEST_F(X86FPRoundAccuracy, NativeFsMemoryUsesTheArchitecturalBaseOnce) {
#if defined(__linux__)
  SourceSegment = NdMemoryAddressSpace::X86FS;
  for (bool IsDouble : {false, true}) {
    check(IsDouble, true, false, 16, 4, true);
    if (hasAVX())
      check(IsDouble, true, true, 16, 4, true);
  }
#else
  GTEST_SKIP() << "native FS source fixture requires Linux TLS";
#endif
}

} // namespace
