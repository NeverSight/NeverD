#include "neverd/loader/ObjC/ObjCContextCallEffects.h"

#include "../MachO/SourceLocalCall.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/ImmutableNativeCalls.h"
#include "neverd/loader/ObjC/ObjCCallHints.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BranchEncoding.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd {
namespace {
struct ContextBody {
  uint64_t Register;
  SourceFunctionTypeHint Message;
};

std::optional<ContextBody> body(const BinaryImage &Image, va_t Entry) {
  if (Image.Format != BinaryFormat::MachO || Image.Arch != Arch::AArch64 ||
      Image.Bits != Bitness::Bits64 || Image.IsRelocatable || !Entry ||
      Entry % 4 || Entry > InvalidVA - 24)
    return std::nullopt;
  // Negative filter only; the complete immutable range is checked below.
  const auto *Prefix = Image.readVA(Entry, 8);
  if (!Prefix)
    return std::nullopt;
  const auto Load = llvm::support::endian::read32le(Prefix);
  const unsigned Context = (Load >> 5) & 31;
  if ((Load & 0xfffffc1f) != 0xf9400000 || Context < 19 || Context > 28)
    return std::nullopt;
  const auto Next = llvm::support::endian::read32le(Prefix + 4);
  const bool Counter = (Next & 0x9f00001f) == 0x90000008;
  const unsigned Size = Counter ? 24 : 8;
  const auto Bytes = readImmutableCodeBytes(Image, Entry, Size);
  if (!Bytes || !sourceLeafCodeRange(Image, Entry, Size) ||
      std::none_of(
          Image.Symbols.begin(), Image.Symbols.end(),
          [&](const Symbol &S) { return S.IsFunc && S.Addr == Entry; }))
    return std::nullopt;
  const auto Word = [&](unsigned I) {
    return llvm::support::endian::read32le(Bytes->data() + I * 4);
  };
  if (Counter) {
    // ADRP x8; LDR x9,[x8,#offset]; ADD x9,x9,#1; STR x9,[x8,#offset].
    // No context-derived value reaches the counter or another argument.
    if ((Word(2) & 0xffc003ff) != 0xf9400109 || Word(3) != 0x91000529 ||
        Word(4) != (Word(2) & ~0x00400000u))
      return std::nullopt;
    const uint32_t Imm = ((Next >> 29) & 3) | ((Next >> 3) & 0x1ffffc);
    const int64_t Delta =
        (int64_t(Imm) - ((Imm & 0x100000) ? 0x200000 : 0)) * 4096;
    const va_t Page = (Entry + 4) & ~va_t(0xfff);
    if ((Delta < 0 && Page < uint64_t(-Delta)) ||
        (Delta >= 0 && Page > UINT64_MAX - uint64_t(Delta)))
      return std::nullopt;
    const uint64_t Offset = ((Word(2) >> 10) & 0xfff) * 8;
    const va_t Base = Page + Delta;
    if (Base > UINT64_MAX - Offset ||
        !isFileBackedWritableImageRange(Image, Base + Offset, 8))
      return std::nullopt;
  }
  const auto Branch = Word(Size / 4 - 1);
  if (!branch::A64Branch.matches(Branch))
    return std::nullopt;
  const auto Target = branch::a64BranchTarget(Branch, Entry + Size - 4);
  const auto Message =
      Target ? objcSelectorStubSourceCallHint(Image, *Target) : std::nullopt;
  if (!Message || !Message->Signature.ReturnType ||
      Message->Signature.ReturnType->Kind != NdTypeKind::Void)
    return std::nullopt;
  return ContextBody{Context * 8, Message->Signature};
}
} // namespace

bool isObjCContextProjection(const BinaryImage &Image, va_t Entry) {
  return bool(body(Image, Entry));
}

std::optional<SourceFrameEffects>
objcContextProjectionEffects(const BinaryImage &Image, va_t Entry,
                             const SourceFunctionTypeHint &Signature) {
  std::string Error;
  if (!Signature.HasExplicitABI || Signature.Architecture != Image.Arch ||
      Signature.Origin != SourceFunctionTypeHint::OriginKind::NativeAnalysis ||
      Signature.Convention != SourceFunctionTypeHint::ConventionKind::C ||
      !Signature.ReturnType || Signature.ReturnType->Kind != NdTypeKind::Void ||
      !Signature.ReturnComponents.empty() ||
      !validateSourceABI(Signature, Error))
    return std::nullopt;
  const auto Body = body(Image, Entry);
  if (!Body || !Body->Message.ReturnType ||
      Body->Message.ReturnType->Kind != NdTypeKind::Void ||
      !validateSourceABI(Body->Message, Error))
    return std::nullopt;
  const auto Message = sourceABIParameters(Body->Message);
  const auto Native = sourceABIParameters(Signature);
  if (Message.size() < 2 || Native.size() + 1 != Message.size() ||
      Native.size() != Signature.Parameters.size())
    return std::nullopt;
  for (unsigned I = 0; I < 2; ++I)
    if (Message[I].ParameterIndex != I || !Message[I].Type ||
        Message[I].Type->Kind != NdTypeKind::Ptr ||
        Message[I].Type->Size != 8 ||
        Message[I].Location.Kind != SourceABICarrierKind::IntegerRegister ||
        Message[I].Location.RegisterOffset != I * 8 ||
        Message[I].Location.ValueBytes != 8)
      return std::nullopt;
  SourceFrameEffects Effects;
  for (const auto &P : Native) {
    const auto &Logical = Signature.Parameters[P.ParameterIndex];
    if (!Logical.Components.empty() ||
        Logical.TheRole != SourceParameterTypeHint::Role::Ordinary)
      return std::nullopt;
    if (P.Location.Kind == SourceABICarrierKind::IntegerRegister &&
        P.Location.RegisterOffset == Body->Register) {
      Effects.ReadOnlyFrameParameters.emplace(P.ParameterIndex, 8);
      continue;
    }
    // The two scratch registers and incoming selector are never forwarded.
    // Stack arguments and unmodeled aggregates remain unsupported.
    if ((P.Location.Kind != SourceABICarrierKind::IntegerRegister &&
         P.Location.Kind != SourceABICarrierKind::FloatingRegister) ||
        (P.Location.Kind == SourceABICarrierKind::IntegerRegister &&
         (P.Location.RegisterOffset < 16 || P.Location.RegisterOffset > 56)) ||
        std::count_if(Message.begin() + 2, Message.end(), [&](const auto &M) {
          return M.Location.Kind == P.Location.Kind &&
                 M.Location.RegisterOffset == P.Location.RegisterOffset &&
                 M.Location.EntryStackOffset == P.Location.EntryStackOffset &&
                 M.Location.ValueBytes == P.Location.ValueBytes &&
                 M.Location.ExtendTo32Bits == P.Location.ExtendTo32Bits &&
                 equalSourceTypes(M.Type, P.Type);
        }) != 1)
      return std::nullopt;
  }
  return Effects.ReadOnlyFrameParameters.size() == 1 &&
                 sourceFrameEffectsMatchABI(Effects, Signature)
             ? std::optional(Effects)
             : std::nullopt;
}

std::optional<SourceFrameEffects> objcContextCallEffects(
    const BinaryImage &Image, const LowFunc &Caller,
    const SourceCallOccurrenceKey &Site,
    const SourceFunctionTypeHint &Signature,
    const std::map<va_t, SourceFunctionTypeHint> *NativeCallees) {
  std::optional<va_t> Target;
  if (Site.StaticTarget && sourceLocalCalls(Image, Caller).count(Site))
    Target = Site.StaticTarget;
  else if (!Site.StaticTarget && Site.Opcode == NdOp::INDIR_CALL) {
    const auto Calls = immutableNativeCallTargets(Image, Caller, NativeCallees);
    const auto Found = Calls.find(Site.Instruction);
    if (Found != Calls.end() && Found->second.Site == Site)
      Target = Found->second.Target;
  }
  return Target ? objcContextProjectionEffects(Image, *Target, Signature)
                : std::nullopt;
}
} // namespace neverd
