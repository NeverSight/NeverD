//===- RegistrationJumpRuntime.cpp - Checked PE32 jump runtime binding ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Match the original runtime import and physical veneer for nonlocal jumps.

#include "RegistrationJumpRuntime.h"

#include "RegistrationSourceCall.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/RuntimeProviders.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSwitch.h"

namespace neverd::registration_state {

std::optional<JumpRuntimeBinding>
checkedJumpRuntime(const BinaryImage &Image, va_t Target, bool Indirect) {
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF || Target > UINT32_MAX)
    return std::nullopt;
  va_t Slot = Target;
  if (!Indirect) {
    if (Target > UINT32_MAX - 6)
      return std::nullopt;
    // Ordinary callees are not import veneers. A raw prefix can reject them
    // before scanning fixups; a match still needs the complete storage proof.
    const auto *Prefix = Image.readVA(Target, 2);
    if (!Prefix || Prefix[0] != 0xff || Prefix[1] != 0x25)
      return std::nullopt;
    const auto Bytes =
        readImmutablePE32CodeBytes(Image, Target, 6, {Target + 2});
    if (!Bytes || (*Bytes)[0] != 0xff || (*Bytes)[1] != 0x25)
      return std::nullopt;
    Slot = readLE<uint32_t>(Bytes->data() + 2);
  }
  const auto *Imported = Image.findImportAt(Slot);
  if (!Imported || Imported->IATAddr != Slot || Imported->IsDelayImport)
    return std::nullopt;
  if (!coff_loader::isX86RegistrationRuntimeProvider(Imported->Module))
    return std::nullopt;
  using Kind = JumpRuntimeBinding::Kind;
  const auto Routine = llvm::StringSwitch<std::optional<Kind>>(Imported->Name)
#define NEVERD_REGISTRATION_JUMP(Name, Routine) .Case(Name, Kind::Routine)
#include "RegistrationRuntimeImports.def"
                           .Default(std::nullopt);
  if (!Routine)
    return std::nullopt;
  if (!isFileBackedPEImportSlot(Image, Slot))
    return std::nullopt;
  JumpRuntimeBinding Binding;
  Binding.Routine = *Routine;
  Binding.Target = Target;
  Binding.ImportSlot = Slot;
  Binding.Indirect = Indirect;
  return Binding;
}

std::optional<JumpRuntimeBinding>
checkedJumpRuntimeCall(const BinaryImage &Image, const LowFunc &Function,
                       const LowOp &Op, size_t &Work) {
  if (Op.Opcode != NdOp::CALL || Op.NumInputs != 1 || !Op.Inputs[0].isConst() ||
      Op.Inputs[0].Size != 4)
    return std::nullopt;
  const auto Binding = checkedJumpRuntime(Image, Op.Inputs[0].Offset, false);
  if (!Binding || !checkedRegistrationDirectCall(Image, Function, Op, Work))
    return std::nullopt;
  return Binding;
}

bool isSEHJumpRuntimeInstruction(const BinaryImage &Image, va_t Address,
                                 uint16_t Size, llvm::ArrayRef<LowOp> Ops) {
  if (Size != 5 || Address > UINT32_MAX - Size)
    return false;
  const LowOp *Call = nullptr;
  for (const auto &Op : Ops)
    if (Op.Opcode == NdOp::CALL) {
      if (Call || Op.Addr != Address || Op.NumInputs != 1 ||
          !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 4)
        return false;
      Call = &Op;
    }
  if (!Call)
    return false;
  const auto Binding = checkedJumpRuntime(Image, Call->Inputs[0].Offset, false);
  if (!Binding || (Binding->Routine != JumpRuntimeBinding::Kind::Capture &&
                   Binding->Routine != JumpRuntimeBinding::Kind::Resume))
    return false;
  const auto Bytes = readImmutableCodeBytes(Image, Address, Size);
  return Bytes && (*Bytes)[0] == 0xe8 &&
         uint32_t(Address + Size +
                  int64_t(readLE<int32_t>(Bytes->data() + 1))) ==
             Binding->Target;
}

} // namespace neverd::registration_state
