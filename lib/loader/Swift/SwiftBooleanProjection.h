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
inline bool nativeEntry(const BinaryImage &Image, va_t Address,
                        const SourceFunctionTypeHint &Signature) {
  const auto &Returns = getTargetRegInfo(Arch::AArch64).IntReturnRegs;
  const bool ScalarReturn =
      Signature.ReturnType && Signature.ReturnType->Kind == NdTypeKind::Int &&
      Signature.ReturnType->Size == 8 &&
      Signature.ReturnLocation.Kind == SourceABICarrierKind::IntegerRegister &&
      Signature.ReturnLocation.RegisterOffset == Returns[0] &&
      Signature.ReturnLocation.ValueBytes == 8 &&
      Signature.ReturnComponents.empty();
  const bool PairReturn =
      Signature.ReturnType &&
      Signature.ReturnType->Kind == NdTypeKind::Struct &&
      Signature.ReturnType->Size == 16 &&
      Signature.ReturnType->Fields.size() == 2 &&
      Signature.ReturnType->FieldOffsets == std::vector<uint16_t>{0, 8} &&
      std::all_of(Signature.ReturnType->Fields.begin(),
                  Signature.ReturnType->Fields.end(),
                  [](const TypeRef &Field) {
                    return Field && Field->Kind == NdTypeKind::Int &&
                           Field->Size == 8;
                  }) &&
      Signature.ReturnLocation.Kind == SourceABICarrierKind::None &&
      Signature.ReturnComponents.size() == 2 &&
      Signature.ReturnComponents[0].Kind ==
          SourceABICarrierKind::IntegerRegister &&
      Signature.ReturnComponents[0].RegisterOffset == Returns[0] &&
      Signature.ReturnComponents[0].ValueBytes == 8 &&
      Signature.ReturnComponents[1].Kind ==
          SourceABICarrierKind::IntegerRegister &&
      Signature.ReturnComponents[1].RegisterOffset == Returns[1] &&
      Signature.ReturnComponents[1].ValueBytes == 8;
  if (Signature.Origin != SourceFunctionTypeHint::OriginKind::NativeAnalysis ||
      !Image.isCodeAddress(Address) || (!ScalarReturn && !PairReturn))
    return false;
  for (const auto &Method : Image.ObjCMethods)
    if (Method.Implementation == Address)
      return false;
  bool HasFunction = false;
  for (const auto &Symbol : Image.Symbols) {
    if (Symbol.Addr != Address)
      continue;
    if (!Symbol.IsFunc || Symbol.Name.empty())
      return false;
    HasFunction = true;
  }
  return HasFunction;
}

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

inline bool terminalBrk(const BinaryImage &Image, const LowBlock &Block) {
  if (!source_boolean_result_detail::terminalBrk(Block))
    return false;
  const auto Bytes = readImmutableCodeBytes(Image, Block.Ops.front().Addr, 4);
  if (!Bytes)
    return false;
  const auto Word = llvm::support::endian::read32le(Bytes->data());
  return (Word & 0xffe0001f) == 0xd4200000;
}

inline bool ordinaryRuntime(const SourceCallTypeHint &Hint) {
  using Kind = SourceCallTypeHint::Kind;
  return Hint.CallKind == Kind::ObjCRuntimeCall ||
         Hint.CallKind == Kind::SwiftRuntimeCall ||
         Hint.CallKind == Kind::SwiftStringBridge ||
         Hint.CallKind == Kind::SwiftStringFromNSString ||
         Hint.CallKind == Kind::DarwinRuntimeCall;
}

// This identifies a current fixed object-returning SDK message occurrence.
// Its authenticated pointer ABI may be supplied to the Boolean proof; source
// publication still revalidates the message and receiver independently.
inline bool fixedObjectMessage(const BinaryImage &Image,
                               const SourceCallOccurrenceKey &Site,
                               const SourceCallTypeHint &Hint) {
  if (!Site.StaticTarget || *Site.StaticTarget % 4 ||
      *Site.StaticTarget > UINT64_MAX - 20 ||
      Hint.CallKind != SourceCallTypeHint::Kind::ObjCMessage ||
      Hint.TargetAddress != Site.StaticTarget ||
      Hint.TargetName != "objc_msgSend" || Hint.Selector.empty() ||
      !Hint.SelectorReferenceAddress || Hint.Format || Hint.NilTerminated ||
      Hint.WeakImport || Hint.DoesNotReturn ||
      !readImmutableCodeBytes(Image, *Site.StaticTarget, 20) ||
      !objcSelectorStubMatches(Image, *Site.StaticTarget,
                               Hint.SelectorReferenceAddress, Hint.Selector))
    return false;
  const auto Ref =
      Image.ObjCSourceReferences.find(Hint.SelectorReferenceAddress);
  if (Ref == Image.ObjCSourceReferences.end() ||
      Ref->second.TheKind != ObjCSourceReference::Kind::Selector ||
      Ref->second.Address != Hint.SelectorReferenceAddress ||
      Ref->second.Size != 8 || Ref->second.Name != Hint.Selector)
    return false;
  const auto Slot = darwinImportVeneerSlot(Image, *Site.StaticTarget + 8);
  if (!Slot || !isImmutableImageImportSlot(Image, *Slot))
    return false;
  const auto Import = darwinRuntimeImport(Image, *Slot);
  const auto Bind = Image.DyldBindSlots.find(*Slot);
  const auto &Signature = Hint.Signature;
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  std::string Error;
  return Import && *Import == "_objc_msgSend" &&
         Bind != Image.DyldBindSlots.end() &&
         Bind->second.Module == "/usr/lib/libobjc.A.dylib" &&
         std::count(Image.DynInfo.NeededLibs.begin(),
                    Image.DynInfo.NeededLibs.end(), Bind->second.Module) == 1 &&
         Signature.Origin == SourceFunctionTypeHint::OriginKind::ObjCSDK &&
         Signature.Architecture == Arch::AArch64 && Signature.HasExplicitABI &&
         Signature.Convention == SourceFunctionTypeHint::ConventionKind::C &&
         validateSourceABI(Signature, Error) && Signature.ReturnType &&
         Signature.ReturnType->Kind == NdTypeKind::Ptr &&
         Signature.ReturnType->Size == 8 &&
         Signature.ReturnLocation.Kind ==
             SourceABICarrierKind::IntegerRegister &&
         Signature.ReturnLocation.RegisterOffset == TRI.IntReturnReg &&
         Signature.ReturnLocation.ValueBytes == 8 &&
         !Signature.ReturnLocation.ExtendTo32Bits &&
         Signature.ReturnComponents.empty() &&
         Signature.Parameters.size() >= 2 &&
         Signature.Parameters.size() <= TRI.IntParamRegs.size() &&
         std::count(Hint.Selector.begin(), Hint.Selector.end(), ':') ==
             Signature.Parameters.size() - 2 &&
         std::all_of(Signature.Parameters.begin(), Signature.Parameters.end(),
                     [](const auto &P) {
                       return P.Type && P.Type->Kind == NdTypeKind::Ptr &&
                              P.Type->Size == 8 && P.Components.empty() &&
                              P.TheRole ==
                                  SourceParameterTypeHint::Role::Ordinary;
                     }) &&
         std::all_of(Signature.Parameters.begin(), Signature.Parameters.end(),
                     [&](const auto &P) {
                       const auto Index = &P - Signature.Parameters.data();
                       return P.Location.Kind ==
                                  SourceABICarrierKind::IntegerRegister &&
                              P.Location.RegisterOffset ==
                                  TRI.IntParamRegs[Index] &&
                              P.Location.ValueBytes == 8 &&
                              !P.Location.ExtendTo32Bits;
                     });
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

/// Deliberately bounded to a verified Objective-C or native entry, at most
/// eight comparison occurrences, and other direct calls with freshly catalogued
/// runtime ABIs, exact super init dispatch or complete eight-instruction
/// class-accessor machine proofs. Other direct calls require identical physical
/// state and supply no ABI or binding facts. Indirect calls remain unsupported.
inline std::vector<SwiftBooleanProjection>
qualifySwiftBooleanProjections(const BinaryImage &Image, const LowFunc &Low,
                               const SourceFunctionTypeHint &EntrySignature) {
  if (Image.Format != BinaryFormat::MachO || Image.Arch != Arch::AArch64 ||
      Image.Bits != Bitness::Bits64 || Image.IsRelocatable ||
      Low.Blocks.empty() || Low.Blocks.size() > 256)
    return {};
  const auto Entry = objcMethodSourceTypeHint(Image, Low.Entry);
  if (Entry ? !equalSourceABIs(*Entry, EntrySignature)
            : !swift_boolean_projection_detail::nativeEntry(Image, Low.Entry,
                                                            EntrySignature))
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
    if (source_boolean_result_detail::terminalBrk(Block) &&
        !swift_boolean_projection_detail::terminalBrk(Image, Block))
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
        // Other raw Boolean calls establish their inputs and bit 0 of x0.
        // Their higher result bits remain unknown: i1 is not a byte ABI.
        const auto [It, Inserted] =
            RawInputs.emplace(*Site, std::move(*Inputs));
        SourceBooleanOtherCallContract Contract{&It->second};
        Contract.DefinesRawBooleanBit0 = true;
        if (!Inserted || !Calls.emplace(*Site, Contract).second)
          return {};
        Selected.push_back(SwiftBooleanProjection{{&Low, *Site}, *Candidate});
        continue;
      }
      const auto Hint = Hints.find(Op.Addr);
      const auto Slot = darwinImportVeneerSlot(Image, *Site->StaticTarget);
      if (Hint == Hints.end()) {
        // A recognizable import veneer with no current ABI is failed import
        // evidence, not an opaque native-body candidate.
        const auto *TargetSection = Image.getSectionFor(*Site->StaticTarget);
        if (Slot || (TargetSection &&
                     TargetSection->Name == section_names::macho::ObjCStubs))
          return {};
        const auto Machine =
            objcClassAccessorMachine(Image, *Site->StaticTarget);
        if (!Machine) {
          Calls.emplace(*Site,
                        SourceBooleanOtherCallContract{nullptr, false, true});
          continue;
        }
        const auto [It, Inserted] =
            ClassAccessors.emplace(*Site->StaticTarget, Machine->Signature);
        if (!Calls.emplace(*Site, SourceBooleanOtherCallContract{&It->second})
                 .second)
          return {};
        continue;
      }
      if (swift_boolean_projection_detail::fixedObjectMessage(Image, *Site,
                                                              Hint->second)) {
        SourceBooleanOtherCallContract Contract{&Hint->second.Signature};
        Contract.OverwritesObjCCommand = true;
        Calls.emplace(*Site, Contract);
        continue;
      }
      if (!Slot ||
          !((Hint->second.TargetAddress == *Slot &&
             swift_boolean_projection_detail::ordinaryRuntime(Hint->second)) ||
            swift_boolean_projection_detail::superInit(Image, *Site, *Slot,
                                                       Hint->second)) ||
          Hint->second.WeakImport || Hint->second.DoesNotReturn)
        return {};
      if (!Calls
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

/// A provisional native entry claims only a full x0 word as observable. The
/// native source inference must later establish the actual complete entry ABI;
/// publication reruns the LowIR proof against that bound ABI.
inline std::optional<SourceFunctionTypeHint>
provisionalNativeSwiftBooleanEntry(const BinaryImage &Image, va_t Entry) {
  SourceFunctionTypeHint Signature;
  Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Signature.ReturnType = NdType::makeInt(8, false);
  std::string Error;
  if (!assignDarwinScalarSourceABI(Signature, Arch::AArch64, Error) ||
      !swift_boolean_projection_detail::nativeEntry(Image, Entry, Signature))
    return std::nullopt;
  return Signature;
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
