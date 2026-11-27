//===- RegistrationCRTFrame.cpp - Authenticate PE32 CRT invocations -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks PE32 CRT output cells against registration frame ownership.

#include "RegistrationCRTFrame.h"

#include "../../../loader/ReadOnlyBytesIndex.h"
#include "RegistrationABIPrivate.h"
#include "RegistrationFrame.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/RuntimeProviders.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"

#include <algorithm>

namespace neverd::registration_state {

bool isCheckedControlFPSlot(const BinaryImage &Image, va_t Slot) {
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF)
    return false;
  // Reject unrelated identities before scanning current storage and fixups.
  const auto *Imported = Image.findImportAt(Slot);
  if (!Imported || Imported->IsDelayImport || Imported->Name != "_controlfp_s")
    return false;
  return coff_loader::isX86ControlFPProvider(Imported->Module) &&
         isFileBackedPEImportSlot(Image, Slot);
}

std::optional<va_t> checkedCRTCallSlot(const BinaryImage &Image,
                                       const LowOp &Op, size_t &Work) {
  const bool Indirect = Op.Opcode == NdOp::INDIR_CALL;
  const unsigned Size = Indirect ? 6 : 5;
  if ((!Indirect && Op.Opcode != NdOp::CALL) || Op.NumInputs != 1 ||
      Op.Seq < 0 || !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 4 ||
      Op.Inputs[0].Offset > UINT32_MAX ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.Addr > UINT32_MAX - Size ||
      !registration_abi::chargeCalleeWork(Work, Indirect ? 10 : 15))
    return std::nullopt;
  const va_t Target = Op.Inputs[0].Offset;
  if (Indirect) {
    const auto Bytes =
        readImmutablePE32CodeBytes(Image, Op.Addr, 6, {Op.Addr + 2});
    if (!Bytes || (*Bytes)[0] != 0xff || (*Bytes)[1] != 0x15 ||
        readLE<uint32_t>(Bytes->data() + 2) != Target)
      return std::nullopt;
    return Target;
  }
  // A direct CRT veneer is the exact E8 occurrence followed by one immutable
  // FF25 import jump. Cached stub names do not authenticate either instruction.
  if (Target > UINT32_MAX - 6)
    return std::nullopt;
  const auto Bytes = readImmutablePE32CodeBytes(Image, Op.Addr, 5, {});
  const auto Stub = readImmutablePE32CodeBytes(Image, Target, 6, {Target + 2});
  if (!Bytes || (*Bytes)[0] != 0xe8 ||
      uint32_t(Op.Addr + 5 + int64_t(readLE<int32_t>(Bytes->data() + 1))) !=
          Target ||
      !Stub || (*Stub)[0] != 0xff || (*Stub)[1] != 0x25)
    return std::nullopt;
  return readLE<uint32_t>(Stub->data() + 2);
}

namespace {
const LowInstructionBoundary *precedingInstruction(const LowBlock &Block,
                                                   va_t Address, size_t &Work) {
  const auto &Bounds = Block.InstructionBoundaries;
  if (!registration_abi::chargeCalleeWork(Work, Bounds.size()))
    return nullptr;
  const auto Call =
      std::find_if(Bounds.begin(), Bounds.end(),
                   [&](const auto &I) { return I.Address == Address; });
  if (Call == Bounds.begin() || Call == Bounds.end())
    return nullptr;
  const auto &Previous = Call[-1];
  return Address >= Previous.Size && Previous.Address == Address - Previous.Size
             ? &Previous
             : nullptr;
}
} // namespace

std::optional<StartupQuery> kernelStartupQuery(const Import &Imported) {
  if (!llvm::StringRef(Imported.Module).equals_insensitive("kernel32.dll"))
    return std::nullopt;
  if (Imported.Name == "GetCurrentProcessId" ||
      Imported.Name == "GetCurrentThreadId")
    return StartupQuery::Identifier;
  if (Imported.Name == "GetSystemTimeAsFileTime" ||
      Imported.Name == "QueryPerformanceCounter")
    return StartupQuery::Clock;
  return std::nullopt;
}

bool hasPrivateStartupClockOutput(const BinaryImage &Image,
                                  const LowBlock &Block, const LowOp &Op,
                                  va_t Slot, const FrameState &Frame,
                                  size_t &Work) {
  const auto *Imported = Image.findImportAt(Slot);
  if (!Imported || kernelStartupQuery(*Imported) != StartupQuery::Clock ||
      !Frame.ImportBindingsIntact || !isFileBackedPEImportSlot(Image, Slot) ||
      checkedCRTCallSlot(Image, Op, Work) != Slot)
    return false;
  const auto *Push = precedingInstruction(Block, Op.Addr, Work);
  if (!Push || Push->Size != 1)
    return false;
  const auto Byte = readImmutableCodeBytes(Image, Push->Address, 1);
  if (!Byte || (*Byte)[0] < 0x50 || (*Byte)[0] > 0x57 || (*Byte)[0] == 0x54)
    return false;
  // PUSH leaves its source GPR intact. The complete output must be above the
  // argument slot, in the active private frame, below the original return PC.
  const auto &SP = Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride];
  const auto &Buffer = Frame.Registers[(*Byte)[0] - 0x50];
  return SP.Offset && SP == FrameValue::frame(*SP.Offset) && Buffer.Offset &&
         Buffer == FrameValue::frame(*Buffer.Offset) &&
         *Buffer.Offset >= int64_t(*SP.Offset) + 4 &&
         int64_t(*Buffer.Offset) + 8 <= 0;
}

bool isCheckedCRTStandardStreamCall(const BinaryImage &Image,
                                    const LowBlock &Block, const LowOp &Op,
                                    va_t Slot, size_t &Work) {
  const auto *Import = Image.findImportAt(Slot);
  if (!Import || Import->Name != "__acrt_iob_func" ||
      !coff_loader::isUCRTStdioProvider(Import->Module) ||
      !isFileBackedPEImportSlot(Image, Slot) ||
      checkedCRTCallSlot(Image, Op, Work) != Slot)
    return false;
  const auto *Push = precedingInstruction(Block, Op.Addr, Work);
  if (!Push || (Push->Size != 2 && Push->Size != 5))
    return false;
  const auto Bytes =
      readImmutablePE32CodeBytes(Image, Push->Address, Push->Size, {}, Work,
                                 limits::kMaxRegistrationEHStateWork);
  if (!Bytes)
    return false;
  // UCRT returns the FILE member of the selected stdin, stdout or stderr
  // record. A different index has no checked stream object.
  return Push->Size == 2
             ? (*Bytes)[0] == 0x6a && (*Bytes)[1] <= 2
             : (*Bytes)[0] == 0x68 && readLE<uint32_t>(Bytes->data() + 1) <= 2;
}

std::optional<std::vector<va_t>>
checkedCRTInitializerTargets(const BinaryImage &Image, const LowBlock &Block,
                             const LowOp &Op, va_t Slot, size_t &Work) {
  const auto *Import = Image.findImportAt(Slot);
  if (!Import ||
      (Import->Name != "_initterm" && Import->Name != "_initterm_e") ||
      Import->IsDelayImport ||
      !coff_loader::isUCRTRuntimeProvider(Import->Module) || Op.Addr < 10 ||
      checkedCRTCallSlot(Image, Op, Work) != Slot ||
      !registration_abi::checkedRegistrationImportStackPop(Image, Slot) ||
      !registration_abi::chargeCalleeWork(
          Work, Block.InstructionBoundaries.size() + 1))
    return std::nullopt;

  // MSVC pushes end and then begin immediately before the call. Requiring
  // both instructions in this block excludes edges entering its second push.
  const auto &Bounds = Block.InstructionBoundaries;
  const auto Call =
      std::find_if(Bounds.begin(), Bounds.end(),
                   [&](const auto &I) { return I.Address == Op.Addr; });
  if (Call == Bounds.end() || Call - Bounds.begin() < 2 ||
      Call[-2].Address != Op.Addr - 10 || Call[-2].Size != 5 ||
      Call[-1].Address != Op.Addr - 5 || Call[-1].Size != 5)
    return std::nullopt;
  const size_t Limit = limits::kMaxRegistrationEHStateWork;
  const auto Pushes = readRelocatedPE32CodeBytes(
      Image, Op.Addr - 10, 10, {Op.Addr - 9, Op.Addr - 4}, Work, Limit);
  if (!Pushes || (*Pushes)[0] != 0x68 || (*Pushes)[5] != 0x68)
    return std::nullopt;
  const va_t Begin = readLE<uint32_t>(Pushes->data() + 6);
  const va_t End = readLE<uint32_t>(Pushes->data() + 1);
  if (Begin >= End || (Begin | End) % 4 ||
      !registration_abi::chargeCalleeWork(Work, (End - Begin) / 4))
    return std::nullopt;

  std::vector<va_t> Targets;
  for (va_t Address = Begin; Address < End; Address += 4) {
    if (auto Target =
            readImmutablePE32CodePointerStorage(Image, Address, Work, Limit)) {
      Targets.push_back(*Target);
      continue;
    }
    // A missing pointer receipt can also be an overlap or a mutable slot.
    // Only independently immutable, unrelocated zero bytes are padding.
    const auto Bytes = readImmutableImageBytes(Image, Address, 4, Work, Limit);
    if (!Bytes || readLE<uint32_t>(Bytes->data()) != 0)
      return std::nullopt;
  }
  return Targets;
}

bool isCheckedControlFPCall(const BinaryImage &Image, const LowOp &Op,
                            size_t &Work) {
  const auto Slot = checkedCRTCallSlot(Image, Op, Work);
  return Slot && isCheckedControlFPSlot(Image, *Slot);
}

} // namespace neverd::registration_state
