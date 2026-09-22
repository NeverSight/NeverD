#ifndef NEVERD_LOADER_SWIFT_SWIFTBOOLEANPROJECTION_H
#define NEVERD_LOADER_SWIFT_SWIFTBOOLEANPROJECTION_H

#include "../../ir/low/SourceBooleanResultProof.h"
#include "../ObjC/ObjCClassAccessorMachine.h"
#include "SwiftBooleanRuntimeCandidate.h"

#include "neverd/loader/ObjC/ObjCCallHints.h"
#include "neverd/loader/ObjC/ObjCSourceDeclarations.h"
#include "neverd/loader/ReadOnlyBytes.h"

#include "llvm/Support/Endian.h"

namespace neverd {

/// Current caller-specific evidence, not a runtime byte-return declaration.
/// Publication must rerun this owner using the current complete pipeline LowIR
/// and entry metadata. A saved result does not authenticate a source body.
struct SwiftBooleanProjection {
  SourceBooleanResultCertificate Normalization;
  SwiftBooleanRuntimeCandidate Runtime;
};

namespace swift_boolean_projection_detail {
inline bool directCall(const BinaryImage &Image,
                       const SourceCallOccurrenceKey &Site) {
  if (Site.Opcode != NdOp::CALL || !Site.StaticTarget || Site.Instruction % 4 ||
      *Site.StaticTarget % 4)
    return false;
  const auto Bytes = readImmutableCodeBytes(Image, Site.Instruction, 4);
  if (!Bytes)
    return false;
  const auto Word = llvm::support::endian::read32le(Bytes->data());
  if ((Word & 0xfc000000) != 0x94000000)
    return false;
  const int64_t Displacement =
      (int64_t(Word & 0x03ffffff) - ((Word & 0x02000000) ? 0x04000000 : 0)) * 4;
  if ((Displacement < 0 && Site.Instruction < uint64_t(-Displacement)) ||
      (Displacement >= 0 &&
       Site.Instruction > UINT64_MAX - uint64_t(Displacement)))
    return false;
  return Site.Instruction + Displacement == *Site.StaticTarget;
}

inline bool ordinaryRuntime(const SourceCallTypeHint &Hint) {
  using Kind = SourceCallTypeHint::Kind;
  return Hint.CallKind == Kind::ObjCRuntimeCall ||
         Hint.CallKind == Kind::SwiftRuntimeCall ||
         Hint.CallKind == Kind::SwiftStringBridge ||
         Hint.CallKind == Kind::SwiftStringFromNSString ||
         Hint.CallKind == Kind::DarwinRuntimeCall;
}

// This supplies only the complete physical ABI to the difference proof. The
// existing super-dispatch publication gate still owns receiver/frame binding.
inline bool superInit(const BinaryImage &Image,
                      const SourceCallOccurrenceKey &Site, va_t Slot,
                      const SourceCallTypeHint &Hint) {
  const auto Import = darwinRuntimeImport(Image, Slot);
  const auto Bind = Image.DyldBindSlots.find(Slot);
  const auto &Signature = Hint.Signature;
  return Hint.CallKind == SourceCallTypeHint::Kind::ObjCSuper2 &&
         Hint.TargetAddress == Site.StaticTarget && Hint.Selector == "init" &&
         Hint.TargetName == "objc_msgSendSuper2" && Import &&
         *Import == "_objc_msgSendSuper2" &&
         Bind != Image.DyldBindSlots.end() &&
         Bind->second.Module == "/usr/lib/libobjc.A.dylib" && !Hint.Format &&
         !Hint.NilTerminated && !Hint.WeakImport && !Hint.DoesNotReturn &&
         Signature.Convention == SourceFunctionTypeHint::ConventionKind::C &&
         Signature.ReturnType &&
         Signature.ReturnType->Kind == NdTypeKind::Ptr &&
         Signature.ReturnType->Size == 8 && Signature.Parameters.size() == 2 &&
         std::all_of(Signature.Parameters.begin(), Signature.Parameters.end(),
                     [](const auto &P) {
                       return P.Type && P.Type->Kind == NdTypeKind::Ptr &&
                              P.Type->Size == 8 &&
                              P.TheRole ==
                                  SourceParameterTypeHint::Role::Ordinary;
                     });
}
} // namespace swift_boolean_projection_detail

/// Deliberately bounded to an Objective-C entry, at most eight comparison
/// occurrences, and other direct calls with freshly catalogued runtime ABIs,
/// exact super init dispatch or complete eight-instruction class-accessor
/// machine proofs. Other native candidates and dynamic dispatch, weak imports,
/// indirect calls and unknown effects do not borrow authority from candidate
/// signatures.
inline std::vector<SwiftBooleanProjection>
qualifySwiftBooleanProjections(const BinaryImage &Image, const LowFunc &Low,
                               const SourceFunctionTypeHint &EntrySignature) {
  if (Image.Format != BinaryFormat::MachO || Image.Arch != Arch::AArch64 ||
      Image.Bits != Bitness::Bits64 || Image.IsRelocatable ||
      Low.Blocks.empty() || Low.Blocks.size() > 256)
    return {};
  const auto Entry = objcMethodSourceTypeHint(Image, Low.Entry);
  if (!Entry || !equalSourceABIs(*Entry, EntrySignature))
    return {};
  for (const auto &Method : Image.ObjCMethods)
    if (Method.Implementation == Low.Entry && Method.Status != "supported")
      return {};
  const auto Hints = buildObjCSourceCallHints(Image, Low);
  std::vector<SwiftBooleanProjection> Selected;
  std::map<SourceCallOccurrenceKey, SourceFunctionTypeHint> RawInputs;
  std::map<SourceCallOccurrenceKey, SourceBooleanOtherCallContract> Calls;
  std::map<va_t, SourceFunctionTypeHint> ClassAccessors;
  std::set<va_t> CallInstructions;
  for (const auto &Block : Low.Blocks) {
    if (Block.EndAddr <= Block.StartAddr ||
        Block.EndAddr - Block.StartAddr > 32768 ||
        !readImmutableCodeBytes(Image, Block.StartAddr,
                                Block.EndAddr - Block.StartAddr))
      return {};
    for (const auto &Op : Block.Ops) {
      if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL)
        continue;
      const auto Site = sourceCallOccurrenceKey(Op);
      if (!Site || !CallInstructions.insert(Op.Addr).second ||
          !swift_boolean_projection_detail::directCall(Image, *Site))
        return {};
      const auto Candidate =
          swiftBooleanRuntimeVeneerCandidate(Image, *Site->StaticTarget);
      if (Candidate) {
        if (Selected.size() == 8)
          return {};
        auto Inputs = swiftBooleanRuntimeInputs(Candidate->ImportName);
        if (!Inputs)
          return {};
        // Other raw Boolean calls establish only their inputs. No result byte
        // is declared defined: in particular, i1 must not become a byte ABI.
        const auto [It, Inserted] =
            RawInputs.emplace(*Site, std::move(*Inputs));
        if (!Inserted ||
            !Calls.emplace(*Site, SourceBooleanOtherCallContract{&It->second})
                 .second)
          return {};
        Selected.push_back(SwiftBooleanProjection{{&Low, *Site}, *Candidate});
        continue;
      }
      const auto Hint = Hints.find(Op.Addr);
      const auto Slot = darwinImportVeneerSlot(Image, *Site->StaticTarget);
      if (Hint == Hints.end()) {
        const auto Machine =
            objcClassAccessorMachine(Image, *Site->StaticTarget);
        if (!Machine)
          return {};
        const auto [It, Inserted] =
            ClassAccessors.emplace(*Site->StaticTarget, Machine->Signature);
        if (!Calls.emplace(*Site, SourceBooleanOtherCallContract{&It->second})
                 .second)
          return {};
        continue;
      }
      if (!Slot ||
          !((Hint->second.TargetAddress == *Slot &&
             swift_boolean_projection_detail::ordinaryRuntime(Hint->second)) ||
            swift_boolean_projection_detail::superInit(Image, *Site, *Slot,
                                                       Hint->second)) ||
          Hint->second.WeakImport || Hint->second.DoesNotReturn ||
          !Calls
               .emplace(*Site,
                        SourceBooleanOtherCallContract{&Hint->second.Signature})
               .second)
        return {};
    }
  }
  for (const auto &Projection : Selected) {
    auto OtherCalls = Calls;
    OtherCalls.erase(Projection.Normalization.Site);
    if (!proveSourceBooleanResultNormalization(
            Low, Image.Arch, Projection.Normalization.Site,
            Projection.Runtime.RawContract, OtherCalls, EntrySignature))
      return {};
  }
  return Selected;
}

inline std::optional<SwiftBooleanProjection>
qualifySwiftBooleanProjection(const BinaryImage &Image, const LowFunc &Low,
                              const SourceFunctionTypeHint &EntrySignature) {
  auto Projections = qualifySwiftBooleanProjections(Image, Low, EntrySignature);
  return Projections.size() == 1
             ? std::optional<SwiftBooleanProjection>(Projections.front())
             : std::nullopt;
}
} // namespace neverd
#endif
