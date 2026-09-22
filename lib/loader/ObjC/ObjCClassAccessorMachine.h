#ifndef NEVERD_LOADER_OBJC_OBJCCLASSACCESSORMACHINE_H
#define NEVERD_LOADER_OBJC_OBJCCLASSACCESSORMACHINE_H

#include "../SourceUnwind.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/DarwinImportVeneer.h"
#include "neverd/loader/ObjC/ObjCCallHints.h"
#include "neverd/loader/ReadOnlyBytes.h"

#include "llvm/Support/Endian.h"

namespace neverd {
/// Complete machine input/result facts, not permission to rebuild the class
/// pointer or publish a source body. Consumers must independently validate the
/// class identity, current pipeline and dependency closure when needed.
struct ObjCClassAccessorMachine {
  va_t Entry = 0;
  va_t ClassAddress = 0;
  va_t SelfTarget = 0;
  SourceCallTypeHint SelfCall;
  SourceFunctionTypeHint Signature;
};

inline std::optional<ObjCClassAccessorMachine>
objcClassAccessorMachine(const BinaryImage &Image, va_t Entry) {
  if (Image.Format != BinaryFormat::MachO || Image.Arch != Arch::AArch64 ||
      Image.Bits != Bitness::Bits64 || Image.IsRelocatable || Entry % 4 ||
      Entry > UINT64_MAX - 32)
    return std::nullopt;
  const auto Bytes = readImmutableCodeBytes(Image, Entry, 32);
  if (!Bytes)
    return std::nullopt;
  auto Word = [&](unsigned I) {
    return llvm::support::endian::read32le(Bytes->data() + 4 * I);
  };
  if (Word(0) != 0xa9bf7bfd || Word(1) != 0x910003fd || Word(5) != 0xd2800001 ||
      Word(6) != 0xa8c17bfd || Word(7) != 0xd65f03c0 ||
      (Word(2) & 0x9f00001f) != 0x90000000 ||
      (Word(3) & 0xffc003ff) != 0x91000000 ||
      (Word(4) & 0xfc000000) != 0x94000000)
    return std::nullopt;
  for (const auto &Metadata : Image.ExceptionMetadata.Functions)
    if (((Metadata.CodeRange.Begin < Entry + 32 &&
          Metadata.CodeRange.End > Entry) ||
         (Metadata.CodeRange.Begin >= Entry &&
          Metadata.CodeRange.Begin < Entry + 32)) &&
        (Metadata.CodeRange.End <= Metadata.CodeRange.Begin ||
         !isPlainSourceUnwind(Metadata)))
      return std::nullopt;
  auto Add = [](va_t Base, int64_t Offset) -> std::optional<va_t> {
    if ((Offset < 0 && Base < uint64_t(-Offset)) ||
        (Offset >= 0 && Base > UINT64_MAX - uint64_t(Offset)))
      return std::nullopt;
    return Base + Offset;
  };
  const uint32_t PageImmediate =
      ((Word(2) >> 29) & 3) | (((Word(2) >> 5) & 0x7ffff) << 2);
  const auto Page = Add(
      (Entry + 8) & ~va_t(4095),
      (int64_t(PageImmediate) - ((PageImmediate & 0x100000) ? 0x200000 : 0)) *
          4096);
  const auto Class = Page ? Add(*Page, (Word(3) >> 10) & 4095) : std::nullopt;
  const uint32_t BranchImmediate = Word(4) & 0x03ffffff;
  const auto Target =
      Add(Entry + 16, (int64_t(BranchImmediate) -
                       ((BranchImmediate & 0x02000000) ? 0x04000000 : 0)) *
                          4);
  const auto Slot =
      Target ? darwinImportVeneerSlot(Image, *Target) : std::nullopt;
  const auto Call =
      Slot ? objcRuntimeSourceCallHint(Image, *Slot) : std::nullopt;
  const auto Bind =
      Slot ? Image.DyldBindSlots.find(*Slot) : Image.DyldBindSlots.end();
  if (!Class || !Target || !Call || Call->TargetName != "objc_opt_self" ||
      Call->CallKind != SourceCallTypeHint::Kind::ObjCRuntimeCall ||
      Bind == Image.DyldBindSlots.end() ||
      Bind->second.Module != "/usr/lib/libobjc.A.dylib" || Call->WeakImport ||
      Call->DoesNotReturn || Call->Signature.Parameters.size() != 1 ||
      !Call->Signature.ReturnType ||
      Call->Signature.ReturnType->Kind != NdTypeKind::Ptr ||
      Call->Signature.ReturnType->Size != 8)
    return std::nullopt;
  // ADRP/ADD overwrite the sole runtime argument before any effect. The exact
  // epilogue changes only x1, FP, LR and SP, leaving all eight x0 result bytes
  // from the independently declared runtime call intact.
  SourceFunctionTypeHint Signature;
  Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Signature.ReturnType = Call->Signature.ReturnType;
  std::string Error;
  if (!assignDarwinScalarSourceABI(Signature, Image.Arch, Error))
    return std::nullopt;
  return ObjCClassAccessorMachine{Entry, *Class, *Target, *Call, Signature};
}
} // namespace neverd
#endif
