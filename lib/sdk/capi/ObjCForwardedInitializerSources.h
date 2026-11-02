#ifndef NEVERD_SDK_CAPI_OBJCFORWARDEDINITIALIZERSOURCES_H
#define NEVERD_SDK_CAPI_OBJCFORWARDEDINITIALIZERSOURCES_H

#include "ObjCSuperGetterSources.h"

#include "neverd/sdk/Limits.h"

#include <iterator>

namespace neverd::sdk {
namespace objc_forwarded_initializer_detail {
using namespace objc_super_getter_detail;

struct Contract {
  va_t Root = 0;
  va_t Entry = 0;
  va_t SelectorSlot = 0;
  va_t Dispatch = 0;
  ObjCClassAccessorContract Accessor;
  SourceFunctionTypeHint MessageSignature;
};

inline bool canonicalInitializer(const SourceFunctionTypeHint &Signature) {
  auto Expected = parseObjCMethodEncoding("init", "@16@0:8");
  std::string Error;
  if (!Expected || !assignDarwinObjCSourceABI(*Expected, Arch::AArch64, Error))
    return false;
  auto Normalized = Signature;
  Normalized.Origin = Expected->Origin;
  return equalSourceABIs(Normalized, *Expected);
}

// A zero-sized Mach-O symbol can make the native inventory decode the next
// disconnected local entry as another block of this function. Only the
// method's fully audited entry block is part of this tail-call wrapper.
inline const LowFunc *completeCallerLow(const PipelineResult &Result,
                                        va_t Root) {
  const auto *Audit = uniqueEntry(Result.FunctionAudits, Root);
  const auto *Low = uniqueEntry(Result.LowFuncs, Root);
  if (!Audit || !Low ||
      Audit->Disposition != PipelineFunctionDisposition::Accepted ||
      !Audit->HasLowIR || !Audit->HasMedIR || !Audit->MedIRVerified ||
      Audit->DecodedInstructions < 3 ||
      Audit->DecodedInstructions != Audit->LiftedInstructions ||
      !Audit->DecodeFailures.empty() ||
      !Audit->UnsupportedInstructions.empty() ||
      !Audit->TruncatedPaths.empty() || Low->Blocks.empty() ||
      Low->Blocks.size() >
          ::neverd::sdk::
              kObjCForwardedInitializerSourcesCompleteCallerLowLowBlocksSizeLimit)
    return nullptr;
  size_t Decoded = 0;
  const LowBlock *Entry = nullptr;
  for (const auto &Block : Low->Blocks) {
    Decoded += Block.InstructionBoundaries.size();
    if (!Block.Preds.empty() || !Block.Succs.empty() ||
        !Block.ExceptionalPreds.empty() || !Block.ExceptionalSuccs.empty() ||
        Block.Ops.size() >
            ::neverd::sdk::
                kObjCForwardedInitializerSourcesCompleteCallerLowBlockOpsSizeLimit)
      return nullptr;
    if (Block.StartAddr == Root) {
      if (Entry)
        return nullptr;
      Entry = &Block;
    } else {
      if (Block.StartAddr >= Root && Block.StartAddr - Root < 12)
        return nullptr;
      for (const auto &Op : Block.Ops)
        if (Op.Addr >= Root && Op.Addr - Root < 12)
          return nullptr;
    }
  }
  if (!Entry || Decoded != Audit->DecodedInstructions ||
      Entry->InstructionBoundaries.size() != 3)
    return nullptr;
  for (unsigned I = 0; I < 3; ++I)
    if (Entry->InstructionBoundaries[I].Address != Root + I * 4)
      return nullptr;
  for (const auto &Op : Entry->Ops)
    if (Op.Addr < Root || Op.Addr - Root >= 12 || (Op.Addr - Root) % 4 ||
        Op.NumInputs > std::size(Op.Inputs))
      return nullptr;
  return Low;
}

inline std::optional<Contract> prove(const BinaryImage &Image,
                                     const PipelineResult &Result, va_t Root) {
  if (Result.SourceImage != &Image || Image.Format != BinaryFormat::MachO ||
      Image.Arch != Arch::AArch64 || Image.Bits != Bitness::Bits64 ||
      Image.IsRelocatable || Root % 4 ||
      !objc::RuntimeData(Image).supportsPlainObjectPointers())
    return std::nullopt;
  const auto *Caller = code(Image, Root, 12);
  const auto *CallerLow = completeCallerLow(Result, Root);
  const auto *CallerHigh = uniqueEntry(Result.HighFuncs, Root);
  if (!Caller || !CallerLow || !CallerHigh || !CallerHigh->SourceTypeHint ||
      CallerHigh->DoesNotReturn || CallerHigh->StructuredExceptionRegions ||
      CallerHigh->UnstructuredExceptionRegions ||
      CallerHigh->Params.size() != 2 ||
      !canonicalInitializer(*CallerHigh->SourceTypeHint) ||
      !equalSourceTypes(CallerHigh->ReturnType,
                        CallerHigh->SourceTypeHint->ReturnType))
    return std::nullopt;
  const ObjCMethod *Method = nullptr;
  for (const auto &Candidate : Image.ObjCMethods)
    if (Candidate.Implementation == Root) {
      if (Method || Candidate.Status != "supported" ||
          Candidate.IsClassMethod || !Candidate.TypeHint ||
          Candidate.Selector != "init")
        return std::nullopt;
      Method = &Candidate;
    }
  const auto MethodHint = objcMethodSourceTypeHint(Image, Root);
  const auto AccessorAddress =
      pageAddress(llvm::support::endian::read32le(Caller),
                  llvm::support::endian::read32le(Caller + 4), Root, 2);
  const auto Entry =
      branch(llvm::support::endian::read32le(Caller + 8), Root + 8, false);
  if (!Method || !MethodHint || !canonicalInitializer(*MethodHint) ||
      !equalSourceABIs(*MethodHint, *CallerHigh->SourceTypeHint) ||
      !AccessorAddress || !Entry || *Entry % 4 ||
      !isMachOLocalFunctionRange(Image, *Entry, 60))
    return std::nullopt;
  const auto *Bytes = code(Image, *Entry, 60);
  const auto *Low = completeLow(Result, *Entry, 15);
  const auto *High = uniqueEntry(Result.HighFuncs, *Entry);
  if (!Bytes || !Low || !High || High->DoesNotReturn ||
      High->StructuredExceptionRegions || High->UnstructuredExceptionRegions ||
      High->Params.size() != 3)
    return std::nullopt;
  const auto Word = [&](unsigned I) {
    return llvm::support::endian::read32le(Bytes + I * 4);
  };
  // Entire shared compiler body: preserve the receiver, call the exact x2
  // accessor, construct objc_super, then load the selector and dispatch.
  constexpr std::pair<unsigned, uint32_t> Fixed[] = {
      {0, 0xd100c3ff},  {1, 0xa9014ff4},  {2, 0xa9027bfd},  {3, 0x910083fd},
      {4, 0xaa0003f3},  {5, 0xd63f0040},  {6, 0xa90003f3},  {9, 0x910003e0},
      {11, 0xa9427bfd}, {12, 0xa9414ff4}, {13, 0x9100c3ff}, {14, 0xd65f03c0}};
  for (const auto &[Index, Bits] : Fixed)
    if (Word(Index) != Bits)
      return std::nullopt;
  const auto Page = page(Word(7), *Entry + 28, 8);
  if (!Page || (Word(8) & 0xffc003ff) != 0xf9400101u)
    return std::nullopt;
  const auto Slot = addSigned(*Page, ((Word(8) >> 10) & 4095) * 8);
  const auto Dispatch = branch(Word(10), *Entry + 40, true);
  const auto Reference = Slot ? Image.ObjCSourceReferences.find(*Slot)
                              : Image.ObjCSourceReferences.end();
  if (!Slot || !Dispatch ||
      !runtimeSlot(Image, *Dispatch, "_objc_msgSendSuper2") ||
      Reference == Image.ObjCSourceReferences.end() ||
      Reference->second.Address != *Slot || Reference->second.Size != 8 ||
      Reference->second.TheKind != ObjCSourceReference::Kind::Selector ||
      Reference->second.Name != Method->Selector)
    return std::nullopt;
  const auto Accessor = validatedClassAccessor(Image, Result, *AccessorAddress);
  if (!Accessor)
    return std::nullopt;
  const auto Classes = objc_binding_detail::classObjectIdentities(Image);
  const auto Identity = Classes.find(Accessor->ClassAddress);
  if (Identity == Classes.end() ||
      Identity->second.Kind != SourceCallTypeHint::Kind::RuntimeClass ||
      Identity->second.Name != Method->ClassName ||
      Method->ClassAddress != Accessor->ClassAddress)
    return std::nullopt;
  const ObjCClass *Class = nullptr;
  for (const auto &Candidate : Image.ObjCClasses)
    if (Candidate.Name == Method->ClassName) {
      if (Class || Candidate.Address != Accessor->ClassAddress ||
          Candidate.RootClass || Candidate.InheritanceStatus != "resolved" ||
          Candidate.SuperclassName.empty())
        return std::nullopt;
      Class = &Candidate;
    }
  if (!Class)
    return std::nullopt;
  NativeSourceCalls Calls;
  for (const auto &Op : Low->Blocks.front().Ops) {
    if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL)
      continue;
    const auto Key = nativeSourceCallKey(Op);
    if (!Key)
      return std::nullopt;
    NativeSourceCallContract Call;
    if (Op.Addr == *Entry + 20 && Op.Opcode == NdOp::INDIR_CALL &&
        Op.NumInputs == 1 && Op.Inputs[0] == NdVar::reg(16, 8))
      Call.Signature = &Accessor->Signature;
    else if (Op.Addr == *Entry + 40 && Op.Opcode == NdOp::CALL &&
             Op.NumInputs == 1 && Op.Inputs[0].isConst() &&
             Op.Inputs[0].Offset == *Dispatch) {
      Call.Signature = &*MethodHint;
      Call.ReadOnlyFrameParameters.emplace(0, 16);
    } else
      return std::nullopt;
    if (!Calls.emplace(*Key, Call).second)
      return std::nullopt;
  }
  if (Calls.size() != 2 ||
      !restoresNativeSourceState(*Low, Arch::AArch64, Calls))
    return std::nullopt;
  return Contract{Root, *Entry, *Slot, *Dispatch, *Accessor, *MethodHint};
}

inline std::string helperName(va_t Entry) {
  return "neverd_objc_forwarded_initializer_" + llvm::utohexstr(Entry, true);
}

inline SourceCallTypeHint initializerHint(const Contract &C) {
  auto Hint =
      addressHint(SourceCallTypeHint::Kind::RuntimeObjCForwardedInitializer,
                  C.Entry, helperName(C.Entry));
  const auto Pointer = Hint.Signature.ReturnType;
  for (const auto Name : {"self", "command", "selector_slot", "metadata"})
    Hint.Signature.Parameters.push_back({Name, Pointer});
  std::string Error;
  if (!assignDarwinScalarSourceABI(Hint.Signature, Arch::AArch64, Error))
    throw std::runtime_error("invalid forwarded initializer source ABI");
  return Hint;
}
} // namespace objc_forwarded_initializer_detail

// A class accessor is an independently complete compiler function. Its exact
// machine body and class identity justify a provisional scalar source ABI
// before the pipeline sees an address-taken callback. Publication still needs
// the audited caller, shared body and accessor proof below.
inline size_t
seedObjCForwardedInitializerAccessorHints(const BinaryImage &Image,
                                          PipelineOptions &Options) {
  if (Image.Format != BinaryFormat::MachO || Image.Arch != Arch::AArch64 ||
      Image.Bits != Bitness::Bits64 || Image.IsRelocatable ||
      !objc::RuntimeData(Image).supportsPlainObjectPointers())
    return 0;
  const auto Classes = objc_binding_detail::classObjectIdentities(Image);
  SourceFunctionTypeHint Signature;
  Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
  std::string Error;
  if (!assignDarwinScalarSourceABI(Signature, Arch::AArch64, Error))
    return 0;
  size_t Added = 0;
  for (const auto &Method : Image.ObjCMethods) {
    if (Method.Status != "supported" || Method.IsClassMethod ||
        Method.Selector != "init" || !Method.TypeHint ||
        !objc_forwarded_initializer_detail::canonicalInitializer(
            *Method.TypeHint))
      continue;
    const auto *Caller =
        objc_super_getter_detail::code(Image, Method.Implementation, 12);
    if (!Caller)
      continue;
    using namespace objc_super_getter_detail;
    const auto Accessor = pageAddress(
        llvm::support::endian::read32le(Caller),
        llvm::support::endian::read32le(Caller + 4), Method.Implementation, 2);
    const auto Entry = branch(llvm::support::endian::read32le(Caller + 8),
                              Method.Implementation + 8, false);
    if (!Accessor || !Entry || !isMachOLocalFunctionRange(Image, *Entry, 60))
      continue;
    const auto Machine = objcClassAccessorMachine(Image, *Accessor);
    const auto Identity =
        Machine ? Classes.find(Machine->ClassAddress) : Classes.end();
    if (!Machine || Identity == Classes.end() ||
        Identity->second.Kind != SourceCallTypeHint::Kind::RuntimeClass ||
        Identity->second.Name != Method.ClassName ||
        Machine->ClassAddress != Method.ClassAddress)
      continue;
    Added += Options.SourceTypeHints.emplace(*Accessor, Signature).second;
  }
  return Added;
}

struct ObjCForwardedInitializerSourcePlan {
  const PipelineResult *Pipeline = nullptr;
  std::set<va_t> Callers;
};

inline ObjCForwardedInitializerSourcePlan
discoverObjCForwardedInitializerSources(const BinaryImage &Image,
                                        const PipelineResult &Result) {
  ObjCForwardedInitializerSourcePlan Plan{&Result, {}};
  for (const auto &Method : Image.ObjCMethods) {
    if (Method.Selector != "init")
      continue;
    const auto *Bytes = Image.readVA(Method.Implementation, 4);
    if (!Bytes ||
        (llvm::support::endian::read32le(Bytes) & 0x9f00001f) != 0x90000002u)
      continue;
    if (objc_forwarded_initializer_detail::prove(Image, Result,
                                                 Method.Implementation))
      Plan.Callers.insert(Method.Implementation);
  }
  return Plan;
}

inline std::optional<objc_forwarded_initializer_detail::Contract>
validatedObjCForwardedInitializer(
    const BinaryImage &Image, const ObjCForwardedInitializerSourcePlan &Plan,
    va_t Root) {
  if (!Plan.Pipeline || !Plan.Callers.count(Root))
    return std::nullopt;
  return objc_forwarded_initializer_detail::prove(Image, *Plan.Pipeline, Root);
}

struct ObjCForwardedInitializerSourceProjection {
  HighFunc Function;
  std::set<va_t> Dependencies;
  bool Projected = false;
};

inline ObjCForwardedInitializerSourceProjection projectObjCForwardedInitializer(
    const HighFunc &Function, const BinaryImage &Image,
    const ObjCForwardedInitializerSourcePlan &Plan) {
  ObjCForwardedInitializerSourceProjection Projection{Function, {}, false};
  const auto C = validatedObjCForwardedInitializer(Image, Plan, Function.Entry);
  if (!C || !Function.SourceTypeHint || Function.Params.size() != 2 ||
      !objc_forwarded_initializer_detail::canonicalInitializer(
          *Function.SourceTypeHint))
    return Projection;
  using namespace objc_forwarded_initializer_detail;
  const auto Hint = initializerHint(*C);
  std::vector<ExprPtr> Arguments;
  for (unsigned I = 0; I < 2; ++I) {
    if (!equalSourceTypes(Function.Params[I].Type,
                          Hint.Signature.Parameters[I].Type))
      return Projection;
    MedVar Param;
    Param.Kind = MedVar::Param;
    Param.Id = I;
    Param.Size = 8;
    Param.RegOff = Hint.Signature.Parameters[I].Location.RegisterOffset;
    Param.TheArch = Arch::AArch64;
    Arguments.push_back(HighExpr::makeVar(Param, Function.Params[I].Type));
  }
  for (const auto &Address :
       {addressHint(SourceCallTypeHint::Kind::RuntimeSelectorReferenceAddress,
                    C->SelectorSlot, selectorName(C->SelectorSlot)),
        addressHint(SourceCallTypeHint::Kind::NativeAddress, C->Accessor.Entry,
                    {})}) {
    auto Call = HighExpr::makeCall({}, 0, {});
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(Address);
    Call->Type = Address.Signature.ReturnType;
    Arguments.push_back(std::move(Call));
  }
  auto Call = HighExpr::makeCall({}, C->Entry, std::move(Arguments));
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(Hint);
  Call->Type = Hint.Signature.ReturnType;
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.Addr = Function.Entry + 8;
  Return.RetVal = std::move(Call);
  Projection.Function.Body = {std::move(Return)};
  Projection.Function.Locals.clear();
  Projection.Dependencies.insert(C->Accessor.Entry);
  Projection.Projected = true;
  return Projection;
}

inline bool objCForwardedInitializerSourceCallBound(
    const HighExpr &Expression, const BinaryImage &Image,
    const ObjCForwardedInitializerSourcePlan &Plan, const HighFunc &Function,
    const std::map<va_t, const HighFunc *> &Functions) {
  const auto C = validatedObjCForwardedInitializer(Image, Plan, Function.Entry);
  if (!C || Function.Body.size() != 1 ||
      Function.Body.front().Kind != StmtKind::Return ||
      !Function.Body.front().RetVal || Function.Params.size() != 2)
    return false;
  using namespace objc_forwarded_initializer_detail;
  const auto &Call = *Function.Body.front().RetVal;
  const auto Hint = initializerHint(*C);
  if (!callMatches(Call, Hint, C->Entry) ||
      std::any_of(Call.Operands.begin(), Call.Operands.end(),
                  [](const auto &Operand) { return !Operand; }))
    return false;
  for (unsigned I = 0; I < 2; ++I)
    if (!parameter(*Call.Operands[I], I, Hint.Signature.Parameters[I].Type))
      return false;
  if (!callMatches(
          *Call.Operands[2],
          addressHint(SourceCallTypeHint::Kind::RuntimeSelectorReferenceAddress,
                      C->SelectorSlot, selectorName(C->SelectorSlot)),
          0) ||
      !callMatches(*Call.Operands[3],
                   addressHint(SourceCallTypeHint::Kind::NativeAddress,
                               C->Accessor.Entry, {}),
                   0))
    return false;
  const auto Provider = Functions.find(C->Accessor.Entry);
  if (Provider == Functions.end() || !Provider->second ||
      !Provider->second->SourceTypeHint ||
      !equalSourceABIs(*Provider->second->SourceTypeHint,
                       C->Accessor.Signature))
    return false;
  return &Expression == &Call || &Expression == Call.Operands[2].get() ||
         &Expression == Call.Operands[3].get();
}

inline std::string renderObjCForwardedInitializerHelpers(
    const BinaryImage &Image, const ObjCForwardedInitializerSourcePlan &Plan,
    const std::set<va_t> &Callers, std::set<std::string> &SharedFunctions) {
  std::map<va_t, objc_forwarded_initializer_detail::Contract> Contracts;
  std::map<va_t, std::string> Selectors;
  for (const auto Caller : Callers) {
    const auto C = validatedObjCForwardedInitializer(Image, Plan, Caller);
    if (!C)
      throw std::runtime_error(
          "forwarded initializer source contract is no longer valid");
    Contracts.emplace(C->Entry, *C);
    Selectors.emplace(C->SelectorSlot,
                      Image.ObjCSourceReferences.at(C->SelectorSlot).Name);
  }
  if (Contracts.empty())
    return {};
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  OS << "\n#include <objc/runtime.h>\n"
        "extern void objc_msgSendSuper2(void);\n";
  OS << renderObjCSelectorReferenceHelpers(Selectors, SharedFunctions);
  for (const auto &[Entry, C] : Contracts) {
    const auto Name = objc_forwarded_initializer_detail::helperName(Entry);
    SharedFunctions.insert(Name);
    OS << "\nvoid *" << Name
       << "(void *self, void *command, void *selector_slot, void *metadata) {\n"
          "  struct { void *receiver; void *current_class; } super;\n"
          "  (void)command;\n"
          "  void *current_class = (void *)(uintptr_t)(("
       << typeToC(C.Accessor.Signature.ReturnType)
       << " (*)(void))metadata)();\n"
          "  super.receiver = self;\n  super.current_class = current_class;\n"
          "  void *selector = *(void **)selector_slot;\n"
          "  return ((void *(*)(void *, void *))objc_msgSendSuper2)(&super, "
          "selector);\n}\n";
  }
  return Source;
}
} // namespace neverd::sdk

#endif
