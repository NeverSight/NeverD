//===- X86VirtualizationTests.cpp - VMX/SVM completion -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent synthetic instruction encodings. Privileged instructions are
/// never executed on the host: mocked completion tests the architectural merge.
//===----------------------------------------------------------------------===//

#include "X86VirtualizationFixtures.h"
#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <array>
#include <exception>
#include <iterator>
#include <vector>

using namespace neverd;
using namespace neverd::test;

namespace {

std::vector<LowOp> lift(Arch A, const std::vector<uint8_t> &Bytes) {
  Decoder D;
  if (!D.init(A)) {
    ADD_FAILURE();
    return {};
  }
  DecodedInsn I{};
  if (D.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, I) !=
      static_cast<int>(Bytes.size())) {
    ADD_FAILURE();
    return {};
  }
  std::vector<LowOp> Ops;
  try {
    D.liftToLow(I, Ops);
  } catch (const std::exception &E) {
    ADD_FAILURE() << E.what();
  }
  return Ops;
}

const LowOp *operation(const std::vector<LowOp> &Ops, Intrinsic Id) {
  for (const auto &Op : Ops)
    if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
        Op.Inputs[0].isConst() && Op.Inputs[0].Offset == uint64_t(Id))
      return &Op;
  return nullptr;
}
} // namespace

TEST(X86Virtualization, EveryOwnedEncodingRetainsItsOperandsAndShape) {
  for (Arch A : {Arch::X86, Arch::X64})
    for (const auto &E : Encodings) {
      SCOPED_TRACE(x86VirtualizationContract(E.Id).Mnemonic);
      SCOPED_TRACE(A == Arch::X64 ? "x64" : "x86");
      const auto Ops = lift(A, E.Bytes);
      const auto *Op = operation(Ops, E.Id);
      ASSERT_NE(Op, nullptr);
      EXPECT_TRUE(x86VirtualizationShapeIsValid(
          E.Id, x86VirtualizationLowShape(*Op, A)));
      EXPECT_EQ(Op->Addr, 0x1000u);
      EXPECT_TRUE(isSideeffectIntrinsic(E.Id));
      for (const auto &Part : Ops)
        EXPECT_TRUE(Part.Opcode != NdOp::LOAD && Part.Opcode != NdOp::STORE);
      if (E.Id == Intrinsic::SvmInvalidate) {
        ASSERT_EQ(Op->NumInputs, 3);
        EXPECT_EQ(Op->Inputs[1].Offset, x86reg::RAX);
        EXPECT_EQ(Op->Inputs[2].Offset, x86reg::RCX);
        EXPECT_EQ(Op->Inputs[2].Size, 4);
      }
    }
}

TEST(X86Virtualization, VmreadPreservesTheDestinationOnBothFailureKinds) {
  for (Arch A : {Arch::X86, Arch::X64})
    for (uint16_t Flags : {uint16_t(0), uint16_t(1), uint16_t(0x100)}) {
      SCOPED_TRACE(Flags);
      const uint16_t Word = A == Arch::X64 ? 8 : 4;
      const uint64_t Old = A == Arch::X64 ? 0xABCDEF1287654321ULL : 0x87654321;
      const uint64_t New = A == Arch::X64 ? 0x80000000FEDCBA98ULL : 0xFEDCBA98;
      const auto Ops = lift(A, Encodings[0].Bytes);
      ASSERT_NE(operation(Ops, Intrinsic::VmxRead), nullptr);
      BinaryImage Image;
      Image.Arch = A;
      NdOpEmulator Emulator(Image);
      Emulator.setRegister(x86reg::RAX, Old);
      for (const auto Flag : {x86reg::CF, x86reg::ZF, x86reg::AF, x86reg::OF,
                              x86reg::SF, x86reg::PF})
        Emulator.setRegister(Flag, 1);
      for (auto Op : Ops) {
        if (Op.Opcode == NdOp::INTRINSIC) {
          // Independent architectural completion oracle: status occupies the
          // upper natural word. A failed read's candidate must be ignored.
          Op.Opcode = NdOp::CONCAT;
          Op.NumInputs = 0;
          Op.addInput(NdVar::scalar(Flags, Word));
          Op.addInput(NdVar::scalar(New, Word));
        }
        ASSERT_TRUE(Emulator.step(Op));
      }
      EXPECT_EQ(Emulator.getRegister(x86reg::RAX).value_or(0),
                Flags ? Old : New);
      EXPECT_EQ(Emulator.getRegister(x86reg::CF).value_or(7), Flags & 1);
      EXPECT_EQ(Emulator.getRegister(x86reg::ZF).value_or(7), Flags >> 8);
      for (const auto Flag : {x86reg::AF, x86reg::OF, x86reg::SF, x86reg::PF})
        EXPECT_EQ(Emulator.getRegister(Flag).value_or(7), 0u);
    }
}

TEST(X86Virtualization, ScalarEmulationNeverInventsVmxOrSvmCompletion) {
  for (const auto &E : Encodings) {
    const auto Ops = lift(Arch::X64, E.Bytes);
    const auto *Op = operation(Ops, E.Id);
    ASSERT_NE(Op, nullptr);
    BinaryImage Image;
    Image.Arch = Arch::X64;
    NdOpEmulator Emulator(Image);
    Emulator.setRegister(x86reg::RAX, 0xdeadbeef);
    EXPECT_FALSE(Emulator.step(*Op));
    EXPECT_EQ(Emulator.getRegister(x86reg::RAX).value_or(0), 0xdeadbeefu);
  }
}

TEST(X86Virtualization, MalformedShapesNeverBecomeOrdinaryMemoryEffects) {
  for (const auto &E : Encodings) {
    const auto Ops = lift(Arch::X64, E.Bytes);
    const auto *Op = operation(Ops, E.Id);
    ASSERT_NE(Op, nullptr);
    const auto Good = x86VirtualizationLowShape(*Op, Arch::X64);
    ASSERT_TRUE(x86VirtualizationShapeIsValid(E.Id, Good));
    for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
      auto Bad = Good;
      switch (Mutation) {
      case 0:
        Bad.ValidId = false;
        break;
      case 1:
        Bad.OutputSize += 1;
        break;
      case 2:
        Bad.Arguments += 1;
        break;
      case 3:
        Bad.TargetArch = Arch::AArch64;
        break;
      case 4:
        Bad.MemoryOrdering = NdMemoryOrdering::Acquire;
        break;
      }
      EXPECT_FALSE(x86VirtualizationShapeIsValid(E.Id, Bad));
    }
  }
}

TEST(X86Virtualization, AddressSizeAndSegmentsStayWithTheInstruction) {
  for (Intrinsic Id :
       {Intrinsic::SvmLoad, Intrinsic::SvmSave, Intrinsic::SvmInvalidate}) {
    auto It = std::find_if(std::begin(Encodings), std::end(Encodings),
                           [&](const auto &E) { return E.Id == Id; });
    auto Bytes = It->Bytes;
    Bytes.insert(Bytes.begin(), 0x67);
    const auto Ops = lift(Arch::X64, Bytes);
    const auto *Op = operation(Ops, Id);
    ASSERT_NE(Op, nullptr);
    EXPECT_EQ(Op->Inputs[1].Size, 4);
  }
  auto Bytes = Encodings[1].Bytes;
  Bytes.insert(Bytes.begin(), 0x65);
  const auto Ops = lift(Arch::X64, Bytes);
  const auto *Op = operation(Ops, Intrinsic::VmxReadMemory);
  ASSERT_NE(Op, nullptr);
  EXPECT_EQ(Op->MemoryAddressSpace, NdMemoryAddressSpace::X86GS);
  EXPECT_TRUE(x86VirtualizationShapeIsValid(
      Intrinsic::VmxReadMemory, x86VirtualizationLowShape(*Op, Arch::X64)));
}

TEST(X86Virtualization, KvmAbiRequiresFiveNaturalWidthValues) {
  for (const auto Id : {Intrinsic::X86KvmVmcall, Intrinsic::X86KvmVmmcall})
    for (const auto A : {Arch::X86, Arch::X64}) {
      const uint16_t Word = A == Arch::X64 ? 8 : 4;
      LowOp Op{.Opcode = NdOp::INTRINSIC, .Output = NdVar::tmp(0, Word)};
      Op.addInput(NdVar::cst(uint64_t(Id), 2));
      for (unsigned I = 0; I != 5; ++I)
        Op.addInput(NdVar::reg(I * 8, Word));
      const auto Valid = [&](const LowOp &Candidate) {
        return x86VirtualizationShapeIsValid(
            Id, x86VirtualizationLowShape(Candidate, A));
      };
      EXPECT_TRUE(Valid(Op));
      EXPECT_TRUE(isSideeffectIntrinsic(Id));
      for (unsigned I = 1; I != 6; ++I) {
        auto Wrong = Op;
        Wrong.Inputs[I].Size = 2;
        EXPECT_FALSE(Valid(Wrong));
      }
      auto Missing = Op;
      --Missing.NumInputs;
      EXPECT_FALSE(Valid(Missing));
      auto WrongResult = Op;
      WrongResult.Output.Size = 2;
      EXPECT_FALSE(Valid(WrongResult));
      auto Segmented = Op;
      Segmented.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      EXPECT_FALSE(Valid(Segmented));
    }
}

TEST(X86Virtualization, SgxTransferRequiresACompleteBankAndOriginalPC) {
  LowOp Op{.Opcode = NdOp::INTRINSIC};
  Op.addInput(NdVar::cst(uint64_t(Intrinsic::X86SgxTransferAtContext), 2));
  Op.addInput(NdVar::reg(x86reg::RDI, 8));
  Op.addInput(NdVar::scalar(0xffff800012345678ULL, 8));
  const auto Valid = [&](const LowOp &Candidate, Arch A = Arch::X64) {
    return x86VirtualizationShapeIsValid(
        Intrinsic::X86SgxTransferAtContext,
        x86VirtualizationLowShape(Candidate, A));
  };
  EXPECT_TRUE(Valid(Op));
  EXPECT_FALSE(Valid(Op, Arch::X86));
  EXPECT_FALSE(Valid(Op, Arch::AArch64));
  EXPECT_TRUE(isSideeffectIntrinsic(Intrinsic::X86SgxTransferAtContext));
  for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
    auto Bad = Op;
    switch (Mutation) {
    case 0:
      Bad.Inputs[1].Size = 4;
      break;
    case 1:
      Bad.Inputs[2] = NdVar::reg(x86reg::RCX, 8);
      break;
    case 2:
      Bad.Inputs[2].Size = 4;
      break;
    case 3:
      Bad.Output = NdVar::reg(x86reg::RAX, 8);
      break;
    case 4:
      Bad.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      break;
    case 5:
      Bad.MemoryOrdering = NdMemoryOrdering::Acquire;
      break;
    case 6:
      Bad.NumInputs = 2;
      break;
    }
    EXPECT_FALSE(Valid(Bad)) << Mutation;
  }
}

TEST(X86Virtualization, VmrunContextRequiresAnExplicitPointerAndConstantWidth) {
  LowOp Op{.Opcode = NdOp::INTRINSIC};
  Op.addInput(NdVar::cst(uint64_t(Intrinsic::X86SvmRunContext), 2));
  Op.addInput(NdVar::reg(x86reg::RDI, 8));
  Op.addInput(NdVar::scalar(64, 4));
  const auto Valid = [&](const LowOp &Candidate, Arch A = Arch::X64) {
    return x86VirtualizationShapeIsValid(
        Intrinsic::X86SvmRunContext, x86VirtualizationLowShape(Candidate, A));
  };
  EXPECT_TRUE(Valid(Op));
  EXPECT_FALSE(Valid(Op, Arch::X86));
  EXPECT_FALSE(Valid(Op, Arch::AArch64));
  EXPECT_TRUE(isSideeffectIntrinsic(Intrinsic::X86SvmRunContext));
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    auto Bad = Op;
    switch (Mutation) {
    case 0:
      Bad.Inputs[1].Size = 4;
      break;
    case 1:
      Bad.Inputs[2] = NdVar::reg(x86reg::RCX, 4);
      break;
    case 2:
      Bad.Inputs[2] = NdVar::scalar(16, 4);
      break;
    case 3:
      Bad.Inputs[2].Size = 8;
      break;
    case 4:
      Bad.Output = NdVar::reg(x86reg::RAX, 8);
      break;
    case 5:
      Bad.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      break;
    case 6:
      Bad.MemoryOrdering = NdMemoryOrdering::Acquire;
      break;
    case 7:
      Bad.NumInputs = 2;
      break;
    }
    EXPECT_FALSE(Valid(Bad)) << Mutation;
  }
  Op.Inputs[2] = NdVar::scalar(32, 4);
  EXPECT_TRUE(Valid(Op));
}
