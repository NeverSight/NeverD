//===- NdOpEmulatorX87Context.cpp - Owned x87 concrete state ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A complete x87 snapshot, with explicit provenance for retained payloads.
//===----------------------------------------------------------------------===//

#include "neverd/Limits.h"
#include "neverd/ir/X87Capabilities.h"
#include "neverd/ir/X87Context.h"
#include "neverd/ir/X87Control.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/ir/low/X87NativeContext.h"
#include "neverd/ir/low/X87PortableContext.h"
#include "neverd/lift/X86Regs.h"

#include <algorithm>
#include <cstring>

namespace neverd {
namespace {
constexpr uint64_t ContextHandle = UINT64_C(0xffffffffffff8700);
uint16_t word(const std::array<uint8_t, 512> &S, unsigned Offset) {
  return S[Offset] | (unsigned(S[Offset + 1]) << 8);
}
} // namespace

bool NdOpEmulator::setX87Context(llvm::ArrayRef<uint8_t> Image) {
  if (Img.Arch != Arch::X64 || Image.size() != X87ContextState.size())
    return false;
  const uint16_t CW = Image[0] | (unsigned(Image[1]) << 8);
  if (!x87ControlWordIsValid(CW))
    return false;
  std::copy_n(Image.begin(), 24, X87ContextState.begin());
  std::copy_n(Image.begin() + 32, 128, X87ContextState.begin() + 32);
  X87ContextKnown = true;
  X87PayloadKnown = 0xff;
  Registers[x86reg::X87ContextHandle] = ContextHandle;
  publishX87ContextRegisters();
  return true;
}

std::optional<std::array<uint8_t, 512>> NdOpEmulator::getX87Context() const {
  if (!X87ContextKnown || X87PayloadKnown != 0xff)
    return std::nullopt;
  return X87ContextState;
}

void NdOpEmulator::publishX87ContextRegisters() {
  Registers[x86reg::FPU_CW] = word(X87ContextState, 0);
  Registers[x86reg::FPU_SW] = word(X87ContextState, 2);
  const unsigned Top = (word(X87ContextState, 2) >> 11) & 7;
  for (unsigned Physical = 0; Physical < 8; ++Physical) {
    const uint64_t Offset = x86reg::stReg(Physical);
    if (!(X87PayloadKnown & (1u << Physical))) {
      Registers.erase(Offset);
      WideRegisters.erase(Offset);
      continue;
    }
    const unsigned Logical = (Physical + 8 - Top) & 7;
    writeOutputBytes(
        NdVar::reg(Offset, 10),
        llvm::ArrayRef(X87ContextState).slice(32 + Logical * 16, 10));
  }
}

bool NdOpEmulator::executeX87Context(const LowOp &Op) {
  if (Img.Arch != Arch::X64 || !x87ContextLowShapeIsValid(Op, Img.Arch))
    return false;
  const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
  if (Id == Intrinsic::X87ContextBegin) {
    writeOutput(Op.Output, ContextHandle);
    return true;
  }
  const auto Known = [&](const NdVar &V) -> std::optional<uint64_t> {
    return V.isConst() ? std::optional<uint64_t>(V.Offset)
                       : getRegister(V.Offset);
  };
  const auto Handle = Known(Op.Inputs[1]);
  if (!Handle || *Handle != ContextHandle)
    return false;
  if (Id != Intrinsic::X87ContextStep) {
    if (X87PortableMode && (Id == Intrinsic::X87ContextMMXRead ||
                            Id == Intrinsic::X87ContextMMXWrite ||
                            Id == Intrinsic::X87ContextMMXEnter))
      return false;
    if (!X87ContextKnown)
      return false;
    const uint16_t SW = word(X87ContextState, 2);
    const unsigned Top = (SW >> 11) & 7;
    if (Id == Intrinsic::X87ContextMMXRead) {
      const unsigned Physical = Op.Inputs[2].Offset;
      if (!(X87PayloadKnown & (1u << Physical)))
        return false;
      uint64_t Value;
      std::memcpy(&Value,
                  X87ContextState.data() + 32 + ((Physical + 8 - Top) & 7) * 16,
                  8);
      writeOutput(Op.Output, Value);
    } else if (Id == Intrinsic::X87ContextMMXEnter) {
      const auto Old = X87ContextState;
      for (unsigned I = 0; I < 8; ++I)
        std::copy_n(Old.begin() + 32 + ((I + 8 - Top) & 7) * 16, 16,
                    X87ContextState.begin() + 32 + I * 16);
      X87ContextState[3] &= 0xc7;
      X87ContextState[4] = 0xff;
      Registers[x86reg::FPU_SW] = SW & 0xc7ff;
    } else if (Id == Intrinsic::X87ContextMMXWrite) {
      if (Top != 0 || X87ContextState[4] != 0xff)
        return false;
      const auto Value = Known(Op.Inputs[3]);
      if (!Value)
        return false;
      const unsigned Physical = Op.Inputs[2].Offset;
      std::memcpy(X87ContextState.data() + 32 + Physical * 16, &*Value, 8);
      X87ContextState[40 + Physical * 16] = 0xff;
      X87ContextState[41 + Physical * 16] = 0xff;
      X87PayloadKnown |= 1u << Physical;
      publishX87ContextRegisters();
    }
    return true;
  }

  const uint32_t Selector = Op.Inputs[2].Offset;
  const auto Spec = x87ContextInstruction(Selector);
  if (!Spec)
    return false;
  const unsigned Escape = Selector & 0xff, ModRM = (Selector >> 8) & 0xff;
  const bool Initialize = !(Selector & X87ContextMemory) && Escape == 0xdb &&
                          ModRM == 0xe3 && !(Selector & X87ContextWaitPrefix);
  if (!X87ContextKnown && !Initialize)
    return false;
  auto State = X87ContextState;
  if (!X87ContextKnown) {
    State.fill(0);
    State[0] = 0x7f;
    State[1] = 3;
  }
  const uint16_t CW = word(State, 0), SW = word(State, 2);
  // Pending #MF precedes address evaluation. Neither state nor output is
  // changed on this refusal, including in the lenient switch-tracing mode.
  if (Spec->Waiting && ((SW & 0x80) || (SW & ~CW & 0x3f)))
    return false;
  if (!X87PortableMode && (State[4] & ~X87PayloadKnown) && !Initialize &&
      Selector != 0x770f)
    return false;
  // FNSAVE stores every register's retained bits, empty ones included.
  if ((Selector & X87ContextMemory) && Escape == 0xdd && (ModRM >> 3) == 6 &&
      X87PayloadKnown != 0xff)
    return false;
  if (!(Selector & X87ContextMemory) && Escape == 0xd9 && ModRM == 0xe5 &&
      !(X87PayloadKnown & (1u << ((SW >> 11) & 7))))
    return false;
  const bool Approximate = x87InstructionIsProcessorDependent(Selector);
  if (Approximate && (StrictMode || !X87NativeReferenceMode))
    return false;
  if (X87PortableMode) {
    const auto *Form = x87InstructionForm(Selector);
    if (!Form || Form->PortableOperation == X87PortableOperation::None)
      return false;
  }
  const auto Flags =
      Spec->ReadsFlags ? Known(Op.Inputs[4]) : std::optional<uint64_t>(0);
  if (!Flags)
    return false;

  std::vector<uint8_t> Memory;
  std::optional<uint64_t> Address;
  if (Spec->MemoryBytes) {
    const auto Offset = Known(Op.Inputs[3]);
    if (!Offset)
      return false;
    Address = resolveMemoryAddress(Op, *Offset);
    if (!Address || !isX86CanonicalMemoryRange(*Address, Spec->MemoryBytes))
      return false;
    // Authenticate the complete operand, even in the lenient emulator. A
    // suppressed x87 store cannot authorize an unmapped architectural access.
    for (unsigned I = 0; I < Spec->MemoryBytes; ++I) {
      const auto *Segment = Img.getSegmentFor(*Address + I);
      if (!Segment || (Spec->ReadsMemory && !Segment->isReadable()) ||
          (Spec->WritesMemory && !Segment->isWritable()))
        return false;
    }
    const auto Loaded = loadMemoryBytes(*Address, Spec->MemoryBytes);
    if (!Loaded)
      return false;
    Memory = *Loaded;
    if (Spec->WritesMemory && !MemStore.contains(*Address) &&
        MemStore.size() >= limits::kMaxEmulatorStoreEntries)
      return false;
    // Environment loads can establish tags, but never manufacture evidence
    // for unknown retained register payloads.
    if ((Escape == 0xd9 && ModRM == 0x20) && X87PayloadKnown != 0xff)
      return false;
    if ((Escape == 0xd9 && (ModRM == 0x20 || ModRM == 0x28)) ||
        (Escape == 0xdd && ModRM == 0x20)) {
      const uint16_t Incoming = Memory[0] | (unsigned(Memory[1]) << 8);
      if (!x87ControlWordIsValid(Incoming))
        return false;
    }
  }
  uint64_t PreviousIP = 0;
  uint64_t PreviousDP = 0;
  const uint16_t PreviousOpcode = word(State, 6);
  std::memcpy(&PreviousIP, State.data() + 8, 8);
  std::memcpy(&PreviousDP, State.data() + 16, 8);
  uint64_t Result = 0;
  auto PayloadKnown = X87PayloadKnown;
  if (X87PortableMode) {
    if (executePortableX87Context(Selector, State, PayloadKnown, Memory,
                                  Op.Inputs[5].Offset, Address.value_or(0),
                                  Result) != X87PortableOutcome::Completed)
      return false;
  } else if (!executeNativeX87Context(Selector, State,
                                      Memory.empty() ? nullptr : Memory.data(),
                                      *Flags, Result)) {
    return false;
  }
  uint64_t CurrentIP = 0;
  std::memcpy(&CurrentIP, State.data() + 8, 8);
  if (!X87PortableMode && x87ContextUpdatesInstructionPointer(Selector) &&
      CurrentIP != PreviousIP)
    std::memcpy(State.data() + 8, &Op.Inputs[5].Offset, 8);
  if (!X87PortableMode && x87ContextUpdatesInstructionPointer(Selector) &&
      (word(State, 6) != PreviousOpcode || (word(State, 2) & 0x80))) {
    const uint16_t FOP =
        ((Escape & 7) << 8) |
        ((Selector & X87ContextMemory)
             ? (Selector >> X87ContextOriginalModRMShift) & 0xff
             : ModRM);
    std::memcpy(State.data() + 6, &FOP, 2);
  }
  if (!X87PortableMode && Address &&
      x87ContextUpdatesInstructionPointer(Selector)) {
    uint64_t CurrentDP = 0;
    std::memcpy(&CurrentDP, State.data() + 16, 8);
    if (CurrentDP != PreviousDP)
      std::memcpy(State.data() + 16, &*Address, 8);
  }
  if (Spec->WritesMemory && !storeMemoryBytes(*Address, Memory))
    return false;
  if (CollectLoads && Spec->ReadsMemory)
    LoadLog.push_back({*Address, static_cast<uint16_t>(Spec->MemoryBytes)});
  if (X87PortableMode)
    X87PayloadKnown = PayloadKnown;
  else if ((Selector & X87ContextMemory) && Escape == 0xdd && ModRM == 0x20)
    X87PayloadKnown = 0xff;
  else if (!Initialize &&
           !((Selector & X87ContextMemory) && Escape == 0xd9 && ModRM == 0x20))
    X87PayloadKnown |= State[4];
  X87ContextState = State;
  X87ContextKnown = true;
  publishX87ContextRegisters();
  if (Spec->WritesFlags)
    Result = (Result & 0xd5) | ((Result & 0x100) ? 0x800 : 0);
  if (Op.Output.Size)
    writeOutput(Op.Output, Result);
  if (Approximate)
    ++Skips.ApproximatedOps;
  return true;
}
} // namespace neverd
