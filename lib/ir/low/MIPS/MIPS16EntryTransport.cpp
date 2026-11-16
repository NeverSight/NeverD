//===- MIPS16EntryTransport.cpp - Proven classic entry transfers --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Replay pure register transfers and the physical delay slot once for both
/// call-graph discovery and source ABI projection.
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/MIPS/MIPS16EntryTransport.h"

#include "RegisterByteTransport.h"

#include "neverd/ArchSupport.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/MIPS/MIPSControlTarget.h"
#include "neverd/lift/MIPS/MIPSRegs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ExecutableCodeOwnerIndex.h"

#include <algorithm>
#include <array>
#include <vector>

namespace neverd {
namespace {

struct EntryTransport : mips::RegisterByteTransport {
  std::array<bool, 4> ArgumentWrites{};
  unsigned TargetWrites = 0;

  bool step(const LowOp &Op, bool &WroteRegister) {
    if (!RegisterByteTransport::step(Op))
      return false;
    const auto &Output = Op.Output;
    if (!Output.isReg() || !Output.Size)
      return true;
    if (Output.Size != 4)
      return false;
    if (Output.Offset == mipsreg::r(25)) {
      if (++TargetWrites > 2 || !constant(*read(Output)))
        return false;
    } else if (Output.Offset >= mipsreg::r(4) &&
               Output.Offset <= mipsreg::r(7) &&
               Output.Offset % mipsreg::SlotBytes == 0) {
      auto &Written = ArgumentWrites[Output.Offset / mipsreg::SlotBytes - 4];
      if (Written)
        return false;
      Written = true;
    } else {
      return false;
    }
    WroteRegister = true;
    return true;
  }
};

} // namespace

std::optional<MIPS16EntryTransport>
proveMIPS16EntryTransport(const BinaryImage &Image, va_t Entry,
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
  EntryTransport State;
  std::optional<va_t> Body;
  va_t Transfer = InvalidVA;
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
      if (Body)
        return std::nullopt;
      if (Op.Opcode == NdOp::INDIR_BR && Op.NumInputs == 1) {
        const auto Target = State.read(Op.Inputs[0]);
        const auto Raw = Target ? EntryTransport::constant(*Target)
                                : std::optional<uint64_t>{};
        if (!Raw || !Unit.HasDelaySlot)
          return std::nullopt;
        Transfer = Unit.Head.Addr;
        Body = qualifiedMIPSControlTarget(
            Image, InstructionMode::MIPSClassic,
            Decode.controlTargetMode(Unit.Head, InstructionMode::MIPSClassic),
            *Raw);
        if (!Body || Image.instructionModeAt(*Body) != InstructionMode::MIPS16)
          return std::nullopt;
      } else if (!State.step(Op, WroteRegister)) {
        return std::nullopt;
      }
    }
    if (Body)
      break;
    // At most two writes construct t9, and each argument register is written
    // once. Every other nonterminal unit refuses, so metadata size cannot
    // turn adapter discovery into an unbounded straight-line scan.
    if (!WroteRegister)
      return std::nullopt;
    PC += Unit.spanBytes();
  }
  if (!Body ||
      std::none_of(State.ArgumentWrites.begin(), State.ArgumentWrites.end(),
                   [](bool X) { return X; }))
    return std::nullopt;
  MIPS16EntryTransport Result;
  Result.Body = *Body;
  Result.Transfer = Transfer;
  for (unsigned A = 0; A < 4; ++A)
    for (unsigned I = 0; I < 4; ++I) {
      const auto Value = State.read(NdVar::reg(mipsreg::r(4 + A) + I, 1));
      if (Value->front() >= EntryTransport::RegisterByteBase)
        Result.ArgumentBytes[A * 4 + I] =
            Value->front() - EntryTransport::RegisterByteBase;
    }
  return Result;
}

} // namespace neverd
