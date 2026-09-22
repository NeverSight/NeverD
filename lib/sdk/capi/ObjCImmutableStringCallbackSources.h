#ifndef NEVERD_SDK_CAPI_OBJCIMMUTABLESTRINGCALLBACKSOURCES_H
#define NEVERD_SDK_CAPI_OBJCIMMUTABLESTRINGCALLBACKSOURCES_H

#include "ObjCSuperGetterSources.h"
#include "ObjCSwiftOnceSources.h"

#include "neverd/loader/MachO/DarwinImportVeneer.h"

namespace neverd::sdk {
namespace objc_immutable_string_callback_detail {
using namespace objc_super_getter_detail;

struct Contract {
  va_t Root = 0, Shared = 0, Provider = 0, Pair = 0, Destination = 0;
  va_t RetainTarget = 0;
  uint64_t Word = 0;
  SwiftLiteralString Literal;
  SourceCallTypeHint Storage, Pool, Retain;
};

// This is a caller-specific specialization of one complete machine path. The
// shared helper and addressor acquire no general native source declaration.
inline std::optional<Contract> prove(const BinaryImage &Image,
                                     const PipelineResult &Result,
                                     const SwiftOnceSourcePlan &Once,
                                     va_t Root) {
  if (Result.SourceImage != &Image || Image.Format != BinaryFormat::MachO ||
      Image.Arch != Arch::AArch64 || Image.Bits != Bitness::Bits64 ||
      Image.IsRelocatable || Root % 4 || Image.MachOChainedFixupsAmbiguous)
    return std::nullopt;
  const auto Callback = Once.CallbackHints.find(Root);
  const auto *High = uniqueEntry(Result.HighFuncs, Root);
  const auto Bytes = readImmutableCodeBytes(Image, Root, 24);
  if (Callback == Once.CallbackHints.end() || !High || !High->SourceTypeHint ||
      !equalSourceABIs(Callback->second,
                       swift_once_source_detail::callbackHint(Image.Arch)) ||
      !equalSourceABIs(*High->SourceTypeHint, Callback->second) || !Bytes ||
      !completeLow(Result, Root, 6))
    return std::nullopt;
  const auto WordAt = [&](unsigned I) {
    return llvm::support::endian::read32le(Bytes->data() + I * 4);
  };
  const auto Destination = pageAddress(WordAt(0), WordAt(1), Root, 2);
  const auto Provider = pageAddress(WordAt(2), WordAt(3), Root + 8, 1);
  const auto Shared = branch(WordAt(5), Root + 20, false);
  if (!Destination || !Provider || !Shared || WordAt(4) != 0x91002043u ||
      *Destination % 8 || *Provider % 4 || *Shared % 4)
    return std::nullopt;
  const auto Storage =
      objc_binding_detail::localStorageHint(Image, *Destination, 16);
  const auto SharedBytes = readImmutableCodeBytes(Image, *Shared, 48);
  const auto ProviderBytes = readImmutableCodeBytes(Image, *Provider, 12);
  const auto *Low = completeLow(Result, *Shared, 12);
  if (!Storage || !SharedBytes || !ProviderBytes || !Low ||
      !completeLow(Result, *Provider, 3))
    return std::nullopt;
  for (const auto Entry : {Root, *Shared, *Provider}) {
    const auto *F = uniqueEntry(Result.HighFuncs, Entry);
    if (!F || F->DoesNotReturn ||
        (F->ExceptionMetadata &&
         !objc_projection_detail::isPlainUnwind(*F->ExceptionMetadata)) ||
        F->StructuredExceptionRegions || F->UnstructuredExceptionRegions)
      return std::nullopt;
  }
  const auto SharedWord = [&](unsigned I) {
    return llvm::support::endian::read32le(SharedBytes->data() + I * 4);
  };
  constexpr uint32_t Fixed[] = {0xa9be4ff4, 0xa9017bfd, 0x910043fd, 0xaa0303f3,
                                0xaa0203f4, 0xd63f0020, 0xa9400008, 0xf9000288,
                                0xf9000260, 0xa9417bfd, 0xa8c24ff4};
  for (unsigned I = 0; I < std::size(Fixed); ++I)
    if (SharedWord(I) != Fixed[I])
      return std::nullopt;
  const auto Pair = pageAddress(
      llvm::support::endian::read32le(ProviderBytes->data()),
      llvm::support::endian::read32le(ProviderBytes->data() + 4), *Provider, 0);
  if (!Pair || *Pair % 8 || *Pair > InvalidVA - 16 ||
      llvm::support::endian::read32le(ProviderBytes->data() + 8) != 0xd65f03c0u)
    return std::nullopt;
  const auto First = readImmutableImageBytes(Image, *Pair, 8);
  const auto Second = readImmutableChainedImageValue(Image, *Pair + 8);
  const uint64_t Word =
      First ? llvm::support::endian::read64le(First->data()) : 0;
  const auto Literal =
      Second ? swiftLiteralString(Image, Word, *Second) : std::nullopt;
  const auto *Section =
      Literal ? Image.getSectionFor(Literal->Contents) : nullptr;
  const auto Pool =
      Section ? cstringStorageSourceHint(Image, Section->VA) : std::nullopt;
  const auto RetainTarget = branch(SharedWord(11), *Shared + 44, false);
  const auto Slot = RetainTarget ? darwinImportVeneerSlot(Image, *RetainTarget)
                                 : std::nullopt;
  const auto Import = Slot ? darwinRuntimeImport(Image, *Slot) : std::nullopt;
  const auto Retain =
      Slot ? swiftRuntimeSourceCallHint(Image, *Slot) : std::nullopt;
  const auto Bind =
      Slot ? Image.DyldBindSlots.find(*Slot) : Image.DyldBindSlots.end();
  constexpr auto ProviderName = "/usr/lib/swift/libswiftCore.dylib";
  if (!Literal || !Pool || !Import || *Import != "_swift_bridgeObjectRetain" ||
      !Retain || Retain->DoesNotReturn || Bind == Image.DyldBindSlots.end() ||
      Bind->second.Module != ProviderName ||
      std::count(Image.DynInfo.NeededLibs.begin(),
                 Image.DynInfo.NeededLibs.end(), ProviderName) != 1)
    return std::nullopt;
  if (!Image.isValidImportStorageSlot(*Slot, *Import))
    return std::nullopt;
  const auto Imports = Image.collectImportStorageSlots();
  const auto CurrentSlot = Imports.Slots.find(*Slot);
  if (Imports.Conflicts.count(*Slot) || CurrentSlot == Imports.Slots.end() ||
      CurrentSlot->second.Name != *Import || CurrentSlot->second.Addend)
    return std::nullopt;
  SourceFunctionTypeHint Addressor;
  Addressor.ReturnType = NdType::makePtr(NdType::makeVoid());
  std::string Error;
  if (!assignDarwinScalarSourceABI(Addressor, Image.Arch, Error))
    return std::nullopt;
  NativeSourceCalls Calls;
  for (const auto &Op : Low->Blocks.front().Ops) {
    if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL)
      continue;
    const auto Key = nativeSourceCallKey(Op);
    if (!Key)
      return std::nullopt;
    NativeSourceCallContract Call;
    if (Op.Addr == *Shared + 20 && Op.Opcode == NdOp::INDIR_CALL &&
        Op.Inputs[0] == NdVar::reg(8, 8))
      Call.Signature = &Addressor;
    else if (Op.Addr == *Shared + 44 && Op.Opcode == NdOp::CALL &&
             Op.Inputs[0] == NdVar::cst(*RetainTarget, 8))
      Call.Signature = &Retain->Signature;
    else
      return std::nullopt;
    if (!Calls.emplace(*Key, Call).second)
      return std::nullopt;
  }
  if (Calls.size() != 2 || !restoresNativeSourceState(*Low, Image.Arch, Calls))
    return std::nullopt;
  for (const auto &F : Result.LowFuncs)
    for (const auto &B : F.Blocks)
      for (const auto &Op : B.Ops)
        if (Op.Opcode == NdOp::CALL && Op.NumInputs && Op.Inputs[0].isConst() &&
            Op.Inputs[0].Offset == Root)
          return std::nullopt;
  return Contract{Root, *Shared,  *Provider, *Pair, *Destination, *RetainTarget,
                  Word, *Literal, *Storage,  *Pool, *Retain};
}

inline ExprPtr scalar(uint64_t Value) {
  return HighExpr::makeConst(Value, 8, ConstantAddressProvenance::Scalar);
}
inline ExprPtr address(const SourceCallTypeHint &Hint) {
  auto E = HighExpr::makeCall({}, 0, {});
  E->Type = NdType::makeInt(8, false);
  E->SourceCallHint = std::make_shared<SourceCallTypeHint>(Hint);
  return E;
}
inline ObjCSourceBindingResult project(const HighFunc &Function,
                                       const Contract &C) {
  ObjCSourceBindingResult P;
  P.Function = Function;
  P.Function.Locals.clear();
  P.Function.Body.clear();
  const auto StorageWord = [&] {
    return HighExpr::makeBinop(
        NdOp::INT_OR,
        HighExpr::makeBinop(NdOp::INT_SUB,
                            HighExpr::makeBinop(NdOp::INT_ADD, address(C.Pool),
                                                scalar(C.Literal.Contents -
                                                       C.Pool.TargetAddress)),
                            scalar(SwiftLiteralString::StorageBias)),
        scalar(SwiftLiteralString::ImmortalTag));
  };
  for (unsigned I = 0; I < 2; ++I) {
    HighStmt S;
    S.Kind = StmtKind::Store;
    S.Addr = C.Shared + 28 + I * 4;
    S.StoreAddr =
        I ? HighExpr::makeBinop(NdOp::INT_ADD, address(C.Storage), scalar(8))
          : address(C.Storage);
    S.StoreVal = I ? StorageWord() : scalar(C.Word);
    P.Function.Body.push_back(std::move(S));
  }
  HighStmt Retain;
  Retain.Kind = StmtKind::Call;
  Retain.Addr = C.Shared + 44;
  Retain.CallExpr = HighExpr::makeCall({}, C.RetainTarget, {StorageWord()});
  Retain.CallExpr->Type = C.Retain.Signature.ReturnType;
  Retain.CallExpr->SourceCallHint =
      std::make_shared<SourceCallTypeHint>(C.Retain);
  auto Discard = std::make_shared<HighExpr>();
  Discard->Kind = ExprKind::Cast;
  Discard->Type = Discard->CastTo = NdType::makeVoid();
  Discard->Operands = {Retain.CallExpr};
  Retain.CallExpr = std::move(Discard);
  P.Function.Body.push_back(std::move(Retain));
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.Addr = C.Root + 20;
  P.Function.Body.push_back(std::move(Return));
  P.LocalStorageExtents.emplace(C.Destination, 16);
  P.CStringSections.insert(C.Pool.TargetAddress);
  return P;
}

// Compare only the small admitted tree language, including current bindings.
// A saved projection, name or matching scalar bits never replace prove().
inline bool sameExpression(const ExprPtr &A, const ExprPtr &B,
                           const BinaryImage &Image, unsigned Depth = 0) {
  if (!A || !B)
    return !A && !B;
  if (Depth > 12 || A->Kind != B->Kind || A->Op != B->Op ||
      !equalSourceTypes(A->Type, B->Type) ||
      bool(A->CastTo) != bool(B->CastTo) ||
      (A->CastTo && !equalSourceTypes(A->CastTo, B->CastTo)) ||
      A->MemoryOrdering != NdMemoryOrdering::None ||
      A->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      A->IntrinsicId != Intrinsic::None || !A->IntrinsicOutputs.empty() ||
      A->IsIndirectCall || A->IndirectParamIdx != -1 ||
      A->Operands.size() != B->Operands.size())
    return false;
  if (A->Kind == ExprKind::Call) {
    if (!A->SourceCallHint || !B->SourceCallHint ||
        A->CallAddr != B->CallAddr || A->CallTarget != B->CallTarget ||
        A->SourceCallHint->CallKind != B->SourceCallHint->CallKind ||
        A->SourceCallHint->TargetAddress != B->SourceCallHint->TargetAddress ||
        A->SourceCallHint->ByteCount != B->SourceCallHint->ByteCount ||
        A->SourceCallHint->TargetName != B->SourceCallHint->TargetName ||
        !objcSourceCallBound(*A, Image, {}))
      return false;
  } else if (A->SourceCallHint || A->CallAddr || !A->CallTarget.empty()) {
    return false;
  }
  if (A->Kind == ExprKind::Const) {
    if (A->ConstVal != B->ConstVal ||
        A->ConstProvenance != B->ConstProvenance ||
        A->AddressOwnerVA != B->AddressOwnerVA)
      return false;
  } else if (A->Kind != ExprKind::BinOp && A->Kind != ExprKind::Call &&
             A->Kind != ExprKind::Cast) {
    return false;
  }
  for (size_t I = 0; I < A->Operands.size(); ++I)
    if (!sameExpression(A->Operands[I], B->Operands[I], Image, Depth + 1))
      return false;
  return true;
}
} // namespace objc_immutable_string_callback_detail

inline std::optional<ObjCSourceBindingResult>
projectObjCImmutableStringCallback(const HighFunc &Function,
                                   const BinaryImage &Image,
                                   const PipelineResult &Result,
                                   const SwiftOnceSourcePlan &Once) {
  const auto C = objc_immutable_string_callback_detail::prove(
      Image, Result, Once, Function.Entry);
  if (!C || !Function.SourceTypeHint ||
      !equalSourceABIs(*Function.SourceTypeHint,
                       Once.CallbackHints.at(Function.Entry)))
    return std::nullopt;
  return objc_immutable_string_callback_detail::project(Function, *C);
}

inline bool objCImmutableStringCallbackValid(const HighFunc &Function,
                                             const BinaryImage &Image,
                                             const PipelineResult &Result,
                                             const SwiftOnceSourcePlan &Once) {
  const auto Expected =
      projectObjCImmutableStringCallback(Function, Image, Result, Once);
  if (!Expected || Function.Params.size() != 1 || !Function.Params[0].Type ||
      !equalSourceTypes(Function.Params[0].Type,
                        NdType::makePtr(NdType::makeVoid())) ||
      !equalSourceTypes(Function.ReturnType, NdType::makeVoid()) ||
      !Function.Locals.empty() || Function.DoesNotReturn ||
      (Function.ExceptionMetadata &&
       !objc_projection_detail::isPlainUnwind(*Function.ExceptionMetadata)) ||
      Function.StructuredExceptionRegions ||
      Function.UnstructuredExceptionRegions || Function.Body.size() != 4)
    return false;
  for (size_t I = 0; I < 4; ++I) {
    const auto &A = Function.Body[I];
    const auto &B = Expected->Function.Body[I];
    if (A.Kind != B.Kind || A.Addr != B.Addr || A.Dst || A.Val || A.Cond ||
        A.RetVal || A.SwitchExpr || A.GotoTarget || A.LoopHeaderAddr ||
        !A.Body.empty() || !A.ElseBody.empty() || !A.Cases.empty() ||
        !A.DefaultBody.empty() || !A.EHClauses.empty() ||
        !A.EHClauseBodies.empty() || A.EHIsReducible || A.IsPhiCopy ||
        A.MemoryOrdering != NdMemoryOrdering::None ||
        A.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        !objc_immutable_string_callback_detail::sameExpression(
            A.StoreAddr, B.StoreAddr, Image) ||
        !objc_immutable_string_callback_detail::sameExpression(
            A.StoreVal, B.StoreVal, Image) ||
        !objc_immutable_string_callback_detail::sameExpression(
            A.CallExpr, B.CallExpr, Image))
      return false;
  }
  return true;
}
} // namespace neverd::sdk
#endif
