//===- NativeStateAuditTests.cpp - String/control admission -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Native bytes, complete effect receipts, and independent state oracles.
//===----------------------------------------------------------------------===//

#include "../../lib/analysis/arch/x86_64/StringTransfer.h"
#include "../../lib/analysis/core/RecoveryArchitecture.h"
#include "../../lib/lift/X86/X86CompatibilityReturnUndefined.h"
#include "../../lib/lift/X86/X86LegacyVectorMoveAudit.h"
#include "../../lib/lift/X86/X86MultiplyUndefined.h"
#include "../../lib/lift/X86/X86NativeStateAudit.h"
#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/symbolic/SymExec.h"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <vector>

using namespace neverd;
using namespace neverd::analysis;
using namespace neverd::symbolic;

namespace {

constexpr uint64_t CodeAddress = 0x1000, MemoryBase = 0x8000;
constexpr std::array<unsigned, 7> Flags = {x86reg::CF, x86reg::PF, x86reg::AF,
                                           x86reg::ZF, x86reg::SF, x86reg::DF,
                                           x86reg::OF};

std::vector<LowOp>
lift(llvm::ArrayRef<uint8_t> Bytes,
     LowUndefinedCoverage ExpectedCoverage = LowUndefinedCoverage::Complete) {
  Decoder Dec;
  EXPECT_TRUE(Dec.init(Arch::X64));
  Dec.setStrict(true);
  DecodedInsn Insn{};
  EXPECT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), CodeAddress, Insn),
            int(Bytes.size()));
  if (!Insn.Raw)
    return {};
  std::vector<LowOp> Ops, Plain;
  LowInstructionUndefinedEffects Effects;
  LowInstructionPreservedState Preserved;
  Preserved.Audit = LowPreservedStateAudit::LegacyIntegerV1;
  Dec.liftToLow(Insn, Ops, {}, {}, &Effects, &Preserved);
  Dec.liftToLow(Insn, Plain);
  EXPECT_EQ(Effects.Coverage, ExpectedCoverage) << Effects.Diagnostic;
  EXPECT_TRUE(Effects.Effects.empty());
  EXPECT_EQ(Effects.OpCount, Ops.size());
  EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Ops));
  EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Plain));
  EXPECT_EQ(Preserved, LowInstructionPreservedState{});
  return Ops;
}

TEST(NativeStateAudit, IntegerExtensionsRequireExactEncodingAndEffectReceipts) {
  struct Case {
    std::vector<uint8_t> Bytes;
    unsigned Kind, Arbitrary;
  };
  for (const auto &C : std::vector<Case>{{{0x0f, 0x57, 0xc1}, 0, 0},
                                         {{0x66, 0x48, 0x0f, 0x7e, 0xc8}, 0, 0},
                                         {{0x66, 0x0f, 0x6e, 0xc8}, 0, 0},
                                         {{0x66, 0x0f, 0x73, 0xd9, 0xff}, 0, 0},
                                         {{0x48, 0xf7, 0xe1}, 1, 4},
                                         {{0x48, 0xf7, 0xe9}, 1, 4},
                                         {{0x48, 0x0f, 0xaf, 0xc1}, 1, 4},
                                         {{0xf0, 0x48, 0x0f, 0xc1, 0x37}, 2, 0},
                                         {{0xf0, 0x0f, 0xba, 0x2f, 0x81}, 2, 4},
                                         {{0xf0, 0x48, 0x0f, 0xb3, 0x37}, 2, 4},
                                         {{0xf0, 0x83, 0x0f, 0xff}, 2, 1}}) {
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X64));
    Dec.setStrict(true);
    DecodedInsn Instruction{};
    ASSERT_EQ(Dec.decodeOneForLift(C.Bytes.data(), C.Bytes.size(), 0x1000,
                                   Instruction),
              int(C.Bytes.size()));
    const auto Form = [&](const cs_insn *Raw) {
      return C.Kind == 0   ? vectormoveaudit::form(Raw, Arch::X64)
             : C.Kind == 1 ? multiplyundefined::form(Raw, Arch::X64)
                           : nativestateaudit::form(Raw, Arch::X64);
    };
    ASSERT_TRUE(Form(Instruction.Raw));
    std::vector<LowOp> Ops, Plain;
    LowInstructionUndefinedEffects Effects;
    Dec.liftToLow(Instruction, Ops, {}, {}, &Effects);
    Dec.liftToLow(Instruction, Plain);
    ASSERT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
        << Effects.Diagnostic;
    EXPECT_EQ(Effects.Effects.size(), C.Arbitrary);
    EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Ops));
    EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Plain));
    if (C.Kind == 1)
      EXPECT_TRUE(multiplyundefined::matches(Ops, Effects));
    if (C.Kind == 2) {
      unsigned Transactions = 0;
      for (const auto &Op : Ops) {
        EXPECT_NE(Op.Opcode, NdOp::LOAD);
        EXPECT_NE(Op.Opcode, NdOp::STORE);
        Transactions +=
            Op.MemoryOrdering == NdMemoryOrdering::SequentiallyConsistent;
      }
      EXPECT_EQ(Transactions, 1u);
    }
    for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
      cs_insn Raw = *Instruction.Raw;
      cs_detail Detail = *Raw.detail;
      Raw.detail = &Detail;
      if (Mutation == 0)
        Detail.x86.operands[0].size ^= 1;
      else if (Mutation == 1)
        Detail.x86.op_count += 1;
      else if (Mutation == 2)
        Raw.bytes[0] ^= 1;
      else
        Raw.size -= 1;
      EXPECT_FALSE(Form(&Raw)) << Mutation;
    }
  }
}

TEST(NativeStateAudit, LegacyMsrAccessKeepsRuntimeSelectorAndWholeValue) {
  for (bool Write : {false, true}) {
    const auto Ops = lift({0x0f, uint8_t(Write ? 0x30 : 0x32)});
    unsigned Count = 0;
    for (const auto &Op : Ops)
      if (Op.Opcode == NdOp::INTRINSIC) {
        ++Count;
        EXPECT_EQ(Op.Inputs[1], NdVar::reg(x86reg::RCX, 4));
        EXPECT_EQ(Op.NumInputs, Write ? 3 : 2);
        if (Write)
          EXPECT_EQ(Op.Inputs[2].Size, 8);
        else
          EXPECT_EQ(Op.Output.Size, 8);
      }
    EXPECT_EQ(Count, 1u);
  }
}

TEST(NativeStateAudit, DescriptorQueriesRetainExactOperandsAndEffects) {
  for (unsigned Destination = 0; Destination != 16; ++Destination)
    for (unsigned Source = 0; Source != 16; ++Source) {
      std::vector<uint8_t> Bytes;
      if (Destination >= 8 || Source >= 8)
        Bytes.push_back(0x40 | (Destination >= 8 ? 4 : 0) |
                        (Source >= 8 ? 1 : 0));
      Bytes.insert(Bytes.end(),
                   {0x0f, 0x03,
                    uint8_t(0xc0 | ((Destination & 7) << 3) | (Source & 7))});
      const auto Ops = lift(Bytes);
      ASSERT_FALSE(Ops.empty());
      EXPECT_EQ(Ops.front().Inputs[1], NdVar::reg(Source * 8, 2));
      EXPECT_EQ(Ops.back().Output, NdVar::reg(Destination * 8, 8));
    }
  for (const auto &Bytes : std::vector<std::vector<uint8_t>>{
           {0x0f, 0x00, 0x6c, 0x24, 0x20},
           {0x0f, 0x01, 0x45, 0xf0},
           {0x65, 0x0f, 0x00, 0x2c, 0x25, 0x2a, 0x90, 0x00, 0x00},
           {0x64, 0x67, 0x0f, 0x01, 0x00}})
    EXPECT_FALSE(lift(Bytes).empty());
}

TEST(NativeStateAudit, LockedIncrementAndDecrementHaveOneAtomicTransaction) {
  for (unsigned Width : {1u, 2u, 4u, 8u})
    for (bool Decrement : {false, true}) {
      std::vector<uint8_t> Bytes{0xf0};
      if (Width == 2)
        Bytes.push_back(0x66);
      if (Width == 8)
        Bytes.push_back(0x48);
      Bytes.push_back(Width == 1 ? 0xfe : 0xff);
      Bytes.push_back(Decrement ? 0x08 : 0x00);
      const auto Ops = lift(Bytes);
      unsigned Atomic = 0;
      BinaryImage PrefixImage;
      PrefixImage.Arch = Arch::X64;
      NdOpEmulator Prefix(PrefixImage);
      Prefix.setRegister(x86reg::RAX, 0x8000);
      for (const auto &Op : Ops) {
        EXPECT_NE(Op.Opcode, NdOp::LOAD);
        EXPECT_NE(Op.Opcode, NdOp::STORE);
        EXPECT_FALSE(Op.Output.isReg() && Op.Output.Offset == x86reg::CF);
        if (Op.Opcode == NdOp::ATOMIC_ADD) {
          ++Atomic;
          EXPECT_EQ(Op.MemoryOrdering,
                    NdMemoryOrdering::SequentiallyConsistent);
          EXPECT_EQ(Op.Output.Size, Width);
          const auto Addend = Op.Inputs[1].isConst()
                                  ? std::optional<uint64_t>(Op.Inputs[1].Offset)
                                  : Prefix.getRegister(Op.Inputs[1].Offset);
          ASSERT_TRUE(Addend);
          EXPECT_EQ(*Addend, Decrement ? UINT64_MAX >> (64 - Width * 8) : 1);
        } else if (!Atomic) {
          ASSERT_TRUE(Prefix.step(Op));
        }
      }
      EXPECT_EQ(Atomic, 1u);
    }
}

BinaryImage image(uint64_t Base = MemoryBase, unsigned Size = 256) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Segment Memory;
  Memory.VA = Base;
  Memory.Size = Size;
  Memory.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  for (unsigned I = 0; I != Size; ++I)
    Memory.Data.push_back(uint8_t(19 + I * 29 + I / 7));
  Image.Segments.push_back(std::move(Memory));
  return Image;
}

struct StateOracle {
  std::array<uint64_t, 16> Registers;
  std::array<std::vector<uint8_t>, x86reg::VectorRegCount> Vectors;
  std::array<uint8_t, Flags.size()> FlagValues;
  uint32_t MXCSR = 0x1f80;

  explicit StateOracle(NdOpEmulator &Emulator, unsigned Seed = 0) {
    for (unsigned I = 0; I != Registers.size(); ++I)
      setRegister(Emulator, I * 8, UINT64_C(0xabcdee0012345000) + I * 61);
    for (unsigned I = 0; I != Vectors.size(); ++I) {
      for (unsigned B = 0; B != 64; ++B)
        Vectors[I].push_back(uint8_t(13 + Seed * 71 + I * 53 + B * 29));
      Emulator.setRegisterBytes(x86reg::vectorReg(I), Vectors[I]);
    }
    for (unsigned I = 0; I != Flags.size(); ++I) {
      FlagValues[I] = (Seed + I) % 2;
      Emulator.setRegister(Flags[I], FlagValues[I]);
    }
    Emulator.setMXCSR(MXCSR);
  }

  void setRegister(NdOpEmulator &Emulator, unsigned Offset, uint64_t Value) {
    Registers[Offset / 8] = Value;
    Emulator.setRegister(Offset, Value);
  }

  void setDirection(NdOpEmulator &Emulator, bool Backward) {
    FlagValues[5] = Backward;
    Emulator.setRegister(x86reg::DF, Backward);
  }

  void expect(const NdOpEmulator &Emulator) const {
    for (unsigned I = 0; I != Registers.size(); ++I)
      EXPECT_EQ(Emulator.getRegister(I * 8),
                std::optional<uint64_t>(Registers[I]))
          << "GPR " << I;
    for (unsigned I = 0; I != Vectors.size(); ++I)
      EXPECT_EQ(Emulator.getRegisterBytes(x86reg::vectorReg(I)),
                std::optional<std::vector<uint8_t>>(Vectors[I]))
          << "complete vector " << I;
    for (unsigned I = 0; I != Flags.size(); ++I)
      EXPECT_EQ(Emulator.getRegister(Flags[I]),
                std::optional<uint64_t>(FlagValues[I]));
    EXPECT_EQ(Emulator.getMXCSR(), MXCSR);
  }
};

void expectMemory(NdOpEmulator &Emulator, uint64_t Base,
                  const std::vector<uint8_t> &Expected) {
  for (unsigned I = 0; I != Expected.size(); ++I) {
    LowOp Load;
    Load.Opcode = NdOp::LOAD;
    Load.Output = NdVar::tmp(0x71000000, 1);
    Load.addInput(NdVar::cst(Base + I, 8));
    ASSERT_TRUE(Emulator.step(Load));
    EXPECT_EQ(Emulator.getRegister(Load.Output.Offset),
              std::optional<uint64_t>(Expected[I]))
        << "byte " << I;
  }
}

TEST(X86NativeStateAudit, RepMovsqRetainsOrderedCopyAndCompleteState) {
  const auto Ops = lift({0xf3, 0x48, 0xa5});
  ASSERT_FALSE(Ops.empty());
  const auto It = std::find_if(Ops.begin(), Ops.end(), [](const LowOp &Op) {
    return detail::stringTransferShape(Op).has_value();
  });
  ASSERT_NE(It, Ops.end());
  EXPECT_EQ(It->Inputs[1], NdVar::reg(x86reg::RSI, 8));
  EXPECT_EQ(It->Inputs[2], NdVar::reg(x86reg::RDI, 8));
  EXPECT_EQ(It->Inputs[3], NdVar::reg(x86reg::RCX, 8));
  EXPECT_EQ(It->Inputs[4].Size, X86RepStatusBytes);
  EXPECT_EQ(detail::stringTransferShape(*It)->CompleteState, true);
  for (bool Backward : {false, true})
    for (unsigned Count : {0, 1, 3, 7})
      for (int Gap : {-32, -8, -1, 0, 1, 8, 32})
        for (unsigned Seed : {0, 1}) {
          SCOPED_TRACE(::testing::Message() << Backward << '/' << Count << '/'
                                            << Gap << '/' << Seed);
          BinaryImage Image = image();
          NdOpEmulator Emulator(Image);
          Emulator.setStrictMode(true);
          StateOracle Expected(Emulator, Seed);
          const int Source = Backward ? 160 : 64;
          const int Destination = Source + Gap;
          Expected.setRegister(Emulator, x86reg::RSI, MemoryBase + Source);
          Expected.setRegister(Emulator, x86reg::RDI, MemoryBase + Destination);
          Expected.setRegister(Emulator, x86reg::RCX, Count);
          Expected.setDirection(Emulator, Backward);
          auto Memory = Image.Segments[0].Data;
          // Per-element snapshot deliberately differs from memcpy/memmove:
          // overlap can feed a completed element into the next source read.
          for (unsigned I = 0; I != Count; ++I) {
            const int Delta = (Backward ? -1 : 1) * int(I * 8);
            std::array<uint8_t, 8> Element;
            std::copy_n(Memory.begin() + Source + Delta, 8, Element.begin());
            std::copy(Element.begin(), Element.end(),
                      Memory.begin() + Destination + Delta);
          }
          auto Expanded = detail::lowerStringTransfer(
              *It, Count, Backward, Ops, 0x70000000, Count * 4 + 3);
          ASSERT_TRUE(bool(Expanded)) << llvm::toString(Expanded.takeError());
          Expanded->insert(Expanded->begin(), Ops.begin(), It);
          Expanded->insert(Expanded->end(), It + 1, Ops.end());
          const uint64_t Delta = Backward ? uint64_t(0) - Count * 8 : Count * 8;
          Expected.Registers[x86reg::RSI / 8] += Delta;
          Expected.Registers[x86reg::RDI / 8] += Delta;
          Expected.Registers[x86reg::RCX / 8] = 0;
          EXPECT_EQ(Emulator.run(*Expanded), Expanded->size());
          Expected.expect(Emulator);
          expectMemory(Emulator, MemoryBase, Memory);
        }
}

TEST(X86NativeStateAudit, AccessCheckHasNoArbitraryOrOpaqueStateCertificate) {
  for (uint8_t Opcode : {uint8_t{0xca}, uint8_t{0xcb}}) {
    const auto Ops = lift({0x0f, 0x01, Opcode});
    ASSERT_EQ(Ops.size(), 1u);
    EXPECT_EQ(Ops.front().Opcode, NdOp::INTRINSIC);
    ASSERT_EQ(Ops.front().NumInputs, 1u);
    EXPECT_EQ(Ops.front().Inputs[0].Offset,
              static_cast<uint64_t>(Opcode == 0xca ? Intrinsic::Clac
                                                   : Intrinsic::Stac));
    EXPECT_EQ(Ops.front().Output.Size, 0u);
  }
}

TEST(X86NativeStateAudit, AccessCheckRequiresMatchingCanonicalBytesAndDetail) {
  for (uint8_t Opcode : {uint8_t{0xca}, uint8_t{0xcb}}) {
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X64));
    const uint8_t Bytes[]{0x0f, 0x01, Opcode};
    DecodedInsn Insn;
    ASSERT_EQ(Dec.decodeOneForLift(Bytes, sizeof(Bytes), CodeAddress, Insn), 3);
    ASSERT_NE(Insn.Raw, nullptr);
    ASSERT_TRUE(nativestateaudit::accessCheckForm(Insn.Raw, Arch::X64));
    EXPECT_FALSE(nativestateaudit::accessCheckForm(Insn.Raw, Arch::X86));
    for (unsigned Mutation = 0; Mutation != 12; ++Mutation) {
      cs_insn Copy = *Insn.Raw;
      cs_detail Detail = *Copy.detail;
      Copy.detail = &Detail;
      auto &X = Detail.x86;
      switch (Mutation) {
      case 0:
        Copy.bytes[2] ^= 1;
        break;
      case 1:
        Copy.size = 4;
        break;
      case 2:
        Copy.id = X86_INS_SWAPGS;
        break;
      case 3:
        X.modrm ^= 1;
        break;
      case 4:
        X.prefix[0] = 0xf0;
        break;
      case 5:
        X.rex = 0x48;
        break;
      case 6:
        X.addr_size = 4;
        break;
      case 7:
        X.encoding.modrm_offset = 1;
        break;
      case 8:
        X.op_count = 1;
        break;
      case 9:
        X.eflags ^= X86_EFLAGS_UNDEFINED_CF;
        break;
      case 10:
        X.opcode[2] = 1;
        break;
      case 11:
        X.avx_sae = true;
        break;
      }
      EXPECT_FALSE(nativestateaudit::accessCheckForm(&Copy, Arch::X64))
          << "mutation " << Mutation;
    }
  }
}

TEST(X86NativeStateAudit, RepMovsqZeroCountAvoidsMemoryAndBudgetIsExact) {
  const auto Ops = lift({0xf3, 0x48, 0xa5});
  ASSERT_FALSE(Ops.empty());
  const auto It = std::find_if(Ops.begin(), Ops.end(), [](const LowOp &Op) {
    return detail::stringTransferShape(Op).has_value();
  });
  ASSERT_NE(It, Ops.end());
  auto Exact = detail::lowerStringTransfer(*It, 7, true, Ops, 0x70000000, 31);
  ASSERT_TRUE(bool(Exact)) << llvm::toString(Exact.takeError());
  for (uint64_t Count : {uint64_t(7), UINT64_MAX}) {
    auto Refused =
        detail::lowerStringTransfer(*It, Count, true, Ops, 0x70000000, 30);
    EXPECT_FALSE(bool(Refused));
    if (!Refused)
      llvm::consumeError(Refused.takeError());
  }
  for (bool Backward : {false, true}) {
    auto Zero =
        detail::lowerStringTransfer(*It, 0, Backward, Ops, 0x70000000, 3);
    ASSERT_TRUE(bool(Zero)) << llvm::toString(Zero.takeError());
    Zero->insert(Zero->begin(), Ops.begin(), It);
    Zero->insert(Zero->end(), It + 1, Ops.end());
    for (const auto &Op : *Zero) {
      EXPECT_NE(Op.Opcode, NdOp::LOAD);
      EXPECT_NE(Op.Opcode, NdOp::STORE);
    }
    BinaryImage Image = image();
    Image.Segments.clear();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    StateOracle Expected(Emulator);
    Expected.setRegister(Emulator, x86reg::RSI, UINT64_MAX);
    Expected.setRegister(Emulator, x86reg::RDI, 0);
    Expected.setRegister(Emulator, x86reg::RCX, 0);
    Expected.setDirection(Emulator, Backward);
    EXPECT_EQ(Emulator.run(*Zero), Zero->size());
    Expected.expect(Emulator);
  }
}

class TransferProvider : public SpecializationProvider {
public:
  std::map<va_t, SpecializationInstruction> Code;

  void add(va_t Address, unsigned Bytes, std::vector<LowOp> Ops) {
    SpecializationInstruction Instruction;
    Instruction.Origin.Address = Address;
    Instruction.Origin.Size = Bytes;
    Instruction.Origin.OpCount = Ops.size();
    Instruction.Fallthrough = {Address + Bytes};
    for (size_t I = 0; I != Ops.size(); ++I) {
      Ops[I].Addr = Address;
      Ops[I].Seq = int(I);
      if (Ops[I].Opcode == NdOp::RETURN) {
        Instruction.Origin.Control = LowInstructionControl::Return;
        Instruction.Origin.ControlFlags = LowInstructionControlFlag::Return;
      }
    }
    Instruction.Ops = std::move(Ops);
    Code.emplace(Address, std::move(Instruction));
  }

  llvm::Expected<SpecializationInstruction>
  instruction(SpecializationCursor Cursor) override {
    const auto Found = Code.find(Cursor.Address);
    if (Found == Code.end())
      return llvm::createStringError(llvm::errc::invalid_argument,
                                     "missing state-audit test instruction");
    return Found->second;
  }

  std::optional<SpecializationImmutableRead> immutableRead(va_t,
                                                           uint16_t) override {
    return std::nullopt;
  }
};

TEST(X86NativeStateAudit, RepAdmissionStillRequiresCountAndDirectionProofs) {
  const auto Ops = lift({0xf3, 0x48, 0xa5});
  ASSERT_FALSE(Ops.empty());
  for (unsigned Case = 0; Case != 3; ++Case) {
    TransferProvider Provider;
    LowOp Entry;
    Entry.Opcode = Case ? NdOp::COPY : NdOp::NOP;
    if (Case) {
      Entry.Output = NdVar::reg(x86reg::RCX, 8);
      Entry.addInput(NdVar::scalar(Case == 1 ? 1 : 0, 8));
    }
    Provider.add(CodeAddress - 1, 1, {Entry});
    Provider.add(CodeAddress, 3, Ops);
    LowOp Return;
    Return.Opcode = NdOp::RETURN;
    Provider.add(CodeAddress + 3, 1, {Return});
    SpecializationOptions Options;
    Options.ExplicitMachineState = true;
    Options.NormalNonfaultingExecution = true;
    const auto Result =
        specializeInterpreter(Provider, {CodeAddress - 1}, Options);
    if (Case == 2) {
      // Zero count needs neither a direction proof nor mapped source bytes.
      EXPECT_TRUE(Result.complete()) << Result.Diagnostic;
    } else {
      EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported)
          << Result.Diagnostic;
      EXPECT_NE(Result.Diagnostic.find("proved count and direction"),
                std::string::npos);
      EXPECT_TRUE(Result.Residual.Blocks.empty());
    }
  }
}

struct LoadCase {
  std::vector<uint8_t> Bytes;
  std::vector<std::pair<unsigned, uint64_t>> Registers;
  uint64_t Address;
  uint64_t Base = MemoryBase;
};

TEST(X86NativeStateAudit, LdmxcsrReadsFourExactBytesAndPreservesOtherState) {
  const LoadCase Cases[] = {
      {{0x0f, 0xae, 0x10}, {{x86reg::RAX, MemoryBase + 1}}, MemoryBase + 1},
      {{0x0f, 0xae, 0x54, 0x4b, 0xfd},
       {{x86reg::RBX, MemoryBase + 1}, {x86reg::RCX, 2}},
       MemoryBase + 2},
      {{0x41, 0x0f, 0xae, 0x10},
       {{x86reg::R8, MemoryBase + 3}},
       MemoryBase + 3},
      {{0x42, 0x0f, 0xae, 0x14, 0},
       {{x86reg::RAX, MemoryBase}, {x86reg::R8, 4}},
       MemoryBase + 4},
      {{0x0f, 0xae, 0x15, 0xfe, 0x6f, 0, 0}, {}, MemoryBase + 5},
      {{0x67, 0x0f, 0xae, 0x15, 0xfe, 0x6f, 0, 0}, {}, MemoryBase + 6},
      {{0x67, 0x0f, 0xae, 0x14, 0x25, 7, 0x80, 0, 0}, {}, MemoryBase + 7},
      {{0x67, 0x0f, 0xae, 0x50, 0x40},
       {{x86reg::RAX, UINT64_C(0xcafebabefffffff0)}},
       0x30,
       0x20}};
  for (const auto &C : Cases) {
    const auto Ops = lift(C.Bytes);
    ASSERT_FALSE(Ops.empty());
    ASSERT_EQ(Ops.back().Opcode, NdOp::INTRINSIC);
    EXPECT_EQ(Ops.back().Inputs[0].Offset, uint64_t(Intrinsic::Ldmxcsr));
    EXPECT_EQ(Ops.back().MemoryAddressSpace, NdMemoryAddressSpace::Default);
    for (uint32_t Word :
         {0U, 0x1f80U, 0x5f80U, 0xffbfU, 0x10000U, UINT32_MAX}) {
      BinaryImage Image = image(C.Base);
      for (unsigned I = 0; I != 4; ++I)
        Image.Segments[0].Data[C.Address - C.Base + I] =
            uint8_t(Word >> (8 * I));
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(true);
      StateOracle Expected(Emulator);
      for (const auto &[Offset, Value] : C.Registers)
        Expected.setRegister(Emulator, Offset, Value);
      const bool Reserved = (Word & 0xffff0000U) != 0;
      if (!Reserved)
        Expected.MXCSR = Word;
      EXPECT_EQ(Emulator.run(Ops), Ops.size() - unsigned(Reserved));
      Expected.expect(Emulator);
      expectMemory(Emulator, C.Base, Image.Segments[0].Data);
    }
  }
}

TEST(X86NativeStateAudit, LdmxcsrMissingBytesDoNotCommitState) {
  const auto Ops = lift({0x0f, 0xae, 0x10});
  ASSERT_FALSE(Ops.empty());
  for (unsigned Available = 0; Available != 4; ++Available) {
    BinaryImage Image = image(MemoryBase, Available);
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    StateOracle Expected(Emulator);
    Expected.setRegister(Emulator, x86reg::RAX, MemoryBase);
    EXPECT_EQ(Emulator.run(Ops), Ops.size() - 1);
    Expected.expect(Emulator);
    expectMemory(Emulator, MemoryBase, Image.Segments[0].Data);
  }
}

TEST(X86NativeStateAudit,
     AdmissionDoesNotSupplyMXCSRContextOrReservedBitProof) {
  const auto Ops = lift({0x0f, 0xae, 0x10});
  ASSERT_FALSE(Ops.empty());
  const auto &Load = Ops.back();
  EXPECT_FALSE(supportsRuntimeControlOperation(Load, {}));
  for (uint32_t Mask : {0xffbfU, 0xffffU})
    for (uint32_t Value : {0x1f80U, 0x40U, 0xffffU, 0x10000U}) {
      SpecializationOptions Options;
      Options.ExplicitMachineState = true;
      Options.NormalNonfaultingExecution = true;
      Options.X64MXCSRMask = Mask;
      EXPECT_TRUE(supportsRuntimeControlOperation(Load, Options));
      EXPECT_EQ(llvm::toString(validateRuntimeControlOptions(Options)), "");
      SymContext Ctx;
      SymState State(Ctx);
      State.write(SymSpace::Register, x86reg::RAX, Ctx.mkConst(64, MemoryBase));
      State.store(Ctx.mkConst(64, MemoryBase), Ctx.mkConst(32, Value));
      SymExec Exec(Ctx, State);
      for (size_t I = 0; I + 1 < Ops.size(); ++I)
        ASSERT_EQ(Exec.step(Ops[I]), StepResult::Continue);
      RuntimeControlState Controls{{Ctx.mkConst(32, 0x1f80)}};
      uint64_t Next = 0x70000000;
      auto Transition = lowerRuntimeControlOperation(
          Load, State, Controls, Options, [&](uint16_t Bytes) {
            const auto Temporary = NdVar::tmp(Next, Bytes);
            Next += Bytes;
            return Temporary;
          });
      ASSERT_TRUE(Transition);
      ASSERT_TRUE(Transition->Rejected);
      ASSERT_TRUE(Transition->OutputState);
      EXPECT_EQ(Exec.run(Transition->Ops), Transition->Ops.size());
      const auto Bad = Ctx.asConst(Exec.operandValue(*Transition->Rejected));
      ASSERT_TRUE(Bad);
      EXPECT_EQ(Bad->getZExtValue(), uint64_t((Value & ~Mask) != 0));
      const auto Final =
          Ctx.asConst(Exec.operandValue(Transition->OutputState->second));
      ASSERT_TRUE(Final);
      EXPECT_EQ(Final->getZExtValue(), (Value & ~Mask) ? 0x1f80U : Value);
    }
}

void expectUnaudited(Decoder &Dec, DecodedInsn &Insn) {
  std::vector<LowOp> Ops;
  LowInstructionUndefinedEffects Effects;
  Effects.Coverage = LowUndefinedCoverage::Complete;
  Effects.OperationDigest = "stale";
  Effects.Effects.push_back({1, NdVar::reg(x86reg::AF, 1), 0, 1, {}});
  LowInstructionPreservedState Preserved;
  Preserved.Audit = LowPreservedStateAudit::LegacyIntegerV1;
  try {
    Dec.liftToLow(Insn, Ops, {}, {}, &Effects, &Preserved);
  } catch (const UnliftedInstruction &) {
    // Semantic rejection also leaves undefined-output admission closed.
  }
  EXPECT_NE(Effects.Coverage, LowUndefinedCoverage::Complete);
  EXPECT_TRUE(Effects.Effects.empty());
  EXPECT_NE(Effects.OperationDigest, "stale");
  EXPECT_EQ(Preserved, LowInstructionPreservedState{});
}

TEST(X86NativeStateAudit, LegacyMsrAuditRejectsChangedEncodingAndDetail) {
  using Mutation = void (*)(cs_insn &);
  const Mutation Mutations[] = {
      [](cs_insn &I) { I.bytes[1] ^= 2; },
      [](cs_insn &I) { I.bytes[I.size++] = 0x90; },
      [](cs_insn &I) { I.detail->x86.op_count = 1; },
      [](cs_insn &I) { I.detail->x86.opcode[1] ^= 2; },
      [](cs_insn &I) { I.detail->x86.prefix[0] = 0xf0; },
      [](cs_insn &I) { I.detail->x86.addr_size = 4; },
      [](cs_insn &I) { I.detail->x86.rex = 0x48; },
      [](cs_insn &I) { I.detail->x86.encoding.modrm_offset = 1; },
      [](cs_insn &I) { I.detail->x86.eflags = X86_EFLAGS_MODIFY_CF; }};
  for (uint8_t Opcode : {0x30, 0x32}) {
    const uint8_t Bytes[] = {0x0f, Opcode};
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X64));
    for (const auto Mutate : Mutations) {
      DecodedInsn Insn{};
      ASSERT_EQ(Dec.decodeOneForLift(Bytes, sizeof(Bytes), CodeAddress, Insn),
                2);
      Mutate(*const_cast<cs_insn *>(Insn.Raw));
      expectUnaudited(Dec, Insn);
    }
  }
}

TEST(X86NativeStateAudit, DescriptorAuditRejectsChangedMemoryAndRegisters) {
  using Mutation = void (*)(cs_insn &);
  const Mutation Mutations[] = {
      [](cs_insn &I) { I.bytes[1] ^= 2; },
      [](cs_insn &I) { I.bytes[I.size++] = 0x90; },
      [](cs_insn &I) { ++I.detail->x86.op_count; },
      [](cs_insn &I) { ++I.detail->x86.operands[0].size; },
      [](cs_insn &I) { I.detail->x86.prefix[0] = 0xf0; },
      [](cs_insn &I) { I.detail->x86.addr_size = 4; },
      [](cs_insn &I) { I.detail->x86.rex = 0x48; },
      [](cs_insn &I) { I.detail->x86.encoding.modrm_offset = 1; },
      [](cs_insn &I) { I.detail->x86.eflags ^= X86_EFLAGS_MODIFY_CF; }};
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  for (const auto &Bytes :
       std::vector<std::vector<uint8_t>>{{0x0f, 0x03, 0xc0},
                                         {0x0f, 0x00, 0x6c, 0x24, 0x20},
                                         {0x0f, 0x01, 0x45, 0xf0}})
    for (const auto Mutate : Mutations) {
      DecodedInsn Insn{};
      ASSERT_EQ(
          Dec.decodeOneForLift(Bytes.data(), Bytes.size(), CodeAddress, Insn),
          int(Bytes.size()));
      Mutate(*const_cast<cs_insn *>(Insn.Raw));
      expectUnaudited(Dec, Insn);
    }
}

TEST(X86NativeStateAudit, OtherPrefixesModesAndInstructionFamiliesStayClosed) {
  const std::vector<std::vector<uint8_t>> Cases = {
      {0x48, 0xa5},
      {0xf3, 0xa5},
      {0xf3, 0xa4},
      {0xf3, 0x48, 0xab},
      {0xf2, 0x48, 0xa5},
      {0xf3, 0xf3, 0x48, 0xa5},
      {0x67, 0xf3, 0x48, 0xa5},
      {0x64, 0xf3, 0x48, 0xa5},
      {0x65, 0xf3, 0x48, 0xa5},
      {0xf3, 0x49, 0xa5},
      {0xc5, 0xf8, 0xae, 0x10},
      {0x66, 0x0f, 0xae, 0x10},
      {0x67, 0x67, 0x0f, 0xae, 0x10},
      {0x40, 0x40, 0x0f, 0xae, 0x10},
      {0x44, 0x0f, 0xae, 0x10},
      {0x48, 0x0f, 0xae, 0x10},
      {0x64, 0x64, 0x0f, 0xae, 0x10},
      {0x65, 0x65, 0x0f, 0xae, 0x10}};
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  for (const auto &Bytes : Cases) {
    DecodedInsn Insn{};
    const int Size =
        Dec.decodeOneForLift(Bytes.data(), Bytes.size(), CodeAddress, Insn);
    if (Size <= 0)
      continue;
    ASSERT_EQ(Size, int(Bytes.size()));
    SCOPED_TRACE(Insn.Raw->mnemonic);
    expectUnaudited(Dec, Insn);
  }
  Decoder Narrow;
  ASSERT_TRUE(Narrow.init(Arch::X86));
  const uint8_t Bytes[] = {0x0f, 0xae, 0x10};
  DecodedInsn Insn{};
  ASSERT_EQ(Narrow.decodeOneForLift(Bytes, sizeof(Bytes), CodeAddress, Insn),
            3);
  expectUnaudited(Narrow, Insn);
}

TEST(X86NativeStateAudit, RawAndDetailDisagreementCannotAuthorizeEffects) {
  using Mutation = void (*)(cs_insn &);
  const Mutation Common[] = {
      [](cs_insn &I) { I.bytes[I.size++] = 0x90; },
      [](cs_insn &I) { I.detail->x86.op_count += 1; },
      [](cs_insn &I) { I.detail->x86.operands[0].size = 1; },
      [](cs_insn &I) { I.detail->x86.addr_size = 4; },
      [](cs_insn &I) { I.detail->x86.rex ^= 1; },
      [](cs_insn &I) { I.detail->x86.opcode[0] ^= 1; },
      [](cs_insn &I) { I.detail->x86.prefix[0] ^= 1; },
      [](cs_insn &I) { I.detail->x86.operands[0].mem.base = X86_REG_RDX; },
      [](cs_insn &I) { I.detail->x86.operands[0].mem.index = X86_REG_R9; },
      [](cs_insn &I) { I.detail->x86.operands[0].mem.scale = 3; },
      [](cs_insn &I) { I.detail->x86.operands[0].mem.disp += 1; },
      [](cs_insn &I) { I.detail->x86.operands[0].mem.segment = X86_REG_FS; },
      [](cs_insn &I) { I.detail->x86.encoding.imm_size = 1; },
      [](cs_insn &I) { I.detail->x86.operands[0].avx_zero_opmask = true; }};
  const Mutation Rep[] = {
      [](cs_insn &I) { I.bytes[0] = 0xf2; },
      [](cs_insn &I) { I.bytes[1] = 0x40; },
      [](cs_insn &I) { I.bytes[2] = 0xa4; },
      [](cs_insn &I) { I.id = X86_INS_STOSQ; },
      [](cs_insn &I) { I.detail->x86.operands[1].mem.base = X86_REG_RDI; },
      [](cs_insn &I) { I.detail->x86.operands[1].mem.segment = X86_REG_GS; },
      [](cs_insn &I) { I.detail->x86.operands[1].size = 4; },
      [](cs_insn &I) { I.detail->x86.encoding.modrm_offset = 1; }};
  const Mutation MXCSR[] = {
      [](cs_insn &I) { I.bytes[2] ^= 8; },
      [](cs_insn &I) { I.bytes[3] ^= 8; },
      [](cs_insn &I) { I.id = X86_INS_STMXCSR; },
      [](cs_insn &I) { I.detail->x86.modrm ^= 8; },
      [](cs_insn &I) { I.detail->x86.encoding.modrm_offset += 1; },
      [](cs_insn &I) { I.detail->x86.encoding.disp_offset -= 1; },
      [](cs_insn &I) { I.detail->x86.encoding.disp_size = 4; }};
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  for (bool String : {false, true}) {
    const std::vector<uint8_t> Bytes =
        String ? std::vector<uint8_t>{0xf3, 0x48, 0xa5}
               : std::vector<uint8_t>{0x0f, 0xae, 0x54, 0x4b, 0xfd};
    const auto Specific = String ? llvm::ArrayRef<Mutation>(Rep)
                                 : llvm::ArrayRef<Mutation>(MXCSR);
    std::vector<Mutation> Mutations(std::begin(Common), std::end(Common));
    Mutations.insert(Mutations.end(), Specific.begin(), Specific.end());
    for (unsigned Index = 0; Index != Mutations.size(); ++Index) {
      SCOPED_TRACE(::testing::Message() << String << '/' << Index);
      DecodedInsn Insn{};
      ASSERT_EQ(
          Dec.decodeOneForLift(Bytes.data(), Bytes.size(), CodeAddress, Insn),
          int(Bytes.size()));
      Mutations[Index](*Insn.Raw);
      expectUnaudited(Dec, Insn);
    }
  }
}

TEST(X86NativeStateAudit, MXCSRStoreAndSegmentFormsHaveCompleteFlagCoverage) {
  for (uint8_t Opcode : {0x10, 0x18})
    for (uint8_t Segment : {0, 0x64, 0x65}) {
      std::vector<uint8_t> Bytes{0x0f, 0xae, Opcode};
      if (Segment)
        Bytes.insert(Bytes.begin(), Segment);
      EXPECT_FALSE(lift(Bytes).empty());
    }
}

TEST(X86NativeStateAudit, CR8EncodingBindsSixArbitraryFlagsWithoutOpaqueProof) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  Dec.setStrict(true);
  for (uint8_t Register = 0; Register != 16; ++Register)
    for (uint8_t Opcode : {0x20, 0x22}) {
      const uint8_t Bytes[] = {uint8_t(Register < 8 ? 0x44 : 0x45), 0x0f,
                               Opcode, uint8_t(0xc0 | (Register & 7))};
      DecodedInsn Insn{};
      ASSERT_EQ(Dec.decodeOneForLift(Bytes, sizeof(Bytes), CodeAddress, Insn),
                4);
      std::vector<LowOp> Ops;
      LowInstructionUndefinedEffects Effects;
      LowInstructionPreservedState Preserved;
      Dec.liftToLow(Insn, Ops, {}, {}, &Effects, &Preserved);
      ASSERT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
          << Effects.Diagnostic;
      ASSERT_EQ(Effects.Effects.size(), 6u);
      const unsigned Undefined[] = {x86reg::OF, x86reg::SF, x86reg::ZF,
                                    x86reg::AF, x86reg::PF, x86reg::CF};
      for (unsigned I = 0; I != 6; ++I) {
        EXPECT_EQ(Effects.Effects[I].Output, NdVar::reg(Undefined[I], 1));
        EXPECT_EQ(Effects.Effects[I].BitCount, 1u);
        EXPECT_EQ(Effects.Effects[I].AfterOp, Ops.size());
      }
      EXPECT_EQ(Preserved, LowInstructionPreservedState{});
      Insn.Raw->detail->x86.operands[Opcode == 0x20 ? 1 : 0].reg = X86_REG_CR2;
      expectUnaudited(Dec, Insn);
    }
}

TEST(X86NativeStateAudit, CR2EncodingBindsSixArbitraryFlagsWithoutOpaqueProof) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  Dec.setStrict(true);
  for (uint8_t Register = 0; Register != 16; ++Register)
    for (uint8_t Opcode : {0x20, 0x22}) {
      std::vector<uint8_t> Bytes{0x0f, Opcode, uint8_t(0xd0 | (Register & 7))};
      if (Register >= 8)
        Bytes.insert(Bytes.begin(), 0x41);
      DecodedInsn Insn{};
      ASSERT_EQ(
          Dec.decodeOneForLift(Bytes.data(), Bytes.size(), CodeAddress, Insn),
          int(Bytes.size()));
      std::vector<LowOp> Ops;
      LowInstructionUndefinedEffects Effects;
      LowInstructionPreservedState Preserved;
      Dec.liftToLow(Insn, Ops, {}, {}, &Effects, &Preserved);
      ASSERT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
          << Effects.Diagnostic;
      ASSERT_EQ(Effects.Effects.size(), 6u);
      for (const auto &Effect : Effects.Effects) {
        EXPECT_EQ(Effect.BitCount, 1u);
        EXPECT_EQ(Effect.AfterOp, Ops.size());
      }
      EXPECT_EQ(Preserved, LowInstructionPreservedState{});
      Insn.Raw->detail->x86.operands[Opcode == 0x20 ? 1 : 0].reg = X86_REG_CR3;
      expectUnaudited(Dec, Insn);
    }
}

TEST(X86NativeStateAudit, SysretAuditBindsEncodingAndCompleteDefinedFlags) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  Dec.setStrict(true);
  const uint8_t Bytes[] = {0x48, 0x0f, 0x07};
  const auto Decode = [&] {
    DecodedInsn I{};
    EXPECT_EQ(Dec.decodeOneForLift(Bytes, sizeof(Bytes), CodeAddress, I), 3);
    return I;
  };
  auto Insn = Decode();
  std::vector<LowOp> Ops;
  LowInstructionUndefinedEffects Effects;
  LowInstructionPreservedState Preserved;
  Dec.liftToLow(Insn, Ops, {}, {}, &Effects, &Preserved);
  EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
      << Effects.Diagnostic;
  EXPECT_TRUE(Effects.Effects.empty());
  EXPECT_EQ(Preserved, LowInstructionPreservedState{});
  Insn.Raw->bytes[0] = 0x40;
  expectUnaudited(Dec, Insn);
  Insn = Decode();
  Insn.Raw->detail->x86.rex = 0;
  expectUnaudited(Dec, Insn);
  Insn = Decode();
  Insn.Raw->detail->x86.eflags = 0;
  expectUnaudited(Dec, Insn);
  Insn = Decode();
  Insn.Raw->detail->x86.op_count = 1;
  expectUnaudited(Dec, Insn);
}

TEST(X86NativeStateAudit, FastSystemFormsBindBytesAndCompatibilityHighHalves) {
  for (unsigned Kind = 0; Kind != 3; ++Kind) {
    const std::vector<uint8_t> Bytes =
        Kind == 0   ? std::vector<uint8_t>{0x0f, 0x34}
        : Kind == 1 ? std::vector<uint8_t>{0x0f, 0x35}
                    : std::vector<uint8_t>{0x48, 0x0f, 0x35};
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X64));
    Dec.setStrict(true);
    const auto Decode = [&] {
      DecodedInsn I{};
      EXPECT_EQ(
          Dec.decodeOneForLift(Bytes.data(), Bytes.size(), CodeAddress, I),
          int(Bytes.size()));
      return I;
    };
    auto Insn = Decode();
    ASSERT_TRUE(nativestateaudit::fastSystemForm(Insn.Raw, Arch::X64));
    EXPECT_FALSE(nativestateaudit::fastSystemForm(Insn.Raw, Arch::X86));
    std::vector<LowOp> Ops;
    LowInstructionUndefinedEffects Effects;
    LowInstructionPreservedState Preserved;
    Dec.liftToLow(Insn, Ops, {}, {}, &Effects, &Preserved);
    ASSERT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
        << Effects.Diagnostic;
    EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Ops));
    EXPECT_EQ(Effects.Effects.size(), Kind == 1 ? 8u : 0u);
    EXPECT_EQ(Preserved, LowInstructionPreservedState{});
    EXPECT_TRUE(Dec.isFunctionTerminator(Insn));
    if (Kind == 1) {
      EXPECT_TRUE(compatibilityreturn::matches(Ops, Effects));
      for (unsigned I = 0; I < 8; ++I) {
        auto Bad = Effects;
        Bad.Effects[I].Output = NdVar::reg(x86reg::R8, 8);
        EXPECT_FALSE(compatibilityreturn::matches(Ops, Bad));
        Bad = Effects;
        Bad.Effects[I].BitOffset = 0;
        EXPECT_FALSE(compatibilityreturn::matches(Ops, Bad));
      }
      auto Bad = Ops;
      Bad[0].Inputs[2] = NdVar::reg(x86reg::R11, 4);
      EXPECT_FALSE(compatibilityreturn::matches(Bad, Effects));
    }
    for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
      Insn = Decode();
      auto &X = Insn.Raw->detail->x86;
      switch (Mutation) {
      case 0:
        Insn.Raw->bytes[Insn.Raw->size - 1] ^= 1;
        break;
      case 1:
        X.rex ^= 0x40;
        break;
      case 2:
        X.eflags = 0;
        break;
      case 3:
        X.op_count = 1;
        break;
      case 4:
        X.addr_size = 4;
        break;
      case 5:
        X.encoding.imm_size = 1;
        break;
      case 6:
        X.prefix[1] = X86_PREFIX_FS;
        break;
      case 7:
        X.opcode[1] ^= 1;
        break;
      }
      EXPECT_FALSE(nativestateaudit::fastSystemForm(Insn.Raw, Arch::X64));
      expectUnaudited(Dec, Insn);
    }
  }
}

TEST(X86NativeStateAudit, SysretCompatibilityBindsEveryArbitraryHighHalf) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  Dec.setStrict(true);
  const uint8_t Bytes[] = {0x0f, 0x07};
  const auto Decode = [&] {
    DecodedInsn I{};
    EXPECT_EQ(Dec.decodeOneForLift(Bytes, sizeof(Bytes), CodeAddress, I), 2);
    return I;
  };
  auto Insn = Decode();
  std::vector<LowOp> Ops;
  LowInstructionUndefinedEffects Effects;
  LowInstructionPreservedState Preserved;
  Dec.liftToLow(Insn, Ops, {}, {}, &Effects, &Preserved);
  ASSERT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
      << Effects.Diagnostic;
  ASSERT_EQ(Effects.OpCount, 1u);
  ASSERT_EQ(Effects.Effects.size(), 8u);
  EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Ops));
  EXPECT_EQ(Preserved, LowInstructionPreservedState{});
  EXPECT_TRUE(compatibilityreturn::matches(Ops, Effects));
  for (unsigned I = 0; I != 8; ++I) {
    const auto &E = Effects.Effects[I];
    EXPECT_EQ(E.AfterOp, 1u);
    EXPECT_EQ(E.Output, NdVar::reg(I * 8, 8));
    EXPECT_EQ(E.BitOffset, 32u);
    EXPECT_EQ(E.BitCount, 32u);
    EXPECT_FALSE(E.When);
    for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
      auto Bad = Effects;
      auto &Changed = Bad.Effects[I];
      switch (Mutation) {
      case 0:
        Changed.AfterOp = 0;
        break;
      case 1:
        Changed.Output = NdVar::reg(x86reg::R8, 8);
        break;
      case 2:
        Changed.BitOffset = 0;
        break;
      case 3:
        Changed.BitCount = 31;
        break;
      case 4:
        Changed.When = NdVar::cst(1, 1);
        break;
      case 5:
        Bad.Effects.erase(Bad.Effects.begin() + I);
        break;
      }
      EXPECT_FALSE(compatibilityreturn::matches(Ops, Bad));
    }
  }
  auto Extra = Effects;
  Extra.Effects.push_back(Effects.Effects.front());
  EXPECT_FALSE(compatibilityreturn::matches(Ops, Extra));
  for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
    Insn = Decode();
    auto &X = Insn.Raw->detail->x86;
    switch (Mutation) {
    case 0:
      Insn.Raw->bytes[1] ^= 1;
      break;
    case 1:
      Insn.Raw->id = X86_INS_SYSRETQ;
      break;
    case 2:
      Insn.Raw->size = 3;
      break;
    case 3:
      X.rex = 0x48;
      break;
    case 4:
      X.prefix[2] = 0x66;
      break;
    case 5:
      X.addr_size = 4;
      break;
    case 6:
      X.op_count = 1;
      break;
    case 7:
      X.eflags = 0;
      break;
    case 8:
      X.opcode[1] ^= 1;
      break;
    }
    expectUnaudited(Dec, Insn);
  }
}

TEST(X86NativeStateAudit, LongModeEventsBindExactBytesWithoutOpaqueProof) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  Dec.setStrict(true);
  for (const auto &Bytes :
       std::vector<std::vector<uint8_t>>{{0x48, 0xcf}, {0xcc}}) {
    SCOPED_TRACE(Bytes.size());
    EXPECT_FALSE(lift(Bytes, Bytes.size() == 2 ? LowUndefinedCoverage::Missing
                                               : LowUndefinedCoverage::Complete)
                     .empty());
    const auto Decode = [&] {
      DecodedInsn I{};
      EXPECT_EQ(
          Dec.decodeOneForLift(Bytes.data(), Bytes.size(), CodeAddress, I),
          int(Bytes.size()));
      return I;
    };
    auto Insn = Decode();
    EXPECT_TRUE(nativestateaudit::longModeEventForm(Insn.Raw, Arch::X64));
    EXPECT_FALSE(nativestateaudit::longModeEventForm(Insn.Raw, Arch::X86));
    EXPECT_EQ(nativestateaudit::form(Insn.Raw, Arch::X64), Bytes.size() == 1);
    Insn.Raw->bytes[0] ^= 1;
    EXPECT_FALSE(nativestateaudit::longModeEventForm(Insn.Raw, Arch::X64));
    expectUnaudited(Dec, Insn);
    Insn = Decode();
    Insn.Raw->detail->x86.eflags = 0;
    EXPECT_FALSE(nativestateaudit::longModeEventForm(Insn.Raw, Arch::X64));
    expectUnaudited(Dec, Insn);
    Insn = Decode();
    Insn.Raw->detail->x86.op_count = 1;
    EXPECT_FALSE(nativestateaudit::longModeEventForm(Insn.Raw, Arch::X64));
    expectUnaudited(Dec, Insn);
    Insn = Decode();
    Insn.Raw->detail->x86.prefix[0] = 0xf3;
    EXPECT_FALSE(nativestateaudit::longModeEventForm(Insn.Raw, Arch::X64));
    expectUnaudited(Dec, Insn);
  }
}

TEST(X86NativeStateAudit, InterruptVectorsBindUnsignedImmediateAndExtent) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  Dec.setStrict(true);
  for (unsigned Vector = 0; Vector != 256; ++Vector) {
    SCOPED_TRACE(Vector);
    const std::vector<uint8_t> Bytes{0xcd, uint8_t(Vector)};
    EXPECT_FALSE(lift(Bytes).empty());
    const auto Decode = [&] {
      DecodedInsn I{};
      EXPECT_EQ(
          Dec.decodeOneForLift(Bytes.data(), Bytes.size(), CodeAddress, I),
          int(Bytes.size()));
      return I;
    };
    auto Insn = Decode();
    EXPECT_TRUE(nativestateaudit::longModeEventForm(Insn.Raw, Arch::X64));
    EXPECT_FALSE(nativestateaudit::longModeEventForm(Insn.Raw, Arch::X86));
    for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
      SCOPED_TRACE(Mutation);
      Insn = Decode();
      auto &X = Insn.Raw->detail->x86;
      switch (Mutation) {
      case 0:
        Insn.Raw->bytes[1] ^= 1;
        break;
      case 1:
        X.operands[0].imm = int64_t(Vector) - 256;
        break;
      case 2:
        X.operands[0].size = 8;
        break;
      case 3:
        X.operands[0].access = CS_AC_READ;
        break;
      case 4:
        X.encoding.imm_offset = 0;
        break;
      case 5:
        X.encoding.imm_size = 2;
        break;
      case 6:
        X.op_count = 0;
        break;
      case 7:
        X.eflags = 0;
        break;
      case 8:
        X.prefix[0] = 0xf3;
        break;
      }
      expectUnaudited(Dec, Insn);
    }
  }
}

TEST(X86NativeStateAudit, FarReturnsBindCleanupEncodingWithoutOpaqueProof) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  Dec.setStrict(true);
  for (unsigned Value : {65536u, 0u, 1u, 255u, 32767u, 32768u, 65535u}) {
    SCOPED_TRACE(Value);
    const std::vector<uint8_t> Bytes =
        Value == 65536 ? std::vector<uint8_t>{0x48, 0xcb}
                       : std::vector<uint8_t>{0x48, 0xca, uint8_t(Value),
                                              uint8_t(Value >> 8)};
    const auto Ops = lift(Bytes, LowUndefinedCoverage::Missing);
    ASSERT_EQ(Ops.size(), 4u);
    EXPECT_EQ(Ops.back().Inputs[0], NdVar::cst(uint64_t(Intrinsic::Retf), 2));
    const auto Decode = [&] {
      DecodedInsn I{};
      EXPECT_EQ(
          Dec.decodeOneForLift(Bytes.data(), Bytes.size(), CodeAddress, I),
          int(Bytes.size()));
      return I;
    };
    auto Insn = Decode();
    EXPECT_TRUE(nativestateaudit::farReturnForm(Insn.Raw, Arch::X64));
    EXPECT_FALSE(nativestateaudit::farReturnForm(Insn.Raw, Arch::X86));
    EXPECT_FALSE(nativestateaudit::form(Insn.Raw, Arch::X64));
    for (unsigned Mutation = 0; Mutation != 16; ++Mutation) {
      SCOPED_TRACE(Mutation);
      Insn = Decode();
      auto &X = Insn.Raw->detail->x86;
      switch (Mutation) {
      case 0:
        Insn.Raw->bytes[0] ^= 1;
        break;
      case 1:
        Insn.Raw->bytes[1] ^= 1;
        break;
      case 2:
        X.opcode[0] ^= 1;
        break;
      case 3:
        X.rex ^= 1;
        break;
      case 4:
        X.addr_size = 4;
        break;
      case 5:
        X.prefix[2] = 0x66;
        break;
      case 6:
        X.eflags = X86_EFLAGS_MODIFY_CF;
        break;
      case 7:
        X.op_count ^= 1;
        break;
      case 8:
        X.encoding.imm_offset ^= 1;
        break;
      case 9:
        X.encoding.imm_size ^= 1;
        break;
      case 10:
        X.encoding.disp_size = 1;
        break;
      case 11:
        X.modrm = 0xc0;
        break;
      default:
        if (Value == 65536) {
          X.avx_sae = true;
          break;
        }
        if (Mutation == 12)
          X.operands[0].imm += 65536;
        if (Mutation == 13)
          X.operands[0].size = 2;
        if (Mutation == 14)
          X.operands[0].access = CS_AC_READ;
        if (Mutation == 15)
          Insn.Raw->bytes[2] ^= 1;
      }
      EXPECT_FALSE(nativestateaudit::farReturnForm(Insn.Raw, Arch::X64));
      expectUnaudited(Dec, Insn);
    }
  }
  // Independently decode every imm16, including Capstone's signed display
  // half. The architectural unsigned cleanup value is bound by these bytes.
  for (unsigned Value = 0; Value != 65536; ++Value) {
    const uint8_t Bytes[] = {0x48, 0xca, uint8_t(Value), uint8_t(Value >> 8)};
    DecodedInsn Insn{};
    ASSERT_EQ(Dec.decodeOneForLift(Bytes, sizeof(Bytes), CodeAddress, Insn), 4);
    ASSERT_TRUE(nativestateaudit::farReturnForm(Insn.Raw, Arch::X64)) << Value;
  }
}

TEST(X86NativeStateAudit, CR3EncodingKeepsArbitraryFlagsAndRejectsForgedBank) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  Dec.setStrict(true);
  for (uint8_t Register = 0; Register != 16; ++Register)
    for (uint8_t Opcode : {0x20, 0x22}) {
      std::vector<uint8_t> Bytes{0x0f, Opcode, uint8_t(0xd8 | (Register & 7))};
      if (Register >= 8)
        Bytes.insert(Bytes.begin(), 0x41);
      DecodedInsn Insn{};
      ASSERT_EQ(
          Dec.decodeOneForLift(Bytes.data(), Bytes.size(), CodeAddress, Insn),
          int(Bytes.size()));
      std::vector<LowOp> Ops;
      LowInstructionUndefinedEffects Effects;
      LowInstructionPreservedState Preserved;
      Dec.liftToLow(Insn, Ops, {}, {}, &Effects, &Preserved);
      ASSERT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
          << Effects.Diagnostic;
      ASSERT_EQ(Effects.Effects.size(), 6u);
      const unsigned Flags[] = {x86reg::OF, x86reg::SF, x86reg::ZF,
                                x86reg::AF, x86reg::PF, x86reg::CF};
      for (unsigned I = 0; I != 6; ++I) {
        EXPECT_EQ(Effects.Effects[I].Output, NdVar::reg(Flags[I], 1));
        EXPECT_EQ(Effects.Effects[I].BitCount, 1u);
        EXPECT_EQ(Effects.Effects[I].AfterOp, Ops.size());
      }
      EXPECT_EQ(Preserved, LowInstructionPreservedState{});
      Insn.Raw->detail->x86.operands[Opcode == 0x20 ? 1 : 0].reg = X86_REG_CR4;
      expectUnaudited(Dec, Insn);
    }
}

TEST(X86NativeStateAudit, TimestampCoverageRequiresExactEncodingAndDetails) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  Dec.setStrict(true);
  for (const std::vector<uint8_t> Bytes :
       {std::vector<uint8_t>{0x0f, 0x31},
        std::vector<uint8_t>{0x0f, 0x01, 0xf9}}) {
    const auto Decode = [&] {
      DecodedInsn I{};
      EXPECT_EQ(
          Dec.decodeOneForLift(Bytes.data(), Bytes.size(), CodeAddress, I),
          int(Bytes.size()));
      return I;
    };
    auto I = Decode();
    std::vector<LowOp> Ops;
    LowInstructionUndefinedEffects Effects;
    LowInstructionPreservedState Preserved;
    Dec.liftToLow(I, Ops, {}, {}, &Effects, &Preserved);
    EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete);
    EXPECT_TRUE(Effects.Effects.empty());
    EXPECT_EQ(Preserved, LowInstructionPreservedState{});
    for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
      auto Forged = Decode();
      auto &X = Forged.Raw->detail->x86;
      switch (Mutation) {
      case 0:
        X.op_count = 1;
        break;
      case 1:
        X.prefix[0] = 0xf0;
        break;
      case 2:
        X.encoding.modrm_offset ^= 2;
        break;
      case 3:
        X.eflags = X86_EFLAGS_MODIFY_CF;
        break;
      case 4:
        Forged.Raw->bytes[1] ^= 1;
        break;
      }
      expectUnaudited(Dec, Forged);
    }
  }
}

TEST(X86NativeStateAudit, WritePrefetchBindsItsActualAddressAndHint) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  Dec.setStrict(true);
  for (uint8_t Segment : {0, 0x64, 0x65})
    for (uint8_t Address : {0, 0x67}) {
      std::vector<uint8_t> Bytes;
      if (Segment)
        Bytes.push_back(Segment);
      if (Address)
        Bytes.push_back(Address);
      Bytes.insert(Bytes.end(), {0x0f, 0x0d, 0x8f, 0x90, 0, 0, 0});
      DecodedInsn I{};
      ASSERT_EQ(
          Dec.decodeOneForLift(Bytes.data(), Bytes.size(), CodeAddress, I),
          int(Bytes.size()));
      std::vector<LowOp> Ops;
      LowInstructionUndefinedEffects Effects;
      Dec.liftToLow(I, Ops, {}, {}, &Effects);
      EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
          << Effects.Diagnostic;
      EXPECT_TRUE(Effects.Effects.empty());
      I.Raw->detail->x86.operands[0].mem.disp++;
      expectUnaudited(Dec, I);
    }
}

TEST(X86NativeStateAudit, UnsignedDivisionBindsFlagsAfterSuccessfulCommit) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  Dec.setStrict(true);
  for (uint8_t Register = 0; Register != 16; ++Register) {
    const uint8_t Bytes[] = {uint8_t(Register < 8 ? 0x48 : 0x49), 0xf7,
                             uint8_t(0xf0 | (Register & 7))};
    DecodedInsn Insn{};
    ASSERT_EQ(Dec.decodeOneForLift(Bytes, sizeof(Bytes), CodeAddress, Insn), 3);
    std::vector<LowOp> Ops;
    LowInstructionUndefinedEffects Effects;
    LowInstructionPreservedState Preserved;
    Dec.liftToLow(Insn, Ops, {}, {}, &Effects, &Preserved);
    ASSERT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
        << Effects.Diagnostic;
    EXPECT_EQ(Effects.OpCount, Ops.size());
    EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Ops));
    ASSERT_EQ(Effects.Effects.size(), 6u);
    const unsigned Undefined[] = {x86reg::OF, x86reg::SF, x86reg::ZF,
                                  x86reg::AF, x86reg::PF, x86reg::CF};
    for (unsigned I = 0; I != 6; ++I) {
      EXPECT_EQ(Effects.Effects[I].Output, NdVar::reg(Undefined[I], 1));
      EXPECT_EQ(Effects.Effects[I].BitOffset, 0u);
      EXPECT_EQ(Effects.Effects[I].BitCount, 1u);
      EXPECT_FALSE(Effects.Effects[I].When);
      EXPECT_EQ(Effects.Effects[I].AfterOp, Ops.size());
    }
    EXPECT_EQ(Preserved, LowInstructionPreservedState{});
  }
}

TEST(X86NativeStateAudit, UnsignedDivisionReadsActualHighHalfAfterPriorLifts) {
  for (unsigned Prior = 0; Prior != 3; ++Prior) {
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X64));
    Dec.setStrict(true);
    const uint8_t Clear[] = {0x31, 0xd2};
    const uint8_t Divide[] = {0x48, 0xf7, 0xf1};
    std::vector<LowOp> Ignored;
    DecodedInsn Insn{};
    if (Prior) {
      ASSERT_EQ(Dec.decodeOneForLift(Clear, sizeof(Clear), CodeAddress, Insn),
                2);
      Dec.liftToLow(Insn, Ignored);
    }
    ASSERT_EQ(
        Dec.decodeOneForLift(Divide, sizeof(Divide), CodeAddress + 2, Insn), 3);
    if (Prior == 2)
      Dec.liftToLow(Insn, Ignored);
    std::vector<LowOp> Ops;
    LowInstructionUndefinedEffects Effects;
    Dec.liftToLow(Insn, Ops, {}, {}, &Effects);
    ASSERT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete);
    for (uint64_t Divisor : {0, 1, 3}) {
      NdOpEmulator Emulator(image());
      Emulator.setStrictMode(true);
      StateOracle Expected(Emulator);
      Expected.setRegister(Emulator, x86reg::RAX, 13);
      Expected.setRegister(Emulator, x86reg::RDX, 1);
      Expected.setRegister(Emulator, x86reg::RCX, Divisor);
      const auto Completed = Emulator.run(Ops);
      if (Divisor > 1) {
        EXPECT_EQ(Completed, Ops.size());
        // (2^64 + 13) / 3 = 6148914691236517209, remainder 2.
        Expected.Registers[0] = UINT64_C(6148914691236517209);
        Expected.Registers[2] = 2;
      } else {
        EXPECT_LT(Completed, Ops.size());
      }
      // The sidecar owns arbitrary flags. The concrete representative must
      // leave every other bank unchanged, including on a failed precondition.
      Expected.expect(Emulator);
    }
  }
}

TEST(X86NativeStateAudit, DivisionRejectsUnauditedFormsAndForgedDetails) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  Dec.setStrict(true);
  using Mutation = void (*)(cs_insn &);
  const Mutation Mutations[] = {
      [](cs_insn &I) { I.id = X86_INS_IDIV; },
      [](cs_insn &I) { I.bytes[I.size++] = 0x90; },
      [](cs_insn &I) { I.bytes[0] = 0x40; },
      [](cs_insn &I) { I.bytes[1] = 0xf6; },
      [](cs_insn &I) { I.bytes[2] = 0xf9; },
      [](cs_insn &I) { I.detail->x86.op_count = 2; },
      [](cs_insn &I) { I.detail->x86.modrm ^= 1; },
      [](cs_insn &I) { I.detail->x86.encoding.modrm_offset = 1; },
      [](cs_insn &I) { I.detail->x86.opcode[0] ^= 1; },
      [](cs_insn &I) { I.detail->x86.opcode[1] = 1; },
      [](cs_insn &I) { I.detail->x86.rex ^= 1; },
      [](cs_insn &I) { I.detail->x86.prefix[0] = 0xf0; },
      [](cs_insn &I) { I.detail->x86.prefix[1] = 0x65; },
      [](cs_insn &I) { I.detail->x86.prefix[2] = 0x66; },
      [](cs_insn &I) { I.detail->x86.prefix[3] = 0x67; },
      [](cs_insn &I) { I.detail->x86.addr_size = 4; },
      [](cs_insn &I) { I.detail->x86.sib = 1; },
      [](cs_insn &I) { I.detail->x86.disp = 1; },
      [](cs_insn &I) { I.detail->x86.encoding.disp_offset = 2; },
      [](cs_insn &I) { I.detail->x86.encoding.disp_size = 1; },
      [](cs_insn &I) { I.detail->x86.encoding.imm_offset = 2; },
      [](cs_insn &I) { I.detail->x86.encoding.imm_size = 1; },
      [](cs_insn &I) { I.detail->x86.avx_sae = true; },
      [](cs_insn &I) { I.detail->x86.avx_rm = X86_AVX_RM_RN; },
      [](cs_insn &I) { I.detail->x86.avx_cc = X86_AVX_CC_EQ; },
      [](cs_insn &I) { I.detail->x86.operands[0].size = 4; },
      [](cs_insn &I) { I.detail->x86.operands[0].reg = X86_REG_RDX; },
      [](cs_insn &I) { I.detail->x86.operands[0].avx_zero_opmask = true; },
      [](cs_insn &I) {
        I.detail->x86.operands[0].avx_bcast = X86_AVX_BCAST_2;
      }};
  for (unsigned I = 0; I != std::size(Mutations); ++I) {
    SCOPED_TRACE(I);
    const uint8_t Bytes[] = {0x48, 0xf7, 0xf1};
    DecodedInsn Insn{};
    ASSERT_EQ(Dec.decodeOneForLift(Bytes, sizeof(Bytes), CodeAddress, Insn), 3);
    Mutations[I](*Insn.Raw);
    expectUnaudited(Dec, Insn);
  }
  const std::vector<std::vector<uint8_t>> Forms = {{0xf6, 0xf1},
                                                   {0x66, 0xf7, 0xf1},
                                                   {0xf7, 0xf1},
                                                   {0x48, 0xf7, 0x31},
                                                   {0x48, 0xf7, 0xf9},
                                                   {0x66, 0x48, 0xf7, 0xf1},
                                                   {0x67, 0x48, 0xf7, 0xf1},
                                                   {0x48, 0x48, 0xf7, 0xf1}};
  for (const auto &Bytes : Forms) {
    DecodedInsn Insn{};
    ASSERT_EQ(
        Dec.decodeOneForLift(Bytes.data(), Bytes.size(), CodeAddress, Insn),
        int(Bytes.size()));
    expectUnaudited(Dec, Insn);
  }
}

} // namespace
