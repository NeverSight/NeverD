#ifndef NEVERD_IR_LOW_SOURCEFRAMEEFFECTS_H
#define NEVERD_IR_LOW_SOURCEFRAMEEFFECTS_H

#include "neverd/ir/SourceABI.h"

#include <map>
#include <optional>
#include <set>

namespace neverd {

/// A call result may be the unchanged borrowed argument or external storage.
/// This is possible frame provenance, never an identity of frame contents.
struct SourceFrameReturnAlias {
  size_t Parameter = 0;
  size_t Bytes = 0;
  bool operator==(const SourceFrameReturnAlias &) const = default;
};

struct SourceFrameScalarCondition {
  size_t Parameter = 0;
  std::set<uint64_t> Values;
  bool operator==(const SourceFrameScalarCondition &) const = default;
};

/// Opaque scratch state, not a claim about its physical bytes. A finishing
/// borrow requires the same live, unmodified record on every reaching path.
struct SourceFrameScratchEffect {
  enum class Domain { SwiftUntrackedAccess } TheDomain;
  enum class Action { Initialize, Finish } TheAction;
  size_t Parameter = 0;
  size_t Bytes = 0;
  std::optional<SourceFrameScalarCondition> Condition;
  bool operator==(const SourceFrameScratchEffect &) const = default;
};

/// Synchronous, nonescaping borrows, independently authenticated by a caller.
/// These bounds describe only effects on its private frame. The call may still
/// allocate, release objects, invoke callbacks, or mutate external memory.
struct SourceFrameEffects {
  std::map<size_t, size_t> ReadOnlyFrameParameters;
  std::map<size_t, size_t> WritableFrameParameters;
  std::optional<SourceFrameReturnAlias> ReturnFrameOrExternal;
  std::optional<SourceFrameScratchEffect> Scratch;

  bool empty() const {
    return ReadOnlyFrameParameters.empty() && WritableFrameParameters.empty() &&
           !ReturnFrameOrExternal && !Scratch;
  }
  bool operator==(const SourceFrameEffects &) const = default;
};

/// Validate effect carriers, not the provenance of the effect certificate.
/// Keep logical parameter indexes distinct from physical record components.
inline bool
sourceFrameEffectsMatchABI(const SourceFrameEffects &Effects,
                           const SourceFunctionTypeHint &Signature) {
  std::string Error;
  if (!validateSourceABI(Signature, Error))
    return false;
  const auto ScalarPointerCarrier = [](const TypeRef &Type,
                                       const SourceABIValueLocation &Location) {
    return Type && Type->Size == 8 &&
           (Type->Kind == NdTypeKind::Ptr || Type->Kind == NdTypeKind::Int) &&
           Location.Kind == SourceABICarrierKind::IntegerRegister &&
           Location.ValueBytes == 8;
  };
  const auto Parameter = [&](size_t Index, size_t Bytes) {
    return Bytes && Bytes <= (1U << 20) &&
           Index < Signature.Parameters.size() &&
           Signature.Parameters[Index].Components.empty() &&
           ScalarPointerCarrier(Signature.Parameters[Index].Type,
                                Signature.Parameters[Index].Location);
  };
  for (const auto &[Index, Bytes] : Effects.ReadOnlyFrameParameters)
    if (!Parameter(Index, Bytes) ||
        Effects.WritableFrameParameters.count(Index))
      return false;
  for (const auto &[Index, Bytes] : Effects.WritableFrameParameters)
    if (!Parameter(Index, Bytes))
      return false;
  if (const auto &Scratch = Effects.Scratch) {
    using Action = SourceFrameScratchEffect::Action;
    if (Scratch->TheDomain !=
            SourceFrameScratchEffect::Domain::SwiftUntrackedAccess ||
        (Scratch->TheAction != Action::Initialize &&
         Scratch->TheAction != Action::Finish) ||
        !Parameter(Scratch->Parameter, Scratch->Bytes) ||
        Effects.ReturnFrameOrExternal)
      return false;
    const auto &Borrows = Scratch->TheAction == Action::Initialize
                              ? Effects.WritableFrameParameters
                              : Effects.ReadOnlyFrameParameters;
    const auto Borrow = Borrows.find(Scratch->Parameter);
    if (Borrow == Borrows.end() || Borrow->second != Scratch->Bytes)
      return false;
    if (const auto &Condition = Scratch->Condition) {
      if (Scratch->TheAction != Action::Initialize ||
          Condition->Parameter == Scratch->Parameter ||
          !Parameter(Condition->Parameter, 8) || Condition->Values.empty() ||
          Condition->Values.size() > 16 ||
          Signature.Parameters[Condition->Parameter].Type->Kind !=
              NdTypeKind::Int)
        return false;
    }
  }
  if (const auto &Alias = Effects.ReturnFrameOrExternal) {
    const auto ReadOnly =
        Effects.ReadOnlyFrameParameters.find(Alias->Parameter);
    const auto Writable =
        Effects.WritableFrameParameters.find(Alias->Parameter);
    if (!Parameter(Alias->Parameter, Alias->Bytes) ||
        !Signature.ReturnComponents.empty() ||
        !ScalarPointerCarrier(Signature.ReturnType, Signature.ReturnLocation) ||
        (ReadOnly == Effects.ReadOnlyFrameParameters.end() &&
         Writable == Effects.WritableFrameParameters.end()) ||
        (ReadOnly != Effects.ReadOnlyFrameParameters.end() &&
         Alias->Bytes > ReadOnly->second) ||
        (Writable != Effects.WritableFrameParameters.end() &&
         Alias->Bytes > Writable->second))
      return false;
  }
  return true;
}

} // namespace neverd
#endif
