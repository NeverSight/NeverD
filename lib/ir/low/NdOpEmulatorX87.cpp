//===- NdOpEmulatorX87.cpp - Exact x87 state emulation -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Strict concrete semantics for the x87 state operations used by iterative
/// FPREM/FPREM1 loops and by FNINIT/FNSTCW control-word probes.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/ir/low/X87PartialRemainder.h"
#include "neverd/lift/X86Regs.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <utility>

namespace neverd {

namespace {

constexpr uint16_t X87DefaultControlWord = UINT16_C(0x037f);
constexpr uint16_t X87ExceptionStatusMask = UINT16_C(0x80ff);

} // namespace

NdOpEmulator::NdOpEmulator(const BinaryImage &Image) : Img(Image) {
  resetX87State();
}

NdOpEmulator::NdOpEmulator(BinaryImage &&Image)
    : OwnedImg(std::make_shared<BinaryImage>(std::move(Image))),
      Img(*OwnedImg) {
  resetX87State();
}

void NdOpEmulator::resetX87State() {
  if (Img.Arch != Arch::X86 && Img.Arch != Arch::X64)
    return;
  // These pseudo-registers are complete 16-bit architectural containers, not
  // subregister writes into a wider GPR alias.
  Registers[x86reg::FPU_CW] = X87DefaultControlWord;
  Registers[x86reg::FPU_SW] = 0;
  WideRegisters.erase(x86reg::FPU_CW);
  WideRegisters.erase(x86reg::FPU_SW);
}

bool NdOpEmulator::executeX87(const LowOp &Op) {
  if (Img.Arch != Arch::X86 && Img.Arch != Arch::X64)
    return false;
  if (Op.NumInputs == 0 || !Op.Inputs[0].isConst() ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;

  const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
  if (Id == Intrinsic::X87Wait) {
    if (Op.NumInputs != 1 || Op.Output.Size != 0)
      return false;
    const auto StatusValue = getRegister(x86reg::FPU_SW);
    const auto ControlValue = getRegister(x86reg::FPU_CW);
    if (!StatusValue || !ControlValue)
      return false;
    const uint16_t Status = static_cast<uint16_t>(*StatusValue);
    const uint16_t Control = static_cast<uint16_t>(*ControlValue);
    // A pending unmasked exception makes WAIT trap. The lightweight emulator
    // has no exception delivery, so stop instead of continuing past the trap.
    return (Status & UINT16_C(0x80)) == 0 &&
           (Status & ~Control & UINT16_C(0x3f)) == 0;
  }

  if (Id == Intrinsic::X87Fninit) {
    if (Op.NumInputs != 1 || Op.Output.Size != 0)
      return false;

    // FNINIT makes every x87 tag empty.  The emulator has no separate tag
    // array, so invalidate the cached payloads: retaining them would let a
    // later malformed stack read fold a stale pre-reset value.
    for (unsigned Index = 0; Index != x86reg::FPUStackDepth; ++Index) {
      const uint64_t Offset =
          x86reg::ST0 + static_cast<uint64_t>(Index) * x86reg::FPURegStride;
      Registers.erase(Offset);
      WideRegisters.erase(Offset);
    }
    resetX87State();
    return true;
  }

  if (Id == Intrinsic::X87Fnclex) {
    if (Op.NumInputs != 1 || Op.Output.Size != 0)
      return false;
    const uint16_t Status = static_cast<uint16_t>(
        getRegister(x86reg::FPU_SW).value_or(0) & ~X87ExceptionStatusMask);
    Registers[x86reg::FPU_SW] = Status;
    WideRegisters.erase(x86reg::FPU_SW);
    return true;
  }

  if (Id == Intrinsic::X87Ffree) {
    if (Op.NumInputs != 3 || Op.Output.Size != 0 || !Op.Inputs[1].isReg() ||
        Op.Inputs[1].Size != x86reg::FPURegSize ||
        Op.Inputs[1].Offset < x86reg::ST0 ||
        Op.Inputs[1].Offset > x86reg::ST7 ||
        (Op.Inputs[1].Offset - x86reg::ST0) % x86reg::FPURegStride != 0 ||
        !Op.Inputs[2].isConst() || Op.Inputs[2].Size != 1 ||
        Op.Inputs[2].Offset >= x86reg::FPUStackDepth)
      return false;
    // FFREE leaves C0/C1/C2/C3 undefined. The strict emulator has no
    // unknown-bit status representation, so continuing would let a later
    // FNSTSW observe the stale condition codes.
    return false;
  }

  if (Id == Intrinsic::X87Fincstp) {
    // Physical ST slots are statically rebased by the lifter, but this
    // emulator does not track TOP through every FLD/FSTP. Updating SW.TOP
    // from its cached word could publish a false status, so stop instead.
    return false;
  }

  if (Id == Intrinsic::X87ReadStatus) {
    if (Op.NumInputs != 1 || Op.Output.Size != 2 ||
        (!Op.Output.isReg() && !Op.Output.isTemp()))
      return false;
    writeOutput(Op.Output, getRegister(x86reg::FPU_SW).value_or(0));
    return true;
  }

  if (Id != Intrinsic::X87Fprem && Id != Intrinsic::X87Fprem1)
    return false;
  if (Op.NumInputs != 3 || Op.Output.Size != x86reg::FPURegSize ||
      Op.Inputs[1].Size != x86reg::FPURegSize ||
      Op.Inputs[2].Size != x86reg::FPURegSize ||
      (!Op.Output.isReg() && !Op.Output.isTemp()))
    return false;

  auto knownX87Bytes =
      [&](const NdVar &Operand) -> std::optional<std::array<uint8_t, 10>> {
    if ((!Operand.isReg() && !Operand.isTemp()) ||
        Operand.Size != x86reg::FPURegSize)
      return std::nullopt;
    const auto It = WideRegisters.find(Operand.Offset);
    if (It == WideRegisters.end() || It->second.size() < x86reg::FPURegSize)
      return std::nullopt;
    std::array<uint8_t, 10> Bytes{};
    std::copy_n(It->second.begin(), Bytes.size(), Bytes.begin());
    return Bytes;
  };

  const auto DividendBytes = knownX87Bytes(Op.Inputs[1]);
  const auto DivisorBytes = knownX87Bytes(Op.Inputs[2]);
  if (!DividendBytes || !DivisorBytes)
    return false;
  const auto Result = evaluateX87PartialRemainder(*DividendBytes, *DivisorBytes,
                                                  Id == Intrinsic::X87Fprem1);
  if (!Result)
    return false;

  const uint16_t OldStatus =
      static_cast<uint16_t>(getRegister(x86reg::FPU_SW).value_or(0));
  const uint16_t NewStatus = static_cast<uint16_t>(
      (OldStatus & ~X87PartialRemainderConditionMask) | Result->ConditionCodes);
  writeOutputBytes(Op.Output, Result->Value);
  Registers[x86reg::FPU_SW] = NewStatus;
  WideRegisters.erase(x86reg::FPU_SW);
  return true;
}

} // namespace neverd
