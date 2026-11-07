#ifndef NEVERD_IR_LOW_SOURCEFRAMEEFFECTS_H
#define NEVERD_IR_LOW_SOURCEFRAMEEFFECTS_H

#include "neverd/ir/Limits.h"
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

/// An independently authenticated operation on one opaque value. Identity is
/// an uninterpreted token shared by its effect owner, not a type-name parser or
/// a layout claim. Initialization establishes a live value, not initialized
/// padding or scalar byte identities; Read and Destroy require that same value.
struct SourceFrameValueEffect {
  enum class Action {
    None,
    Initialize,
    Read,
    Destroy
  } TheAction = Action::None;
  std::string Identity;
  size_t Bytes = 0;
  bool operator==(const SourceFrameValueEffect &) const = default;
};

struct SourceFrameScalarCondition {
  size_t Parameter = 0;
  std::set<uint64_t> Values;
  bool operator==(const SourceFrameScalarCondition &) const = default;
};

/// Opaque scratch state, not a claim about its physical bytes. A finishing
/// borrow requires the same live record on every reaching path. Runtime-owned
/// links may change other retained records; their bytes are never identities.
struct SourceFrameScratchEffect {
  enum class Domain { SwiftAccess } TheDomain;
  enum class Action { Initialize, Finish } TheAction;
  size_t Parameter = 0;
  size_t Bytes = 0;
  std::optional<SourceFrameScalarCondition> Condition;
  // A subset of Condition.Values retains the scratch address until Finish.
  // This is an explicit lifetime obligation, not a synchronous noescape grant.
  std::set<uint64_t> RetainedValues;
  bool operator==(const SourceFrameScratchEffect &) const = default;
};

/// Bounded frame effects, independently authenticated by a caller. Borrows are
/// synchronous and nonescaping unless Scratch explicitly retains its record.
/// These bounds describe only effects on its private frame. The call may still
/// allocate, release objects, invoke callbacks, or mutate external memory.
struct SourceFrameEffects {
  std::map<size_t, size_t> ReadOnlyFrameParameters;
  std::map<size_t, size_t> WritableFrameParameters;
  // These scalar-pointer borrows must read completely initialized bytes.
  // Their existing bounds and write permissions still own the effect.
  std::set<size_t> InitializedFrameParameters;
  // Independently proved complete pointer-free writes through an ordinary
  // scalar pointer, on every normal return. The writable bound owns its
  // extent. This establishes no logical record or byte-value identity.
  std::set<size_t> InitializesFrameParameters;
  // Typed opaque value lifetimes are distinct from initialized raw bytes.
  std::map<size_t, SourceFrameValueEffect> OpaqueValueParameters;
  // Logical indirect records consume an initialized private by-value copy.
  // The callee may modify it: subsequent reads need a new definite write.
  // This certificate never converts an arbitrary source pointer to a record.
  std::set<size_t> ByValueFrameParameters;
  // An independently authenticated producer completely initializes the
  // logical indirect result, with no private-frame pointers in its contents.
  // A return ABI alone does not establish this effect.
  bool InitializesIndirectResult = false;
  std::optional<SourceFrameReturnAlias> ReturnFrameOrExternal;
  std::optional<SourceFrameScratchEffect> Scratch;

  bool empty() const {
    return ReadOnlyFrameParameters.empty() && WritableFrameParameters.empty() &&
           InitializedFrameParameters.empty() &&
           InitializesFrameParameters.empty() &&
           OpaqueValueParameters.empty() && ByValueFrameParameters.empty() &&
           !InitializesIndirectResult && !ReturnFrameOrExternal && !Scratch;
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
           Signature.Parameters[Index].TheRole !=
               SourceParameterTypeHint::Role::SwiftErrorResult &&
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
  for (size_t Index : Effects.InitializedFrameParameters)
    if (!Effects.ReadOnlyFrameParameters.count(Index) &&
        !Effects.WritableFrameParameters.count(Index))
      return false;
  for (size_t Index : Effects.InitializesFrameParameters)
    if (!Effects.WritableFrameParameters.count(Index) ||
        Signature.Parameters[Index].Type->Kind != NdTypeKind::Ptr ||
        Signature.Parameters[Index].TheRole !=
            SourceParameterTypeHint::Role::Ordinary ||
        Signature.Parameters[Index].IndirectByValue ||
        Effects.OpaqueValueParameters.count(Index) || Effects.Scratch ||
        Effects.ReturnFrameOrExternal)
      return false;
  for (const auto &[Index, Value] : Effects.OpaqueValueParameters) {
    using Action = SourceFrameValueEffect::Action;
    if (Value.Identity.empty() ||
        Value.Identity.size() >
            ::neverd::limits::ir::
                kSourceFrameEffectsMatchABIValueIdentitySizeLimit ||
        !Parameter(Index, Value.Bytes) || Effects.Scratch ||
        Effects.InitializedFrameParameters.count(Index) ||
        (Value.TheAction != Action::Initialize &&
         Value.TheAction != Action::Read && Value.TheAction != Action::Destroy))
      return false;
    const auto &Borrows = Value.TheAction == Action::Read
                              ? Effects.ReadOnlyFrameParameters
                              : Effects.WritableFrameParameters;
    const auto Borrow = Borrows.find(Index);
    if (Borrow == Borrows.end() || Borrow->second != Value.Bytes)
      return false;
  }
  for (size_t Index : Effects.ByValueFrameParameters)
    if (Index >= Signature.Parameters.size() ||
        !Signature.Parameters[Index].IndirectByValue ||
        (Signature.Parameters[Index].Location.Kind !=
             SourceABICarrierKind::IntegerRegister &&
         Signature.Parameters[Index].Location.Kind !=
             SourceABICarrierKind::Stack) ||
        Effects.ReadOnlyFrameParameters.count(Index) ||
        Effects.WritableFrameParameters.count(Index))
      return false;
  if (Effects.InitializesIndirectResult &&
      (Signature.Architecture != Arch::AArch64 ||
       Signature.Convention != SourceFunctionTypeHint::ConventionKind::C ||
       Signature.ReturnLocation.Kind !=
           SourceABICarrierKind::IndirectResultPointer ||
       Effects.Scratch || Effects.ReturnFrameOrExternal))
    return false;
  if (const auto &Scratch = Effects.Scratch) {
    using Action = SourceFrameScratchEffect::Action;
    if (Scratch->TheDomain != SourceFrameScratchEffect::Domain::SwiftAccess ||
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
    if (!Scratch->RetainedValues.empty()) {
      if (Scratch->TheAction != Action::Initialize || !Scratch->Condition ||
          Scratch->RetainedValues.size() > 16)
        return false;
      for (uint64_t Value : Scratch->RetainedValues)
        if (!Scratch->Condition->Values.count(Value))
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
