//===- X86_64_X87IntegerLoadTests.cpp - Exact integer loads -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compare FILD source widths and binary80 transports with original bytes.
//===----------------------------------------------------------------------===//

#include "X86FPStateAccuracyFixture.h"

#include "neverd/ir/X87Context.h"
#include "neverd/lift/X86Lifter.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/ScopeExit.h"

#include <optional>

namespace {
#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
std::array<uint8_t, 10> nativeIntegerLoad(uint64_t Value, unsigned Width) {
  std::array<uint8_t, 10> Result{};
  if (Width == 2)
    __asm__ volatile("filds %1\n\tfstpt %0"
                     : "=m"(Result)
                     : "m"(Value)
                     : "st", "memory");
  else if (Width == 4)
    __asm__ volatile("fildl %1\n\tfstpt %0"
                     : "=m"(Result)
                     : "m"(Value)
                     : "st", "memory");
  else
    __asm__ volatile("fildq %1\n\tfstpt %0"
                     : "=m"(Result)
                     : "m"(Value)
                     : "st", "memory");
  return Result;
}
#endif

TEST(X87IntegerLoadConcrete, AllWordEncodingsAndWideEdgesMatchNative) {
#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
  uint16_t Saved;
  __asm__ volatile("fnstcw %0" : "=m"(Saved));
  llvm::scope_exit Restore([&] {
    __asm__ volatile("fninit\n\tfldcw %0" ::"m"(Saved) : "memory");
  });
  __asm__ volatile("fninit" ::: "memory");
  BinaryImage Image;
  Image.Arch = Arch::X64;
  for (unsigned Width : {2U, 4U, 8U}) {
    std::vector<uint64_t> Inputs;
    if (Width == 2) {
      for (unsigned I = 0; I < 65536; ++I)
        Inputs.push_back(I);
    } else {
      Inputs = {0, 1, UINT64_MAX};
      for (unsigned Bit = 0; Bit < Width * 8; ++Bit) {
        const uint64_t Value = UINT64_C(1) << Bit;
        for (uint64_t V : {Value - 1, Value, Value + 1}) {
          Inputs.push_back(V);
          Inputs.push_back(uint64_t(0) - V);
        }
      }
    }
    for (uint64_t Value : Inputs) {
      const auto Expected = nativeIntegerLoad(Value, Width);
      for (bool Strict : {false, true})
        for (uint32_t MXCSR : {0U, 0x1f80U, 0xffffU}) {
          NdOpEmulator E(Image);
          E.setStrictMode(Strict);
          E.setMXCSR(MXCSR);
          E.setRegister(10001, Value);
          E.setRegisterBytes(10002, std::vector<uint8_t>(16, 0xa5));
          LowOp Op;
          Op.Opcode = NdOp::FLOAT_INT2FLOAT;
          Op.Output = NdVar::reg(10002, 10);
          Op.addInput(NdVar::reg(10001, Width));
          ASSERT_TRUE(E.step(Op)) << Width << ':' << Value << ':' << Strict;
          auto Padded = std::vector<uint8_t>(Expected.begin(), Expected.end());
          Padded.resize(16, 0xa5);
          EXPECT_EQ(E.getRegisterBytes(10002), Padded) << Width << ':' << Value;
          EXPECT_EQ(E.getMXCSR(), MXCSR);
          EXPECT_EQ(E.getRegister(x86reg::FPU_CW), 0x37fU);
          EXPECT_EQ(E.getRegister(x86reg::FPU_SW), 0U);
        }
    }
  }
#else
  GTEST_SKIP() << "native x64 GCC/Clang oracle required";
#endif
}

TEST(X87IntegerLoadConcrete, UnsignedPureConversionIsExactWithoutFPState) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  for (unsigned Width : {1U, 2U, 4U, 8U})
    for (uint64_t Value :
         {UINT64_C(0), UINT64_C(1), UINT64_MAX, UINT64_C(0x8000000000000000)}) {
      NdOpEmulator E(Image);
      E.setMXCSR(0);
      E.setRegister(x86reg::FPU_CW, 0);
      E.setRegister(x86reg::FPU_SW, 0x82);
      LowOp Op;
      Op.Opcode = NdOp::FLOAT_UINT2FLOAT;
      Op.Output = NdVar::tmp(10002, 10);
      Op.addInput(NdVar::cst(Value, Width));
      llvm::APFloat Reference(llvm::APFloat::x87DoubleExtended());
      ASSERT_EQ(Reference.convertFromAPInt(llvm::APInt(Width * 8, Value), false,
                                           llvm::APFloat::rmNearestTiesToEven),
                llvm::APFloat::opOK);
      ASSERT_TRUE(E.step(Op));
      auto Bits = Reference.bitcastToAPInt();
      std::vector<uint8_t> Expected(10);
      for (unsigned I = 0; I < 10; ++I)
        Expected[I] = Bits.extractBitsAsZExtValue(8, I * 8);
      EXPECT_EQ(E.getRegisterBytes(10002), Expected);
      EXPECT_EQ(E.getMXCSR(), 0U);
      EXPECT_EQ(E.getRegister(x86reg::FPU_CW), 0U);
      EXPECT_EQ(E.getRegister(x86reg::FPU_SW), 0x82U);
    }
}

TEST(X87IntegerLoadContract, MalformedAndUnknownPureInputsRefuseAtomically) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  for (bool Strict : {false, true})
    for (NdOp Code : {NdOp::FLOAT_INT2FLOAT, NdOp::FLOAT_UINT2FLOAT})
      for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
        NdOpEmulator E(Image);
        E.setStrictMode(Strict);
        E.setMXCSR(0xffbf);
        E.setRegisterBytes(10002, std::vector<uint8_t>(16, 0xa5));
        LowOp Op;
        Op.Opcode = Code;
        Op.Output = NdVar::reg(10002, 10);
        Op.addInput(NdVar::reg(10001, 8));
        if (Mutation)
          E.setRegisterBytes(10001, std::vector<uint8_t>(8, 0xb3));
        switch (Mutation) {
        case 1:
          Op.NumInputs = 0;
          break;
        case 2:
          Op.addInput(NdVar::cst(0, 1));
          break;
        case 3:
          Op.Inputs[0].Size = 3;
          break;
        case 4:
          Op.Inputs[0].Size = 16;
          break;
        case 5:
          Op.Inputs[0].Space = VnodeSpace::RAM;
          break;
        case 6:
          Op.Output.Space = VnodeSpace::CONST;
          break;
        case 7:
          E.setRegisterBytes(10001, std::vector<uint8_t>(4, 0xb3));
          break;
        default:
          break;
        }
        EXPECT_FALSE(E.step(Op)) << Mutation << ':' << Strict;
        EXPECT_EQ(E.getRegisterBytes(10002), std::vector<uint8_t>(16, 0xa5));
        EXPECT_EQ(E.getMXCSR(), 0xffbfU);
      }
}

TEST(X87IntegerLoadContract, WaitPrecedesMemoryAndStopsPendingExceptions) {
  // Typed values wait before the load; an owned context's step waits itself.
  for (bool Owned : {false, true})
    for (unsigned Width : {2U, 4U, 8U}) {
      Decoder D;
      ASSERT_TRUE(D.init(Arch::X64));
      D.setX87ContextOwnership(Owned);
      BinaryImage Image;
      Image.Arch = Arch::X64;
      const uint8_t Bytes[] = {uint8_t(Width == 4 ? 0xdb : 0xdf),
                               uint8_t(Width == 8 ? 0x29 : 0x01)};
      DecodedInsn I;
      ASSERT_EQ(D.decodeOneForLift(Bytes, sizeof(Bytes), Entry, I), 2);
      std::vector<LowOp> Ops;
      ASSERT_NO_THROW(D.liftToLow(I, Ops));
      ASSERT_FALSE(Ops.empty());
      if (Owned) {
        ASSERT_EQ(Ops.back().Opcode, NdOp::INTRINSIC);
        ASSERT_EQ(Ops.back().Inputs[0].Offset,
                  unsigned(Intrinsic::X87ContextStep));
        ASSERT_TRUE(x87ContextInstruction(Ops.back().Inputs[2].Offset));
        EXPECT_TRUE(
            x87ContextInstruction(Ops.back().Inputs[2].Offset)->Waiting);
      } else {
        ASSERT_EQ(Ops.front().Opcode, NdOp::INTRINSIC);
        ASSERT_EQ(Ops.front().Inputs[0].Offset, unsigned(Intrinsic::X87Wait));
      }
      for (bool Unknown : {false, true}) {
        NdOpEmulator E(Image);
        E.setStrictMode(true);
        E.setLoadCollect(true);
        if (Unknown) {
          E.setCallPreservedRegisters({});
          LowOp Call;
          Call.Opcode = NdOp::CALL;
          Call.addInput(NdVar::cst(Entry, 8));
          ASSERT_TRUE(E.step(Call));
        } else {
          E.setRegister(x86reg::FPU_CW, 0x37e);
          E.setRegister(x86reg::FPU_SW, 0x281);
        }
        E.setRegisterBytes(x86reg::ST7, std::vector<uint8_t>(16, 0xa5));
        if (Owned)
          EXPECT_LT(E.run(Ops), Ops.size());
        else
          EXPECT_EQ(E.run(Ops), 0U);
        EXPECT_TRUE(E.getLoadRecords().empty());
        EXPECT_EQ(E.getRegisterBytes(x86reg::ST7),
                  std::vector<uint8_t>(16, 0xa5));
      }
    }
}

class X87IntegerLoadAdmission : public X86FPStateFixture {};
TEST_F(X87IntegerLoadAdmission, DestinationAdmissionFollowsTheX87Model) {
  for (BinaryFormat Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
    for (bool NoOpt : {false, true})
      for (unsigned Scenario = 0; Scenario < 15; ++Scenario) {
        // An owned context (Win64) captures the actual incoming stack, so it
        // needs no proof. Typed values (System V) admit FILD only where its
        // destination is proven empty on every path. Scenarios 1, 9 and 10
        // start at the function boundary, whose calling convention empties
        // the register stack, as compiled FILD relies on.
        const bool Owned = x87ContextOwnsStack(Arch::X64, Format, std::nullopt);
        const bool Accepted = Owned || Scenario == 0 || Scenario == 1 ||
                              Scenario == 4 || Scenario == 5 || Scenario == 6 ||
                              Scenario == 7 || Scenario == 10 || Scenario == 13;
        std::vector<uint8_t> Bytes;
        if (Scenario != 1 && Scenario != 9 && Scenario != 10)
          Bytes = {0xdb, 0xe3}; // fninit authenticates empty tags
        if (Scenario == 2 || Scenario == 3 || Scenario == 9 || Scenario == 11 ||
            Scenario == 12)
          for (unsigned I = 0; I < 8; ++I)
            Bytes.insert(Bytes.end(), {0xd9, 0xe8}); // fill all slots
        if (Scenario == 3)
          Bytes.insert(Bytes.end(), {0x85, 0xc9, 0x74, 0x02, 0xdd, 0xd8});
        if (Scenario == 4)
          Bytes.insert(Bytes.end(), {0xd9, 0xe8, 0xdd, 0xd8}); // pop frees slot
        // An unknown callee also returns under its calling convention: the
        // empty slots stay empty (5), the full ones become unknown (12). A
        // context exports and refreshes either stack.
        if (Scenario == 5 || Scenario == 12)
          Bytes.insert(Bytes.end(), {0xff, 0xd0}); // call rax
        if (Scenario == 6)
          Bytes.insert(Bytes.end(),
                       {0x85, 0xc9, 0x74, 0x02, 0xd9, 0xe8}); // two TOPs
        // Only a state restore replaces tags, not an unrelated intrinsic.
        if (Scenario == 13)
          Bytes.insert(Bytes.end(), {0x0f, 0xa2}); // cpuid
        if (Scenario == 14)
          Bytes.insert(Bytes.end(), {0xdd, 0x22}); // frstor [rdx]
        // So does a direct call to a callee whose x87 effect is proven (10,
        // 11), and a context crosses it whole.
        std::optional<size_t> CallAt;
        if (Scenario == 10 || Scenario == 11) {
          CallAt = Bytes.size();
          Bytes.insert(Bytes.end(), {0xe8, 0, 0, 0, 0});
        }
        const va_t Address = Entry + Bytes.size();
        Bytes.insert(Bytes.end(), {0xdf, 0x2a});
        if (Scenario == 7 || Scenario == 8) {
          if (Scenario == 7)
            Bytes.insert(Bytes.end(), {0xdb, 0x3a}); // balanced loop frees slot
          Bytes.insert(Bytes.end(), {0xff, 0xc9, 0x75,
                                     uint8_t(Scenario == 7 ? 0xf8 : 0xfa)});
        } else {
          Bytes.insert(Bytes.end(), {0xdb, 0x3a});
        }
        Bytes.push_back(0xc3);
        if (CallAt) {
          // The callee is a bare return after the function.
          const uint32_t Displacement = uint32_t(Bytes.size() - (*CallAt + 5));
          for (unsigned I = 0; I < 4; ++I)
            Bytes[*CallAt + 1 + I] = uint8_t(Displacement >> (8 * I));
          Bytes.push_back(0xc3);
        }
        auto Image = image(Bytes, Format);
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
        EXPECT_EQ(Audit.HasLLVMDefinition, Accepted)
            << Scenario << ':' << NoOpt;
        EXPECT_EQ(Audit.Disposition,
                  Accepted ? PipelineFunctionDisposition::Accepted
                           : PipelineFunctionDisposition::RejectedIncomplete)
            << Scenario << ':' << NoOpt << ':' << Result.Error;
        if (!Accepted)
          EXPECT_NE(std::find(Audit.UnsupportedInstructions.begin(),
                              Audit.UnsupportedInstructions.end(), Address),
                    Audit.UnsupportedInstructions.end())
              << Scenario;
        if (!Owned)
          continue;
        EXPECT_TRUE(Audit.UnsupportedInstructions.empty());
        ASSERT_EQ(Result.LowFuncs.size(), 1U);
        unsigned Steps = 0;
        for (const auto &Block : Result.LowFuncs[0].Blocks)
          for (const auto &Op : Block.Ops)
            if (Op.Addr == Address && Op.Opcode == NdOp::INTRINSIC &&
                Op.Inputs[0].Offset == unsigned(Intrinsic::X87ContextStep)) {
              ++Steps;
              EXPECT_EQ(Op.Inputs[2].Offset & 0x1ffffu,
                        0xdfu | (0x28u << 8) | X87ContextMemory);
              EXPECT_TRUE(Op.Inputs[1].isTemp());
            }
        EXPECT_EQ(Steps, 1u);
      }
}

TEST(X87IntegerLoadContract, InvalidOperandMetadataDoesNotPushOrRead) {
  Decoder D;
  ASSERT_TRUE(D.init(Arch::X64));
  const uint8_t Bytes[] = {0xdf, 0x29};
  DecodedInsn I;
  ASSERT_EQ(D.decodeOneForLift(Bytes, sizeof(Bytes), Entry, I), 2);
  for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
    auto Raw = *I.Raw;
    auto Detail = *I.Raw->detail;
    Raw.detail = &Detail;
    switch (Mutation) {
    case 0:
      Detail.x86.op_count = 0;
      break;
    case 1:
      Detail.x86.op_count = 2;
      break;
    case 2:
      Detail.x86.operands[0].type = X86_OP_REG;
      break;
    case 3:
      Detail.x86.operands[0].size = 3;
      break;
    case 4:
      Detail.x86.operands[0].size = 16;
      break;
    }
    X86Lifter L(Arch::X64);
    std::vector<LowOp> Ops;
    EXPECT_THROW(L.lift(&Raw, Ops), UnliftedInstruction);
    EXPECT_TRUE(Ops.empty());
    EXPECT_EQ(L.getFpuTop(), 0);
  }
}

TEST(X87IntegerLoadContract, MemoryLoadsRequireOnlyTheExactSourceExtent) {
  Decoder D;
  ASSERT_TRUE(D.init(Arch::X64));
  for (unsigned Width : {2U, 4U, 8U})
    for (unsigned Alignment : {0U, 1U})
      for (bool Complete : {false, true}) {
        const uint8_t Bytes[] = {uint8_t(Width == 4 ? 0xdb : 0xdf),
                                 uint8_t(Width == 8 ? 0x29 : 0x01)};
        DecodedInsn I;
        ASSERT_EQ(D.decodeOneForLift(Bytes, sizeof(Bytes), Entry, I), 2);
        std::vector<LowOp> Ops;
        ASSERT_NO_THROW(D.liftToLow(I, Ops));
        ASSERT_EQ(Ops.back().Opcode, NdOp::FLOAT_INT2FLOAT);
        const NdVar Destination = Ops.back().Output;
        BinaryImage Image;
        Image.Arch = Arch::X64;
        const uint64_t Value = UINT64_C(0xfedcba9876543210);
        Segment Data;
        Data.VA = 0x2000 + Alignment;
        Data.Flags = SegmentFlags::Readable;
        Data.Data.resize(Width - unsigned(!Complete));
        std::memcpy(Data.Data.data(), &Value, Data.Data.size());
        Data.Size = Data.FileSz = Data.Data.size();
        Image.Segments.push_back(Data);
        NdOpEmulator E(Image);
        E.setStrictMode(true);
        E.setRegister(x86reg::RCX, Data.VA);
        E.setRegisterBytes(Destination.Offset, std::vector<uint8_t>(16, 0xa5));
        if (!Complete) {
          EXPECT_LT(E.run(Ops), Ops.size());
          EXPECT_EQ(E.getRegisterBytes(Destination.Offset),
                    std::vector<uint8_t>(16, 0xa5));
        } else {
          EXPECT_EQ(E.run(Ops), Ops.size());
          llvm::APFloat Reference(llvm::APFloat::x87DoubleExtended());
          ASSERT_EQ(
              Reference.convertFromAPInt(llvm::APInt(Width * 8, Value), true,
                                         llvm::APFloat::rmNearestTiesToEven),
              llvm::APFloat::opOK);
          std::vector<uint8_t> Expected(16, 0xa5);
          const auto Bits = Reference.bitcastToAPInt();
          for (unsigned N = 0; N < 10; ++N)
            Expected[N] = Bits.extractBitsAsZExtValue(8, N * 8);
          EXPECT_EQ(E.getRegisterBytes(Destination.Offset), Expected);
        }
      }
}

TEST(X87IntegerLoadContract, OwnedContextLoadsRequireOnlyTheExactSourceExtent) {
  Decoder D;
  ASSERT_TRUE(D.init(Arch::X64));
  D.setX87ContextOwnership(true);
  for (unsigned Width : {2U, 4U, 8U})
    for (unsigned Alignment : {0U, 1U})
      for (bool Complete : {false, true}) {
        const uint8_t Bytes[] = {uint8_t(Width == 4 ? 0xdb : 0xdf),
                                 uint8_t(Width == 8 ? 0x29 : 0x01)};
        DecodedInsn I;
        ASSERT_EQ(D.decodeOneForLift(Bytes, sizeof(Bytes), Entry, I), 2);
        std::vector<LowOp> Ops;
        ASSERT_NO_THROW(D.liftToLow(I, Ops));
        ASSERT_EQ(Ops.back().Opcode, NdOp::INTRINSIC);
        ASSERT_EQ(Ops.back().Inputs[0].Offset,
                  unsigned(Intrinsic::X87ContextStep));
        const NdVar Destination = NdVar::reg(x86reg::ST7, 10);
        BinaryImage Image;
        Image.Arch = Arch::X64;
        const uint64_t Value = UINT64_C(0xfedcba9876543210);
        Segment Data;
        Data.VA = 0x2000 + Alignment;
        Data.Flags = SegmentFlags::Readable;
        Data.Data.resize(Width - unsigned(!Complete));
        std::memcpy(Data.Data.data(), &Value, Data.Data.size());
        Data.Size = Data.FileSz = Data.Data.size();
        Image.Segments.push_back(Data);
        NdOpEmulator E(Image);
#if !(defined(__x86_64__) || defined(_M_X64)) ||                               \
    !(defined(__clang__) || defined(__GNUC__))
        // Keep this host-independent contract runnable through the explicit
        // portable slice; native original-instruction tests remain separate.
        E.setX87PortableMode(true);
#endif
        E.setStrictMode(true);
        ASSERT_TRUE(E.setX86LinearAddressBits(48));
        E.setRegister(x86reg::RCX, Data.VA);
        std::array<uint8_t, 512> State{};
        State[0] = 0x7f;
        State[1] = 3;
        std::fill_n(State.begin() + 144, 16, 0xa5);
        ASSERT_TRUE(E.setX87Context(State));
        const auto Before = E.getX87Context();
        if (!Complete) {
          EXPECT_LT(E.run(Ops), Ops.size());
          EXPECT_EQ(E.getRegisterBytes(Destination.Offset),
                    std::vector<uint8_t>(10, 0xa5));
          EXPECT_EQ(E.getX87Context(), Before);
        } else {
          EXPECT_EQ(E.run(Ops), Ops.size());
          llvm::APFloat Reference(llvm::APFloat::x87DoubleExtended());
          ASSERT_EQ(
              Reference.convertFromAPInt(llvm::APInt(Width * 8, Value), true,
                                         llvm::APFloat::rmNearestTiesToEven),
              llvm::APFloat::opOK);
          std::vector<uint8_t> Expected(10);
          const auto Bits = Reference.bitcastToAPInt();
          for (unsigned N = 0; N < 10; ++N)
            Expected[N] = Bits.extractBitsAsZExtValue(8, N * 8);
          EXPECT_EQ(E.getRegisterBytes(Destination.Offset), Expected);
          ASSERT_TRUE(E.getX87Context());
          EXPECT_EQ((*E.getX87Context())[4], 0x80);
          EXPECT_EQ((*E.getRegister(x86reg::FPU_SW) >> 11) & 7, 7u);
        }
      }
}

class X87IntegerLoadFixture : public X86FPStateFixture {
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
  const uint64_t inputs[] = {
    0, 1, UINT64_MAX, 2, UINT64_MAX-1,
    UINT64_C(0x7fff), UINT64_C(0x8000), UINT64_C(0xffff),
    UINT64_C(0xffffff), UINT64_C(0x1000001),
    UINT64_C(0x7fffffff), UINT64_C(0x80000000),
    UINT64_C(0x1fffffffffffff), UINT64_C(0x20000000000001),
    UINT64_C(0x7fffffffffffffff), UINT64_C(0x8000000000000000),
    UINT64_C(0x8000000000000001), UINT64_C(0xfedcba9876543210)
  };
  unsigned comparisons = 0;
  for (unsigned rc = 0; rc < 4; ++rc)
    for (unsigned pc = 0; pc < 4; ++pc) {
      if (pc == 1) continue;
      for (unsigned masks = 0; masks < 64; ++masks)
        for (unsigned i = 0; i < sizeof(inputs)/sizeof(inputs[0]); ++i) {
          const uint16_t control = 0x0040 | (pc << 8) | (rc << 10) | masks;
          unsigned char expected[128], actual[128];
          memset(expected, 0xa5, sizeof(expected));
          memcpy(expected, &control, 2);
          memcpy(expected + 2, &saved, 2);
          memcpy(expected + 8, &inputs[i], 8);
          const uint64_t one = UINT64_C(0x8000000000000000);
          const uint16_t negative = 0xbfff;
          memcpy(expected + 80, &one, 8);
          memcpy(expected + 88, &negative, 2);
          memcpy(actual, expected, sizeof(actual));
          __asm__ volatile("fninit" ::: "memory");
          native_probe((uintptr_t)expected);
          if (OBSERVE_C1 && (expected[50] || expected[51])) {
            puts("original FILD did not clear C1"); return 1;
          }
          __asm__ volatile("fninit" ::: "memory");
          isa_probe((uintptr_t)actual);
          __asm__ volatile("fninit\n\tfldcw %0" :: "m"(saved) : "memory");
          if (memcmp(expected, actual, sizeof(actual))) {
            printf("cw=%04x input=%llx expected=", control,
                   (unsigned long long)inputs[i]);
            for (unsigned j=32;j<42;++j) printf("%02x", expected[j]);
            printf(" actual=");
            for (unsigned j=32;j<42;++j) printf("%02x", actual[j]);
            printf(" C1=%02x/%02x\n",expected[51],actual[51]);
            return 1;
          }
          ++comparisons;
        }
    }
  printf("%u exact FILD binary80/control comparisons\n", comparisons);
  return 0;
}
)";
    return Text;
  }

  void check(unsigned Width, bool C1 = false) {
    ObserveC1 = C1;
    const uint8_t Base = memoryModRM() & 7;
    std::vector<uint8_t> Bytes = {
        0xdb, 0xe3,                // fninit: source stack is known empty
        0xd9, uint8_t(0x28 | Base) // fldcw [arg]
    };
    if (C1)
      Bytes.insert(Bytes.end(), {0xdb, uint8_t(0x68 | Base), 80, 0xd9, 0xe5});
    Bytes.insert(Bytes.end(), {uint8_t(Width == 4 ? 0xdb : 0xdf),
                               uint8_t((Width == 8 ? 0x68 : 0x40) | Base), 8});
    if (C1)
      Bytes.insert(Bytes.end(), {0xdf, 0xe0, 0x66, 0x25, 0x00, 0x02, 0x66, 0x89,
                                 uint8_t(0x40 | Base), 50});
    Bytes.insert(Bytes.end(), {0xdb, uint8_t(0x78 | Base), 32});
    if (C1)
      Bytes.insert(Bytes.end(), {0xdd, 0xd8}); // pop retained negative value
    Bytes.insert(Bytes.end(), {0xd9, uint8_t(0x78 | Base), 48, 0xd9,
                               uint8_t(0x68 | Base), 2, 0xc3});
    compareFloating(Bytes, false);
  }
};

class X87IntegerLoads : public X87IntegerLoadFixture,
                        public testing::WithParamInterface<unsigned> {};
TEST_P(X87IntegerLoads, ExactBinary80AndControl) { check(GetParam()); }
TEST_P(X87IntegerLoads, ClearsC1WithoutRounding) { check(GetParam(), true); }
INSTANTIATE_TEST_SUITE_P(AllWidths, X87IntegerLoads,
                         testing::Values(2U, 4U, 8U));
} // namespace
