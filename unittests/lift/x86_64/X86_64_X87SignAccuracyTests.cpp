//===- X86_64_X87SignAccuracyTests.cpp - Exact x87 signs ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check binary80 sign changes against original bytes on every output route.
//===----------------------------------------------------------------------===//

#include "X86FPStateAccuracyFixture.h"

#include "neverd/ir/X87Context.h"
#include "neverd/lift/X86Lifter.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/ScopeExit.h"

namespace {
#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
template <unsigned Opcode>
std::array<uint8_t, 10> nativeSign(const std::array<uint8_t, 10> &Input) {
  std::array<uint8_t, 10> Output{};
  __asm__ volatile("fldt %1\n\t.byte 0xd9,%c2\n\tfstpt %0"
                   : "=m"(Output)
                   : "m"(Input), "i"(Opcode)
                   : "st", "memory");
  return Output;
}
#endif

TEST(X87SignConcrete, EverySignExponentWordMatchesOriginalBytes) {
#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
  Decoder D;
  ASSERT_TRUE(D.init(Arch::X64));
  BinaryImage Image;
  Image.Arch = Arch::X64;
  uint16_t Saved;
  __asm__ volatile("fnstcw %0" : "=m"(Saved));
  llvm::scope_exit Restore([&] {
    __asm__ volatile("fninit\n\tfldcw %0" ::"m"(Saved) : "memory");
  });
  const uint16_t Control = 0x37f;
  __asm__ volatile("fninit\n\tfldcw %0" ::"m"(Control) : "memory");
  for (unsigned Opcode : {0xe0U, 0xe1U}) {
    const uint8_t Bytes[] = {0xd9, uint8_t(Opcode)};
    DecodedInsn I;
    ASSERT_EQ(D.decodeOneForLift(Bytes, sizeof(Bytes), Entry, I), 2);
    std::vector<LowOp> Ops;
    ASSERT_NO_THROW(D.liftToLow(I, Ops));
    for (unsigned Word = 0; Word < 65536; ++Word)
      for (uint64_t Mantissa : {UINT64_C(0), UINT64_MAX}) {
        std::array<uint8_t, 10> Input{};
        std::memcpy(Input.data(), &Mantissa, 8);
        Input[8] = uint8_t(Word);
        Input[9] = uint8_t(Word >> 8);
        const auto Expected =
            Opcode == 0xe0 ? nativeSign<0xe0>(Input) : nativeSign<0xe1>(Input);
        for (bool Strict : {false, true}) {
          NdOpEmulator E(Image);
          E.setStrictMode(Strict);
          E.setRegisterBytes(x86reg::ST0, Input);
          E.setRegister(x86reg::FPU_CW, Control);
          E.setRegister(x86reg::FPU_SW, 0x2322);
          for (const auto &Op : Ops)
            ASSERT_TRUE(E.step(Op)) << Opcode << ':' << Word << ':' << Strict;
          EXPECT_EQ(E.getRegisterBytes(x86reg::ST0),
                    std::vector<uint8_t>(Expected.begin(), Expected.end()))
              << Opcode << ':' << Word << ':' << Mantissa << ':' << Strict;
          EXPECT_EQ(*E.getRegister(x86reg::FPU_SW) & ~0x4500U, 0x2022U);
          EXPECT_EQ(E.getRegister(x86reg::FPU_CW), Control);
        }
      }
  }
#else
  GTEST_SKIP() << "native x64 GCC/Clang oracle required";
#endif
}

TEST(X87SignConcrete, OwnedContextSignExponentWordsMatchOriginalBytes) {
#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
  Decoder D;
  ASSERT_TRUE(D.init(Arch::X64));
  D.setX87ContextOwnership(true);
  BinaryImage Image;
  Image.Arch = Arch::X64;
  uint16_t Saved;
  __asm__ volatile("fnstcw %0" : "=m"(Saved));
  llvm::scope_exit Restore([&] {
    __asm__ volatile("fninit\n\tfldcw %0" ::"m"(Saved) : "memory");
  });
  const uint16_t Control = 0x37f;
  __asm__ volatile("fninit\n\tfldcw %0" ::"m"(Control) : "memory");
  for (unsigned Opcode : {0xe0U, 0xe1U}) {
    const uint8_t Bytes[] = {0xd9, uint8_t(Opcode)};
    DecodedInsn I;
    ASSERT_EQ(D.decodeOneForLift(Bytes, sizeof(Bytes), Entry, I), 2);
    std::vector<LowOp> Ops;
    ASSERT_NO_THROW(D.liftToLow(I, Ops));
    for (unsigned Word = 0; Word < 65536; ++Word)
      for (uint64_t Mantissa : {UINT64_C(0), UINT64_MAX}) {
        std::array<uint8_t, 10> Input{};
        std::memcpy(Input.data(), &Mantissa, 8);
        Input[8] = uint8_t(Word);
        Input[9] = uint8_t(Word >> 8);
        const auto Expected =
            Opcode == 0xe0 ? nativeSign<0xe0>(Input) : nativeSign<0xe1>(Input);
        for (bool Strict : {false, true}) {
          NdOpEmulator E(Image);
          E.setStrictMode(Strict);
          std::array<uint8_t, 512> State{};
          std::memcpy(State.data(), &Control, 2);
          State[2] = 0x22;
          State[3] = 0x23; // TOP=4, with masked sticky exceptions and C1.
          State[4] = 0x10;
          std::copy(Input.begin(), Input.end(), State.begin() + 32);
          ASSERT_TRUE(E.setX87Context(State));
          for (const auto &Op : Ops)
            ASSERT_TRUE(E.step(Op)) << Opcode << ':' << Word << ':' << Strict;
          EXPECT_EQ(E.getRegisterBytes(x86reg::ST4),
                    std::vector<uint8_t>(Expected.begin(), Expected.end()))
              << Opcode << ':' << Word << ':' << Mantissa << ':' << Strict;
          EXPECT_EQ(*E.getRegister(x86reg::FPU_SW) & ~0x4500U, 0x2022U);
          EXPECT_EQ(E.getRegister(x86reg::FPU_CW), Control);
        }
      }
  }
#else
  GTEST_SKIP() << "native x64 GCC/Clang oracle required";
#endif
}

TEST(X87SignConcrete, ScalarIEEEOperationsPreserveNaNsAndSignedZero) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  for (unsigned Size : {4U, 8U})
    for (NdOp Code : {NdOp::FLOAT_NEG, NdOp::FLOAT_ABS})
      for (uint64_t Value :
           {UINT64_C(0), UINT64_C(1), UINT64_MAX, UINT64_C(0x7f800000),
            UINT64_C(0x7fa12345), UINT64_C(0xff812345), UINT64_C(0x80000000),
            UINT64_C(0x8000000000000000), UINT64_C(0x7ff0000000000000),
            UINT64_C(0x7ff0000000001234), UINT64_C(0xfff8000000001234)}) {
        llvm::APFloat Reference(Size == 4 ? llvm::APFloat::IEEEsingle()
                                          : llvm::APFloat::IEEEdouble(),
                                llvm::APInt(Size * 8, Value));
        if (Code == NdOp::FLOAT_NEG)
          Reference.changeSign();
        else
          Reference.clearSign();
        NdOpEmulator E(Image);
        E.setMXCSR(0xffbf);
        E.setRegister(10001, Value);
        LowOp Op;
        Op.Opcode = Code;
        Op.Output = NdVar::tmp(10002, Size);
        Op.addInput(NdVar::reg(10001, Size));
        ASSERT_TRUE(E.step(Op));
        EXPECT_EQ(E.getRegister(10002),
                  Reference.bitcastToAPInt().getZExtValue());
        EXPECT_EQ(E.getMXCSR(), 0xffbfU);
      }
}

TEST(X87SignContract, MalformedOrUnknownPureValuesRefuseAtomically) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  for (unsigned Size : {4U, 8U, 10U})
    for (NdOp Code : {NdOp::FLOAT_NEG, NdOp::FLOAT_ABS})
      for (unsigned Mutation = 0; Mutation < 7; ++Mutation) {
        NdOpEmulator E(Image);
        E.setStrictMode(true);
        E.setMXCSR(0xffbf);
        E.setRegisterBytes(10002, std::vector<uint8_t>(16, 0xa5));
        LowOp Op;
        Op.Opcode = Code;
        Op.Output = NdVar::reg(10002, Size);
        Op.addInput(NdVar::reg(10001, Size));
        if (Mutation != 0)
          E.setRegisterBytes(10001, std::vector<uint8_t>(Size, 0xb3));
        switch (Mutation) {
        case 1:
          Op.Output.Size = 12;
          break;
        case 2:
          Op.Inputs[0].Size = 2;
          break;
        case 3:
          Op.NumInputs = 0;
          break;
        case 4:
          Op.addInput(NdVar::cst(0, 1));
          break;
        case 5:
          Op.Output.Space = VnodeSpace::CONST;
          break;
        case 6:
          Op.Inputs[0].Space = VnodeSpace::RAM;
          break;
        default:
          break;
        }
        EXPECT_FALSE(E.step(Op)) << Size << ':' << Mutation;
        EXPECT_EQ(E.getRegisterBytes(10002), std::vector<uint8_t>(16, 0xa5));
        EXPECT_EQ(E.getMXCSR(), 0xffbfU);
      }
}

TEST(X87SignContract, PendingOrUnknownExceptionsStopBeforeValueAndC1) {
  Decoder D;
  ASSERT_TRUE(D.init(Arch::X64));
  BinaryImage Image;
  Image.Arch = Arch::X64;
  for (unsigned Opcode : {0xe0U, 0xe1U})
    for (unsigned State = 0; State < 3; ++State) {
      const uint8_t Bytes[] = {0xd9, uint8_t(Opcode)};
      DecodedInsn I;
      ASSERT_EQ(D.decodeOneForLift(Bytes, sizeof(Bytes), Entry, I), 2);
      std::vector<LowOp> Ops;
      ASSERT_NO_THROW(D.liftToLow(I, Ops));
      ASSERT_FALSE(Ops.empty());
      EXPECT_EQ(Ops[0].Opcode, NdOp::INTRINSIC);
      EXPECT_EQ(Ops[0].Inputs[0].Offset, unsigned(Intrinsic::X87Wait));
      NdOpEmulator E(Image);
      if (State == 2) {
        E.setCallPreservedRegisters({});
        LowOp Call;
        Call.Opcode = NdOp::CALL;
        Call.addInput(NdVar::cst(Entry, 8));
        ASSERT_TRUE(E.step(Call));
      } else {
        E.setRegister(x86reg::FPU_CW, 0x37e);
        E.setRegister(x86reg::FPU_SW, State == 0 ? 0x281 : 0x201);
      }
      const auto Status = E.getRegister(x86reg::FPU_SW);
      const std::vector<uint8_t> Value(10, 0xa5);
      E.setRegisterBytes(x86reg::ST0, Value);
      EXPECT_FALSE(E.step(Ops[0]));
      EXPECT_EQ(E.getRegisterBytes(x86reg::ST0), Value);
      EXPECT_EQ(E.getRegister(x86reg::FPU_SW), Status);
    }
}

TEST(X87SignContract, OwnedContextPendingExceptionsStopBeforeValueAndC1) {
  Decoder D;
  ASSERT_TRUE(D.init(Arch::X64));
  D.setX87ContextOwnership(true);
  BinaryImage Image;
  Image.Arch = Arch::X64;
  for (unsigned Opcode : {0xe0U, 0xe1U})
    for (unsigned State = 0; State < 3; ++State) {
      const uint8_t Bytes[] = {0xd9, uint8_t(Opcode)};
      DecodedInsn I;
      ASSERT_EQ(D.decodeOneForLift(Bytes, sizeof(Bytes), Entry, I), 2);
      std::vector<LowOp> Ops;
      ASSERT_NO_THROW(D.liftToLow(I, Ops));
      ASSERT_FALSE(Ops.empty());
      EXPECT_EQ(Ops[0].Opcode, NdOp::INTRINSIC);
      EXPECT_EQ(Ops[0].Inputs[0].Offset, unsigned(Intrinsic::X87ContextStep));
      ASSERT_TRUE(x87ContextInstruction(Ops[0].Inputs[2].Offset));
      EXPECT_TRUE(x87ContextInstruction(Ops[0].Inputs[2].Offset)->Waiting);
      NdOpEmulator E(Image);
      if (State == 2) {
        E.setCallPreservedRegisters({});
        LowOp Call;
        Call.Opcode = NdOp::CALL;
        Call.addInput(NdVar::cst(Entry, 8));
        ASSERT_TRUE(E.step(Call));
      } else {
        E.setRegister(x86reg::FPU_CW, 0x37e);
        E.setRegister(x86reg::FPU_SW, State == 0 ? 0x281 : 0x201);
      }
      const auto Status = E.getRegister(x86reg::FPU_SW);
      const std::vector<uint8_t> Value(10, 0xa5);
      E.setRegisterBytes(x86reg::ST0, Value);
      EXPECT_FALSE(E.step(Ops[0]));
      EXPECT_EQ(E.getRegisterBytes(x86reg::ST0), Value);
      EXPECT_EQ(E.getRegister(x86reg::FPU_SW), Status);
    }
}

TEST(X87SignContract, PhysicalStackSlotsAndPaddingRemainDistinct) {
  Decoder D;
  ASSERT_TRUE(D.init(Arch::X64));
  BinaryImage Image;
  Image.Arch = Arch::X64;
  for (unsigned Depth = 0; Depth < 8; ++Depth)
    for (unsigned Opcode : {0xe0U, 0xe1U}) {
      X86Lifter L(Arch::X64);
      DecodedInsn I;
      const uint8_t Push[] = {0xd9, 0xe8};
      ASSERT_EQ(D.decodeOneForLift(Push, sizeof(Push), Entry, I), 2);
      for (unsigned N = 0; N < Depth; ++N) {
        std::vector<LowOp> Discard;
        L.lift(I.Raw, Discard);
      }
      const uint8_t Bytes[] = {0xd9, uint8_t(Opcode)};
      ASSERT_EQ(D.decodeOneForLift(Bytes, sizeof(Bytes), Entry, I), 2);
      std::vector<LowOp> Ops;
      L.lift(I.Raw, Ops);
      const unsigned Top = L.getFpuTop();
      NdOpEmulator E(Image);
      std::array<std::vector<uint8_t>, 8> Expected;
      for (unsigned Slot = 0; Slot < 8; ++Slot) {
        Expected[Slot] = std::vector<uint8_t>(16, uint8_t(0xd0 + Slot));
        E.setRegisterBytes(x86reg::stReg(Slot), Expected[Slot]);
      }
      E.setRegister(x86reg::FPU_SW, 0x2322);
      E.setRegister(x86reg::FPU_CW, 0x37f);
      if (Opcode == 0xe0)
        Expected[Top][9] ^= 0x80;
      else
        Expected[Top][9] &= 0x7f;
      for (const auto &Op : Ops)
        ASSERT_TRUE(E.step(Op));
      for (unsigned Slot = 0; Slot < 8; ++Slot)
        EXPECT_EQ(E.getRegisterBytes(x86reg::stReg(Slot)), Expected[Slot])
            << Depth << ':' << Opcode << ':' << Slot;
      EXPECT_EQ(L.getFpuTop(), int(Top));
      EXPECT_EQ(*E.getRegister(x86reg::FPU_SW) & ~0x4500U, 0x2022U);
    }
}

TEST(X87SignContract, OwnedContextStackSlotsAndPaddingRemainDistinct) {
  Decoder D;
  ASSERT_TRUE(D.init(Arch::X64));
  BinaryImage Image;
  Image.Arch = Arch::X64;
  for (unsigned Depth = 0; Depth < 8; ++Depth)
    for (unsigned Opcode : {0xe0U, 0xe1U}) {
      X86Lifter L(Arch::X64);
      L.setX87ContextOwnership(true);
      DecodedInsn I;
      const uint8_t Push[] = {0xd9, 0xe8};
      ASSERT_EQ(D.decodeOneForLift(Push, sizeof(Push), Entry, I), 2);
      for (unsigned N = 0; N < Depth; ++N) {
        std::vector<LowOp> Discard;
        L.lift(I.Raw, Discard);
      }
      const uint8_t Bytes[] = {0xd9, uint8_t(Opcode)};
      ASSERT_EQ(D.decodeOneForLift(Bytes, sizeof(Bytes), Entry, I), 2);
      std::vector<LowOp> Ops;
      L.lift(I.Raw, Ops);
      const unsigned Top = (8 - Depth) & 7;
      NdOpEmulator E(Image);
#if !(defined(__x86_64__) || defined(_M_X64)) ||                               \
    !(defined(__clang__) || defined(__GNUC__))
      // Keep this host-independent contract runnable through the explicit
      // portable slice; native original-instruction tests remain separate.
      E.setX87PortableMode(true);
#endif
      std::array<std::vector<uint8_t>, 8> Expected;
      std::array<uint8_t, 512> State{};
      State[0] = 0x7f;
      State[1] = 3;
      State[2] = 0x22;
      State[3] = uint8_t((Top << 3) | 3);
      State[4] = 0xff;
      for (unsigned Slot = 0; Slot < 8; ++Slot) {
        Expected[Slot] = std::vector<uint8_t>(10, uint8_t(0xd0 + Slot));
        E.setRegister(x86reg::stReg(Slot) + 10, 0xa500 + Slot);
        std::fill_n(State.begin() + 32 + ((Slot + 8 - Top) & 7) * 16, 16,
                    uint8_t(0xd0 + Slot));
      }
      ASSERT_TRUE(E.setX87Context(State));
      if (Opcode == 0xe0)
        Expected[Top][9] ^= 0x80;
      else
        Expected[Top][9] &= 0x7f;
      for (const auto &Op : Ops)
        ASSERT_TRUE(E.step(Op));
      for (unsigned Slot = 0; Slot < 8; ++Slot)
        EXPECT_EQ(E.getRegisterBytes(x86reg::stReg(Slot)), Expected[Slot])
            << Depth << ':' << Opcode << ':' << Slot;
      ASSERT_TRUE(E.getX87Context());
      for (unsigned Slot = 0; Slot < 8; ++Slot)
        EXPECT_EQ(E.getRegister(x86reg::stReg(Slot) + 10), 0xa500 + Slot);
      EXPECT_EQ((*E.getRegister(x86reg::FPU_SW) >> 11) & 7, Top);
      EXPECT_EQ(*E.getRegister(x86reg::FPU_SW) & ~0x7d00U, 0x22U);
    }
}

TEST(X87SignContract, KnownEmptyOrPoppedStacksAreRejected) {
  for (unsigned Opcode : {0xe0U, 0xe1U})
    for (bool Popped : {false, true}) {
      BinaryImage Image;
      Image.Arch = Arch::X64;
      Image.Bits = Bitness::Bits64;
      Image.Format = BinaryFormat::COFF;
      Image.X87ContextOverride = false;
      Image.Base = Image.Entry = Entry;
      Segment Text;
      Text.Name = ".text";
      Text.VA = Entry;
      Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
      Text.Data = {0xdb, 0xe3}; // fninit
      if (Popped)
        Text.Data.insert(Text.Data.end(), {0xd9, 0xe8, 0xdd, 0xd8});
      Text.Data.insert(Text.Data.end(), {0xd9, uint8_t(Opcode), 0xc3});
      Text.Size = Text.FileSz = Text.Data.size();
      Image.Segments.push_back(Text);
      Section Code;
      Code.Name = Text.Name;
      Code.VA = Entry;
      Code.Size = Code.FileSz = Text.Size;
      Code.Flags = Text.Flags;
      Code.Data = Text.Data;
      Image.Sections.push_back(Code);
      auto Symbol = Symbol::makeFunc(Entry, Text.Size);
      Symbol.Name = "empty_sign";
      Image.Symbols.push_back(Symbol);
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      Options.OnlyFunctionEntries = {Entry};
      auto Result = Pipeline().run(Image, Context, Options);
      EXPECT_TRUE(Result.LowFuncs.empty()) << Opcode << ':' << Popped;
      ASSERT_EQ(Result.FunctionAudits.size(), 1U);
      const va_t SignAddress = Entry + (Popped ? 6 : 2);
      const auto &Unsupported =
          Result.FunctionAudits[0].UnsupportedInstructions;
      EXPECT_NE(std::find(Unsupported.begin(), Unsupported.end(), SignAddress),
                Unsupported.end());
      EXPECT_EQ(Result.FunctionAudits[0].Disposition,
                PipelineFunctionDisposition::RejectedIncomplete);
      EXPECT_FALSE(Result.FunctionAudits[0].HasLLVMDefinition);
    }
}

TEST(X87SignContract, EmptyOrPoppedStacksRetainOwnedRuntimeExceptionState) {
  for (unsigned Opcode : {0xe0U, 0xe1U})
    for (bool Popped : {false, true}) {
      BinaryImage Image;
      Image.Arch = Arch::X64;
      Image.Bits = Bitness::Bits64;
      Image.Format = BinaryFormat::COFF;
      Image.X87ContextOverride = true;
      Image.Base = Image.Entry = Entry;
      Segment Text;
      Text.Name = ".text";
      Text.VA = Entry;
      Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
      Text.Data = {0xdb, 0xe3}; // fninit
      if (Popped)
        Text.Data.insert(Text.Data.end(), {0xd9, 0xe8, 0xdd, 0xd8});
      Text.Data.insert(Text.Data.end(), {0xd9, uint8_t(Opcode), 0xc3});
      Text.Size = Text.FileSz = Text.Data.size();
      Image.Segments.push_back(Text);
      Section Code;
      Code.Name = Text.Name;
      Code.VA = Entry;
      Code.Size = Code.FileSz = Text.Size;
      Code.Flags = Text.Flags;
      Code.Data = Text.Data;
      Image.Sections.push_back(Code);
      auto Symbol = Symbol::makeFunc(Entry, Text.Size);
      Symbol.Name = "empty_sign";
      Image.Symbols.push_back(Symbol);
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      Options.LiftMode = true;
      Options.SourceProjection = true;
      Options.OnlyFunctionEntries = {Entry};
      auto Result = Pipeline().run(Image, Context, Options);
      ASSERT_EQ(Result.LowFuncs.size(), 1U) << Opcode << ':' << Popped;
      ASSERT_EQ(Result.FunctionAudits.size(), 1U);
      const va_t SignAddress = Entry + (Popped ? 6 : 2);
      const auto &Unsupported =
          Result.FunctionAudits[0].UnsupportedInstructions;
      EXPECT_TRUE(Unsupported.empty());
      EXPECT_EQ(Result.FunctionAudits[0].Disposition,
                PipelineFunctionDisposition::Accepted);
      EXPECT_TRUE(Result.FunctionAudits[0].HasLLVMDefinition);
      unsigned Steps = 0;
      for (const auto &Block : Result.LowFuncs[0].Blocks)
        for (const auto &Op : Block.Ops)
          if (Op.Addr == SignAddress && Op.Opcode == NdOp::INTRINSIC &&
              Op.Inputs[0].Offset == unsigned(Intrinsic::X87ContextStep)) {
            ++Steps;
            EXPECT_EQ(Op.Inputs[2].Offset, 0xd9u | (Opcode << 8));
          }
      EXPECT_EQ(Steps, 1u);
    }
}

BinaryImage signControlFlowImage(llvm::ArrayRef<uint8_t> Bytes,
                                 BinaryFormat Format) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = Format;
  Image.Base = Image.Entry = Entry;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Entry;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Bytes.begin(), Bytes.end());
  Text.Size = Text.FileSz = Text.Data.size();
  Image.Segments.push_back(Text);
  Section Code;
  Code.Name = Text.Name;
  Code.VA = Entry;
  Code.Size = Code.FileSz = Text.Size;
  Code.Flags = Text.Flags;
  Code.Data = Text.Data;
  Image.Sections.push_back(Code);
  auto Symbol = Symbol::makeFunc(Entry, Text.Size);
  Symbol.Name = "sign_flow";
  Image.Symbols.push_back(Symbol);
  return Image;
}

TEST(X87SignContract, EveryIncomingBranchAndLoopMustRetainAFullSlot) {
  for (BinaryFormat Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
    for (bool NoOpt : {false, true})
      for (unsigned Opcode : {0xe0U, 0xe1U})
        for (bool Loop : {false, true})
          for (bool Invalid : {false, true}) {
            std::vector<uint8_t> Bytes;
            va_t SignAddress;
            if (Loop) {
              Bytes = {0xdb, 0xe3,           0xd9, 0xe8, // fninit; fld1
                       0xd9, uint8_t(Opcode)};
              SignAddress = Entry + 4;
              if (Invalid)
                Bytes.insert(Bytes.end(), {0xdb, 0xe3}); // reset on backedge
              Bytes.insert(Bytes.end(),
                           {0xff, 0xc9, 0x75, uint8_t(Invalid ? 0xf8 : 0xfa),
                            0xdb, 0x3a, 0xc3}); // dec ecx; jnz; fstp [rdx]; ret
            } else {
              Bytes = {0xdb,
                       0xe3,
                       0x85,
                       0xc9,
                       0x74,
                       0x04,
                       0xd9,
                       0xe8,
                       0xeb,
                       0x02,
                       uint8_t(Invalid ? 0x90 : 0xd9),
                       uint8_t(Invalid ? 0x90 : 0xee),
                       0xd9,
                       uint8_t(Opcode),
                       0xdb,
                       0x3a,
                       0xc3};
              SignAddress = Entry + 12;
            }
            auto Image = signControlFlowImage(Bytes, Format);
            Image.X87ContextOverride = false;
            llvm::LLVMContext Context;
            PipelineOptions Options;
            Options.EmitDumpOutput = false;
            Options.NoOpt = NoOpt;
            Options.LiftMode = true;
            Options.SourceProjection = true;
            Options.OnlyFunctionEntries = {Entry};
            auto Result = Pipeline().run(Image, Context, Options);
            ASSERT_EQ(Result.FunctionAudits.size(), 1U);
            const auto &Audit = Result.FunctionAudits[0];
            if (Invalid) {
              EXPECT_EQ(Audit.Disposition,
                        PipelineFunctionDisposition::RejectedIncomplete);
              EXPECT_NE(std::find(Audit.UnsupportedInstructions.begin(),
                                  Audit.UnsupportedInstructions.end(),
                                  SignAddress),
                        Audit.UnsupportedInstructions.end());
              EXPECT_FALSE(Audit.HasLLVMDefinition);
            } else {
              EXPECT_EQ(Audit.Disposition,
                        PipelineFunctionDisposition::Accepted)
                  << Result.Error;
              EXPECT_TRUE(Audit.UnsupportedInstructions.empty());
              EXPECT_TRUE(Audit.HasLLVMDefinition);
            }
          }
}

TEST(X87SignContract, EveryIncomingBranchAndLoopRetainsTheRuntimeContext) {
  for (BinaryFormat Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
    for (bool NoOpt : {false, true})
      for (unsigned Opcode : {0xe0U, 0xe1U})
        for (bool Loop : {false, true})
          for (bool Invalid : {false, true}) {
            std::vector<uint8_t> Bytes;
            va_t SignAddress;
            if (Loop) {
              Bytes = {0xdb, 0xe3,           0xd9, 0xe8, // fninit; fld1
                       0xd9, uint8_t(Opcode)};
              SignAddress = Entry + 4;
              if (Invalid)
                Bytes.insert(Bytes.end(), {0xdb, 0xe3}); // reset on backedge
              Bytes.insert(Bytes.end(),
                           {0xff, 0xc9, 0x75, uint8_t(Invalid ? 0xf8 : 0xfa),
                            0xdb, 0x3a, 0xc3}); // dec ecx; jnz; fstp [rdx]; ret
            } else {
              Bytes = {0xdb,
                       0xe3,
                       0x85,
                       0xc9,
                       0x74,
                       0x04,
                       0xd9,
                       0xe8,
                       0xeb,
                       0x02,
                       uint8_t(Invalid ? 0x90 : 0xd9),
                       uint8_t(Invalid ? 0x90 : 0xee),
                       0xd9,
                       uint8_t(Opcode),
                       0xdb,
                       0x3a,
                       0xc3};
              SignAddress = Entry + 12;
            }
            auto Image = signControlFlowImage(Bytes, Format);
            Image.X87ContextOverride = true;
            llvm::LLVMContext Context;
            PipelineOptions Options;
            Options.EmitDumpOutput = false;
            Options.NoOpt = NoOpt;
            Options.LiftMode = true;
            Options.SourceProjection = true;
            Options.OnlyFunctionEntries = {Entry};
            auto Result = Pipeline().run(Image, Context, Options);
            ASSERT_EQ(Result.FunctionAudits.size(), 1U);
            const auto &Audit = Result.FunctionAudits[0];
            EXPECT_EQ(Audit.Disposition, PipelineFunctionDisposition::Accepted)
                << Result.Error;
            EXPECT_TRUE(Audit.UnsupportedInstructions.empty());
            EXPECT_TRUE(Audit.HasLLVMDefinition);
            ASSERT_EQ(Result.LowFuncs.size(), 1U);
            unsigned Steps = 0;
            for (const auto &Block : Result.LowFuncs[0].Blocks)
              for (const auto &Op : Block.Ops)
                if (Op.Addr == SignAddress && Op.Opcode == NdOp::INTRINSIC &&
                    Op.Inputs[0].Offset ==
                        unsigned(Intrinsic::X87ContextStep)) {
                  ++Steps;
                  EXPECT_EQ(Op.Inputs[2].Offset, 0xd9u | (Opcode << 8));
                  EXPECT_TRUE(Op.Inputs[1].isTemp());
                }
            EXPECT_EQ(Steps, 1u);
          }
}

TEST(X87SignContract, AStaticFunctionBoundaryProvesNoSourceValue) {
  // The calling convention empties the register stack at the boundary but
  // defines no slot, so a sign change or FXAM there still reads no value.
  for (BinaryFormat Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
    for (unsigned Opcode : {0xe0U, 0xe1U, 0xe5U}) {
      auto Image =
          signControlFlowImage({0xd9, uint8_t(Opcode), 0xdb, 0x3a, 0xc3},
                               Format); // op; fstp [rdx]; ret
      Image.Arch = Arch::X86;
      Image.Bits = Bitness::Bits32;
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      Options.LiftMode = true;
      Options.SourceProjection = true;
      Options.OnlyFunctionEntries = {Entry};
      auto Result = Pipeline().run(Image, Context, Options);
      ASSERT_EQ(Result.FunctionAudits.size(), 1U);
      const auto &Audit = Result.FunctionAudits[0];
      EXPECT_EQ(Audit.Disposition,
                PipelineFunctionDisposition::RejectedIncomplete)
          << Opcode;
      EXPECT_NE(std::find(Audit.UnsupportedInstructions.begin(),
                          Audit.UnsupportedInstructions.end(), Entry),
                Audit.UnsupportedInstructions.end())
          << Opcode;
      EXPECT_FALSE(Audit.HasLLVMDefinition);
    }
}

TEST(X87SignContract, AFunctionBoundaryProvesNoSourceValue) {
  // The calling convention empties the register stack at the boundary but
  // defines no slot, so a sign change or FXAM there still reads no value.
  for (BinaryFormat Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
    for (unsigned Opcode : {0xe0U, 0xe1U, 0xe5U}) {
      auto Image =
          signControlFlowImage({0xd9, uint8_t(Opcode), 0xdb, 0x3a, 0xc3},
                               Format); // op; fstp [rdx]; ret
      Image.X87ContextOverride = false;
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      Options.LiftMode = true;
      Options.SourceProjection = true;
      Options.OnlyFunctionEntries = {Entry};
      auto Result = Pipeline().run(Image, Context, Options);
      ASSERT_EQ(Result.FunctionAudits.size(), 1U);
      const auto &Audit = Result.FunctionAudits[0];
      EXPECT_EQ(Audit.Disposition,
                PipelineFunctionDisposition::RejectedIncomplete)
          << Opcode;
      EXPECT_NE(std::find(Audit.UnsupportedInstructions.begin(),
                          Audit.UnsupportedInstructions.end(), Entry),
                Audit.UnsupportedInstructions.end())
          << Opcode;
      EXPECT_FALSE(Audit.HasLLVMDefinition);
    }
}

TEST(X87SignContract, X64EntryCapturesItsRuntimeStateBeforeReadingASlot) {
  for (BinaryFormat Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
    for (unsigned Opcode : {0xe0U, 0xe1U, 0xe5U}) {
      auto Image = signControlFlowImage({0xd9, uint8_t(Opcode), 0xc3}, Format);
      Image.X87ContextOverride = true;
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      Options.LiftMode = true;
      Options.SourceProjection = true;
      Options.OnlyFunctionEntries = {Entry};
      auto Result = Pipeline().run(Image, Context, Options);
      ASSERT_EQ(Result.FunctionAudits.size(), 1U);
      const auto &Audit = Result.FunctionAudits[0];
      EXPECT_EQ(Audit.Disposition, PipelineFunctionDisposition::Accepted);
      EXPECT_TRUE(Audit.HasLLVMDefinition);
      EXPECT_TRUE(Audit.UnsupportedInstructions.empty());
      ASSERT_EQ(Result.LowFuncs.size(), 1U);
      unsigned Captures = 0, Steps = 0;
      for (const auto &Block : Result.LowFuncs[0].Blocks)
        for (const auto &Op : Block.Ops) {
          if (Op.Opcode != NdOp::INTRINSIC || !Op.NumInputs ||
              !Op.Inputs[0].isConst())
            continue;
          if (Op.Inputs[0].Offset == unsigned(Intrinsic::X87ContextBegin))
            ++Captures;
          if (Op.Addr == Entry &&
              Op.Inputs[0].Offset == unsigned(Intrinsic::X87ContextStep)) {
            EXPECT_EQ(Captures, 1U);
            EXPECT_EQ(Op.Inputs[2].Offset, 0xd9U | (Opcode << 8));
            ++Steps;
          }
        }
      EXPECT_EQ(Captures, 1U);
      EXPECT_EQ(Steps, 1U);
    }
}

TEST(X87SignContract, AValueInstructionKeepsOtherSlotsFull) {
  // fninit; fld1; fld1; <value op>; fstp st(0); fabs; fstp [rdx]; ret. The
  // value instruction defines st(0) alone, so the first load stays full.
  for (BinaryFormat Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
    for (uint8_t Value :
         {0xfeU, 0xf0U, 0xfdU, 0xf8U}) { // fsin f2xm1 fscale fprem
      auto Image =
          signControlFlowImage({0xdb, 0xe3, 0xd9, 0xe8, 0xd9, 0xe8, 0xd9, Value,
                                0xdd, 0xd8, 0xd9, 0xe1, 0xdb, 0x3a, 0xc3},
                               Format);
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      Options.LiftMode = true;
      Options.SourceProjection = true;
      Options.OnlyFunctionEntries = {Entry};
      auto Result = Pipeline().run(Image, Context, Options);
      ASSERT_EQ(Result.FunctionAudits.size(), 1U);
      const auto &Audit = Result.FunctionAudits[0];
      EXPECT_EQ(std::find(Audit.UnsupportedInstructions.begin(),
                          Audit.UnsupportedInstructions.end(), Entry + 10),
                Audit.UnsupportedInstructions.end())
          << unsigned(Value);
      EXPECT_EQ(Audit.Disposition, PipelineFunctionDisposition::Accepted)
          << unsigned(Value) << ':' << Result.Error;
    }
}

class X87SignFixture : public X86FPStateFixture {
protected:
  bool ObserveC1 = false;
  std::string comparisonDriver(bool, bool DeclareProbe, bool,
                               bool = false) override {
    std::string Text =
        ObserveC1 ? "#define OBSERVE_C1 1\n" : "#define OBSERVE_C1 0\n";
    Text += R"(
#include <stdint.h>
#include <stdio.h>
#include <string.h>
extern void native_probe(uintptr_t);
)";
    if (DeclareProbe)
      Text += "extern void isa_probe(uintptr_t);\n";
    Text += R"(
int main(void) {
  uint16_t saved;
  __asm__ volatile("fnstcw %0" : "=m"(saved));
  const uint64_t fractions[] = {
    UINT64_C(0x8000000000000000), UINT64_C(0x8000000000000001),
    UINT64_C(0x80000000000007ff), UINT64_C(0x8000000000000801),
    UINT64_C(0xc90fdaa22168c235), UINT64_C(0xffffffffffffffff)
  };
  const uint16_t exponents[] = {1, 0x3bcd, 0x3fff, 0x43ff, 0x7ffe};
  const struct { uint64_t fraction; uint16_t exponent; } special[] = {
    {0, 0}, {1, 0}, {UINT64_C(0x7fffffffffffffff), 0},
    {UINT64_C(0x8000000000000000), 0}, {0, 1},
    {UINT64_C(0x7fffffffffffffff), 0x3fff},
    {UINT64_C(0x8000000000000000), 0x7fff},
    {UINT64_C(0xc123456789abcdef), 0x7fff},
    {UINT64_C(0x8000000000000001), 0x7fff}, {1, 0x7fff}
  };
  unsigned comparisons = 0;
  for (unsigned rc = 0; rc < 4; ++rc)
    for (unsigned pc = 0; pc < 4; ++pc) {
      if (pc == 1) continue;
      const uint16_t control = 0x007f | (pc << 8) | (rc << 10);
      for (unsigned sign = 0; sign < 2; ++sign)
        for (unsigned e = 0; e <= sizeof(exponents)/sizeof(exponents[0]); ++e)
          for (unsigned f = 0; f < (e == 5 ? 10 : 6); ++f) {
            unsigned char expected[64], actual[64];
            memset(expected, 0xa5, sizeof(expected));
            memcpy(expected, &control, 2);
            memcpy(expected + 2, &saved, 2);
            const uint64_t fraction = e == 5 ? special[f].fraction : fractions[f];
            memcpy(expected + 8, &fraction, 8);
            const uint16_t exponent = (e == 5 ? special[f].exponent : exponents[e]) | (sign << 15);
            memcpy(expected + 16, &exponent, 2);
            memcpy(actual, expected, sizeof(actual));
            __asm__ volatile("fninit\n\tfldcw %0" :: "m"(control) : "memory");
            native_probe((uintptr_t)expected);
            if (OBSERVE_C1 && (expected[50] || expected[51])) {
              puts("original instruction did not clear C1");
              return 1;
            }
            __asm__ volatile("fninit\n\tfldcw %0" :: "m"(control) : "memory");
            isa_probe((uintptr_t)actual);
            __asm__ volatile("fninit\n\tfldcw %0" :: "m"(saved) : "memory");
            if (memcmp(actual, expected, sizeof(actual))) {
              printf("control=%04x sign=%u exp=%04x fraction=%llx expected=",
                     control, sign, exponent, (unsigned long long)fraction);
              for (unsigned i = 32; i < 42; ++i) printf("%02x", expected[i]);
              printf(" actual=");
              for (unsigned i = 32; i < 42; ++i) printf("%02x", actual[i]);
              puts("");
              return 1;
            }
            ++comparisons;
          }
    }
  printf("%u exact binary80 sign comparisons\n", comparisons);
  return 0;
}
)";
    return Text;
  }

  void check(std::initializer_list<uint8_t> Operations, bool CheckC1 = false) {
    ObserveC1 = CheckC1;
    const uint8_t Base = memoryModRM() & 7;
    std::vector<uint8_t> Bytes = {
        0xd9, uint8_t(0x28 | Base),   // fldcw [arg]
        0xdb, uint8_t(0x68 | Base), 8 // fld tbyte [arg+8]
    };
    if (CheckC1)
      Bytes.insert(Bytes.begin(),
                   {0xdb, 0xe3}); // fninit defines initial status
    if (CheckC1)
      Bytes.insert(Bytes.end(), {0xd9, 0xe5}); // fxam seeds C1 with source sign
    for (auto Opcode : Operations) {
      Bytes.push_back(0xd9);
      Bytes.push_back(Opcode);
    }
    if (CheckC1)
      Bytes.insert(Bytes.end(),
                   {
                       0xdf, 0xe0,             // fnstsw ax
                       0x66, 0x25, 0x00, 0x02, // and ax, 0x200: only defined C1
                       0x66, 0x89, uint8_t(0x40 | Base), 50 // mov [arg+50], ax
                   });
    Bytes.insert(Bytes.end(),
                 {0xdb, uint8_t(0x78 | Base), 32, // fstp tbyte [arg+32]
                  0xd9, uint8_t(0x78 | Base), 48, // fnstcw [arg+48]
                  0xd9, uint8_t(0x68 | Base), 2,  // fldcw [arg+2]
                  0xc3});
    compareFloating(Bytes, false);
  }
};

TEST_F(X87SignFixture, NegationPreservesAllSignificandAndExponentBits) {
  check({0xe0});
}
TEST_F(X87SignFixture, AbsoluteValuePreservesAllSignificandAndExponentBits) {
  check({0xe1});
}
TEST_F(X87SignFixture, RepeatedNegationAndAbsoluteValueRetainRawValues) {
  check({0xe0, 0xe0, 0xe1, 0xe0});
}
TEST_F(X87SignFixture, NegationClearsPreviouslySetC1) { check({0xe0}, true); }
TEST_F(X87SignFixture, AbsoluteValueClearsPreviouslySetC1) {
  check({0xe1}, true);
}
TEST_F(X87SignFixture, MixedSignChangesClearC1) {
  check({0xe0, 0xe0, 0xe1, 0xe0}, true);
}
} // namespace
