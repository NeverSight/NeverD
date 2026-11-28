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
#include "RegistrationMemoryPermissions.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/RegistrationABI.h"
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

namespace {
std::optional<va_t> checkedCallSlot(const BinaryImage &Image, const LowOp &Op,
                                    size_t &Work, bool RequireSlotFixup) {
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
  auto readSlotInstruction = [&](va_t Address) {
    if (RequireSlotFixup)
      return readRelocatedPE32CodeBytes(Image, Address, 6, {Address + 2}, Work,
                                        limits::kMaxRegistrationEHStateWork);
    return readImmutablePE32CodeBytes(Image, Address, 6, {Address + 2});
  };
  if (Indirect) {
    const auto Bytes = readSlotInstruction(Op.Addr);
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
  const auto Stub = readSlotInstruction(Target);
  if (!Bytes || (*Bytes)[0] != 0xe8 ||
      uint32_t(Op.Addr + 5 + int64_t(readLE<int32_t>(Bytes->data() + 1))) !=
          Target ||
      !Stub || (*Stub)[0] != 0xff || (*Stub)[1] != 0x25)
    return std::nullopt;
  return readLE<uint32_t>(Stub->data() + 2);
}

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

std::optional<uint32_t> pushedScalarArgument(const BinaryImage &Image,
                                             const LowBlock &Block, va_t Call,
                                             bool AllowGetter, size_t &Work);

std::optional<uint32_t> pushedImageAddress(const BinaryImage &Image,
                                           const LowInstructionBoundary *Push,
                                           size_t &Work) {
  if (!Push || Push->Size != 5)
    return std::nullopt;
  const auto Bytes =
      readRelocatedPE32CodeBytes(Image, Push->Address, 5, {Push->Address + 1},
                                 Work, limits::kMaxRegistrationEHStateWork);
  if (!Bytes || (*Bytes)[0] != 0x68)
    return std::nullopt;
  return readLE<uint32_t>(Bytes->data() + 1);
}
} // namespace

std::optional<va_t> checkedCRTCallSlot(const BinaryImage &Image,
                                       const LowOp &Op, size_t &Work) {
  return checkedCallSlot(Image, Op, Work, false);
}

std::optional<KernelStartupCall> kernelStartupCall(const Import &Imported) {
  if (!llvm::StringRef(Imported.Module).equals_insensitive("kernel32.dll"))
    return std::nullopt;
  if (Imported.Name == "GetCurrentProcessId" ||
      Imported.Name == "GetCurrentThreadId")
    return KernelStartupCall::Identifier;
  if (Imported.Name == "GetSystemTimeAsFileTime" ||
      Imported.Name == "QueryPerformanceCounter")
    return KernelStartupCall::Clock;
  if (Imported.Name == "InitializeSListHead")
    return KernelStartupCall::InitializeSList;
  if (Imported.Name == "SetUnhandledExceptionFilter")
    return KernelStartupCall::SetExceptionFilter;
  if (Imported.Name == "IsProcessorFeaturePresent")
    return KernelStartupCall::ProcessorFeature;
  return std::nullopt;
}

bool hasBindingPreservingKernelArgument(const BinaryImage &Image,
                                        const LowBlock &Block, const LowOp &Op,
                                        va_t Slot, const FrameState &Frame,
                                        size_t &Work) {
  const auto *Imported = Image.findImportAt(Slot);
  if (!Imported || !Frame.ImportBindingsIntact)
    return false;
  const auto Call = kernelStartupCall(*Imported);
  if (!Call || *Call == KernelStartupCall::Identifier ||
      !isFileBackedPEImportSlot(Image, Slot) ||
      checkedCallSlot(Image, Op, Work,
                      *Call == KernelStartupCall::InitializeSList ||
                          *Call == KernelStartupCall::ProcessorFeature) != Slot)
    return false;
  if (*Call == KernelStartupCall::ProcessorFeature) {
    // The CRT queries PF_XMMI64_INSTRUCTIONS_AVAILABLE and
    // PF_FASTFAIL_AVAILABLE before choosing a code path on this machine.
    // The scalar query retains the IAT and leaves the BOOL result unknown.
    const auto &SP = Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride];
    if (!SP.Offset || *SP.Offset > -4 || SP != FrameValue::frame(*SP.Offset))
      return false;
    const auto Feature =
        pushedScalarArgument(Image, Block, Op.Addr, false, Work);
    return Feature == 10 || Feature == 23;
  }
  const auto *Push = precedingInstruction(Block, Op.Addr, Work);
  if (!Push)
    return false;
  if (*Call == KernelStartupCall::SetExceptionFilter ||
      *Call == KernelStartupCall::InitializeSList) {
    const auto Pushed = pushedImageAddress(Image, Push, Work);
    if (!Pushed)
      return false;
    const auto Address = *Pushed;
    if (*Call == KernelStartupCall::InitializeSList) {
      // The Win32 SLIST_HEADER occupies one aligned eight-byte cell. Check
      // the whole output, including the last byte, against every import.
      constexpr uint16_t HeaderSize = 8;
      return Image.Base % HeaderSize == 0 && Address % HeaderSize == 0 &&
             registration_abi::hasResidentRegistrationData(
                 Image, FrameValue::imageAddress(Address), true, Work) &&
             registration_abi::hasSEHCallbackImageMemory(
                 Image, Address, HeaderSize, true, Work);
    }
    // Installation records the pointer in Windows. A later unhandled
    // exception has its own dispatch and handler obligations.
    return readImmutablePE32CodeBytes(Image, Address, 1, {}, Work,
                                      limits::kMaxRegistrationEHStateWork)
        .has_value();
  }
  if (Push->Size != 1)
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

namespace {
std::optional<uint32_t> pushedScalarArgument(const BinaryImage &Image,
                                             const LowBlock &Block, va_t Call,
                                             bool AllowGetter, size_t &Work) {
  const auto *Push = precedingInstruction(Block, Call, Work);
  if (!Push || (Push->Size != 1 && Push->Size != 2 && Push->Size != 5))
    return std::nullopt;
  const size_t Limit = limits::kMaxRegistrationEHStateWork;
  const auto Bytes = readImmutablePE32CodeBytes(Image, Push->Address,
                                                Push->Size, {}, Work, Limit);
  if (!Bytes)
    return std::nullopt;
  if (Push->Size == 2 && (*Bytes)[0] == 0x6a)
    return uint32_t(int32_t(int8_t((*Bytes)[1])));
  if (Push->Size == 5 && (*Bytes)[0] == 0x68)
    return readLE<uint32_t>(Bytes->data() + 1);
  if (!AllowGetter || Push->Size != 1 || (*Bytes)[0] != 0x50)
    return std::nullopt;

  // MSVC may obtain a startup mode from a separately linked option object.
  // The same block must call the getter immediately before PUSH EAX.
  const auto *Getter = precedingInstruction(Block, Push->Address, Work);
  if (!Getter || Getter->Size != 5 || !Getter->Immediate ||
      Getter->Control != LowInstructionControl::Call ||
      !hasCheckedX86RegistrationDirectCallBytes(
          Image, Getter->Address, *Getter->Immediate, Work, Limit))
    return std::nullopt;
  const va_t Target = *Getter->Immediate;
  const auto Prefix =
      readImmutablePE32CodeBytes(Image, Target, 3, {}, Work, Limit);
  if (!Prefix)
    return std::nullopt;
  // XOR EAX,EAX; RET or MOV EAX,imm32; RET. No incoming register,
  // file-initial data, relocation or callback contributes to these results.
  if (((*Prefix)[0] == 0x33 || (*Prefix)[0] == 0x31) && (*Prefix)[1] == 0xc0 &&
      (*Prefix)[2] == 0xc3)
    return 0;
  if ((*Prefix)[0] != 0xb8)
    return std::nullopt;
  const auto Body =
      readImmutablePE32CodeBytes(Image, Target, 6, {}, Work, Limit);
  if (!Body || (*Body)[5] != 0xc3)
    return std::nullopt;
  return readLE<uint32_t>(Body->data() + 1);
}
} // namespace

bool hasBindingPreservingCRTArgument(const BinaryImage &Image,
                                     const LowBlock &Block, const LowOp &Op,
                                     va_t Slot, size_t &Work) {
  const auto *Import = Image.findImportAt(Slot);
  if (!Import)
    return false;
  const llvm::StringRef Name = Import->Name;
  const bool Stream = Name == "__acrt_iob_func";
  const bool ExitTable = Name == "_initialize_onexit_table";
  if ((!Stream && !ExitTable && Name != "_set_app_type" &&
       Name != "_set_fmode" && Name != "_set_new_mode") ||
      !registration_abi::checkedRegistrationImportStackPop(Image, Slot) ||
      checkedCallSlot(Image, Op, Work, ExitTable) != Slot)
    return false;
  if (ExitTable) {
    const auto Table = pushedImageAddress(
        Image, precedingInstruction(Block, Op.Addr, Work), Work);
    // The UCRT initializer can write the three PE32 pointer fields. It
    // neither allocates entries nor calls any registered function.
    return Table &&
           registration_abi::hasResidentRegistrationData(
               Image, FrameValue::imageAddress(*Table), true, Work) &&
           registration_abi::hasSEHCallbackImageMemory(
               Image, *Table, 3 * sizeof(uint32_t), true, Work);
  }
  const auto Value = pushedScalarArgument(Image, Block, Op.Addr, !Stream, Work);
  if (!Value)
    return false;
  if (Name == "_set_fmode")
    // _O_TEXT, _O_BINARY, _O_WTEXT. Invalid modes can invoke application code.
    return *Value == 0x4000 || *Value == 0x8000 || *Value == 0x10000;
  if (Name == "_set_new_mode")
    return *Value <= 1;
  // Standard FILE indices and the documented _crt_app_type enumerators.
  return *Value <= 2;
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
