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

#include "neverd/ir/X86/X87Constant.h"
#include "neverd/ir/X86/X87Stack.h"
#include "neverd/ir/X86/X87Value.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/ir/low/X86/X87PartialRemainder.h"
#include "neverd/lift/X86/X86Regs.h"

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
  X87ContextKnown = false;
  X87PayloadKnown = 0;
  X87ContextState.fill(0);
  if (Img.Arch == Arch::X64)
    Registers[x86reg::X87ContextHandle] = UINT64_C(0xffffffffffff8700);
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
  // Scalar pseudo-register updates cannot authenticate the retained tags and
  // payloads of an owned snapshot. Mixing the two models must stop atomically.
  if (X87ContextKnown)
    return false;
  if (Op.NumInputs == 0 || !Op.Inputs[0].isConst() ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;

  const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
  if (x87ValueInfo(Id) && !x87ValueShapeIsValid(x87ValueLowShape(Op, Img.Arch)))
    return false;
  if (isX87ConstantControlIntrinsic(Id)) {
    if (!x87ConstantControlShapeIsValid(
            Id, x87ConstantControlLowShape(Op, Img.Arch)))
      return false;
    if (Id == Intrinsic::X87ReadControl) {
      const auto Control = getRegister(x86reg::FPU_CW);
      if (!Control)
        return false;
      writeOutput(Op.Output, *Control);
      return true;
    }
    const auto &Input = Op.Inputs[Id == Intrinsic::X87LoadConstant ? 2 : 1];
    const auto Control = Input.isConst() ? std::optional<uint64_t>(Input.Offset)
                                         : getRegister(Input.Offset);
    if (!Control)
      return false;
    const auto Status = getRegister(x86reg::FPU_SW);
    // Waiting instructions cannot pass an unknown or pending exception state.
    const auto OldControl = getRegister(x86reg::FPU_CW);
    if (!Status || !OldControl || (*Status & 0x80) ||
        (*Status & ~*OldControl & 0x3f))
      return false;
    if (Id == Intrinsic::X87WriteControl) {
      // The numerical model admits defined control fields only. Reserved PC
      // and nonarchitectural reserved bits must not acquire guessed semantics.
      if ((*Control & ~UINT64_C(0x1f7f)) || !(*Control & 0x40) ||
          ((*Control >> 8) & 3) == 1)
        return false;
      Registers[x86reg::FPU_CW] = *Control;
      WideRegisters.erase(x86reg::FPU_CW);
      return true;
    }
    const auto Bits = x87ConstantBits(Op.Inputs[1].Offset, *Control);
    if (!Bits)
      return false;
    std::array<uint8_t, 10> Bytes{};
    for (unsigned I = 0; I < 8; ++I)
      Bytes[I] = static_cast<uint8_t>(Bits->Significand >> (I * 8));
    Bytes[8] = static_cast<uint8_t>(Bits->Exponent);
    Bytes[9] = static_cast<uint8_t>(Bits->Exponent >> 8);
    writeOutputBytes(Op.Output, Bytes);
    return true;
  }
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
    const auto OldStatus = getRegister(x86reg::FPU_SW);
    if (!OldStatus)
      return false;
    const uint16_t Status =
        static_cast<uint16_t>(*OldStatus & ~X87ExceptionStatusMask);
    Registers[x86reg::FPU_SW] = Status;
    WideRegisters.erase(x86reg::FPU_SW);
    return true;
  }

  if (const auto Stack = x87StackEffect(Id); Stack && Stack->FreesOperand) {
    if (Op.NumInputs != 3 || Op.Output.Size != 0 || !Op.Inputs[1].isReg() ||
        Op.Inputs[1].Size != x86reg::FPURegSize ||
        Op.Inputs[1].Offset < x86reg::ST0 ||
        Op.Inputs[1].Offset > x86reg::ST7 ||
        (Op.Inputs[1].Offset - x86reg::ST0) % x86reg::FPURegStride != 0 ||
        !Op.Inputs[2].isConst() || Op.Inputs[2].Size != 1 ||
        Op.Inputs[2].Offset >= x86reg::FPUStackDepth)
      return false;
    // Free-register operations leave C0/C1/C2/C3 undefined. The emulator has no
    // unknown-bit status representation, so continuing would let a later
    // FNSTSW observe the stale condition codes.
    return false;
  }

  if (x87StackEffect(Id)) {
    // Physical ST slots are statically rebased by the lifter, but this
    // emulator does not track TOP through every FLD/FSTP. Updating SW.TOP
    // from its cached word could publish a false status, so stop instead.
    return false;
  }

  if (Id == Intrinsic::X87ReadStatus) {
    const auto Status = getRegister(x86reg::FPU_SW);
    if (!Status)
      return false;
    writeOutput(Op.Output, *Status);
    return true;
  }

  if (Id != Intrinsic::X87Fprem && Id != Intrinsic::X87Fprem1)
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
  const auto OldStatus = getRegister(x86reg::FPU_SW);
  if (!DividendBytes || !DivisorBytes || !OldStatus)
    return false;
  const auto Result = evaluateX87PartialRemainder(*DividendBytes, *DivisorBytes,
                                                  Id == Intrinsic::X87Fprem1);
  if (!Result)
    return false;

  const uint16_t NewStatus =
      static_cast<uint16_t>((*OldStatus & ~X87PartialRemainderConditionMask) |
                            Result->ConditionCodes);
  writeOutputBytes(Op.Output, Result->Value);
  Registers[x86reg::FPU_SW] = NewStatus;
  WideRegisters.erase(x86reg::FPU_SW);
  return true;
}

} // namespace neverd
