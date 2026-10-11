//===- WindowsEHSemanticDigest.cpp - Source WinEH identity --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/WindowsEHSemanticDigest.h"

#include "neverd/loader/ExceptionInfo.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/SHA256.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace neverd::windows_eh_semantics {
namespace {

constexpr llvm::StringLiteral GraphDomain("windows-eh.semantic-graph");
constexpr llvm::StringLiteral TokenDomain("windows-eh.semantic-token");
constexpr llvm::StringLiteral GSFH4Domain("windows-eh.gs-fh4");
constexpr uint8_t SEHGraphKind = 1;
constexpr uint8_t FH3GraphKind = 2;
constexpr uint8_t FH4GraphKind = 3;
// Kind 4 omitted store widths. A new domain rejects those old receipts without
// changing the existing table-driven SEH and C++ semantic identities.
constexpr uint8_t RegistrationSEHGraphKind = 5;
constexpr uint8_t RegistrationCxxGraphKind = 6;
static_assert(GraphDomain.size() <= std::numeric_limits<uint32_t>::max());
static_assert(TokenDomain.size() <= std::numeric_limits<uint32_t>::max());

class CanonicalBytes {
public:
  void appendU8(uint8_t Value) { Bytes.push_back(Value); }

  void appendU32(uint32_t Value) {
    for (unsigned I = 0; I != 4; ++I)
      appendU8(static_cast<uint8_t>(Value >> (I * 8)));
  }

  void appendI32(int32_t Value) { appendU32(static_cast<uint32_t>(Value)); }

  void appendU64(uint64_t Value) {
    for (unsigned I = 0; I != 8; ++I)
      appendU8(static_cast<uint8_t>(Value >> (I * 8)));
  }

  void appendString(llvm::StringRef Value) {
    appendU32(static_cast<uint32_t>(Value.size()));
    Bytes.insert(Bytes.end(), Value.bytes_begin(), Value.bytes_end());
  }

  void appendDigest(const std::array<uint8_t, 32> &Digest) {
    Bytes.insert(Bytes.end(), Digest.begin(), Digest.end());
  }

  llvm::ArrayRef<uint8_t> bytes() const { return Bytes; }

private:
  std::vector<uint8_t> Bytes;
};

bool appendCount(CanonicalBytes &Bytes, size_t Count) {
  if (Count > std::numeric_limits<uint32_t>::max())
    return false;
  Bytes.appendU32(static_cast<uint32_t>(Count));
  return true;
}

std::optional<uint8_t> getStableArch(Arch TargetArch) {
  switch (TargetArch) {
  case Arch::X86:
    return 1;
  case Arch::X64:
    return 2;
  case Arch::ARM:
    return 3;
  case Arch::AArch64:
    return 4;
  case Arch::EVM:
  case Arch::SBF:
  case Arch::Unknown:
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<uint8_t> getStableSEHScopeKind(SEHScopeKind Kind) {
  switch (Kind) {
  case SEHScopeKind::Filter:
    return 1;
  case SEHScopeKind::CatchAll:
    return 2;
  case SEHScopeKind::Finally:
    return 3;
  }
  return std::nullopt;
}

std::optional<uint8_t> getStableCxxVersion(CxxFuncInfoVersion Version) {
  switch (Version) {
  case CxxFuncInfoVersion::Original:
    return 1;
  case CxxFuncInfoVersion::WithExceptionSpecs:
    return 2;
  case CxxFuncInfoVersion::WithEHFlags:
    return 3;
  }
  return std::nullopt;
}

std::optional<uint8_t>
getStableCxxActionKind(CxxUnwindAction::ActionKind Kind) {
  switch (Kind) {
  case CxxUnwindAction::ActionKind::None:
    return 1;
  case CxxUnwindAction::ActionKind::Direct:
    return 2;
  case CxxUnwindAction::ActionKind::DestructorWithObject:
    return 3;
  case CxxUnwindAction::ActionKind::DestructorWithObjectPointer:
    return 4;
  }
  return std::nullopt;
}

bool appendGraphHeader(CanonicalBytes &Bytes, uint8_t GraphKind,
                       Arch TargetArch, const ExceptionFunction &EH) {
  const auto &OwnerRange = EH.CodeRange;
  const std::optional<uint8_t> StableArch = getStableArch(TargetArch);
  if (!StableArch || !OwnerRange.isValid())
    return false;
  Bytes.appendString(GraphDomain);
  Bytes.appendU32(SemanticDigestSchemaVersion);
  Bytes.appendU8(GraphKind);
  Bytes.appendU8(*StableArch);
  Bytes.appendU64(OwnerRange.Begin);
  Bytes.appendU64(OwnerRange.End);
  if (!appendCount(Bytes, EH.FragmentRanges.size()))
    return false;
  for (const auto &Range : EH.FragmentRanges) {
    if (!Range.isValid())
      return false;
    Bytes.appendU64(Range.Begin);
    Bytes.appendU64(Range.End);
  }
  return true;
}

std::optional<std::array<uint8_t, 32>>
getSEHGraphDigest(const ExceptionFunction &EH, Arch TargetArch) {
  if (!EH.SEH || EH.Cxx || EH.GSCookie ||
      EH.Personality != ExceptionPersonality::CSpecificHandler ||
      EH.ParseStatus != ExceptionParseStatus::Complete ||
      EH.model() != ExceptionModel::WindowsTable ||
      EH.SEH->Scopes.size() >
          static_cast<size_t>(std::numeric_limits<int32_t>::max()))
    return std::nullopt;

  CanonicalBytes Bytes;
  if (!appendGraphHeader(Bytes, SEHGraphKind, TargetArch, EH) ||
      !appendCount(Bytes, EH.SEH->Scopes.size()))
    return std::nullopt;

  std::vector<ExceptionAddressRange> SemanticRanges;
  SemanticRanges.reserve(EH.SEH->Scopes.size());
  for (const SEHScopeRecord &Scope : EH.SEH->Scopes) {
    if (Scope.ParseStatus != ExceptionParseStatus::Complete ||
        !getStableSEHScopeKind(Scope.Kind))
      return std::nullopt;
    const std::optional<ExceptionAddressRange> SemanticRange =
        getSemanticSEHGuardedRange(Scope, TargetArch, EH.CodeRange);
    if (!SemanticRange)
      return std::nullopt;
    SemanticRanges.push_back(*SemanticRange);
  }

  // The native table does not store an explicit parent.  Recover the same
  // unique immediate-containment graph the WinEH lowering consumes, and make
  // it part of the canonical source identity.
  for (size_t I = 0; I != SemanticRanges.size(); ++I)
    for (size_t J = I + 1; J != SemanticRanges.size(); ++J) {
      const ExceptionAddressRange &A = SemanticRanges[I];
      const ExceptionAddressRange &B = SemanticRanges[J];
      if (!A.overlaps(B))
        continue;
      if ((A.Begin == B.Begin && A.End == B.End) ||
          (!A.contains(B) && !B.contains(A)))
        return std::nullopt;
    }

  for (size_t I = 0; I != EH.SEH->Scopes.size(); ++I) {
    int32_t Parent = -1;
    uint64_t ParentSize = std::numeric_limits<uint64_t>::max();
    for (size_t J = 0; J != SemanticRanges.size(); ++J) {
      if (I == J || !SemanticRanges[J].contains(SemanticRanges[I]) ||
          SemanticRanges[J].size() >= ParentSize)
        continue;
      Parent = static_cast<int32_t>(J);
      ParentSize = SemanticRanges[J].size();
    }

    const SEHScopeRecord &Scope = EH.SEH->Scopes[I];
    const std::optional<uint8_t> StableKind = getStableSEHScopeKind(Scope.Kind);
    if (!StableKind)
      return std::nullopt;
    Bytes.appendU64(Scope.GuardedRange.Begin);
    Bytes.appendU64(Scope.GuardedRange.End);
    Bytes.appendU64(SemanticRanges[I].Begin);
    Bytes.appendU64(SemanticRanges[I].End);
    Bytes.appendU8(*StableKind);
    Bytes.appendU64(Scope.FilterOrFinallyVA);
    Bytes.appendU64(Scope.HandlerVA);
    Bytes.appendU64(Scope.ContinuationVA);
    Bytes.appendU64(Scope.NormalizedFilterVA);
    Bytes.appendU8(1); // Complete parse status in schema v1.
    Bytes.appendI32(Parent);
  }
  return llvm::SHA256::hash(Bytes.bytes());
}

bool appendRegistrationContract(CanonicalBytes &Bytes,
                                const ExceptionFunction &EH, uint8_t Encoding) {
  const RegistrationChainInfo &Chain = *EH.Registration;
  if (!appendCount(Bytes, Chain.Scopes.size()))
    return false;
  Bytes.appendU8(Encoding);
  Bytes.appendU64(EH.PersonalityVA);
  Bytes.appendU64(EH.HandlerDataVA);
  Bytes.appendU64(Chain.HandlerVA);
  Bytes.appendU64(Chain.ScopeTableVA);
  for (const auto &Offset :
       {Chain.TryLevelOffset, Chain.SeededTryLevel, Chain.RegistrationOffset}) {
    Bytes.appendU8(Offset.has_value());
    if (Offset)
      Bytes.appendI32(*Offset);
  }
  Bytes.appendU8(Chain.HasSecurityCookies);
  Bytes.appendI32(Chain.GSCookieOffset);
  Bytes.appendI32(Chain.GSCookieXOROffset);
  Bytes.appendI32(Chain.EHCookieOffset);
  Bytes.appendI32(Chain.EHCookieXOROffset);
  Bytes.appendU32(Chain.ScopeTableMagic);
  Bytes.appendU64(Chain.ChainInstallVA);
  Bytes.appendU64(Chain.ChainRemoveVA);
  Bytes.appendU8(Chain.RealignedFrame.has_value());
  if (Chain.RealignedFrame) {
    const auto &Frame = *Chain.RealignedFrame;
    Bytes.appendU8(Frame.BaseRegister);
    Bytes.appendU64(Frame.DefinitionVA);
    Bytes.appendU32(Frame.Alignment);
    Bytes.appendU32(Frame.AllocationBytes);
    Bytes.appendI32(Frame.BaseOffset);
    Bytes.appendI32(Frame.SavedParentFrameOffset);
  }
  for (const RegistrationScopeRecord &Scope : Chain.Scopes) {
    Bytes.appendI32(Scope.EnclosingLevel);
    Bytes.appendU64(Scope.FilterVA);
    Bytes.appendU64(Scope.HandlerVA);
    Bytes.appendU8(Scope.IsFinally);
  }
  if (!appendCount(Bytes, Chain.TryLevelStores.size()))
    return false;
  for (const RegistrationTryLevelStore &Store : Chain.TryLevelStores) {
    if ((Store.Width != 1 && Store.Width != 2 && Store.Width != 4) ||
        (Store.Width == 1 && uint32_t(Store.Level) > UINT8_MAX) ||
        (Store.Width == 2 && uint32_t(Store.Level) > UINT16_MAX))
      return false;
    Bytes.appendU64(Store.StoreVA);
    Bytes.appendU64(Store.EndVA);
    Bytes.appendI32(Store.Level);
    Bytes.appendU8(Store.Width);
  }
  return true;
}

std::optional<std::array<uint8_t, 32>>
getRegistrationSEHGraphDigest(const ExceptionFunction &EH) {
  if (!EH.Registration || EH.SEH || EH.Cxx || EH.GSCookie ||
      EH.ParseStatus != ExceptionParseStatus::Complete ||
      (EH.Encoding != ExceptionEncoding::X86ScopeTableEH3 &&
       EH.Encoding != ExceptionEncoding::X86ScopeTableEH4))
    return std::nullopt;
  CanonicalBytes Bytes;
  if (!appendGraphHeader(Bytes, RegistrationSEHGraphKind, Arch::X86, EH) ||
      !appendRegistrationContract(
          Bytes, EH,
          EH.Encoding == ExceptionEncoding::X86ScopeTableEH4 ? 2 : 1))
    return std::nullopt;
  return llvm::SHA256::hash(Bytes.bytes());
}

std::optional<std::array<uint8_t, 32>>
getCxxGraphDigest(const ExceptionFunction &EH, Arch TargetArch) {
  const bool Registration = EH.Registration.has_value();
  if (!EH.Cxx || EH.SEH || EH.ParseStatus != ExceptionParseStatus::Complete ||
      !EH.Cxx->hasValidStateGraph() ||
      (Registration ? (TargetArch != Arch::X86 ||
                       EH.Encoding != ExceptionEncoding::X86CxxFuncInfo ||
                       !EH.Registration->Scopes.empty())
                    : EH.model() != ExceptionModel::WindowsTable))
    return std::nullopt;

  const CxxExceptionInfo &Cxx = *EH.Cxx;
  const bool IsFH3 =
      (EH.Personality == ExceptionPersonality::CxxFrameHandler3 ||
       (Registration &&
        EH.Personality == ExceptionPersonality::CxxFrameHandlerX86)) &&
      Cxx.NativeEncoding == CxxExceptionInfo::Encoding::FH3;
  const bool IsGSFH4 =
      EH.Personality == ExceptionPersonality::GSHandlerCheckEH4;
  const bool IsFH4 =
      (EH.Personality == ExceptionPersonality::CxxFrameHandler4 || IsGSFH4) &&
      Cxx.NativeEncoding == CxxExceptionInfo::Encoding::FH4;
  if ((Registration && !IsFH3) || (!IsGSFH4 && EH.GSCookie) ||
      (IsGSFH4 && (!EH.GSCookie || EH.GSCookie->ParseStatus !=
                                       ExceptionParseStatus::Complete)) ||
      (!IsFH3 && !IsFH4))
    return std::nullopt;
  const std::optional<uint8_t> StableVersion = getStableCxxVersion(Cxx.Version);
  if (!StableVersion)
    return std::nullopt;

  CanonicalBytes Bytes;
  const uint8_t GraphKind = Registration ? RegistrationCxxGraphKind
                            : IsFH3      ? FH3GraphKind
                                         : FH4GraphKind;
  if (!appendGraphHeader(Bytes, GraphKind, TargetArch, EH))
    return std::nullopt;
  if (Registration) {
    Bytes.appendU8(
        EH.Personality == ExceptionPersonality::CxxFrameHandler3 ? 1 : 2);
    if (!appendRegistrationContract(Bytes, EH, 3))
      return std::nullopt;
  }
  Bytes.appendU8(IsFH3 ? 1 : 2);
  Bytes.appendU64(Cxx.NativeFuncInfoVA);
  Bytes.appendU32(Cxx.Magic);
  Bytes.appendU8(*StableVersion);
  Bytes.appendU32(Cxx.Flags);
  Bytes.appendU32(Cxx.MaxState);
  Bytes.appendI32(Cxx.UnwindHelpOffset);
  Bytes.appendU64(Cxx.ESTypeListVA);
  if (!appendCount(Bytes, Cxx.ExceptionSpecTypes.size()))
    return std::nullopt;
  for (const CxxExceptionSpecType &Type : Cxx.ExceptionSpecTypes) {
    Bytes.appendU32(Type.Adjectives);
    Bytes.appendU64(Type.TypeDescriptorVA);
  }
  Bytes.appendU32(Cxx.BBTFlags);
  Bytes.appendU32(Cxx.FrameOffset);
  Bytes.appendU8(Cxx.IsCatchFunclet ? 1 : 0);
  Bytes.appendU8(Cxx.IsSeparated ? 1 : 0);
  Bytes.appendU8(Cxx.IsSynchronous ? 1 : 0);
  Bytes.appendU8(Cxx.IsNoExcept ? 1 : 0);
  Bytes.appendU8(Cxx.HasDynamicStackAlignment ? 1 : 0);

  if (!appendCount(Bytes, Cxx.UnwindMap.size()))
    return std::nullopt;
  for (const CxxUnwindAction &Action : Cxx.UnwindMap) {
    const std::optional<uint8_t> StableKind =
        getStableCxxActionKind(Action.Kind);
    if (!StableKind)
      return std::nullopt;
    Bytes.appendI32(Action.ToState);
    Bytes.appendU64(Action.ActionVA);
    Bytes.appendU8(*StableKind);
    Bytes.appendI32(Action.ObjectOffset);
  }

  if (!appendCount(Bytes, Cxx.TryBlocks.size()))
    return std::nullopt;
  for (const CxxTryBlock &Try : Cxx.TryBlocks) {
    Bytes.appendI32(Try.TryLow);
    Bytes.appendI32(Try.TryHigh);
    Bytes.appendI32(Try.CatchHigh);
    if (!appendCount(Bytes, Try.Handlers.size()))
      return std::nullopt;
    for (const CxxCatchHandler &Catch : Try.Handlers) {
      Bytes.appendU32(Catch.Adjectives);
      Bytes.appendU64(Catch.TypeDescriptorVA);
      Bytes.appendI32(Catch.CatchObjectOffset);
      Bytes.appendU64(Catch.HandlerVA);
      Bytes.appendI32(Catch.ParentFrameOffset);
      if (!appendCount(Bytes, Catch.ContinuationVAs.size()))
        return std::nullopt;
      for (va_t ContinuationVA : Catch.ContinuationVAs)
        Bytes.appendU64(ContinuationVA);
    }
  }

  if (!appendCount(Bytes, Cxx.IPMap.size()))
    return std::nullopt;
  for (const CxxIPState &State : Cxx.IPMap) {
    Bytes.appendU64(State.IP);
    Bytes.appendI32(State.State);
  }
  if (IsGSFH4) {
    const GSCookieInfo &Cookie = *EH.GSCookie;
    Bytes.appendString(GSFH4Domain);
    Bytes.appendI32(Cookie.CookieOffset);
    Bytes.appendU8(Cookie.HasExceptionHandler ? 1 : 0);
    Bytes.appendU8(Cookie.HasUnwindHandler ? 1 : 0);
    Bytes.appendU8(Cookie.HasAlignment ? 1 : 0);
    Bytes.appendI32(Cookie.AlignmentBaseOffset);
    Bytes.appendU32(Cookie.Alignment);
    if (!appendCount(Bytes, Cookie.Payload.size()))
      return std::nullopt;
    for (uint8_t Byte : Cookie.Payload)
      Bytes.appendU8(Byte);
  }
  return llvm::SHA256::hash(Bytes.bytes());
}

llvm::mc_rewrite::RewriteWinEHSemanticToken
makeToken(const std::array<uint8_t, 32> &GraphDigest,
          llvm::mc_rewrite::RewriteWinEHSemanticKind Kind, uint32_t Region,
          uint32_t Clause) {
  CanonicalBytes Bytes;
  Bytes.appendString(TokenDomain);
  Bytes.appendU32(SemanticDigestSchemaVersion);
  Bytes.appendDigest(GraphDigest);
  Bytes.appendU8(static_cast<uint8_t>(Kind));
  Bytes.appendU32(Region);
  Bytes.appendU32(Clause);
  const std::array<uint8_t, 32> Digest = llvm::SHA256::hash(Bytes.bytes());

  llvm::mc_rewrite::RewriteWinEHSemanticToken Token;
  Token.Kind = Kind;
  Token.Region = Region;
  Token.Clause = Clause;
  for (unsigned Word = 0; Word != Token.Digest.size(); ++Word)
    for (unsigned Byte = 0; Byte != 8; ++Byte)
      Token.Digest[Word] |= static_cast<uint64_t>(Digest[Word * 8 + Byte])
                            << (Byte * 8);
  return Token;
}

} // namespace

std::optional<llvm::mc_rewrite::RewriteWinEHSemanticToken>
getSEHScopeSemanticToken(const ExceptionFunction &EH, Arch TargetArch,
                         uint32_t ScopeIndex) {
  const bool Registration = TargetArch == Arch::X86 && EH.Registration;
  if (Registration ? ScopeIndex >= EH.Registration->Scopes.size()
                   : (!EH.SEH || ScopeIndex >= EH.SEH->Scopes.size()))
    return std::nullopt;
  const std::optional<std::array<uint8_t, 32>> GraphDigest =
      Registration ? getRegistrationSEHGraphDigest(EH)
                   : getSEHGraphDigest(EH, TargetArch);
  if (!GraphDigest)
    return std::nullopt;
  return makeToken(*GraphDigest,
                   llvm::mc_rewrite::RewriteWinEHSemanticKind::SEHScope,
                   ScopeIndex, /*Clause=*/0);
}

std::optional<llvm::mc_rewrite::RewriteWinEHSemanticToken>
getCxxCatchSemanticToken(const ExceptionFunction &EH, Arch TargetArch,
                         uint32_t TryBlockIndex, uint32_t CatchIndex) {
  if (!EH.Cxx || TryBlockIndex >= EH.Cxx->TryBlocks.size() ||
      CatchIndex >= EH.Cxx->TryBlocks[TryBlockIndex].Handlers.size())
    return std::nullopt;
  const std::optional<std::array<uint8_t, 32>> GraphDigest =
      getCxxGraphDigest(EH, TargetArch);
  if (!GraphDigest)
    return std::nullopt;
  return makeToken(*GraphDigest,
                   llvm::mc_rewrite::RewriteWinEHSemanticKind::CxxCatch,
                   TryBlockIndex, CatchIndex);
}

std::optional<llvm::mc_rewrite::RewriteWinEHSemanticToken>
getCxxCleanupSemanticToken(const ExceptionFunction &EH, Arch TargetArch,
                           uint32_t ActionIndex) {
#ifndef LLVM_NEVERD_X86_CXX_FUNCTION_RECEIPTS
  return std::nullopt;
#else
  if (TargetArch != Arch::X86 || !EH.Registration || !EH.Cxx ||
      ActionIndex >= EH.Cxx->UnwindMap.size())
    return std::nullopt;
  const auto &Action = EH.Cxx->UnwindMap[ActionIndex];
  if (!Action.ActionVA || Action.Kind != CxxUnwindAction::ActionKind::Direct)
    return std::nullopt;
  const auto GraphDigest = getCxxGraphDigest(EH, TargetArch);
  if (!GraphDigest)
    return std::nullopt;
  return makeToken(*GraphDigest,
                   llvm::mc_rewrite::RewriteWinEHSemanticKind::CxxCleanup,
                   ActionIndex, 0);
#endif
}

} // namespace neverd::windows_eh_semantics
