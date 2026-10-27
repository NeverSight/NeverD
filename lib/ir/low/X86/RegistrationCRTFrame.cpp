//===- RegistrationCRTFrame.cpp - Authenticate PE32 CRT invocations -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks PE32 CRT output cells against registration frame ownership.

#include "RegistrationCRTFrame.h"

#include "RegistrationABIPrivate.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"

namespace neverd::registration_state {

bool isCheckedControlFPSlot(const BinaryImage &Image, va_t Slot) {
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF ||
      !isFileBackedPEImportSlot(Image, Slot))
    return false;
  const auto *Imported = Image.findImportAt(Slot);
  if (!Imported || Imported->IsDelayImport || Imported->Name != "_controlfp_s")
    return false;
  const llvm::StringRef Module = Imported->Module;
  return Module.equals_insensitive("api-ms-win-crt-math-l1-1-0.dll") ||
         Module.equals_insensitive("api-ms-win-crt-runtime-l1-1-0.dll") ||
         Module.equals_insensitive("ucrtbase.dll") ||
         Module.equals_insensitive("ucrtbased.dll") ||
         Module.equals_insensitive("msvcrt.dll") ||
         Module.equals_insensitive("msvcr100.dll");
}

bool isCheckedControlFPCall(const BinaryImage &Image, const LowOp &Op,
                            size_t &Work) {
  const bool Indirect = Op.Opcode == NdOp::INDIR_CALL;
  const unsigned Size = Indirect ? 6 : 5;
  if ((!Indirect && Op.Opcode != NdOp::CALL) || Op.NumInputs != 1 ||
      Op.Seq < 0 || !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 4 ||
      Op.Inputs[0].Offset > UINT32_MAX ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.Addr > UINT32_MAX - Size ||
      !registration_abi::chargeCalleeWork(Work, Indirect ? 10 : 15))
    return false;
  const va_t Target = Op.Inputs[0].Offset;
  if (Indirect) {
    const auto Bytes =
        readImmutablePE32CodeBytes(Image, Op.Addr, 6, {Op.Addr + 2});
    return Bytes && (*Bytes)[0] == 0xff && (*Bytes)[1] == 0x15 &&
           readLE<uint32_t>(Bytes->data() + 2) == Target &&
           isCheckedControlFPSlot(Image, Target);
  }
  // A direct CRT veneer is the exact E8 occurrence followed by one immutable
  // FF25 import jump. Cached stub names do not authenticate either instruction.
  if (Target > UINT32_MAX - 6)
    return false;
  const auto Bytes = readImmutablePE32CodeBytes(Image, Op.Addr, 5, {});
  const auto Stub = readImmutablePE32CodeBytes(Image, Target, 6, {Target + 2});
  return Bytes && (*Bytes)[0] == 0xe8 &&
         uint32_t(Op.Addr + 5 + int64_t(readLE<int32_t>(Bytes->data() + 1))) ==
             Target &&
         Stub && (*Stub)[0] == 0xff && (*Stub)[1] == 0x25 &&
         isCheckedControlFPSlot(Image, readLE<uint32_t>(Stub->data() + 2));
}

} // namespace neverd::registration_state
