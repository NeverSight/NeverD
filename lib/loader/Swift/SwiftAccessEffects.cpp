#include "neverd/loader/Swift/SwiftAccessEffects.h"

#include "../MachO/SourceLocalCall.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/DarwinImportVeneer.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/loader/Swift/SwiftRuntimeCalls.h"

namespace neverd {
namespace {
std::optional<SourceCallTypeHint> accessImport(const BinaryImage &Image,
                                               va_t Target) {
  if (Image.Format != BinaryFormat::MachO || Image.Arch != Arch::AArch64 ||
      Image.Bits != Bitness::Bits64 || Image.IsRelocatable)
    return std::nullopt;
  const auto Slot = darwinImportVeneerSlot(Image, Target);
  if (!Slot || !isImmutableImageImportSlot(Image, *Slot))
    return std::nullopt;
  const auto Import = Image.DyldBindSlots.find(*Slot);
  const auto Runtime = swiftRuntimeSourceCallHint(Image, *Slot);
  if (Import == Image.DyldBindSlots.end() ||
      Import->second.Module != "/usr/lib/swift/libswiftCore.dylib" ||
      !Runtime || Runtime->WeakImport || Runtime->DoesNotReturn ||
      Runtime->CallKind != SourceCallTypeHint::Kind::SwiftRuntimeCall ||
      Runtime->TargetAddress != *Slot ||
      (Runtime->TargetName != "swift_beginAccess" &&
       Runtime->TargetName != "swift_endAccess"))
    return std::nullopt;
  return Runtime;
}
} // namespace

bool isSwiftAccessCallTarget(const BinaryImage &Image, va_t Target) {
  const auto Slot = darwinImportVeneerSlot(Image, Target);
  if (!Slot)
    return false;
  const auto Import = Image.DyldBindSlots.find(*Slot);
  return Import != Image.DyldBindSlots.end() &&
         (Import->second.Name == "_swift_beginAccess" ||
          Import->second.Name == "_swift_endAccess");
}

std::optional<SourceFrameEffects>
swiftAccessCallEffects(const BinaryImage &Image, const LowFunc &Caller,
                       const SourceCallOccurrenceKey &Site,
                       const SourceCallTypeHint &Binding) {
  if (!Site.StaticTarget || !sourceLocalCalls(Image, Caller).count(Site))
    return std::nullopt;
  const auto Current = accessImport(Image, *Site.StaticTarget);
  if (!Current || Binding.CallKind != Current->CallKind ||
      Binding.TargetName != Current->TargetName ||
      Binding.TargetAddress != Current->TargetAddress || Binding.WeakImport ||
      Binding.DoesNotReturn || Binding.ImmutableNativeCall ||
      !equalSourceABIs(Binding.Signature, Current->Signature))
    return std::nullopt;
  // The runtime ABI owns ValueBuffer's opaque three-word capacity. Read and
  // Modify without Tracking (0x20) do not retain its address. Do not infer
  // nonescaping behavior merely from this import's name or scratch extent.
  // Swift 6.1.2: ABI/MetadataValues.h and runtime/Exclusivity.cpp.
  SourceFrameEffects Effects;
  const bool Begin = Current->TargetName == "swift_beginAccess";
  const size_t Parameter = Begin ? 1 : 0;
  if (Begin)
    Effects.WritableFrameParameters.emplace(Parameter, 24);
  else
    Effects.ReadOnlyFrameParameters.emplace(Parameter, 24);
  Effects.Scratch = SourceFrameScratchEffect{
      SourceFrameScratchEffect::Domain::SwiftUntrackedAccess,
      Begin ? SourceFrameScratchEffect::Action::Initialize
            : SourceFrameScratchEffect::Action::Finish,
      Parameter, 24, std::nullopt};
  if (Begin)
    Effects.Scratch->Condition = SourceFrameScalarCondition{2, {0, 1}};
  return sourceFrameEffectsMatchABI(Effects, Binding.Signature)
             ? std::optional(Effects)
             : std::nullopt;
}
} // namespace neverd
