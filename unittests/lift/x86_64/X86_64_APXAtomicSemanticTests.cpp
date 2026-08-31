//===- X86_64_APXAtomicSemanticTests.cpp - APX atomic semantics ----------===//

#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/Support/Error.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t kInstructionAddress = 0x1000;
constexpr uint64_t kProbeTemp = UINT64_C(0x7a000000);
constexpr uint64_t kAtomicOutputTemp = UINT64_C(0x7b000000);

enum class SegmentOverride { None, FS, GS };

enum class RaoOperation : uint8_t { Add, And, Or, Xor };

struct LiftedInstruction {
  unsigned Id = X86_INS_INVALID;
  std::vector<LowOp> Ops;
};

struct Flags {
  bool CF = false;
  bool PF = false;
  bool AF = false;
  bool ZF = false;
  bool SF = false;
  bool OF = false;
  bool DF = false;
};

uint8_t segmentPrefix(SegmentOverride Segment) {
  switch (Segment) {
  case SegmentOverride::FS:
    return 0x64;
  case SegmentOverride::GS:
    return 0x65;
  case SegmentOverride::None:
    return 0;
  }
  return 0;
}

uint8_t encodeP0(unsigned Map, unsigned ModRMReg, unsigned Base,
                 unsigned Index) {
  uint8_t P0 = static_cast<uint8_t>(Map & 7);
  if ((ModRMReg & 8) == 0)
    P0 |= 0x80;
  if ((ModRMReg & 16) == 0)
    P0 |= 0x10;
  if ((Base & 8) == 0)
    P0 |= 0x20;
  if ((Base & 16) != 0)
    P0 |= 0x08;
  if ((Index & 8) == 0)
    P0 |= 0x40;
  return P0;
}

uint8_t raoPP(RaoOperation Operation) {
  switch (Operation) {
  case RaoOperation::Add:
    return 0;
  case RaoOperation::And:
    return 1;
  case RaoOperation::Or:
    return 3;
  case RaoOperation::Xor:
    return 2;
  }
  return 0;
}

unsigned raoInstructionId(RaoOperation Operation) {
  switch (Operation) {
  case RaoOperation::Add:
    return X86_INS_AADD;
  case RaoOperation::And:
    return X86_INS_AAND;
  case RaoOperation::Or:
    return X86_INS_AOR;
  case RaoOperation::Xor:
    return X86_INS_AXOR;
  }
  return X86_INS_INVALID;
}

Intrinsic raoIntrinsic(RaoOperation Operation) {
  switch (Operation) {
  case RaoOperation::Add:
    return Intrinsic::ApxRaoAdd;
  case RaoOperation::And:
    return Intrinsic::ApxRaoAnd;
  case RaoOperation::Or:
    return Intrinsic::ApxRaoOr;
  case RaoOperation::Xor:
    return Intrinsic::ApxRaoXor;
  }
  return Intrinsic::None;
}

std::vector<uint8_t> encodeRao(RaoOperation Operation, unsigned Width,
                               unsigned Source = 26, unsigned Base = 29,
                               unsigned Index = 14, bool Address32 = false,
                               SegmentOverride Segment = SegmentOverride::None,
                               int8_t Displacement = 0x20) {
  std::vector<uint8_t> Bytes;
  if (Address32)
    Bytes.push_back(0x67);
  if (const uint8_t Prefix = segmentPrefix(Segment))
    Bytes.push_back(Prefix);
  Bytes.insert(
      Bytes.end(),
      {0x62, encodeP0(4, Source, Base, Index),
       static_cast<uint8_t>(0x78 | ((Index & 16) == 0 ? 0x04 : 0) |
                            (Width == 8 ? 0x80 : 0) | raoPP(Operation)),
       0x08, 0xfc, static_cast<uint8_t>(0x40 | ((Source & 7) << 3) | 4),
       static_cast<uint8_t>(0x80 | ((Index & 7) << 3) | (Base & 7)),
       static_cast<uint8_t>(Displacement)});
  return Bytes;
}

std::vector<uint8_t>
encodeCmpccXadd(unsigned Condition, unsigned Width, unsigned Compare = 26,
                unsigned Add = 17, unsigned Base = 29, unsigned Index = 14,
                bool Address32 = false,
                SegmentOverride Segment = SegmentOverride::None,
                int8_t Displacement = 0x20) {
  std::vector<uint8_t> Bytes;
  if (Address32)
    Bytes.push_back(0x67);
  if (const uint8_t Prefix = segmentPrefix(Segment))
    Bytes.push_back(Prefix);
  const uint8_t P1 =
      static_cast<uint8_t>((Width == 8 ? 0x80 : 0) | (((~Add) & 15) << 3) |
                           ((Index & 16) == 0 ? 0x04 : 0) | 0x01);
  const uint8_t P2 = (Add & 16) != 0 ? 0 : 0x08;
  Bytes.insert(Bytes.end(),
               {0x62, encodeP0(2, Compare, Base, Index), P1, P2,
                static_cast<uint8_t>(0xe0 | (Condition & 15)),
                static_cast<uint8_t>(0x40 | ((Compare & 7) << 3) | 4),
                static_cast<uint8_t>(0x80 | ((Index & 7) << 3) | (Base & 7)),
                static_cast<uint8_t>(Displacement)});
  return Bytes;
}

LiftedInstruction liftX64(const std::vector<uint8_t> &Bytes) {
  Decoder Dec;
  if (!Dec.init(Arch::X64)) {
    ADD_FAILURE() << "failed to initialize x86-64 decoder";
    return {};
  }
  DecodedInsn Insn{};
  if (Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kInstructionAddress,
                           Insn) != static_cast<int>(Bytes.size())) {
    ADD_FAILURE() << "failed to decode complete APX atomic instruction";
    return {};
  }
  LiftedInstruction Result;
  Result.Id = Insn.Id;
  try {
    Dec.liftToLow(Insn, Result.Ops);
  } catch (const UnliftedInstruction &) {
    ADD_FAILURE() << "APX atomic instruction was not lifted";
  }
  return Result;
}

template <typename Mutator>
void expectMutatedLiftRejected(const std::vector<uint8_t> &Bytes,
                               Mutator Mutate) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(),
                                 kInstructionAddress, Insn),
            static_cast<int>(Bytes.size()));
  ASSERT_NE(Insn.Raw, nullptr);
  ASSERT_NE(Insn.Raw->detail, nullptr);
  Mutate(*Insn.Raw);
  std::vector<LowOp> Ops;
  EXPECT_THROW(Dec.liftToLow(Insn, Ops), UnliftedInstruction);
  EXPECT_TRUE(Ops.empty());
}

void expectDecodeOrLiftRejected(const std::vector<uint8_t> &Bytes,
                                Arch Target = Arch::X64) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Target));
  DecodedInsn Insn{};
  if (Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kInstructionAddress,
                           Insn) != static_cast<int>(Bytes.size()))
    return;
  std::vector<LowOp> Ops;
  EXPECT_THROW(Dec.liftToLow(Insn, Ops), UnliftedInstruction);
  EXPECT_TRUE(Ops.empty());
}

const LowOp *findIntrinsic(const std::vector<LowOp> &Ops, Intrinsic Id) {
  auto It = std::find_if(Ops.begin(), Ops.end(), [&](const LowOp &Op) {
    return Op.Opcode == NdOp::INTRINSIC && Op.NumInputs != 0 &&
           Op.Inputs[0].isConst() &&
           Op.Inputs[0].Offset == static_cast<uint64_t>(Id);
  });
  return It == Ops.end() ? nullptr : &*It;
}

BinaryImage makeImage(uint64_t Address, unsigned Width, SegmentFlags Flags,
                      uint64_t Value) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  if (Width != 0) {
    Segment Data;
    Data.VA = Address;
    Data.Size = Width;
    Data.Flags = Flags;
    Data.Data.resize(Width);
    for (unsigned I = 0; I != Width; ++I)
      Data.Data[I] = static_cast<uint8_t>(Value >> (I * 8));
    Image.Segments.push_back(std::move(Data));
  }
  return Image;
}

std::optional<uint64_t> probeMemory(NdOpEmulator &Emulator, uint64_t Address,
                                    unsigned Width) {
  LowOp Probe;
  Probe.Addr = kInstructionAddress + 1;
  Probe.Opcode = NdOp::LOAD;
  Probe.Output = NdVar::tmp(kProbeTemp + Width, Width);
  Probe.addInput(NdVar::cst(Address, 8));
  if (!Emulator.step(Probe))
    return std::nullopt;
  return Emulator.getRegister(Probe.Output.Offset);
}

LowOp makeRaoIntrinsic(Intrinsic Id, unsigned Width, uint64_t Address,
                       uint64_t Source) {
  LowOp Op;
  Op.Addr = kInstructionAddress;
  Op.Opcode = NdOp::INTRINSIC;
  Op.MemoryOrdering = NdMemoryOrdering::Relaxed;
  Op.addInput(NdVar::cst(static_cast<uint64_t>(Id), 2));
  Op.addInput(NdVar::cst(Address, 8));
  Op.addInput(NdVar::cst(Source, Width));
  return Op;
}

LowOp makeCmpIntrinsic(unsigned Width, uint64_t Address, uint64_t Add,
                       uint64_t Compare, unsigned Condition) {
  LowOp Op;
  Op.Addr = kInstructionAddress;
  Op.Opcode = NdOp::INTRINSIC;
  Op.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
  Op.Output = NdVar::tmp(kAtomicOutputTemp, Width);
  Op.addInput(NdVar::cst(static_cast<uint64_t>(Intrinsic::ApxCmpccXadd), 2));
  Op.addInput(NdVar::cst(Address, 8));
  Op.addInput(NdVar::cst(Add, Width));
  Op.addInput(NdVar::cst(Compare, Width));
  Op.addInput(NdVar::cst(Condition, 1));
  return Op;
}

LowOp makePdepPextIntrinsic(Intrinsic Id, unsigned Width = 8) {
  LowOp Op;
  Op.Addr = kInstructionAddress;
  Op.Opcode = NdOp::INTRINSIC;
  Op.Output = NdVar::tmp(kAtomicOutputTemp, Width);
  Op.addInput(NdVar::cst(static_cast<uint64_t>(Id), 2));
  Op.addInput(NdVar::cst(UINT64_C(0x0123456789abcdef), Width));
  Op.addInput(NdVar::cst(UINT64_C(0xf0f00f0faaaa5555), Width));
  return Op;
}

LowFunc makeSingleInstructionFunction(LowOp Op) {
  Op.Addr = kInstructionAddress;
  LowBlock Block;
  Block.Id = 0;
  Block.StartAddr = kInstructionAddress;
  Block.EndAddr = kInstructionAddress + 1;
  Block.Ops.push_back(std::move(Op));
  LowInstructionBoundary Boundary;
  Boundary.Address = kInstructionAddress;
  Boundary.Size = 1;
  Boundary.OpCount = 1;
  Block.InstructionBoundaries.push_back(Boundary);

  LowFunc Function;
  Function.Entry = kInstructionAddress;
  Function.Blocks.push_back(std::move(Block));
  return Function;
}

std::string lowValidationError(const LowFunc &Function) {
  return llvm::toString(validateLowInstructionBoundaries(
      Function, LowInstructionBoundaryRequirement::Required));
}

MedVar medTemp(int Id, unsigned Size, Arch TheArch = Arch::X64) {
  MedVar Value;
  Value.Kind = MedVar::Temp;
  Value.TheArch = TheArch;
  Value.Id = Id;
  Value.Size = Size;
  return Value;
}

MedFunc makeMedCmpIntrinsic() {
  MedOp Op;
  Op.Addr = kInstructionAddress;
  Op.Opcode = NdOp::INTRINSIC;
  Op.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
  Op.Output = medTemp(1, 8);
  Op.addInput(
      MedVar::makeConst(static_cast<uint64_t>(Intrinsic::ApxCmpccXadd), 2));
  Op.addInput(medTemp(2, 8));
  Op.addInput(medTemp(3, 8));
  Op.addInput(medTemp(4, 8));
  Op.addInput(MedVar::makeConst(5, 1));

  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = kInstructionAddress;
  Block.EndAddr = kInstructionAddress + 1;
  Block.Ops.push_back(std::move(Op));
  MedFunc Function;
  Function.Entry = kInstructionAddress;
  Function.Blocks.push_back(std::move(Block));
  return Function;
}

MedFunc makeMedPdepPextIntrinsic(Intrinsic Id, unsigned Width = 8) {
  MedOp Op;
  Op.Addr = kInstructionAddress;
  Op.Opcode = NdOp::INTRINSIC;
  Op.Output = medTemp(1, Width);
  Op.addInput(MedVar::makeConst(static_cast<uint64_t>(Id), 2));
  Op.addInput(MedVar::makeConst(UINT64_C(0x0123456789abcdef), Width));
  Op.addInput(MedVar::makeConst(UINT64_C(0xf0f00f0faaaa5555), Width));

  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = kInstructionAddress;
  Block.EndAddr = kInstructionAddress + 1;
  Block.Ops.push_back(std::move(Op));
  MedFunc Function;
  Function.Entry = kInstructionAddress;
  Function.Blocks.push_back(std::move(Block));
  return Function;
}

MedFunc makeMedDivPreconditionWithParameterDivisor() {
  MedOp Op;
  Op.Addr = kInstructionAddress;
  Op.Opcode = NdOp::INTRINSIC;
  Op.addInput(MedVar::makeConst(
      static_cast<uint64_t>(Intrinsic::X86RequireDivPrecondition), 2));
  Op.addInput(medTemp(1, 8, Arch::X86));
  MedVar Divisor;
  Divisor.Kind = MedVar::Param;
  Divisor.TheArch = Arch::X86;
  Divisor.Id = 1;
  Divisor.Size = 4;
  Divisor.RegOff = kNoParamReg;
  Op.addInput(Divisor);
  Op.addInput(MedVar::makeConst(
      static_cast<uint64_t>(X86DivKind::Unsigned), 1));

  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = kInstructionAddress;
  Block.EndAddr = kInstructionAddress + 1;
  Block.Ops.push_back(std::move(Op));
  MedFunc Function;
  Function.Entry = kInstructionAddress;
  Function.Params.push_back(Divisor);
  Function.Blocks.push_back(std::move(Block));
  return Function;
}

LowOp makeStore(uint64_t Address, uint64_t Value, unsigned Width) {
  LowOp Op;
  Op.Addr = kInstructionAddress;
  Op.Opcode = NdOp::STORE;
  Op.addInput(NdVar::cst(0, 8));
  Op.addInput(NdVar::cst(Address, 8));
  Op.addInput(NdVar::cst(Value, Width));
  return Op;
}

uint64_t widthMask(unsigned Width) {
  return Width == 8 ? UINT64_MAX : (UINT64_C(1) << (Width * 8)) - UINT64_C(1);
}

bool evenParity(uint8_t Value) {
  unsigned Count = 0;
  for (unsigned I = 0; I != 8; ++I)
    Count += (Value >> I) & 1;
  return (Count & 1) == 0;
}

Flags subtractionFlags(unsigned Width, uint64_t Left, uint64_t Right, bool DF) {
  const uint64_t Mask = widthMask(Width);
  const uint64_t A = Left & Mask;
  const uint64_t B = Right & Mask;
  const uint64_t Result = (A - B) & Mask;
  const uint64_t Sign = UINT64_C(1) << (Width * 8 - 1);
  return {A < B,
          evenParity(static_cast<uint8_t>(Result)),
          ((A ^ B ^ Result) & 0x10) != 0,
          Result == 0,
          (Result & Sign) != 0,
          (((A ^ B) & (A ^ Result)) & Sign) != 0,
          DF};
}

bool conditionHolds(unsigned Condition, const Flags &F) {
  switch (Condition) {
  case 0:
    return F.OF;
  case 1:
    return !F.OF;
  case 2:
    return F.CF;
  case 3:
    return !F.CF;
  case 4:
    return F.ZF;
  case 5:
    return !F.ZF;
  case 6:
    return F.CF || F.ZF;
  case 7:
    return !F.CF && !F.ZF;
  case 8:
    return F.SF;
  case 9:
    return !F.SF;
  case 10:
    return F.PF;
  case 11:
    return !F.PF;
  case 12:
    return F.SF != F.OF;
  case 13:
    return F.SF == F.OF;
  case 14:
    return F.ZF || F.SF != F.OF;
  case 15:
    return !F.ZF && F.SF == F.OF;
  default:
    return false;
  }
}

void setFlags(NdOpEmulator &Emulator, const Flags &F) {
  Emulator.setRegister(x86reg::CF, F.CF);
  Emulator.setRegister(x86reg::PF, F.PF);
  Emulator.setRegister(x86reg::AF, F.AF);
  Emulator.setRegister(x86reg::ZF, F.ZF);
  Emulator.setRegister(x86reg::SF, F.SF);
  Emulator.setRegister(x86reg::OF, F.OF);
  Emulator.setRegister(x86reg::DF, F.DF);
}

void expectFlags(const NdOpEmulator &Emulator, const Flags &F) {
  EXPECT_EQ(Emulator.getRegister(x86reg::CF), F.CF);
  EXPECT_EQ(Emulator.getRegister(x86reg::PF), F.PF);
  EXPECT_EQ(Emulator.getRegister(x86reg::AF), F.AF);
  EXPECT_EQ(Emulator.getRegister(x86reg::ZF), F.ZF);
  EXPECT_EQ(Emulator.getRegister(x86reg::SF), F.SF);
  EXPECT_EQ(Emulator.getRegister(x86reg::OF), F.OF);
  EXPECT_EQ(Emulator.getRegister(x86reg::DF), F.DF);
}

uint64_t raoResult(RaoOperation Operation, unsigned Width, uint64_t Old,
                   uint64_t Source) {
  const uint64_t Mask = widthMask(Width);
  switch (Operation) {
  case RaoOperation::Add:
    return (Old + Source) & Mask;
  case RaoOperation::And:
    return (Old & Source) & Mask;
  case RaoOperation::Or:
    return (Old | Source) & Mask;
  case RaoOperation::Xor:
    return (Old ^ Source) & Mask;
  }
  return 0;
}

void initializeAddress(NdOpEmulator &Emulator, uint64_t Offset, bool Address32,
                       unsigned Base = 29, unsigned Index = 14,
                       uint64_t IndexValue = 3, int8_t Displacement = 0x20) {
  const uint64_t BaseValue =
      Offset - IndexValue * 4 - static_cast<int64_t>(Displacement);
  const auto RegisterOffset = [](unsigned Number) {
    return Number < 16 ? static_cast<uint64_t>(Number) * 8
                       : x86reg::extendedGeneralReg(Number - 16);
  };
  Emulator.setRegister(RegisterOffset(Base),
                       (Address32 ? UINT64_C(0xabcdefff00000000) : 0) |
                           BaseValue);
  Emulator.setRegister(RegisterOffset(Index),
                       (Address32 ? UINT64_C(0x1234567800000000) : 0) |
                           IndexValue);
}

TEST(X86APXAtomic, LiftUsesMemoryOwningAtomicIntrinsics) {
  constexpr std::array<RaoOperation, 4> RaoOperations = {
      RaoOperation::Add, RaoOperation::And, RaoOperation::Or,
      RaoOperation::Xor};
  for (RaoOperation Operation : RaoOperations) {
    for (unsigned Width : {4u, 8u}) {
      SCOPED_TRACE(::testing::Message()
                   << "rao=" << static_cast<unsigned>(Operation)
                   << " width=" << Width);
      const LiftedInstruction Lifted = liftX64(encodeRao(Operation, Width));
      ASSERT_EQ(Lifted.Id, raoInstructionId(Operation));
      const LowOp *Atomic = findIntrinsic(Lifted.Ops, raoIntrinsic(Operation));
      ASSERT_NE(Atomic, nullptr);
      EXPECT_TRUE(isSideeffectIntrinsic(raoIntrinsic(Operation)));
      EXPECT_EQ(Atomic->NumInputs, 3);
      EXPECT_EQ(Atomic->Output.Size, 0);
      EXPECT_EQ(Atomic->Inputs[1].Size, 8);
      EXPECT_EQ(Atomic->Inputs[2].Size, Width);
      EXPECT_EQ(Atomic->MemoryOrdering, NdMemoryOrdering::Relaxed);
      EXPECT_EQ(Atomic->MemoryAddressSpace, NdMemoryAddressSpace::Default);
      EXPECT_TRUE(std::none_of(
          Lifted.Ops.begin(), Lifted.Ops.end(), [](const LowOp &Op) {
            return Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE ||
                   Op.Opcode == NdOp::ATOMIC_ADD ||
                   Op.Opcode == NdOp::ATOMIC_CMPXCHG ||
                   Op.Opcode == NdOp::ATOMIC_XCHG;
          }));
    }
  }

  for (unsigned Condition = 0; Condition != 16; ++Condition) {
    for (unsigned Width : {4u, 8u}) {
      SCOPED_TRACE(::testing::Message()
                   << "condition=" << Condition << " width=" << Width);
      const LiftedInstruction Lifted =
          liftX64(encodeCmpccXadd(Condition, Width));
      ASSERT_EQ(Lifted.Id, X86_INS_CMPOXADD + Condition);
      const LowOp *Atomic = findIntrinsic(Lifted.Ops, Intrinsic::ApxCmpccXadd);
      ASSERT_NE(Atomic, nullptr);
      EXPECT_TRUE(isSideeffectIntrinsic(Intrinsic::ApxCmpccXadd));
      EXPECT_EQ(Atomic->NumInputs, 5);
      EXPECT_EQ(Atomic->Output.Size, Width);
      EXPECT_EQ(Atomic->Inputs[1].Size, 8);
      EXPECT_EQ(Atomic->Inputs[2].Size, Width);
      EXPECT_EQ(Atomic->Inputs[3].Size, Width);
      ASSERT_TRUE(Atomic->Inputs[4].isConst());
      EXPECT_EQ(Atomic->Inputs[4].Size, 1);
      EXPECT_EQ(Atomic->Inputs[4].Offset, Condition);
      EXPECT_EQ(Atomic->MemoryOrdering,
                NdMemoryOrdering::SequentiallyConsistent);
      EXPECT_TRUE(std::none_of(
          Lifted.Ops.begin(), Lifted.Ops.end(), [](const LowOp &Op) {
            return Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE ||
                   Op.Opcode == NdOp::ATOMIC_ADD ||
                   Op.Opcode == NdOp::ATOMIC_CMPXCHG ||
                   Op.Opcode == NdOp::ATOMIC_XCHG;
          }));
    }
  }
}

TEST(X86APXAtomic, RawEncodingAndDecoderDetailMustAgreeExactly) {
  constexpr uint64_t CmpFlags = X86_EFLAGS_MODIFY_OF | X86_EFLAGS_MODIFY_SF |
                                X86_EFLAGS_MODIFY_ZF | X86_EFLAGS_MODIFY_AF |
                                X86_EFLAGS_MODIFY_PF | X86_EFLAGS_MODIFY_CF;
  const auto Rao =
      encodeRao(RaoOperation::Add, 8, 26, 29, 14, false, SegmentOverride::FS);
  {
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X64));
    DecodedInsn Insn{};
    ASSERT_EQ(
        Dec.decodeOneForLift(Rao.data(), Rao.size(), kInstructionAddress, Insn),
        static_cast<int>(Rao.size()));
    ASSERT_NE(Insn.Raw, nullptr);
    ASSERT_NE(Insn.Raw->detail, nullptr);
    EXPECT_EQ(Insn.Raw->detail->x86.eflags, 0u);
    EXPECT_EQ(Insn.Raw->detail->regs_read_count, 0u);
    EXPECT_EQ(Insn.Raw->detail->regs_write_count, 0u);
  }
  expectMutatedLiftRejected(Rao,
                            [](cs_insn &Raw) { Raw.detail->x86.eflags = 1; });
  expectMutatedLiftRejected(Rao, [](cs_insn &Raw) {
    Raw.detail->regs_write[0] = X86_REG_EFLAGS;
    Raw.detail->regs_write_count = 1;
  });
  expectMutatedLiftRejected(Rao, [](cs_insn &Raw) { Raw.bytes[2] ^= 1; });
  expectMutatedLiftRejected(Rao,
                            [](cs_insn &Raw) { Raw.bytes[3] &= ~uint8_t{4}; });
  expectMutatedLiftRejected(Rao, [](cs_insn &Raw) { Raw.bytes[4] ^= 1; });
  expectMutatedLiftRejected(Rao, [](cs_insn &Raw) { Raw.bytes[5] = 0xfd; });
  expectMutatedLiftRejected(Rao, [](cs_insn &Raw) { Raw.bytes[6] ^= 0x08; });
  expectMutatedLiftRejected(Rao, [](cs_insn &Raw) { Raw.bytes[7] ^= 0x08; });
  expectMutatedLiftRejected(Rao, [](cs_insn &Raw) { Raw.bytes[8] ^= 1; });
  expectMutatedLiftRejected(Rao, [](cs_insn &Raw) {
    Raw.detail->x86.operands[0].mem.base = X86_REG_R28;
  });
  expectMutatedLiftRejected(Rao, [](cs_insn &Raw) {
    Raw.detail->x86.operands[0].mem.index = X86_REG_R15;
  });
  expectMutatedLiftRejected(
      Rao, [](cs_insn &Raw) { Raw.detail->x86.operands[0].mem.scale = 2; });
  expectMutatedLiftRejected(Rao, [](cs_insn &Raw) {
    Raw.detail->x86.operands[0].access = CS_AC_READ;
  });
  expectMutatedLiftRejected(
      Rao, [](cs_insn &Raw) { Raw.detail->x86.operands[1].reg = X86_REG_R25; });
  expectMutatedLiftRejected(
      Rao, [](cs_insn &Raw) { Raw.detail->x86.addr_size = 4; });
  expectMutatedLiftRejected(
      Rao, [](cs_insn &Raw) { Raw.detail->x86.encoding.disp_offset++; });
  expectMutatedLiftRejected(
      Rao, [](cs_insn &Raw) { Raw.detail->x86.opcode[1] ^= 1; });

  const auto Cmp =
      encodeCmpccXadd(14, 8, 26, 17, 29, 14, true, SegmentOverride::GS);
  {
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X64));
    DecodedInsn Insn{};
    ASSERT_EQ(
        Dec.decodeOneForLift(Cmp.data(), Cmp.size(), kInstructionAddress, Insn),
        static_cast<int>(Cmp.size()));
    ASSERT_NE(Insn.Raw, nullptr);
    ASSERT_NE(Insn.Raw->detail, nullptr);
    EXPECT_EQ(Insn.Raw->detail->x86.eflags, CmpFlags);
    EXPECT_EQ(Insn.Raw->detail->regs_read_count, 0u);
    ASSERT_EQ(Insn.Raw->detail->regs_write_count, 1u);
    EXPECT_EQ(Insn.Raw->detail->regs_write[0], X86_REG_EFLAGS);
  }
  expectMutatedLiftRejected(Cmp,
                            [](cs_insn &Raw) { Raw.detail->x86.eflags = 0; });
  expectMutatedLiftRejected(
      Cmp, [](cs_insn &Raw) { Raw.detail->regs_write_count = 0; });
  expectMutatedLiftRejected(
      Cmp, [](cs_insn &Raw) { Raw.detail->regs_write[0] = X86_REG_RAX; });
  expectMutatedLiftRejected(Cmp, [](cs_insn &Raw) {
    Raw.detail->regs_read[0] = X86_REG_EFLAGS;
    Raw.detail->regs_read_count = 1;
  });
  expectMutatedLiftRejected(Cmp, [](cs_insn &Raw) { Raw.bytes[3] ^= 0x08; });
  expectMutatedLiftRejected(Cmp, [](cs_insn &Raw) { Raw.bytes[4] ^= 0x08; });
  expectMutatedLiftRejected(Cmp, [](cs_insn &Raw) { Raw.bytes[5] ^= 1; });
  expectMutatedLiftRejected(
      Cmp, [](cs_insn &Raw) { Raw.detail->x86.operands[1].reg = X86_REG_R27; });
  expectMutatedLiftRejected(
      Cmp, [](cs_insn &Raw) { Raw.detail->x86.operands[2].reg = X86_REG_R16; });
  expectMutatedLiftRejected(Cmp, [](cs_insn &Raw) {
    Raw.detail->x86.operands[0].mem.segment = X86_REG_FS;
  });
  expectMutatedLiftRejected(Cmp, [](cs_insn &Raw) {
    for (size_t I = Raw.size; I != 0; --I)
      Raw.bytes[I] = Raw.bytes[I - 1];
    Raw.bytes[0] = 0x65;
    ++Raw.size;
  });
  expectMutatedLiftRejected(Cmp, [](cs_insn &Raw) {
    for (size_t I = Raw.size; I != 0; --I)
      Raw.bytes[I] = Raw.bytes[I - 1];
    Raw.bytes[0] = 0x67;
    ++Raw.size;
  });

  auto LockedRao = encodeRao(RaoOperation::Xor, 8);
  LockedRao.insert(LockedRao.begin(), 0xf0);
  expectDecodeOrLiftRejected(LockedRao);
  auto LockedCmp = encodeCmpccXadd(5, 4);
  LockedCmp.insert(LockedCmp.begin(), 0xf0);
  expectDecodeOrLiftRejected(LockedCmp);

  expectDecodeOrLiftRejected(encodeRao(RaoOperation::Add, 4), Arch::X86);
  expectDecodeOrLiftRejected(encodeCmpccXadd(5, 4), Arch::X86);
}

TEST(X86APXAtomic, ExecutorRejectsMalformedShapeConditionAndOrdering) {
  constexpr uint64_t Address = 0x4800;
  constexpr uint64_t Old = UINT64_C(0x1122334455667788);
  constexpr uint64_t Sentinel = UINT64_C(0xaabbccddeeff0011);

  auto ExpectCmpRejected = [&](LowOp Op) {
    BinaryImage Image = makeImage(
        Address, 8, SegmentFlags::Readable | SegmentFlags::Writable, Old);
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(kAtomicOutputTemp, Sentinel);
    EXPECT_FALSE(Emulator.step(Op));
    EXPECT_EQ(Emulator.getRegister(kAtomicOutputTemp), Sentinel);
    EXPECT_TRUE(Emulator.getLoadRecords().empty());
    EXPECT_EQ(probeMemory(Emulator, Address, 8), Old);
    EXPECT_FALSE(Emulator.skips().any());
  };

  LowOp Cmp = makeCmpIntrinsic(8, Address, 3, 7, 5);
  LowOp NarrowIntrinsicId = Cmp;
  NarrowIntrinsicId.Inputs[0].Size = 1;
  ExpectCmpRejected(NarrowIntrinsicId);
  LowOp NonConstantCondition = Cmp;
  NonConstantCondition.Inputs[4] = NdVar::tmp(kAtomicOutputTemp + 1, 1);
  ExpectCmpRejected(NonConstantCondition);
  LowOp WideCondition = Cmp;
  WideCondition.Inputs[4] = NdVar::cst(5, 2);
  ExpectCmpRejected(WideCondition);
  LowOp OutOfRangeCondition = Cmp;
  OutOfRangeCondition.Inputs[4] = NdVar::cst(16, 1);
  ExpectCmpRejected(OutOfRangeCondition);
  LowOp WeakOrdering = Cmp;
  WeakOrdering.MemoryOrdering = NdMemoryOrdering::Relaxed;
  ExpectCmpRejected(WeakOrdering);
  LowOp InvalidAddressSpace = Cmp;
  InvalidAddressSpace.MemoryAddressSpace =
      static_cast<NdMemoryAddressSpace>(0xff);
  ExpectCmpRejected(InvalidAddressSpace);

  BinaryImage Image = makeImage(
      Address, 8, SegmentFlags::Readable | SegmentFlags::Writable, Old);
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setLoadCollect(true);
  LowOp Rao = makeRaoIntrinsic(Intrinsic::ApxRaoAdd, 8, Address, UINT64_C(3));
  Rao.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
  EXPECT_FALSE(Emulator.step(Rao));
  EXPECT_TRUE(Emulator.getLoadRecords().empty());
  EXPECT_EQ(probeMemory(Emulator, Address, 8), Old);
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86APXAtomic, IRBoundariesRejectMalformedAtomicContracts) {
  const LowOp Cmp = makeCmpIntrinsic(8, 0x4800, 3, 7, 5);
  EXPECT_TRUE(lowValidationError(makeSingleInstructionFunction(Cmp)).empty());

  auto ExpectRejected = [&](LowOp Op) {
    EXPECT_NE(lowValidationError(makeSingleInstructionFunction(std::move(Op)))
                  .find("APX atomic intrinsic"),
              std::string::npos);
  };

  LowOp NarrowIntrinsicId = Cmp;
  NarrowIntrinsicId.Inputs[0].Size = 1;
  ExpectRejected(std::move(NarrowIntrinsicId));
  LowOp ConstantOutput = Cmp;
  ConstantOutput.Output = NdVar::cst(0, 8);
  ExpectRejected(std::move(ConstantOutput));
  LowOp MemoryAddress = Cmp;
  MemoryAddress.Inputs[1] = NdVar::ram(0x4800, 8);
  ExpectRejected(std::move(MemoryAddress));
  LowOp NonConstantCondition = Cmp;
  NonConstantCondition.Inputs[4] = NdVar::tmp(kAtomicOutputTemp + 1, 1);
  ExpectRejected(std::move(NonConstantCondition));
  LowOp OutOfRangeCondition = Cmp;
  OutOfRangeCondition.Inputs[4] = NdVar::cst(16, 1);
  ExpectRejected(std::move(OutOfRangeCondition));
  LowOp WeakOrdering = Cmp;
  WeakOrdering.MemoryOrdering = NdMemoryOrdering::Relaxed;
  ExpectRejected(std::move(WeakOrdering));

  LowOp Rao = makeRaoIntrinsic(Intrinsic::ApxRaoAdd, 8, 0x4800, 3);
  EXPECT_TRUE(lowValidationError(makeSingleInstructionFunction(Rao)).empty());
  Rao.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
  ExpectRejected(std::move(Rao));
}

TEST(X86APXAtomic, LowToMedRejectsMalformedOrNonX64AtomicContracts) {
  const LowFunc Valid = makeSingleInstructionFunction(
      makeCmpIntrinsic(8, 0x4800, 3, 7, 5));
  EXPECT_NO_FATAL_FAILURE(
      (void)LowToMedConverter().convert(Valid, Arch::X64));

  LowFunc BadCondition = Valid;
  BadCondition.Blocks.front().Ops.front().Inputs[4] = NdVar::cst(16, 1);
  const LowOp &BadConditionOp = BadCondition.Blocks.front().Ops.front();
  EXPECT_FALSE(intrinsicApxAtomicShapeIsValid(
      Intrinsic::ApxCmpccXadd,
      apxAtomicLowShape(BadConditionOp, Arch::X64)));
  const LowOp &ValidOp = Valid.Blocks.front().Ops.front();
  EXPECT_FALSE(intrinsicApxAtomicShapeIsValid(
      Intrinsic::ApxCmpccXadd, apxAtomicLowShape(ValidOp, Arch::X86)));
}

TEST(X86APXAtomic, MedVerifierRejectsMalformedAtomicContracts) {
  const MedFunc Valid = makeMedCmpIntrinsic();
  EXPECT_TRUE(verifyMedFunc(Valid, "valid-apx-atomic"));

  MedFunc BadCondition = Valid;
  BadCondition.Blocks.front().Ops.front().Inputs[4] =
      MedVar::makeConst(16, 1);
  EXPECT_FALSE(verifyMedFunc(BadCondition, "bad-apx-condition"));

  MedFunc BadOrdering = Valid;
  BadOrdering.Blocks.front().Ops.front().MemoryOrdering =
      NdMemoryOrdering::Relaxed;
  EXPECT_FALSE(verifyMedFunc(BadOrdering, "bad-apx-ordering"));

  MedFunc ConstantOutput = Valid;
  ConstantOutput.Blocks.front().Ops.front().Output =
      MedVar::makeConst(0, 8);
  EXPECT_FALSE(verifyMedFunc(ConstantOutput, "bad-apx-output-kind"));

  MedFunc NonX64 = Valid;
  NonX64.Blocks.front().Ops.front().Output.TheArch = Arch::AArch64;
  EXPECT_FALSE(verifyMedFunc(NonX64, "bad-apx-target"));
}

TEST(X86APXAtomic, DividePreconditionAcceptsRecoveredParameterDivisor) {
  const MedFunc Valid = makeMedDivPreconditionWithParameterDivisor();
  EXPECT_TRUE(verifyMedFunc(Valid, "valid-div-parameter"));

  MedFunc StackDivisor = Valid;
  StackDivisor.Blocks.front().Ops.front().Inputs[2].Kind = MedVar::Stack;
  EXPECT_FALSE(verifyMedFunc(StackDivisor, "bad-div-stack"));
}

TEST(X86APXAtomic, PdepPextCrossLayerContractsRejectMalformedIR) {
  for (Intrinsic Id : {Intrinsic::Pdep, Intrinsic::Pext}) {
    SCOPED_TRACE(::testing::Message()
                 << "intrinsic=" << static_cast<unsigned>(Id));
    const LowOp ValidOp = makePdepPextIntrinsic(Id);
    const LowFunc ValidLow = makeSingleInstructionFunction(ValidOp);
    EXPECT_TRUE(lowValidationError(ValidLow).empty());

    MedFunc Lowered = LowToMedConverter().convert(ValidLow, Arch::X64);
    EXPECT_TRUE(verifyMedFunc(Lowered, "valid-lowered-pdep-pext"));

    const auto ExpectLowContractRejected = [&](LowOp Op) {
      EXPECT_NE(lowValidationError(makeSingleInstructionFunction(std::move(Op)))
                    .find("PDEP/PEXT intrinsic"),
                std::string::npos);
    };

    LowOp WrongArity = ValidOp;
    WrongArity.NumInputs = 2;
    ExpectLowContractRejected(std::move(WrongArity));
    LowOp NarrowIntrinsicId = ValidOp;
    NarrowIntrinsicId.Inputs[0].Size = 1;
    ExpectLowContractRejected(std::move(NarrowIntrinsicId));
    LowOp ConstantOutput = ValidOp;
    ConstantOutput.Output = NdVar::cst(0, 8);
    ExpectLowContractRejected(std::move(ConstantOutput));
    LowOp NarrowOutput = ValidOp;
    NarrowOutput.Output.Size = 2;
    ExpectLowContractRejected(std::move(NarrowOutput));
    LowOp MemorySource = ValidOp;
    MemorySource.Inputs[1] = NdVar::ram(0x4800, 8);
    ExpectLowContractRejected(std::move(MemorySource));
    LowOp NarrowSource = ValidOp;
    NarrowSource.Inputs[1].Size = 4;
    ExpectLowContractRejected(std::move(NarrowSource));
    LowOp MemoryMask = ValidOp;
    MemoryMask.Inputs[2] = NdVar::ram(0x4800, 8);
    ExpectLowContractRejected(std::move(MemoryMask));
    LowOp NarrowMask = ValidOp;
    NarrowMask.Inputs[2].Size = 4;
    ExpectLowContractRejected(std::move(NarrowMask));
    LowOp Ordered = ValidOp;
    Ordered.MemoryOrdering = NdMemoryOrdering::Relaxed;
    ExpectLowContractRejected(std::move(Ordered));
    LowOp Segmented = ValidOp;
    Segmented.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
    ExpectLowContractRejected(std::move(Segmented));

    LowOp NonConstantId = ValidOp;
    NonConstantId.Inputs[0] = NdVar::tmp(kAtomicOutputTemp + 1, 2);
    EXPECT_NE(lowValidationError(
                  makeSingleInstructionFunction(std::move(NonConstantId)))
                  .find("constant intrinsic ID"),
              std::string::npos);

    LowOp ConversionBad = ValidOp;
    ConversionBad.MemoryOrdering = NdMemoryOrdering::Relaxed;
    EXPECT_FALSE(
        intrinsicPdepPextShapeIsValid(Id, pdepPextLowShape(ConversionBad)));

    const MedFunc ValidMed = makeMedPdepPextIntrinsic(Id);
    EXPECT_TRUE(verifyMedFunc(ValidMed, "valid-pdep-pext"));
    const auto ExpectMedRejected = [](MedFunc Function, const char *Name) {
      EXPECT_FALSE(verifyMedFunc(Function, Name));
    };

    MedFunc MedWrongArity = ValidMed;
    MedWrongArity.Blocks.front().Ops.front().NumInputs = 2;
    ExpectMedRejected(std::move(MedWrongArity), "bad-pdep-pext-arity");
    MedFunc MedNarrowIntrinsicId = ValidMed;
    MedNarrowIntrinsicId.Blocks.front().Ops.front().Inputs[0].Size = 1;
    ExpectMedRejected(std::move(MedNarrowIntrinsicId), "bad-pdep-pext-id-size");
    MedFunc MedConstantOutput = ValidMed;
    MedConstantOutput.Blocks.front().Ops.front().Output =
        MedVar::makeConst(0, 8);
    ExpectMedRejected(std::move(MedConstantOutput),
                      "bad-pdep-pext-output-kind");
    MedFunc MedNarrowOutput = ValidMed;
    MedNarrowOutput.Blocks.front().Ops.front().Output.Size = 2;
    ExpectMedRejected(std::move(MedNarrowOutput), "bad-pdep-pext-output-size");
    MedFunc MedStackSource = ValidMed;
    MedStackSource.Blocks.front().Ops.front().Inputs[1].Kind = MedVar::Stack;
    ExpectMedRejected(std::move(MedStackSource), "bad-pdep-pext-source-kind");
    MedFunc MedNarrowSource = ValidMed;
    MedNarrowSource.Blocks.front().Ops.front().Inputs[1].Size = 4;
    ExpectMedRejected(std::move(MedNarrowSource), "bad-pdep-pext-source-size");
    MedFunc MedParamMask = ValidMed;
    MedParamMask.Blocks.front().Ops.front().Inputs[2].Kind = MedVar::Param;
    ExpectMedRejected(std::move(MedParamMask), "bad-pdep-pext-mask-kind");
    MedFunc MedNarrowMask = ValidMed;
    MedNarrowMask.Blocks.front().Ops.front().Inputs[2].Size = 4;
    ExpectMedRejected(std::move(MedNarrowMask), "bad-pdep-pext-mask-size");
    MedFunc MedOrdered = ValidMed;
    MedOrdered.Blocks.front().Ops.front().MemoryOrdering =
        NdMemoryOrdering::Relaxed;
    ExpectMedRejected(std::move(MedOrdered), "bad-pdep-pext-ordering");
    MedFunc MedSegmented = ValidMed;
    MedSegmented.Blocks.front().Ops.front().MemoryAddressSpace =
        NdMemoryAddressSpace::X86GS;
    ExpectMedRejected(std::move(MedSegmented), "bad-pdep-pext-address-space");
    MedFunc MedNonConstantId = ValidMed;
    MedNonConstantId.Blocks.front().Ops.front().Inputs[0] = medTemp(8, 2);
    ExpectMedRejected(std::move(MedNonConstantId),
                      "bad-pdep-pext-nonconstant-id");
  }
}

TEST(X86APXAtomic, RaoOperationsUpdateMemoryWithoutChangingSourceOrFlags) {
  constexpr std::array<RaoOperation, 4> Operations = {
      RaoOperation::Add, RaoOperation::And, RaoOperation::Or,
      RaoOperation::Xor};
  constexpr uint64_t Offset = 0x5000;
  constexpr uint64_t Source = UINT64_C(0xa5a55a5af0f00f0f);
  constexpr uint64_t Old = UINT64_C(0x8877665544332211);
  const Flags Initial{true, false, true, false, true, false, true};
  const RegInfo SourceReg = mapCapstoneReg(X86_REG_R26);

  for (RaoOperation Operation : Operations) {
    for (unsigned Width : {4u, 8u}) {
      SCOPED_TRACE(::testing::Message()
                   << "rao=" << static_cast<unsigned>(Operation)
                   << " width=" << Width);
      const LiftedInstruction Lifted = liftX64(encodeRao(Operation, Width));
      ASSERT_FALSE(Lifted.Ops.empty());
      BinaryImage Image = makeImage(
          Offset, Width, SegmentFlags::Readable | SegmentFlags::Writable, Old);
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(true);
      Emulator.setLoadCollect(true);
      initializeAddress(Emulator, Offset, false);
      Emulator.setRegister(SourceReg.Offset, Source);
      setFlags(Emulator, Initial);

      ASSERT_EQ(Emulator.run(Lifted.Ops), Lifted.Ops.size());
      EXPECT_EQ(Emulator.getRegister(SourceReg.Offset), Source);
      expectFlags(Emulator, Initial);
      ASSERT_EQ(Emulator.getLoadRecords().size(), 1u);
      EXPECT_EQ(Emulator.getLoadRecords()[0].Addr, Offset);
      EXPECT_EQ(Emulator.getLoadRecords()[0].Size, Width);
      EXPECT_EQ(probeMemory(Emulator, Offset, Width),
                raoResult(Operation, Width, Old, Source));
      EXPECT_FALSE(Emulator.skips().any());
    }
  }
}

TEST(X86APXAtomic, AtomicReadsComposePartialAndOverlappingStoreBytes) {
  constexpr uint64_t Address = 0x5800;
  constexpr uint64_t Underlying = UINT64_C(0x1122334455667788);
  struct SeedStore {
    uint64_t AddressDelta;
    uint64_t Value;
    unsigned Width;
  };
  struct Case {
    std::vector<SeedStore> Stores;
    uint64_t ExpectedOld;
  };
  const std::array<Case, 4> Cases = {
      Case{{SeedStore{0, 0xaa, 1}}, UINT64_C(0x11223344556677aa)},
      Case{{SeedStore{1, 0xbb, 1}}, UINT64_C(0x112233445566bb88)},
      Case{{SeedStore{0, 0xaabbccdd, 4}}, UINT64_C(0x11223344aabbccdd)},
      Case{{SeedStore{0, UINT64_C(0xfedcba9876543210), 8},
            SeedStore{0, 0x0badf00d, 4}},
           UINT64_C(0xfedcba980badf00d)},
  };

  for (size_t Index = 0; Index < Cases.size(); ++Index) {
    SCOPED_TRACE(::testing::Message() << "case=" << Index);
    BinaryImage Image =
        makeImage(Address, 8, SegmentFlags::Readable | SegmentFlags::Writable,
                  Underlying);
    NdOpEmulator Emulator(Image);
    for (const SeedStore &Store : Cases[Index].Stores)
      ASSERT_TRUE(Emulator.step(
          makeStore(Address + Store.AddressDelta, Store.Value, Store.Width)));
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    const LowOp Atomic = makeRaoIntrinsic(Intrinsic::ApxRaoAdd, 8, Address, 1);
    ASSERT_TRUE(Emulator.step(Atomic));
    ASSERT_EQ(Emulator.getLoadRecords().size(), 1u);
    EXPECT_EQ(Emulator.getLoadRecords()[0].Addr, Address);
    EXPECT_EQ(Emulator.getLoadRecords()[0].Size, 8u);
    EXPECT_EQ(probeMemory(Emulator, Address, 8), Cases[Index].ExpectedOld + 1);
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86APXAtomic, EveryCmpConditionControlsTheAtomicWrite) {
  struct Pair {
    uint64_t Old;
    uint64_t Compare;
  };
  constexpr uint64_t Address = 0x6000;
  for (unsigned Width : {4u, 8u}) {
    const std::array<Pair, 4> Pairs = {
        Pair{Width == 4 ? UINT64_C(0x80000000) : UINT64_C(0x8000000000000000),
             1},
        Pair{1, 2}, Pair{5, 5}, Pair{2, 1}};
    const uint64_t Add =
        Width == 4 ? UINT64_C(0x12345678) : UINT64_C(0x123456789abcdef0);
    const RegInfo CompareReg =
        mapCapstoneReg(Width == 4 ? X86_REG_R26D : X86_REG_R26);
    const RegInfo AddReg =
        mapCapstoneReg(Width == 4 ? X86_REG_R17D : X86_REG_R17);
    const uint64_t CompareHigh = Width == 4 ? UINT64_C(0xaaaaaaaa00000000) : 0;
    const uint64_t AddHigh = Width == 4 ? UINT64_C(0xbbbbbbbb00000000) : 0;

    for (unsigned Condition = 0; Condition != 16; ++Condition) {
      bool SawTrue = false;
      bool SawFalse = false;
      for (const Pair &Values : Pairs) {
        SCOPED_TRACE(::testing::Message()
                     << "width=" << Width << " condition=" << Condition
                     << " old=" << Values.Old << " compare=" << Values.Compare);
        const LiftedInstruction Lifted =
            liftX64(encodeCmpccXadd(Condition, Width));
        ASSERT_EQ(Lifted.Id, X86_INS_CMPOXADD + Condition);
        BinaryImage Image = makeImage(
            Address, Width, SegmentFlags::Readable | SegmentFlags::Writable,
            Values.Old);
        NdOpEmulator Emulator(Image);
        Emulator.setStrictMode(true);
        Emulator.setLoadCollect(true);
        initializeAddress(Emulator, Address, false);
        Emulator.setRegister(CompareReg.Offset, CompareHigh | Values.Compare);
        Emulator.setRegister(AddReg.Offset, AddHigh | Add);
        setFlags(Emulator, Flags{true, true, true, true, true, true, true});

        const Flags Expected =
            subtractionFlags(Width, Values.Old, Values.Compare, true);
        const bool StoreSum = conditionHolds(Condition, Expected);
        SawTrue |= StoreSum;
        SawFalse |= !StoreSum;
        ASSERT_EQ(Emulator.run(Lifted.Ops), Lifted.Ops.size());
        EXPECT_EQ(Emulator.getRegister(CompareReg.Offset), Values.Old);
        EXPECT_EQ(Emulator.getRegister(AddReg.Offset), AddHigh | Add);
        expectFlags(Emulator, Expected);
        const uint64_t ExpectedMemory =
            StoreSum ? (Values.Old + Add) & widthMask(Width) : Values.Old;
        EXPECT_EQ(probeMemory(Emulator, Address, Width), ExpectedMemory);
        ASSERT_EQ(Emulator.getLoadRecords().size(), 2u);
        EXPECT_EQ(Emulator.getLoadRecords()[0].Addr, Address);
        EXPECT_EQ(Emulator.getLoadRecords()[0].Size, Width);
        EXPECT_FALSE(Emulator.skips().any());
      }
      EXPECT_TRUE(SawTrue) << "condition never selected the sum";
      EXPECT_TRUE(SawFalse) << "condition never selected the old value";
    }
  }
}

TEST(X86APXAtomic, DwordCompareWritebackClearsRspAndRbpUpperBits) {
  constexpr uint64_t Address = 0x6800;
  constexpr uint64_t Old = UINT64_C(0x11223344);
  constexpr uint64_t Compare = 1;
  constexpr uint64_t Add = 3;
  for (const auto &[CompareNumber, CompareOffset] :
       {std::pair<unsigned, uint64_t>{4, x86reg::RSP},
        std::pair<unsigned, uint64_t>{5, x86reg::RBP}}) {
    SCOPED_TRACE(::testing::Message() << "compare=" << CompareNumber);
    const LiftedInstruction Lifted =
        liftX64(encodeCmpccXadd(5, 4, CompareNumber));
    BinaryImage Image = makeImage(
        Address, 4, SegmentFlags::Readable | SegmentFlags::Writable, Old);
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    initializeAddress(Emulator, Address, false);
    Emulator.setRegister(CompareOffset, UINT64_C(0xdeadbeef00000000) | Compare);
    Emulator.setRegister(x86reg::R17, Add);
    setFlags(Emulator, Flags{true, true, true, true, true, true, true});

    const Flags Expected = subtractionFlags(4, Old, Compare, true);
    ASSERT_TRUE(conditionHolds(5, Expected));
    ASSERT_EQ(Emulator.run(Lifted.Ops), Lifted.Ops.size());
    EXPECT_EQ(Emulator.getRegister(CompareOffset), Old);
    EXPECT_EQ(Emulator.getRegister(x86reg::R17), Add);
    EXPECT_EQ(probeMemory(Emulator, Address, 4), Old + Add);
    expectFlags(Emulator, Expected);
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86APXAtomic, SegmentsAddress32WidthsAndEgprsArePreserved) {
  const RegInfo CompareReg = mapCapstoneReg(X86_REG_R26);
  const RegInfo AddReg = mapCapstoneReg(X86_REG_R17);
  constexpr uint64_t Offset = 0x7000;
  constexpr uint64_t Old = UINT64_C(0xfedcba9876543210);
  constexpr uint64_t Compare = UINT64_C(0x1020304050607080);
  constexpr uint64_t Add = UINT64_C(0x1112131415161718);

  for (unsigned Width : {4u, 8u}) {
    for (bool Address32 : {false, true}) {
      for (SegmentOverride Segment :
           {SegmentOverride::FS, SegmentOverride::GS}) {
        SCOPED_TRACE(::testing::Message()
                     << "width=" << Width << " address32=" << Address32
                     << " segment=" << static_cast<unsigned>(Segment));
        const uint64_t SegmentBase =
            Segment == SegmentOverride::FS ? 0x100000 : 0x200000;
        const uint64_t Target = SegmentBase + Offset;
        const LiftedInstruction Lifted = liftX64(
            encodeCmpccXadd(3, Width, 26, 17, 29, 14, Address32, Segment));
        const LowOp *Atomic =
            findIntrinsic(Lifted.Ops, Intrinsic::ApxCmpccXadd);
        ASSERT_NE(Atomic, nullptr);
        EXPECT_EQ(Atomic->MemoryAddressSpace,
                  Segment == SegmentOverride::FS ? NdMemoryAddressSpace::X86FS
                                                 : NdMemoryAddressSpace::X86GS);

        BinaryImage Image =
            makeImage(Target, Width,
                      SegmentFlags::Readable | SegmentFlags::Writable, Old);
        NdOpEmulator Emulator(Image);
        Emulator.setStrictMode(true);
        ASSERT_TRUE(Emulator.setMemoryAddressSpaceBase(
            Atomic->MemoryAddressSpace, SegmentBase));
        initializeAddress(Emulator, Offset, Address32);
        Emulator.setRegister(CompareReg.Offset, Compare);
        Emulator.setRegister(AddReg.Offset, Add);
        setFlags(Emulator,
                 Flags{false, false, false, false, false, false, true});

        ASSERT_EQ(Emulator.run(Lifted.Ops), Lifted.Ops.size());
        const uint64_t Mask = widthMask(Width);
        const Flags Expected = subtractionFlags(Width, Old, Compare, true);
        ASSERT_TRUE(conditionHolds(3, Expected));
        EXPECT_EQ(probeMemory(Emulator, Target, Width), (Old + Add) & Mask);
        EXPECT_EQ(Emulator.getRegister(CompareReg.Offset), Old & Mask);
        EXPECT_EQ(Emulator.getRegister(AddReg.Offset), Add);
        expectFlags(Emulator, Expected);
        EXPECT_FALSE(Emulator.skips().any());
      }
    }
  }
}

TEST(X86APXAtomic, ExtendedSibIndexBitIsDecodedForEveryAtomicFamily) {
  constexpr uint64_t Offset = 0x7800;
  constexpr uint64_t SegmentBase = 0x300000;
  constexpr uint64_t Target = SegmentBase + Offset;
  constexpr uint64_t Old = UINT64_C(0x8877665544332211);
  constexpr uint64_t Source = UINT64_C(0x0102030405060708);

  for (RaoOperation Operation : {RaoOperation::Add, RaoOperation::And,
                                 RaoOperation::Or, RaoOperation::Xor}) {
    for (unsigned Width : {4u, 8u}) {
      for (bool Address32 : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << "rao=" << static_cast<unsigned>(Operation)
                     << " width=" << Width << " address32=" << Address32);
        const auto Bytes =
            encodeRao(Operation, Width, 26, 29, 28, Address32,
                      Address32 ? SegmentOverride::GS : SegmentOverride::FS);
        Decoder Dec;
        ASSERT_TRUE(Dec.init(Arch::X64));
        DecodedInsn Insn{};
        ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(),
                                       kInstructionAddress, Insn),
                  static_cast<int>(Bytes.size()));
        ASSERT_NE(Insn.Raw, nullptr);
        ASSERT_NE(Insn.Raw->detail, nullptr);
        ASSERT_EQ(Insn.Raw->detail->x86.op_count, 2u);
        EXPECT_EQ(Insn.Raw->detail->x86.operands[0].mem.index,
                  Address32 ? X86_REG_R28D : X86_REG_R28);

        const LiftedInstruction Lifted = liftX64(Bytes);
        BinaryImage Image =
            makeImage(Target, Width,
                      SegmentFlags::Readable | SegmentFlags::Writable, Old);
        NdOpEmulator Emulator(Image);
        Emulator.setStrictMode(true);
        const NdMemoryAddressSpace AddressSpace =
            Address32 ? NdMemoryAddressSpace::X86GS
                      : NdMemoryAddressSpace::X86FS;
        ASSERT_TRUE(
            Emulator.setMemoryAddressSpaceBase(AddressSpace, SegmentBase));
        initializeAddress(Emulator, Offset, Address32, 29, 28);
        Emulator.setRegister(x86reg::R26, Source);
        ASSERT_EQ(Emulator.run(Lifted.Ops), Lifted.Ops.size());
        EXPECT_EQ(probeMemory(Emulator, Target, Width),
                  raoResult(Operation, Width, Old, Source));
        EXPECT_FALSE(Emulator.skips().any());
      }
    }
  }

  for (unsigned Condition : {0u, 4u, 12u, 15u}) {
    for (unsigned Width : {4u, 8u}) {
      for (bool Address32 : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << "condition=" << Condition << " width=" << Width
                     << " address32=" << Address32);
        const auto Bytes = encodeCmpccXadd(
            Condition, Width, 26, 17, 29, 28, Address32,
            Address32 ? SegmentOverride::FS : SegmentOverride::GS);
        Decoder Dec;
        ASSERT_TRUE(Dec.init(Arch::X64));
        DecodedInsn Insn{};
        ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(),
                                       kInstructionAddress, Insn),
                  static_cast<int>(Bytes.size()));
        ASSERT_NE(Insn.Raw, nullptr);
        ASSERT_NE(Insn.Raw->detail, nullptr);
        ASSERT_EQ(Insn.Raw->detail->x86.op_count, 3u);
        EXPECT_EQ(Insn.Raw->detail->x86.operands[0].mem.index,
                  Address32 ? X86_REG_R28D : X86_REG_R28);

        const LiftedInstruction Lifted = liftX64(Bytes);
        BinaryImage Image =
            makeImage(Target, Width,
                      SegmentFlags::Readable | SegmentFlags::Writable, Old);
        NdOpEmulator Emulator(Image);
        Emulator.setStrictMode(true);
        const NdMemoryAddressSpace AddressSpace =
            Address32 ? NdMemoryAddressSpace::X86FS
                      : NdMemoryAddressSpace::X86GS;
        ASSERT_TRUE(
            Emulator.setMemoryAddressSpaceBase(AddressSpace, SegmentBase));
        initializeAddress(Emulator, Offset, Address32, 29, 28);
        Emulator.setRegister(x86reg::R26, Old);
        Emulator.setRegister(x86reg::R17, Source);
        ASSERT_EQ(Emulator.run(Lifted.Ops), Lifted.Ops.size());
        const Flags Expected = subtractionFlags(Width, Old, Old, false);
        const uint64_t ExpectedMemory = conditionHolds(Condition, Expected)
                                            ? (Old + Source) & widthMask(Width)
                                            : Old & widthMask(Width);
        EXPECT_EQ(probeMemory(Emulator, Target, Width), ExpectedMemory);
        EXPECT_EQ(Emulator.getRegister(x86reg::R26), Old & widthMask(Width));
        EXPECT_FALSE(Emulator.skips().any());
      }
    }
  }
}

TEST(X86APXAtomic, CompareFlagsComeFromOldMemoryMinusCompareNotTheAdd) {
  constexpr uint64_t Address = 0x8000;
  constexpr uint64_t Old = UINT64_C(0x7fffffff);
  constexpr uint64_t Compare = UINT64_C(0xffffffff);
  constexpr uint64_t Add = 1;
  const LiftedInstruction Lifted = liftX64(encodeCmpccXadd(0, 4));
  BinaryImage Image = makeImage(
      Address, 4, SegmentFlags::Readable | SegmentFlags::Writable, Old);
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  initializeAddress(Emulator, Address, false);
  Emulator.setRegister(x86reg::R26, Compare);
  Emulator.setRegister(x86reg::R17, Add);
  setFlags(Emulator, Flags{false, false, false, false, false, false, true});

  const Flags Expected = subtractionFlags(4, Old, Compare, true);
  ASSERT_TRUE(Expected.OF);
  ASSERT_EQ(Emulator.run(Lifted.Ops), Lifted.Ops.size());
  expectFlags(Emulator, Expected);
  EXPECT_EQ(probeMemory(Emulator, Address, 4),
            (Old + Add) & UINT64_C(0xffffffff));
  EXPECT_EQ(Emulator.getRegister(x86reg::R26), Old);
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86APXAtomic, AliasedCompareAddAndAddressUseOriginalRegisterValue) {
  constexpr uint64_t Base = 0x9000;
  constexpr int8_t Displacement = 0x20;
  constexpr uint64_t Address = Base + Displacement;
  constexpr uint64_t Old = UINT64_C(0x10000);
  const LiftedInstruction Lifted = liftX64(encodeCmpccXadd(
      3, 8, 26, 26, 26, 4, false, SegmentOverride::None, Displacement));
  BinaryImage Image = makeImage(
      Address, 8, SegmentFlags::Readable | SegmentFlags::Writable, Old);
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegister(x86reg::R26, Base);
  setFlags(Emulator, Flags{true, false, true, false, true, false, true});

  const Flags Expected = subtractionFlags(8, Old, Base, true);
  ASSERT_TRUE(conditionHolds(3, Expected));
  ASSERT_EQ(Emulator.run(Lifted.Ops), Lifted.Ops.size());
  EXPECT_EQ(probeMemory(Emulator, Address, 8), Old + Base);
  EXPECT_EQ(Emulator.getRegister(x86reg::R26), Old);
  expectFlags(Emulator, Expected);
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86APXAtomic,
     FaultsAreAtomicAndFalseConditionsStillRequireWritableMemory) {
  constexpr uint64_t Aligned = 0xa000;
  constexpr uint64_t Old = UINT64_C(0x8877665544332211);
  constexpr uint64_t Source = UINT64_C(0x0102030405060708);
  const Flags Initial{true, false, true, false, true, false, true};

  struct FaultCase {
    uint64_t Address;
    SegmentFlags Permissions;
    bool Mapped;
  };
  const std::array<FaultCase, 4> Cases = {
      FaultCase{Aligned + 2, SegmentFlags::Readable | SegmentFlags::Writable,
                true},
      FaultCase{Aligned, SegmentFlags::Readable, true},
      FaultCase{Aligned, SegmentFlags::Writable, true},
      FaultCase{Aligned, SegmentFlags::None, false},
  };

  for (const FaultCase &Case : Cases) {
    SCOPED_TRACE(::testing::Message()
                 << "address=" << Case.Address
                 << " permissions=" << static_cast<unsigned>(Case.Permissions)
                 << " mapped=" << Case.Mapped);
    const LiftedInstruction Rao = liftX64(encodeRao(
        RaoOperation::Add, 8, 26, 29, 4, false, SegmentOverride::None, 0));
    BinaryImage RaoImage =
        Case.Mapped ? makeImage(Case.Address, 8, Case.Permissions, Old)
                    : makeImage(0, 0, SegmentFlags::None, 0);
    NdOpEmulator RaoEmulator(RaoImage);
    RaoEmulator.setStrictMode(true);
    RaoEmulator.setLoadCollect(true);
    RaoEmulator.setRegister(x86reg::R29, Case.Address);
    RaoEmulator.setRegister(x86reg::R26, Source);
    setFlags(RaoEmulator, Initial);
    EXPECT_LT(RaoEmulator.run(Rao.Ops), Rao.Ops.size());
    EXPECT_EQ(RaoEmulator.getRegister(x86reg::R26), Source);
    expectFlags(RaoEmulator, Initial);
    EXPECT_TRUE(RaoEmulator.getLoadRecords().empty());
    if (Case.Mapped && hasFlag(Case.Permissions, SegmentFlags::Readable))
      EXPECT_EQ(probeMemory(RaoEmulator, Case.Address, 8), Old);
    EXPECT_FALSE(RaoEmulator.skips().any());
  }

  // CMPZ is false because old memory differs from the compare register.  The
  // instruction nevertheless performs the architectural write-back of the old
  // value, so a read-only destination faults before registers or flags commit.
  const LiftedInstruction FalseCmp = liftX64(
      encodeCmpccXadd(4, 8, 26, 17, 29, 4, false, SegmentOverride::None, 0));
  BinaryImage ReadOnly = makeImage(Aligned, 8, SegmentFlags::Readable, Old);
  NdOpEmulator CmpEmulator(ReadOnly);
  CmpEmulator.setStrictMode(true);
  CmpEmulator.setLoadCollect(true);
  CmpEmulator.setRegister(x86reg::R29, Aligned);
  CmpEmulator.setRegister(x86reg::R26, 1);
  CmpEmulator.setRegister(x86reg::R17, Source);
  setFlags(CmpEmulator, Initial);
  EXPECT_LT(CmpEmulator.run(FalseCmp.Ops), FalseCmp.Ops.size());
  EXPECT_EQ(CmpEmulator.getRegister(x86reg::R26), 1u);
  EXPECT_EQ(CmpEmulator.getRegister(x86reg::R17), Source);
  expectFlags(CmpEmulator, Initial);
  EXPECT_TRUE(CmpEmulator.getLoadRecords().empty());
  EXPECT_EQ(probeMemory(CmpEmulator, Aligned, 8), Old);
  EXPECT_FALSE(CmpEmulator.skips().any());
}

TEST(X86APXAtomic, CompleteMappedAndFileBackedRangesArePreflighted) {
  constexpr uint64_t Address = 0xa800;
  constexpr uint64_t Old = UINT64_C(0x8877665544332211);
  constexpr uint64_t Source = UINT64_C(0x0102030405060708);
  constexpr uint64_t Sentinel = UINT64_C(0xdeadbeefcafef00d);
  const Flags Initial{true, false, true, false, true, false, true};

  const LiftedInstruction Rao = liftX64(encodeRao(
      RaoOperation::Add, 8, 26, 29, 4, false, SegmentOverride::None, 0));
  BinaryImage ShortMapping = makeImage(
      Address, 8, SegmentFlags::Readable | SegmentFlags::Writable, Old);
  ASSERT_EQ(ShortMapping.Segments.size(), 1u);
  ShortMapping.Segments[0].Size = 7;
  const std::vector<uint8_t> OriginalRaoBytes = ShortMapping.Segments[0].Data;
  NdOpEmulator RaoEmulator(ShortMapping);
  RaoEmulator.setStrictMode(true);
  RaoEmulator.setLoadCollect(true);
  RaoEmulator.setRegister(x86reg::R29, Address);
  RaoEmulator.setRegister(x86reg::R26, Source);
  setFlags(RaoEmulator, Initial);
  EXPECT_LT(RaoEmulator.run(Rao.Ops), Rao.Ops.size());
  EXPECT_EQ(ShortMapping.Segments[0].Data, OriginalRaoBytes);
  EXPECT_EQ(RaoEmulator.getRegister(x86reg::R26), Source);
  expectFlags(RaoEmulator, Initial);
  EXPECT_TRUE(RaoEmulator.getLoadRecords().empty());
  EXPECT_FALSE(RaoEmulator.skips().any());

  const LiftedInstruction Cmp = liftX64(
      encodeCmpccXadd(4, 8, 26, 17, 29, 4, false, SegmentOverride::None, 0));
  const LowOp *Atomic = findIntrinsic(Cmp.Ops, Intrinsic::ApxCmpccXadd);
  ASSERT_NE(Atomic, nullptr);
  BinaryImage ShortData = makeImage(
      Address, 8, SegmentFlags::Readable | SegmentFlags::Writable, Old);
  ASSERT_EQ(ShortData.Segments.size(), 1u);
  ShortData.Segments[0].Data.resize(7);
  const std::vector<uint8_t> OriginalCmpBytes = ShortData.Segments[0].Data;
  NdOpEmulator CmpEmulator(ShortData);
  CmpEmulator.setStrictMode(true);
  CmpEmulator.setLoadCollect(true);
  CmpEmulator.setRegister(x86reg::R29, Address);
  CmpEmulator.setRegister(x86reg::R26, Old);
  CmpEmulator.setRegister(x86reg::R17, Source);
  CmpEmulator.setRegister(Atomic->Output.Offset, Sentinel);
  setFlags(CmpEmulator, Initial);
  EXPECT_LT(CmpEmulator.run(Cmp.Ops), Cmp.Ops.size());
  EXPECT_EQ(ShortData.Segments[0].Data, OriginalCmpBytes);
  EXPECT_EQ(CmpEmulator.getRegister(Atomic->Output.Offset), Sentinel);
  EXPECT_EQ(CmpEmulator.getRegister(x86reg::R26), Old);
  EXPECT_EQ(CmpEmulator.getRegister(x86reg::R17), Source);
  expectFlags(CmpEmulator, Initial);
  EXPECT_TRUE(CmpEmulator.getLoadRecords().empty());
  EXPECT_FALSE(CmpEmulator.skips().any());
}

TEST(X86APXAtomic, StoreCapacityFailureDoesNotPartiallyCommit) {
  constexpr uint64_t Address = 0xac00;
  constexpr uint64_t Old = UINT64_C(0x8877665544332211);
  constexpr uint64_t Source = UINT64_C(0x0102030405060708);
  constexpr uint64_t Sentinel = UINT64_C(0xdeadbeefcafef00d);
  const Flags Initial{true, false, true, false, true, false, true};

  auto FillStoreCapacity = [](NdOpEmulator &Emulator) {
    for (int I = 0; I != limits::kMaxEmulatorStoreEntries; ++I)
      ASSERT_TRUE(Emulator.step(
          makeStore(UINT64_C(0x200000) + static_cast<uint64_t>(I) * 16,
                    static_cast<uint64_t>(I), 8)));
  };

  const LiftedInstruction Rao = liftX64(encodeRao(
      RaoOperation::Xor, 8, 26, 29, 4, false, SegmentOverride::None, 0));
  {
    BinaryImage Image = makeImage(
        Address, 8, SegmentFlags::Readable | SegmentFlags::Writable, Old);
    NdOpEmulator Emulator(Image);
    FillStoreCapacity(Emulator);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::R29, Address);
    Emulator.setRegister(x86reg::R26, Source);
    setFlags(Emulator, Initial);
    EXPECT_LT(Emulator.run(Rao.Ops), Rao.Ops.size());
    EXPECT_EQ(Emulator.getRegister(x86reg::R26), Source);
    expectFlags(Emulator, Initial);
    EXPECT_TRUE(Emulator.getLoadRecords().empty());
    EXPECT_EQ(Emulator.skips().DroppedStores, 1u);
    EXPECT_EQ(probeMemory(Emulator, Address, 8), Old);
  }

  for (bool ConditionTrue : {false, true}) {
    SCOPED_TRACE(::testing::Message() << "condition_true=" << ConditionTrue);
    const LiftedInstruction Cmp = liftX64(
        encodeCmpccXadd(4, 8, 26, 17, 29, 4, false, SegmentOverride::None, 0));
    const LowOp *Atomic = findIntrinsic(Cmp.Ops, Intrinsic::ApxCmpccXadd);
    ASSERT_NE(Atomic, nullptr);
    BinaryImage Image = makeImage(
        Address, 8, SegmentFlags::Readable | SegmentFlags::Writable, Old);
    NdOpEmulator Emulator(Image);
    FillStoreCapacity(Emulator);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::R29, Address);
    Emulator.setRegister(x86reg::R26, ConditionTrue ? Old : Old + UINT64_C(1));
    Emulator.setRegister(x86reg::R17, Source);
    Emulator.setRegister(Atomic->Output.Offset, Sentinel);
    setFlags(Emulator, Initial);
    EXPECT_LT(Emulator.run(Cmp.Ops), Cmp.Ops.size());
    EXPECT_EQ(Emulator.getRegister(Atomic->Output.Offset), Sentinel);
    EXPECT_EQ(Emulator.getRegister(x86reg::R26),
              ConditionTrue ? Old : Old + UINT64_C(1));
    EXPECT_EQ(Emulator.getRegister(x86reg::R17), Source);
    expectFlags(Emulator, Initial);
    EXPECT_TRUE(Emulator.getLoadRecords().empty());
    EXPECT_EQ(Emulator.skips().DroppedStores, 1u);
    EXPECT_EQ(probeMemory(Emulator, Address, 8), Old);
  }
}

TEST(X86APXAtomic, WriteBackOverlayCannotBypassUnderlyingReadPermission) {
  constexpr uint64_t Address = 0xae00;
  constexpr uint64_t Old = UINT64_C(0x1111222233334444);
  constexpr uint64_t Overlay = UINT64_C(0x8877665544332211);
  constexpr uint64_t Source = UINT64_C(0x0102030405060708);
  constexpr uint64_t Sentinel = UINT64_C(0xdeadbeefcafef00d);
  const Flags Initial{true, false, true, false, true, false, true};
  const LiftedInstruction Cmp = liftX64(
      encodeCmpccXadd(4, 8, 26, 17, 29, 4, false, SegmentOverride::None, 0));
  const LowOp *Atomic = findIntrinsic(Cmp.Ops, Intrinsic::ApxCmpccXadd);
  ASSERT_NE(Atomic, nullptr);

  BinaryImage Image = makeImage(Address, 8, SegmentFlags::Writable, Old);
  NdOpEmulator Emulator(Image);
  ASSERT_TRUE(Emulator.step(makeStore(Address, Overlay, 8)));
  Emulator.setStrictMode(true);
  Emulator.setLoadCollect(true);
  Emulator.setRegister(x86reg::R29, Address);
  Emulator.setRegister(x86reg::R26, UINT64_C(1));
  Emulator.setRegister(x86reg::R17, Source);
  Emulator.setRegister(Atomic->Output.Offset, Sentinel);
  setFlags(Emulator, Initial);
  EXPECT_LT(Emulator.run(Cmp.Ops), Cmp.Ops.size());
  EXPECT_EQ(Emulator.getRegister(Atomic->Output.Offset), Sentinel);
  EXPECT_EQ(Emulator.getRegister(x86reg::R26), 1u);
  EXPECT_EQ(Emulator.getRegister(x86reg::R17), Source);
  expectFlags(Emulator, Initial);
  EXPECT_TRUE(Emulator.getLoadRecords().empty());
  EXPECT_EQ(probeMemory(Emulator, Address, 8), Overlay);
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86APXAtomic, SegmentBaseParticipatesInNaturalAlignment) {
  constexpr uint64_t Offset = 0xb000;
  constexpr uint64_t SegmentBase = 2;
  constexpr uint64_t Target = Offset + SegmentBase;
  constexpr uint64_t Old = UINT64_C(0x8877665544332211);
  constexpr uint64_t Source = UINT64_C(0x0102030405060708);
  const Flags Initial{true, false, true, false, true, false, true};
  const LiftedInstruction Lifted = liftX64(encodeRao(
      RaoOperation::Add, 8, 26, 29, 4, false, SegmentOverride::FS, 0));
  const LowOp *Atomic = findIntrinsic(Lifted.Ops, Intrinsic::ApxRaoAdd);
  ASSERT_NE(Atomic, nullptr);
  ASSERT_EQ(Atomic->MemoryAddressSpace, NdMemoryAddressSpace::X86FS);

  BinaryImage Image = makeImage(
      Target, 8, SegmentFlags::Readable | SegmentFlags::Writable, Old);
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setLoadCollect(true);
  ASSERT_TRUE(Emulator.setMemoryAddressSpaceBase(NdMemoryAddressSpace::X86FS,
                                                 SegmentBase));
  Emulator.setRegister(x86reg::R29, Offset);
  Emulator.setRegister(x86reg::R26, Source);
  setFlags(Emulator, Initial);

  EXPECT_LT(Emulator.run(Lifted.Ops), Lifted.Ops.size());
  EXPECT_EQ(Emulator.getRegister(x86reg::R26), Source);
  expectFlags(Emulator, Initial);
  EXPECT_TRUE(Emulator.getLoadRecords().empty());
  EXPECT_EQ(probeMemory(Emulator, Target, 8), Old);
  EXPECT_FALSE(Emulator.skips().any());
}

} // namespace
