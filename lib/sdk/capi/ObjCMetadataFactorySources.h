#ifndef NEVERD_SDK_CAPI_OBJCMETADATAFACTORYSOURCES_H
#define NEVERD_SDK_CAPI_OBJCMETADATAFACTORYSOURCES_H

#include "ObjCSuperGetterSources.h"

namespace neverd::sdk {
namespace objc_metadata_factory_detail {
using namespace objc_super_getter_detail;

struct Contract {
  va_t Root = 0;
  va_t Entry = 0;
  va_t Counter = 0;
  va_t ProfileBase = 0;
  ObjCClassAccessorContract Accessor;
  va_t ConversionTarget = 0;
  SourceCallTypeHint Conversion;
};

inline bool canonicalClassGetter(const SourceFunctionTypeHint &Signature) {
  auto Expected = parseObjCMethodEncoding("value", "#16@0:8");
  std::string Error;
  if (!Expected || !assignDarwinObjCSourceABI(*Expected, Arch::AArch64, Error))
    return false;
  auto Normalized = Signature;
  Normalized.Origin = Expected->Origin;
  return equalSourceABIs(Normalized, *Expected);
}

inline std::optional<Contract> prove(const BinaryImage &Image,
                                     const PipelineResult &Result,
                                     const ObjCProfileStorage &Storage,
                                     va_t Root) {
  if (Result.SourceImage != &Image || Image.Format != BinaryFormat::MachO ||
      Image.Arch != Arch::AArch64 || Image.Bits != Bitness::Bits64 ||
      Image.IsRelocatable || Root % 4 ||
      !objc::RuntimeData(Image).supportsPlainObjectPointers())
    return std::nullopt;
  const auto CallerBytes = readImmutableCodeBytes(Image, Root, 20);
  const auto *CallerLow = completeLow(Result, Root, 5);
  const auto *CallerHigh = uniqueEntry(Result.HighFuncs, Root);
  if (!CallerBytes || !CallerLow || !CallerHigh ||
      !CallerHigh->SourceTypeHint || CallerHigh->DoesNotReturn ||
      CallerHigh->StructuredExceptionRegions ||
      CallerHigh->UnstructuredExceptionRegions ||
      CallerHigh->Params.size() != 2 ||
      !canonicalClassGetter(*CallerHigh->SourceTypeHint) ||
      !equalSourceTypes(CallerHigh->ReturnType,
                        CallerHigh->SourceTypeHint->ReturnType))
    return std::nullopt;
  const ObjCMethod *Method = nullptr;
  for (const auto &Candidate : Image.ObjCMethods)
    if (Candidate.Implementation == Root) {
      if (Method || Candidate.Status != "supported" ||
          !Candidate.IsClassMethod || !Candidate.TypeHint)
        return std::nullopt;
      Method = &Candidate;
    }
  const auto MethodHint = objcMethodSourceTypeHint(Image, Root);
  if (!Method || !MethodHint || !canonicalClassGetter(*MethodHint) ||
      !equalSourceABIs(*MethodHint, *CallerHigh->SourceTypeHint))
    return std::nullopt;
  const auto CallerWord = [&](unsigned I) {
    return llvm::support::endian::read32le(CallerBytes->data() + I * 4);
  };
  const auto Counter = pageAddress(CallerWord(0), CallerWord(1), Root, 2);
  const auto Metadata = pageAddress(CallerWord(2), CallerWord(3), Root + 8, 3);
  const auto Entry = branch(CallerWord(4), Root + 16, false);
  const auto Base = Counter && *Counter % 8 == 0
                        ? Storage.sectionFor(*Counter, 8)
                        : std::nullopt;
  if (!Counter || !Metadata || !Entry || !Base || *Entry % 4)
    return std::nullopt;
  const auto Bytes = readImmutableCodeBytes(Image, *Entry, 36);
  const auto *Low = completeLow(Result, *Entry, 9);
  const auto *High = uniqueEntry(Result.HighFuncs, *Entry);
  if (!Bytes || !Low || !High || High->DoesNotReturn ||
      High->StructuredExceptionRegions || High->UnstructuredExceptionRegions)
    return std::nullopt;
  const auto Word = [&](unsigned I) {
    return llvm::support::endian::read32le(Bytes->data() + I * 4);
  };
  // The complete body reads no original receiver/context. x3 is not changed
  // before its one indirect call; its target comes from this caller's exact
  // two-instruction address materialization, not from a global callee hint.
  constexpr uint32_t Fixed[] = {0xa9bf7bfd, 0x910003fd, 0xf9400048, 0x91000508,
                                0xf9000048, 0xd2800000, 0xd63f0060, 0xa8c17bfd};
  for (unsigned I = 0; I < std::size(Fixed); ++I)
    if (Word(I) != Fixed[I])
      return std::nullopt;
  const auto Accessor = validatedClassAccessor(Image, Result, *Metadata);
  const auto ConversionTarget = branch(Word(8), *Entry + 32, false);
  const auto Stub = ConversionTarget
                        ? readImmutableCodeBytes(Image, *ConversionTarget, 12)
                        : std::nullopt;
  if (!Accessor || !ConversionTarget || !Stub)
    return std::nullopt;
  const auto First = llvm::support::endian::read32le(Stub->data());
  const auto Second = llvm::support::endian::read32le(Stub->data() + 4);
  const auto Third = llvm::support::endian::read32le(Stub->data() + 8);
  const auto Page = page(First, *ConversionTarget, 16);
  const auto Slot =
      Page && (Second & 0xffc003ff) == 0xf9400210u && Third == 0xd61f0200u
          ? addSigned(*Page, ((Second >> 10) & 4095) * 8)
          : std::nullopt;
  const auto Conversion =
      Slot ? swiftRuntimeSourceCallHint(Image, *Slot) : std::nullopt;
  const auto Bind =
      Slot ? Image.DyldBindSlots.find(*Slot) : Image.DyldBindSlots.end();
  // The runtime catalog supplies the ABI; this specialization also requires
  // the current image's strong, exact provider rather than its spelling alone.
  if (Bind == Image.DyldBindSlots.end() ||
      Bind->second.Module != "/usr/lib/swift/libswiftCore.dylib" ||
      !Conversion ||
      Conversion->TargetName != "swift_getObjCClassFromMetadata" ||
      Conversion->DoesNotReturn)
    return std::nullopt;
  NativeSourceCalls Calls;
  for (const auto &Op : Low->Blocks.front().Ops) {
    if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL)
      continue;
    const auto Key = nativeSourceCallKey(Op);
    if (!Key)
      return std::nullopt;
    NativeSourceCallContract Call;
    if (Op.Addr == *Entry + 24 && Op.Opcode == NdOp::INDIR_CALL &&
        Op.Inputs[0] == NdVar::reg(24, 8))
      Call.Signature = &Accessor->Signature;
    else if (Op.Addr == *Entry + 32 && Op.Opcode == NdOp::CALL &&
             Op.Inputs[0].isConst() && Op.Inputs[0].Offset == *ConversionTarget)
      Call.Signature = &Conversion->Signature;
    else
      return std::nullopt;
    if (!Calls.emplace(*Key, Call).second)
      return std::nullopt;
  }
  if (Calls.size() != 2 ||
      !restoresNativeSourceState(*Low, Arch::AArch64, Calls))
    return std::nullopt;
  return Contract{
      Root, *Entry, *Counter, *Base, *Accessor, *ConversionTarget, *Conversion};
}

inline std::string helperName(va_t Root) {
  return "neverd_objc_metadata_factory_" + llvm::utohexstr(Root, true);
}

inline SourceCallTypeHint factoryHint(const Contract &C) {
  auto Hint = addressHint(SourceCallTypeHint::Kind::RuntimeObjCMetadataFactory,
                          C.Root, helperName(C.Root));
  for (const auto Name : {"storage", "metadata"})
    Hint.Signature.Parameters.push_back({Name, Hint.Signature.ReturnType});
  std::string Error;
  if (!assignDarwinScalarSourceABI(Hint.Signature, Arch::AArch64, Error))
    throw std::runtime_error("invalid metadata factory source ABI");
  return Hint;
}
} // namespace objc_metadata_factory_detail

struct ObjCMetadataFactorySourcePlan {
  const PipelineResult *Pipeline = nullptr;
  std::set<va_t> Callers;
};

inline ObjCMetadataFactorySourcePlan
discoverObjCMetadataFactorySources(const BinaryImage &Image,
                                   const PipelineResult &Result,
                                   const ObjCProfileStorage &Storage) {
  ObjCMetadataFactorySourcePlan Plan{&Result, {}};
  for (const auto &Method : Image.ObjCMethods) {
    // Cheap negative filter before full immutable-range and audit validation.
    const auto *Bytes = Image.readVA(Method.Implementation, 4);
    if (!Bytes ||
        (llvm::support::endian::read32le(Bytes) & 0x9f00001f) != 0x90000002u)
      continue;
    if (objc_metadata_factory_detail::prove(Image, Result, Storage,
                                            Method.Implementation))
      Plan.Callers.insert(Method.Implementation);
  }
  return Plan;
}

inline std::optional<objc_metadata_factory_detail::Contract>
validatedObjCMetadataFactory(const BinaryImage &Image,
                             const ObjCMetadataFactorySourcePlan &Plan,
                             const ObjCProfileStorage &Storage, va_t Root) {
  if (!Plan.Pipeline || !Plan.Callers.count(Root))
    return std::nullopt;
  return objc_metadata_factory_detail::prove(Image, *Plan.Pipeline, Storage,
                                             Root);
}

struct ObjCMetadataFactorySourceProjection {
  HighFunc Function;
  std::set<va_t> Dependencies;
  std::set<va_t> ProfileSections;
  bool Projected = false;
};

inline ObjCMetadataFactorySourceProjection
projectObjCMetadataFactory(const HighFunc &Function, const BinaryImage &Image,
                           const ObjCMetadataFactorySourcePlan &Plan,
                           const ObjCProfileStorage &Storage) {
  ObjCMetadataFactorySourceProjection Projection{Function, {}, {}, false};
  const auto C =
      validatedObjCMetadataFactory(Image, Plan, Storage, Function.Entry);
  if (!C || !Function.SourceTypeHint || Function.Params.size() != 2 ||
      !objc_metadata_factory_detail::canonicalClassGetter(
          *Function.SourceTypeHint))
    return Projection;
  using namespace objc_metadata_factory_detail;
  const auto Profile =
      objc_binding_detail::profileStorageHint(Image.Arch, C->ProfileBase);
  if (!Profile)
    return Projection;
  std::vector<ExprPtr> Arguments;
  for (const auto &Address :
       {*Profile, addressHint(SourceCallTypeHint::Kind::NativeAddress,
                              C->Accessor.Entry, {})}) {
    auto Call = HighExpr::makeCall({}, 0, {});
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(Address);
    Call->Type = Address.Signature.ReturnType;
    Arguments.push_back(std::move(Call));
  }
  const auto Hint = factoryHint(*C);
  auto Call = HighExpr::makeCall({}, C->Root, std::move(Arguments));
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(Hint);
  Call->Type = Hint.Signature.ReturnType;
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.Addr = Function.Entry + 16;
  Return.RetVal = std::move(Call);
  Projection.Function.Body = {std::move(Return)};
  Projection.Function.Locals.clear();
  Projection.Dependencies.insert(C->Accessor.Entry);
  Projection.ProfileSections.insert(C->ProfileBase);
  Projection.Projected = true;
  return Projection;
}

inline bool objCMetadataFactorySourceCallBound(
    const HighExpr &Expression, const BinaryImage &Image,
    const ObjCMetadataFactorySourcePlan &Plan,
    const ObjCProfileStorage &Storage, const HighFunc &Function,
    const std::map<va_t, const HighFunc *> &Functions) {
  const auto C =
      validatedObjCMetadataFactory(Image, Plan, Storage, Function.Entry);
  if (!C || !Function.SourceTypeHint ||
      !objc_metadata_factory_detail::canonicalClassGetter(
          *Function.SourceTypeHint) ||
      Function.Params.size() != 2 || Function.Body.size() != 1 ||
      Function.Body.front().Kind != StmtKind::Return ||
      !Function.Body.front().RetVal)
    return false;
  using namespace objc_metadata_factory_detail;
  const auto &Call = *Function.Body.front().RetVal;
  const auto Profile =
      objc_binding_detail::profileStorageHint(Image.Arch, C->ProfileBase);
  if (!Profile || !callMatches(Call, factoryHint(*C), C->Root) ||
      !Call.Operands[0] || !Call.Operands[1] ||
      !callMatches(*Call.Operands[0], *Profile, 0) ||
      !callMatches(*Call.Operands[1],
                   addressHint(SourceCallTypeHint::Kind::NativeAddress,
                               C->Accessor.Entry, {}),
                   0))
    return false;
  const auto Provider = Functions.find(C->Accessor.Entry);
  if (Provider == Functions.end() || !Provider->second ||
      Provider->second->Entry != C->Accessor.Entry ||
      !Provider->second->SourceTypeHint ||
      !equalSourceABIs(*Provider->second->SourceTypeHint,
                       C->Accessor.Signature))
    return false;
  return &Expression == &Call || &Expression == Call.Operands[0].get() ||
         &Expression == Call.Operands[1].get();
}

inline std::string renderObjCMetadataFactoryHelpers(
    const BinaryImage &Image, const ObjCMetadataFactorySourcePlan &Plan,
    const ObjCProfileStorage &Storage, const std::set<va_t> &Callers,
    std::set<std::string> &SharedFunctions) {
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  if (!Callers.empty())
    OS << "\nextern void *swift_getObjCClassFromMetadata(void *);\n";
  for (const auto Root : Callers) {
    const auto C = validatedObjCMetadataFactory(Image, Plan, Storage, Root);
    if (!C)
      throw std::runtime_error(
          "metadata factory source contract is no longer valid");
    const auto Name = objc_metadata_factory_detail::helperName(Root);
    SharedFunctions.insert(Name);
    OS << "\nvoid *" << Name
       << "(void *storage, void *metadata) {\n"
          "  unsigned char *counter = (unsigned char *)storage + "
       << C->Counter - C->ProfileBase
       << ";\n"
          "  uint64_t value;\n  __builtin_memcpy(&value, counter, 8);\n"
          "  value += UINT64_C(1);\n  __builtin_memcpy(counter, &value, 8);\n"
          "  void *result = (void *)(uintptr_t)(("
       << typeToC(C->Accessor.Signature.ReturnType)
       << " (*)(void))metadata)();\n"
          "  return swift_getObjCClassFromMetadata(result);\n}\n";
  }
  return Source;
}
} // namespace neverd::sdk
#endif
