//===- X86SystemStateTests.cpp - System completion regression tests -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Strict decode, explicit operands, conditional destinations and refusal to
/// invent hardware answers. No privileged host instruction is executed.
//===----------------------------------------------------------------------===//

#include "X86SystemStateFixtures.h"
#include "gtest/gtest.h"

#include "neverd/backend/llvm/X86/X86SystemStateAsm.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86/X86Lifter.h"
#include "neverd/lift/X86/X86Regs.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <string>
#include <utility>
#include <vector>

using namespace neverd;

namespace {

std::vector<LowOp> lift(Arch A, const std::vector<uint8_t> &Bytes) {
  Decoder D;
  EXPECT_TRUE(D.init(A));
  DecodedInsn I{};
  EXPECT_EQ(D.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, I),
            Bytes.size());
  std::vector<LowOp> Ops;
  try {
    D.liftToLow(I, Ops);
  } catch (const std::exception &E) {
    ADD_FAILURE() << E.what();
  }
  return Ops;
}

std::vector<std::vector<uint8_t>> transactionPrefixes(Arch A) {
  std::vector<std::vector<uint8_t>> Prefixes = {
      {}, {0x66}, {0x67}, {0x64}, {0x2e}, {0x66, 0x67}, {0xf2}, {0xf3}};
  if (A == Arch::X64)
    Prefixes.insert(Prefixes.end(), {{0x40}, {0x48}, {0x66, 0x48}});
  return Prefixes;
}

TEST(X86SystemState, InstructionsHaveValidatedEffectsAndNoInventedResult) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto &E : test::systemEncodings(A)) {
      SCOPED_TRACE(E.Mnemonic);
      SCOPED_TRACE(static_cast<unsigned>(A));
      const auto Ops = lift(A, E.Bytes);
      ASSERT_FALSE(Ops.empty());
      unsigned Count = 0;
      for (const auto &Op : Ops) {
        EXPECT_NE(Op.Opcode, NdOp::NOP);
        if (Op.Opcode != NdOp::INTRINSIC)
          continue;
        ++Count;
        EXPECT_EQ(Op.Inputs[0].Offset, static_cast<uint64_t>(E.Id));
        EXPECT_TRUE(
            x86SystemStateShapeIsValid(E.Id, x86SystemStateLowShape(Op, A)));
        EXPECT_TRUE(isSideeffectIntrinsic(E.Id));
        EXPECT_STREQ(intrinsicAsmMnemonic(E.Id), E.Mnemonic);
        EXPECT_NE(intrinsicCName(E.Id), nullptr);
        BinaryImage Image;
        Image.Arch = A;
        NdOpEmulator Emu(Image);
        Emu.setRegister(x86reg::RAX, 0xabcdef0123456789);
        EXPECT_FALSE(Emu.step(Op));
        EXPECT_EQ(Emu.getRegister(x86reg::RAX), 0xabcdef0123456789);
        auto Bad = Op;
        Bad.Output.Size = 3;
        EXPECT_FALSE(
            x86SystemStateShapeIsValid(E.Id, x86SystemStateLowShape(Bad, A)));
      }
      EXPECT_EQ(Count, 1u);
    }
}

TEST(X86SystemState, TransactionAbortRetainsEveryEncodedByte) {
  for (Arch A : {Arch::X86, Arch::X64}) {
    Decoder D;
    ASSERT_TRUE(D.init(A));
    D.setStrict(true);
    for (const auto &Prefix : transactionPrefixes(A))
      for (unsigned Code = 0; Code != 256; ++Code)
        for (unsigned Route : {0u, 1u, 2u}) {
          SCOPED_TRACE(static_cast<unsigned>(A));
          SCOPED_TRACE(testing::PrintToString(Prefix));
          SCOPED_TRACE(Code);
          SCOPED_TRACE(Route);
          auto Bytes = Prefix;
          Bytes.insert(Bytes.end(), {0xc6, 0xf8, static_cast<uint8_t>(Code)});
          DecodedInsn Instruction{};
          const int Size =
              Route == 0
                  ? D.decodeOne(Bytes.data(), Bytes.size(), 0x1000, Instruction)
              : Route == 1 ? D.decodeOneLight(Bytes.data(), Bytes.size(),
                                              0x1000, Instruction)
                           : D.decodeOneForLift(Bytes.data(), Bytes.size(),
                                                0x1000, Instruction);
          ASSERT_EQ(Size, Bytes.size());
          std::vector<LowOp> Ops;
          ASSERT_NO_THROW(D.liftToLow(Instruction, Ops));
          ASSERT_EQ(Ops.size(), 1u);
          const auto &Op = Ops.front();
          ASSERT_EQ(Op.Opcode, NdOp::INTRINSIC);
          ASSERT_EQ(Op.NumInputs, 2u);
          EXPECT_EQ(Op.Output.Size, 0);
          EXPECT_EQ(Op.Inputs[0].Offset,
                    static_cast<uint64_t>(Intrinsic::Xabort));
          EXPECT_EQ(Op.Inputs[1].Size, 1);
          EXPECT_EQ(Op.Inputs[1].Offset, Code);
          const auto Shape = x86SystemStateLowShape(Op, A);
          EXPECT_TRUE(x86SystemStateShapeIsValid(Intrinsic::Xabort, Shape));
          EXPECT_EQ(Shape.TransactionAbortCode, Code);
        }
  }
}

TEST(X86SystemState, TransactionCompletionEncodingsAreValidated) {
  for (Arch A : {Arch::X86, Arch::X64}) {
    Decoder D;
    ASSERT_TRUE(D.init(A));
    D.setStrict(true);
    for (uint8_t ModRM : {0xd5, 0xd6}) {
      const auto Id = ModRM == 0xd5 ? Intrinsic::Xend : Intrinsic::Xtest;
      for (const auto &Prefix : transactionPrefixes(A))
        for (unsigned Route : {0u, 1u, 2u}) {
          SCOPED_TRACE(static_cast<unsigned>(A));
          SCOPED_TRACE(ModRM);
          SCOPED_TRACE(testing::PrintToString(Prefix));
          SCOPED_TRACE(Route);
          auto Bytes = Prefix;
          Bytes.insert(Bytes.end(), {0x0f, 0x01, ModRM});
          DecodedInsn Instruction{};
          const int Size =
              Route == 0
                  ? D.decodeOne(Bytes.data(), Bytes.size(), 0x1000, Instruction)
              : Route == 1 ? D.decodeOneLight(Bytes.data(), Bytes.size(),
                                              0x1000, Instruction)
                           : D.decodeOneForLift(Bytes.data(), Bytes.size(),
                                                0x1000, Instruction);
          ASSERT_EQ(Size, Bytes.size());
          std::vector<LowOp> Ops;
          ASSERT_NO_THROW(D.liftToLow(Instruction, Ops));
          ASSERT_FALSE(Ops.empty());
          EXPECT_EQ(Ops.front().Inputs[0].Offset, static_cast<uint64_t>(Id));
        }
      const std::vector<uint8_t> Bytes{0x0f, 0x01, ModRM};
      DecodedInsn Instruction{};
      ASSERT_EQ(
          D.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Instruction),
          Bytes.size());
      for (bool Strict : {false, true})
        for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
          SCOPED_TRACE(ModRM);
          SCOPED_TRACE(Mutation);
          auto Raw = *Instruction.Raw;
          auto Detail = *Raw.detail;
          Raw.detail = &Detail;
          switch (Mutation) {
          case 0:
            Detail.x86.op_count = 1;
            break;
          case 1:
            Detail.x86.encoding.imm_size = 1;
            break;
          case 2:
            Detail.x86.encoding.imm_offset = 2;
            break;
          case 3:
            Detail.x86.encoding.modrm_offset = 1;
            break;
          case 4:
            Detail.x86.modrm ^= 1;
            break;
          case 5:
            Detail.x86.opcode[1] = 0;
            break;
          case 6:
            Raw.bytes[2] ^= 1;
            break;
          case 7:
            Raw.size = 2;
            break;
          case 8:
            Detail.x86.encoding.disp_size = 1;
            break;
          case 9:
            Raw.size = 4;
            Raw.bytes[0] = 0xf0;
            Raw.bytes[1] = 0x0f;
            Raw.bytes[2] = 0x01;
            Raw.bytes[3] = ModRM;
            Detail.x86.prefix[0] = 0xf0;
            Detail.x86.encoding.modrm_offset = 3;
            break;
          }
          X86Lifter L(A);
          L.setStrict(Strict);
          std::vector<LowOp> Ops;
          EXPECT_THROW(L.lift(&Raw, Ops), UnliftedInstruction);
          EXPECT_TRUE(Ops.empty());
        }
    }
  }
}

TEST(X86SystemState, TransactionAbortRejectsMissingAndForgedImmediates) {
  for (Arch A : {Arch::X86, Arch::X64}) {
    Decoder D;
    ASSERT_TRUE(D.init(A));
    const std::vector<uint8_t> Bytes{0xc6, 0xf8, 0x80};
    DecodedInsn Instruction{};
    ASSERT_EQ(
        D.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Instruction),
        Bytes.size());
    for (bool Strict : {false, true})
      for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
        SCOPED_TRACE(Mutation);
        auto Raw = *Instruction.Raw;
        auto Detail = *Raw.detail;
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
          Detail.x86.operands[0].imm = 0x180;
          break;
        case 4:
          Detail.x86.encoding.imm_size = 0;
          break;
        case 5:
          Detail.x86.encoding.imm_offset = 0;
          break;
        case 6:
          Raw.bytes[Raw.size - 1] ^= 1;
          break;
        case 7:
          Detail.x86.operands[0].size = 3;
          break;
        case 8:
          Detail.x86.modrm = 0;
          break;
        }
        X86Lifter L(A);
        L.setStrict(Strict);
        std::vector<LowOp> Ops;
        EXPECT_THROW(L.lift(&Raw, Ops), UnliftedInstruction);
        EXPECT_TRUE(Ops.empty());
      }
    const auto Ops = lift(A, Bytes);
    ASSERT_EQ(Ops.size(), 1u);
    for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
      auto Bad = Ops.front();
      switch (Mutation) {
      case 0:
        Bad.NumInputs = 1;
        break;
      case 1:
        Bad.Inputs[1] = NdVar::reg(x86reg::RAX, 1);
        break;
      case 2:
        Bad.Inputs[1].Size = 4;
        break;
      case 3:
        Bad.Inputs[1].Offset = 256;
        break;
      case 4:
        Bad.Output = NdVar::reg(x86reg::RAX, 1);
        break;
      case 5:
        Bad.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
        break;
      case 6:
        Bad.MemoryOrdering = NdMemoryOrdering::Acquire;
        break;
      }
      EXPECT_FALSE(x86SystemStateShapeIsValid(Intrinsic::Xabort,
                                              x86SystemStateLowShape(Bad, A)));
    }
    BinaryImage Image;
    Image.Arch = A;
    NdOpEmulator Emu(Image);
    Emu.setStrictMode(true);
    Emu.setRegister(x86reg::RAX, 0x12345678);
    Emu.setRegister(x86reg::ZF, 1);
    EXPECT_FALSE(Emu.step(Ops.front()));
    EXPECT_EQ(Emu.getRegister(x86reg::RAX), 0x12345678u);
    EXPECT_EQ(Emu.getRegister(x86reg::ZF), 1u);
  }
}

TEST(X86SystemState, TransactionQueryDefinesAllArithmeticFlags) {
  const std::vector<uint64_t> Flags = {x86reg::CF, x86reg::PF, x86reg::AF,
                                       x86reg::ZF, x86reg::SF, x86reg::OF};
  for (Arch A : {Arch::X86, Arch::X64}) {
    const auto Ops = lift(A, {0x0f, 0x01, 0xd6});
    ASSERT_FALSE(Ops.empty());
    ASSERT_EQ(Ops.front().Opcode, NdOp::INTRINSIC);
    EXPECT_EQ(Ops.front().Inputs[0].Offset,
              static_cast<uint64_t>(Intrinsic::Xtest));
    EXPECT_EQ(Ops.front().NumInputs, 1u);
    for (unsigned Initial = 0; Initial != 64; ++Initial)
      for (unsigned Active : {0u, 1u}) {
        SCOPED_TRACE(static_cast<unsigned>(A));
        SCOPED_TRACE(Initial);
        SCOPED_TRACE(Active);
        BinaryImage Image;
        Image.Arch = A;
        NdOpEmulator Emu(Image);
        Emu.setStrictMode(true);
        for (unsigned I = 0; I != Flags.size(); ++I)
          Emu.setRegister(Flags[I], (Initial >> I) & 1);
        Emu.setRegister(x86reg::DF, 1);
        Emu.setRegister(x86reg::RAX, 0x1122334455667788);
        // The concrete executor cannot invent ambient transaction state.
        EXPECT_FALSE(Emu.step(Ops.front()));
        for (unsigned I = 0; I != Flags.size(); ++I)
          EXPECT_EQ(Emu.getRegister(Flags[I]), (Initial >> I) & 1);
        // Supply each architectural query completion, then execute the real
        // lifted flag updates. No RTM/HLE host capability is assumed.
        LowOp Completion;
        Completion.Opcode = NdOp::COPY;
        Completion.Output = Ops.front().Output;
        Completion.addInput(NdVar::cst(Active, Completion.Output.Size));
        ASSERT_TRUE(Emu.step(Completion));
        for (size_t I = 1; I < Ops.size(); ++I)
          ASSERT_TRUE(Emu.step(Ops[I]));
        for (uint64_t Flag : Flags)
          EXPECT_EQ(Emu.getRegister(Flag), Flag == x86reg::ZF ? !Active : 0u);
        EXPECT_EQ(Emu.getRegister(x86reg::DF), 1u);
        EXPECT_EQ(Emu.getRegister(x86reg::RAX), 0x1122334455667788u);
      }
  }
}

TEST(X86SystemState, CacheZeroReadsTheAddressWithoutLoadingOrChangingFlags) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto &E : test::cacheZeroEncodings()) {
      const auto Ops = lift(A, E.Bytes);
      ASSERT_FALSE(Ops.empty());
      const auto &Last = Ops.back();
      ASSERT_EQ(Last.Opcode, NdOp::INTRINSIC);
      const auto Shape = x86SystemStateLowShape(Last, A);
      ASSERT_TRUE(x86SystemStateShapeIsValid(Intrinsic::Clzero, Shape));
      ASSERT_EQ(Last.NumInputs, 2u);
      EXPECT_EQ(Last.Output.Size, 0);
      EXPECT_EQ(Last.Inputs[1].Size, 8);
      const bool Narrow =
          std::find(E.Bytes.begin(), E.Bytes.end(), 0x67) != E.Bytes.end();
      const unsigned Width =
          A == Arch::X64 ? (Narrow ? 4 : 8) : (Narrow ? 2 : 4);
      const uint64_t Input = 0x11223344ffeecdbb;
      const uint64_t Expected =
          Width == 8 ? Input : Input & ((uint64_t{1} << (8 * Width)) - 1);
      BinaryImage Image;
      Image.Arch = A;
      NdOpEmulator Emu(Image);
      Emu.setRegister(x86reg::RAX, Input);
      for (size_t I = 0; I + 1 < Ops.size(); ++I) {
        EXPECT_EQ(Ops[I].Opcode, NdOp::INT_ZEXT);
        ASSERT_TRUE(Emu.step(Ops[I]));
      }
      LowOp Read;
      Read.Opcode = NdOp::COPY;
      Read.Output = NdVar::reg(x86reg::RDX, 8);
      Read.NumInputs = 1;
      Read.Inputs[0] = Last.Inputs[1];
      ASSERT_TRUE(Emu.step(Read));
      EXPECT_EQ(Emu.getRegister(x86reg::RDX), Expected);
      EXPECT_FALSE(Emu.step(Last));
      EXPECT_EQ(Emu.getRegister(x86reg::RAX), Input);
      const auto Plan =
          x86SystemStateAsm(Intrinsic::Clzero, A, 0, Last.MemoryAddressSpace);
      ASSERT_TRUE(Plan);
      EXPECT_EQ(Plan->InputConstraints, std::vector<std::string>{"{ax}"});
      EXPECT_FALSE(Plan->ChangesFlags);
      EXPECT_NE(Plan->Constraints.find("~{memory}"), std::string::npos);
      for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
        auto Bad = Shape;
        if (Mutation == 0)
          Bad.Arguments = 0;
        else if (Mutation == 1)
          Bad.InputSizes[0] = 4;
        else if (Mutation == 2)
          Bad.OutputSize = 4;
        else if (Mutation == 3)
          Bad.MemoryOrdering = NdMemoryOrdering::Acquire;
        else
          Bad.TargetArch = Arch::AArch64;
        EXPECT_FALSE(x86SystemStateShapeIsValid(Intrinsic::Clzero, Bad));
      }
    }
}

TEST(X86SystemState, CacheZeroRejectsForgedOperandAndPrefixDetails) {
  for (const auto A : {Arch::X86, Arch::X64}) {
    Decoder D;
    ASSERT_TRUE(D.init(A));
    for (const auto &E : test::cacheZeroEncodings()) {
      DecodedInsn I{};
      ASSERT_EQ(D.decodeOneForLift(E.Bytes.data(), E.Bytes.size(), 0x1000, I),
                E.Bytes.size());
      for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
        auto Raw = *I.Raw;
        auto Detail = *Raw.detail;
        Raw.detail = &Detail;
        if (Mutation == 0)
          Raw.bytes[Raw.size - 1] ^= 1;
        else if (Mutation == 1)
          Detail.x86.op_count = 1;
        else if (Mutation == 2)
          Detail.x86.addr_size = 1;
        else if (Mutation == 3)
          Detail.x86.eflags = X86_EFLAGS_MODIFY_CF;
        else if (Mutation == 4)
          Detail.regs_read_count = 1;
        else if (Mutation == 5)
          Detail.x86.prefix[1] ^= 1;
        else if (Mutation == 6)
          Detail.x86.encoding.modrm_offset = 0;
        else
          Detail.x86.opcode[1] ^= 1;
        X86Lifter L(A);
        L.setStrict(true);
        std::vector<LowOp> Ops;
        EXPECT_THROW(L.lift(&Raw, Ops), UnliftedInstruction);
        EXPECT_TRUE(Ops.empty());
      }
    }
  }
}

TEST(X86SystemState, SmapAccessControlsHaveNoScalarOperandsOrFlagResult) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto Id : {Intrinsic::Clac, Intrinsic::Stac}) {
      const auto Ops =
          lift(A, {0x0f, 0x01,
                   static_cast<uint8_t>(Id == Intrinsic::Clac ? 0xca : 0xcb)});
      ASSERT_EQ(Ops.size(), 1u);
      const auto Shape = x86SystemStateLowShape(Ops.front(), A);
      ASSERT_TRUE(x86SystemStateShapeIsValid(Id, Shape));
      EXPECT_EQ(Shape.Arguments, 0);
      EXPECT_EQ(Shape.OutputSize, 0);
      const auto Plan =
          x86SystemStateAsm(Id, A, 0, NdMemoryAddressSpace::Default);
      ASSERT_TRUE(Plan);
      EXPECT_TRUE(Plan->InputBytes.empty());
      EXPECT_TRUE(Plan->OutputBytes.empty());
      // AC is hidden architectural state. CF/PF/AF/ZF/SF/OF are preserved.
      EXPECT_FALSE(Plan->ChangesFlags);
      EXPECT_NE(Plan->Constraints.find("~{memory}"), std::string::npos);
      for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
        auto Bad = Shape;
        if (Mutation == 0)
          Bad.Arguments = 1;
        else if (Mutation == 1)
          Bad.OutputSize = 1;
        else if (Mutation == 2)
          Bad.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
        else
          Bad.TargetArch = Arch::AArch64;
        EXPECT_FALSE(x86SystemStateShapeIsValid(Id, Bad));
      }
    }
}

TEST(X86SystemState, WaitReadsControlAndBothDeadlineHalvesAndClearsOtherFlags) {
  for (const auto Prefix : {0x66, 0xf2}) {
    const auto Ops =
        lift(Arch::X64, {static_cast<uint8_t>(Prefix), 0x0f, 0xae, 0xf1});
    ASSERT_FALSE(Ops.empty());
    const auto &Wait = Ops.front();
    ASSERT_EQ(Wait.Opcode, NdOp::INTRINSIC);
    ASSERT_EQ(Wait.NumInputs, 4);
    EXPECT_EQ(Wait.Inputs[1], NdVar::reg(x86reg::RCX, 4));
    EXPECT_EQ(Wait.Inputs[2], NdVar::reg(x86reg::RAX, 4));
    EXPECT_EQ(Wait.Inputs[3], NdVar::reg(x86reg::RDX, 4));
    EXPECT_EQ(Wait.Output, NdVar::reg(x86reg::CF, 1));
    for (uint64_t Result : {0, 1}) {
      BinaryImage Image;
      Image.Arch = Arch::X64;
      NdOpEmulator Emu(Image);
      for (const auto Flag : {x86reg::CF, x86reg::ZF, x86reg::OF, x86reg::SF,
                              x86reg::AF, x86reg::PF})
        Emu.setRegister(Flag, 1);
      Emu.setRegister(x86reg::CF, Result);
      for (size_t I = 1; I < Ops.size(); ++I)
        ASSERT_TRUE(Emu.step(Ops[I]));
      EXPECT_EQ(Emu.getRegister(x86reg::CF), Result);
      for (const auto Flag :
           {x86reg::ZF, x86reg::OF, x86reg::SF, x86reg::AF, x86reg::PF})
        EXPECT_EQ(Emu.getRegister(Flag), 0u);
    }
  }
}

TEST(X86ImplicitMsr, OperandsRetainTheirArchitecturalWidthsAndLinearAddresses) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto &E : test::systemEncodings(A)) {
      const auto Kind = x86SystemStateContract(E.Id).Kind;
      const bool List = Kind == X86SystemStateKind::MsrList;
      if (!List && Kind != X86SystemStateKind::WriteMsr)
        continue;
      for (uint8_t Prefix : {0, 0x67, 0x64, 0x65, 0x4f, 0x66}) {
        if ((Prefix == 0x4f && A == Arch::X86) || (Prefix == 0x66 && !List))
          continue;
        auto Bytes = E.Bytes;
        if (Prefix)
          Bytes.insert(Bytes.begin(), Prefix);
        SCOPED_TRACE(E.Mnemonic);
        SCOPED_TRACE(Prefix);
        const auto Ops = lift(A, Bytes);
        ASSERT_EQ(Ops.size(), 1u);
        const auto &Op = Ops.front();
        ASSERT_EQ(Op.Opcode, NdOp::INTRINSIC);
        ASSERT_EQ(Op.NumInputs, 4);
        EXPECT_EQ(Op.Inputs[0].Offset, static_cast<uint64_t>(E.Id));
        EXPECT_EQ(Op.Output, List ? NdVar::reg(x86reg::RCX, 8) : NdVar());
        EXPECT_EQ(Op.Inputs[1], List ? NdVar::reg(x86reg::RSI, 8)
                                     : NdVar::reg(x86reg::RCX, 4));
        EXPECT_EQ(Op.Inputs[2], List ? NdVar::reg(x86reg::RDI, 8)
                                     : NdVar::reg(x86reg::RAX, 4));
        EXPECT_EQ(Op.Inputs[3], List ? NdVar::reg(x86reg::RCX, 8)
                                     : NdVar::reg(x86reg::RDX, 4));
        EXPECT_EQ(Op.MemoryAddressSpace, NdMemoryAddressSpace::Default);
        const auto Shape = x86SystemStateLowShape(Op, A);
        ASSERT_TRUE(x86SystemStateShapeIsValid(E.Id, Shape));
        for (unsigned Mutation = 0; Mutation < 9; ++Mutation) {
          auto Bad = Shape;
          switch (Mutation) {
          case 0:
            Bad.OutputSize = List ? 0 : 4;
            break;
          case 1:
            Bad.InputSizes[0] = List ? 4 : 8;
            break;
          case 2:
            Bad.InputSizes[1] = List ? 4 : 8;
            break;
          case 3:
            Bad.InputSizes[2] = List ? 4 : 8;
            break;
          case 4:
            Bad.ScalarInputs[0] = false;
            break;
          case 5:
            Bad.Arguments = 2;
            break;
          case 6:
            Bad.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
            break;
          case 7:
            Bad.MemoryOrdering = NdMemoryOrdering::Relaxed;
            break;
          case 8:
            Bad.TargetArch = List ? Arch::X86 : Arch::AArch64;
            break;
          }
          EXPECT_FALSE(x86SystemStateShapeIsValid(E.Id, Bad));
        }
      }
    }
}

TEST(X86ImplicitMsr, ForgedEncodingAndDetailsRefuseBeforeEmission) {
  Decoder D;
  ASSERT_TRUE(D.init(Arch::X64));
  for (const auto &E : test::systemEncodings(Arch::X64)) {
    const auto Kind = x86SystemStateContract(E.Id).Kind;
    if (Kind != X86SystemStateKind::MsrList &&
        Kind != X86SystemStateKind::WriteMsr)
      continue;
    DecodedInsn I{};
    ASSERT_EQ(D.decodeOneForLift(E.Bytes.data(), E.Bytes.size(), 0x1000, I),
              E.Bytes.size());
    for (unsigned Mutation = 0; Mutation < 11; ++Mutation) {
      cs_insn Raw = *I.Raw;
      cs_detail Detail = *Raw.detail;
      Raw.detail = &Detail;
      switch (Mutation) {
      case 0:
        Detail.x86.op_count = 1;
        break;
      case 1:
        Raw.bytes[Raw.size - 1] = 0xc7;
        break;
      case 2:
        Raw.id =
            Raw.id == X86_INS_RDMSRLIST ? X86_INS_WRMSRLIST : X86_INS_RDMSRLIST;
        break;
      case 3:
        Raw.bytes[0] = 0xf0;
        break;
      case 4:
        Detail.x86.eflags = X86_EFLAGS_MODIFY_CF;
        break;
      case 5:
        Detail.x86.encoding.imm_size = 4;
        break;
      case 6:
        Detail.groups_count = 0;
        break;
      case 7:
        Detail.x86.encoding.modrm_offset = 0;
        break;
      case 8:
        Detail.x86.prefix[0] = 0xf0;
        break;
      case 9:
        Detail.regs_read_count = 0;
        break;
      case 10:
        Detail.x86.addr_size = 2;
        break;
      }
      X86Lifter L(Arch::X64);
      L.setStrict(true);
      std::vector<LowOp> Ops;
      EXPECT_THROW(L.lift(&Raw, Ops), UnliftedInstruction);
      EXPECT_TRUE(Ops.empty());
    }
  }
}

TEST(X86SystemState, UnmodeledRefiningPrefixesCannotBecomeLegacyInstructions) {
  const std::vector<std::vector<uint8_t>> Encodings = {
      {0xf2, 0x0f, 0x01, 0xca},
      {0xf3, 0x0f, 0x01, 0xca},
      {0xf3, 0x0f, 0x01, 0xee},
      {0xf3, 0x0f, 0x01, 0xef}};
  for (auto A : {Arch::X86, Arch::X64}) {
    Decoder D;
    ASSERT_TRUE(D.init(A));
    for (const auto &Bytes : Encodings) {
      DecodedInsn I{};
      const auto Size =
          D.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, I);
      if (A == Arch::X64 && Bytes.back() >= 0xee) {
        ASSERT_EQ(Size, Bytes.size());
        EXPECT_EQ(lift(A, Bytes).front().Inputs[0].Offset,
                  static_cast<uint64_t>(Bytes.back() == 0xee
                                            ? Intrinsic::X86ClearUserInterrupt
                                            : Intrinsic::X86SetUserInterrupt));
      } else
        EXPECT_EQ(Size, 0);
    }
  }
}

TEST(X86SystemExtensions,
     UserInterruptTestReturnsCarryAndClearsArithmeticFlags) {
  const auto Ops = lift(Arch::X64, {0xf3, 0x0f, 0x01, 0xed});
  ASSERT_EQ(Ops.size(), 6u);
  ASSERT_EQ(Ops.front().Opcode, NdOp::INTRINSIC);
  EXPECT_EQ(Ops.front().Output, NdVar::reg(x86reg::CF, 1));
  for (uint64_t Carry : {0, 1}) {
    BinaryImage Image;
    Image.Arch = Arch::X64;
    NdOpEmulator Emu(Image);
    for (auto F : {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF, x86reg::SF,
                   x86reg::OF, x86reg::DF})
      Emu.setRegister(F, 1);
    Emu.setRegister(x86reg::CF, Carry);
    for (size_t I = 1; I < Ops.size(); ++I)
      ASSERT_TRUE(Emu.step(Ops[I]));
    EXPECT_EQ(Emu.getRegister(x86reg::CF), Carry);
    EXPECT_EQ(Emu.getRegister(x86reg::DF), 1u);
    for (auto F : {x86reg::PF, x86reg::AF, x86reg::ZF, x86reg::SF, x86reg::OF})
      EXPECT_EQ(Emu.getRegister(F), 0u);
  }
}

TEST(X86SystemState, FarAndInterruptReturnsCannotBecomeAbiReturns) {
  for (auto A : {Arch::X86, Arch::X64}) {
    Decoder D;
    ASSERT_TRUE(D.init(A));
    std::vector<std::vector<uint8_t>> Encodings = {
        {0xcb}, {0xca, 0x08, 0x00}, {0x66, 0xcb}, {0xcf}, {0x66, 0xcf}};
    if (A == Arch::X64) {
      Encodings.push_back({0x48, 0xcf});
      Encodings.push_back({0x48, 0xcb});
    }
    for (const auto &Bytes : Encodings) {
      DecodedInsn I{};
      ASSERT_EQ(D.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, I),
                Bytes.size());
      std::vector<LowOp> Ops;
      EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
      EXPECT_TRUE(Ops.empty());
    }
  }
}

TEST(X86SystemExtensions, InputWidthsAndMalformedShapes) {
  for (auto A : {Arch::X86, Arch::X64})
    for (const auto &E : test::systemExtensionEncodings(A)) {
      SCOPED_TRACE(E.Mnemonic);
      const auto Ops = lift(A, E.Bytes);
      const auto It = std::find_if(Ops.begin(), Ops.end(), [](const auto &Op) {
        return Op.Opcode == NdOp::INTRINSIC;
      });
      ASSERT_NE(It, Ops.end());
      const auto Shape = x86SystemStateLowShape(*It, A);
      ASSERT_TRUE(x86SystemStateShapeIsValid(E.Id, Shape));
      if (E.Id == Intrinsic::X86InvalidateBroadcast) {
        EXPECT_EQ(It->Inputs[1],
                  NdVar::reg(x86reg::RAX,
                             A == Arch::X64 && E.Bytes[0] != 0x67 ? 8 : 4));
        EXPECT_EQ(It->Inputs[2], NdVar::reg(x86reg::RDX, 4));
        EXPECT_EQ(It->Inputs[3], NdVar::reg(x86reg::RCX, 4));
      } else if (E.Id == Intrinsic::X86HistoryReset)
        EXPECT_EQ(It->Inputs[1], NdVar::reg(x86reg::RAX, 4));
      else if (E.Id == Intrinsic::X86LoadKernelGs)
        EXPECT_EQ(It->Inputs[1], NdVar::reg(x86reg::RSI, 2));
      else if (E.Id == Intrinsic::X86SendUserInterrupt)
        EXPECT_EQ(It->Inputs[1], NdVar::reg(x86reg::RSI, 8));
      for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
        auto Bad = Shape;
        switch (Mutation) {
        case 0:
          Bad.TargetArch = Arch::AArch64;
          break;
        case 1:
          ++Bad.Arguments;
          break;
        case 2:
          Bad.OutputSize = 4;
          break;
        case 3:
          Bad.MemoryOrdering = NdMemoryOrdering::Acquire;
          break;
        case 4:
          Bad.MemoryAddressSpace = static_cast<NdMemoryAddressSpace>(255);
          break;
        case 5:
          if (Bad.Arguments)
            Bad.InputSizes[0] = 1;
          else
            Bad.WritableOutput = false, Bad.OutputSize = 1;
          break;
        }
        EXPECT_FALSE(x86SystemStateShapeIsValid(E.Id, Bad));
      }
    }
}

TEST(X86SystemExtensions, ForgedDecodingRefusesBeforeEmission) {
  Decoder D;
  ASSERT_TRUE(D.init(Arch::X64));
  for (const auto &E : test::systemExtensionEncodings(Arch::X64)) {
    DecodedInsn I{};
    ASSERT_EQ(D.decodeOneForLift(E.Bytes.data(), E.Bytes.size(), 0x1000, I),
              E.Bytes.size());
    for (unsigned Mutation = 0; Mutation < 7; ++Mutation) {
      auto Raw = *I.Raw;
      auto Detail = *Raw.detail;
      Raw.detail = &Detail;
      switch (Mutation) {
      case 0:
        Raw.bytes[Detail.x86.encoding.modrm_offset] ^= 8;
        break;
      case 1:
        ++Detail.x86.op_count;
        break;
      case 2:
        Detail.x86.eflags ^= X86_EFLAGS_MODIFY_CF;
        break;
      case 3:
        Detail.regs_read_count = 10;
        break;
      case 4:
        Detail.regs_write_count = 10;
        break;
      case 5:
        Detail.x86.prefix[0] ^= 1;
        break;
      case 6:
        Detail.x86.addr_size = 2;
        break;
      }
      X86Lifter L(Arch::X64);
      L.setStrict(true);
      std::vector<LowOp> Ops;
      EXPECT_THROW(L.lift(&Raw, Ops), UnliftedInstruction);
      EXPECT_TRUE(Ops.empty());
    }
  }
}

TEST(X86SystemState, DescriptorFailurePreservesTheCompleteDestination) {
  for (const uint8_t Opcode : {0x02, 0x03}) {
    const auto Ops = lift(Arch::X64, {0x0f, Opcode, 0xc1});
    ASSERT_FALSE(Ops.empty());
    ASSERT_EQ(Ops.front().Opcode, NdOp::INTRINSIC);
    for (uint64_t Success : {0, 1}) {
      BinaryImage Image;
      Image.Arch = Arch::X64;
      NdOpEmulator Emu(Image);
      Emu.setRegister(x86reg::RAX, 0xabcdef0123456789);
      // Supply a completion at the explicit environment boundary, then check
      // only the lifter's independent destination and flag projection.
      LowOp Completion = Ops.front();
      Completion.Opcode = NdOp::COPY;
      Completion.NumInputs = 1;
      Completion.Inputs[0] = NdVar::scalar((Success << 32) | 0x809b00, 8);
      ASSERT_TRUE(Emu.step(Completion));
      for (size_t I = 1; I < Ops.size(); ++I)
        ASSERT_TRUE(Emu.step(Ops[I]));
      EXPECT_EQ(Emu.getRegister(x86reg::RAX),
                Success ? 0x809b00 : 0xabcdef0123456789);
      EXPECT_EQ(Emu.getRegister(x86reg::ZF), Success);
    }
  }
}

TEST(X86SystemState,
     InstructionOwnedMemoryIsNeverPreloadedOrUnconditionallyStored) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto &E : test::systemEncodings(A)) {
      if (x86SystemStateContract(E.Id).MemoryArgument < 0)
        continue;
      SCOPED_TRACE(E.Mnemonic);
      const auto Ops = lift(A, E.Bytes);
      EXPECT_TRUE(std::none_of(Ops.begin(), Ops.end(), [](const LowOp &Op) {
        return Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE;
      }));
    }
}

TEST(X86SystemMemory, SharedContractRetainsAddressesWidthAndRealCompletion) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto &E : test::systemMemoryEncodings(A)) {
      SCOPED_TRACE(E.Mnemonic);
      SCOPED_TRACE(static_cast<unsigned>(A));
      const auto Ops = lift(A, E.Bytes);
      const auto It = std::find_if(Ops.begin(), Ops.end(), [&](const auto &Op) {
        return Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
               Op.Inputs[0].Offset == static_cast<uint64_t>(E.Id);
      });
      ASSERT_NE(It, Ops.end());
      EXPECT_TRUE(std::none_of(Ops.begin(), Ops.end(), [](const LowOp &Op) {
        return Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE ||
               Op.Opcode == NdOp::NOP;
      }));
      const auto Shape = x86SystemStateLowShape(*It, A);
      EXPECT_TRUE(x86SystemStateShapeIsValid(E.Id, Shape));
      const bool Enqueue =
          E.Id == Intrinsic::Enqcmd || E.Id == Intrinsic::Enqcmds;
      const bool Block = Enqueue || E.Id == Intrinsic::X86DirectStore64B;
      EXPECT_EQ(It->Output.Size, Enqueue ? 1 : 0);
      const auto Plan = x86SystemStateAsm(
          E.Id, A, x86SystemStateWidth(E.Id, Shape), Shape.MemoryAddressSpace);
      ASSERT_TRUE(Plan);
      EXPECT_TRUE(Plan->Text.starts_with(E.Mnemonic));
      EXPECT_NE(Plan->Constraints.find("~{memory}"), std::string::npos);
      EXPECT_EQ(Plan->ChangesFlags, Enqueue);
      EXPECT_EQ(Plan->Text.find("setz") != std::string::npos, Enqueue);
      EXPECT_EQ(
          x86SystemStateMemoryBytes(E.Id, x86SystemStateWidth(E.Id, Shape)),
          Block ? 64 : It->Inputs[2].Size);
      for (unsigned Fault = 0; Fault != 7; ++Fault) {
        auto Bad = Shape;
        switch (Fault) {
        case 0:
          Bad.ValidId = false;
          break;
        case 1:
          Bad.Arguments = 1;
          break;
        case 2:
          Bad.InputSizes[0] = 4;
          break;
        case 3:
          Bad.InputSizes[1] = 2;
          break;
        case 4:
          Bad.ScalarInputs[1] = false;
          break;
        case 5:
          Bad.OutputSize = 4;
          break;
        case 6:
          Bad.MemoryAddressSpace = static_cast<NdMemoryAddressSpace>(255);
          break;
        }
        EXPECT_FALSE(x86SystemStateShapeIsValid(E.Id, Bad));
      }
      if (Block || It->Inputs[2].Size == 8) {
        auto Bad = Shape;
        Bad.TargetArch = Arch::X86;
        EXPECT_FALSE(x86SystemStateShapeIsValid(E.Id, Bad));
      }
    }
}

TEST(X86DirectStore, ScalarMemoryProjectionKeepsWidthWrappingSegmentsAndFlags) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto &E : test::directStoreEncodings(A)) {
      if (E.Id != Intrinsic::X86DirectStore)
        continue;
      const auto Ops = lift(A, E.Bytes);
      ASSERT_FALSE(Ops.empty());
      const auto &Store = Ops.back();
      ASSERT_EQ(Store.Opcode, NdOp::INTRINSIC);
      const unsigned Width = Store.Inputs[2].Size;
      const uint64_t Base =
          Store.MemoryAddressSpace == NdMemoryAddressSpace::Default ? 0
                                                                    : 0x100000;
      for (unsigned Misalignment : {0, 1}) {
        BinaryImage Image;
        Image.Arch = A;
        Segment Memory;
        Memory.VA = Base + 0x3000;
        Memory.Size = Memory.FileSz = 64;
        Memory.Data.assign(64, 0x5a);
        Memory.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
        Image.Segments.push_back(Memory);
        NdOpEmulator Emu(Image);
        Emu.setStrictMode(true);
        if (Base)
          ASSERT_TRUE(
              Emu.setMemoryAddressSpaceBase(Store.MemoryAddressSpace, Base));
        Emu.setRegister(x86reg::RAX, 0x3010 + Misalignment);
        Emu.setRegister(x86reg::RBX, 0xfff0);
        Emu.setRegister(x86reg::RSI, 0x3020 + Misalignment);
        constexpr uint64_t Value = 0xfedcba9876543210;
        Emu.setRegister(x86reg::RCX, Value);
        for (uint64_t F : {x86reg::CF, x86reg::ZF, x86reg::OF, x86reg::SF,
                           x86reg::AF, x86reg::PF})
          Emu.setRegister(F, 1);
        ASSERT_EQ(Emu.run(Ops), Ops.size());
        LowOp Read;
        Read.Opcode = NdOp::LOAD;
        Read.Output = NdVar::reg(x86reg::XMM0, 64);
        Read.addInput(NdVar::cst(Memory.VA, 8));
        ASSERT_TRUE(Emu.step(Read));
        const auto Actual = Emu.getRegisterBytes(x86reg::XMM0);
        ASSERT_TRUE(Actual);
        auto Expected = Memory.Data;
        for (unsigned I = 0; I != Width; ++I)
          Expected[16 + Misalignment + I] = Value >> (8 * I);
        EXPECT_EQ(*Actual, Expected);
        for (uint64_t F : {x86reg::CF, x86reg::ZF, x86reg::OF, x86reg::SF,
                           x86reg::AF, x86reg::PF})
          EXPECT_EQ(Emu.getRegister(F), 1u);
        EXPECT_FALSE(Emu.skips().any());
      }
    }
}

TEST(X86DirectStore, ForgedOperandDetailsRefuseBeforeEmission) {
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Arch::X64));
  for (const auto &E : test::directStoreEncodings(Arch::X64)) {
    DecodedInsn I{};
    ASSERT_EQ(
        Decode.decodeOneForLift(E.Bytes.data(), E.Bytes.size(), 0x1000, I),
        E.Bytes.size());
    const bool Block = E.Id == Intrinsic::X86DirectStore64B;
    for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
      cs_insn Raw = *I.Raw;
      cs_detail Detail = *Raw.detail;
      Raw.detail = &Detail;
      auto &X = Detail.x86;
      switch (Mutation) {
      case 0:
        X.operands[Block ? 0 : 1].reg = X86_REG_XMM0;
        break;
      case 1:
        X.operands[Block ? 1 : 0].size = 2;
        break;
      case 2:
        X.operands[Block ? 1 : 0].mem.base = X86_REG_R15;
        break;
      case 3:
        X.op_count = 1;
        break;
      case 4:
        X.addr_size = 2;
        break;
      case 5:
        X.prefix[0] = 0xf0;
        break;
      }
      X86Lifter L(Arch::X64);
      L.setStrict(true);
      std::vector<LowOp> Ops;
      EXPECT_THROW(L.lift(&Raw, Ops), UnliftedInstruction);
      EXPECT_TRUE(Ops.empty());
    }
  }
}

TEST(X86MaskedStore, SelectedLanesWriteWithoutTouchingMaskedUnmappedMemory) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (const bool Integer : {false, true})
      for (const bool Qword : {false, true})
        for (const bool Wide : {false, true}) {
          const uint16_t Lane = Qword ? 8 : 4;
          const uint16_t Size = Wide ? 32 : 16;
          // [eax/rax], xmm/ymm5, xmm/ymm2. Floating forms select element width
          // by opcode; integer forms select it by VEX.W.
          const auto Ops =
              lift(A, {0xc4, 0xe2,
                       static_cast<uint8_t>(0x51 | (Wide ? 4 : 0) |
                                            (Integer && Qword ? 0x80 : 0)),
                       static_cast<uint8_t>(Integer ? 0x8e
                                            : Qword ? 0x2f
                                                    : 0x2e),
                       0x10});
          ASSERT_FALSE(Ops.empty());
          BinaryImage Image;
          Image.Arch = A;
          Segment Memory;
          Memory.VA = 0x4000;
          Memory.Size = Memory.FileSz = 2 * Lane;
          Memory.Data.assign(2 * Lane, 0x55);
          Memory.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
          Image.Segments.push_back(std::move(Memory));
          NdOpEmulator Emu(Image);
          Emu.setStrictMode(true);
          Emu.setRegister(x86reg::RAX, 0x4000);
          std::vector<uint8_t> Mask(Size, 0x7f), Data(Size, 0xa5);
          Mask[Lane - 1] = 0x80;
          Emu.setRegisterBytes(x86reg::XMM5, Mask);
          Emu.setRegisterBytes(x86reg::XMM2, Data);
          for (const auto &Op : Ops) {
            EXPECT_FALSE(Op.Output.isReg() && Op.Output.Offset == x86reg::RAX);
            ASSERT_TRUE(Emu.step(Op));
          }
          EXPECT_EQ(Emu.getRegister(x86reg::RAX), 0x4000u);
          LowOp Read;
          Read.Opcode = NdOp::LOAD;
          Read.Output = NdVar::reg(x86reg::XMM0, 2 * Lane);
          Read.addInput(NdVar::scalar(0x4000, 8));
          ASSERT_TRUE(Emu.step(Read));
          const auto Bytes = Emu.getRegisterBytes(x86reg::XMM0);
          ASSERT_TRUE(Bytes);
          for (unsigned I = 0; I != 2 * Lane; ++I)
            EXPECT_EQ((*Bytes)[I], I < Lane ? 0xa5 : 0x55);
        }
}

} // namespace

TEST(X86Sgx, ImplicitOperandsAndIgnoredPrefixesKeepNativeWidths) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto Opcode : {0xcf, 0xc0})
      for (const uint8_t Prefix : {0, 0x67, 0x64, 0x65, 0x4f}) {
        if (Prefix == 0x4f && A == Arch::X86)
          continue;
        std::vector<uint8_t> Bytes;
        if (Prefix)
          Bytes.push_back(Prefix);
        Bytes.insert(Bytes.end(), {0x0f, 0x01, static_cast<uint8_t>(Opcode)});
        const auto Ops = lift(A, Bytes);
        const auto It =
            std::find_if(Ops.begin(), Ops.end(), [](const auto &Op) {
              return Op.Opcode == NdOp::INTRINSIC;
            });
        ASSERT_NE(It, Ops.end());
        const auto Id =
            Opcode == 0xcf ? Intrinsic::X86Encls : Intrinsic::X86Enclv;
        const uint16_t Word = A == Arch::X64 ? 8 : 4;
        ASSERT_EQ(It->NumInputs, 5);
        EXPECT_EQ(It->Output, NdVar::reg(x86reg::RAX, Word));
        EXPECT_EQ(It->Inputs[1], NdVar::reg(x86reg::RAX, Word));
        for (unsigned I = 2; I <= 4; ++I) {
          EXPECT_TRUE(It->Inputs[I].isTemp());
          EXPECT_EQ(It->Inputs[I].Size, Word);
        }
        EXPECT_TRUE(x86SgxShapeIsValid(Id, x86SgxLowShape(*It, A)));
        EXPECT_EQ(intrinsicOutputCount(Id), 2u);
        EXPECT_TRUE(isSideeffectIntrinsic(Id));
        BinaryImage Image;
        Image.Arch = A;
        NdOpEmulator Emulator(Image);
        EXPECT_FALSE(Emulator.step(*It));
        for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
          auto S = x86SgxLowShape(*It, A);
          switch (Mutation) {
          case 0:
            S.InputSizes[3] = 2;
            break;
          case 1:
            S.OutputSize = 0;
            break;
          case 2:
            S.Arguments = 3;
            break;
          case 3:
            S.ValidId = false;
            break;
          case 4:
            S.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
            break;
          case 5:
            S.TargetArch = Arch::AArch64;
            break;
          }
          EXPECT_FALSE(x86SgxShapeIsValid(Id, S));
        }
      }
}

TEST(X86Sgx, FlagProjectionUsesIncomingLeafAndPreservesDirection) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (bool Supervisor : {false, true})
      for (unsigned Leaf = 0; Leaf <= 24; ++Leaf)
        for (unsigned Old : {0, 1})
          for (unsigned Completion = 0; Completion < 4; ++Completion) {
            if (!Supervisor && Leaf > 2)
              continue;
            const auto Ops =
                lift(A, {0x0f, 0x01,
                         static_cast<uint8_t>(Supervisor ? 0xcf : 0xc0)});
            BinaryImage Image;
            Image.Arch = A;
            NdOpEmulator E(Image);
            E.setRegister(x86reg::RAX,
                          (A == Arch::X64 ? UINT64_C(0x1234567800000000) : 0) |
                              Leaf);
            E.setRegister(x86reg::RBX, 0x12345678);
            E.setRegister(x86reg::RCX, 0x23456789);
            E.setRegister(x86reg::RDX, 0x3456789a);
            for (auto F : {x86reg::CF, x86reg::ZF, x86reg::AF, x86reg::PF,
                           x86reg::SF, x86reg::OF, x86reg::DF})
              E.setRegister(F, Old);
            auto Supply = [&](NdVar Dst, uint64_t Value) {
              LowOp O;
              O.Opcode = NdOp::COPY;
              O.Output = Dst;
              O.addInput(NdVar::scalar(Value, Dst.Size));
              EXPECT_TRUE(E.step(O));
            };
            for (unsigned I = 0; I < Ops.size(); ++I) {
              if (Ops[I].Opcode == NdOp::INTRINSIC) {
                // Substitute only the unavailable CPU completion. The actual
                // lifted flag projection and auxiliary transport run unchanged.
                ASSERT_LT(I + 2, Ops.size());
                Supply(Ops[I].Output, 0xdeadbeef);
                Supply(Ops[I + 1].Inputs[0], 0xabcde123);
                Supply(Ops[I + 2].Inputs[0],
                       (Completion & 1) | ((Completion >> 1) << 8));
              } else
                ASSERT_TRUE(E.step(Ops[I]));
            }
            const bool Preserve =
                Supervisor && (Leaf == 0 || Leaf == 1 || Leaf == 6 ||
                               Leaf == 10 || Leaf == 13);
            EXPECT_EQ(E.getRegister(x86reg::CF),
                      Preserve ? Old : Completion & 1);
            EXPECT_EQ(E.getRegister(x86reg::ZF),
                      Preserve ? Old : Completion >> 1);
            for (auto F : {x86reg::AF, x86reg::PF, x86reg::SF, x86reg::OF})
              EXPECT_EQ(E.getRegister(F), Preserve ? Old : 0);
            EXPECT_EQ(E.getRegister(x86reg::DF), Old);
            EXPECT_EQ(E.getRegister(x86reg::RBX),
                      Supervisor && Leaf == 4 ? 0xabcde123u : 0x12345678u);
            EXPECT_EQ(E.getRegister(x86reg::RAX), 0xdeadbeefu);
          }
}

TEST(X86Sgx, RefiningPrefixesNeverBecomeUnprefixedInstructions) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (const uint8_t Opcode : {0xcf, 0xc0})
      for (const uint8_t Prefix : {0xf0, 0x66, 0xf2, 0xf3}) {
        Decoder D;
        ASSERT_TRUE(D.init(A));
        const std::vector<uint8_t> Bytes = {Prefix, 0x0f, 0x01, Opcode};
        DecodedInsn I{};
        const auto Size =
            D.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, I);
        if (!Size)
          continue;
        std::vector<LowOp> Ops;
        EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
        EXPECT_TRUE(Ops.empty());
      }
}

TEST(X86SegmentedBlock, OffsetsSegmentsAddressWidthsAndCompletionStayExplicit) {
  for (unsigned Kind = 0; Kind != 3; ++Kind)
    for (unsigned Segment = 0; Segment != 7; ++Segment)
      for (bool Address16 : {false, true}) {
        SCOPED_TRACE(Kind);
        SCOPED_TRACE(Segment);
        SCOPED_TRACE(Address16);
        const uint8_t Prefixes[] = {0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65, 0};
        std::vector<uint8_t> Bytes;
        if (Prefixes[Segment])
          Bytes.push_back(Prefixes[Segment]);
        if (Address16)
          Bytes.push_back(0x67);
        Bytes.insert(Bytes.end(),
                     {uint8_t(Kind == 0   ? 0x66
                              : Kind == 1 ? 0xf2
                                          : 0xf3),
                      0x0f, 0x38, 0xf8, uint8_t(Address16 ? 0x3c : 0x3e)});
        const auto Ops = lift(Arch::X86, Bytes);
        auto I = std::find_if(Ops.begin(), Ops.end(), [](const auto &Op) {
          return Op.Opcode == NdOp::INTRINSIC;
        });
        ASSERT_NE(I, Ops.end());
        EXPECT_EQ(I->Inputs[0].Offset,
                  uint16_t(Intrinsic::X86SegmentedBlockMemory));
        const auto Shape = x86SystemStateLowShape(*I, Arch::X86);
        ASSERT_TRUE(x86SystemStateShapeIsValid(
            Intrinsic::X86SegmentedBlockMemory, Shape));
        EXPECT_EQ(Shape.SegmentedBlockSelector,
                  x86SegmentedBlockSelector(
                      static_cast<X86SegmentedBlockKind>(Kind),
                      static_cast<X86BlockSegment>(Segment == 6 ? 3 : Segment),
                      Address16));
        EXPECT_EQ(I->Output.Size, Kind == 0 ? 0 : 1);
        BinaryImage Image;
        Image.Arch = Arch::X86;
        NdOpEmulator Emu(Image);
        Emu.setRegister(x86reg::RSI, 0xa5a51234);
        Emu.setRegister(x86reg::RDI, 0x5a5a5678);
        Emu.setRegister(x86reg::CF, 1);
        for (auto P = Ops.begin(); P != I; ++P)
          ASSERT_TRUE(Emu.step(*P));
        EXPECT_EQ(Emu.getRegister(I->Inputs[2].Offset),
                  Address16 ? 0x1234u : 0xa5a51234u);
        EXPECT_FALSE(Emu.step(*I)); // No implicit flat segment environment.
        EXPECT_EQ(Emu.getRegister(x86reg::CF), 1u);
        for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
          auto Bad = Shape;
          switch (Mutation) {
          case 0:
            Bad.TargetArch = Arch::X64;
            break;
          case 1:
            Bad.SegmentedBlockSelector.reset();
            break;
          case 2:
            Bad.SegmentedBlockSelector = 3;
            break;
          case 3:
            Bad.SegmentedBlockSelector = 64;
            break;
          case 4:
            Bad.InputSizes[1] = 8;
            break;
          case 5:
            Bad.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
            break;
          }
          EXPECT_FALSE(x86SystemStateShapeIsValid(
              Intrinsic::X86SegmentedBlockMemory, Bad));
        }
      }
}

TEST(X86SegmentedBlock, StackAddressDefaultsToSSAndMalformedDetailIsRefused) {
  for (const auto &Bytes :
       {std::vector<uint8_t>{0x66, 0x0f, 0x38, 0xf8, 0x7d, 0},
        std::vector<uint8_t>{0x67, 0x66, 0x0f, 0x38, 0xf8, 0x7e, 0}}) {
    auto Ops = lift(Arch::X86, Bytes);
    auto I = std::find_if(Ops.begin(), Ops.end(), [](const auto &Op) {
      return Op.Opcode == NdOp::INTRINSIC;
    });
    ASSERT_NE(I, Ops.end());
    EXPECT_EQ((I->Inputs[1].Offset >> 2) & 7, unsigned(X86BlockSegment::SS));
    Decoder D;
    ASSERT_TRUE(D.init(Arch::X86));
    DecodedInsn DI{};
    ASSERT_EQ(D.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, DI),
              Bytes.size());
    // The public lifter requires trusted bytes/detail; changing the register
    // operand width must never produce a different segmented transaction.
    auto *CI = DI.Raw;
    CI->detail->x86.operands[0].size = 8;
    X86Lifter L(Arch::X86);
    L.setStrict(true);
    std::vector<LowOp> Bad;
    EXPECT_THROW(L.lift(CI, Bad), std::exception);
    EXPECT_TRUE(Bad.empty());
  }
}
