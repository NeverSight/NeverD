//===- MIPS16ReturnTransport.cpp - Proven FP result transfers ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Qualify pure o32 GPR-to-FPR return helpers from decoded LowIR. Names,
/// declarations and the fact that a body returns are insufficient evidence.
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/MIPS/MIPS16ReturnTransport.h"

#include "RegisterByteTransport.h"

#include "neverd/ArchSupport.h"
#include "neverd/decode/Decoder.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ExecutableCodeOwnerIndex.h"

#include <algorithm>
#include <array>

namespace neverd {
namespace {

struct ReturnTransport : mips::RegisterByteTransport {
  std::array<unsigned, 4> Writes{};

  bool step(const LowOp &Op, bool FP64, bool &WroteRegister) {
    if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs == 1 &&
        Op.Inputs[0].isConst() &&
        Op.Inputs[0].Offset == unsigned(Intrinsic::MIPSUnpredictable) &&
        Op.Output.isTemp() && Op.Output.Size == 4)
      return write(Op.Output, Bytes(4, UnknownByte));
    if (!RegisterByteTransport::step(Op))
      return false;
    const auto &Output = Op.Output;
    if (!Output.isReg() || !Output.Size)
      return true;
    if (Output.Offset < mipsreg::w(0) || Output.Offset > mipsreg::w(3) ||
        (Output.Offset - mipsreg::w(0)) % mipsreg::VectorBytes ||
        Output.Size != (FP64 ? 8 : 4))
      return false;
    const unsigned Index =
        (Output.Offset - mipsreg::w(0)) / mipsreg::VectorBytes;
    if (++Writes[Index] > (FP64 ? 2u : 1u))
      return false;
    WroteRegister = true;
    return true;
  }

  bool matches(bool FP64, bool Little, unsigned ElementBytes,
               unsigned Elements) const {
    std::array<std::array<uint64_t, 8>, 4> Expected;
    for (unsigned R = 0; R != 4; ++R)
      for (unsigned I = 0; I != 8; ++I)
        Expected[R][I] = RegisterByteBase + mipsreg::w(R) + I;
    for (unsigned E = 0; E != Elements; ++E) {
      for (unsigned I = 0; I != ElementBytes; ++I) {
        const unsigned Word = ElementBytes == 4 ? 0 : (I / 4) ^ !Little;
        const unsigned GPR = 2 + E * (ElementBytes / 4) + Word;
        const unsigned R = 2 * E + (!FP64 && ElementBytes == 8 ? I / 4 : 0);
        Expected[R][FP64 ? I : I % 4] =
            RegisterByteBase + mipsreg::r(GPR) + I % 4;
      }
      if (FP64 && ElementBytes == 4)
        for (unsigned I = 4; I != 8; ++I)
          Expected[2 * E][I] = UnknownByte;
    }
    for (unsigned R = 0; R != 4; ++R) {
      const auto Actual = read(NdVar::reg(mipsreg::w(R), FP64 ? 8 : 4));
      if (!std::equal(Actual->begin(), Actual->end(), Expected[R].begin()))
        return false;
    }
    return true;
  }
};

} // namespace

std::optional<MIPS16ReturnTransport>
proveMIPS16ReturnTransport(const BinaryImage &Image, va_t Entry,
                           const ExecutableCodeOwnerIndex *Owners) {
  if (!isMIPS(Image.Arch) || isMIPS64(Image.Arch) || !Image.MIPS ||
      !Image.MIPS->HasMIPS16 || !Image.MIPS->hasScalarFPABI(false) ||
      Image.instructionModeAt(Entry) != InstructionMode::MIPSClassic)
    return std::nullopt;
  const va_t End = Image.getFunctionMetadataEnd(Entry, Owners);
  if (End == InvalidVA || End <= Entry)
    return std::nullopt;
  Decoder Decode;
  if (!Decode.init(Image))
    return std::nullopt;
  const bool FP64 = Image.MIPS->hasScalarFP64ABI(false);
  ReturnTransport State;
  MIPS16ReturnTransport Result;
  bool Returned = false;
  for (va_t PC = Entry; PC < End;) {
    if (Image.instructionModeAt(PC) != InstructionMode::MIPSClassic ||
        !Decode.selectMode(Image, PC))
      return std::nullopt;
    const size_t Available = std::min<va_t>(End - PC, 8);
    const auto *Code = Image.readVA(PC, Available);
    DecodedExecutionUnit Unit;
    if (!Code || !Decode.decodeUnitForLift(Code, Available, PC, Unit))
      return std::nullopt;
    std::vector<LowOp> Ops;
    try {
      Decode.liftUnitToLow(Unit, Ops);
    } catch (const UnliftedInstruction &) {
      return std::nullopt;
    }
    State.Temporaries.clear();
    bool WroteRegister = false;
    for (const LowOp &Op : Ops) {
      if (Returned)
        return std::nullopt;
      if (Op.Opcode == NdOp::RETURN && Op.NumInputs == 1) {
        const auto Target = State.read(Op.Inputs[0]);
        if (!Target || Target->size() != 4 || !Unit.HasDelaySlot)
          return std::nullopt;
        for (unsigned I = 0; I != 4; ++I)
          if ((*Target)[I] !=
              ReturnTransport::RegisterByteBase + mipsreg::RA + I)
            return std::nullopt;
        Returned = true;
      } else {
        if (!State.step(Op, FP64, WroteRegister))
          return std::nullopt;
        Result.Effects.push_back(Op);
      }
    }
    if (Returned)
      break;
    // Every nonterminal unit must advance a bounded FPR write. Padding,
    // memory, unknown operations and loops cannot extend this leaf proof.
    if (!WroteRegister)
      return std::nullopt;
    PC += Unit.spanBytes();
  }
  if (!Returned)
    return std::nullopt;
  for (unsigned Elements : {1u, 2u})
    for (unsigned Bytes : {4u, 8u})
      if (State.matches(FP64, archLittleEndian(Image.Arch), Bytes, Elements)) {
        Result.ElementBytes = Bytes;
        Result.Elements = Elements;
        return Result;
      }
  return std::nullopt;
}

} // namespace neverd
