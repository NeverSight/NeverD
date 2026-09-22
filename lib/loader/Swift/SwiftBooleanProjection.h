#ifndef NEVERD_LOADER_SWIFT_SWIFTBOOLEANPROJECTION_H
#define NEVERD_LOADER_SWIFT_SWIFTBOOLEANPROJECTION_H

#include "../../ir/low/SourceBooleanResultProof.h"
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
} // namespace swift_boolean_projection_detail

/// Deliberately bounded to an Objective-C entry, one comparison occurrence,
/// and other direct calls with freshly catalogued runtime ABIs. Native callees,
/// dynamic dispatch, weak imports, indirect calls and unknown effects require
/// separate evidence and do not borrow authority from candidate signatures.
inline std::optional<SwiftBooleanProjection>
qualifySwiftBooleanProjection(const BinaryImage &Image, const LowFunc &Low,
                              const SourceFunctionTypeHint &EntrySignature) {
  if (Image.Format != BinaryFormat::MachO || Image.Arch != Arch::AArch64 ||
      Image.Bits != Bitness::Bits64 || Image.IsRelocatable ||
      Low.Blocks.empty() || Low.Blocks.size() > 256)
    return std::nullopt;
  const auto Entry = objcMethodSourceTypeHint(Image, Low.Entry);
  if (!Entry || !equalSourceABIs(*Entry, EntrySignature))
    return std::nullopt;
  for (const auto &Method : Image.ObjCMethods)
    if (Method.Implementation == Low.Entry && Method.Status != "supported")
      return std::nullopt;
  const auto Hints = buildObjCSourceCallHints(Image, Low);
  std::optional<SwiftBooleanProjection> Selected;
  std::map<SourceCallOccurrenceKey, SourceBooleanOtherCallContract> Calls;
  std::set<va_t> CallInstructions;
  for (const auto &Block : Low.Blocks) {
    if (Block.EndAddr <= Block.StartAddr ||
        Block.EndAddr - Block.StartAddr > 32768 ||
        !readImmutableCodeBytes(Image, Block.StartAddr,
                                Block.EndAddr - Block.StartAddr))
      return std::nullopt;
    for (const auto &Op : Block.Ops) {
      if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL)
        continue;
      const auto Site = sourceCallOccurrenceKey(Op);
      if (!Site || !CallInstructions.insert(Op.Addr).second ||
          !swift_boolean_projection_detail::directCall(Image, *Site))
        return std::nullopt;
      const auto Candidate =
          swiftBooleanRuntimeVeneerCandidate(Image, *Site->StaticTarget);
      if (Candidate) {
        if (Selected)
          return std::nullopt;
        Selected = SwiftBooleanProjection{{&Low, *Site}, *Candidate};
        continue;
      }
      const auto Hint = Hints.find(Op.Addr);
      const auto Slot = darwinImportVeneerSlot(Image, *Site->StaticTarget);
      if (!Slot || Hint == Hints.end() || Hint->second.TargetAddress != *Slot ||
          !swift_boolean_projection_detail::ordinaryRuntime(Hint->second) ||
          Hint->second.WeakImport || Hint->second.DoesNotReturn ||
          !Calls
               .emplace(*Site,
                        SourceBooleanOtherCallContract{&Hint->second.Signature})
               .second)
        return std::nullopt;
    }
  }
  if (!Selected || !proveSourceBooleanResultNormalization(
                       Low, Image.Arch, Selected->Normalization.Site,
                       Selected->Runtime.RawContract, Calls, EntrySignature))
    return std::nullopt;
  return Selected;
}
} // namespace neverd
#endif
