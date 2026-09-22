#ifndef NEVERD_SDK_CAPI_OBJCSUPERGETTERSOURCES_H
#define NEVERD_SDK_CAPI_OBJCSUPERGETTERSOURCES_H

#include "../../loader/MachO/DarwinRuntimeImport.h"
#include "../../loader/MachO/MachOLocalFunction.h"
#include "../../loader/ObjC/ObjCClassAccessorMachine.h"
#include "../../pipeline/NativeSourcePreservation.h"
#include "ObjCSourceBindings.h"

#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/loader/ObjC/ObjCEncoding.h"
#include "neverd/loader/ObjC/ObjCSourceDeclarations.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/Support/Endian.h"

namespace neverd::sdk {

struct ObjCSuperGetterContract {
  va_t Entry = 0;
  va_t MetadataAccessor = 0;
  va_t CurrentClass = 0;
  va_t SuperDispatch = 0;
  std::string ClassName;
  SourceFunctionTypeHint MessageSignature;
  SourceFunctionTypeHint MetadataSignature;
  std::map<va_t, va_t> CallerSelectorSlots;
};

namespace objc_super_getter_detail {

inline std::optional<va_t> addSigned(va_t Base, int64_t Offset) {
  if ((Offset < 0 && Base < uint64_t(-Offset)) ||
      (Offset >= 0 && Base > InvalidVA - uint64_t(Offset)))
    return std::nullopt;
  return Offset < 0 ? Base - uint64_t(-Offset) : Base + uint64_t(Offset);
}

inline std::optional<va_t> page(uint32_t Word, va_t Address,
                                unsigned Register) {
  if ((Word & 0x9f00001f) != (0x90000000u | Register))
    return std::nullopt;
  const uint32_t Immediate =
      ((Word >> 29) & 3) | (((Word >> 5) & 0x7ffff) << 2);
  const int64_t Signed =
      Immediate & 0x100000 ? int64_t(Immediate) - 0x200000 : int64_t(Immediate);
  return addSigned(Address & ~va_t(4095), Signed * 4096);
}

inline std::optional<va_t> pageAddress(uint32_t First, uint32_t Second,
                                       va_t Address, unsigned Register) {
  const auto Page = page(First, Address, Register);
  if (!Page ||
      (Second & 0xffc003ff) != (0x91000000u | (Register << 5) | Register))
    return std::nullopt;
  return addSigned(*Page, (Second >> 10) & 4095);
}

inline std::optional<va_t> branch(uint32_t Word, va_t Address, bool Link) {
  if ((Word & 0xfc000000) != (Link ? 0x94000000u : 0x14000000u))
    return std::nullopt;
  const uint32_t Immediate = Word & 0x03ffffff;
  return addSigned(Address,
                   (Immediate & 0x02000000 ? int64_t(Immediate) - 0x04000000
                                           : int64_t(Immediate)) *
                       4);
}

inline const uint8_t *code(const BinaryImage &Image, va_t Address,
                           uint64_t Size) {
  const auto *Section = Image.getSectionFor(Address);
  const auto *Segment = Image.getSegmentFor(Address);
  if (!Section || !Segment || !Section->isExecutable() ||
      !(Section->Type & (llvm::MachO::S_ATTR_PURE_INSTRUCTIONS |
                         llvm::MachO::S_ATTR_SOME_INSTRUCTIONS)) ||
      !Segment->isExecutable() || Address > InvalidVA - Size ||
      !rangeInBounds(Address - Section->VA, Size, Section->FileSz) ||
      !rangeInBounds(Address - Segment->VA, Size, Segment->FileSz))
    return nullptr;
  return Image.readVA(Address, Size);
}

template <typename T>
inline const T *uniqueEntry(const std::vector<T> &Values, va_t Entry) {
  const T *Result = nullptr;
  for (const auto &Value : Values)
    if (Value.Entry == Entry) {
      if (Result)
        return nullptr;
      Result = &Value;
    }
  return Result;
}

inline const LowFunc *completeLow(const PipelineResult &Result, va_t Entry,
                                  unsigned Instructions) {
  const auto *Audit = uniqueEntry(Result.FunctionAudits, Entry);
  const auto *Low = uniqueEntry(Result.LowFuncs, Entry);
  if (!Audit || !Low ||
      Audit->Disposition != PipelineFunctionDisposition::Accepted ||
      !Audit->HasLowIR || !Audit->HasMedIR || !Audit->MedIRVerified ||
      Audit->DecodedInstructions != Instructions ||
      Audit->LiftedInstructions != Instructions ||
      !Audit->DecodeFailures.empty() ||
      !Audit->UnsupportedInstructions.empty() ||
      !Audit->TruncatedPaths.empty() || Low->Blocks.size() != 1)
    return nullptr;
  const auto &Block = Low->Blocks.front();
  if (Block.StartAddr != Entry || !Block.Preds.empty() ||
      !Block.Succs.empty() || !Block.ExceptionalPreds.empty() ||
      !Block.ExceptionalSuccs.empty() || Block.Ops.size() > 256)
    return nullptr;
  std::set<va_t> Addresses;
  for (const auto &Operation : Block.Ops) {
    if (Operation.Addr < Entry || Operation.Addr - Entry >= Instructions * 4 ||
        (Operation.Addr - Entry) % 4 || Operation.NumInputs > 6)
      return nullptr;
    Addresses.insert(Operation.Addr);
  }
  return Addresses.size() == Instructions ? Low : nullptr;
}

inline std::optional<va_t> runtimeSlot(const BinaryImage &Image, va_t Target,
                                       llvm::StringRef Name) {
  const auto *Bytes = code(Image, Target, 12);
  if (!Bytes)
    return std::nullopt;
  const auto First = llvm::support::endian::read32le(Bytes);
  const auto Second = llvm::support::endian::read32le(Bytes + 4);
  const auto Third = llvm::support::endian::read32le(Bytes + 8);
  const auto Page = page(First, Target, 16);
  if (!Page || (Second & 0xffc003ff) != 0xf9400210u || Third != 0xd61f0200u)
    return std::nullopt;
  const auto Slot = addSigned(*Page, ((Second >> 10) & 4095) * 8);
  const auto Import = Slot ? darwinRuntimeImport(Image, *Slot) : std::nullopt;
  const auto Bound =
      Slot ? Image.DyldBindSlots.find(*Slot) : Image.DyldBindSlots.end();
  if (!Slot || !Import || *Import != Name ||
      Bound == Image.DyldBindSlots.end() ||
      Bound->second.Module != "/usr/lib/libobjc.A.dylib")
    return std::nullopt;
  return Slot;
}

inline bool
callsRestore(const LowFunc &Low, va_t FirstSite, va_t FirstTarget,
             const SourceFunctionTypeHint &FirstSignature, va_t SecondSite = 0,
             va_t SecondTarget = 0,
             const SourceFunctionTypeHint *SecondSignature = nullptr) {
  NativeSourceCalls Calls;
  for (const auto &Op : Low.Blocks.front().Ops) {
    if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL)
      continue;
    const auto Key = nativeSourceCallKey(Op);
    if (!Key || Op.Opcode != NdOp::CALL || Op.NumInputs != 1 ||
        !Op.Inputs[0].isConst())
      return false;
    NativeSourceCallContract Contract;
    if (Op.Addr == FirstSite && Op.Inputs[0].Offset == FirstTarget)
      Contract.Signature = &FirstSignature;
    else if (SecondSignature && Op.Addr == SecondSite &&
             Op.Inputs[0].Offset == SecondTarget) {
      Contract.Signature = SecondSignature;
      Contract.ReadOnlyFrameParameters.emplace(0, 16);
    } else
      return false;
    if (!Calls.emplace(*Key, std::move(Contract)).second)
      return false;
  }
  return Calls.size() == (SecondSignature ? 2U : 1U) &&
         restoresNativeSourceState(Low, Arch::AArch64, Calls);
}

inline bool canonicalBoolGetter(const SourceFunctionTypeHint &Signature) {
  auto Expected = parseObjCMethodEncoding("value", "B16@0:8");
  std::string Error;
  if (!Expected || !assignDarwinObjCSourceABI(*Expected, Arch::AArch64, Error))
    return false;
  auto Normalized = Signature;
  Normalized.Origin = Expected->Origin;
  return equalSourceABIs(Normalized, *Expected);
}

struct ObjCClassAccessorContract {
  va_t Entry = 0;
  va_t ClassAddress = 0;
  SourceFunctionTypeHint Signature;
};

// The complete accessor overwrites x0 before its only call, independently
// proving that an incoming metadata request is unused. Both projections retain
// the accessor as a dependency; this proof does not establish source closure.
inline std::optional<ObjCClassAccessorContract>
validatedClassAccessor(const BinaryImage &Image, const PipelineResult &Result,
                       va_t Entry) {
  if (Result.SourceImage != &Image || Image.Format != BinaryFormat::MachO ||
      Image.Arch != Arch::AArch64 || Image.Bits != Bitness::Bits64 ||
      Image.IsRelocatable ||
      !objc::RuntimeData(Image).supportsPlainObjectPointers())
    return std::nullopt;
  const auto Machine = objcClassAccessorMachine(Image, Entry);
  const auto *AccessorLow = completeLow(Result, Entry, 8);
  const auto *AccessorHigh = uniqueEntry(Result.HighFuncs, Entry);
  if (!Machine || !AccessorLow || !AccessorHigh ||
      !AccessorHigh->SourceTypeHint || AccessorHigh->DoesNotReturn ||
      AccessorHigh->StructuredExceptionRegions ||
      AccessorHigh->UnstructuredExceptionRegions ||
      !AccessorHigh->Params.empty())
    return std::nullopt;
  const auto Classes = objc_binding_detail::classObjectIdentities(Image);
  const auto ClassIdentity = Classes.find(Machine->ClassAddress);
  if (ClassIdentity == Classes.end() ||
      ClassIdentity->second.Kind != SourceCallTypeHint::Kind::RuntimeClass)
    return std::nullopt;
  const auto &Signature = *AccessorHigh->SourceTypeHint;
  if (!Signature.Parameters.empty() || !Signature.ReturnType ||
      Signature.ReturnType->Size != 8 ||
      (Signature.ReturnType->Kind != NdTypeKind::Int &&
       Signature.ReturnType->Kind != NdTypeKind::Ptr) ||
      !equalSourceTypes(AccessorHigh->ReturnType, Signature.ReturnType))
    return std::nullopt;
  SourceFunctionTypeHint Expected;
  Expected.Origin = Signature.Origin;
  Expected.ReturnType = Signature.ReturnType;
  std::string Error;
  if (!assignDarwinScalarSourceABI(Expected, Arch::AArch64, Error) ||
      !equalSourceABIs(Signature, Expected) ||
      !callsRestore(*AccessorLow, Entry + 16, Machine->SelfTarget,
                    Machine->SelfCall.Signature))
    return std::nullopt;
  return ObjCClassAccessorContract{Entry, Machine->ClassAddress, Signature};
}

inline std::optional<ObjCSuperGetterContract>
prove(const BinaryImage &Image, const PipelineResult &Result, va_t Entry,
      va_t Root) {
  if (Result.SourceImage != &Image || Image.Format != BinaryFormat::MachO ||
      Image.Arch != Arch::AArch64 || Image.Bits != Bitness::Bits64 ||
      Image.IsRelocatable ||
      !objc::RuntimeData(Image).supportsPlainObjectPointers())
    return std::nullopt;
  const auto *Bytes = code(Image, Entry, 60);
  const auto *Low = completeLow(Result, Entry, 15);
  const auto *High = uniqueEntry(Result.HighFuncs, Entry);
  if (!Bytes || !Low || !High || High->DoesNotReturn ||
      High->StructuredExceptionRegions || High->UnstructuredExceptionRegions ||
      High->Params.size() != 3)
    return std::nullopt;
  const auto Word = [&](unsigned I) {
    return llvm::support::endian::read32le(Bytes + I * 4);
  };
  // Complete ARM64 compiler body: save frame and x19/x20, retain the selector
  // slot and receiver, call the class accessor, materialize objc_super, load
  // the slot exactly once, dispatch, restore every saved byte, and return.
  constexpr std::pair<unsigned, uint32_t> Fixed[] = {
      {0, 0xd100c3ff}, {1, 0xa9014ff4},  {2, 0xa9027bfd},  {3, 0x910083fd},
      {4, 0xaa0203f3}, {5, 0xaa0003f4},  {7, 0xa90003f4},  {8, 0xf9400261},
      {9, 0x910003e0}, {11, 0xa9427bfd}, {12, 0xa9414ff4}, {13, 0x9100c3ff},
      {14, 0xd65f03c0}};
  for (const auto &[Index, Bits] : Fixed)
    if (Word(Index) != Bits)
      return std::nullopt;
  const auto Accessor = branch(Word(6), Entry + 24, true);
  const auto Dispatch = branch(Word(10), Entry + 40, true);
  if (!Accessor || !Dispatch ||
      !runtimeSlot(Image, *Dispatch, "_objc_msgSendSuper2"))
    return std::nullopt;

  const auto AccessorContract =
      validatedClassAccessor(Image, Result, *Accessor);
  if (!AccessorContract)
    return std::nullopt;
  const auto ClassAddress = AccessorContract->ClassAddress;
  const auto Classes = objc_binding_detail::classObjectIdentities(Image);
  const auto ClassIdentity = Classes.find(ClassAddress);
  if (ClassIdentity == Classes.end())
    return std::nullopt;
  const ObjCClass *Class = nullptr;
  for (const auto &Candidate : Image.ObjCClasses)
    if (Candidate.Name == ClassIdentity->second.Name) {
      if (Class || Candidate.Address != ClassAddress || Candidate.RootClass ||
          Candidate.InheritanceStatus != "resolved" ||
          Candidate.SuperclassName.empty())
        return std::nullopt;
      Class = &Candidate;
    }
  if (!Class)
    return std::nullopt;
  const auto &MetadataSignature = AccessorContract->Signature;

  if (Root > InvalidVA - 8 || !isMachOLocalFunctionRange(Image, Entry, 60))
    return std::nullopt;
  ObjCSuperGetterContract Contract;
  Contract.Entry = Entry;
  Contract.MetadataAccessor = *Accessor;
  Contract.CurrentClass = ClassAddress;
  Contract.ClassName = Class->Name;
  Contract.SuperDispatch = *Dispatch;
  Contract.MetadataSignature = MetadataSignature;
  const va_t Caller = Root;
  const auto *CallerBytes = code(Image, Caller, 12);
  const auto *CallerLow = completeLow(Result, Caller, 3);
  const auto *CallerHigh = uniqueEntry(Result.HighFuncs, Caller);
  if (!CallerBytes ||
      branch(llvm::support::endian::read32le(CallerBytes + 8), Caller + 8,
             false) != std::optional<va_t>(Entry) ||
      !CallerLow || !CallerHigh || !CallerHigh->SourceTypeHint ||
      CallerHigh->DoesNotReturn || CallerHigh->Params.size() != 2 ||
      !canonicalBoolGetter(*CallerHigh->SourceTypeHint) ||
      !equalSourceTypes(CallerHigh->ReturnType,
                        CallerHigh->SourceTypeHint->ReturnType))
    return std::nullopt;
  const ObjCMethod *Method = nullptr;
  for (const auto &Candidate : Image.ObjCMethods)
    if (Candidate.Implementation == Caller) {
      if (Method || Candidate.Status != "supported" ||
          Candidate.IsClassMethod || !Candidate.TypeHint ||
          Candidate.ClassName != Class->Name)
        return std::nullopt;
      Method = &Candidate;
    }
  const auto MethodHint = objcMethodSourceTypeHint(Image, Caller);
  const auto Slot =
      pageAddress(llvm::support::endian::read32le(CallerBytes),
                  llvm::support::endian::read32le(CallerBytes + 4), Caller, 2);
  const auto Reference = Slot ? Image.ObjCSourceReferences.find(*Slot)
                              : Image.ObjCSourceReferences.end();
  if (!Method || !MethodHint || !canonicalBoolGetter(*MethodHint) ||
      !equalSourceABIs(*MethodHint, *CallerHigh->SourceTypeHint) || !Slot ||
      Reference == Image.ObjCSourceReferences.end() ||
      Reference->second.Address != *Slot || Reference->second.Size != 8 ||
      Reference->second.TheKind != ObjCSourceReference::Kind::Selector ||
      Reference->second.Name != Method->Selector)
    return std::nullopt;
  // Global agreement is stricter than superclass-only lookup: every known
  // declaration must supply this same ABI. No representative selector is
  // substituted into the dynamic dispatch in the generated helper.
  const auto Signature = objcSelectorSourceTypeHint(Image, Method->Selector);
  if (!Signature || !canonicalBoolGetter(*Signature))
    return std::nullopt;
  // Each declaration was authenticated independently above. The declaration
  // providers may differ, while every actual source/ABI field must agree.
  auto Normalized = *Signature;
  Normalized.Origin = SourceFunctionTypeHint::OriginKind::ObjCRuntime;
  Contract.MessageSignature = Normalized;
  Contract.CallerSelectorSlots.emplace(Caller, *Slot);
  if (!callsRestore(*Low, Entry + 24, *Accessor, MetadataSignature, Entry + 40,
                    *Dispatch, &Contract.MessageSignature))
    return std::nullopt;
  return Contract;
}

inline std::string helperName(va_t Entry) {
  return "neverd_objc_super_getter_" + llvm::utohexstr(Entry, true);
}

inline std::string selectorName(va_t Slot) {
  return "neverd_objc_selector_reference_" + llvm::utohexstr(Slot, true) +
         "_address";
}

inline SourceCallTypeHint addressHint(SourceCallTypeHint::Kind Kind,
                                      va_t Address, std::string Name) {
  SourceCallTypeHint Hint;
  Hint.CallKind = Kind;
  Hint.TargetAddress = Address;
  Hint.TargetName = std::move(Name);
  Hint.Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Hint.Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
  std::string Error;
  if (!assignDarwinScalarSourceABI(Hint.Signature, Arch::AArch64, Error))
    throw std::runtime_error("invalid super getter address ABI");
  return Hint;
}

inline SourceCallTypeHint getterHint(const ObjCSuperGetterContract &Contract) {
  auto Hint = addressHint(SourceCallTypeHint::Kind::RuntimeObjCSuperGetter,
                          Contract.Entry, helperName(Contract.Entry));
  const auto Pointer = Hint.Signature.ReturnType;
  Hint.Signature.ReturnType = Contract.MessageSignature.ReturnType;
  for (const auto Name : {"self", "command", "selector_slot", "metadata"})
    Hint.Signature.Parameters.push_back({Name, Pointer});
  std::string Error;
  if (!assignDarwinScalarSourceABI(Hint.Signature, Arch::AArch64, Error))
    throw std::runtime_error("invalid super getter source ABI");
  return Hint;
}

inline bool callMatches(const HighExpr &Expression,
                        const SourceCallTypeHint &Expected, va_t CallAddress) {
  if (Expression.Kind != ExprKind::Call || Expression.IsIndirectCall ||
      Expression.CallAddr != CallAddress || !Expression.CallTarget.empty() ||
      !Expression.SourceCallHint || Expression.IntrinsicId != Intrinsic::None ||
      !Expression.IntrinsicOutputs.empty() ||
      Expression.MemoryOrdering != NdMemoryOrdering::None ||
      Expression.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Expression.Operands.size() != Expected.Signature.Parameters.size() ||
      !equalSourceTypes(Expression.Type, Expected.Signature.ReturnType))
    return false;
  const auto &Actual = *Expression.SourceCallHint;
  auto Plain = Actual;
  Plain.CallKind = SourceCallTypeHint::Kind::Native;
  return objc_binding_detail::plainNativeBinding(Plain) &&
         Actual.CallKind == Expected.CallKind &&
         Actual.TargetAddress == Expected.TargetAddress &&
         Actual.TargetName == Expected.TargetName &&
         equalSourceABIs(Actual.Signature, Expected.Signature);
}

inline bool parameter(const HighExpr &Expression, unsigned Index,
                      const TypeRef &Type) {
  return Expression.Kind == ExprKind::Var &&
         Expression.Var.Kind == MedVar::Param &&
         Expression.Var.Id == int(Index) && Expression.Var.Size == 8 &&
         Expression.Var.SSAVer == 0 && Expression.Var.RegOff == Index * 8 &&
         Expression.Var.RenameTag == -1 && Expression.Operands.empty() &&
         equalSourceTypes(Expression.Type, Type) &&
         Expression.IntrinsicId == Intrinsic::None &&
         Expression.IntrinsicOutputs.empty() &&
         Expression.MemoryOrdering == NdMemoryOrdering::None &&
         Expression.MemoryAddressSpace == NdMemoryAddressSpace::Default;
}
} // namespace objc_super_getter_detail

struct ObjCSuperGetterSourcePlan {
  const PipelineResult *Pipeline = nullptr;
  std::map<va_t, va_t> Callers;
};

inline ObjCSuperGetterSourcePlan
discoverObjCSuperGetterSources(const BinaryImage &Image,
                               const PipelineResult &Result) {
  ObjCSuperGetterSourcePlan Plan;
  Plan.Pipeline = &Result;
  for (const auto &Method : Image.ObjCMethods) {
    const auto *Caller =
        objc_super_getter_detail::code(Image, Method.Implementation, 12);
    if (!Caller)
      continue;
    const auto Target = objc_super_getter_detail::branch(
        llvm::support::endian::read32le(Caller + 8), Method.Implementation + 8,
        false);
    const auto *Bytes =
        Target ? objc_super_getter_detail::code(Image, *Target, 60) : nullptr;
    if (!Bytes || llvm::support::endian::read32le(Bytes) != 0xd100c3ff ||
        llvm::support::endian::read32le(Bytes + 4) != 0xa9014ff4)
      continue;
    const auto Contract = objc_super_getter_detail::prove(
        Image, Result, *Target, Method.Implementation);
    if (Contract)
      Plan.Callers.emplace(Method.Implementation, *Target);
  }
  return Plan;
}

inline std::optional<ObjCSuperGetterContract>
validatedObjCSuperGetter(const BinaryImage &Image,
                         const ObjCSuperGetterSourcePlan &Plan, va_t Caller) {
  const auto Found = Plan.Callers.find(Caller);
  if (!Plan.Pipeline || Plan.Pipeline->SourceImage != &Image ||
      Found == Plan.Callers.end())
    return std::nullopt;
  const auto Contract = objc_super_getter_detail::prove(Image, *Plan.Pipeline,
                                                        Found->second, Caller);
  if (!Contract || !Contract->CallerSelectorSlots.count(Caller))
    return std::nullopt;
  return Contract;
}

struct ObjCSuperGetterSourceProjection {
  HighFunc Function;
  std::set<va_t> Dependencies;
  bool Projected = false;
};

inline ObjCSuperGetterSourceProjection
projectObjCSuperGetter(const HighFunc &Function, const BinaryImage &Image,
                       const ObjCSuperGetterSourcePlan &Plan) {
  ObjCSuperGetterSourceProjection Projection{Function, {}, false};
  const auto Contract = validatedObjCSuperGetter(Image, Plan, Function.Entry);
  if (!Contract || !Function.SourceTypeHint || Function.Params.size() != 2 ||
      !objc_super_getter_detail::canonicalBoolGetter(*Function.SourceTypeHint))
    return Projection;
  using namespace objc_super_getter_detail;
  const auto Hint = getterHint(*Contract);
  std::vector<ExprPtr> Arguments;
  for (unsigned I = 0; I < 2; ++I) {
    if (!equalSourceTypes(Function.Params[I].Type,
                          Hint.Signature.Parameters[I].Type))
      return Projection;
    MedVar Parameter;
    Parameter.Kind = MedVar::Param;
    Parameter.Id = I;
    Parameter.Size = 8;
    Parameter.RegOff = Hint.Signature.Parameters[I].Location.RegisterOffset;
    Parameter.TheArch = Arch::AArch64;
    Arguments.push_back(HighExpr::makeVar(Parameter, Function.Params[I].Type));
  }
  const va_t Slot = Contract->CallerSelectorSlots.at(Function.Entry);
  for (const auto &Address :
       {addressHint(SourceCallTypeHint::Kind::RuntimeSelectorReferenceAddress,
                    Slot, selectorName(Slot)),
        addressHint(SourceCallTypeHint::Kind::NativeAddress,
                    Contract->MetadataAccessor, {})}) {
    auto Call = HighExpr::makeCall({}, 0, {});
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(Address);
    Call->Type = Address.Signature.ReturnType;
    Arguments.push_back(std::move(Call));
  }
  auto Call = HighExpr::makeCall({}, Contract->Entry, std::move(Arguments));
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(Hint);
  Call->Type = Hint.Signature.ReturnType;
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.Addr = Function.Entry + 8;
  Return.RetVal = std::move(Call);
  Projection.Function.Body = {std::move(Return)};
  Projection.Function.Locals.clear();
  Projection.Dependencies.insert(Contract->MetadataAccessor);
  Projection.Projected = true;
  return Projection;
}

inline bool objCSuperGetterSourceCallBound(
    const HighExpr &Expression, const BinaryImage &Image,
    const ObjCSuperGetterSourcePlan &Plan, const HighFunc &Function,
    const std::map<va_t, const HighFunc *> &Functions) {
  const auto Contract = validatedObjCSuperGetter(Image, Plan, Function.Entry);
  if (!Contract || Function.Body.size() != 1 ||
      Function.Body.front().Kind != StmtKind::Return ||
      !Function.Body.front().RetVal || Function.Params.size() != 2)
    return false;
  using namespace objc_super_getter_detail;
  const auto &Call = *Function.Body.front().RetVal;
  const auto Hint = getterHint(*Contract);
  if (!callMatches(Call, Hint, Contract->Entry) ||
      std::any_of(Call.Operands.begin(), Call.Operands.end(),
                  [](const auto &Operand) { return !Operand; }))
    return false;
  for (unsigned I = 0; I < 2; ++I)
    if (!parameter(*Call.Operands[I], I, Hint.Signature.Parameters[I].Type))
      return false;
  const va_t Slot = Contract->CallerSelectorSlots.at(Function.Entry);
  if (!callMatches(
          *Call.Operands[2],
          addressHint(SourceCallTypeHint::Kind::RuntimeSelectorReferenceAddress,
                      Slot, selectorName(Slot)),
          0) ||
      !callMatches(*Call.Operands[3],
                   addressHint(SourceCallTypeHint::Kind::NativeAddress,
                               Contract->MetadataAccessor, {}),
                   0))
    return false;
  const auto Provider = Functions.find(Contract->MetadataAccessor);
  if (Provider == Functions.end() || !Provider->second ||
      !Provider->second->SourceTypeHint ||
      !equalSourceABIs(*Provider->second->SourceTypeHint,
                       Contract->MetadataSignature))
    return false;
  return &Expression == &Call || &Expression == Call.Operands[2].get() ||
         &Expression == Call.Operands[3].get();
}

inline std::string renderObjCSuperGetterHelpers(
    const BinaryImage &Image, const ObjCSuperGetterSourcePlan &Plan,
    const std::set<va_t> &Callers, std::set<std::string> &SharedFunctions) {
  std::map<va_t, ObjCSuperGetterContract> Contracts;
  std::map<va_t, std::string> Selectors;
  for (const auto Caller : Callers) {
    const auto Contract = validatedObjCSuperGetter(Image, Plan, Caller);
    if (!Contract)
      throw std::runtime_error(
          "super getter source contract is no longer valid");
    Contracts.emplace(Contract->Entry, *Contract);
    const va_t Slot = Contract->CallerSelectorSlots.at(Caller);
    Selectors.emplace(Slot, Image.ObjCSourceReferences.at(Slot).Name);
  }
  if (Contracts.empty())
    return {};
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  OS << "\n#include <objc/runtime.h>\n"
        "extern void objc_msgSendSuper2(void);\n";
  for (const auto &[Slot, Selector] : Selectors) {
    const auto Name = objc_super_getter_detail::selectorName(Slot);
    SharedFunctions.insert(Name);
    OS << "\nuintptr_t " << Name
       << "(void) {\n"
          "  static void *reference;\n  static unsigned state;\n"
          "  if (__atomic_load_n(&state, __ATOMIC_ACQUIRE) != 2) {\n"
          "    unsigned expected = 0;\n"
          "    if (__atomic_compare_exchange_n(&state, &expected, 1, 0, "
          "__ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {\n"
          "      reference = sel_registerName(\"";
    OS.write_escaped(Selector);
    OS << "\");\n      __atomic_store_n(&state, 2, __ATOMIC_RELEASE);\n"
          "    } else {\n"
          "      while (__atomic_load_n(&state, __ATOMIC_ACQUIRE) != 2) {}\n"
          "    }\n  }\n  return (uintptr_t)&reference;\n}\n";
  }
  for (const auto &[Entry, Contract] : Contracts) {
    const auto Name = objc_super_getter_detail::helperName(Entry);
    SharedFunctions.insert(Name);
    OS << "\n"
       << typeToC(Contract.MessageSignature.ReturnType) << " " << Name
       << "(void *self, void *command, void *selector_slot, void *metadata) {\n"
          "  struct { void *receiver; void *current_class; } super;\n"
          "  (void)command;\n"
          "  void *current_class = (void *)(uintptr_t)(("
       << typeToC(Contract.MetadataSignature.ReturnType)
       << " (*)(void))metadata)();\n"
          "  super.receiver = self;\n  super.current_class = current_class;\n"
          "  void *selector = *(void **)selector_slot;\n"
          "  return (("
       << typeToC(Contract.MessageSignature.ReturnType)
       << " (*)(void *, void *))objc_msgSendSuper2)(&super, selector);\n}\n";
  }
  return Source;
}
} // namespace neverd::sdk
#endif
