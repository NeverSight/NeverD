//===- Win64CallStackInput.cpp - Original outgoing scalar slots -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate the fixed slot and original rel32 CALL before attributing
/// an otherwise unresolved scalar to physical call-time memory.
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/X86/Win64CallStackInput.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

#include <limits>

namespace neverd {
bool Win64CallStackInput::validOffset(uint64_t Offset) {
  const auto Layout =
      getTargetRegInfo(Arch::X64).integerArgumentLayout(BinaryFormat::COFF);
  return Offset >= uint64_t(Layout.CallStackBase) && Offset <= UINT32_MAX &&
         (Offset - Layout.CallStackBase) % Layout.SlotBytes == 0;
}

void Win64CallStackInput::bind(MedVar &Value, const BinaryImage *Image,
                               va_t Site, va_t Callee, int64_t Offset,
                               int &LastVersion) {
  if (LastVersion == std::numeric_limits<int>::max() ||
      Value.Kind != MedVar::Unspecified || Value.TheArch != Arch::X64 ||
      Value.Size != sizeof(uint64_t) || !Image || Image->Arch != Arch::X64 ||
      Image->Format != BinaryFormat::COFF || Image->IsRelocatable ||
      Site > InvalidVA - 5 || !validOffset(Offset) ||
      !Image->isCodeAddress(Site) || !Image->isCodeAddress(Site + 4) ||
      !Image->isCodeAddress(Callee))
    return;
  const auto *Bytes = Image->readVA(Site, 5);
  if (!Bytes || Bytes[0] != 0xe8 ||
      Site + 5 + int32_t(readLE<uint32_t>(Bytes + 1)) != Callee)
    return;
  Value.SSAVer = ++LastVersion;
  Value.AddressOwnerVA = Site;
  Value.StackOff = Offset;
}
} // namespace neverd
