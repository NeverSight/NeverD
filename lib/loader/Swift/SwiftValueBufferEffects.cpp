#include "neverd/loader/Swift/SwiftValueBufferEffects.h"

#include "../MachO/SourceLocalCall.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/DarwinImportVeneer.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/loader/Swift/SwiftRuntimeCalls.h"
#include "neverd/support/BranchEncoding.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <array>

namespace neverd {
std::optional<SourceFrameEffects>
swiftValueBufferProjectionEffects(const BinaryImage &Image, va_t Entry,
                                  const SourceFunctionTypeHint &Signature) {
  if (Image.Format != BinaryFormat::MachO || Image.Arch != Arch::AArch64 ||
      Image.Bits != Bitness::Bits64 || Image.IsRelocatable || !Entry ||
      Entry % 4 || Entry > InvalidVA - 40 || !Signature.HasExplicitABI ||
      Signature.Architecture != Image.Arch ||
      Signature.Origin != SourceFunctionTypeHint::OriginKind::NativeAnalysis ||
      Signature.Convention != SourceFunctionTypeHint::ConventionKind::C ||
      Signature.Parameters.size() != 2 || !Signature.ReturnComponents.empty() ||
      Signature.ReturnLocation.RegisterOffset != 0)
    return std::nullopt;
  SourceFrameEffects Effects;
  // Conservatively invalidate all three buffer words. Runtime allocation,
  // value-witness copying and release remain observable external effects.
  Effects.WritableFrameParameters.emplace(0, 24);
  Effects.ReturnFrameOrExternal = SourceFrameReturnAlias{0, 24};
  if (!sourceFrameEffectsMatchABI(Effects, Signature))
    return std::nullopt;
  for (size_t I = 0; I < 2; ++I) {
    const auto &Parameter = Signature.Parameters[I];
    if (!Parameter.Type || Parameter.Type->Kind != NdTypeKind::Ptr ||
        Parameter.Type->Size != 8 || !Parameter.Components.empty() ||
        Parameter.TheRole != SourceParameterTypeHint::Role::Ordinary ||
        Parameter.Location.Kind != SourceABICarrierKind::IntegerRegister ||
        Parameter.Location.RegisterOffset != I * 8 ||
        Parameter.Location.ValueBytes != 8)
      return std::nullopt;
  }
  const auto Bytes = readImmutableCodeBytes(Image, Entry, 40);
  if (!Bytes)
    return std::nullopt;
  // Both paths are complete. Inline storage returns x0 unchanged; the boxed
  // path passes the original buffer and metadata, masks the VWT alignment,
  // and returns x1 from the declared two-pointer runtime result.
  constexpr std::array<uint32_t, 10> Words = {
      0xf85f8028, 0xb9405108, 0x368800e8, 0xa9bf7bfd, 0x910003fd,
      0x92401d02, 0,          0xaa0103e0, 0xa8c17bfd, 0xd65f03c0};
  for (size_t I = 0; I < Words.size(); ++I)
    if (I != 6 &&
        llvm::support::endian::read32le(Bytes->data() + I * 4) != Words[I])
      return std::nullopt;
  if (!sourceLeafCodeRange(Image, Entry, 40) ||
      std::none_of(
          Image.Symbols.begin(), Image.Symbols.end(),
          [&](const Symbol &S) { return S.IsFunc && S.Addr == Entry; }))
    return std::nullopt;
  const auto Word = llvm::support::endian::read32le(Bytes->data() + 24);
  if (!branch::A64BranchLink.matches(Word))
    return std::nullopt;
  const auto Target = branch::a64BranchTarget(Word, Entry + 24);
  const auto Slot =
      Target ? darwinImportVeneerSlot(Image, *Target) : std::nullopt;
  if (!Slot)
    return std::nullopt;
  const auto Import = Image.DyldBindSlots.find(*Slot);
  const auto Runtime = swiftRuntimeSourceCallHint(Image, *Slot);
  if (Import == Image.DyldBindSlots.end() ||
      Import->second.Module != "/usr/lib/swift/libswiftCore.dylib" ||
      !Runtime || Runtime->WeakImport || Runtime->DoesNotReturn ||
      Runtime->CallKind != SourceCallTypeHint::Kind::SwiftRuntimeCall ||
      Runtime->TargetAddress != *Slot ||
      Runtime->TargetName != "swift_makeBoxUnique")
    return std::nullopt;
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  SourceFunctionTypeHint Expected;
  Expected.Origin = SourceFunctionTypeHint::OriginKind::SwiftRuntime;
  Expected.ReturnType = NdType::makeStruct({Pointer, Pointer});
  Expected.Parameters = {{"arg0", Pointer},
                         {"arg1", Pointer},
                         {"arg2", NdType::makeInt(8, false)}};
  std::string Error;
  if (!assignDarwinSwiftSourceABI(Expected, Image.Arch, Error) ||
      !equalSourceABIs(Expected, Runtime->Signature))
    return std::nullopt;
  return Effects;
}

bool isSwiftValueBufferProjection(const BinaryImage &Image, va_t Entry) {
  // A cheap negative filter avoids running immutable-range and ABI proofs
  // for every unrelated call. These raw bytes never confer authority.
  if (Image.Arch != Arch::AArch64 || Entry > InvalidVA - 12)
    return false;
  const auto *Prefix = Image.readVA(Entry, 12);
  if (!Prefix || llvm::support::endian::read32le(Prefix) != 0xf85f8028 ||
      llvm::support::endian::read32le(Prefix + 4) != 0xb9405108 ||
      llvm::support::endian::read32le(Prefix + 8) != 0x368800e8)
    return false;
  SourceFunctionTypeHint Physical;
  Physical.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Physical.ReturnType = NdType::makeInt(8);
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  Physical.Parameters = {{"buffer", Pointer}, {"metadata", Pointer}};
  std::string Error;
  return assignDarwinScalarSourceABI(Physical, Image.Arch, Error) &&
         bool(swiftValueBufferProjectionEffects(Image, Entry, Physical));
}

std::optional<SourceFrameEffects>
swiftValueBufferCallEffects(const BinaryImage &Image, const LowFunc &Caller,
                            const SourceCallOccurrenceKey &Site,
                            const SourceFunctionTypeHint &Signature) {
  if (!Site.StaticTarget)
    return std::nullopt;
  auto Effects =
      swiftValueBufferProjectionEffects(Image, *Site.StaticTarget, Signature);
  return Effects && sourceLocalCalls(Image, Caller).count(Site) ? Effects
                                                                : std::nullopt;
}
} // namespace neverd
