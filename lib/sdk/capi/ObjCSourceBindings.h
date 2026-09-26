#ifndef NEVERD_SDK_CAPI_OBJCSOURCEBINDINGS_H
#define NEVERD_SDK_CAPI_OBJCSOURCEBINDINGS_H

#include "../../ir/high/pass/HighFrameAddress.h"
#include "../../loader/MachO/DarwinRuntimeImport.h"
#include "../../loader/MachO/DarwinSourceDeclarations.h"
#include "../../loader/ObjC/ObjCRuntimeData.h"
#include "BorrowedByteSources.h"
#include "CStringStorageSources.h"
#include "ObjCConstantObjectSources.h"
#include "ObjCProfileStorage.h"
#include "ObjCReadOnlyScalarSources.h"
#include "ObjCSentinelStack.h"
#include "ObjCSourceProjection.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/DarwinRuntimeCalls.h"
#include "neverd/loader/ObjC/ObjCCallHints.h"
#include "neverd/loader/ObjC/ObjCFormattedCalls.h"
#include "neverd/loader/ObjC/ObjCSentinelCalls.h"
#include "neverd/loader/ObjC/ObjCSourceDeclarations.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/loader/Swift/SwiftMetadata.h"
#include "neverd/loader/Swift/SwiftRuntimeCalls.h"
#include "neverd/loader/Swift/SwiftStringCalls.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Demangle/SwiftDemangle.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <array>
#include <optional>

namespace neverd::sdk {

inline bool objcSourceCallBound(
    const HighExpr &Expression, const BinaryImage &Image,
    const std::map<va_t, const HighFunc *> &Functions,
    const ObjCProfileStorage *ProfileStorage = nullptr,
    const std::set<const HighExpr *> *ReadOnlyHelpers = nullptr,
    const HighFunc *ContainingFunction = nullptr,
    const std::map<va_t, std::map<unsigned, ObjCReceiverTypeHint>>
        *BlockParameterReceivers = nullptr);

struct ObjCSourceBindingResult {
  HighFunc Function;
  std::string Limitation;
  std::set<va_t> Dependencies;
  std::set<std::string> InstanceLayoutClasses;
  std::set<std::string> RuntimeProtocols;
  std::set<va_t> AssociationKeys;
  std::set<va_t> KVOContexts;
  std::set<va_t> StaticIdentities;
  std::set<va_t> ClassReferenceCells;
  std::map<va_t, uint64_t> LocalStorageExtents;
  std::set<va_t> SwiftSmallStrings;
  std::map<va_t, SourceCallTypeHint::SwiftTypeMetadataAddress>
      SwiftTypeMetadataPairs;
  std::map<va_t, std::string> SwiftNominalDescriptors;
  std::map<va_t, std::string> SwiftNominalMetadata;
  /// Cache address -> exact accessor entry, revalidated when helpers render.
  std::map<va_t, va_t> SwiftWitnessCaches;
  /// Exact compiler-emitted Swift lazy global addressors used by this body.
  std::set<va_t> SwiftOnceAccessors;
  /// Exact Objective-C entry thunks for Swift lazy static object properties.
  std::set<va_t> SwiftOnceObjCThunks;
  /// Parameters whose only proven use was an ignored swift_once context.
  std::set<size_t> ErasedSwiftOnceContextParameters;
  std::set<va_t> ProfileCounterSections;
  std::set<va_t> ConstantStrings;
  std::set<va_t> ConstantObjects;
  std::map<va_t, uint32_t> ConstantObjectTables;
  std::set<BorrowedByteRange> BorrowedBytes;
  std::set<va_t> CStringSections, CStringPointerSlots;
  SourceProjectionDiagnostics Diagnostics{};
};

namespace objc_binding_detail {

inline std::optional<std::vector<va_t>>
formatAddresses(const SourceCallTypeHint::FormatArguments &Format) {
  if (!Format.FormatAddress || Format.AlternativeFormatAddresses.size() >= 64 ||
      !std::is_sorted(Format.AlternativeFormatAddresses.begin(),
                      Format.AlternativeFormatAddresses.end()) ||
      std::adjacent_find(Format.AlternativeFormatAddresses.begin(),
                         Format.AlternativeFormatAddresses.end()) !=
          Format.AlternativeFormatAddresses.end() ||
      (!Format.AlternativeFormatAddresses.empty() &&
       Format.AlternativeFormatAddresses.front() <= Format.FormatAddress))
    return std::nullopt;
  std::vector<va_t> Result{Format.FormatAddress};
  Result.insert(Result.end(), Format.AlternativeFormatAddresses.begin(),
                Format.AlternativeFormatAddresses.end());
  return Result;
}

inline bool sameSourceLocation(const SourceABIValueLocation &Left,
                               const SourceABIValueLocation &Right) {
  return Left.Kind == Right.Kind &&
         Left.RegisterOffset == Right.RegisterOffset &&
         Left.EntryStackOffset == Right.EntryStackOffset &&
         Left.ValueBytes == Right.ValueBytes &&
         Left.ExtendTo32Bits == Right.ExtendTo32Bits;
}

inline bool sameScalarCarrier(const TypeRef &Observed,
                              const TypeRef &Declared) {
  if (!Observed || !Declared || Observed->Size != Declared->Size)
    return false;
  if (Declared->Kind == NdTypeKind::Void)
    return Observed->Kind == NdTypeKind::Void;
  if (Declared->Kind == NdTypeKind::Float)
    return Observed->Kind == NdTypeKind::Float;
  if (Declared->Kind == NdTypeKind::Ptr)
    return Observed->Size == 8 && (Observed->Kind == NdTypeKind::Ptr ||
                                   Observed->Kind == NdTypeKind::Int);
  if (Declared->Kind == NdTypeKind::Int)
    return Observed->Kind == NdTypeKind::Int;
  return equalSourceTypes(Observed, Declared);
}

/// A native-analysis call can be reclassified only when it describes exactly
/// the same physical scalar call as the authoritative SDK declaration. Source
/// pointer spelling may be absent from native analysis; register class, width,
/// extension, stack location, and every aggregate component may not differ.
inline bool samePhysicalSourceCall(const SourceFunctionTypeHint &Observed,
                                   const SourceFunctionTypeHint &Declared) {
  std::string ObservedError, DeclaredError;
  if (Observed.Origin != SourceFunctionTypeHint::OriginKind::NativeAnalysis ||
      !validateSourceABI(Observed, ObservedError) ||
      !validateSourceABI(Declared, DeclaredError) ||
      Observed.Architecture != Declared.Architecture ||
      Observed.Convention != Declared.Convention || !Observed.HasExplicitABI ||
      !Declared.HasExplicitABI ||
      !sameScalarCarrier(Observed.ReturnType, Declared.ReturnType) ||
      !sameSourceLocation(Observed.ReturnLocation, Declared.ReturnLocation) ||
      Observed.ReturnComponents.size() != Declared.ReturnComponents.size() ||
      Observed.Parameters.size() != Declared.Parameters.size())
    return false;
  for (size_t I = 0; I < Observed.ReturnComponents.size(); ++I)
    if (!sameSourceLocation(Observed.ReturnComponents[I],
                            Declared.ReturnComponents[I]))
      return false;
  for (size_t I = 0; I < Observed.Parameters.size(); ++I) {
    const auto &Left = Observed.Parameters[I];
    const auto &Right = Declared.Parameters[I];
    if (Left.TheRole != SourceParameterTypeHint::Role::Ordinary ||
        Right.TheRole != SourceParameterTypeHint::Role::Ordinary ||
        !sameScalarCarrier(Left.Type, Right.Type) ||
        !sameSourceLocation(Left.Location, Right.Location) ||
        Left.Components.size() != Right.Components.size())
      return false;
    for (size_t J = 0; J < Left.Components.size(); ++J)
      if (!sameSourceLocation(Left.Components[J], Right.Components[J]))
        return false;
  }
  return true;
}

inline bool plainNativeBinding(const SourceCallTypeHint &Binding) {
  return Binding.CallKind == SourceCallTypeHint::Kind::Native &&
         !Binding.BooleanResult && !Binding.ValueWitness &&
         !Binding.DoesNotReturn && !Binding.WeakImport &&
         !Binding.ReturnedArgument && !Binding.RuntimeObjCResultType &&
         Binding.Selector.empty() && Binding.OwnerClass.empty() &&
         !Binding.SelectorReferenceAddress &&
         Binding.BorrowedByteInputs.empty() &&
         Binding.SwiftStringInputs.empty() && !Binding.Format &&
         !Binding.NilTerminated && !Binding.SwiftTypeMetadata &&
         !Binding.Receiver && !Binding.SelectorResultUse &&
         !Binding.SelectorResultTypeUse && !Binding.SelectorArgumentTypeUse &&
         !Binding.SelectorForwardingUse &&
         !Binding.SelectorArgumentStorageUse &&
         !Binding.ObjCIndirectResultStorage && !Binding.ByteCount &&
         !Binding.ImmutablePointerSlot;
}

inline std::optional<SourceCallTypeHint>
runtimeSourceCallHint(const BinaryImage &Image,
                      const SourceCallTypeHint &Binding) {
  using Kind = SourceCallTypeHint::Kind;
  switch (Binding.CallKind) {
  case Kind::ObjCRuntimeCall:
    return objcRuntimeSourceCallHint(Image, Binding.TargetAddress);
  case Kind::SwiftRuntimeCall:
    return swiftRuntimeSourceCallHint(Image, Binding.TargetAddress);
  case Kind::SwiftStringBridge:
  case Kind::SwiftStringFromNSString:
    return swiftStringSourceCallHint(Image, Binding.TargetAddress);
  case Kind::DarwinRuntimeCall:
    if (const auto CompilerRT =
            darwinCompilerRTSourceCallHint(Image, Binding.TargetAddress))
      return CompilerRT;
    return Binding.Format
               ? darwinFormattedSourceCallHint(Image, Binding.TargetAddress,
                                               Binding.Format->FormatAddress)
               : darwinRuntimeSourceCallHint(Image, Binding.TargetAddress);
  case Kind::DarwinRuntimeGlobalAddress:
    return darwinRuntimeGlobalAddressHint(Image, Binding.TargetAddress);
  default:
    return std::nullopt;
  }
}

inline bool runtimeBindingMatches(const SourceCallTypeHint &Binding,
                                  const SourceCallTypeHint &Expected) {
  return Binding.CallKind == Expected.CallKind &&
         Binding.DoesNotReturn == Expected.DoesNotReturn &&
         Binding.WeakImport == Expected.WeakImport &&
         Binding.ReturnedArgument == Expected.ReturnedArgument &&
         Binding.RuntimeObjCResultType == Expected.RuntimeObjCResultType &&
         Binding.TargetName == Expected.TargetName &&
         Binding.Selector.empty() && Binding.OwnerClass.empty() &&
         !Binding.SelectorReferenceAddress && !Binding.SelectorResultUse &&
         !Binding.SelectorResultTypeUse && !Binding.SelectorArgumentTypeUse &&
         !Binding.SelectorForwardingUse &&
         !Binding.SelectorArgumentStorageUse &&
         !Binding.ObjCIndirectResultStorage && !Binding.ByteCount &&
         !Binding.SwiftTypeMetadata && !Binding.NilTerminated &&
         !Expected.NilTerminated &&
         bool(Binding.Format) == bool(Expected.Format) &&
         (!Binding.Format ||
          (Binding.Format->FixedCount == Expected.Format->FixedCount &&
           Binding.Format->Syntax == Expected.Format->Syntax &&
           Binding.Format->FormatParameter ==
               Expected.Format->FormatParameter &&
           Binding.Format->FormatAddress == Expected.Format->FormatAddress &&
           Binding.Format->DynamicWithoutArguments ==
               Expected.Format->DynamicWithoutArguments &&
           Binding.Format->DynamicPointerArguments ==
               Expected.Format->DynamicPointerArguments &&
           Binding.Format->DynamicInteger64Arguments ==
               Expected.Format->DynamicInteger64Arguments &&
           Binding.Format->AlternativeFormatAddresses ==
               Expected.Format->AlternativeFormatAddresses)) &&
         Binding.BorrowedByteInputs == Expected.BorrowedByteInputs &&
         Binding.SwiftStringInputs == Expected.SwiftStringInputs &&
         objc_projection_detail::sameHint(Binding.Signature,
                                          Expected.Signature);
}

/// Prove that a value has a complete pointer source carrier even when native
/// SSA still spells the receiving variable as an integer. Every definition of
/// a merged value must resolve to a pointer-typed value or to an authenticated
/// Objective-C runtime call whose catalogued result is a pointer.
inline bool
provenSourcePointerValue(const ExprPtr &Value,
                         const VarKeyMap<std::vector<ExprPtr>> &Definitions,
                         const BinaryImage &Image, size_t &Budget,
                         std::set<VarKey> &Active, unsigned Depth = 0) {
  if (!Value || !Budget-- || Depth > 64)
    return false;
  if (Value->Type && Value->Type->Kind == NdTypeKind::Ptr &&
      Value->Type->Size == 8)
    return true;
  if ((Value->Kind == ExprKind::Cast || Value->Kind == ExprKind::BitCast) &&
      Value->Operands.size() == 1 && Value->Type && Value->Operands.front() &&
      Value->Operands.front()->Type && Value->Type->Size == 8 &&
      Value->Operands.front()->Type->Size == 8)
    return provenSourcePointerValue(Value->Operands.front(), Definitions, Image,
                                    Budget, Active, Depth + 1);
  if (Value->Kind == ExprKind::BinOp && Value->Op == NdOp::SELECT &&
      Value->Operands.size() == 3)
    return provenSourcePointerValue(Value->Operands[1], Definitions, Image,
                                    Budget, Active, Depth + 1) &&
           provenSourcePointerValue(Value->Operands[2], Definitions, Image,
                                    Budget, Active, Depth + 1);
  if (Value->Kind == ExprKind::Call && Value->SourceCallHint) {
    const auto &Binding = *Value->SourceCallHint;
    const auto Expected =
        Binding.CallKind == SourceCallTypeHint::Kind::ObjCRuntimeCall
            ? runtimeSourceCallHint(Image, Binding)
            : std::nullopt;
    std::string Reason;
    return Expected && Expected->Signature.ReturnType &&
           Expected->Signature.ReturnType->Kind == NdTypeKind::Ptr &&
           Expected->Signature.ReturnType->Size == 8 &&
           Value->IntrinsicId == Intrinsic::None &&
           Value->MemoryOrdering == NdMemoryOrdering::None &&
           Value->MemoryAddressSpace == NdMemoryAddressSpace::Default &&
           Value->Operands.size() == Binding.Signature.Parameters.size() &&
           validateSourceABI(Binding.Signature, Reason) &&
           Binding.Signature.Architecture == Image.Arch &&
           runtimeBindingMatches(Binding, *Expected);
  }
  if (Value->Kind != ExprKind::Var && Value->Kind != ExprKind::Phi)
    return false;
  const auto Key = varKey(Value->Var);
  if (!Active.insert(Key).second)
    return false;
  const auto Found = Definitions.find(Key);
  bool Valid = Found != Definitions.end() && !Found->second.empty();
  if (Valid)
    for (const auto &Definition : Found->second)
      if (!provenSourcePointerValue(Definition, Definitions, Image, Budget,
                                    Active, Depth + 1)) {
        Valid = false;
        break;
      }
  Active.erase(Key);
  return Valid;
}

/// An integer spelling alone is not a source declaration. Trace every
/// definition to an independently validated message result, keeping the
/// declaration's signedness. Narrowing, raw loads and unknown values cannot
/// acquire a complete variadic carrier through this proof.
inline TypeRef
provenSourceInteger64Value(const ExprPtr &Value,
                           const VarKeyMap<std::vector<ExprPtr>> &Definitions,
                           const BinaryImage &Image, size_t &Budget,
                           std::set<VarKey> &Active, unsigned Depth = 0) {
  if (!Value || !Budget || Depth > 64 || !Value->Type ||
      Value->Type->Kind != NdTypeKind::Int || Value->Type->Size != 8 ||
      Value->IntrinsicId != Intrinsic::None ||
      !Value->IntrinsicOutputs.empty() ||
      Value->MemoryOrdering != NdMemoryOrdering::None ||
      Value->MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return {};
  --Budget;
  if (Value->Kind == ExprKind::Cast || Value->Kind == ExprKind::BitCast) {
    if (Value->Operands.size() != 1 ||
        (Value->Kind == ExprKind::Cast && !Value->CastTo) ||
        (Value->CastTo && !equalSourceTypes(Value->Type, Value->CastTo)))
      return {};
    return provenSourceInteger64Value(Value->Operands.front(), Definitions,
                                      Image, Budget, Active, Depth + 1);
  }
  if (Value->Kind == ExprKind::BinOp && Value->Op == NdOp::SELECT &&
      Value->Operands.size() == 3) {
    const auto Left = provenSourceInteger64Value(
        Value->Operands[1], Definitions, Image, Budget, Active, Depth + 1);
    const auto Right = provenSourceInteger64Value(
        Value->Operands[2], Definitions, Image, Budget, Active, Depth + 1);
    return Left && Right && equalSourceTypes(Left, Right) ? Left : TypeRef{};
  }
  if (Value->Kind == ExprKind::Call && Value->SourceCallHint) {
    const auto &Binding = *Value->SourceCallHint;
    const auto &Signature = Binding.Signature;
    const auto &Return = Signature.ReturnType;
    std::string Error;
    if (Binding.CallKind != SourceCallTypeHint::Kind::ObjCMessage ||
        Binding.Format || Binding.NilTerminated || Binding.DoesNotReturn ||
        !Return || Return->Kind != NdTypeKind::Int || Return->Size != 8 ||
        !Signature.HasExplicitABI || Signature.Architecture != Image.Arch ||
        Value->Operands.size() != Signature.Parameters.size() ||
        !validateSourceABI(Signature, Error) ||
        !objcSourceCallBound(*Value, Image, {}, nullptr, nullptr, nullptr))
      return {};
    // The ordinary publication gate also accepts body-proven runtime calls.
    // This narrower type proof additionally requires a current declaration.
    if (!Binding.Receiver &&
        Signature.Origin != SourceFunctionTypeHint::OriginKind::ObjCSDK &&
        !(Binding.SelectorResultUse && Binding.SelectorResultTypeUse)) {
      const auto Expected = objcSelectorSourceTypeHint(Image, Binding.Selector);
      if (!Expected || !objc_projection_detail::sameHint(Signature, *Expected))
        return {};
    }
    return Return;
  }
  if ((Value->Kind != ExprKind::Var && Value->Kind != ExprKind::Phi) ||
      (Value->Var.Kind != MedVar::Reg && Value->Var.Kind != MedVar::Temp) ||
      Value->Var.Size != 8)
    return {};
  const auto Key = varKey(Value->Var);
  if (!Active.insert(Key).second)
    return {};
  TypeRef Result;
  const auto Found = Definitions.find(Key);
  if (Found != Definitions.end())
    for (const auto &Definition : Found->second) {
      const auto Type = provenSourceInteger64Value(
          Definition, Definitions, Image, Budget, Active, Depth + 1);
      if (!Type || (Result && !equalSourceTypes(Result, Type))) {
        Result.reset();
        break;
      }
      Result = Type;
    }
  Active.erase(Key);
  return Result;
}

inline std::optional<uint32_t> constantBorrowedByteCount(const ExprPtr &Value,
                                                         const TypeRef &Type,
                                                         unsigned Depth = 0) {
  if (!Value || !Type || Type->Kind != NdTypeKind::Int ||
      (Type->Size != 1 && Type->Size != 2 && Type->Size != 4 &&
       Type->Size != 8) ||
      Depth > 16)
    return std::nullopt;
  const auto Normalize = [](uint64_t Raw,
                            const TypeRef &Integer) -> std::optional<uint64_t> {
    if (!Integer || Integer->Kind != NdTypeKind::Int ||
        (Integer->Size != 1 && Integer->Size != 2 && Integer->Size != 4 &&
         Integer->Size != 8))
      return std::nullopt;
    const unsigned Bits = Integer->Size * 8;
    const uint64_t Mask = Bits == 64 ? UINT64_MAX : (UINT64_C(1) << Bits) - 1;
    Raw &= Mask;
    if (Integer->IsSigned && Bits < 64 && (Raw & (UINT64_C(1) << (Bits - 1))))
      Raw |= ~Mask;
    return Raw;
  };
  // Evaluate only the constant integer conversions represented explicitly in
  // HighIR. Preserve each intermediate width so a sign extension cannot be
  // mistaken for a large positive byte count.
  const auto Evaluate = [&](const auto &Self, const ExprPtr &Expression,
                            unsigned CurrentDepth) -> std::optional<uint64_t> {
    if (!Expression || !Expression->Type || CurrentDepth > 16)
      return std::nullopt;
    if (Expression->Kind == ExprKind::Const)
      return Normalize(Expression->ConstVal, Expression->Type);
    if (Expression->Kind == ExprKind::UnaryOp &&
        Expression->Operands.size() == 1 && Expression->Operands[0] &&
        (Expression->Op == NdOp::INT_ZEXT ||
         Expression->Op == NdOp::INT_SEXT)) {
      const auto &Input = Expression->Operands[0];
      if (!Input->Type || Input->Type->Kind != NdTypeKind::Int ||
          (Input->Type->Size != 1 && Input->Type->Size != 2 &&
           Input->Type->Size != 4 && Input->Type->Size != 8) ||
          Expression->Type->Kind != NdTypeKind::Int ||
          Expression->Type->Size < Input->Type->Size)
        return std::nullopt;
      const auto Inner = Self(Self, Input, CurrentDepth + 1);
      if (!Inner)
        return std::nullopt;
      const unsigned InputBits = Input->Type->Size * 8;
      const uint64_t InputMask =
          InputBits == 64 ? UINT64_MAX : (UINT64_C(1) << InputBits) - 1;
      uint64_t Extended = *Inner & InputMask;
      if (Expression->Op == NdOp::INT_SEXT && InputBits < 64 &&
          (Extended & (UINT64_C(1) << (InputBits - 1))))
        Extended |= ~InputMask;
      return Normalize(Extended, Expression->Type);
    }
    if (Expression->Kind != ExprKind::Cast ||
        Expression->Operands.size() != 1 || !Expression->CastTo ||
        !equalSourceTypes(Expression->Type, Expression->CastTo))
      return std::nullopt;
    const auto Inner = Self(Self, Expression->Operands[0], CurrentDepth + 1);
    return Inner ? Normalize(*Inner, Expression->Type) : std::nullopt;
  };
  const auto ValueBits = Evaluate(Evaluate, Value, Depth);
  const auto Declared = ValueBits ? Normalize(*ValueBits, Type) : std::nullopt;
  if (!Declared)
    return std::nullopt;
  const unsigned Bits = Type->Size * 8;
  const uint64_t Mask = Bits == 64 ? UINT64_MAX : (UINT64_C(1) << Bits) - 1;
  const uint64_t Count = *Declared & Mask;
  if ((Type->IsSigned && (Count & (UINT64_C(1) << (Bits - 1)))) ||
      Count > 1024 * 1024)
    return std::nullopt;
  return static_cast<uint32_t>(Count);
}

inline const Symbol *uniqueWritableDataSymbol(const BinaryImage &Image,
                                              va_t Address, uint64_t Width);

inline std::optional<SourceCallTypeHint>
associationKeyHint(const BinaryImage &Image, va_t Address,
                   bool AllowWritable = false) {
  if (Image.Format != BinaryFormat::MachO || Image.Bits != Bitness::Bits64 ||
      !Address)
    return std::nullopt;
  const auto *Section = Image.getSectionFor(Address);
  const auto *Segment = Image.getSegmentFor(Address);
  if (!Section || !Segment || !Section->isReadable() ||
      !Segment->isReadable() || Section->isExecutable() ||
      Segment->isExecutable() || !Image.readVA(Address, 1))
    return std::nullopt;
  const bool Writable = (Section->isWritable() || Segment->isWritable()) &&
                        !Segment->ReadOnlyAfterRelocations;
  if (Writable && !AllowWritable)
    return std::nullopt;
  const bool CString = (Section->Type & llvm::MachO::SECTION_TYPE) ==
                       llvm::MachO::S_CSTRING_LITERALS;
  const Symbol *Identity = nullptr;
  if (Writable) {
    // Associated-object keys use only the address. Require the same exact,
    // uniquely named writable storage proof as KVO context identities.
    Identity = uniqueWritableDataSymbol(Image, Address, 1);
    if (!Identity)
      return std::nullopt;
  } else if (CString) {
    // A pool with complete byte/identity evidence must use the same address
    // helper in key consumers and ordinary pointer uses.
    if (cstringStorageSourceHint(Image, Section->VA))
      return std::nullopt;
  } else {
    for (const auto &Symbol : Image.Symbols) {
      if (Symbol.Addr != Address)
        continue;
      if (Identity || Symbol.IsFunc || Symbol.Name.empty() ||
          llvm::StringRef(Symbol.Name).starts_with(kAutoFuncPrefix))
        return std::nullopt;
      Identity = &Symbol;
    }
    if (!Identity)
      return std::nullopt;
  }
  SourceCallTypeHint Hint;
  Hint.CallKind = SourceCallTypeHint::Kind::RuntimeAssociationKey;
  Hint.TargetAddress = Address;
  Hint.ByteCount = Writable ? 1 : 0;
  if (Identity)
    Hint.TargetName = Identity->Name;
  Hint.Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
  std::string Reason;
  if (!assignDarwinScalarSourceABI(Hint.Signature, Image.Arch, Reason))
    return std::nullopt;
  return Hint;
}

inline const Symbol *uniqueWritableDataSymbol(const BinaryImage &Image,
                                              va_t Address, uint64_t Width) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64) ||
      Image.MachOChainedFixupsAmbiguous || !Address || !Width ||
      Width > 1024 * 1024 || Width > InvalidVA - Address)
    return nullptr;
  const auto *Section = Image.getSectionFor(Address);
  const auto *Segment = Image.getSegmentFor(Address);
  if (!Section || !Segment || !Section->isReadable() ||
      !Section->isWritable() || Section->isExecutable() ||
      !Segment->isReadable() || !Segment->isWritable() ||
      Segment->isExecutable() || Address < Section->VA ||
      Address < Segment->VA ||
      Width > Section->Size - (Address - Section->VA) ||
      Width > Segment->Size - (Address - Segment->VA) ||
      !Image.readVA(Address, Width))
    return nullptr;
  const Symbol *Found = nullptr;
  for (const auto &Symbol : Image.Symbols) {
    if (Symbol.IsFunc || Symbol.Name.empty() ||
        llvm::StringRef(Symbol.Name).starts_with(kAutoFuncPrefix))
      continue;
    if (Symbol.Addr > Address && Symbol.Addr < Address + Width)
      return nullptr;
    if (Symbol.Addr != Address)
      continue;
    if (Found || (Symbol.Size && Width > Symbol.Size))
      return nullptr;
    Found = &Symbol;
  }
  return Found;
}

inline std::optional<SourceCallTypeHint>
staticIdentityHint(const BinaryImage &Image, va_t Address,
                   bool StorageAddress = false) {
  const auto *Symbol = uniqueWritableDataSymbol(Image, Address, 8);
  const auto Value =
      Symbol ? objc::RuntimeData(Image).localPointer(Address) : std::nullopt;
  if (!Symbol || !Value || *Value != Address ||
      !Image.MachOResolvedChainedPointerSlots.count(Address) ||
      (StorageAddress && readImmutableImagePointer(Image, Address) != Address))
    return std::nullopt;
  SourceCallTypeHint Hint;
  Hint.CallKind = SourceCallTypeHint::Kind::RuntimeStaticIdentity;
  Hint.TargetAddress = Address;
  Hint.TargetName = Symbol->Name;
  Hint.ByteCount = StorageAddress ? 8 : 0;
  Hint.Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
  std::string Reason;
  if (!assignDarwinScalarSourceABI(Hint.Signature, Image.Arch, Reason))
    return std::nullopt;
  return Hint;
}

inline std::optional<SourceCallTypeHint>
kvoContextHint(const BinaryImage &Image, va_t Address) {
  const auto *Symbol = uniqueWritableDataSymbol(Image, Address, 1);
  if (!Symbol)
    return std::nullopt;
  SourceCallTypeHint Hint;
  Hint.CallKind = SourceCallTypeHint::Kind::RuntimeKVOContext;
  Hint.TargetAddress = Address;
  Hint.TargetName = Symbol->Name;
  Hint.Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
  std::string Reason;
  if (!assignDarwinScalarSourceABI(Hint.Signature, Image.Arch, Reason))
    return std::nullopt;
  return Hint;
}

struct ConstantObjectTableEntry {
  va_t Target = 0;
  bool IsString = false;
};

inline std::optional<std::vector<ConstantObjectTableEntry>>
constantObjectTableEntries(const BinaryImage &Image, va_t Address,
                           uint32_t ByteCount) {
  if (!Address || !ByteCount || ByteCount > 65536 || ByteCount % 8 ||
      ByteCount > InvalidVA - Address)
    return std::nullopt;
  std::vector<ConstantObjectTableEntry> Entries;
  Entries.reserve(ByteCount / 8);
  for (uint32_t Offset = 0; Offset < ByteCount; Offset += 8) {
    const va_t Slot = Address + Offset;
    const auto Target = readImmutableImagePointer(Image, Slot);
    if (!Target) {
      const auto Bytes = readImmutableImageBytes(Image, Slot, 8);
      if (!Bytes || !std::all_of(Bytes->begin(), Bytes->end(),
                                 [](uint8_t Byte) { return Byte == 0; }))
        return std::nullopt;
      Entries.push_back({});
      continue;
    }
    if (constantStringSourceHint(Image, *Target)) {
      Entries.push_back({*Target, true});
      continue;
    }
    if (!constantObjectSourceHint(Image, *Target))
      return std::nullopt;
    Entries.push_back({*Target, false});
  }
  return Entries;
}

inline std::optional<SourceCallTypeHint>
constantObjectTableHint(const BinaryImage &Image, va_t Address,
                        uint32_t ByteCount) {
  if (!constantObjectTableEntries(Image, Address, ByteCount))
    return std::nullopt;
  SourceCallTypeHint Hint;
  Hint.CallKind = SourceCallTypeHint::Kind::RuntimeConstantObjectTable;
  Hint.TargetAddress = Address;
  Hint.ByteCount = ByteCount;
  Hint.Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
  std::string Reason;
  if (!assignDarwinScalarSourceABI(Hint.Signature, Image.Arch, Reason))
    return std::nullopt;
  return Hint;
}

inline bool overlapsPointerStorage(const BinaryImage &Image, va_t Address,
                                   uint64_t Width) {
  // Slots are ordered and occupy eight bytes. Earlier slots cannot overlap;
  // if this first candidate starts beyond the access, later ones cannot either.
  // Subtraction avoids wrapping at either end of the address space.
  const va_t First = Address >= 7 ? Address - 7 : 0;
  auto Overlaps = [&](va_t Slot) {
    constexpr uint64_t PointerWidth = 8;
    return Slot <= Address ? Address - Slot < PointerWidth
                           : Slot - Address < Width;
  };
  auto SetOverlaps = [&](const auto &Values) {
    const auto It = Values.lower_bound(First);
    return It != Values.end() && Overlaps(*It);
  };
  auto MapOverlaps = [&](const auto &Values) {
    const auto It = Values.lower_bound(First);
    return It != Values.end() && Overlaps(It->first);
  };
  return SetOverlaps(Image.MachOResolvedChainedPointerSlots) ||
         SetOverlaps(Image.CodePtrRelocSlots) ||
         SetOverlaps(Image.DataPtrRelocSlots) ||
         SetOverlaps(Image.RelDataPtrRelocSlots) ||
         SetOverlaps(Image.RelCodeRelocSlots) ||
         MapOverlaps(Image.ImportPtrSlots) ||
         MapOverlaps(Image.ImportStorageSlots) ||
         MapOverlaps(Image.DyldBindSlots);
}

inline std::optional<std::string>
swiftSimpleDescriptorModule(llvm::StringRef Symbol,
                            llvm::StringRef DescriptorKind,
                            llvm::StringRef DeclKind) {
  Symbol.consume_front("_");
  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = 8000;
  Options.MaxNodes = 1024;
  Options.MaxDepth = 64;
  Options.MaxMemoryBytes = 1024 * 1024;
  Options.MaxOperations = 100000;
  const auto Parsed = llvm::swiftDemangle(Symbol, Options);
  const auto Shape = [](const llvm::SwiftDemangleNode &Node,
                        llvm::StringRef Kind, size_t Children) {
    return Node.Kind == Kind && !Node.Text && !Node.Index &&
           Node.Children.size() == Children;
  };
  if (!Parsed.Root || !Parsed.Error.empty() ||
      !Shape(*Parsed.Root, "Global", 1))
    return std::nullopt;
  const auto &Descriptor = Parsed.Root->Children[0];
  if (!Shape(Descriptor, DescriptorKind, 1) ||
      !Shape(Descriptor.Children[0], "Type", 1))
    return std::nullopt;
  const auto &Nominal = Descriptor.Children[0].Children[0];
  const bool ExpectedDeclaration =
      DeclKind.empty() ? (Nominal.Kind == "Structure" ||
                          Nominal.Kind == "Class" || Nominal.Kind == "Enum")
                       : Nominal.Kind == DeclKind;
  if (!ExpectedDeclaration || Nominal.Text || Nominal.Index ||
      Nominal.Children.size() != 2)
    return std::nullopt;
  const auto &DeclaredModule = Nominal.Children[0];
  const auto &Name = Nominal.Children[1];
  if (DeclaredModule.Kind != "Module" || !DeclaredModule.Text ||
      DeclaredModule.Text->empty() || DeclaredModule.Index ||
      !DeclaredModule.Children.empty() || Name.Kind != "Identifier" ||
      !Name.Text || Name.Text->empty() || Name.Index || !Name.Children.empty())
    return std::nullopt;
  return *DeclaredModule.Text;
}

inline bool swiftSimpleDescriptor(llvm::StringRef Symbol,
                                  llvm::StringRef DescriptorKind,
                                  llvm::StringRef DeclKind,
                                  llvm::StringRef Module = {}) {
  const auto DeclaredModule =
      swiftSimpleDescriptorModule(Symbol, DescriptorKind, DeclKind);
  return DeclaredModule && (Module.empty() || *DeclaredModule == Module.str());
}

inline bool swiftNominalDescriptor(llvm::StringRef Symbol,
                                   llvm::StringRef Module) {
  return swiftSimpleDescriptor(Symbol, "NominalTypeDescriptor", {}, Module);
}

inline bool swiftExportedNominalDescriptor(llvm::StringRef Symbol) {
  Symbol.consume_front("_");
  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = 8000;
  Options.MaxNodes = 1024;
  Options.MaxDepth = 64;
  Options.MaxMemoryBytes = 1024 * 1024;
  Options.MaxOperations = 100000;
  const auto Parsed = llvm::swiftDemangle(Symbol, Options);
  const auto Shape = [](const llvm::SwiftDemangleNode &Node,
                        llvm::StringRef Kind, size_t Children) {
    return Node.Kind == Kind && !Node.Text && !Node.Index &&
           Node.Children.size() == Children;
  };
  if (!Parsed.Root || !Parsed.Error.empty() ||
      !Shape(*Parsed.Root, "Global", 1) ||
      !Shape(Parsed.Root->Children[0], "NominalTypeDescriptor", 1) ||
      !Shape(Parsed.Root->Children[0].Children[0], "Type", 1))
    return false;

  const auto ValidDeclaration = [&](const auto &Self,
                                    const llvm::SwiftDemangleNode &Node,
                                    unsigned Depth) -> bool {
    if (Depth >= 8 ||
        (Node.Kind != "Structure" && Node.Kind != "Class" &&
         Node.Kind != "Enum") ||
        Node.Text || Node.Index || Node.Children.size() != 2)
      return false;
    const auto &Context = Node.Children[0];
    const auto &Name = Node.Children[1];
    if (Name.Kind != "Identifier" || !Name.Text || Name.Text->empty() ||
        Name.Index || !Name.Children.empty())
      return false;
    if (Context.Kind == "Module")
      return Context.Text && !Context.Text->empty() && !Context.Index &&
             Context.Children.empty();
    return Self(Self, Context, Depth + 1);
  };
  return ValidDeclaration(ValidDeclaration,
                          Parsed.Root->Children[0].Children[0].Children[0], 0);
}

inline bool swiftProtocolDescriptor(llvm::StringRef Symbol) {
  return swiftSimpleDescriptor(Symbol, "ProtocolDescriptor", "Protocol");
}

inline bool swiftMangledType(llvm::StringRef Symbol) {
  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = 8000;
  Options.MaxNodes = 1024;
  Options.MaxDepth = 64;
  Options.MaxMemoryBytes = 1024 * 1024;
  Options.MaxOperations = 100000;
  const auto Parsed = llvm::swiftDemangle(Symbol, Options);
  return Parsed.Root && Parsed.Error.empty() && Parsed.Root->Kind == "Global" &&
         !Parsed.Root->Text && !Parsed.Root->Index &&
         Parsed.Root->Children.size() == 1;
}

inline bool swiftSystemFrameworkNominalDescriptor(llvm::StringRef Symbol,
                                                  llvm::StringRef Provider) {
  const auto Module =
      swiftSimpleDescriptorModule(Symbol, "NominalTypeDescriptor", {});
  if (!Module || !std::all_of(Module->begin(), Module->end(), [](char C) {
        return llvm::isAlnum(C) || C == '_';
      }))
    return false;
  const std::string Prefix =
      "/System/Library/Frameworks/" + *Module + ".framework/";
  return Provider == Prefix + *Module ||
         Provider == Prefix + "Versions/A/" + *Module;
}

inline bool swiftImportedNominalDescriptor(const BinaryImage &Image, va_t Slot,
                                           llvm::StringRef Symbol,
                                           llvm::StringRef Provider) {
  if (swiftSystemFrameworkNominalDescriptor(Symbol, Provider))
    return true;
  // Actions 35683213827, consumer bfe0f17b3d7140d468f53a6f7a07375d3b7d898a:
  // retained Xcode 26.5 libswiftCore TBDs export this exact descriptor for
  // arm64e-ios and arm64-ios-simulator. Manifest SHA-256:
  // 95e268bb837009a8881ecb210aa6cc3a970602b6277799dc68f048fbd13c6e40.
  // Bind only its identity in a proven type-reference recipe; no descriptor
  // bytes, runtime class layout, or callable ABI are inferred from the name.
  if (Image.Arch != Arch::AArch64 ||
      Provider != "/usr/lib/swift/libswiftCore.dylib" ||
      !isImmutableImageImportSlot(Image, Slot))
    return false;
  if (Symbol == "_$ss23_ContiguousArrayStorageCMn")
    return true;
  // The linked image's exact strong dyld bind proves this descriptor's
  // provider on its target runtime. Only its identity is reconstructed;
  // the storage class layout and metadata contents remain opaque.
  return Symbol == "_$ss18_DictionaryStorageCMn" ||
         Symbol == "_$ss13ManagedBufferCMn";
}

inline std::optional<va_t> swiftRelativeAddress(const BinaryImage &Image,
                                                va_t Field) {
  const auto *Bytes = Image.readVA(Field, 4);
  if (!Bytes)
    return std::nullopt;
  const int64_t Delta =
      static_cast<int32_t>(llvm::support::endian::read32le(Bytes));
  if (!Delta || (Delta < 0 && Field < static_cast<uint64_t>(-Delta)) ||
      (Delta > 0 && Field > InvalidVA - static_cast<uint64_t>(Delta)))
    return std::nullopt;
  return Delta < 0 ? Field - static_cast<uint64_t>(-Delta)
                   : Field + static_cast<uint64_t>(Delta);
}

inline std::optional<std::string>
swiftLocalExportedDescriptor(const BinaryImage &Image, va_t DescriptorSlot) {
  const auto Imports = Image.collectImportStorageSlot(DescriptorSlot);
  const auto *SlotSection = Image.getSectionFor(DescriptorSlot);
  const auto *SlotSegment = Image.getSegmentFor(DescriptorSlot);
  const auto *SlotBytes = Image.readVA(DescriptorSlot, 8);
  if (DescriptorSlot % 8 || !SlotSection || !SlotSegment || !SlotBytes ||
      !SlotSection->isReadable() || !SlotSegment->isReadable() ||
      !SlotSegment->ReadOnlyAfterRelocations ||
      Image.hasExecutableCodeOwnerAt(DescriptorSlot) ||
      !Image.DataPtrRelocSlots.count(DescriptorSlot) ||
      !Image.MachOResolvedChainedPointerSlots.count(DescriptorSlot) ||
      Image.CodePtrRelocSlots.count(DescriptorSlot) ||
      Image.RelDataPtrRelocSlots.count(DescriptorSlot) ||
      Image.RelCodeRelocSlots.count(DescriptorSlot) ||
      Imports.Conflicts.count(DescriptorSlot) ||
      Imports.Slots.count(DescriptorSlot) ||
      Image.DyldBindSlots.count(DescriptorSlot))
    return std::nullopt;

  const va_t DescriptorAddress = llvm::support::endian::read64le(SlotBytes);
  const auto *DescriptorSection = Image.getSectionFor(DescriptorAddress);
  const auto *DescriptorSegment = Image.getSegmentFor(DescriptorAddress);
  const auto Owner = Image.DataPtrRelocTargetOwners.find(DescriptorSlot);
  if (!DescriptorAddress || !DescriptorSection || !DescriptorSegment ||
      !DescriptorSection->isReadable() || !DescriptorSegment->isReadable() ||
      DescriptorSection->isWritable() || DescriptorSegment->isWritable() ||
      Image.hasExecutableCodeOwnerAt(DescriptorAddress) ||
      Owner == Image.DataPtrRelocTargetOwners.end() ||
      Owner->second != DescriptorSection->VA)
    return std::nullopt;

  const Symbol *Descriptor = nullptr;
  for (const auto &Candidate : Image.Symbols) {
    if (Candidate.Addr != DescriptorAddress || Candidate.IsFunc ||
        Candidate.Name.empty())
      continue;
    if (Descriptor)
      return std::nullopt;
    Descriptor = &Candidate;
  }
  if (!Descriptor || (!swiftProtocolDescriptor(Descriptor->Name) &&
                      !swiftExportedNominalDescriptor(Descriptor->Name)))
    return std::nullopt;

  size_t MatchingExports = 0;
  for (const auto &Export : Image.Exports) {
    if (Export.Addr == DescriptorAddress && Export.Name == Descriptor->Name)
      ++MatchingExports;
    else if (Export.Addr == DescriptorAddress)
      return std::nullopt;
  }
  if (MatchingExports != 1)
    return std::nullopt;
  return Descriptor->Name;
}

inline std::optional<std::string>
swiftTypeMetadataDescriptor(const BinaryImage &Image, va_t DescriptorSlot) {
  const auto Imports = Image.collectImportStorageSlot(DescriptorSlot);
  const auto Import = Imports.Slots.find(DescriptorSlot);
  const auto Bind = Image.DyldBindSlots.find(DescriptorSlot);
  std::optional<std::string> DescriptorSymbol;
  if (!Imports.Conflicts.count(DescriptorSlot) &&
      Import != Imports.Slots.end() &&
      Import->second.Evidence == ImportStorageEvidence::LoaderBind &&
      !Import->second.Addend && Bind != Image.DyldBindSlots.end() &&
      Bind->second.Name == Import->second.Name && !Bind->second.Addend &&
      !Bind->second.WeakImport &&
      swiftImportedNominalDescriptor(Image, DescriptorSlot, Import->second.Name,
                                     Bind->second.Module))
    DescriptorSymbol = Import->second.Name;
  else
    DescriptorSymbol = swiftLocalExportedDescriptor(Image, DescriptorSlot);
  if (!DescriptorSymbol)
    return std::nullopt;

  llvm::StringRef Descriptor(*DescriptorSymbol);
  Descriptor.consume_front("_");
  const bool Nominal = Descriptor.ends_with("Mn");
  const bool Protocol = Descriptor.ends_with("Mp");
  const bool ValidNominal =
      Nominal &&
      (Bind != Image.DyldBindSlots.end()
           ? swiftImportedNominalDescriptor(
                 Image, DescriptorSlot, *DescriptorSymbol, Bind->second.Module)
           : swiftExportedNominalDescriptor(*DescriptorSymbol));
  if ((!Nominal && !Protocol) || (Nominal && !ValidNominal) ||
      (Protocol != swiftProtocolDescriptor(*DescriptorSymbol)))
    return std::nullopt;
  return DescriptorSymbol;
}

inline std::optional<std::string>
swiftDirectTypeMetadataDescriptor(const BinaryImage &Image,
                                  va_t DescriptorAddress) {
  const auto *Section = Image.getSectionFor(DescriptorAddress);
  const auto *Segment = Image.getSegmentFor(DescriptorAddress);
  if (!Section || !Segment || !Section->isReadable() || Section->isWritable() ||
      !Segment->isReadable() || Segment->isWritable() ||
      Image.hasExecutableCodeOwnerAt(DescriptorAddress))
    return std::nullopt;
  const Symbol *Descriptor = nullptr;
  for (const auto &Candidate : Image.Symbols) {
    if (Candidate.Addr != DescriptorAddress || Candidate.IsFunc ||
        Candidate.Name.empty())
      continue;
    if (Descriptor)
      return std::nullopt;
    Descriptor = &Candidate;
  }
  if (!Descriptor || (!swiftExportedNominalDescriptor(Descriptor->Name) &&
                      !swiftProtocolDescriptor(Descriptor->Name)))
    return std::nullopt;
  size_t MatchingExports = 0;
  for (const auto &Export : Image.Exports) {
    if (Export.Addr != DescriptorAddress) {
      if (Export.Name == Descriptor->Name)
        return std::nullopt;
      continue;
    }
    if (Export.Name != Descriptor->Name)
      return std::nullopt;
    ++MatchingExports;
  }
  return MatchingExports == 1 ? std::optional<std::string>(Descriptor->Name)
                              : std::nullopt;
}

inline std::optional<SourceCallTypeHint>
swiftNominalDescriptorAddressHint(const BinaryImage &Image, va_t Address) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
      !Image.MachOTwoLevelNamespace || Image.MachOChainedFixupsAmbiguous)
    return std::nullopt;
  const auto Symbol = swiftDirectTypeMetadataDescriptor(Image, Address);
  if (!Symbol || !swiftExportedNominalDescriptor(*Symbol))
    return std::nullopt;
  SourceCallTypeHint Hint;
  Hint.CallKind =
      SourceCallTypeHint::Kind::RuntimeSwiftNominalDescriptorAddress;
  Hint.TargetAddress = Address;
  Hint.TargetName = *Symbol;
  Hint.Signature.Origin = SourceFunctionTypeHint::OriginKind::SwiftRuntime;
  Hint.Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
  std::string Reason;
  if (!assignDarwinScalarSourceABI(Hint.Signature, Image.Arch, Reason))
    return std::nullopt;
  return Hint;
}

// Swift may merge nominal metadata accessors into a native helper which
// forwards its third argument to swift_getSingletonMetadata. A caller's
// exported descriptor is still the runtime's descriptor identity when that
// forwarding is present in the helper's typed body.
inline bool swiftSingletonDescriptorForwardedByNativeHelper(
    const BinaryImage &Image, const HighFunc &Helper,
    const SourceCallTypeHint &Call) {
  std::string ABIError;
  if (Image.Arch != Arch::AArch64 || Helper.Entry != Call.TargetAddress ||
      !Image.isCodeAddress(Helper.Entry) || !Helper.SourceTypeHint ||
      Helper.SourceTypeHint->Origin !=
          SourceFunctionTypeHint::OriginKind::NativeAnalysis ||
      !Helper.SourceTypeHint->HasExplicitABI ||
      !validateSourceABI(*Helper.SourceTypeHint, ABIError) ||
      !equalSourceABIs(*Helper.SourceTypeHint, Call.Signature) ||
      Helper.SourceTypeHint->Parameters.size() != 3 ||
      Helper.Params.size() != 3 || Helper.Body.empty() ||
      Helper.StructuredExceptionRegions ||
      Helper.UnstructuredExceptionRegions ||
      (Helper.ExceptionMetadata &&
       !objc_projection_detail::isPlainUnwind(*Helper.ExceptionMetadata)))
    return false;
  const auto &Forwarded = Helper.SourceTypeHint->Parameters[2];
  if (Forwarded.Location.Kind != SourceABICarrierKind::IntegerRegister ||
      Forwarded.Location.RegisterOffset !=
          getTargetRegInfo(Image.Arch).IntParamRegs[2] ||
      Forwarded.Location.ValueBytes != 8 || !Forwarded.Type ||
      Forwarded.Type->Size != 8)
    return false;
  std::set<const HighExpr *> Seen;
  size_t RuntimeCalls = 0;
  size_t Budget = 4096;
  bool Valid = true;
  walkStmts(Helper.Body, [&](const HighStmt &Statement) {
    forEachExpr(Statement, [&](const ExprPtr &Root) {
      std::vector<const HighExpr *> Pending{Root.get()};
      while (Valid && !Pending.empty()) {
        const HighExpr *Expression = Pending.back();
        Pending.pop_back();
        if (!Expression || !Seen.insert(Expression).second)
          continue;
        if (!Budget--) {
          Valid = false;
          break;
        }
        if (Expression->Kind == ExprKind::Call && Expression->SourceCallHint &&
            Expression->SourceCallHint->CallKind ==
                SourceCallTypeHint::Kind::SwiftRuntimeCall &&
            Expression->SourceCallHint->TargetName ==
                "swift_getSingletonMetadata") {
          const auto Expected = swiftRuntimeSourceCallHint(
              Image, Expression->SourceCallHint->TargetAddress);
          const auto &Operands = Expression->Operands;
          const HighExpr *Value =
              Operands.size() == 2 ? Operands[1].get() : nullptr;
          for (unsigned Depth = 0; Value && Depth < 4 &&
                                   (Value->Kind == ExprKind::Cast ||
                                    Value->Kind == ExprKind::BitCast) &&
                                   Value->Type && Value->Type->Size == 8 &&
                                   Value->Operands.size() == 1;
               ++Depth)
            Value = Value->Operands[0].get();
          if (!Expected ||
              !runtimeBindingMatches(*Expression->SourceCallHint, *Expected) ||
              Expression->IsIndirectCall || !Value ||
              Value->Kind != ExprKind::Var || !Value->Type ||
              Value->Type->Size != 8 || Value->Var.Kind != MedVar::Param ||
              Value->Var.Id != 2 || Value->Var.SSAVer != 0)
            Valid = false;
          ++RuntimeCalls;
        }
        for (const auto &Operand : Expression->Operands)
          Pending.push_back(Operand.get());
      }
    });
  });
  return Valid && RuntimeCalls == 1;
}

// A value-witness call needs the original concrete metadata identity. A
// copied metadata record would have different runtime identity and witness
// pointers, so accept only one exported Swift struct metadata symbol whose
// descriptor word points to its matching exported nominal descriptor.
inline std::optional<SourceCallTypeHint>
swiftNominalMetadataAddressHint(const BinaryImage &Image, va_t Address) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
      !Image.MachOTwoLevelNamespace || Image.MachOChainedFixupsAmbiguous ||
      Address % 8 || Address > InvalidVA - 16)
    return std::nullopt;
  const auto *Section = Image.getSectionFor(Address);
  const auto Bytes = readImmutableImageBytes(Image, Address, 8);
  const auto DescriptorAddress = readImmutableImagePointer(Image, Address + 8);
  if (!Section || Image.getSectionFor(Address + 15) != Section || !Bytes ||
      !DescriptorAddress ||
      llvm::support::endian::read64le(Bytes->data()) != 0x200)
    return std::nullopt;
  const Symbol *Metadata = nullptr;
  for (const auto &Candidate : Image.Symbols)
    if (Candidate.Addr == Address && !Candidate.IsFunc &&
        !Candidate.Name.empty()) {
      if (Metadata)
        return std::nullopt;
      Metadata = &Candidate;
    }
  if (!Metadata || (Metadata->Size && Metadata->Size < 16))
    return std::nullopt;
  llvm::StringRef Name(Metadata->Name);
  if (!Name.starts_with("_$s") || !Name.ends_with("VN"))
    return std::nullopt;
  const auto Descriptor =
      swiftDirectTypeMetadataDescriptor(Image, *DescriptorAddress);
  if (!Descriptor || *Descriptor != Name.drop_back(1).str() + "Mn")
    return std::nullopt;
  size_t Exports = 0;
  for (const auto &Export : Image.Exports) {
    if (Export.Addr == Address && Export.Name == Metadata->Name)
      ++Exports;
    else if (Export.Addr == Address || Export.Name == Metadata->Name)
      return std::nullopt;
  }
  if (Exports != 1)
    return std::nullopt;
  SourceCallTypeHint Hint;
  Hint.CallKind = SourceCallTypeHint::Kind::RuntimeSwiftNominalMetadataAddress;
  Hint.TargetAddress = Address;
  Hint.TargetName = Metadata->Name;
  Hint.Signature.Origin = SourceFunctionTypeHint::OriginKind::SwiftRuntime;
  Hint.Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
  std::string Reason;
  return assignDarwinScalarSourceABI(Hint.Signature, Image.Arch, Reason)
             ? std::optional<SourceCallTypeHint>(std::move(Hint))
             : std::nullopt;
}

struct SwiftTypeMetadataDescriptorReference {
  uint32_t Offset = 0;
  va_t Target = 0;
  std::string Symbol;
};

struct SwiftTypeMetadataPairProof {
  SourceCallTypeHint::SwiftTypeMetadataAddress Address;
  std::vector<SwiftTypeMetadataDescriptorReference> Descriptors;
  std::string TypeReference;
};

inline std::optional<SwiftTypeMetadataPairProof>
swiftTypeMetadataPairProof(const BinaryImage &Image, va_t CacheAddress,
                           va_t ReferenceAddress) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64) ||
      Image.MachOChainedFixupsAmbiguous || !Image.MachOTwoLevelNamespace ||
      !CacheAddress || !ReferenceAddress || CacheAddress == ReferenceAddress)
    return std::nullopt;

  const Symbol *Cache = nullptr, *Reference = nullptr;
  for (const auto &Symbol : Image.Symbols) {
    if (Symbol.IsFunc || Symbol.Name.empty())
      continue;
    if (Symbol.Addr == CacheAddress) {
      if (Cache)
        return std::nullopt;
      Cache = &Symbol;
    }
    if (Symbol.Addr == ReferenceAddress) {
      if (Reference)
        return std::nullopt;
      Reference = &Symbol;
    }
  }
  if (!Cache || !Reference)
    return std::nullopt;
  llvm::StringRef CacheName(Cache->Name), ReferenceName(Reference->Name);
  const bool CacheDecorated = CacheName.consume_front("_");
  const bool ReferenceDecorated = ReferenceName.consume_front("_");
  if (CacheDecorated != ReferenceDecorated || !CacheName.ends_with("Md") ||
      !ReferenceName.ends_with("MR") ||
      CacheName.drop_back(2) != ReferenceName.drop_back(2) ||
      uniqueWritableDataSymbol(Image, Cache->Addr, 8) != Cache ||
      overlapsPointerStorage(Image, Cache->Addr, 8))
    return std::nullopt;
  const std::string Base = CacheName.drop_back(2).str();
  const auto *CacheBytes = Image.readVA(Cache->Addr, 8);
  if (!CacheBytes || !std::all_of(CacheBytes, CacheBytes + 8,
                                  [](uint8_t Byte) { return Byte == 0; }))
    return std::nullopt;

  const auto *ReferenceSection = Image.getSectionFor(Reference->Addr);
  const auto *ReferenceSegment = Image.getSegmentFor(Reference->Addr);
  const auto *ReferenceBytes = Image.readVA(Reference->Addr, 8);
  if (!ReferenceSection || !ReferenceSegment || !ReferenceBytes ||
      Reference->Addr > InvalidVA - 7 || !ReferenceSection->isReadable() ||
      ReferenceSection->isWritable() || !ReferenceSegment->isReadable() ||
      ReferenceSegment->isWritable() ||
      Image.hasExecutableCodeOwnerAt(Reference->Addr) ||
      Image.hasExecutableCodeOwnerAt(Reference->Addr + 7) ||
      (Reference->Size && Reference->Size < 8) ||
      overlapsPointerStorage(Image, Reference->Addr, 8))
    return std::nullopt;
  const uint32_t Length = llvm::support::endian::read32le(ReferenceBytes + 4);
  if (!Length || Length > 128)
    return std::nullopt;
  const auto TypeReference = swiftRelativeAddress(Image, Reference->Addr);
  const auto *TypeBytes =
      TypeReference ? Image.readVA(*TypeReference, uint64_t(Length) + 1)
                    : nullptr;
  if (!TypeReference || !TypeBytes || *TypeReference > InvalidVA - Length ||
      TypeBytes[Length] != 0 ||
      Image.hasExecutableCodeOwnerAt(*TypeReference) ||
      Image.hasExecutableCodeOwnerAt(*TypeReference + Length) ||
      overlapsPointerStorage(Image, *TypeReference, uint64_t(Length) + 1))
    return std::nullopt;

  constexpr size_t MaxDescriptors = 8;
  std::vector<SwiftTypeMetadataDescriptorReference> Descriptors;
  std::string Expanded = "$s";
  for (uint32_t I = 0; I < Length;) {
    if (TypeBytes[I] != 1 && TypeBytes[I] != 2) {
      if (TypeBytes[I] < 0x21 || TypeBytes[I] > 0x7e)
        return std::nullopt;
      Expanded.push_back(static_cast<char>(TypeBytes[I++]));
      continue;
    }
    if (Length - I < 5 || Descriptors.size() >= MaxDescriptors)
      return std::nullopt;
    const auto DescriptorAddress =
        swiftRelativeAddress(Image, *TypeReference + I + 1);
    const auto DescriptorSymbol =
        !DescriptorAddress ? std::nullopt
        : TypeBytes[I] == 1
            ? swiftDirectTypeMetadataDescriptor(Image, *DescriptorAddress)
            : swiftTypeMetadataDescriptor(Image, *DescriptorAddress);
    if (!DescriptorAddress || !DescriptorSymbol)
      return std::nullopt;
    llvm::StringRef Descriptor(*DescriptorSymbol);
    Descriptor.consume_front("_");
    if (!Descriptor.consume_front("$s") ||
        (!Descriptor.ends_with("Mn") && !Descriptor.ends_with("Mp")))
      return std::nullopt;
    Expanded += Descriptor.drop_back(2).str();
    // Rebuild direct local descriptors through a private pointer cell too.
    // Swift's indirect symbolic-reference kind resolves the same exported
    // descriptor without requiring a new relative relocation at link time.
    Descriptors.push_back({I, *DescriptorAddress, *DescriptorSymbol});
    I += 5;
  }
  if (Expanded != Base || !swiftMangledType(Expanded))
    return std::nullopt;

  const std::string TypeReferenceBytes(
      reinterpret_cast<const char *>(TypeBytes), Length);
  return SwiftTypeMetadataPairProof{
      SourceCallTypeHint::SwiftTypeMetadataAddress{
          Cache->Addr, Reference->Addr, *TypeReference,
          Descriptors.empty() ? 0 : Descriptors.front().Target,
          Descriptors.empty() ? std::string{} : Descriptors.front().Symbol,
          Descriptors.empty() ? TypeReferenceBytes
                              : TypeReferenceBytes.substr(5)},
      std::move(Descriptors), TypeReferenceBytes};
}

inline std::optional<SourceCallTypeHint::SwiftTypeMetadataAddress>
swiftTypeMetadataPair(const BinaryImage &Image, va_t CacheAddress,
                      va_t ReferenceAddress) {
  const auto Proof =
      swiftTypeMetadataPairProof(Image, CacheAddress, ReferenceAddress);
  if (!Proof)
    return std::nullopt;
  return Proof->Address;
}

inline std::optional<SourceCallTypeHint> swiftTypeMetadataAddressHint(
    const BinaryImage &Image, va_t Address,
    const SourceCallTypeHint::SwiftTypeMetadataAddress &Candidate) {
  const auto Pair = swiftTypeMetadataPair(Image, Candidate.CacheAddress,
                                          Candidate.ReferenceAddress);
  if (!Pair || *Pair != Candidate ||
      (Address != Pair->CacheAddress && Address != Pair->ReferenceAddress))
    return std::nullopt;
  SourceCallTypeHint Hint;
  Hint.CallKind = SourceCallTypeHint::Kind::RuntimeSwiftTypeMetadataAddress;
  Hint.TargetAddress = Address;
  Hint.TargetName = Address == Pair->CacheAddress ? "cache" : "reference";
  Hint.SwiftTypeMetadata = *Pair;
  Hint.Signature.Origin = SourceFunctionTypeHint::OriginKind::SwiftRuntime;
  Hint.Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
  std::string Reason;
  if (!assignDarwinScalarSourceABI(Hint.Signature, Image.Arch, Reason))
    return std::nullopt;
  return Hint;
}

inline std::optional<uint64_t> constantAddress(const HighExpr &Expression,
                                               unsigned Depth = 0);

/// Recognize a complete compiler-emitted lazy witness
/// accessor. Private Swift cache symbols can repeat, so the proof is rooted in
/// this exact function's dataflow rather than a suffix or global name lookup.
inline std::optional<SourceCallTypeHint>
swiftWitnessCacheAddressHint(const HighFunc &Function, const BinaryImage &Image,
                             va_t Address,
                             std::array<std::string, 2> *Globals = nullptr) {
  if (!Function.Entry || !Function.ReturnType ||
      Function.ReturnType->Kind == NdTypeKind::Void ||
      Function.ReturnType->Size != 8 || Image.Format != BinaryFormat::MachO ||
      Image.IsRelocatable || Image.Bits != Bitness::Bits64 ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64) ||
      Image.MachOChainedFixupsAmbiguous || !Image.MachOTwoLevelNamespace ||
      Address % 8)
    return std::nullopt;
  const auto *Cache = uniqueWritableDataSymbol(Image, Address, 8);
  const auto *Bytes = Cache ? Image.readVA(Address, 8) : nullptr;
  if (!Cache || !Bytes ||
      !std::all_of(Bytes, Bytes + 8, [](uint8_t Byte) { return Byte == 0; }))
    return std::nullopt;

  const Symbol *Accessor = nullptr;
  for (const auto &Symbol : Image.Symbols) {
    if (!Symbol.IsFunc || Symbol.Addr != Function.Entry ||
        Symbol.Name.empty() ||
        llvm::StringRef(Symbol.Name).starts_with(kAutoFuncPrefix))
      continue;
    if (Accessor)
      return std::nullopt;
    Accessor = &Symbol;
  }
  llvm::StringRef CacheName(Cache->Name);
  if (!Accessor || !CacheName.ends_with("WL") ||
      Accessor->Name != CacheName.drop_back(1).str() + "l" ||
      (!Function.Name.empty() && Function.Name != Accessor->Name))
    return std::nullopt;

  VarKeyMap<std::vector<ExprPtr>> Definitions;
  walkStmts(Function.Body, [&](const HighStmt &Statement) {
    if (Statement.Kind == StmtKind::Assign && Statement.Dst && Statement.Val &&
        (Statement.Dst->Kind == ExprKind::Var ||
         Statement.Dst->Kind == ExprKind::Phi))
      Definitions[varKey(Statement.Dst->Var)].push_back(Statement.Val);
  });
  const auto Resolve = [&](auto &&Self, ExprPtr Value,
                           unsigned Depth = 0) -> ExprPtr {
    if (!Value || Depth > 64)
      return nullptr;
    if ((Value->Kind == ExprKind::Cast || Value->Kind == ExprKind::BitCast) &&
        Value->Operands.size() == 1 && Value->Type && Value->Operands[0] &&
        Value->Operands[0]->Type &&
        Value->Type->Size == Value->Operands[0]->Type->Size)
      return Self(Self, Value->Operands[0], Depth + 1);
    if (Value->Kind != ExprKind::Var && Value->Kind != ExprKind::Phi)
      return Value;
    const auto Found = Definitions.find(varKey(Value->Var));
    if (Found == Definitions.end() || Found->second.size() != 1 ||
        Found->second.front().get() == Value.get())
      return Value;
    return Self(Self, Found->second.front(), Depth + 1);
  };
  const auto ExactAddress = [&](const ExprPtr &Value) {
    return Value && constantAddress(*Value) == std::optional<va_t>(Address);
  };

  std::vector<ExprPtr> CacheLoads, WitnessCalls, Returns;
  std::vector<const HighStmt *> CacheStores;
  walkStmts(Function.Body, [&](const HighStmt &Statement) {
    if (Statement.Kind == StmtKind::Store && ExactAddress(Statement.StoreAddr))
      CacheStores.push_back(&Statement);
    if (Statement.Kind == StmtKind::Return && Statement.RetVal)
      Returns.push_back(Resolve(Resolve, Statement.RetVal));
    forEachExpr(Statement, [&](const ExprPtr &Expression) {
      if (!Expression)
        return;
      if (Expression->Kind == ExprKind::Load &&
          Expression->Operands.size() == 1 &&
          ExactAddress(Expression->Operands[0]))
        CacheLoads.push_back(Expression);
      if (Expression->Kind == ExprKind::Call && Expression->SourceCallHint &&
          Expression->SourceCallHint->CallKind ==
              SourceCallTypeHint::Kind::SwiftRuntimeCall &&
          Expression->SourceCallHint->TargetName == "swift_getWitnessTable")
        WitnessCalls.push_back(Expression);
    });
  });
  if (CacheLoads.size() != 1 || CacheStores.size() != 1 ||
      WitnessCalls.size() != 1 || Returns.size() != 2)
    return std::nullopt;
  const auto &Load = CacheLoads.front();
  const auto *Store = CacheStores.front();
  const auto &Witness = WitnessCalls.front();
  if (!Load->Type || Load->Type->Size != 8 ||
      Load->MemoryOrdering != NdMemoryOrdering::None ||
      Load->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      !Store->StoreVal || !Store->StoreVal->Type ||
      Store->StoreVal->Type->Size != 8 ||
      Store->MemoryOrdering != NdMemoryOrdering::Release ||
      Store->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Witness->IsIndirectCall || Witness->Operands.size() != 3 ||
      Witness->MemoryOrdering != NdMemoryOrdering::None ||
      Witness->MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return std::nullopt;
  const auto ExpectedRuntime =
      swiftRuntimeSourceCallHint(Image, Witness->SourceCallHint->TargetAddress);
  if (!ExpectedRuntime ||
      !runtimeBindingMatches(*Witness->SourceCallHint, *ExpectedRuntime) ||
      Resolve(Resolve, Store->StoreVal).get() != Witness.get())
    return std::nullopt;

  std::array<std::string, 2> ImportedGlobals;
  for (size_t Index = 0; Index < 2; ++Index) {
    const auto Value = Resolve(Resolve, Witness->Operands[Index]);
    if (!Value || Value->Kind != ExprKind::Load ||
        Value->Operands.size() != 1 || !Value->Type || Value->Type->Size != 8 ||
        Value->MemoryOrdering != NdMemoryOrdering::None ||
        Value->MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return std::nullopt;
    const auto Slot = constantAddress(*Value->Operands[0]);
    const auto Global =
        Slot ? darwinRuntimeGlobalAddressHint(Image, *Slot) : std::nullopt;
    if (!Global || Global->Signature.Origin !=
                       SourceFunctionTypeHint::OriginKind::SwiftRuntime)
      return std::nullopt;
    ImportedGlobals[Index] = Global->TargetName;
  }
  if (ImportedGlobals != std::array<std::string, 2>{"$sSSSysMc", "$sSSN"} &&
      ImportedGlobals != std::array<std::string, 2>{"$sSsSTsMc", "$sSsN"})
    return std::nullopt;
  const bool ReturnsLoad =
      std::any_of(Returns.begin(), Returns.end(), [&](const ExprPtr &Value) {
        return Value.get() == Load.get();
      });
  const bool ReturnsWitness =
      std::any_of(Returns.begin(), Returns.end(), [&](const ExprPtr &Value) {
        return Value.get() == Witness.get();
      });
  if (!ReturnsLoad || !ReturnsWitness)
    return std::nullopt;

  SourceCallTypeHint Hint;
  Hint.CallKind = SourceCallTypeHint::Kind::RuntimeSwiftWitnessCacheAddress;
  Hint.TargetAddress = Address;
  Hint.TargetName = Cache->Name;
  Hint.Signature.Origin = SourceFunctionTypeHint::OriginKind::SwiftRuntime;
  Hint.Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
  std::string Reason;
  if (!assignDarwinScalarSourceABI(Hint.Signature, Image.Arch, Reason))
    return std::nullopt;
  if (Globals)
    *Globals = std::move(ImportedGlobals);
  return Hint;
}

/// Bind a compiler-emitted lazy witness accessor as a source helper. The
/// compiler-level third swift_getWitnessTable operand is undef, so the
/// incidental live machine value is deliberately not exposed as an accessor
/// parameter. The complete accessor body above is the proof boundary.
inline std::optional<SourceCallTypeHint>
swiftWitnessAccessorCallHint(const HighFunc &Function, const BinaryImage &Image,
                             va_t *CacheAddress = nullptr) {
  const llvm::StringRef AccessorName(Function.Name);
  if (!AccessorName.ends_with("Wl"))
    return std::nullopt;
  const std::string CacheName = AccessorName.drop_back(1).str() + "L";
  std::optional<SourceCallTypeHint> Result;
  for (const auto &Symbol : Image.Symbols) {
    if (Symbol.IsFunc || Symbol.Name != CacheName)
      continue;
    const auto Cache =
        swiftWitnessCacheAddressHint(Function, Image, Symbol.Addr);
    if (!Cache)
      continue;
    if (Result)
      return std::nullopt;
    SourceCallTypeHint Hint;
    Hint.CallKind = SourceCallTypeHint::Kind::RuntimeSwiftWitnessAccessor;
    Hint.TargetAddress = Function.Entry;
    Hint.TargetName = Function.Name;
    Hint.Signature.Origin = SourceFunctionTypeHint::OriginKind::SwiftRuntime;
    Hint.Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
    std::string Reason;
    if (!assignDarwinScalarSourceABI(Hint.Signature, Image.Arch, Reason))
      return std::nullopt;
    Result = std::move(Hint);
    if (CacheAddress)
      *CacheAddress = Symbol.Addr;
  }
  return Result;
}

inline std::optional<va_t>
localStringPointerInitializer(const BinaryImage &Image, va_t Address,
                              uint64_t Width) {
  if (Width != 8 || Address % 8 ||
      !uniqueWritableDataSymbol(Image, Address, Width))
    return std::nullopt;
  const auto Target = readInitialImagePointer(Image, Address);
  return Target && constantStringSourceHint(Image, *Target) ? Target
                                                            : std::nullopt;
}

inline std::optional<SourceCallTypeHint>
localStorageHint(const BinaryImage &Image, va_t Address, uint64_t Width) {
  const auto *Symbol = uniqueWritableDataSymbol(Image, Address, Width);
  const auto *Bytes = Symbol ? Image.readVA(Address, Width) : nullptr;
  uint64_t Bits = 0;
  if (Bytes && Width <= 8)
    for (uint64_t I = 0; I < Width; ++I)
      Bits |= uint64_t(Bytes[I]) << (I * 8);
  if (!Symbol || !Bytes)
    return std::nullopt;
  if ((overlapsPointerStorage(Image, Address, Width) ||
       (Width <= 8 && isImagePointerBitPattern(Image, Bits, Width))) &&
      !localStringPointerInitializer(Image, Address, Width))
    return std::nullopt;
  SourceCallTypeHint Hint;
  Hint.CallKind = SourceCallTypeHint::Kind::RuntimeLocalStorageAddress;
  Hint.TargetAddress = Address;
  Hint.TargetName = Symbol->Name;
  Hint.ByteCount = Width;
  Hint.Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
  std::string Reason;
  if (!assignDarwinScalarSourceABI(Hint.Signature, Image.Arch, Reason))
    return std::nullopt;
  return Hint;
}

inline bool swiftStaticStringStorageSymbol(llvm::StringRef Name) {
  Name.consume_front("_");
  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = 8000;
  Options.MaxNodes = 1024;
  Options.MaxDepth = 64;
  Options.MaxMemoryBytes = 1024 * 1024;
  Options.MaxOperations = 100000;
  const auto Parsed = llvm::swiftDemangle(Name, Options);
  const auto Shape = [](const llvm::SwiftDemangleNode &Node,
                        llvm::StringRef Kind, size_t Children) {
    return Node.Kind == Kind && !Node.Text && !Node.Index &&
           Node.Children.size() == Children;
  };
  const auto Named = [](const llvm::SwiftDemangleNode &Node,
                        llvm::StringRef Kind) {
    return Node.Kind == Kind && Node.Text && !Node.Text->empty() &&
           !Node.Index && Node.Children.empty();
  };
  if (!Parsed.Root || !Parsed.Error.empty() ||
      !Shape(*Parsed.Root, "Global", 1) ||
      !Shape(Parsed.Root->Children[0], "Static", 1) ||
      !Shape(Parsed.Root->Children[0].Children[0], "Variable", 3))
    return false;
  const auto &Variable = Parsed.Root->Children[0].Children[0];
  const auto &Owner = Variable.Children[0];
  const auto &Property = Variable.Children[1];
  const auto &Type = Variable.Children[2];
  if ((Owner.Kind != "Structure" && Owner.Kind != "Class" &&
       Owner.Kind != "Enum") ||
      Owner.Text || Owner.Index || Owner.Children.size() != 2 ||
      !Named(Owner.Children[0], "Module") ||
      !Named(Owner.Children[1], "Identifier") ||
      !Named(Property, "Identifier") || !Shape(Type, "Type", 1))
    return false;
  const auto &String = Type.Children[0];
  return Shape(String, "Structure", 2) && Named(String.Children[0], "Module") &&
         *String.Children[0].Text == "Swift" &&
         Named(String.Children[1], "Identifier") &&
         *String.Children[1].Text == "String";
}

inline std::optional<SourceCallTypeHint>
swiftSmallStringStorageHint(const BinaryImage &Image, va_t Address) {
  constexpr uint64_t Width = 16;
  if (Image.Format != BinaryFormat::MachO || Image.Arch != Arch::AArch64 ||
      Image.Bits != Bitness::Bits64 || Image.IsRelocatable ||
      !Image.MachOTwoLevelNamespace || Image.MachOChainedFixupsAmbiguous ||
      !Address || Address % Width || Address > InvalidVA - Width)
    return std::nullopt;
  const auto Bytes = readImmutableImageBytes(Image, Address, Width);
  if (!Bytes || ((*Bytes)[15] & 0xf0) != 0xe0)
    return std::nullopt;
  const uint8_t Count = (*Bytes)[15] & 0x0f;
  for (unsigned I = 0; I < 15; ++I)
    if ((I < Count && (*Bytes)[I] >= 0x80) || (I >= Count && (*Bytes)[I] != 0))
      return std::nullopt;

  const Symbol *Storage = nullptr;
  for (const auto &Candidate : Image.Symbols) {
    if (Candidate.Addr > Address && Candidate.Addr < Address + Width)
      return std::nullopt;
    if (Candidate.Addr != Address)
      continue;
    if (Storage || Candidate.IsFunc || Candidate.Name.empty() ||
        (Candidate.Size && Candidate.Size < Width))
      return std::nullopt;
    Storage = &Candidate;
  }
  if (!Storage || !swiftStaticStringStorageSymbol(Storage->Name))
    return std::nullopt;
  size_t Exports = 0;
  for (const auto &Export : Image.Exports) {
    if (Export.Addr == Address && Export.Name == Storage->Name)
      ++Exports;
    else if (Export.Addr == Address || Export.Name == Storage->Name)
      return std::nullopt;
  }
  if (Exports != 1)
    return std::nullopt;
  SourceCallTypeHint Hint;
  Hint.CallKind = SourceCallTypeHint::Kind::RuntimeSwiftSmallStringAddress;
  Hint.TargetAddress = Address;
  Hint.TargetName = Storage->Name;
  Hint.ByteCount = Width;
  Hint.Signature.Origin = SourceFunctionTypeHint::OriginKind::SwiftRuntime;
  Hint.Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
  std::string Reason;
  return assignDarwinScalarSourceABI(Hint.Signature, Image.Arch, Reason)
             ? std::optional<SourceCallTypeHint>(std::move(Hint))
             : std::nullopt;
}

inline std::optional<SourceCallTypeHint>
swiftPrivateScalarStorageHint(const BinaryImage &Image, va_t Address) {
  const auto *Symbol = uniqueWritableDataSymbol(Image, Address, 1);
  const auto Width =
      Symbol ? swiftPrivateScalarStorageWidth(Symbol->Name) : std::nullopt;
  return Width ? localStorageHint(Image, Address, *Width) : std::nullopt;
}

// Darwin dispatch_once_t (also used by swift_once) is an intptr_t, initialized
// to zero in static storage. A completed token cannot be transplanted without
// its initialized state. Keep token storage shared through the normal helpers.
inline std::optional<SourceCallTypeHint>
oncePredicateStorageHint(const BinaryImage &Image, va_t Address) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.MachOChainedFixupsAmbiguous ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64) || Address % 8)
    return std::nullopt;
  auto Hint = localStorageHint(Image, Address, 8);
  if (!Hint) {
    const auto *Section = Image.getSectionFor(Address);
    const auto *Segment = Image.getSegmentFor(Address);
    if (!Section || !Segment || !Section->isReadable() ||
        !Section->isWritable() || Section->isExecutable() ||
        !Segment->isReadable() || !Segment->isWritable() ||
        Segment->isExecutable() ||
        (Section->Type & llvm::MachO::SECTION_TYPE) !=
            llvm::MachO::S_ZEROFILL ||
        Address < Section->VA || Address < Segment->VA ||
        8 > Section->Size - (Address - Section->VA) ||
        8 > Segment->Size - (Address - Segment->VA) ||
        overlapsPointerStorage(Image, Address, 8))
      return std::nullopt;
    for (const auto &Symbol : Image.Symbols) {
      if (Symbol.IsFunc)
        continue;
      if ((Symbol.Addr >= Address && Symbol.Addr < Address + 8) ||
          (Symbol.Size && Symbol.Addr <= InvalidVA - Symbol.Size &&
           Symbol.Addr < Address + 8 && Address < Symbol.Addr + Symbol.Size))
        return std::nullopt;
    }
    SourceCallTypeHint Anonymous;
    Anonymous.CallKind = SourceCallTypeHint::Kind::RuntimeLocalStorageAddress;
    Anonymous.TargetAddress = Address;
    Anonymous.TargetName =
        "dispatch_once_predicate_" + llvm::utohexstr(Address, true);
    Anonymous.ByteCount = 8;
    Anonymous.Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
    std::string Reason;
    if (!assignDarwinScalarSourceABI(Anonymous.Signature, Image.Arch, Reason))
      return std::nullopt;
    Hint = std::move(Anonymous);
  }
  const auto *Bytes = Hint ? Image.readVA(Address, 8) : nullptr;
  if (!Bytes ||
      !std::all_of(Bytes, Bytes + 8, [](uint8_t B) { return B == 0; }))
    return std::nullopt;
  return Hint;
}

/// Bind a direct scalar access inside named writable storage to the symbol's
/// base helper. Mach-O nlist entries commonly omit data-object sizes, so the
/// access itself proves only the required prefix. Keeping that prefix rooted
/// at the nearest exact symbol preserves aliases between independently
/// emitted field accesses without treating the distance to the next symbol as
/// an object extent.
inline std::optional<SourceCallTypeHint>
localStorageAccessHint(const BinaryImage &Image, va_t Address, uint64_t Width) {
  if (auto Exact = localStorageHint(Image, Address, Width))
    return Exact;
  if (!Address || !Width || Width > InvalidVA - Address)
    return std::nullopt;
  const auto *Section = Image.getSectionFor(Address);
  const Symbol *Base = nullptr;
  for (const auto &Symbol : Image.Symbols) {
    if (Symbol.IsFunc || Symbol.Name.empty() || Symbol.Addr >= Address ||
        llvm::StringRef(Symbol.Name).starts_with(kAutoFuncPrefix) ||
        Image.getSectionFor(Symbol.Addr) != Section)
      continue;
    if (!Base || Symbol.Addr > Base->Addr)
      Base = &Symbol;
  }
  if (!Base || Width > 1024 * 1024 ||
      Address - Base->Addr > 1024 * 1024 - Width)
    return std::nullopt;
  const uint64_t Extent = Address - Base->Addr + Width;
  if (Base->Size && Extent > Base->Size)
    return std::nullopt;
  return localStorageHint(Image, Base->Addr, Extent);
}

inline bool localStorageAccessTypeSupported(const TypeRef &Type) {
  // A pointer-typed load/store has the same complete machine-word extent as
  // its integer carrier. Storage and initializer proofs still authenticate
  // the cell; this does not bind the stored pointer to an image address.
  if (Type && Type->Kind == NdTypeKind::Ptr)
    return Type->Size == 8 && Type->Pointee;
  return Type &&
         (Type->Kind == NdTypeKind::Int || Type->Kind == NdTypeKind::Float) &&
         (Type->Kind != NdTypeKind::Float || Type->Size == 4 ||
          Type->Size == 8) &&
         (Type->Size == 1 || Type->Size == 2 || Type->Size == 4 ||
          Type->Size == 8 || Type->Size == 16);
}

/// Prove the extent of a named local storage address from an actual memory
/// access in this function. Swift's exclusivity marker receives the same
/// address, but its void-pointer ABI alone does not establish a byte width.
inline std::map<va_t, uint64_t>
directLocalStorageAccessExtents(const HighFunc &Function,
                                const BinaryImage &Image) {
  std::map<va_t, uint64_t> Result;
  auto Record = [&](const ExprPtr &Address, const TypeRef &Type,
                    NdMemoryOrdering Ordering,
                    NdMemoryAddressSpace AddressSpace) {
    if (!Address || !localStorageAccessTypeSupported(Type) ||
        Ordering != NdMemoryOrdering::None ||
        AddressSpace != NdMemoryAddressSpace::Default)
      return;
    const auto Value = constantAddress(*Address);
    if (!Value || !localStorageHint(Image, *Value, Type->Size))
      return;
    Result[*Value] = std::max<uint64_t>(Result[*Value], Type->Size);
  };
  std::set<const HighExpr *> Seen;
  std::function<void(const ExprPtr &)> Scan = [&](const ExprPtr &Expression) {
    if (!Expression || !Seen.insert(Expression.get()).second)
      return;
    if (Expression->Kind == ExprKind::Load && Expression->Operands.size() == 1)
      Record(Expression->Operands[0], Expression->Type,
             Expression->MemoryOrdering, Expression->MemoryAddressSpace);
    if (Expression->Kind == ExprKind::Store &&
        Expression->Operands.size() == 2 && Expression->Operands[1])
      Record(Expression->Operands[0], Expression->Operands[1]->Type,
             Expression->MemoryOrdering, Expression->MemoryAddressSpace);
    for (const auto &Operand : Expression->Operands)
      Scan(Operand);
  };
  walkStmts(Function.Body, [&](const HighStmt &Statement) {
    if (Statement.Kind == StmtKind::Store && Statement.StoreVal)
      Record(Statement.StoreAddr, Statement.StoreVal->Type,
             Statement.MemoryOrdering, Statement.MemoryAddressSpace);
    forEachExpr(Statement, Scan);
  });
  return Result;
}

inline std::optional<size_t> identityKeyParameter(const HighExpr &Expression,
                                                  const BinaryImage &Image) {
  if (Expression.Kind != ExprKind::Call || !Expression.SourceCallHint ||
      Expression.IntrinsicId != Intrinsic::None ||
      Expression.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Expression.MemoryOrdering != NdMemoryOrdering::None)
    return std::nullopt;
  const auto &Hint = *Expression.SourceCallHint;
  std::optional<size_t> Parameter;
  std::optional<SourceCallTypeHint> Expected;
  if (Hint.CallKind == SourceCallTypeHint::Kind::ObjCRuntimeCall &&
      (Hint.TargetName == "objc_getAssociatedObject" ||
       Hint.TargetName == "objc_setAssociatedObject")) {
    Parameter = 1;
    Expected = objcRuntimeSourceCallHint(Image, Hint.TargetAddress);
  } else if (Hint.CallKind == SourceCallTypeHint::Kind::DarwinRuntimeCall) {
    if (Hint.TargetName == "dispatch_get_specific")
      Parameter = 0;
    else if (Hint.TargetName == "dispatch_queue_get_specific")
      Parameter = 1;
    Expected = darwinRuntimeSourceCallHint(Image, Hint.TargetAddress);
  }
  if (!Parameter || !Expected || *Parameter >= Expression.Operands.size() ||
      Expression.Operands.size() != Hint.Signature.Parameters.size() ||
      Expected->TargetName != Hint.TargetName ||
      !objc_projection_detail::sameHint(Expected->Signature, Hint.Signature))
    return std::nullopt;
  return Parameter;
}

inline std::optional<size_t>
kvoRegistrationContextParameter(const HighExpr &Expression,
                                const BinaryImage &Image) {
  constexpr llvm::StringLiteral Selector =
      "addObserver:forKeyPath:options:context:";
  constexpr size_t ContextParameter = 5;
  if (Expression.Kind != ExprKind::Call || !Expression.SourceCallHint ||
      Expression.IsIndirectCall || Expression.IntrinsicId != Intrinsic::None ||
      Expression.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Expression.MemoryOrdering != NdMemoryOrdering::None)
    return std::nullopt;
  const auto &Hint = *Expression.SourceCallHint;
  if (Hint.CallKind != SourceCallTypeHint::Kind::ObjCMessage ||
      Hint.Selector != Selector || Hint.Format || Hint.NilTerminated ||
      Hint.SelectorResultUse || Hint.SelectorResultTypeUse ||
      Hint.SelectorArgumentTypeUse || Hint.SelectorForwardingUse ||
      Hint.SelectorArgumentStorageUse || Hint.ObjCIndirectResultStorage ||
      Hint.DoesNotReturn || Hint.WeakImport || Hint.ReturnedArgument ||
      Hint.RuntimeObjCResultType || Hint.ValueWitness ||
      !Hint.BorrowedByteInputs.empty() || !Hint.SwiftStringInputs.empty() ||
      Expression.Operands.size() != Hint.Signature.Parameters.size() ||
      ContextParameter >= Expression.Operands.size())
    return std::nullopt;
  std::optional<SourceFunctionTypeHint> Expected;
  if (Hint.Receiver) {
    const auto Declaration =
        objcReceiverSourceTypeHint(Image, Hint.Selector, *Hint.Receiver);
    if (Declaration.HasDeclaration && Declaration.Signature)
      Expected = *Declaration.Signature;
  } else if (Hint.Signature.Origin ==
             SourceFunctionTypeHint::OriginKind::ObjCSDK) {
    Expected = objcSelectorSourceTypeHint(Image, Hint.Selector);
  }
  if (!Expected ||
      !objc_projection_detail::sameHint(Hint.Signature, *Expected) ||
      !Expected->Parameters[ContextParameter].Type ||
      Expected->Parameters[ContextParameter].Type->Kind != NdTypeKind::Ptr)
    return std::nullopt;
  return ContextParameter;
}

inline std::optional<size_t>
kvoCallbackContextParameter(const HighFunc &Function,
                            const BinaryImage &Image) {
  constexpr llvm::StringLiteral Selector =
      "observeValueForKeyPath:ofObject:change:context:";
  constexpr size_t ContextParameter = 5;
  const ObjCMethod *Method = nullptr;
  for (const auto &Candidate : Image.ObjCMethods) {
    if (Candidate.Implementation != Function.Entry)
      continue;
    if (Method)
      return std::nullopt;
    Method = &Candidate;
  }
  if (!Method || Method->Status != "supported" || Method->IsClassMethod ||
      Method->Selector != Selector || !Method->TypeHint ||
      !Function.SourceTypeHint ||
      !objc_projection_detail::sameHint(*Method->TypeHint,
                                        *Function.SourceTypeHint) ||
      Function.Params.size() != Function.SourceTypeHint->Parameters.size() ||
      ContextParameter >= Function.Params.size() ||
      !Function.Params[ContextParameter].Type ||
      Function.Params[ContextParameter].Type->Kind != NdTypeKind::Ptr)
    return std::nullopt;
  for (size_t I = 0; I < Function.Params.size(); ++I)
    if (Function.Params[I].Name !=
            Function.SourceTypeHint->Parameters[I].Name ||
        !equalSourceTypes(Function.Params[I].Type,
                          Function.SourceTypeHint->Parameters[I].Type))
      return std::nullopt;
  return ContextParameter;
}

inline bool exactParameterValue(const ExprPtr &Value, size_t Parameter,
                                unsigned Depth = 0) {
  if (!Value || Depth > 8 || !Value->Type || Value->Type->Size != 8)
    return false;
  if (Value->Kind == ExprKind::Var)
    return Value->Var.Kind == MedVar::Param && Value->Var.Id >= 0 &&
           static_cast<size_t>(Value->Var.Id) == Parameter &&
           Value->Var.Size == 8;
  if ((Value->Kind == ExprKind::Cast || Value->Kind == ExprKind::BitCast) &&
      Value->Operands.size() == 1 && Value->Operands[0] &&
      Value->Operands[0]->Type && Value->Operands[0]->Type->Size == 8)
    return exactParameterValue(Value->Operands[0], Parameter, Depth + 1);
  return false;
}

inline std::optional<SourceCallTypeHint> profileStorageHint(Arch Architecture,
                                                            va_t Base) {
  SourceCallTypeHint Hint;
  Hint.CallKind = SourceCallTypeHint::Kind::RuntimeProfileCounterStorage;
  Hint.TargetAddress = Base;
  Hint.Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
  std::string Reason;
  if (!assignDarwinScalarSourceABI(Hint.Signature, Architecture, Reason))
    return std::nullopt;
  return Hint;
}

/// A selected storage pointer may be rebuilt only when every definition is an
/// authenticated cell and every use remains a bounded access to that cell.
/// The source helper is never allowed to escape through a call or return.
struct SelectedStorageSeed {
  SourceCallTypeHint Hint;
  va_t Address = 0;
  bool Profile = false;
};
inline std::map<const HighExpr *, SelectedStorageSeed>
selectedStorageSeeds(const HighFunc &Function, const BinaryImage &Image,
                     const ObjCProfileStorage &Storage) {
  VarKeyMap<std::vector<ExprPtr>> Definitions;
  VarKeySet Candidates, UnsupportedKinds;
  walkStmts(Function.Body, [&](const HighStmt &Statement) {
    if (Statement.Kind == StmtKind::Assign && Statement.Dst && Statement.Val &&
        (Statement.Dst->Kind == ExprKind::Var ||
         Statement.Dst->Kind == ExprKind::Phi)) {
      const auto Key = varKey(Statement.Dst->Var);
      Definitions[Key].push_back(Statement.Val);
      if (Statement.Dst->Var.Kind == MedVar::Temp ||
          Statement.Dst->Var.Kind == MedVar::Reg)
        Candidates.insert(Key);
      else
        UnsupportedKinds.insert(Key);
    }
  });
  std::map<const HighExpr *, SelectedStorageSeed> Seeds;
  std::optional<bool> FlowValid;
  for (const auto &[Key, Values] : Definitions) {
    if (!Candidates.count(Key) || UnsupportedKinds.count(Key) ||
        Values.size() < 2 || Values.size() > 64)
      continue;
    std::vector<va_t> Addresses;
    std::optional<va_t> Base;
    std::optional<bool> Profile;
    std::vector<SourceCallTypeHint> Hints;
    bool Valid = true;
    for (const auto &Value : Values) {
      if (!Value || Value->Kind != ExprKind::Const || !Value->Type ||
          Value->Type->Size != 8 ||
          !isExactAddressProvenance(Value->ConstProvenance) ||
          isCodeAddressProvenance(Value->ConstProvenance) ||
          (Value->AddressOwnerVA != InvalidVA &&
           Value->AddressOwnerVA != Value->ConstVal)) {
        Valid = false;
        break;
      }
      const auto Section = Storage.sectionFor(Value->ConstVal, 1);
      auto Hint = Section ? profileStorageHint(Image.Arch, *Section)
                          : localStorageHint(Image, Value->ConstVal, 8);
      if (!Hint || (Profile && *Profile != bool(Section)) ||
          (Section && Base && *Base != *Section)) {
        Valid = false;
        break;
      }
      Profile = bool(Section);
      if (Section)
        Base = Section;
      Hints.push_back(std::move(*Hint));
      Addresses.push_back(Value->ConstVal);
    }
    if (!Valid || !Profile)
      continue;
    if (!FlowValid) {
      const auto Flow = analyzeHighSourceFlow(Function, false);
      FlowValid = Flow.Complete && Flow.Items.empty();
    }
    if (!*FlowValid)
      continue;
    std::set<const HighExpr *> SeedNodes;
    for (const auto &Value : Values)
      SeedNodes.insert(Value.get());
    const auto ContainsVar = [&](auto &&Self, const ExprPtr &E,
                                 unsigned Depth) -> bool {
      if (!E || Depth > 64)
        return false;
      if ((E->Kind == ExprKind::Var || E->Kind == ExprKind::Phi) &&
          varKey(E->Var) == Key)
        return true;
      for (const auto &Operand : E->Operands)
        if (Self(Self, Operand, Depth + 1))
          return true;
      return false;
    };
    const auto Offset = [&](auto &&Self, const ExprPtr &E,
                            unsigned Depth) -> std::optional<uint64_t> {
      if (!E || !E->Type || E->Type->Size != 8 || Depth > 16)
        return std::nullopt;
      if ((E->Kind == ExprKind::Var || E->Kind == ExprKind::Phi) &&
          varKey(E->Var) == Key)
        return 0;
      if ((E->Kind == ExprKind::Cast || E->Kind == ExprKind::BitCast) &&
          E->Operands.size() == 1)
        return Self(Self, E->Operands[0], Depth + 1);
      if (E->Kind == ExprKind::BinOp && E->Op == NdOp::INT_ADD &&
          E->Operands.size() == 2)
        for (unsigned I = 0; I < 2; ++I) {
          const auto Inner = Self(Self, E->Operands[I], Depth + 1);
          const auto &Constant = E->Operands[1 - I];
          if (Inner && Constant && Constant->Kind == ExprKind::Const &&
              (Constant->ConstProvenance == ConstantAddressProvenance::Scalar ||
               Constant->ConstProvenance ==
                   ConstantAddressProvenance::Unknown) &&
              Constant->ConstVal <= 4096 &&
              *Inner <= UINT64_MAX - Constant->ConstVal)
            return *Inner + Constant->ConstVal;
        }
      return std::nullopt;
    };
    const auto SafeAccess = [&](const ExprPtr &Address, const TypeRef &Type,
                                NdMemoryOrdering Ordering,
                                NdMemoryAddressSpace Space) {
      if (!Type || !Type->Size || Ordering != NdMemoryOrdering::None ||
          Space != NdMemoryAddressSpace::Default)
        return false;
      const auto Delta = Offset(Offset, Address, 0);
      if (!Delta)
        return false;
      if (!*Profile)
        return !*Delta && Type->Size == 8 &&
               (Type->Kind == NdTypeKind::Int || Type->Kind == NdTypeKind::Ptr);
      if (Type->Kind != NdTypeKind::Int || Type->Size > 16)
        return false;
      for (va_t Candidate : Addresses)
        if (Candidate > UINT64_MAX - *Delta ||
            Storage.sectionFor(Candidate + *Delta, Type->Size) != Base)
          return false;
      return true;
    };
    const auto SafeExpression = [&](auto &&Self, const ExprPtr &E,
                                    unsigned Depth) -> bool {
      if (!E || Depth > 64)
        return !E;
      if (SeedNodes.count(E.get()))
        return false;
      if (E->Kind == ExprKind::Load && E->Operands.size() == 1 &&
          ContainsVar(ContainsVar, E->Operands[0], 0))
        return SafeAccess(E->Operands[0], E->Type, E->MemoryOrdering,
                          E->MemoryAddressSpace);
      if ((E->Kind == ExprKind::Var || E->Kind == ExprKind::Phi) &&
          varKey(E->Var) == Key)
        return false;
      for (const auto &Operand : E->Operands)
        if (!Self(Self, Operand, Depth + 1))
          return false;
      return true;
    };
    walkStmts(Function.Body, [&](const HighStmt &Statement) {
      if (!Valid)
        return;
      if (Statement.StoreAddr &&
          ContainsVar(ContainsVar, Statement.StoreAddr, 0) &&
          !SafeAccess(Statement.StoreAddr,
                      Statement.StoreVal ? Statement.StoreVal->Type : nullptr,
                      Statement.MemoryOrdering, Statement.MemoryAddressSpace))
        Valid = false;
      forEachExpr(Statement, [&](const ExprPtr &Root) {
        if ((Root == Statement.Dst && Statement.Kind == StmtKind::Assign &&
             Statement.Dst &&
             (Statement.Dst->Kind == ExprKind::Var ||
              Statement.Dst->Kind == ExprKind::Phi)) ||
            (Root == Statement.StoreAddr &&
             ContainsVar(ContainsVar, Root, 0)) ||
            (Statement.Kind == StmtKind::Assign && Statement.Dst &&
             (Statement.Dst->Kind == ExprKind::Var ||
              Statement.Dst->Kind == ExprKind::Phi) &&
             varKey(Statement.Dst->Var) == Key && Root == Statement.Val))
          return;
        if (!SafeExpression(SafeExpression, Root, 0))
          Valid = false;
      });
    });
    if (Valid)
      for (size_t I = 0; I < Values.size(); ++I)
        Seeds.emplace(Values[I].get(),
                      SelectedStorageSeed{Hints[I], Addresses[I], *Profile});
  }
  return Seeds;
}

/// A storage address may cross a native source-call boundary only when the
/// complete typed callee proves that this parameter is used solely as the
/// exact address of bounded scalar loads/stores. The source ABI's pointer type
/// alone says nothing about pointee width or escape behavior.
inline std::optional<uint64_t> nativeScalarStorageArgumentExtent(
    const HighFunc &Function, size_t Parameter, bool AllowPointerValues,
    bool AllowStores = true,
    const std::map<va_t, const HighFunc *> *Functions = nullptr) {
  size_t Budget = 100000;
  std::set<std::pair<const HighFunc *, size_t>> Active;
  const auto Analyze = [&](const auto &Self, const HighFunc &Current,
                           size_t CurrentParameter,
                           unsigned CallDepth) -> std::optional<uint64_t> {
    if (!Budget || CallDepth > 32 || !Current.SourceTypeHint ||
        Current.Params.size() != Current.SourceTypeHint->Parameters.size() ||
        CurrentParameter >= Current.Params.size() ||
        !Current.Params[CurrentParameter].Type ||
        Current.Params[CurrentParameter].Type->Kind != NdTypeKind::Ptr ||
        !Current.SourceTypeHint->Parameters[CurrentParameter].Type ||
        Current.SourceTypeHint->Parameters[CurrentParameter].Type->Kind !=
            NdTypeKind::Ptr ||
        !equalSourceTypes(
            Current.Params[CurrentParameter].Type,
            Current.SourceTypeHint->Parameters[CurrentParameter].Type))
      return std::nullopt;
    std::string ABIError;
    if (!validateSourceABI(*Current.SourceTypeHint, ABIError) ||
        !Active.emplace(&Current, CurrentParameter).second)
      return std::nullopt;
    for (size_t I = 0; I < Current.Params.size(); ++I)
      if (Current.Params[I].Name !=
              Current.SourceTypeHint->Parameters[I].Name ||
          !equalSourceTypes(Current.Params[I].Type,
                            Current.SourceTypeHint->Parameters[I].Type)) {
        Active.erase({&Current, CurrentParameter});
        return std::nullopt;
      }

    bool Valid = true, Used = false;
    uint64_t Extent = 0;
    auto Contains = [&](const auto &ContainsSelf, const ExprPtr &Expression,
                        unsigned Depth) -> bool {
      if (!Expression || !Budget || Depth > 200)
        return false;
      --Budget;
      if (Expression->Kind == ExprKind::Var &&
          Expression->Var.Kind == MedVar::Param && Expression->Var.Id >= 0 &&
          static_cast<size_t>(Expression->Var.Id) == CurrentParameter)
        return true;
      for (const auto &Operand : Expression->Operands)
        if (ContainsSelf(ContainsSelf, Operand, Depth + 1))
          return true;
      return false;
    };
    auto Access = [&](const ExprPtr &Pointer, const TypeRef &Type,
                      NdMemoryOrdering Ordering,
                      NdMemoryAddressSpace AddressSpace, bool Store) {
      if (!Valid || !Contains(Contains, Pointer, 0))
        return;
      const bool SupportedOrdering =
          Ordering == NdMemoryOrdering::None ||
          (Store && AllowStores && Ordering == NdMemoryOrdering::Release);
      if (!exactParameterValue(Pointer, CurrentParameter) || !Type ||
          (!AllowPointerValues && Type->Kind == NdTypeKind::Ptr) ||
          (Store && !AllowStores) || !localStorageAccessTypeSupported(Type) ||
          !SupportedOrdering || AddressSpace != NdMemoryAddressSpace::Default) {
        Valid = false;
        return;
      }
      Extent = std::max<uint64_t>(Extent, Type->Size);
      Used = true;
    };
    std::function<void(const ExprPtr &, unsigned)> Scan;
    Scan = [&](const ExprPtr &Expression, unsigned Depth) {
      if (!Expression || !Valid)
        return;
      if (!Budget || Depth > 200) {
        Valid = false;
        return;
      }
      --Budget;
      if (Expression->Kind == ExprKind::Load &&
          Expression->Operands.size() == 1) {
        Access(Expression->Operands[0], Expression->Type,
               Expression->MemoryOrdering, Expression->MemoryAddressSpace,
               false);
        if (Contains(Contains, Expression->Operands[0], 0))
          return;
      }
      if (Expression->Kind == ExprKind::Store &&
          Expression->Operands.size() == 2 && Expression->Operands[1]) {
        Access(Expression->Operands[0], Expression->Operands[1]->Type,
               Expression->MemoryOrdering, Expression->MemoryAddressSpace,
               true);
        if (Contains(Contains, Expression->Operands[0], 0)) {
          Scan(Expression->Operands[1], Depth + 1);
          return;
        }
      }
      if (Functions && Expression->Kind == ExprKind::Call &&
          Expression->SourceCallHint && !Expression->IsIndirectCall &&
          Expression->IntrinsicId == Intrinsic::None &&
          Expression->IntrinsicOutputs.empty() &&
          Expression->MemoryOrdering == NdMemoryOrdering::None &&
          Expression->MemoryAddressSpace == NdMemoryAddressSpace::Default &&
          Expression->SourceCallHint->CallKind ==
              SourceCallTypeHint::Kind::Native) {
        const auto &Binding = *Expression->SourceCallHint;
        const auto Callee = Functions->find(Binding.TargetAddress);
        bool Forwarded = false;
        for (size_t I = 0; I < Expression->Operands.size(); ++I) {
          if (!Contains(Contains, Expression->Operands[I], 0))
            continue;
          Forwarded = true;
          if (!exactParameterValue(Expression->Operands[I], CurrentParameter) ||
              Expression->CallAddr != Binding.TargetAddress ||
              Callee == Functions->end() || !Callee->second ||
              !Callee->second->SourceTypeHint ||
              Binding.Signature.Parameters.size() !=
                  Expression->Operands.size() ||
              !objc_projection_detail::sameHint(
                  Binding.Signature, *Callee->second->SourceTypeHint)) {
            Valid = false;
            return;
          }
          const auto ForwardedExtent =
              Self(Self, *Callee->second, I, CallDepth + 1);
          if (!ForwardedExtent) {
            Valid = false;
            return;
          }
          Extent = std::max(Extent, *ForwardedExtent);
          Used = true;
        }
        if (Forwarded)
          return;
      }
      if (Expression->Kind == ExprKind::Var &&
          Expression->Var.Kind == MedVar::Param && Expression->Var.Id >= 0 &&
          static_cast<size_t>(Expression->Var.Id) == CurrentParameter) {
        Valid = false;
        return;
      }
      for (const auto &Operand : Expression->Operands)
        Scan(Operand, Depth + 1);
    };
    walkStmts(Current.Body, [&](const HighStmt &Statement) {
      const bool DirectStore = Statement.Kind == StmtKind::Store &&
                               Statement.StoreVal &&
                               Contains(Contains, Statement.StoreAddr, 0);
      if (DirectStore)
        Access(Statement.StoreAddr, Statement.StoreVal->Type,
               Statement.MemoryOrdering, Statement.MemoryAddressSpace, true);
      forEachExpr(Statement, [&](const ExprPtr &Expression) {
        if (DirectStore && Expression == Statement.StoreAddr)
          return;
        Scan(Expression, 0);
      });
    });
    Active.erase({&Current, CurrentParameter});
    return Valid && Used && Budget ? std::optional<uint64_t>(Extent)
                                   : std::nullopt;
  };
  return Analyze(Analyze, Function, Parameter, 0);
}

inline std::optional<va_t>
nativeProfileCounterArgument(const HighFunc &Function, size_t Parameter,
                             va_t Address, const ObjCProfileStorage &Storage) {
  const auto Extent =
      nativeScalarStorageArgumentExtent(Function, Parameter, false);
  return Extent ? Storage.sectionFor(Address, *Extent) : std::nullopt;
}

struct ClassObjectIdentity {
  SourceCallTypeHint::Kind Kind;
  std::string Name;
};

inline std::optional<SourceCallTypeHint>
classReferenceAddressHint(const BinaryImage &Image, va_t Address) {
  const auto Found = Image.ObjCSourceReferences.find(Address);
  if (Found == Image.ObjCSourceReferences.end() ||
      Found->second.Address != Address || Found->second.Size != 8 ||
      Found->second.Name.empty() || Found->second.Name.size() > 1024)
    return std::nullopt;
  const auto Kind = Found->second.TheKind;
  if (Kind != ObjCSourceReference::Kind::Class &&
      Kind != ObjCSourceReference::Kind::Metaclass)
    return std::nullopt;
  for (const char C : Found->second.Name)
    if (!llvm::isAlnum(C) && C != '_' && C != '.' && C != '$')
      return std::nullopt;
  SourceCallTypeHint Hint;
  Hint.CallKind =
      Kind == ObjCSourceReference::Kind::Class
          ? SourceCallTypeHint::Kind::RuntimeClassReferenceAddress
          : SourceCallTypeHint::Kind::RuntimeMetaclassReferenceAddress;
  Hint.TargetAddress = Address;
  Hint.TargetName = Found->second.Name;
  Hint.Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
  std::string Reason;
  if (!assignDarwinScalarSourceABI(Hint.Signature, Image.Arch, Reason))
    return std::nullopt;
  return Hint;
}

inline std::map<va_t, ClassObjectIdentity>
classObjectIdentities(const BinaryImage &Image) {
  std::map<va_t, ClassObjectIdentity> Result;
  std::set<va_t> Conflicts;
  std::map<std::string, size_t> NameCounts;
  for (const auto &Class : Image.ObjCClasses)
    ++NameCounts[Class.Name];
  const objc::RuntimeData Data(Image);
  auto Publish = [&](va_t Address, ClassObjectIdentity Identity) {
    if (Conflicts.count(Address))
      return;
    auto [It, Inserted] = Result.emplace(Address, Identity);
    if (!Inserted && (It->second.Kind != Identity.Kind ||
                      It->second.Name != Identity.Name)) {
      Result.erase(It);
      Conflicts.insert(Address);
    }
  };
  for (const auto &Class : Image.ObjCClasses) {
    if (Class.Name.empty() || NameCounts[Class.Name] != 1)
      continue;
    const auto RO = Data.classRO(Class.Address);
    const auto Flags = RO ? Data.u32(*RO) : std::nullopt;
    const auto Name = Data.className(Class.Address);
    if (!Flags || (*Flags & 1) || !Name || *Name != Class.Name)
      continue;
    Publish(Class.Address,
            {SourceCallTypeHint::Kind::RuntimeClass, Class.Name});
    const auto Meta = Data.pointer(Class.Address);
    const auto MetaRO = Meta ? Data.classRO(*Meta) : std::nullopt;
    const auto MetaFlags = MetaRO ? Data.u32(*MetaRO) : std::nullopt;
    const auto MetaName = Meta ? Data.className(*Meta) : std::nullopt;
    if (MetaFlags && (*MetaFlags & 1) && MetaName && *MetaName == Class.Name)
      Publish(*Meta, {SourceCallTypeHint::Kind::RuntimeMetaclass, Class.Name});
  }
  return Result;
}

inline std::optional<uint64_t> constantAddress(const HighExpr &Expression,
                                               unsigned Depth) {
  const auto Integer = [&](const auto &Self, const HighExpr &E,
                           unsigned CurrentDepth) -> std::optional<uint64_t> {
    if (CurrentDepth > 128 || !E.Type || !E.Type->Size || E.Type->Size > 8 ||
        (E.Type->Kind != NdTypeKind::Int && E.Type->Kind != NdTypeKind::Ptr))
      return std::nullopt;
    const auto Mask = [](uint16_t Bytes) {
      return Bytes == 8 ? UINT64_MAX : (UINT64_C(1) << (Bytes * 8)) - 1;
    };
    const auto Normalize = [&](uint64_t Value) {
      return Value & Mask(E.Type->Size);
    };
    if (E.Kind == ExprKind::Const)
      return Normalize(E.ConstVal);
    if ((E.Kind == ExprKind::Cast || E.Kind == ExprKind::BitCast) &&
        E.Operands.size() == 1 && E.Operands[0] && E.Operands[0]->Type) {
      const auto Value = Self(Self, *E.Operands[0], CurrentDepth + 1);
      if (!Value || (E.Kind == ExprKind::BitCast &&
                     E.Operands[0]->Type->Size != E.Type->Size))
        return std::nullopt;
      uint64_t Result = *Value & Mask(E.Operands[0]->Type->Size);
      if (E.Kind == ExprKind::Cast &&
          E.Type->Size > E.Operands[0]->Type->Size &&
          E.Operands[0]->Type->IsSigned) {
        const unsigned Bits = E.Operands[0]->Type->Size * 8;
        if (Result & (UINT64_C(1) << (Bits - 1)))
          Result |= ~Mask(E.Operands[0]->Type->Size);
      }
      return Normalize(Result);
    }
    if (E.Kind == ExprKind::UnaryOp && E.Operands.size() == 1 &&
        E.Operands[0] && E.Operands[0]->Type) {
      const auto Value = Self(Self, *E.Operands[0], CurrentDepth + 1);
      if (!Value)
        return std::nullopt;
      uint64_t Result = *Value & Mask(E.Operands[0]->Type->Size);
      if (E.Op == NdOp::INT_SEXT && E.Type->Size >= E.Operands[0]->Type->Size) {
        const unsigned Bits = E.Operands[0]->Type->Size * 8;
        if (Result & (UINT64_C(1) << (Bits - 1)))
          Result |= ~Mask(E.Operands[0]->Type->Size);
      } else if (E.Op != NdOp::INT_ZEXT && E.Op != NdOp::INT_NOT &&
                 E.Op != NdOp::INT_NEGATE) {
        return std::nullopt;
      }
      if (E.Op == NdOp::INT_NOT)
        Result = ~Result;
      if (E.Op == NdOp::INT_NEGATE)
        Result = uint64_t(0) - Result;
      return Normalize(Result);
    }
    if (E.Kind != ExprKind::BinOp || E.Operands.size() != 2 || !E.Operands[0] ||
        !E.Operands[1] || !E.Operands[0]->Type || !E.Operands[1]->Type)
      return std::nullopt;
    const auto Left = Self(Self, *E.Operands[0], CurrentDepth + 1);
    const auto Right = Self(Self, *E.Operands[1], CurrentDepth + 1);
    if (!Left || !Right)
      return std::nullopt;
    uint64_t Result = 0;
    switch (E.Op) {
    case NdOp::INT_ADD:
      Result = *Left + *Right;
      break;
    case NdOp::INT_SUB:
      Result = *Left - *Right;
      break;
    case NdOp::INT_MULT:
      Result = *Left * *Right;
      break;
    case NdOp::INT_AND:
      Result = *Left & *Right;
      break;
    case NdOp::INT_OR:
      Result = *Left | *Right;
      break;
    case NdOp::INT_XOR:
      Result = *Left ^ *Right;
      break;
    case NdOp::INT_LEFT:
    case NdOp::INT_RIGHT:
    case NdOp::INT_ASHR: {
      const unsigned Bits = E.Operands[0]->Type->Size * 8;
      if (*Right >= Bits)
        return std::nullopt;
      const uint64_t Input = *Left & Mask(E.Operands[0]->Type->Size);
      if (E.Op == NdOp::INT_LEFT)
        Result = Input << *Right;
      else if (E.Op == NdOp::INT_RIGHT)
        Result = Input >> *Right;
      else if (!*Right)
        Result = Input;
      else {
        Result = Input >> *Right;
        if (Input & (UINT64_C(1) << (Bits - 1)))
          Result |= UINT64_MAX << (Bits - *Right);
      }
      break;
    }
    case NdOp::CONCAT: {
      const unsigned LowBits = E.Operands[1]->Type->Size * 8;
      if (E.Type->Size !=
              E.Operands[0]->Type->Size + E.Operands[1]->Type->Size ||
          LowBits >= 64)
        return std::nullopt;
      Result = (*Left << LowBits) | *Right;
      break;
    }
    case NdOp::SUBBYTES: {
      if (*Right > E.Operands[0]->Type->Size ||
          E.Type->Size > E.Operands[0]->Type->Size - *Right)
        return std::nullopt;
      Result = *Left >> (*Right * 8);
      break;
    }
    default:
      return std::nullopt;
    }
    return Normalize(Result);
  };
  if (Depth > 128 || !Expression.Type || Expression.Type->Size != 8)
    return std::nullopt;
  return Integer(Integer, Expression, Depth);
}

inline SourceCallTypeHint::Kind runtimeKind(ObjCSourceReference::Kind Kind) {
  using K = SourceCallTypeHint::Kind;
  switch (Kind) {
  case ObjCSourceReference::Kind::Selector:
    return K::RuntimeSelector;
  case ObjCSourceReference::Kind::Class:
    return K::RuntimeClass;
  case ObjCSourceReference::Kind::Metaclass:
    return K::RuntimeMetaclass;
  case ObjCSourceReference::Kind::IvarOffset:
    return K::RuntimeIvarOffset;
  case ObjCSourceReference::Kind::Protocol:
    return K::RuntimeProtocol;
  }
  return K::Native;
}

struct MergedIvarOffsetPlans {
  std::map<const HighExpr *, ObjCSourceReference> Leaves;
  std::map<const HighExpr *, TypeRef> Loads;
};

/// Clang may merge addresses of two ivar-offset cells through a local and load
/// the selected cell after the branch. Rebuild the source values before that
/// merge only when every reaching definition is an authenticated ivar offset
/// from the same class and the address-valued local has no other use.
inline MergedIvarOffsetPlans mergedIvarOffsetPlans(const HighFunc &Function,
                                                   const BinaryImage &Image) {
  using Local = HighSourceLocalIdentity;
  std::map<Local, std::vector<ExprPtr>> Definitions;
  walkStmts(Function.Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Assign && S.Dst && S.Val &&
        (S.Dst->Kind == ExprKind::Var || S.Dst->Kind == ExprKind::Phi) &&
        S.Dst->Operands.empty() && S.Dst->Type && S.Dst->Type->Size == 8 &&
        (S.Dst->Var.Kind == MedVar::Reg || S.Dst->Var.Kind == MedVar::Temp))
      Definitions[highSourceLocalIdentity(S.Dst->Var)].push_back(S.Val);
  });
  struct Resolution {
    std::map<const HighExpr *, ObjCSourceReference> Leaves;
    std::set<Local> Locals;
    std::string ClassName;
  };
  auto Resolve = [&](const auto &Self, const ExprPtr &Value, uint16_t Width,
                     std::set<Local> &Visiting,
                     unsigned Depth) -> std::optional<Resolution> {
    if (!Value || Depth > 64 || !Value->Type || Value->Type->Size != 8 ||
        Value->IntrinsicId != Intrinsic::None ||
        !Value->IntrinsicOutputs.empty() ||
        Value->MemoryOrdering != NdMemoryOrdering::None ||
        Value->MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return std::nullopt;
    if ((Value->Kind == ExprKind::Cast || Value->Kind == ExprKind::BitCast) &&
        Value->Operands.size() == 1)
      return Self(Self, Value->Operands[0], Width, Visiting, Depth + 1);
    if (Value->Kind == ExprKind::Const && Value->Operands.empty()) {
      const auto Address = constantAddress(*Value);
      const auto Found = Address ? Image.ObjCSourceReferences.find(*Address)
                                 : Image.ObjCSourceReferences.end();
      if (Found == Image.ObjCSourceReferences.end() ||
          Found->second.TheKind != ObjCSourceReference::Kind::IvarOffset ||
          Found->second.Size != Width || Found->second.Name.empty() ||
          Found->second.ClassName.empty())
        return std::nullopt;
      Resolution Result;
      Result.Leaves.emplace(Value.get(), Found->second);
      Result.ClassName = Found->second.ClassName;
      return Result;
    }
    std::vector<ExprPtr> Sources;
    std::optional<Local> Identity;
    if ((Value->Kind == ExprKind::Var || Value->Kind == ExprKind::Phi) &&
        !Value->Operands.empty()) {
      Sources = Value->Operands;
    } else if ((Value->Kind == ExprKind::Var || Value->Kind == ExprKind::Phi) &&
               Value->Operands.empty() &&
               (Value->Var.Kind == MedVar::Reg ||
                Value->Var.Kind == MedVar::Temp)) {
      Identity = highSourceLocalIdentity(Value->Var);
      const auto Found = Definitions.find(*Identity);
      if (Found == Definitions.end() || Found->second.empty() ||
          !Visiting.insert(*Identity).second)
        return std::nullopt;
      Sources = Found->second;
    } else {
      return std::nullopt;
    }
    Resolution Result;
    for (const auto &Source : Sources) {
      auto Part = Self(Self, Source, Width, Visiting, Depth + 1);
      if (!Part ||
          (!Result.ClassName.empty() && Result.ClassName != Part->ClassName)) {
        if (Identity)
          Visiting.erase(*Identity);
        return std::nullopt;
      }
      Result.ClassName = Part->ClassName;
      Result.Locals.insert(Part->Locals.begin(), Part->Locals.end());
      for (const auto &[Leaf, Reference] : Part->Leaves) {
        auto [It, Fresh] = Result.Leaves.emplace(Leaf, Reference);
        if (!Fresh && (It->second.Address != Reference.Address ||
                       It->second.Name != Reference.Name ||
                       It->second.ClassName != Reference.ClassName)) {
          if (Identity)
            Visiting.erase(*Identity);
          return std::nullopt;
        }
      }
    }
    if (Identity)
      Visiting.erase(*Identity);
    if (Identity)
      Result.Locals.insert(*Identity);
    return Result.Leaves.empty() ? std::nullopt
                                 : std::optional<Resolution>(std::move(Result));
  };

  std::vector<ExprPtr> Candidates;
  size_t Budget = 100000;
  walkStmts(Function.Body, [&](const HighStmt &S) {
    forEachRhsExpr(S, [&](const ExprPtr &Root) {
      std::vector<ExprPtr> Pending{Root};
      std::set<const HighExpr *> Seen;
      while (!Pending.empty() && Budget) {
        --Budget;
        auto E = Pending.back();
        Pending.pop_back();
        if (!E || !Seen.insert(E.get()).second)
          continue;
        if (E->Kind == ExprKind::Load && E->Operands.size() == 1 && E->Type &&
            E->Type->Kind == NdTypeKind::Int &&
            (E->Type->Size == 4 || E->Type->Size == 8) &&
            !constantAddress(*E->Operands[0]) &&
            E->IntrinsicId == Intrinsic::None && E->IntrinsicOutputs.empty() &&
            E->MemoryOrdering == NdMemoryOrdering::None &&
            E->MemoryAddressSpace == NdMemoryAddressSpace::Default) {
          std::set<Local> Visiting;
          if (Resolve(Resolve, E->Operands[0], E->Type->Size, Visiting, 0))
            Candidates.push_back(E);
        }
        Pending.insert(Pending.end(), E->Operands.begin(), E->Operands.end());
      }
    });
  });
  MergedIvarOffsetPlans Plans;
  if (!Budget)
    return Plans;
  for (const auto &Candidate : Candidates) {
    std::set<Local> Visiting;
    const auto Current = Resolve(Resolve, Candidate->Operands[0],
                                 Candidate->Type->Size, Visiting, 0);
    if (!Current)
      continue;
    bool Valid = true;
    size_t UsesBudget = 100000;
    const auto Related = [&](const Resolution &Other) {
      for (const auto &[Leaf, Reference] : Other.Leaves) {
        (void)Reference;
        if (Current->Leaves.count(Leaf))
          return true;
      }
      for (const auto &Local : Other.Locals)
        if (Current->Locals.count(Local))
          return true;
      return false;
    };
    walkStmts(Function.Body, [&](const HighStmt &S) {
      if (!Valid || !UsesBudget)
        return;
      forEachRhsExpr(S, [&](const ExprPtr &Root) {
        if (!Valid || !UsesBudget || !Root)
          return;
        std::set<Local> RootVisiting;
        const auto RootResolution =
            Resolve(Resolve, Root, Candidate->Type->Size, RootVisiting, 0);
        const bool AliasDefinition =
            S.Kind == StmtKind::Assign && Root == S.Val && S.Dst &&
            (S.Dst->Kind == ExprKind::Var || S.Dst->Kind == ExprKind::Phi) &&
            RootResolution && Related(*RootResolution);
        if (AliasDefinition)
          return;
        const auto Visit = [&](const auto &Self, const ExprPtr &E,
                               unsigned Depth) -> void {
          if (!Valid || !E || !UsesBudget || Depth > 128) {
            Valid = false;
            return;
          }
          --UsesBudget;
          if (E.get() == Candidate.get())
            return;
          std::set<Local> Active;
          const auto Use =
              Resolve(Resolve, E, Candidate->Type->Size, Active, 0);
          if (Use && Related(*Use)) {
            Valid = false;
            return;
          }
          for (const auto &Operand : E->Operands)
            Self(Self, Operand, Depth + 1);
        };
        Visit(Visit, Root, 0);
      });
    });
    if (!Valid || !UsesBudget)
      continue;
    bool Conflict = false;
    for (const auto &[Leaf, Reference] : Current->Leaves) {
      const auto Existing = Plans.Leaves.find(Leaf);
      if (Existing != Plans.Leaves.end() &&
          (Existing->second.Address != Reference.Address ||
           Existing->second.Name != Reference.Name ||
           Existing->second.ClassName != Reference.ClassName))
        Conflict = true;
    }
    if (Conflict)
      continue;
    Plans.Loads.emplace(Candidate.get(), Candidate->Type);
    Plans.Leaves.insert(Current->Leaves.begin(), Current->Leaves.end());
  }
  return Plans;
}

inline std::optional<uint64_t> swiftLiteralStorageWord(const ExprPtr &Value) {
  size_t Budget = 256;
  const auto Evaluate = [&](auto &&Self, const ExprPtr &Current,
                            unsigned Depth) -> std::optional<uint64_t> {
    if (!Current || Depth > 16 || !Budget || !Current->Type ||
        Current->Type->Size != 8 ||
        (Current->Type->Kind != NdTypeKind::Int &&
         Current->Type->Kind != NdTypeKind::Ptr))
      return std::nullopt;
    --Budget;
    if (Current->Kind == ExprKind::Const)
      return Current->ConstVal;
    if (Current->Kind == ExprKind::Cast && Current->Operands.size() == 1)
      return Self(Self, Current->Operands[0], Depth + 1);
    if (Current->Kind != ExprKind::BinOp || Current->Operands.size() != 2)
      return std::nullopt;
    const auto Left = Self(Self, Current->Operands[0], Depth + 1);
    const auto Right = Self(Self, Current->Operands[1], Depth + 1);
    if (!Left || !Right)
      return std::nullopt;
    if (Current->Op == NdOp::INT_OR)
      return *Left | *Right;
    if (Current->Op == NdOp::INT_ADD)
      return *Left + *Right;
    if (Current->Op == NdOp::INT_SUB)
      return *Left - *Right;
    return std::nullopt;
  };
  return Evaluate(Evaluate, Value, 0);
}

inline bool containsBorrowedStorage(const ExprPtr &Value) {
  std::vector<const HighExpr *> Pending{Value.get()};
  std::set<const HighExpr *> Seen;
  size_t Budget = 1000;
  while (!Pending.empty()) {
    if (!Budget--)
      return true;
    const auto *Current = Pending.back();
    Pending.pop_back();
    if (!Current || !Seen.insert(Current).second)
      continue;
    if (Current->SourceCallHint &&
        Current->SourceCallHint->CallKind ==
            SourceCallTypeHint::Kind::RuntimeBorrowedBytes)
      return true;
    for (const auto &Child : Current->Operands)
      Pending.push_back(Child.get());
  }
  return false;
}

inline const HighExpr *swiftLiteralStorageHelper(const ExprPtr &Value) {
  if (!Value || Value->Kind != ExprKind::BinOp || Value->Op != NdOp::INT_OR ||
      !Value->Type || Value->Type->Kind != NdTypeKind::Int ||
      Value->Type->Size != 8 || Value->Operands.size() != 2 ||
      !Value->Operands[1] ||
      swiftLiteralStorageWord(Value->Operands[1]) !=
          SwiftLiteralString::ImmortalTag)
    return nullptr;
  const auto &Address = Value->Operands[0];
  if (!Address || Address->Kind != ExprKind::BinOp ||
      Address->Op != NdOp::INT_SUB || !Address->Type ||
      Address->Type->Kind != NdTypeKind::Int || Address->Type->Size != 8 ||
      Address->Operands.size() != 2 || !Address->Operands[1] ||
      swiftLiteralStorageWord(Address->Operands[1]) !=
          SwiftLiteralString::StorageBias)
    return nullptr;
  const auto &Helper = Address->Operands[0];
  if (!Helper || Helper->Kind != ExprKind::Call || Helper->IsIndirectCall ||
      !Helper->Type || Helper->Type->Size != 8 || !Helper->Operands.empty() ||
      !Helper->SourceCallHint ||
      Helper->SourceCallHint->CallKind !=
          SourceCallTypeHint::Kind::RuntimeBorrowedBytes)
    return nullptr;
  return Helper.get();
}

inline bool isRuntimeReference(SourceCallTypeHint::Kind Kind) {
  using K = SourceCallTypeHint::Kind;
  return Kind == K::RuntimeSelector || Kind == K::RuntimeClass ||
         Kind == K::RuntimeMetaclass || Kind == K::RuntimeIvarOffset ||
         Kind == K::RuntimeProtocol;
}

inline std::optional<size_t>
taggedCStringAddressOperand(const HighExpr &Expression,
                            const BinaryImage &Image) {
  if (Expression.Kind != ExprKind::BinOp || Expression.Op != NdOp::INT_OR ||
      !Expression.Type || Expression.Type->Kind != NdTypeKind::Int ||
      Expression.Type->Size != 8 || Expression.Operands.size() != 2)
    return std::nullopt;
  for (size_t Index = 0; Index != 2; ++Index) {
    const auto &Address = Expression.Operands[Index];
    const auto &Tag = Expression.Operands[1 - Index];
    const auto Word = [](const ExprPtr &Value) {
      return Value && Value->Kind == ExprKind::Const && Value->Type &&
             Value->Type->Kind == NdTypeKind::Int && Value->Type->Size == 8;
    };
    if (!Word(Address) || !Word(Tag) ||
        !isExactAddressProvenance(Address->ConstProvenance) ||
        isCodeAddressProvenance(Address->ConstProvenance) ||
        (Address->AddressOwnerVA != InvalidVA &&
         Address->AddressOwnerVA != Address->ConstVal) ||
        Tag->ConstVal != SwiftLiteralString::ImmortalTag ||
        Tag->ConstProvenance != ConstantAddressProvenance::Scalar ||
        Tag->AddressOwnerVA != InvalidVA || (Address->ConstVal & Tag->ConstVal))
      continue;
    const auto *Section = Image.getSectionFor(Address->ConstVal);
    if (Section && cstringStorageSourceHint(Image, Section->VA))
      return Index;
  }
  return std::nullopt;
}

} // namespace objc_binding_detail

/// Clone before attaching relocation bindings: other native exports keep the
/// original HighIR. A load from a proven runtime slot is a runtime query; the
/// address of that slot is never itself replaced with the loaded value.
inline ObjCSourceBindingResult bindObjCSourceReferences(
    const HighFunc &Function, const BinaryImage &Image,
    const ObjCProfileStorage *ProfileStorage = nullptr,
    const std::map<va_t, const HighFunc *> *Functions = nullptr) {
  using namespace objc_binding_detail;
  ObjCSourceBindingResult Result{Function};
  std::optional<ObjCProfileStorage> LocalStorage;
  if (!ProfileStorage) {
    LocalStorage.emplace(Image);
    ProfileStorage = &*LocalStorage;
  }
  // Most functions never consume a direct class-object constant. Defer this
  // whole-image proof until such a use exists, but keep it local to this bind
  // so another call always validates the current image again.
  std::optional<std::map<va_t, ClassObjectIdentity>> ClassObjects;
  auto ClassObjectAt = [&](va_t Address) -> const ClassObjectIdentity * {
    if (!ClassObjects)
      ClassObjects.emplace(classObjectIdentities(Image));
    const auto Found = ClassObjects->find(Address);
    return Found == ClassObjects->end() ? nullptr : &Found->second;
  };
  auto ScalarLoads = readOnlyScalarLoadPlans(Function, Image);
  const auto LoopBytes = readOnlyLoopBytePlans(Function, Image);
  for (const auto &[Load, Plan] : LoopBytes.Loads)
    ScalarLoads.emplace(Load, Plan);
  const auto MergedIvarOffsets = mergedIvarOffsetPlans(Function, Image);
  const auto ObjectPointerLoads =
      readOnlyObjectPointerLoadPlans(Function, Image);
  const auto ObjectPointerConsumers =
      readOnlyObjectPointerLoadConsumers(Function, ObjectPointerLoads);
  const auto StorageSeeds =
      selectedStorageSeeds(Function, Image, *ProfileStorage);
  const auto DirectLocalStorage =
      directLocalStorageAccessExtents(Function, Image);
  const auto KVOCallbackParameter =
      kvoCallbackContextParameter(Function, Image);
  // A metadata pair can be moved through locals before an outlined helper
  // call. Bind its defining address only when every read of each local is a
  // direct argument of an exact, typed call carrying the proven pair.
  using MetadataLocal = HighSourceLocalIdentity;
  using MetadataAlias =
      std::pair<va_t, SourceCallTypeHint::SwiftTypeMetadataAddress>;
  struct MetadataDefinition {
    va_t Address = 0;
    unsigned Count = 0;
    bool Valid = false;
  };
  std::map<MetadataLocal, MetadataDefinition> MetadataDefinitions;
  walkStmts(Function.Body, [&](const HighStmt &Statement) {
    if (Statement.Kind != StmtKind::Assign || !Statement.Dst ||
        (Statement.Dst->Kind != ExprKind::Var &&
         Statement.Dst->Kind != ExprKind::Phi) ||
        !Statement.Dst->Operands.empty() ||
        (Statement.Dst->Var.Kind != MedVar::Reg &&
         Statement.Dst->Var.Kind != MedVar::Temp))
      return;
    auto &Definition =
        MetadataDefinitions[highSourceLocalIdentity(Statement.Dst->Var)];
    ++Definition.Count;
    const auto Address =
        Statement.Val ? constantAddress(*Statement.Val) : std::nullopt;
    Definition.Valid = Definition.Count == 1 && Address &&
                       Statement.Val->Kind == ExprKind::Const &&
                       Statement.Val->Type && Statement.Val->Type->Size == 8;
    if (Definition.Valid)
      Definition.Address = *Address;
  });
  std::map<MetadataLocal, unsigned> MetadataReads, MetadataPairedReads;
  std::map<MetadataLocal, MetadataAlias> MetadataPairCandidates;
  std::set<MetadataLocal> MetadataConflicts;
  size_t MetadataScanBudget = 1000000;
  bool MetadataScanComplete = true;
  const auto MetadataLocalAt =
      [&](const ExprPtr &Value) -> std::optional<MetadataLocal> {
    if (!Value ||
        (Value->Kind != ExprKind::Var && Value->Kind != ExprKind::Phi) ||
        !Value->Operands.empty() ||
        (Value->Var.Kind != MedVar::Reg && Value->Var.Kind != MedVar::Temp))
      return std::nullopt;
    const auto Key = highSourceLocalIdentity(Value->Var);
    const auto Found = MetadataDefinitions.find(Key);
    if (Found == MetadataDefinitions.end() || !Found->second.Valid)
      return std::nullopt;
    return Key;
  };
  const auto MetadataAddressAt =
      [&](const ExprPtr &Value) -> std::optional<va_t> {
    if (!Value)
      return std::nullopt;
    if (const auto Local = MetadataLocalAt(Value))
      return MetadataDefinitions.at(*Local).Address;
    return constantAddress(*Value);
  };
  std::function<void(const ExprPtr &, unsigned)> ScanMetadata =
      [&](const ExprPtr &Value, unsigned Depth) {
        if (!Value)
          return;
        if (Depth > 200 || !MetadataScanBudget--) {
          MetadataScanComplete = false;
          return;
        }
        if (const auto Local = MetadataLocalAt(Value))
          ++MetadataReads[*Local];
        if (Value->Kind == ExprKind::Call && Value->SourceCallHint &&
            Value->SourceCallHint->CallKind ==
                SourceCallTypeHint::Kind::Native &&
            Value->Operands.size() >= 2 && Value->Operands.size() <= 4 &&
            Value->SourceCallHint->Signature.Parameters.size() ==
                Value->Operands.size()) {
          const auto &Signature = Value->SourceCallHint->Signature;
          std::string Reason;
          const bool PointerParameters = std::all_of(
              Signature.Parameters.begin(), Signature.Parameters.end(),
              [](const auto &Parameter) {
                return Parameter.Type &&
                       Parameter.Type->Kind == NdTypeKind::Ptr;
              });
          if (PointerParameters && validateSourceABI(Signature, Reason))
            for (size_t I = 0; I < Value->Operands.size(); ++I)
              for (size_t J = I + 1; J < Value->Operands.size(); ++J) {
                if (Value->Operands.size() == 4 && (I != 2 || J != 3))
                  continue;
                // Direct constants are handled by the ordinary call binder.
                // This prepass only needs pairs involving a local alias.
                if (!MetadataLocalAt(Value->Operands[I]) &&
                    !MetadataLocalAt(Value->Operands[J]))
                  continue;
                const auto First = MetadataAddressAt(Value->Operands[I]);
                const auto Second = MetadataAddressAt(Value->Operands[J]);
                if (!First || !Second)
                  continue;
                auto Pair = swiftTypeMetadataPair(Image, *First, *Second);
                if (!Pair)
                  Pair = swiftTypeMetadataPair(Image, *Second, *First);
                if (!Pair)
                  continue;
                for (const auto Index : {I, J})
                  if (const auto Local =
                          MetadataLocalAt(Value->Operands[Index])) {
                    ++MetadataPairedReads[*Local];
                    const MetadataAlias Alias{
                        MetadataDefinitions.at(*Local).Address, *Pair};
                    const auto [Found, Fresh] =
                        MetadataPairCandidates.emplace(*Local, Alias);
                    if (!Fresh && Found->second != Alias)
                      MetadataConflicts.insert(*Local);
                  }
              }
        }
        for (const auto &Operand : Value->Operands)
          ScanMetadata(Operand, Depth + 1);
      };
  walkStmts(Function.Body, [&](const HighStmt &Statement) {
    if (Statement.Dst)
      for (const auto &Operand : Statement.Dst->Operands)
        ScanMetadata(Operand, 0);
    forEachRhsExpr(Statement,
                   [&](const ExprPtr &Value) { ScanMetadata(Value, 0); });
  });
  std::map<MetadataLocal, MetadataAlias> MetadataAliasPlans;
  if (MetadataScanComplete)
    for (const auto &[Local, Alias] : MetadataPairCandidates)
      if (!MetadataConflicts.count(Local) && MetadataReads[Local] &&
          MetadataReads[Local] == MetadataPairedReads[Local] &&
          swiftTypeMetadataAddressHint(Image, Alias.first, Alias.second))
        MetadataAliasPlans.emplace(Local, Alias);
  if (!MetadataAliasPlans.empty()) {
    const auto Flow = analyzeHighSourceFlow(Function, false);
    if (!Flow.Complete || !Flow.Items.empty())
      MetadataAliasPlans.clear();
  }
  // A control-flow-selected format remains an address-valued local until the
  // call. Trace only that operand's complete definition family so unrelated
  // scalar occurrences with the same bits never acquire object identity.
  VarKeyMap<std::vector<ExprPtr>> FormatDefinitions;
  walkStmts(Function.Body, [&](const HighStmt &Statement) {
    if (Statement.Kind == StmtKind::Assign && Statement.Dst && Statement.Val &&
        (Statement.Dst->Kind == ExprKind::Var ||
         Statement.Dst->Kind == ExprKind::Phi))
      FormatDefinitions[varKey(Statement.Dst->Var)].push_back(Statement.Val);
  });
  std::set<const HighExpr *> FormatObjectExpressions;
  std::set<const HighExpr *> FormatScanSeen;
  std::function<void(const ExprPtr &)> ScanFormats =
      [&](const ExprPtr &Expression) {
        if (!Expression || !FormatScanSeen.insert(Expression.get()).second)
          return;
        if (Expression->SourceCallHint && Expression->SourceCallHint->Format) {
          const auto &Format = *Expression->SourceCallHint->Format;
          const auto Addresses = formatAddresses(Format);
          if (Addresses &&
              Format.FormatParameter < Expression->Operands.size()) {
            const std::set<va_t> Candidates(Addresses->begin(),
                                            Addresses->end());
            std::set<const HighExpr *> Leaves;
            std::set<VarKey> Active;
            size_t TraceBudget = 4096;
            const auto Trace = [&](auto &&Self, const ExprPtr &Value,
                                   unsigned Depth) -> bool {
              if (!Value || !TraceBudget-- || Depth > 64)
                return false;
              if ((Value->Kind == ExprKind::Cast ||
                   Value->Kind == ExprKind::BitCast) &&
                  Value->Operands.size() == 1 && Value->Type &&
                  Value->Type->Size == 8)
                return Self(Self, Value->Operands.front(), Depth + 1);
              if (Value->Kind == ExprKind::Call && Value->SourceCallHint &&
                  Value->SourceCallHint->CallKind ==
                      SourceCallTypeHint::Kind::RuntimeConstantString)
                return Value->Operands.empty() &&
                       Candidates.count(Value->SourceCallHint->TargetAddress) !=
                           0;
              if (Value->Kind == ExprKind::Const) {
                if (!Candidates.count(Value->ConstVal))
                  return false;
                Leaves.insert(Value.get());
                return true;
              }
              if (Value->Kind == ExprKind::BinOp && Value->Op == NdOp::SELECT &&
                  Value->Operands.size() == 3)
                return Self(Self, Value->Operands[1], Depth + 1) &&
                       Self(Self, Value->Operands[2], Depth + 1);
              if (Value->Kind != ExprKind::Var && Value->Kind != ExprKind::Phi)
                return false;
              const auto Key = varKey(Value->Var);
              if (!Active.insert(Key).second)
                return false;
              const auto Found = FormatDefinitions.find(Key);
              bool Valid =
                  Found != FormatDefinitions.end() && !Found->second.empty();
              if (Valid)
                for (const auto &Definition : Found->second)
                  if (!Self(Self, Definition, Depth + 1)) {
                    Valid = false;
                    break;
                  }
              Active.erase(Key);
              return Valid;
            };
            if (Trace(Trace, Expression->Operands[Format.FormatParameter], 0))
              FormatObjectExpressions.insert(Leaves.begin(), Leaves.end());
          }
        }
        for (const auto &Operand : Expression->Operands)
          ScanFormats(Operand);
      };
  walkStmts(Function.Body, [&](const HighStmt &Statement) {
    forEachExpr(Statement, ScanFormats);
  });
  // Contextual bindings create temporary input nodes. Retain those nodes for
  // the lifetime of their memoized copies so allocator address reuse cannot
  // make a later argument borrow an earlier argument's binding.
  using CopyKey = std::tuple<ExprPtr, bool, bool, bool>;
  std::map<CopyKey, ExprPtr> Copies;
  size_t Budget = 1000000;
  va_t StatementAddress = 0;
  auto Fail = [&](const char *Message, const HighExpr *Expression = nullptr) {
    if (Result.Limitation.empty())
      Result.Limitation = Message;
    const bool IsData = Expression != nullptr;
    if (!IsData)
      Result.Diagnostics.Complete = false;
    Result.Diagnostics.add(IsData ? SourceProjectionIssue::DataBinding
                                  : SourceProjectionIssue::Budget,
                           Message, StatementAddress, Expression,
                           IsData ? Expression->ConstVal : 0);
  };
  auto BindMemoryAddress = [&](ExprPtr &Operand, const TypeRef &Type,
                               NdMemoryOrdering Ordering,
                               NdMemoryAddressSpace AddressSpace) {
    if (!Operand || !Type || AddressSpace != NdMemoryAddressSpace::Default ||
        !localStorageAccessTypeSupported(Type))
      return false;
    const auto Address = constantAddress(*Operand);
    auto WitnessCache =
        Address && Type->Size == 8 &&
                (Ordering == NdMemoryOrdering::None ||
                 Ordering == NdMemoryOrdering::Release)
            ? swiftWitnessCacheAddressHint(Function, Image, *Address)
            : std::nullopt;
    if (WitnessCache) {
      auto Bound = HighExpr::makeCall({}, 0, {});
      Bound->Type = NdType::makeInt(8, false);
      Bound->SourceCallHint =
          std::make_shared<SourceCallTypeHint>(std::move(*WitnessCache));
      Operand = std::move(Bound);
      Result.SwiftWitnessCaches[*Address] = Function.Entry;
      return true;
    }
    if (Ordering != NdMemoryOrdering::None)
      return false;
    const auto ProfileBase =
        Address ? ProfileStorage->sectionFor(*Address, Type->Size)
                : std::nullopt;
    // Profiling sections have a separate numeric-counter contract. A pointer
    // access may use a proved named cell, not acquire that counter identity.
    if (ProfileBase && Type->Kind == NdTypeKind::Ptr)
      return false;
    auto Base = ProfileBase;
    auto Hint = Base ? profileStorageHint(Image.Arch, *Base) : std::nullopt;
    if (!Hint) {
      Hint = Address ? localStorageAccessHint(Image, *Address, Type->Size)
                     : std::nullopt;
      if (!Hint)
        return false;
      Base = Hint->TargetAddress;
      Result.LocalStorageExtents[*Base] = std::max<uint64_t>(
          Result.LocalStorageExtents[*Base], Hint->ByteCount);
    }
    auto Bound = HighExpr::makeCall({}, 0, {});
    Bound->Type = NdType::makeInt(8, false);
    Bound->SourceCallHint =
        std::make_shared<SourceCallTypeHint>(std::move(*Hint));
    Operand =
        !Base || *Address == *Base
            ? Bound
            : HighExpr::makeBinop(NdOp::INT_ADD, Bound,
                                  HighExpr::makeConst(*Address - *Base, 8));
    if (ProfileBase)
      Result.ProfileCounterSections.insert(*ProfileBase);
    return true;
  };
  std::function<ExprPtr(const ExprPtr &, unsigned, bool, bool, bool)> Copy;
  Copy = [&](const ExprPtr &Original, unsigned Depth, bool NumericOperand,
             bool AddressContext, bool MemoryAddress) -> ExprPtr {
    if (!Original)
      return nullptr;
    if (Depth > 200 || !Budget) {
      Fail("source reference projection exceeds its complexity budget");
      return HighExpr::makeUndef(Original->Type ? Original->Type->Size : 8);
    }
    --Budget;
    AddressContext |= Original->Type && Original->Type->Kind == NdTypeKind::Ptr;
    const CopyKey Key{Original, NumericOperand, AddressContext, MemoryAddress};
    if (auto Found = Copies.find(Key); Found != Copies.end())
      return Found->second;
    auto Expression = std::make_shared<HighExpr>(*Original);
    Copies.emplace(Key, Expression);
    if (const auto Found = LoopBytes.Initializers.find(Original.get());
        Found != LoopBytes.Initializers.end()) {
      *Expression = *HighExpr::makeConst(Found->second, 8,
                                         ConstantAddressProvenance::Scalar);
      return Expression;
    }
    if (const auto Found = MergedIvarOffsets.Leaves.find(Original.get());
        Found != MergedIvarOffsets.Leaves.end()) {
      const auto &Reference = Found->second;
      auto Binding = std::make_shared<SourceCallTypeHint>();
      Binding->CallKind = SourceCallTypeHint::Kind::RuntimeIvarOffset;
      Binding->TargetAddress = Reference.Address;
      Binding->TargetName = Reference.Name;
      Binding->OwnerClass = Reference.ClassName;
      auto &Hint = Binding->Signature;
      Hint.Architecture = Image.Arch;
      Hint.HasExplicitABI = true;
      Hint.ReturnType = NdType::makeInt(Reference.Size, false);
      Hint.ReturnLocation = {SourceABICarrierKind::IntegerRegister,
                             getTargetRegInfo(Image.Arch).IntReturnReg, 0,
                             Reference.Size};
      *Expression = *HighExpr::makeCall({}, 0, {});
      Expression->Type = Hint.ReturnType;
      Expression->SourceCallHint = std::move(Binding);
      Result.InstanceLayoutClasses.insert(Reference.ClassName);
      return Expression;
    }
    if (const auto Found = MergedIvarOffsets.Loads.find(Original.get());
        Found != MergedIvarOffsets.Loads.end()) {
      auto Value = Copy(Original->Operands[0], Depth + 1, false, false, false);
      auto Cast = std::make_shared<HighExpr>();
      Cast->Kind = ExprKind::Cast;
      Cast->Type = Found->second;
      Cast->CastTo = Found->second;
      Cast->Operands = {std::move(Value)};
      Copies[Key] = Cast;
      return Cast;
    }
    // A file-private Swift scalar has a compiler-encoded byte width. Rebuild
    // its exact data-address value at its defining occurrence so aliases used
    // by both exclusivity and identity-only APIs retain one storage address.
    if (Original->Kind == ExprKind::Const && Original->Type &&
        Original->Type->Size == 8 &&
        (Original->ConstProvenance == ConstantAddressProvenance::DataAddress ||
         Original->ConstProvenance == ConstantAddressProvenance::Address) &&
        !NumericOperand && !MemoryAddress) {
      if (auto Storage =
              swiftPrivateScalarStorageHint(Image, Original->ConstVal)) {
        *Expression = *HighExpr::makeCall({}, 0, {});
        Expression->Type = Original->Type;
        Expression->SourceCallHint =
            std::make_shared<SourceCallTypeHint>(std::move(*Storage));
        Result.LocalStorageExtents[Original->ConstVal] =
            Expression->SourceCallHint->ByteCount;
        return Expression;
      }
    }
    // Machine pointer stores use integer carriers. Preserve the occurrence's
    // complete address provenance even without a pointer-typed consumer; a
    // stored constant object must retain the identity of a directly used one.
    const bool ObjectAddress =
        Original->Kind == ExprKind::Const && Original->Type &&
        Original->Type->Kind == NdTypeKind::Int && Original->Type->Size == 8 &&
        isExactAddressProvenance(Original->ConstProvenance) &&
        !isCodeAddressProvenance(Original->ConstProvenance) &&
        (Original->AddressOwnerVA == InvalidVA ||
         Original->AddressOwnerVA == Original->ConstVal);
    const bool AuthenticatedFormatObject =
        Original->Kind == ExprKind::Const &&
        FormatObjectExpressions.count(Original.get()) != 0;
    if ((AddressContext || ObjectAddress || AuthenticatedFormatObject) &&
        !MemoryAddress && !NumericOperand &&
        !(Original->Kind == ExprKind::Const &&
          Original->ConstProvenance == ConstantAddressProvenance::Scalar)) {
      const auto Address = constantAddress(*Original);
      // An immutable self-pointer's value and storage address are the same
      // identity. Preserve its pointer-sized contents as well as that alias.
      // Mutable storage cannot acquire an address binding from its initializer.
      if (ObjectAddress && Address)
        if (auto Identity = staticIdentityHint(Image, *Address, true)) {
          *Expression = *HighExpr::makeCall({}, 0, {});
          Expression->Type = Original->Type;
          Expression->SourceCallHint =
              std::make_shared<SourceCallTypeHint>(std::move(*Identity));
          Result.StaticIdentities.insert(*Address);
          return Expression;
        }
      if (Address)
        if (auto Storage = swiftSmallStringStorageHint(Image, *Address)) {
          *Expression = *HighExpr::makeCall({}, 0, {});
          Expression->Type = Original->Type;
          Expression->SourceCallHint =
              std::make_shared<SourceCallTypeHint>(std::move(*Storage));
          Result.SwiftSmallStrings.insert(*Address);
          return Expression;
        }
      auto Hint =
          Address ? constantStringSourceHint(Image, *Address) : std::nullopt;
      if (!Hint && Address)
        Hint = constantObjectSourceHint(Image, *Address);
      if (Hint) {
        (Hint->CallKind == SourceCallTypeHint::Kind::RuntimeConstantString
             ? Result.ConstantStrings
             : Result.ConstantObjects)
            .insert(*Address);
        *Expression = *HighExpr::makeCall({}, 0, {});
        Expression->Type = Original->Type;
        Expression->SourceCallHint =
            std::make_shared<SourceCallTypeHint>(std::move(*Hint));
        return Expression;
      }
      if (Address) {
        const auto *Section = Image.getSectionFor(*Address);
        auto CString = Section ? cstringStorageSourceHint(Image, Section->VA)
                               : std::nullopt;
        if (CString) {
          const auto Base = CString->TargetAddress;
          auto Storage = HighExpr::makeCall({}, 0, {});
          Storage->Type = NdType::makeInt(8, false);
          Storage->SourceCallHint =
              std::make_shared<SourceCallTypeHint>(std::move(*CString));
          if (*Address != Base)
            Storage = HighExpr::makeBinop(
                NdOp::INT_ADD, Storage,
                HighExpr::makeConst(*Address - Base, 8,
                                    ConstantAddressProvenance::Scalar));
          *Expression = *Storage;
          Result.CStringSections.insert(Base);
          return Expression;
        }
      }
      if (const auto Seed = StorageSeeds.find(Original.get());
          Seed != StorageSeeds.end() && Address) {
        const auto &Proof = Seed->second;
        if (Proof.Hint.TargetAddress && *Address == Proof.Address) {
          auto Storage = HighExpr::makeCall({}, 0, {});
          Storage->Type = NdType::makeInt(8, false);
          Storage->SourceCallHint =
              std::make_shared<SourceCallTypeHint>(Proof.Hint);
          *Expression = *(*Address == Proof.Hint.TargetAddress
                              ? Storage
                              : HighExpr::makeBinop(
                                    NdOp::INT_ADD, Storage,
                                    HighExpr::makeConst(
                                        *Address - Proof.Hint.TargetAddress, 8,
                                        ConstantAddressProvenance::Scalar)));
          if (Proof.Profile)
            Result.ProfileCounterSections.insert(Proof.Hint.TargetAddress);
          else
            Result.LocalStorageExtents[Proof.Hint.TargetAddress] =
                std::max<uint64_t>(
                    Result.LocalStorageExtents[Proof.Hint.TargetAddress],
                    Proof.Hint.ByteCount);
          return Expression;
        }
      }
    }
    if (Original->Kind == ExprKind::Load && Original->Type &&
        Original->MemoryAddressSpace == NdMemoryAddressSpace::Default &&
        Original->MemoryOrdering == NdMemoryOrdering::None &&
        Original->Operands.size() == 1 && Original->Operands[0]) {
      auto Address = constantAddress(*Original->Operands[0]);
      if (Address && Original->Type->Size == 8 &&
          (Original->Type->Kind == NdTypeKind::Int ||
           Original->Type->Kind == NdTypeKind::Ptr)) {
        if (auto Hint = staticIdentityHint(Image, *Address)) {
          *Expression = *HighExpr::makeCall({}, 0, {});
          Expression->Type = Original->Type;
          Expression->SourceCallHint =
              std::make_shared<SourceCallTypeHint>(std::move(*Hint));
          Result.StaticIdentities.insert(*Address);
          return Expression;
        }
        if (auto Hint = darwinRuntimeGlobalAddressHint(Image, *Address)) {
          *Expression = *HighExpr::makeCall({}, 0, {});
          Expression->Type = Original->Type;
          Expression->SourceCallHint =
              std::make_shared<SourceCallTypeHint>(std::move(*Hint));
          return Expression;
        }
      }
      // A relocated immutable slot supplies an object value, never a source
      // identity for the storage address. Keep the slot proof for validation.
      if (Address && !NumericOperand && !MemoryAddress &&
          Original->Type->Size == 8 &&
          (Original->Type->Kind == NdTypeKind::Int ||
           Original->Type->Kind == NdTypeKind::Ptr)) {
        const auto Target = readImmutableImagePointer(Image, *Address);
        auto Hint = Target ? constantStringSourceHint(Image, *Target, *Address)
                           : std::nullopt;
        if (!Hint && Target)
          Hint = constantObjectSourceHint(Image, *Target, *Address);
        if (!Hint && Target) {
          const auto *Section = Image.getSectionFor(*Target);
          Hint = Section
                     ? cstringStorageSourceHint(Image, Section->VA, *Address)
                     : std::nullopt;
          if (Hint) {
            *Expression = *HighExpr::makeCall({}, 0, {});
            Expression->Type = Original->Type;
            Expression->SourceCallHint =
                std::make_shared<SourceCallTypeHint>(std::move(*Hint));
            Result.CStringSections.insert(Section->VA);
            Result.CStringPointerSlots.insert(*Address);
            return Expression;
          }
        }
        if (Hint) {
          (Hint->CallKind == SourceCallTypeHint::Kind::RuntimeConstantString
               ? Result.ConstantStrings
               : Result.ConstantObjects)
              .insert(*Target);
          *Expression = *HighExpr::makeCall({}, 0, {});
          Expression->Type = Original->Type;
          Expression->SourceCallHint =
              std::make_shared<SourceCallTypeHint>(std::move(*Hint));
          return Expression;
        }
      }
      auto Found = Address ? Image.ObjCSourceReferences.find(*Address)
                           : Image.ObjCSourceReferences.end();
      if (Found != Image.ObjCSourceReferences.end()) {
        const auto &Reference = Found->second;
        const bool Ivar =
            Reference.TheKind == ObjCSourceReference::Kind::IvarOffset;
        if (Original->Type->Size == Reference.Size ||
            (Ivar && Reference.Size == 8 && Original->Type->Size == 4)) {
          auto Binding = std::make_shared<SourceCallTypeHint>();
          Binding->CallKind = runtimeKind(Reference.TheKind);
          Binding->TargetAddress = Reference.Address;
          Binding->TargetName = Reference.Name;
          Binding->OwnerClass = Reference.ClassName;
          if (Ivar)
            Result.InstanceLayoutClasses.insert(Reference.ClassName);
          if (Reference.TheKind == ObjCSourceReference::Kind::Protocol)
            Result.RuntimeProtocols.insert(Reference.Name);
          auto &Hint = Binding->Signature;
          Hint.Architecture = Image.Arch;
          Hint.HasExplicitABI = true;
          Hint.ReturnType = Ivar ? NdType::makeInt(Original->Type->Size, false)
                                 : NdType::makePtr(NdType::makeVoid());
          Hint.ReturnLocation = {SourceABICarrierKind::IntegerRegister,
                                 getTargetRegInfo(Image.Arch).IntReturnReg, 0,
                                 Hint.ReturnType->Size};
          Expression->Kind = ExprKind::Call;
          Expression->CallAddr = 0;
          Expression->CallTarget.clear();
          Expression->SourceCallHint = std::move(Binding);
          Expression->Operands.clear();
          return Expression;
        }
      }
      const auto &Type = Original->Type;
      const bool Scalar =
          (Type->Kind == NdTypeKind::Int &&
           (Type->Size == 1 || Type->Size == 2 || Type->Size == 4 ||
            Type->Size == 8 || Type->Size == 16)) ||
          (Type->Kind == NdTypeKind::Float &&
           (Type->Size == 4 || Type->Size == 8));
      if (Address && Scalar && !AddressContext && !MemoryAddress) {
        if (const auto Bytes =
                readImmutableImageBytes(Image, *Address, Type->Size)) {
          uint64_t Bits = 0, Upper = 0;
          for (unsigned I = 0; I < Bytes->size(); ++I)
            (I < 8 ? Bits : Upper) |= uint64_t((*Bytes)[I]) << ((I % 8) * 8);
          // Reproduce a scalar value, never an original image pointer. The
          // byte reader excludes mutable, overlapping and relocated storage;
          // mapped values remain subject to ordinary address binding.
          // Wide integer carriers may contain two pointer-sized lanes. Neither
          // lane may transplant an image address into the emitted constant.
          const auto LaneWidth = std::min<uint16_t>(Type->Size, 8);
          if (!isImagePointerBitPattern(Image, Bits, LaneWidth) &&
              !isImagePointerBitPattern(Image, Upper, LaneWidth)) {
            auto Value = HighExpr::makeConst(Bits, LaneWidth,
                                             ConstantAddressProvenance::Scalar);
            if (Type->Size == 16)
              Value = HighExpr::makeBinop(
                  NdOp::CONCAT,
                  HighExpr::makeConst(Upper, 8,
                                      ConstantAddressProvenance::Scalar),
                  Value);
            Value->Type = NdType::makeInt(Type->Size, false);
            *Expression = *HighExpr::makeBitCast(Value, Type);
            return Expression;
          }
        }
      }
    }
    if (Original->Kind == ExprKind::Load &&
        (AddressContext || ObjectPointerConsumers.count(Original.get())) &&
        !NumericOperand && !MemoryAddress) {
      if (const auto Found = ObjectPointerLoads.find(Original.get());
          Found != ObjectPointerLoads.end()) {
        const auto &Plan = Found->second;
        const auto Entries =
            constantObjectTableEntries(Image, Plan.Base, Plan.Extent);
        auto Hint = Entries
                        ? constantObjectTableHint(Image, Plan.Base, Plan.Extent)
                        : std::nullopt;
        if (Hint) {
          auto Base = HighExpr::makeCall({}, 0, {});
          Base->Type = NdType::makeInt(8, false);
          Base->SourceCallHint =
              std::make_shared<SourceCallTypeHint>(std::move(*Hint));
          Expression->Operands = {HighExpr::makeBinop(
              NdOp::INT_ADD, Base,
              Copy(Plan.Offset, Depth + 1, false, false, false))};
          Expression->Type = NdType::makePtr(NdType::makeVoid());
          Result.ConstantObjectTables[Plan.Base] = std::max<uint32_t>(
              Result.ConstantObjectTables[Plan.Base], Plan.Extent);
          for (const auto &Entry : *Entries) {
            if (!Entry.Target)
              continue;
            (Entry.IsString ? Result.ConstantStrings : Result.ConstantObjects)
                .insert(Entry.Target);
          }
          return Expression;
        }
      }
    }
    if (Original->Kind == ExprKind::Load && !AddressContext && !MemoryAddress) {
      if (const auto Found = ScalarLoads.find(Original.get());
          Found != ScalarLoads.end()) {
        const auto &Plan = Found->second;
        auto Hint = borrowedByteSourceHint(Image, {Plan.Base, Plan.Extent});
        if (Hint) {
          Hint->CallKind = SourceCallTypeHint::Kind::RuntimeReadOnlyBytes;
          auto Base = HighExpr::makeCall({}, 0, {});
          Base->Type = NdType::makeInt(8, false);
          Base->SourceCallHint =
              std::make_shared<SourceCallTypeHint>(std::move(*Hint));
          Expression->Operands = {HighExpr::makeBinop(
              NdOp::INT_ADD, Base,
              Copy(Plan.Offset, Depth + 1, false, false, false))};
          Result.BorrowedBytes.insert({Plan.Base, Plan.Extent});
          return Expression;
        }
      }
    }
    if (Expression->Kind == ExprKind::Load &&
        Expression->Operands.size() == 1 &&
        BindMemoryAddress(Expression->Operands[0], Expression->Type,
                          Expression->MemoryOrdering,
                          Expression->MemoryAddressSpace))
      return Expression;
    if (Expression->Kind == ExprKind::Store &&
        Expression->Operands.size() == 2 && Expression->Operands[1] &&
        BindMemoryAddress(
            Expression->Operands[0], Expression->Operands[1]->Type,
            Expression->MemoryOrdering, Expression->MemoryAddressSpace)) {
      Expression->Operands[1] =
          Copy(Expression->Operands[1], Depth + 1, false, false, false);
      return Expression;
    }
    if (Expression->Kind == ExprKind::Const && Expression->ConstVal &&
        Image.getSectionFor(Expression->ConstVal) &&
        !(NumericOperand && !AddressContext &&
          Expression->ConstProvenance == ConstantAddressProvenance::Scalar &&
          Expression->AddressOwnerVA == InvalidVA))
      Fail("method retains an image address without a relocatable source "
           "binding",
           Expression.get());
    // A Swift lazy witness accessor is a zero-argument compiler helper even
    // though its body forwards an undef third argument to the runtime. Bind
    // the exact proven accessor before native dependency collection so the
    // incidental incoming register never becomes a public source parameter.
    if (Expression->Kind == ExprKind::Call && Expression->SourceCallHint &&
        Expression->SourceCallHint->CallKind ==
            SourceCallTypeHint::Kind::Native &&
        !Expression->IsIndirectCall && Functions &&
        Expression->CallAddr == Expression->SourceCallHint->TargetAddress) {
      const va_t Target = Expression->SourceCallHint->TargetAddress;
      const auto Callee = Functions->find(Target);
      va_t CacheAddress = 0;
      const auto Accessor = Callee == Functions->end() || !Callee->second
                                ? std::nullopt
                                : swiftWitnessAccessorCallHint(
                                      *Callee->second, Image, &CacheAddress);
      if (Accessor) {
        Expression->Operands.clear();
        Expression->CallTarget.clear();
        Expression->SourceCallHint =
            std::make_shared<SourceCallTypeHint>(*Accessor);
        Expression->Type = Accessor->Signature.ReturnType;
        Result.SwiftWitnessCaches[CacheAddress] = Target;
      }
    }
    // A selector-specific stub may look like an ordinary local function after
    // native lifting. Reclassify it only when the compiler declaration has a
    // format contract and the physical native ABI agrees exactly. A dynamic
    // format may have no tail, a proven pointer tail, or a tail of declared
    // 64-bit integer results. Each independent contract keeps the complete
    // promoted carriers without inferring conversions from the selector.
    if (Expression->Kind == ExprKind::Call && !Expression->IsIndirectCall &&
        Expression->CallAddr && Expression->IntrinsicId == Intrinsic::None &&
        Expression->MemoryAddressSpace == NdMemoryAddressSpace::Default &&
        Expression->MemoryOrdering == NdMemoryOrdering::None) {
      const auto Stub = objcSelectorStubDynamicFormatSourceCallHint(
          Image, Expression->CallAddr);
      std::optional<SourceCallTypeHint> Expected;
      if (Stub && Stub->Format &&
          Expression->Operands.size() >= Stub->Format->FixedCount) {
        const auto Fixed = Stub->Format->FixedCount;
        size_t PointerBudget = 4096;
        std::set<VarKey> ActivePointers;
        const bool PointerTail = std::all_of(
            Expression->Operands.begin() + Fixed, Expression->Operands.end(),
            [&](const ExprPtr &Operand) {
              return provenSourcePointerValue(Operand, FormatDefinitions, Image,
                                              PointerBudget, ActivePointers);
            });
        if (PointerTail) {
          Expected = objcDynamicFormatPointerArgumentsSourceCallHint(
              Image, Stub->Selector,
              unsigned(Expression->Operands.size() - Fixed));
          // The authenticated selector stub fixes the callee and selector. A
          // redundant native hint is unnecessary when every integer tail value
          // has an independently declared source result and the complete call's
          // scalar carriers agree with the format declaration below.
        } else if (!Expression->SourceCallHint ||
                   plainNativeBinding(*Expression->SourceCallHint)) {
          size_t IntegerBudget = 4096;
          std::set<VarKey> ActiveIntegers;
          std::vector<TypeRef> Types;
          for (size_t I = Fixed; I < Expression->Operands.size(); ++I) {
            auto Type = provenSourceInteger64Value(
                Expression->Operands[I], FormatDefinitions, Image,
                IntegerBudget, ActiveIntegers);
            if (!Type) {
              Types.clear();
              break;
            }
            Types.push_back(std::move(Type));
          }
          if (Types.size() == Expression->Operands.size() - Fixed)
            Expected = objcDynamicFormatInteger64ArgumentsSourceCallHint(
                Image, Stub->Selector, Types);
        }
        if (Expected) {
          Expected->TargetAddress = Stub->TargetAddress;
          Expected->SelectorReferenceAddress = Stub->SelectorReferenceAddress;
        }
      }
      bool PhysicalCall = false;
      if (Expected && Expected->Format && Expression->SourceCallHint &&
          plainNativeBinding(*Expression->SourceCallHint) &&
          Expression->CallAddr == Expression->SourceCallHint->TargetAddress &&
          Expression->Operands.size() ==
              Expression->SourceCallHint->Signature.Parameters.size())
        PhysicalCall = samePhysicalSourceCall(
            Expression->SourceCallHint->Signature, Expected->Signature);
      else if (Expected && Expected->Format && !Expression->SourceCallHint &&
               (!Expression->Type ||
                sameScalarCarrier(Expression->Type,
                                  Expected->Signature.ReturnType)) &&
               Expression->Operands.size() ==
                   Expected->Signature.Parameters.size()) {
        PhysicalCall = true;
        for (size_t I = 0; I < Expression->Operands.size(); ++I)
          if (!Expression->Operands[I] ||
              !sameScalarCarrier(Expression->Operands[I]->Type,
                                 Expected->Signature.Parameters[I].Type)) {
            PhysicalCall = false;
            break;
          }
      }
      if (Expected && Expected->Format && PhysicalCall) {
        Expression->CallTarget.clear();
        Expression->SourceCallHint =
            std::make_shared<SourceCallTypeHint>(*Expected);
        Expression->Type = Expected->Signature.ReturnType;
      }
    }
    if (Expression->Kind == ExprKind::Call && Expression->SourceCallHint &&
        Expression->SourceCallHint->CallKind ==
            SourceCallTypeHint::Kind::Native)
      Result.Dependencies.insert(Expression->SourceCallHint->TargetAddress);
    std::optional<SourceCallTypeHint::SwiftTypeMetadataAddress>
        SwiftMetadataPair;
    if (Expression->Kind == ExprKind::Call && Expression->SourceCallHint &&
        Expression->SourceCallHint->CallKind ==
            SourceCallTypeHint::Kind::Native &&
        Expression->Operands.size() >= 2 && Expression->Operands.size() <= 4 &&
        Expression->SourceCallHint->Signature.Parameters.size() ==
            Expression->Operands.size()) {
      const auto &Signature = Expression->SourceCallHint->Signature;
      std::string Reason;
      const bool PointerParameters = std::all_of(
          Signature.Parameters.begin(), Signature.Parameters.end(),
          [](const auto &Parameter) {
            return Parameter.Type && Parameter.Type->Kind == NdTypeKind::Ptr;
          });
      if (PointerParameters && validateSourceABI(Signature, Reason)) {
        bool Ambiguous = false;
        for (size_t FirstIndex = 0; FirstIndex < Expression->Operands.size();
             ++FirstIndex) {
          if (!Expression->Operands[FirstIndex])
            continue;
          const auto First = constantAddress(*Expression->Operands[FirstIndex]);
          if (!First)
            continue;
          for (size_t SecondIndex = FirstIndex + 1;
               SecondIndex < Expression->Operands.size(); ++SecondIndex) {
            // Four-argument value helpers pass destination and source first;
            // only their trailing cache/reference pair is a metadata recipe.
            if (Expression->Operands.size() == 4 &&
                (FirstIndex != 2 || SecondIndex != 3))
              continue;
            if (!Expression->Operands[SecondIndex])
              continue;
            const auto Second =
                constantAddress(*Expression->Operands[SecondIndex]);
            if (!Second)
              continue;
            auto Candidate = swiftTypeMetadataPair(Image, *First, *Second);
            if (!Candidate)
              Candidate = swiftTypeMetadataPair(Image, *Second, *First);
            if (!Candidate)
              continue;
            if (SwiftMetadataPair && *SwiftMetadataPair != *Candidate) {
              Ambiguous = true;
              break;
            }
            SwiftMetadataPair = std::move(Candidate);
          }
          if (Ambiguous)
            break;
        }
        if (Ambiguous)
          SwiftMetadataPair.reset();
      }
    }
    std::optional<va_t> SingletonDescriptor;
    size_t SingletonDescriptorIndex = 0;
    if (Expression->Kind == ExprKind::Call && !Expression->IsIndirectCall &&
        Image.isCodeAddress(Expression->CallAddr) &&
        Expression->SourceCallHint &&
        Expression->SourceCallHint->CallKind ==
            SourceCallTypeHint::Kind::SwiftRuntimeCall &&
        Expression->SourceCallHint->TargetName ==
            "swift_getSingletonMetadata" &&
        Expression->Operands.size() == 2 && Expression->Operands[1]) {
      const auto &Binding = *Expression->SourceCallHint;
      const auto Expected =
          swiftRuntimeSourceCallHint(Image, Binding.TargetAddress);
      if (Expected && Expected->TargetAddress == Binding.TargetAddress &&
          runtimeBindingMatches(Binding, *Expected) &&
          Expected->Signature.Parameters.size() == 2 &&
          Expected->Signature.Parameters[1].Type &&
          Expected->Signature.Parameters[1].Type->Kind == NdTypeKind::Ptr) {
        SingletonDescriptor = constantAddress(*Expression->Operands[1]);
        SingletonDescriptorIndex = 1;
      }
    }
    if (!SingletonDescriptor && Expression->Kind == ExprKind::Call &&
        !Expression->IsIndirectCall && Expression->SourceCallHint &&
        Expression->SourceCallHint->CallKind ==
            SourceCallTypeHint::Kind::Native &&
        Expression->CallAddr == Expression->SourceCallHint->TargetAddress &&
        Expression->Operands.size() == 3 && Expression->Operands[2] &&
        Functions) {
      const auto Candidate = constantAddress(*Expression->Operands[2]);
      if (Candidate && swiftNominalDescriptorAddressHint(Image, *Candidate)) {
        const auto Callee =
            Functions->find(Expression->SourceCallHint->TargetAddress);
        if (Callee != Functions->end() && Callee->second &&
            swiftSingletonDescriptorForwardedByNativeHelper(
                Image, *Callee->second, *Expression->SourceCallHint)) {
          SingletonDescriptor = Candidate;
          SingletonDescriptorIndex = 2;
        }
      }
    }
    std::optional<va_t> WitnessMetadata;
    if (Expression->Kind == ExprKind::Call && Expression->IsIndirectCall &&
        Expression->SourceCallHint &&
        Expression->SourceCallHint->CallKind ==
            SourceCallTypeHint::Kind::SwiftValueWitness &&
        isSwiftValueWitnessSourceCallHint(*Expression->SourceCallHint,
                                          Image.Arch) &&
        Expression->Operands.size() ==
            Expression->SourceCallHint->Signature.Parameters.size() &&
        !Expression->Operands.empty() && Expression->Operands.back() &&
        Expression->Operands.back()->Kind == ExprKind::Const &&
        Expression->Operands.back()->Type &&
        Expression->Operands.back()->Type->Size == 8 &&
        (Expression->Operands.back()->ConstProvenance ==
             ConstantAddressProvenance::Address ||
         Expression->Operands.back()->ConstProvenance ==
             ConstantAddressProvenance::DataAddress))
      WitnessMetadata = constantAddress(*Expression->Operands.back());
    // A Swift literal's tagged word can occupy a pointer-typed source
    // carrier. Its exact OR must still relocate the address leaf even when
    // the enclosing call declares that carrier as a pointer. A real memory
    // address remains excluded: the high-bit tag is not dereferenceable.
    const auto TaggedCString =
        !NumericOperand && !MemoryAddress
            ? taggedCStringAddressOperand(*Original, Image)
            : std::nullopt;
    for (size_t Index = 0; Index < Expression->Operands.size(); ++Index) {
      auto &Operand = Expression->Operands[Index];
      if (Operand && WitnessMetadata &&
          Index + 1 == Expression->Operands.size()) {
        auto Hint = swiftNominalMetadataAddressHint(Image, *WitnessMetadata);
        if (Hint) {
          Result.SwiftNominalMetadata[*WitnessMetadata] = Hint->TargetName;
          auto Metadata = HighExpr::makeCall({}, 0, {});
          Metadata->Type = Operand->Type;
          Metadata->SourceCallHint =
              std::make_shared<SourceCallTypeHint>(std::move(*Hint));
          Operand = std::move(Metadata);
          continue;
        }
      }
      if (Index == SingletonDescriptorIndex && Operand && SingletonDescriptor) {
        auto Hint =
            swiftNominalDescriptorAddressHint(Image, *SingletonDescriptor);
        if (Hint) {
          Result.SwiftNominalDescriptors[*SingletonDescriptor] =
              Hint->TargetName;
          auto Descriptor = HighExpr::makeCall({}, 0, {});
          Descriptor->Type = Operand->Type;
          Descriptor->SourceCallHint =
              std::make_shared<SourceCallTypeHint>(std::move(*Hint));
          Operand = std::move(Descriptor);
          continue;
        }
      }
      // The Swift outlined-destroy helper receives an exact cache/reference
      // pair. Private-linkage symbols may repeat in every compilation unit,
      // so the native call's uniquely matching typed pointer arguments provide
      // the pairing boundary; other helper arguments and global symbol-name
      // order are not evidence.
      if (Operand && SwiftMetadataPair) {
        const auto Address = constantAddress(*Operand);
        auto Hint = Address ? swiftTypeMetadataAddressHint(Image, *Address,
                                                           *SwiftMetadataPair)
                            : std::nullopt;
        if (Hint) {
          Result.SwiftTypeMetadataPairs[SwiftMetadataPair->CacheAddress] =
              *SwiftMetadataPair;
          auto Storage = HighExpr::makeCall({}, 0, {});
          Storage->Type = Operand->Type;
          Storage->SourceCallHint =
              std::make_shared<SourceCallTypeHint>(std::move(*Hint));
          Operand = std::move(Storage);
          continue;
        }
      }
      // Rebuild a profile-counter pointer passed to a native dependency only
      // after that dependency's complete typed body proves bounded numeric
      // accesses and no escape of the parameter.
      if (Operand && Functions && Expression->Kind == ExprKind::Call &&
          Expression->SourceCallHint &&
          Expression->SourceCallHint->CallKind ==
              SourceCallTypeHint::Kind::Native) {
        const auto &Binding = *Expression->SourceCallHint;
        const auto Callee = Functions->find(Binding.TargetAddress);
        const auto Address = constantAddress(*Operand);
        if (Callee != Functions->end() && Callee->second && Address &&
            Index < Binding.Signature.Parameters.size() &&
            Binding.Signature.Parameters.size() ==
                Expression->Operands.size() &&
            Binding.Signature.Parameters[Index].Type &&
            Binding.Signature.Parameters[Index].Type->Kind == NdTypeKind::Ptr &&
            Callee->second->SourceTypeHint &&
            objc_projection_detail::sameHint(Binding.Signature,
                                             *Callee->second->SourceTypeHint)) {
          // Swift can coalesce imported Objective-C class references into an
          // ordinary GOT cell and pass that cell's address to a shared thunk.
          // Preserve the extra indirection only when the complete callee
          // proves only exact pointer-sized loads and no store or escape.
          const auto ReferenceExtent = nativeScalarStorageArgumentExtent(
              *Callee->second, Index, true, false, Functions);
          auto Reference = ReferenceExtent && *ReferenceExtent == 8
                               ? classReferenceAddressHint(Image, *Address)
                               : std::nullopt;
          if (Reference) {
            auto Storage = HighExpr::makeCall({}, 0, {});
            Storage->Type = Operand->Type;
            Storage->SourceCallHint =
                std::make_shared<SourceCallTypeHint>(std::move(*Reference));
            Operand = std::move(Storage);
            Result.ClassReferenceCells.insert(*Address);
            continue;
          }

          const auto Base = nativeProfileCounterArgument(
              *Callee->second, Index, *Address, *ProfileStorage);
          auto Hint =
              Base ? profileStorageHint(Image.Arch, *Base) : std::nullopt;
          if (Hint) {
            auto Storage = HighExpr::makeCall({}, 0, {});
            Storage->Type = Operand->Type;
            Storage->SourceCallHint =
                std::make_shared<SourceCallTypeHint>(std::move(*Hint));
            Operand = *Address == *Base
                          ? Storage
                          : HighExpr::makeBinop(
                                NdOp::INT_ADD, Storage,
                                HighExpr::makeConst(
                                    *Address - *Base, 8,
                                    ConstantAddressProvenance::Scalar));
            Result.ProfileCounterSections.insert(*Base);
            continue;
          }

          // Ordinary named writable storage uses the same non-escape proof,
          // but keeps its captured initializer and aliasing through the
          // existing shared local-storage helper.
          const auto Extent = nativeScalarStorageArgumentExtent(
              *Callee->second, Index, true, true, Functions);
          auto LocalHint =
              Extent ? localStorageAccessHint(Image, *Address, *Extent)
                     : std::nullopt;
          if (LocalHint) {
            const va_t BaseAddress = LocalHint->TargetAddress;
            const uint64_t ByteCount = LocalHint->ByteCount;
            auto Storage = HighExpr::makeCall({}, 0, {});
            Storage->Type = Operand->Type;
            Storage->SourceCallHint =
                std::make_shared<SourceCallTypeHint>(std::move(*LocalHint));
            Operand = *Address == BaseAddress
                          ? Storage
                          : HighExpr::makeBinop(
                                NdOp::INT_ADD, Storage,
                                HighExpr::makeConst(
                                    *Address - BaseAddress, 8,
                                    ConstantAddressProvenance::Scalar));
            Result.LocalStorageExtents[BaseAddress] = std::max<uint64_t>(
                Result.LocalStorageExtents[BaseAddress], ByteCount);
            continue;
          }
        }
      }
      // A bounded content consumer may use a copied byte buffer. This proof
      // belongs to this operand occurrence: pointer identity, ordinary loads,
      // escaping addresses and unrelated calls do not inherit it.
      if (Operand && Expression->Kind == ExprKind::Call &&
          Expression->SourceCallHint &&
          Expression->IntrinsicId == Intrinsic::None &&
          Expression->MemoryAddressSpace == NdMemoryAddressSpace::Default &&
          Expression->MemoryOrdering == NdMemoryOrdering::None &&
          !Expression->IsIndirectCall) {
        const auto &Binding = *Expression->SourceCallHint;
        if (!Binding.SwiftStringInputs.empty()) {
          const auto Expected = runtimeSourceCallHint(Image, Binding);
          if (Expected && runtimeBindingMatches(Binding, *Expected) &&
              Expression->Operands.size() ==
                  Binding.Signature.Parameters.size()) {
            for (const auto &[WordIndex, StorageIndex] :
                 Expected->SwiftStringInputs) {
              if (Index != StorageIndex ||
                  WordIndex >= Expression->Operands.size())
                continue;
              const auto Word =
                  swiftLiteralStorageWord(Expression->Operands[WordIndex]);
              const auto Storage = swiftLiteralStorageWord(Operand);
              const auto Literal =
                  Word && Storage ? swiftLiteralString(Image, *Word, *Storage)
                                  : std::nullopt;
              if (!Literal)
                continue;
              const BorrowedByteRange Range{Literal->Contents, Literal->Bytes};
              auto Bytes = HighExpr::makeCall({}, 0, {});
              Bytes->Type = NdType::makeInt(8, false);
              Bytes->SourceCallHint = std::make_shared<SourceCallTypeHint>(
                  *borrowedByteSourceHint(Image, Range));
              Operand = HighExpr::makeBinop(
                  NdOp::INT_OR,
                  HighExpr::makeBinop(
                      NdOp::INT_SUB, Bytes,
                      HighExpr::makeConst(SwiftLiteralString::StorageBias, 8,
                                          ConstantAddressProvenance::Scalar)),
                  HighExpr::makeConst(SwiftLiteralString::ImmortalTag, 8,
                                      ConstantAddressProvenance::Scalar));
              Result.BorrowedBytes.insert(Range);
              break;
            }
            if (containsBorrowedStorage(Operand))
              continue;
          }
        }
        if (!Binding.BorrowedByteInputs.empty()) {
          const auto Expected = runtimeSourceCallHint(Image, Binding);
          if (Expected && runtimeBindingMatches(Binding, *Expected) &&
              Expression->Operands.size() ==
                  Binding.Signature.Parameters.size()) {
            for (const auto &[PointerIndex, CountIndex] :
                 Expected->BorrowedByteInputs) {
              if (Index != PointerIndex ||
                  CountIndex >= Expression->Operands.size())
                continue;
              const auto Address = constantAddress(*Operand);
              const auto Count = constantBorrowedByteCount(
                  Expression->Operands[CountIndex],
                  Expected->Signature.Parameters[CountIndex].Type);
              auto Hint =
                  Address && Count
                      ? borrowedByteSourceHint(Image, {*Address, *Count})
                      : std::nullopt;
              if (Hint) {
                auto Bytes = HighExpr::makeCall({}, 0, {});
                Bytes->Type = Operand->Type;
                Bytes->SourceCallHint =
                    std::make_shared<SourceCallTypeHint>(std::move(*Hint));
                Result.BorrowedBytes.insert({*Address, *Count});
                Operand = std::move(Bytes);
              }
            }
          }
        }
      }
      // Identity-only runtime APIs compare keys and never read their bytes.
      // Rebuild only this authenticated argument occurrence;
      // ordinary loads, returned addresses, and unrelated calls must retain
      // the unresolved image-data diagnostic. Equal original addresses share
      // one helper across methods, including addresses inside a string.
      if (Operand && identityKeyParameter(*Expression, Image) == Index) {
        const auto Address = constantAddress(*Operand);
        const bool ObjCAssociation =
            Expression->SourceCallHint->CallKind ==
                SourceCallTypeHint::Kind::ObjCRuntimeCall &&
            (Expression->SourceCallHint->TargetName ==
                 "objc_getAssociatedObject" ||
             Expression->SourceCallHint->TargetName ==
                 "objc_setAssociatedObject");
        auto Hint = Address && ObjCAssociation
                        ? swiftPrivateScalarStorageHint(Image, *Address)
                        : std::nullopt;
        const bool PrivateStorage = bool(Hint);
        if (!Hint)
          Hint = Address ? associationKeyHint(Image, *Address, ObjCAssociation)
                         : std::nullopt;
        if (Hint) {
          auto Key = HighExpr::makeCall({}, 0, {});
          Key->Type = Operand->Type;
          Key->SourceCallHint =
              std::make_shared<SourceCallTypeHint>(std::move(*Hint));
          if (PrivateStorage)
            Result.LocalStorageExtents[*Address] =
                std::max<uint64_t>(Result.LocalStorageExtents[*Address],
                                   Key->SourceCallHint->ByteCount);
          else
            Result.AssociationKeys.insert(*Address);
          Operand = std::move(Key);
          continue;
        }
      }
      // KVO preserves an opaque caller-chosen context pointer between observer
      // registration and the callback. Rebuild a writable static token only at
      // the exact declared registration argument, or where the unique matching
      // callback compares its context parameter directly for equality. Other
      // uses of the same address remain unresolved image-data references.
      const bool KVORegistration = Operand && kvoRegistrationContextParameter(
                                                  *Expression, Image) == Index;
      const bool KVOCallbackComparison =
          Operand && KVOCallbackParameter &&
          Expression->Kind == ExprKind::BinOp &&
          (Expression->Op == NdOp::INT_EQUAL ||
           Expression->Op == NdOp::INT_NOTEQUAL) &&
          Expression->Operands.size() == 2 && Index < 2 &&
          exactParameterValue(Expression->Operands[1 - Index],
                              *KVOCallbackParameter);
      if (KVORegistration || KVOCallbackComparison) {
        const auto Address = constantAddress(*Operand);
        // A private Swift scalar may also be used by exclusivity or an
        // associated-object call. All projections of that exact address must
        // share its scalar storage, including KVO's opaque context identity.
        auto Hint = Address ? swiftPrivateScalarStorageHint(Image, *Address)
                            : std::nullopt;
        const bool PrivateStorage = bool(Hint);
        if (!Hint)
          Hint = Address ? kvoContextHint(Image, *Address) : std::nullopt;
        if (Hint) {
          auto Context = HighExpr::makeCall({}, 0, {});
          Context->Type = Operand->Type;
          Context->SourceCallHint =
              std::make_shared<SourceCallTypeHint>(std::move(*Hint));
          if (PrivateStorage)
            Result.LocalStorageExtents[*Address] =
                std::max<uint64_t>(Result.LocalStorageExtents[*Address],
                                   Context->SourceCallHint->ByteCount);
          else
            Result.KVOContexts.insert(*Address);
          Operand = std::move(Context);
          continue;
        }
      }
      // swift_beginAccess marks an address for the exclusivity runtime but
      // does not describe its pointee type. Rebuild this operand only when an
      // actual load/store in the same function or an exact Swift scalar
      // storage symbol independently proves the access width.
      if (Index == 0 && Operand && Expression->Kind == ExprKind::Call &&
          Expression->SourceCallHint &&
          Expression->SourceCallHint->CallKind ==
              SourceCallTypeHint::Kind::SwiftRuntimeCall &&
          Expression->SourceCallHint->TargetName == "swift_beginAccess") {
        const auto Expected =
            runtimeSourceCallHint(Image, *Expression->SourceCallHint);
        const auto Address = constantAddress(*Operand);
        const auto Extent = Address ? DirectLocalStorage.find(*Address)
                                    : DirectLocalStorage.end();
        std::optional<uint64_t> Width;
        if (Extent != DirectLocalStorage.end())
          Width = Extent->second;
        else if (Address)
          if (const auto *Symbol =
                  uniqueWritableDataSymbol(Image, *Address, 1)) {
            Width = swiftStaticScalarStorageWidth(Symbol->Name);
            if (!Width)
              Width = swiftPrivateScalarStorageWidth(Symbol->Name);
          }
        auto Hint = Expected &&
                            runtimeBindingMatches(*Expression->SourceCallHint,
                                                  *Expected) &&
                            Expression->Operands.size() ==
                                Expected->Signature.Parameters.size() &&
                            Width
                        ? localStorageHint(Image, *Address, *Width)
                        : std::nullopt;
        if (Hint) {
          auto Storage = HighExpr::makeCall({}, 0, {});
          Storage->Type = Operand->Type;
          Storage->SourceCallHint =
              std::make_shared<SourceCallTypeHint>(std::move(*Hint));
          Result.LocalStorageExtents[*Address] =
              std::max<uint64_t>(Result.LocalStorageExtents[*Address], *Width);
          Operand = std::move(Storage);
          continue;
        }
        if (Address && Image.getSectionFor(*Address))
          Fail("Swift exclusivity marker retains an unproved image-data "
               "address",
               Operand.get());
      }
      // Authenticated runtime operations consume a known storage cell. Bind
      // only that argument; ownership and synchronization stay in the runtime.
      if (Index == 0 && Operand && Expression->Kind == ExprKind::Call &&
          Expression->SourceCallHint &&
          ((Expression->SourceCallHint->CallKind ==
                SourceCallTypeHint::Kind::DarwinRuntimeCall &&
            (Expression->SourceCallHint->TargetName == "dispatch_once" ||
             Expression->SourceCallHint->TargetName == "os_unfair_lock_lock" ||
             Expression->SourceCallHint->TargetName ==
                 "os_unfair_lock_unlock" ||
             Expression->SourceCallHint->TargetName ==
                 "os_unfair_lock_assert_owner" ||
             Expression->SourceCallHint->TargetName ==
                 "os_unfair_lock_assert_not_owner" ||
             Expression->SourceCallHint->TargetName ==
                 "os_unfair_lock_trylock")) ||
           (Expression->SourceCallHint->CallKind ==
                SourceCallTypeHint::Kind::ObjCRuntimeCall &&
            Expression->SourceCallHint->TargetName == "objc_storeStrong"))) {
        const auto Expected =
            runtimeSourceCallHint(Image, *Expression->SourceCallHint);
        if (!Expected ||
            !runtimeBindingMatches(*Expression->SourceCallHint, *Expected))
          continue;
        const auto Address = constantAddress(*Operand);
        const bool Once = Expected->TargetName == "dispatch_once";
        const uint64_t Width =
            Once || Expected->TargetName == "objc_storeStrong" ? 8 : 4;
        auto Hint = Address ? (Once ? oncePredicateStorageHint(Image, *Address)
                                    : localStorageHint(Image, *Address, Width))
                            : std::nullopt;
        if (Hint) {
          auto Storage = HighExpr::makeCall({}, 0, {});
          Storage->Type = Operand->Type;
          Storage->SourceCallHint =
              std::make_shared<SourceCallTypeHint>(std::move(*Hint));
          Result.LocalStorageExtents[*Address] =
              std::max<uint64_t>(Result.LocalStorageExtents[*Address], Width);
          Operand = std::move(Storage);
          continue;
        }
      }
      // A direct class-object address is already an object value. This is
      // distinct from the address of a classref slot, which requires a LOAD.
      // Exact Objective-C runtime calls can also consume class objects (for
      // example objc_opt_self in a Swift class metadata accessor). Revalidate
      // that imported runtime binding before replacing any pointer argument.
      // Keep this contextual rewrite out of Copies: the same Const node may
      // also occur as an ordinary integer elsewhere in the expression DAG.
      if (Operand && Expression->Kind == ExprKind::Call &&
          Expression->SourceCallHint) {
        const auto &CallBinding = *Expression->SourceCallHint;
        const auto &Signature = CallBinding.Signature;
        std::string Error;
        const bool MessageReceiver =
            Index == 0 &&
            CallBinding.CallKind == SourceCallTypeHint::Kind::ObjCMessage;
        const auto ExpectedRuntime =
            CallBinding.CallKind == SourceCallTypeHint::Kind::ObjCRuntimeCall
                ? runtimeSourceCallHint(Image, CallBinding)
                : std::nullopt;
        const auto RuntimeImport =
            Image.DyldBindSlots.find(CallBinding.TargetAddress);
        const bool RuntimeArgument =
            ExpectedRuntime && RuntimeImport != Image.DyldBindSlots.end() &&
            RuntimeImport->second.Module == "/usr/lib/libobjc.A.dylib" &&
            runtimeBindingMatches(CallBinding, *ExpectedRuntime);
        if ((MessageReceiver || RuntimeArgument) &&
            Index < Signature.Parameters.size() &&
            Signature.Parameters.size() == Expression->Operands.size() &&
            Signature.Parameters[Index].Type &&
            Signature.Parameters[Index].Type->Kind == NdTypeKind::Ptr &&
            validateSourceABI(Signature, Error)) {
          const auto Address = constantAddress(*Operand);
          const auto *Object = Address ? ClassObjectAt(*Address) : nullptr;
          if (Object) {
            auto Binding = std::make_shared<SourceCallTypeHint>();
            Binding->CallKind = Object->Kind;
            Binding->TargetAddress = *Address;
            Binding->TargetName = Object->Name;
            auto &Hint = Binding->Signature;
            Hint.Architecture = Image.Arch;
            Hint.HasExplicitABI = true;
            Hint.ReturnType = NdType::makePtr(NdType::makeVoid());
            Hint.ReturnLocation = {SourceABICarrierKind::IntegerRegister,
                                   getTargetRegInfo(Image.Arch).IntReturnReg, 0,
                                   8};
            auto Value = HighExpr::makeCall({}, 0, {});
            Value->Type = Operand->Type;
            Value->SourceCallHint = std::move(Binding);
            Operand = std::move(Value);
            continue;
          }
        }
      }
      // Numeric provenance is meaningful at an operand occurrence, not for
      // every use of the shared node. An address consumer remains strict even
      // when its arithmetic happens to contain encoded scalar immediates.
      // A tagged literal word retains its original integer OR. Relocate only
      // the complete address leaf into the permanent shared string pool;
      // neither a borrowed buffer nor a decoded Swift object is substituted.
      if (TaggedCString == Index) {
        Operand = Copy(Operand, Depth + 1, false, true, false);
        continue;
      }
      bool OperandAddress =
          AddressContext ||
          (Index == 0 && (Expression->Kind == ExprKind::Load ||
                          Expression->Kind == ExprKind::Store));
      if (Expression->Kind == ExprKind::Call && Expression->SourceCallHint) {
        OperandAddress = MemoryAddress;
        const auto &Parameters =
            Expression->SourceCallHint->Signature.Parameters;
        if (Index < Parameters.size() && Parameters[Index].Type &&
            Parameters[Index].Type->Kind == NdTypeKind::Ptr)
          OperandAddress = true;
      }
      const bool Numeric = (Expression->Kind == ExprKind::BinOp ||
                            Expression->Kind == ExprKind::UnaryOp) &&
                           isNumericConstantOperand(Expression->Op, Index);
      Operand = Copy(Operand, Depth + 1, Numeric, OperandAddress,
                     MemoryAddress ||
                         (Index == 0 && (Expression->Kind == ExprKind::Load ||
                                         Expression->Kind == ExprKind::Store)));
    }
    return Expression;
  };
  std::function<void(std::vector<HighStmt> &, unsigned)> Walk;
  Walk = [&](std::vector<HighStmt> &Body, unsigned Depth) {
    if (Depth > 200) {
      Fail("source reference control flow exceeds its depth budget");
      return;
    }
    for (auto &Statement : Body) {
      StatementAddress = Statement.Addr;
      const bool BoundStore =
          Statement.Kind == StmtKind::Store && Statement.StoreVal &&
          BindMemoryAddress(Statement.StoreAddr, Statement.StoreVal->Type,
                            Statement.MemoryOrdering,
                            Statement.MemoryAddressSpace);
      forEachExpr(Statement, [&](ExprPtr &Expression) {
        if (Statement.Kind == StmtKind::Assign && Expression == Statement.Val &&
            Statement.Dst &&
            (Statement.Dst->Kind == ExprKind::Var ||
             Statement.Dst->Kind == ExprKind::Phi) &&
            Statement.Dst->Operands.empty()) {
          const auto Local = highSourceLocalIdentity(Statement.Dst->Var);
          if (const auto Found = MetadataAliasPlans.find(Local);
              Found != MetadataAliasPlans.end()) {
            const auto &[Address, Pair] = Found->second;
            const auto OriginalAddress = constantAddress(*Expression);
            auto Hint = swiftTypeMetadataAddressHint(Image, Address, Pair);
            if (OriginalAddress == Address && Hint) {
              auto Bound = HighExpr::makeCall({}, 0, {});
              Bound->Type = Expression->Type;
              Bound->SourceCallHint =
                  std::make_shared<SourceCallTypeHint>(std::move(*Hint));
              Expression = std::move(Bound);
              Result.SwiftTypeMetadataPairs[Pair.CacheAddress] = Pair;
              return;
            }
          }
        }
        if (!BoundStore || Expression != Statement.StoreAddr)
          Expression =
              Copy(Expression, 0, false,
                   Expression == Statement.StoreAddr ||
                       (Expression == Statement.RetVal && Function.ReturnType &&
                        Function.ReturnType->Kind == NdTypeKind::Ptr),
                   Expression == Statement.StoreAddr);
      });
      Walk(Statement.Body, Depth + 1);
      Walk(Statement.ElseBody, Depth + 1);
      Walk(Statement.DefaultBody, Depth + 1);
      for (auto &Case : Statement.Cases)
        Walk(Case.Body, Depth + 1);
      for (auto &Clause : Statement.EHClauseBodies)
        Walk(Clause, Depth + 1);
    }
  };
  Walk(Result.Function.Body, 0);
  return Result;
}

inline std::optional<int64_t>
privateFrameArgumentOffset(const ExprPtr &Argument, const HighFunc &Function,
                           Arch Architecture) {
  const auto &TRI = getTargetRegInfo(Architecture);
  if (!Argument || Function.FrameSize <= 0 ||
      (TRI.PointerSize != 4 && TRI.PointerSize != 8))
    return std::nullopt;
  size_t Budget = 100000;
  VarKeyMap<unsigned> Definitions;
  walkStmts(Function.Body, [&](const HighStmt &Statement) {
    if (!Budget)
      return;
    --Budget;
    if (Statement.Kind == StmtKind::Assign && Statement.Dst &&
        Statement.Dst->Kind == ExprKind::Var)
      ++Definitions[varKey(Statement.Dst->Var)];
  });
  if (!Budget)
    return std::nullopt;
  VarKeyMap<ExprPtr> Aliases;
  for (const auto &Statement : Function.Body) {
    if (Statement.Kind != StmtKind::Assign || !Statement.Dst ||
        !Statement.Val || Statement.Dst->Kind != ExprKind::Var ||
        !Statement.Dst->Type || Statement.Dst->Type->Size != TRI.PointerSize ||
        Statement.Dst->Var.Size != TRI.PointerSize ||
        Statement.MemoryOrdering != NdMemoryOrdering::None ||
        Statement.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        !Statement.Body.empty() || !Statement.ElseBody.empty() ||
        !Statement.Cases.empty() || !Statement.DefaultBody.empty() ||
        !Statement.EHClauseBodies.empty() ||
        Definitions[varKey(Statement.Dst->Var)] != 1 ||
        !high_detail::frameAddressOffset(Statement.Val, Function, Architecture,
                                         Budget, 0, &Aliases))
      break;
    Aliases.emplace(varKey(Statement.Dst->Var), Statement.Val);
  }
  auto Offset = high_detail::frameAddressOffset(
      Argument, Function, Architecture, Budget, 0, &Aliases);
  if (!Offset || *Offset < -Function.FrameSize || *Offset >= 0 ||
      uint64_t(TRI.PointerSize) > uint64_t(-*Offset))
    return std::nullopt;
  return Offset;
}

inline bool objcSourceCallBound(
    const HighExpr &Expression, const BinaryImage &Image,
    const std::map<va_t, const HighFunc *> &Functions,
    const ObjCProfileStorage *ProfileStorage,
    const std::set<const HighExpr *> *ReadOnlyHelpers,
    const HighFunc *ContainingFunction,
    const std::map<va_t, std::map<unsigned, ObjCReceiverTypeHint>>
        *BlockParameterReceivers) {
  using namespace objc_binding_detail;
  if (Expression.Kind != ExprKind::Call || !Expression.SourceCallHint ||
      Expression.IntrinsicId != Intrinsic::None ||
      Expression.MemoryOrdering != NdMemoryOrdering::None ||
      Expression.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  const auto &Binding = *Expression.SourceCallHint;
  if (Binding.BooleanResult ||
      Binding.CallKind == SourceCallTypeHint::Kind::SwiftBooleanProjection)
    return false; // Requires the current pipeline and caller proof.
  const auto &Hint = Binding.Signature;
  if (Binding.NilTerminated &&
      (Binding.CallKind != SourceCallTypeHint::Kind::ObjCMessage ||
       !Binding.Receiver || Binding.Format || Binding.DoesNotReturn ||
       Binding.WeakImport || Binding.ReturnedArgument ||
       Binding.RuntimeObjCResultType || Binding.ValueWitness ||
       !Binding.OwnerClass.empty() || !Binding.BorrowedByteInputs.empty() ||
       !Binding.SwiftStringInputs.empty() || Binding.SwiftTypeMetadata ||
       Binding.SelectorResultUse || Binding.SelectorResultTypeUse ||
       Binding.SelectorArgumentTypeUse || Binding.SelectorForwardingUse ||
       Binding.SelectorArgumentStorageUse ||
       Binding.ObjCIndirectResultStorage || Binding.ByteCount ||
       Binding.ImmutablePointerSlot))
    return false;
  if ((Binding.ReturnedArgument || Binding.RuntimeObjCResultType) &&
      Binding.CallKind != SourceCallTypeHint::Kind::ObjCRuntimeCall)
    return false;
  if (Binding.Receiver &&
      ((Binding.CallKind != SourceCallTypeHint::Kind::ObjCMessage &&
        Binding.CallKind != SourceCallTypeHint::Kind::ObjCSuper2) ||
       Binding.Format))
    return false;
  if (Binding.SelectorResultUse &&
      (Binding.CallKind != SourceCallTypeHint::Kind::ObjCMessage ||
       Binding.Receiver || Binding.Format ||
       Binding.ObjCIndirectResultStorage ||
       (Hint.Origin != SourceFunctionTypeHint::OriginKind::ObjCSDK &&
        !(Binding.SelectorResultTypeUse &&
          Hint.Origin == SourceFunctionTypeHint::OriginKind::ObjCRuntime))))
    return false;
  if (Binding.SelectorResultTypeUse &&
      (!Binding.SelectorResultUse ||
       (*Binding.SelectorResultTypeUse != NdTypeKind::Int &&
        *Binding.SelectorResultTypeUse != NdTypeKind::Ptr &&
        *Binding.SelectorResultTypeUse != NdTypeKind::Float)))
    return false;
  if (Binding.SelectorArgumentTypeUse &&
      (Binding.CallKind != SourceCallTypeHint::Kind::ObjCMessage ||
       Binding.Receiver || Binding.Format || Binding.SelectorResultUse ||
       Binding.SelectorResultTypeUse || Binding.SelectorArgumentStorageUse ||
       Binding.SelectorForwardingUse || Binding.ObjCIndirectResultStorage ||
       Hint.Origin != SourceFunctionTypeHint::OriginKind::ObjCSDK))
    return false;
  if (Binding.SelectorArgumentStorageUse &&
      (Binding.CallKind != SourceCallTypeHint::Kind::ObjCMessage ||
       Binding.Receiver || Binding.Format || Binding.SelectorResultUse ||
       Binding.SelectorResultTypeUse || Binding.SelectorArgumentTypeUse ||
       Binding.SelectorForwardingUse || Binding.ObjCIndirectResultStorage ||
       Hint.Origin != SourceFunctionTypeHint::OriginKind::ObjCSDK))
    return false;
  if (Binding.SelectorForwardingUse &&
      (Binding.CallKind != SourceCallTypeHint::Kind::ObjCMessage ||
       Binding.Receiver || Binding.Format || Binding.SelectorResultUse ||
       Binding.SelectorResultTypeUse || Binding.SelectorArgumentTypeUse ||
       Binding.SelectorArgumentStorageUse ||
       Binding.ObjCIndirectResultStorage ||
       Hint.Origin != SourceFunctionTypeHint::OriginKind::ObjCRuntime))
    return false;
  if (Binding.ObjCIndirectResultStorage &&
      (Binding.CallKind != SourceCallTypeHint::Kind::ObjCMessage ||
       !Binding.Receiver || Binding.Format || Binding.NilTerminated ||
       Binding.SelectorResultUse || Binding.SelectorResultTypeUse ||
       Binding.SelectorArgumentTypeUse || Binding.SelectorForwardingUse ||
       Binding.SelectorArgumentStorageUse || Binding.DoesNotReturn ||
       Binding.WeakImport || Binding.ReturnedArgument ||
       Binding.RuntimeObjCResultType || Binding.ValueWitness ||
       !Binding.OwnerClass.empty() || !Binding.BorrowedByteInputs.empty() ||
       !Binding.SwiftStringInputs.empty() || Binding.SwiftTypeMetadata ||
       Binding.ByteCount || Binding.ImmutablePointerSlot))
    return false;
  if (Binding.Format &&
      Binding.CallKind != SourceCallTypeHint::Kind::ObjCMessage &&
      Binding.CallKind != SourceCallTypeHint::Kind::DarwinRuntimeCall)
    return false;
  if (Binding.ValueWitness &&
      Binding.CallKind != SourceCallTypeHint::Kind::SwiftValueWitness)
    return false;
  if (Binding.SwiftTypeMetadata &&
      Binding.CallKind !=
          SourceCallTypeHint::Kind::RuntimeSwiftTypeMetadataAddress)
    return false;
  if (!Binding.SwiftStringInputs.empty() &&
      Binding.CallKind != SourceCallTypeHint::Kind::SwiftRuntimeCall &&
      Binding.CallKind != SourceCallTypeHint::Kind::SwiftStringBridge)
    return false;
  std::string Reason;
  // Runtime effects are checked against their catalog. A native effect also
  // requires the exact typed callee and its complete terminating source flow;
  // its function flag alone is insufficient.
  if (Binding.DoesNotReturn &&
      Binding.CallKind != SourceCallTypeHint::Kind::Native &&
      Binding.CallKind != SourceCallTypeHint::Kind::ObjCRuntimeCall &&
      Binding.CallKind != SourceCallTypeHint::Kind::SwiftRuntimeCall &&
      Binding.CallKind != SourceCallTypeHint::Kind::SwiftStringBridge &&
      Binding.CallKind != SourceCallTypeHint::Kind::SwiftStringFromNSString &&
      Binding.CallKind != SourceCallTypeHint::Kind::DarwinRuntimeCall)
    return false;
  if (Binding.WeakImport &&
      Binding.CallKind != SourceCallTypeHint::Kind::DarwinRuntimeCall &&
      Binding.CallKind != SourceCallTypeHint::Kind::DarwinRuntimeGlobalAddress)
    return false;
  if (!validateSourceABI(Hint, Reason) || Hint.Architecture != Image.Arch ||
      Expression.Operands.size() != Hint.Parameters.size())
    return false;
  if (Binding.SelectorArgumentStorageUse) {
    const auto &Evidence = *Binding.SelectorArgumentStorageUse;
    if (!ContainingFunction || Evidence.Parameter >= Expression.Operands.size())
      return false;
    const auto Offset =
        privateFrameArgumentOffset(Expression.Operands[Evidence.Parameter],
                                   *ContainingFunction, Image.Arch);
    if (!Offset || *Offset != Evidence.FrameOffset)
      return false;
  }
  if (Binding.SelectorForwardingUse) {
    const auto &Evidence = *Binding.SelectorForwardingUse;
    const auto Caller = objcMethodSourceTypeHint(Image, Evidence.MethodEntry);
    const auto Expected = objcSelectorSourceTypeHintForForwardingUse(
        Image, Binding.Selector, Evidence);
    if (!ContainingFunction || !Evidence.MethodEntry ||
        Evidence.MethodEntry != ContainingFunction->Entry ||
        !ContainingFunction->SourceTypeHint || !Caller || !Expected ||
        !objc_projection_detail::sameHint(*ContainingFunction->SourceTypeHint,
                                          *Caller) ||
        !objc_projection_detail::sameHint(Hint, *Expected) ||
        ContainingFunction->Params.size() != Caller->Parameters.size() ||
        Evidence.ArgumentSourceParameters.size() + 2 !=
            Expression.Operands.size() ||
        Evidence.ReceiverSourceParameter >= ContainingFunction->Params.size() ||
        !exactParameterValue(Expression.Operands[0],
                             Evidence.ReceiverSourceParameter))
      return false;
    for (size_t I = 0; I < ContainingFunction->Params.size(); ++I)
      if (ContainingFunction->Params[I].Name != Caller->Parameters[I].Name ||
          !equalSourceTypes(ContainingFunction->Params[I].Type,
                            Caller->Parameters[I].Type))
        return false;
    for (size_t I = 0; I < Evidence.ArgumentSourceParameters.size(); ++I) {
      const auto Parameter = Evidence.ArgumentSourceParameters[I];
      if (Parameter >= ContainingFunction->Params.size() ||
          !exactParameterValue(Expression.Operands[I + 2], Parameter))
        return false;
    }
  }
  if (Binding.ObjCIndirectResultStorage) {
    const auto &Evidence = *Binding.ObjCIndirectResultStorage;
    const auto &Receiver = *Binding.Receiver;
    const auto Caller = objcMethodSourceTypeHint(Image, Evidence.MethodEntry);
    if (!ContainingFunction || !Evidence.MethodEntry || !Evidence.ByteCount ||
        Evidence.MethodEntry != ContainingFunction->Entry ||
        Receiver.Origin != ObjCReceiverTypeHint::OriginKind::MethodEntry ||
        Receiver.Address != Evidence.MethodEntry || !Receiver.Steps.empty() ||
        Hint.Origin != SourceFunctionTypeHint::OriginKind::ObjCSDK ||
        Hint.ReturnLocation.Kind !=
            SourceABICarrierKind::IndirectResultPointer ||
        !Hint.ReturnType || Hint.ReturnType->Kind != NdTypeKind::Struct ||
        Hint.ReturnType->Size != Evidence.ByteCount ||
        !Hint.ReturnComponents.empty() || ContainingFunction->FrameSize <= 0 ||
        Evidence.FrameOffset < -ContainingFunction->FrameSize ||
        Evidence.FrameOffset > -static_cast<int64_t>(Evidence.ByteCount) ||
        !ContainingFunction->SourceTypeHint || !Caller ||
        !objc_projection_detail::sameHint(*ContainingFunction->SourceTypeHint,
                                          *Caller) ||
        ContainingFunction->Params.size() != Caller->Parameters.size() ||
        !exactParameterValue(Expression.Operands[0], 0))
      return false;
    const auto Members = sourceAggregateMembers(Hint.ReturnType);
    if (Members.empty())
      return false;
    for (size_t I = 0; I < ContainingFunction->Params.size(); ++I)
      if (ContainingFunction->Params[I].Name != Caller->Parameters[I].Name ||
          !equalSourceTypes(ContainingFunction->Params[I].Type,
                            Caller->Parameters[I].Type))
        return false;
  }
  if (Binding.ImmutablePointerSlot &&
      Binding.CallKind != SourceCallTypeHint::Kind::RuntimeConstantString &&
      Binding.CallKind != SourceCallTypeHint::Kind::RuntimeConstantObject &&
      Binding.CallKind != SourceCallTypeHint::Kind::RuntimeCStringStorage)
    return false;
  if (Binding.CallKind == SourceCallTypeHint::Kind::RuntimeConstantObject) {
    const auto Expected = constantObjectSourceHint(
        Image, Binding.TargetAddress, Binding.ImmutablePointerSlot);
    return Expected && !Expression.IsIndirectCall &&
           Binding.TargetName.empty() && Binding.Selector.empty() &&
           Binding.OwnerClass.empty() && !Binding.SelectorReferenceAddress &&
           !Binding.ByteCount && Binding.BorrowedByteInputs.empty() &&
           Binding.SwiftStringInputs.empty() &&
           objc_projection_detail::sameHint(Expected->Signature, Hint);
  }
  if (Binding.CallKind == SourceCallTypeHint::Kind::RuntimeConstantString) {
    const auto Expected = constantStringSourceHint(
        Image, Binding.TargetAddress, Binding.ImmutablePointerSlot);
    return Expected && Binding.TargetName.empty() && Binding.Selector.empty() &&
           Binding.OwnerClass.empty() && !Binding.SelectorReferenceAddress &&
           objc_projection_detail::sameHint(Expected->Signature, Hint);
  }
  if (Binding.CallKind == SourceCallTypeHint::Kind::RuntimeCStringStorage) {
    const auto Expected = cstringStorageSourceHint(
        Image, Binding.TargetAddress, Binding.ImmutablePointerSlot);
    return Expected && !Expression.IsIndirectCall && !Expression.CallAddr &&
           Expression.CallTarget.empty() &&
           Expression.IntrinsicOutputs.empty() && Binding.TargetName.empty() &&
           Binding.Selector.empty() && Binding.OwnerClass.empty() &&
           !Binding.SelectorReferenceAddress &&
           Binding.ByteCount == Expected->ByteCount &&
           Binding.BorrowedByteInputs.empty() &&
           Binding.SwiftStringInputs.empty() &&
           objc_projection_detail::sameHint(Expected->Signature, Hint);
  }
  if (Binding.CallKind ==
      SourceCallTypeHint::Kind::RuntimeConstantObjectTable) {
    if (!ReadOnlyHelpers || !ReadOnlyHelpers->count(&Expression) ||
        Expression.IsIndirectCall || Expression.CallAddr ||
        !Expression.CallTarget.empty() || !Expression.IntrinsicOutputs.empty())
      return false;
    const auto Expected = constantObjectTableHint(Image, Binding.TargetAddress,
                                                  Binding.ByteCount);
    return Expected && Binding.TargetName.empty() && Binding.Selector.empty() &&
           Binding.OwnerClass.empty() && !Binding.SelectorReferenceAddress &&
           Binding.BorrowedByteInputs.empty() &&
           Binding.SwiftStringInputs.empty() &&
           objc_projection_detail::sameHint(Expected->Signature, Hint);
  }
  if (Binding.CallKind == SourceCallTypeHint::Kind::RuntimeReadOnlyBytes &&
      (!ReadOnlyHelpers || !ReadOnlyHelpers->count(&Expression) ||
       Expression.IsIndirectCall || Expression.CallAddr ||
       !Expression.CallTarget.empty() || !Expression.IntrinsicOutputs.empty()))
    return false;
  if (Binding.CallKind == SourceCallTypeHint::Kind::RuntimeBorrowedBytes ||
      Binding.CallKind == SourceCallTypeHint::Kind::RuntimeReadOnlyBytes) {
    const auto Expected = borrowedByteSourceHint(
        Image, {Binding.TargetAddress, Binding.ByteCount});
    return Expected && Binding.TargetName.empty() && Binding.Selector.empty() &&
           Binding.OwnerClass.empty() && !Binding.SelectorReferenceAddress &&
           Binding.BorrowedByteInputs.empty() &&
           Binding.SwiftStringInputs.empty() &&
           objc_projection_detail::sameHint(Expected->Signature, Hint);
  }
  if (Binding.CallKind ==
      SourceCallTypeHint::Kind::RuntimeProfileCounterStorage) {
    std::optional<ObjCProfileStorage> LocalStorage;
    if (!ProfileStorage) {
      LocalStorage.emplace(Image);
      ProfileStorage = &*LocalStorage;
    }
    const auto Expected = profileStorageHint(Image.Arch, Binding.TargetAddress);
    return ProfileStorage->contains(Binding.TargetAddress) && Expected &&
           Binding.TargetName.empty() && Binding.Selector.empty() &&
           Binding.OwnerClass.empty() && !Binding.SelectorReferenceAddress &&
           objc_projection_detail::sameHint(Expected->Signature, Hint);
  }
  if (Binding.CallKind == SourceCallTypeHint::Kind::RuntimeAssociationKey) {
    const auto Expected = associationKeyHint(Image, Binding.TargetAddress,
                                             Binding.ByteCount == 1);
    return Expected && Binding.TargetName == Expected->TargetName &&
           Binding.Selector.empty() && Binding.OwnerClass.empty() &&
           !Binding.SelectorReferenceAddress &&
           Binding.ByteCount == Expected->ByteCount &&
           objc_projection_detail::sameHint(Expected->Signature, Hint);
  }
  if (Binding.CallKind == SourceCallTypeHint::Kind::RuntimeKVOContext) {
    const auto Expected = kvoContextHint(Image, Binding.TargetAddress);
    return Expected && Binding.TargetName == Expected->TargetName &&
           Binding.Selector.empty() && Binding.OwnerClass.empty() &&
           !Binding.SelectorReferenceAddress && !Binding.ByteCount &&
           objc_projection_detail::sameHint(Expected->Signature, Hint);
  }
  if (Binding.CallKind == SourceCallTypeHint::Kind::RuntimeStaticIdentity) {
    const auto Expected = staticIdentityHint(Image, Binding.TargetAddress,
                                             Binding.ByteCount == 8);
    return Expected && Binding.TargetName == Expected->TargetName &&
           Binding.Selector.empty() && Binding.OwnerClass.empty() &&
           !Binding.SelectorReferenceAddress &&
           Binding.ByteCount == Expected->ByteCount &&
           objc_projection_detail::sameHint(Expected->Signature, Hint);
  }
  if (Binding.CallKind ==
      SourceCallTypeHint::Kind::RuntimeSwiftTypeMetadataAddress) {
    const auto Expected =
        Binding.SwiftTypeMetadata
            ? swiftTypeMetadataAddressHint(Image, Binding.TargetAddress,
                                           *Binding.SwiftTypeMetadata)
            : std::nullopt;
    return Expected && Binding.SwiftTypeMetadata &&
           Expected->SwiftTypeMetadata == Binding.SwiftTypeMetadata &&
           Binding.TargetName == Expected->TargetName &&
           Binding.Selector.empty() && Binding.OwnerClass.empty() &&
           !Binding.SelectorReferenceAddress && !Binding.ByteCount &&
           Binding.BorrowedByteInputs.empty() &&
           Binding.SwiftStringInputs.empty() && !Expression.IsIndirectCall &&
           !Expression.CallAddr && Expression.CallTarget.empty() &&
           Expression.IntrinsicOutputs.empty() &&
           objc_projection_detail::sameHint(Expected->Signature, Hint);
  }
  if (Binding.CallKind ==
      SourceCallTypeHint::Kind::RuntimeSwiftNominalDescriptorAddress) {
    const auto Expected =
        swiftNominalDescriptorAddressHint(Image, Binding.TargetAddress);
    return Expected && Binding.TargetName == Expected->TargetName &&
           Binding.Selector.empty() && Binding.OwnerClass.empty() &&
           !Binding.SelectorReferenceAddress && !Binding.ByteCount &&
           Binding.BorrowedByteInputs.empty() &&
           Binding.SwiftStringInputs.empty() && !Expression.IsIndirectCall &&
           !Expression.CallAddr && Expression.CallTarget.empty() &&
           Expression.IntrinsicOutputs.empty() &&
           objc_projection_detail::sameHint(Expected->Signature, Hint);
  }
  if (Binding.CallKind ==
      SourceCallTypeHint::Kind::RuntimeSwiftNominalMetadataAddress) {
    const auto Expected =
        swiftNominalMetadataAddressHint(Image, Binding.TargetAddress);
    return Expected && Binding.TargetName == Expected->TargetName &&
           Binding.Selector.empty() && Binding.OwnerClass.empty() &&
           !Binding.SelectorReferenceAddress && !Binding.ByteCount &&
           Binding.BorrowedByteInputs.empty() &&
           Binding.SwiftStringInputs.empty() && !Expression.IsIndirectCall &&
           !Expression.CallAddr && Expression.CallTarget.empty() &&
           Expression.IntrinsicOutputs.empty() &&
           objc_projection_detail::sameHint(Expected->Signature, Hint);
  }
  if (Binding.CallKind ==
      SourceCallTypeHint::Kind::RuntimeSwiftWitnessCacheAddress) {
    const HighFunc *ProofFunction = ContainingFunction;
    if (ContainingFunction) {
      const auto Original = Functions.find(ContainingFunction->Entry);
      if (Original != Functions.end())
        ProofFunction = Original->second;
    }
    const auto Expected =
        ProofFunction ? swiftWitnessCacheAddressHint(*ProofFunction, Image,
                                                     Binding.TargetAddress)
                      : std::nullopt;
    return Expected && Binding.TargetName == Expected->TargetName &&
           Binding.Selector.empty() && Binding.OwnerClass.empty() &&
           !Binding.SelectorReferenceAddress && !Binding.ByteCount &&
           Binding.BorrowedByteInputs.empty() &&
           Binding.SwiftStringInputs.empty() && !Expression.IsIndirectCall &&
           !Expression.CallAddr && Expression.CallTarget.empty() &&
           Expression.IntrinsicOutputs.empty() &&
           objc_projection_detail::sameHint(Expected->Signature, Hint);
  }
  if (Binding.CallKind ==
      SourceCallTypeHint::Kind::RuntimeSwiftWitnessAccessor) {
    const auto Found = Functions.find(Binding.TargetAddress);
    const auto Expected =
        Found == Functions.end() || !Found->second
            ? std::nullopt
            : swiftWitnessAccessorCallHint(*Found->second, Image);
    return Expected && !Expression.IsIndirectCall &&
           Expression.CallAddr == Binding.TargetAddress &&
           Expression.Operands.empty() &&
           Binding.TargetName == Expected->TargetName &&
           Binding.Selector.empty() && Binding.OwnerClass.empty() &&
           !Binding.SelectorReferenceAddress && !Binding.ByteCount &&
           Binding.BorrowedByteInputs.empty() &&
           Binding.SwiftStringInputs.empty() &&
           Expression.IntrinsicOutputs.empty() &&
           objc_projection_detail::sameHint(Expected->Signature, Hint);
  }
  if (Binding.CallKind ==
      SourceCallTypeHint::Kind::RuntimeLocalStorageAddress) {
    auto Expected =
        localStorageHint(Image, Binding.TargetAddress, Binding.ByteCount);
    if (!Expected && Binding.ByteCount == 8)
      Expected = oncePredicateStorageHint(Image, Binding.TargetAddress);
    return Expected && Binding.TargetName == Expected->TargetName &&
           Binding.Selector.empty() && Binding.OwnerClass.empty() &&
           !Binding.SelectorReferenceAddress &&
           objc_projection_detail::sameHint(Expected->Signature, Hint);
  }
  if (Binding.CallKind ==
      SourceCallTypeHint::Kind::RuntimeSwiftSmallStringAddress) {
    const auto Expected =
        swiftSmallStringStorageHint(Image, Binding.TargetAddress);
    return Expected && Binding.TargetName == Expected->TargetName &&
           Binding.ByteCount == Expected->ByteCount &&
           Binding.Selector.empty() && Binding.OwnerClass.empty() &&
           !Binding.SelectorReferenceAddress &&
           Binding.BorrowedByteInputs.empty() &&
           Binding.SwiftStringInputs.empty() && !Expression.IsIndirectCall &&
           !Expression.CallAddr && Expression.CallTarget.empty() &&
           Expression.IntrinsicOutputs.empty() &&
           objc_projection_detail::sameHint(Expected->Signature, Hint);
  }
  if (Binding.CallKind ==
          SourceCallTypeHint::Kind::RuntimeClassReferenceAddress ||
      Binding.CallKind ==
          SourceCallTypeHint::Kind::RuntimeMetaclassReferenceAddress) {
    const auto Expected =
        classReferenceAddressHint(Image, Binding.TargetAddress);
    return Expected && Binding.CallKind == Expected->CallKind &&
           Binding.TargetName == Expected->TargetName &&
           Binding.Selector.empty() && Binding.OwnerClass.empty() &&
           !Binding.SelectorReferenceAddress && !Binding.ByteCount &&
           Binding.BorrowedByteInputs.empty() &&
           Binding.SwiftStringInputs.empty() && !Expression.IsIndirectCall &&
           !Expression.CallAddr && Expression.CallTarget.empty() &&
           Expression.Operands.empty() && Expression.IntrinsicOutputs.empty() &&
           objc_projection_detail::sameHint(Expected->Signature, Hint);
  }
  if (isRuntimeReference(Binding.CallKind)) {
    auto Found = Image.ObjCSourceReferences.find(Binding.TargetAddress);
    if (Expression.Operands.empty() &&
        Found != Image.ObjCSourceReferences.end() &&
        Binding.CallKind == runtimeKind(Found->second.TheKind) &&
        Binding.TargetName == Found->second.Name &&
        Binding.OwnerClass == Found->second.ClassName)
      return true;
    if (!Expression.Operands.empty() || !Binding.OwnerClass.empty() ||
        (Binding.CallKind != SourceCallTypeHint::Kind::RuntimeClass &&
         Binding.CallKind != SourceCallTypeHint::Kind::RuntimeMetaclass))
      return false;
    const auto Objects = classObjectIdentities(Image);
    const auto Object = Objects.find(Binding.TargetAddress);
    return Object != Objects.end() && Object->second.Kind == Binding.CallKind &&
           Object->second.Name == Binding.TargetName &&
           Hint.ReturnType->Kind == NdTypeKind::Ptr;
  }
  if (Binding.CallKind == SourceCallTypeHint::Kind::Native) {
    auto Found = Functions.find(Binding.TargetAddress);
    if (Binding.DoesNotReturn) {
      if (Expression.IsIndirectCall ||
          Expression.CallAddr != Binding.TargetAddress ||
          Found == Functions.end() || !Found->second->DoesNotReturn)
        return false;
      const auto Flow = analyzeHighSourceFlow(*Found->second, false);
      if (!Flow.Complete || !Flow.Items.empty())
        return false;
    }
    return Found != Functions.end() && Found->second->SourceTypeHint &&
           objc_projection_detail::sameHint(Hint,
                                            *Found->second->SourceTypeHint);
  }
  if (Binding.CallKind == SourceCallTypeHint::Kind::SwiftValueWitness)
    return Expression.IsIndirectCall && Expression.CallAddr == 0 &&
           isSwiftValueWitnessSourceCallHint(Binding, Image.Arch);
  if (Binding.NilTerminated) {
    const auto Expected = objcSelectorStubSentinelSourceCallHint(
        Image, Binding.TargetAddress, *Binding.Receiver,
        Binding.NilTerminated->Objects);
    if (!Expected || !Expected->NilTerminated || Expression.IsIndirectCall ||
        Expression.CallAddr != Binding.TargetAddress ||
        !Expression.IntrinsicOutputs.empty() ||
        Binding.TargetName != Expected->TargetName ||
        Binding.Selector != Expected->Selector ||
        Binding.SelectorReferenceAddress !=
            Expected->SelectorReferenceAddress ||
        !objc_projection_detail::sameHint(Hint, Expected->Signature))
      return false;
    VarKeyMap<std::vector<ExprPtr>> Definitions;
    if (ContainingFunction)
      walkStmts(ContainingFunction->Body, [&](const HighStmt &Statement) {
        if (Statement.Kind == StmtKind::Assign && Statement.Dst &&
            Statement.Val &&
            (Statement.Dst->Kind == ExprKind::Var ||
             Statement.Dst->Kind == ExprKind::Phi))
          Definitions[varKey(Statement.Dst->Var)].push_back(Statement.Val);
      });
    size_t ObjectBudget = 4096;
    std::set<VarKey> ActiveObjects;
    const auto GlobalAddress = [&](auto &&Self, const ExprPtr &Value,
                                   size_t &Budget, std::set<VarKey> &Active,
                                   unsigned Depth = 0) -> std::optional<va_t> {
      if (!Value || !Budget-- || Depth > 64 || !Value->Type ||
          Value->Type->Size < 8 || Value->Type->Size > 16 ||
          (Value->Type->Kind != NdTypeKind::Int &&
           Value->Type->Kind != NdTypeKind::Ptr) ||
          Value->IntrinsicId != Intrinsic::None ||
          !Value->IntrinsicOutputs.empty() ||
          Value->MemoryOrdering != NdMemoryOrdering::None ||
          Value->MemoryAddressSpace != NdMemoryAddressSpace::Default)
        return std::nullopt;
      if (Value->Kind == ExprKind::Call && Value->SourceCallHint &&
          Value->SourceCallHint->CallKind ==
              SourceCallTypeHint::Kind::DarwinRuntimeGlobalAddress &&
          darwinDeclaredSourceDataObject(
              Image, Value->SourceCallHint->TargetAddress) &&
          objcSourceCallBound(*Value, Image, Functions))
        return Value->SourceCallHint->TargetAddress;
      if ((Value->Kind == ExprKind::Cast || Value->Kind == ExprKind::BitCast) &&
          Value->Operands.size() == 1) {
        if ((Value->CastTo && !equalSourceTypes(Value->Type, Value->CastTo)) ||
            (Value->Kind == ExprKind::BitCast &&
             (!Value->Operands[0] || !Value->Operands[0]->Type ||
              Value->Operands[0]->Type->Size != Value->Type->Size)))
          return std::nullopt;
        return Self(Self, Value->Operands.front(), Budget, Active, Depth + 1);
      }
      if (Value->Kind != ExprKind::Var && Value->Kind != ExprKind::Phi)
        return std::nullopt;
      const auto Key = varKey(Value->Var);
      if (!Active.insert(Key).second)
        return std::nullopt;
      const auto Found = Definitions.find(Key);
      std::optional<va_t> Result;
      bool Valid = Found != Definitions.end() && !Found->second.empty();
      if (Valid)
        for (const auto &Definition : Found->second) {
          const auto Candidate =
              Self(Self, Definition, Budget, Active, Depth + 1);
          if (!Candidate || (Result && *Result != *Candidate)) {
            Valid = false;
            break;
          }
          Result = *Candidate;
        }
      Active.erase(Key);
      if (Valid)
        return Result;
      return std::nullopt;
    };
    const auto ObjectIdentity = [&](auto &&Self, const ExprPtr &Value,
                                    unsigned Depth = 0) -> std::optional<va_t> {
      if (!Value || !ObjectBudget-- || Depth > 64 || !Value->Type ||
          Value->Type->Size < 8 || Value->Type->Size > 16 ||
          (Value->Type->Kind != NdTypeKind::Int &&
           Value->Type->Kind != NdTypeKind::Ptr) ||
          Value->IntrinsicId != Intrinsic::None ||
          !Value->IntrinsicOutputs.empty() ||
          Value->MemoryOrdering != NdMemoryOrdering::None ||
          Value->MemoryAddressSpace != NdMemoryAddressSpace::Default)
        return std::nullopt;
      if (Value->Kind == ExprKind::Const &&
          (!Value->ConstVal || readObjCConstantString(Image, Value->ConstVal)))
        return Value->ConstVal;
      if (Value->Kind == ExprKind::Call && Value->SourceCallHint &&
          Value->SourceCallHint->CallKind ==
              SourceCallTypeHint::Kind::RuntimeConstantString &&
          objcSourceCallBound(*Value, Image, Functions))
        return Value->SourceCallHint->TargetAddress;
      if (Value->Kind == ExprKind::Load && Value->Operands.size() == 1) {
        const auto &Address = Value->Operands.front();
        size_t AddressBudget = 4096;
        std::set<VarKey> ActiveAddresses;
        if (const auto Storage = GlobalAddress(GlobalAddress, Address,
                                               AddressBudget, ActiveAddresses))
          return Storage;
      }
      if ((Value->Kind == ExprKind::Cast || Value->Kind == ExprKind::BitCast) &&
          Value->Operands.size() == 1) {
        if ((Value->CastTo && !equalSourceTypes(Value->Type, Value->CastTo)) ||
            (Value->Kind == ExprKind::BitCast &&
             (!Value->Operands[0] || !Value->Operands[0]->Type ||
              Value->Operands[0]->Type->Size != Value->Type->Size)))
          return std::nullopt;
        return Self(Self, Value->Operands.front(), Depth + 1);
      }
      if (Value->Kind != ExprKind::Var && Value->Kind != ExprKind::Phi)
        return std::nullopt;
      const auto Key = varKey(Value->Var);
      if (!ActiveObjects.insert(Key).second)
        return std::nullopt;
      const auto Found = Definitions.find(Key);
      std::optional<va_t> Result;
      bool Valid = Found != Definitions.end() && !Found->second.empty();
      if (Valid)
        for (const auto &Definition : Found->second) {
          const auto Candidate = Self(Self, Definition, Depth + 1);
          if (!Candidate || (Result && *Result != *Candidate)) {
            Valid = false;
            break;
          }
          Result = *Candidate;
        }
      ActiveObjects.erase(Key);
      if (Valid)
        return Result;
      return std::nullopt;
    };
    const auto Query = [&](const HighExpr &Value) {
      if (!Value.SourceCallHint)
        return false;
      const auto Kind = Value.SourceCallHint->CallKind;
      return (Kind == SourceCallTypeHint::Kind::RuntimeClass ||
              Kind == SourceCallTypeHint::Kind::RuntimeSelector ||
              Kind == SourceCallTypeHint::Kind::RuntimeConstantString ||
              Kind == SourceCallTypeHint::Kind::DarwinRuntimeGlobalAddress) &&
             objcSourceCallBound(Value, Image, Functions);
    };
    const auto StackLoads =
        ContainingFunction ? sentinelPrivateStackLoads(
                                 *ContainingFunction, Expression,
                                 [&](const ExprPtr &Value) {
                                   return ObjectIdentity(ObjectIdentity, Value);
                                 },
                                 Query)
                           : std::map<const HighExpr *, va_t>{};
    enum class Identity { Object, Class, Selector };
    size_t Budget = 4096;
    std::set<VarKey> Active;
    const auto Matches = [&](auto &&Self, const ExprPtr &Value, va_t Address,
                             Identity Kind, unsigned Depth) -> bool {
      if (!Value || !Budget-- || Depth > 64 || !Value->Type ||
          Value->IntrinsicId != Intrinsic::None ||
          !Value->IntrinsicOutputs.empty() ||
          Value->MemoryOrdering != NdMemoryOrdering::None ||
          Value->MemoryAddressSpace != NdMemoryAddressSpace::Default)
        return false;
      if (Value->Kind == ExprKind::Const)
        return Kind == Identity::Object && Value->ConstVal == Address &&
               (Value->Type->Kind == NdTypeKind::Int ||
                Value->Type->Kind == NdTypeKind::Ptr) &&
               (Value->Type->Size == 8 ||
                (!Address && Value->Type->Size && Value->Type->Size < 8));
      if (Value->Type->Size < 8 || Value->Type->Size > 16 ||
          (Value->Type->Kind != NdTypeKind::Int &&
           Value->Type->Kind != NdTypeKind::Ptr))
        return false;
      if (Value->Kind == ExprKind::Load) {
        if (Kind == Identity::Object &&
            ObjectIdentity(ObjectIdentity, Value) == Address)
          return true;
        const auto Load = StackLoads.find(Value.get());
        return Kind == Identity::Object && Load != StackLoads.end() &&
               Load->second == Address;
      }
      if ((Value->Kind == ExprKind::Cast || Value->Kind == ExprKind::BitCast) &&
          Value->Operands.size() == 1) {
        if ((Value->CastTo && !equalSourceTypes(Value->Type, Value->CastTo)) ||
            (Value->Kind == ExprKind::BitCast &&
             (!Value->Operands[0] || !Value->Operands[0]->Type ||
              Value->Operands[0]->Type->Size != 8)))
          return false;
        return Self(Self, Value->Operands.front(), Address, Kind, Depth + 1);
      }
      if (Value->Kind == ExprKind::Call && Value->SourceCallHint) {
        const auto &Source = *Value->SourceCallHint;
        const auto Wanted =
            Kind == Identity::Object
                ? SourceCallTypeHint::Kind::RuntimeConstantString
            : Kind == Identity::Class
                ? SourceCallTypeHint::Kind::RuntimeClass
                : SourceCallTypeHint::Kind::RuntimeSelector;
        return Source.CallKind == Wanted && Source.TargetAddress == Address &&
               Value->Operands.empty() &&
               objcSourceCallBound(*Value, Image, Functions);
      }
      if (Value->Kind == ExprKind::BinOp && Value->Op == NdOp::SELECT &&
          Value->Operands.size() == 3)
        return Self(Self, Value->Operands[1], Address, Kind, Depth + 1) &&
               Self(Self, Value->Operands[2], Address, Kind, Depth + 1);
      if ((Value->Kind != ExprKind::Var && Value->Kind != ExprKind::Phi) ||
          (Value->Var.Kind != MedVar::Reg && Value->Var.Kind != MedVar::Temp))
        return false;
      const auto Key = varKey(Value->Var);
      if (!Active.insert(Key).second)
        return false;
      const auto Found = Definitions.find(Key);
      bool Valid = Found != Definitions.end() && !Found->second.empty();
      if (Valid)
        for (const auto &Definition : Found->second)
          if (!Self(Self, Definition, Address, Kind, Depth + 1)) {
            Valid = false;
            break;
          }
      Active.erase(Key);
      return Valid;
    };
    if (!Matches(Matches, Expression.Operands[0], Binding.Receiver->Address,
                 Identity::Class, 0) ||
        !Matches(Matches, Expression.Operands[1],
                 Binding.SelectorReferenceAddress, Identity::Selector, 0))
      return false;
    const auto &Objects = Binding.NilTerminated->Objects;
    for (size_t I = 0; I < Objects.size(); ++I)
      if (!Matches(Matches, Expression.Operands[I + 2], Objects[I],
                   Identity::Object, 0))
        return false;
    return Matches(Matches, Expression.Operands[Objects.size() + 2], 0,
                   Identity::Object, 0);
  }
  if (Binding.Format) {
    const auto &Format = *Binding.Format;
    if (Format.DynamicWithoutArguments || Format.DynamicPointerArguments ||
        Format.DynamicInteger64Arguments) {
      const auto Stub = objcSelectorStubDynamicFormatSourceCallHint(
          Image, Binding.TargetAddress);
      const unsigned TailArguments =
          Hint.Parameters.size() >= Format.FixedCount
              ? unsigned(Hint.Parameters.size() - Format.FixedCount)
              : 0;
      std::vector<TypeRef> IntegerTypes;
      if (Format.DynamicInteger64Arguments && TailArguments)
        for (size_t I = Format.FixedCount; I < Hint.Parameters.size(); ++I)
          IntegerTypes.push_back(Hint.Parameters[I].Type);
      auto Expected =
          Stub && Stub->Format
              ? (Format.DynamicInteger64Arguments
                     ? objcDynamicFormatInteger64ArgumentsSourceCallHint(
                           Image, Stub->Selector, IntegerTypes)
                     : objcDynamicFormatPointerArgumentsSourceCallHint(
                           Image, Stub->Selector, TailArguments))
              : std::nullopt;
      if (Expected && Stub) {
        Expected->TargetAddress = Stub->TargetAddress;
        Expected->SelectorReferenceAddress = Stub->SelectorReferenceAddress;
      }
      const bool ProvenTail =
          (Format.DynamicPointerArguments ||
           Format.DynamicInteger64Arguments) &&
          TailArguments &&
          Expression.Operands.size() == Hint.Parameters.size() &&
          ContainingFunction && [&] {
            VarKeyMap<std::vector<ExprPtr>> Definitions;
            walkStmts(ContainingFunction->Body, [&](const HighStmt &Statement) {
              if (Statement.Kind == StmtKind::Assign && Statement.Dst &&
                  Statement.Val &&
                  (Statement.Dst->Kind == ExprKind::Var ||
                   Statement.Dst->Kind == ExprKind::Phi))
                Definitions[varKey(Statement.Dst->Var)].push_back(
                    Statement.Val);
            });
            size_t Budget = 4096;
            std::set<VarKey> Active;
            for (size_t I = Format.FixedCount; I < Expression.Operands.size();
                 ++I) {
              if (Format.DynamicInteger64Arguments) {
                const auto Type = provenSourceInteger64Value(
                    Expression.Operands[I], Definitions, Image, Budget, Active);
                if (!Type || !equalSourceTypes(Type, Hint.Parameters[I].Type))
                  return false;
              } else if (!provenSourcePointerValue(Expression.Operands[I],
                                                   Definitions, Image, Budget,
                                                   Active)) {
                return false;
              }
            }
            return true;
          }();
      return Expected && Expected->Format && !Expression.IsIndirectCall &&
             Expression.CallAddr == Binding.TargetAddress &&
             Expression.CallTarget.empty() &&
             Binding.CallKind == SourceCallTypeHint::Kind::ObjCMessage &&
             Binding.TargetName == Expected->TargetName &&
             Binding.Selector == Expected->Selector &&
             Binding.SelectorReferenceAddress ==
                 Expected->SelectorReferenceAddress &&
             !Binding.DoesNotReturn && !Binding.WeakImport &&
             !Binding.ReturnedArgument && !Binding.RuntimeObjCResultType &&
             Binding.OwnerClass.empty() && Binding.BorrowedByteInputs.empty() &&
             Binding.SwiftStringInputs.empty() && !Binding.SwiftTypeMetadata &&
             !Binding.Receiver && !Binding.SelectorResultUse &&
             !Binding.SelectorResultTypeUse &&
             !Binding.SelectorArgumentTypeUse &&
             !Binding.SelectorForwardingUse &&
             !Binding.SelectorArgumentStorageUse &&
             !Binding.ObjCIndirectResultStorage && !Binding.ByteCount &&
             !Binding.ImmutablePointerSlot && !Format.FormatAddress &&
             Format.AlternativeFormatAddresses.empty() &&
             (unsigned(Format.DynamicWithoutArguments) +
                  unsigned(Format.DynamicPointerArguments) +
                  unsigned(Format.DynamicInteger64Arguments) ==
              1) &&
             std::all_of(
                 Expression.Operands.begin(), Expression.Operands.end(),
                 [](const ExprPtr &Operand) { return bool(Operand); }) &&
             ((Format.DynamicWithoutArguments && !TailArguments &&
               Format.FixedCount == Expression.Operands.size()) ||
              ProvenTail) &&
             Format.FormatParameter < Format.FixedCount &&
             Format.FixedCount == Expected->Format->FixedCount &&
             Format.FormatParameter == Expected->Format->FormatParameter &&
             Format.Syntax == Expected->Format->Syntax &&
             Format.DynamicWithoutArguments ==
                 Expected->Format->DynamicWithoutArguments &&
             Format.DynamicPointerArguments ==
                 Expected->Format->DynamicPointerArguments &&
             Format.DynamicInteger64Arguments ==
                 Expected->Format->DynamicInteger64Arguments &&
             objc_projection_detail::sameHint(Hint, Expected->Signature);
    }
    const auto Addresses = formatAddresses(Format);
    if (!Addresses)
      return false;
    const bool Message =
        Binding.CallKind == SourceCallTypeHint::Kind::ObjCMessage;
    const auto Expected =
        Message
            ? objcFormattedSourceCallHint(Image, Binding.Selector, *Addresses)
            : darwinFormattedSourceCallHint(Image, Binding.TargetAddress,
                                            Format.FormatAddress);
    if (!Expected || !Expected->Format || Binding.DoesNotReturn ||
        Format.Syntax != Expected->Format->Syntax ||
        Format.FixedCount != Expected->Format->FixedCount ||
        Format.FormatParameter != Expected->Format->FormatParameter ||
        Format.DynamicWithoutArguments !=
            Expected->Format->DynamicWithoutArguments ||
        Format.DynamicPointerArguments !=
            Expected->Format->DynamicPointerArguments ||
        Format.DynamicInteger64Arguments !=
            Expected->Format->DynamicInteger64Arguments ||
        Format.AlternativeFormatAddresses !=
            Expected->Format->AlternativeFormatAddresses ||
        Format.FormatParameter >= Expression.Operands.size() ||
        !objc_projection_detail::sameHint(Hint, Expected->Signature) ||
        (!Message && !runtimeBindingMatches(Binding, *Expected)))
      return false;
    auto Argument = Expression.Operands[Format.FormatParameter];
    const std::set<va_t> CandidateSet(Addresses->begin(), Addresses->end());
    VarKeyMap<std::vector<ExprPtr>> Definitions;
    if (ContainingFunction)
      walkStmts(ContainingFunction->Body, [&](const HighStmt &Statement) {
        if (Statement.Kind == StmtKind::Assign && Statement.Dst &&
            Statement.Val &&
            (Statement.Dst->Kind == ExprKind::Var ||
             Statement.Dst->Kind == ExprKind::Phi))
          Definitions[varKey(Statement.Dst->Var)].push_back(Statement.Val);
      });
    size_t CandidateBudget = 4096;
    std::set<VarKey> ActiveVariables;
    const auto MatchesCandidate = [&](auto &&Self, const ExprPtr &Value,
                                      unsigned Depth) -> bool {
      if (!Value || !CandidateBudget-- || Depth > 64)
        return false;
      if ((Value->Kind == ExprKind::Cast || Value->Kind == ExprKind::BitCast) &&
          Value->Operands.size() == 1 && Value->Type && Value->Type->Size == 8)
        return Self(Self, Value->Operands.front(), Depth + 1);
      if (Value->Kind == ExprKind::Call && Value->SourceCallHint &&
          Value->SourceCallHint->CallKind ==
              SourceCallTypeHint::Kind::RuntimeConstantString)
        return Value->Operands.empty() &&
               CandidateSet.count(Value->SourceCallHint->TargetAddress) &&
               objcSourceCallBound(*Value, Image, Functions);
      if (Value->Kind == ExprKind::Const)
        return CandidateSet.count(Value->ConstVal) != 0;
      if (Value->Kind == ExprKind::BinOp && Value->Op == NdOp::SELECT &&
          Value->Operands.size() == 3)
        return Self(Self, Value->Operands[1], Depth + 1) &&
               Self(Self, Value->Operands[2], Depth + 1);
      if (Value->Kind != ExprKind::Var && Value->Kind != ExprKind::Phi)
        return false;
      const auto Key = varKey(Value->Var);
      if (!ActiveVariables.insert(Key).second)
        return false;
      const auto Found = Definitions.find(Key);
      bool Valid = Found != Definitions.end() && !Found->second.empty();
      if (Valid)
        for (const auto &Definition : Found->second)
          if (!Self(Self, Definition, Depth + 1)) {
            Valid = false;
            break;
          }
      ActiveVariables.erase(Key);
      return Valid;
    };
    if (MatchesCandidate(MatchesCandidate, Argument, 0))
      return true;
    if (Format.Syntax == SourceCallTypeHint::FormatSyntax::Printf) {
      auto CStringBase = [&](const ExprPtr &Value) -> std::optional<va_t> {
        if (!Value || Value->Kind != ExprKind::Call || !Value->SourceCallHint ||
            Value->SourceCallHint->CallKind !=
                SourceCallTypeHint::Kind::RuntimeCStringStorage ||
            !objcSourceCallBound(*Value, Image, Functions))
          return std::nullopt;
        return Value->SourceCallHint->TargetAddress;
      };
      if (const auto Base = CStringBase(Argument))
        return CandidateSet.count(*Base) != 0;
      if (Argument->Kind == ExprKind::BinOp && Argument->Op == NdOp::INT_ADD &&
          Argument->Operands.size() == 2) {
        for (unsigned I = 0; I < 2; ++I) {
          const auto Base = CStringBase(Argument->Operands[I]);
          const auto Offset = constantAddress(*Argument->Operands[1 - I]);
          if (Base && Offset && *Base <= InvalidVA - *Offset)
            return CandidateSet.count(*Base + *Offset) != 0;
        }
      }
    }
    const auto Constant = constantAddress(*Argument);
    return Constant && CandidateSet.count(*Constant) != 0;
  }
  if (Binding.CallKind == SourceCallTypeHint::Kind::ObjCRuntimeCall ||
      Binding.CallKind == SourceCallTypeHint::Kind::SwiftRuntimeCall ||
      Binding.CallKind == SourceCallTypeHint::Kind::SwiftStringBridge ||
      Binding.CallKind == SourceCallTypeHint::Kind::SwiftStringFromNSString ||
      Binding.CallKind == SourceCallTypeHint::Kind::DarwinRuntimeCall ||
      Binding.CallKind ==
          SourceCallTypeHint::Kind::DarwinRuntimeGlobalAddress) {
    const auto Expected = runtimeSourceCallHint(Image, Binding);
    if (Expected && runtimeBindingMatches(Binding, *Expected)) {
      for (const auto &[WordIndex, StorageIndex] :
           Expected->SwiftStringInputs) {
        if (WordIndex >= Expression.Operands.size() ||
            StorageIndex >= Expression.Operands.size() ||
            !containsBorrowedStorage(Expression.Operands[StorageIndex]))
          continue;
        const auto *Storage =
            swiftLiteralStorageHelper(Expression.Operands[StorageIndex]);
        const auto Word =
            swiftLiteralStorageWord(Expression.Operands[WordIndex]);
        if (Expression.IsIndirectCall || !Storage || !Word ||
            Storage->SourceCallHint->TargetAddress <
                SwiftLiteralString::StorageBias ||
            !objcSourceCallBound(*Storage, Image, Functions))
          return false;
        const auto &Bytes = *Storage->SourceCallHint;
        const auto Literal = swiftLiteralString(
            Image, *Word,
            (Bytes.TargetAddress - SwiftLiteralString::StorageBias) |
                SwiftLiteralString::ImmortalTag);
        if (!Literal || Literal->Contents != Bytes.TargetAddress ||
            Literal->Bytes != Bytes.ByteCount)
          return false;
      }
    }
    // HighIR retains the original veneer spelling in CallTarget. The source
    // emitter uses the canonical operation carried by this runtime binding.
    return Expected && runtimeBindingMatches(Binding, *Expected);
  }
  if (Binding.Receiver) {
    if (Binding.Receiver->Origin ==
            ObjCReceiverTypeHint::OriginKind::BlockParameter &&
        BlockParameterReceivers) {
      if (!ContainingFunction)
        return false;
      const auto Function =
          BlockParameterReceivers->find(ContainingFunction->Entry);
      if (Function == BlockParameterReceivers->end())
        return false;
      const auto Parameter =
          Function->second.find(Binding.Receiver->SourceParameter);
      if (Parameter == Function->second.end())
        return false;
      auto Root = *Binding.Receiver;
      Root.Steps.clear();
      if (!(Root == Parameter->second))
        return false;
    }
    const auto Expected =
        Binding.ObjCIndirectResultStorage
            ? objcNonNilSelfSourceTypeHint(Image, Binding.Selector,
                                           *Binding.Receiver)
        : Binding.CallKind == SourceCallTypeHint::Kind::ObjCSuper2
            ? objcSuperSourceTypeHint(Image, Binding.Selector,
                                      *Binding.Receiver)
            : objcReceiverSourceTypeHint(Image, Binding.Selector,
                                         *Binding.Receiver);
    if (!Expected.HasDeclaration || !Expected.Signature ||
        !objc_projection_detail::sameHint(Hint, *Expected.Signature))
      return false;
  } else if (Hint.Origin == SourceFunctionTypeHint::OriginKind::ObjCSDK ||
             (Hint.Origin == SourceFunctionTypeHint::OriginKind::ObjCRuntime &&
              ((Binding.SelectorResultUse && Binding.SelectorResultTypeUse) ||
               Binding.SelectorForwardingUse))) {
    const auto Expected =
        Binding.SelectorResultUse
            ? objcSelectorSourceTypeHintForResultUse(
                  Image, Binding.Selector, *Binding.SelectorResultUse,
                  Binding.SelectorResultTypeUse)
        : Binding.SelectorArgumentTypeUse
            ? objcSelectorSourceTypeHintForArgumentTypeUse(
                  Image, Binding.Selector, *Binding.SelectorArgumentTypeUse)
        : Binding.SelectorForwardingUse
            ? objcSelectorSourceTypeHintForForwardingUse(
                  Image, Binding.Selector, *Binding.SelectorForwardingUse)
        : Binding.SelectorArgumentStorageUse
            ? objcSelectorSourceTypeHintForArgumentStorageUse(
                  Image, Binding.Selector, *Binding.SelectorArgumentStorageUse)
            : objcSelectorSourceTypeHint(Image, Binding.Selector);
    if (!Expected || !objc_projection_detail::sameHint(Hint, *Expected))
      return false;
  }
  return (Binding.CallKind == SourceCallTypeHint::Kind::ObjCMessage ||
          Binding.CallKind == SourceCallTypeHint::Kind::ObjCSuper2) &&
         Hint.Parameters.size() >= 2 && !Binding.Selector.empty();
}

inline std::string
renderObjCAssociationKeyHelpers(const std::set<va_t> &Keys,
                                std::set<std::string> &SharedFunctions) {
  std::string Source;
  for (va_t Key : Keys) {
    const std::string Name = "neverd_objc_association_key_" +
                             llvm::utohexstr(Key, true) + "_address";
    SharedFunctions.insert(Name);
    Source += "\nuintptr_t " + Name +
              "(void) {\n"
              "  static unsigned char key;\n"
              "  return (uintptr_t)&key;\n}\n";
  }
  return Source;
}

inline std::string
renderObjCKVOContextHelpers(const std::set<va_t> &Contexts,
                            std::set<std::string> &SharedFunctions) {
  std::string Source;
  for (va_t Address : Contexts) {
    const std::string Name = "neverd_objc_kvo_context_" +
                             llvm::utohexstr(Address, true) + "_address";
    SharedFunctions.insert(Name);
    Source += "\nuintptr_t " + Name +
              "(void) {\n"
              "  static unsigned char context;\n"
              "  return (uintptr_t)&context;\n}\n";
  }
  return Source;
}

inline std::string
renderObjCConstantObjectTableHelpers(const BinaryImage &Image,
                                     const std::map<va_t, uint32_t> &Tables,
                                     std::set<std::string> &SharedFunctions) {
  std::string Source;
  for (const auto &[Address, ByteCount] : Tables) {
    const auto Entries = objc_binding_detail::constantObjectTableEntries(
        Image, Address, ByteCount);
    if (!Entries)
      throw std::runtime_error("constant-object table is no longer valid");
    const std::string Name = "neverd_objc_constant_object_table_" +
                             llvm::utohexstr(Address, true) + "_address";
    SharedFunctions.insert(Name);
    Source += "\nuintptr_t " + Name + "(void) {\n";
    std::set<std::pair<va_t, bool>> Declared;
    for (const auto &Entry : *Entries) {
      if (!Entry.Target ||
          !Declared.emplace(Entry.Target, Entry.IsString).second)
        continue;
      Source += "  extern uintptr_t neverd_objc_constant_" +
                std::string(Entry.IsString ? "string_" : "object_") +
                llvm::utohexstr(Entry.Target, true) + "_address(void);\n";
    }
    Source += "  static const void *entries[" +
              std::to_string(Entries->size()) +
              "];\n"
              "  static unsigned state;\n"
              "  if (__atomic_load_n(&state, __ATOMIC_ACQUIRE) != 2) {\n"
              "    unsigned expected = 0;\n"
              "    if (__atomic_compare_exchange_n(&state, &expected, 1, 0, "
              "__ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {\n";
    for (size_t Index = 0; Index < Entries->size(); ++Index) {
      const auto &Entry = (*Entries)[Index];
      Source += "      entries[" + std::to_string(Index) + "] = ";
      if (!Entry.Target)
        Source += "0;\n";
      else
        Source += "(const void *)neverd_objc_constant_" +
                  std::string(Entry.IsString ? "string_" : "object_") +
                  llvm::utohexstr(Entry.Target, true) + "_address();\n";
    }
    Source +=
        "      __atomic_store_n(&state, 2, __ATOMIC_RELEASE);\n"
        "    } else {\n"
        "      while (__atomic_load_n(&state, __ATOMIC_ACQUIRE) != 2) {}\n"
        "    }\n  }\n"
        "  return (uintptr_t)entries;\n}\n";
  }
  return Source;
}

inline std::string
renderObjCStaticIdentityHelpers(const std::set<va_t> &Identities,
                                std::set<std::string> &SharedFunctions) {
  std::string Source;
  for (va_t Address : Identities) {
    const std::string Name =
        "neverd_static_identity_" + llvm::utohexstr(Address, true) + "_address";
    SharedFunctions.insert(Name);
    Source += "\nuintptr_t " + Name +
              "(void) {\n"
              "  static void *identity = &identity;\n"
              "  return (uintptr_t)&identity;\n}\n";
  }
  return Source;
}

inline std::string
renderObjCClassReferenceHelpers(const BinaryImage &Image,
                                const std::set<va_t> &References,
                                std::set<std::string> &SharedFunctions) {
  std::string Source;
  bool NeedsClassLookup = false;
  bool NeedsMetaclassLookup = false;
  for (const va_t Address : References) {
    const auto Hint =
        objc_binding_detail::classReferenceAddressHint(Image, Address);
    if (!Hint)
      throw std::runtime_error(
          "Objective-C class-reference cell is no longer valid");
    const bool Metaclass =
        Hint->CallKind ==
        SourceCallTypeHint::Kind::RuntimeMetaclassReferenceAddress;
    (Metaclass ? NeedsMetaclassLookup : NeedsClassLookup) = true;
    const std::string Name = "neverd_objc_class_reference_" +
                             llvm::utohexstr(Address, true) + "_address";
    SharedFunctions.insert(Name);
    Source += "\nuintptr_t " + Name +
              "(void) {\n"
              "  static void *reference;\n"
              "  static unsigned state;\n"
              "  if (__atomic_load_n(&state, __ATOMIC_ACQUIRE) != 2) {\n"
              "    unsigned expected = 0;\n"
              "    if (__atomic_compare_exchange_n(&state, &expected, 1, 0, "
              "__ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {\n"
              "      reference = " +
              std::string(Metaclass ? "objc_getMetaClass" : "objc_getClass") +
              "(\"" + Hint->TargetName +
              "\");\n"
              "      __atomic_store_n(&state, 2, __ATOMIC_RELEASE);\n"
              "    } else {\n"
              "      while (__atomic_load_n(&state, __ATOMIC_ACQUIRE) != 2) "
              "{}\n"
              "    }\n  }\n"
              "  return (uintptr_t)&reference;\n}\n";
  }
  std::string Declarations;
  if (NeedsClassLookup || NeedsMetaclassLookup)
    Declarations += "\nstruct objc_class;\n";
  if (NeedsClassLookup)
    Declarations += "extern struct objc_class *objc_getClass(const char *);\n";
  if (NeedsMetaclassLookup)
    Declarations +=
        "extern struct objc_class *objc_getMetaClass(const char *);\n";
  return Declarations + Source;
}

inline std::string renderObjCSwiftTypeMetadataHelpers(
    const BinaryImage &Image,
    const std::map<va_t, SourceCallTypeHint::SwiftTypeMetadataAddress> &Pairs,
    std::set<std::string> &SharedFunctions) {
  std::string Source;
  for (const auto &[CacheAddress, Pair] : Pairs) {
    const auto Proof = objc_binding_detail::swiftTypeMetadataPairProof(
        Image, CacheAddress, Pair.ReferenceAddress);
    if (!Proof || Proof->Address != Pair)
      throw std::runtime_error("Swift type metadata pair is no longer valid");
    const std::string Stem = "neverd_swift_type_metadata_" +
                             llvm::utohexstr(Pair.CacheAddress, true) + "_" +
                             llvm::utohexstr(Pair.ReferenceAddress, true);
    const std::string CacheName = Stem + "_cache_address";
    const std::string ReferenceName = Stem + "_reference_address";
    const std::string DescriptorName = Stem + "_nominal_descriptor";
    const std::string StorageType = Stem + "_storage_type";
    const std::string StorageName = Stem + "_storage";
    const std::string InitializeName = Stem + "_initialize";
    SharedFunctions.insert(CacheName);
    SharedFunctions.insert(ReferenceName);
    const uint64_t Length = Proof->TypeReference.size();
    const auto IndexedName = [](llvm::StringRef Base, size_t I) {
      return I ? Base.str() + "_" + std::to_string(I) : Base.str();
    };
    for (size_t I = 0; I < Proof->Descriptors.size(); ++I)
      Source += (I ? "extern unsigned char " : "\nextern unsigned char ") +
                IndexedName(DescriptorName, I) + "[] __asm__(\"" +
                Proof->Descriptors[I].Symbol + "\");\n";
    if (Proof->Descriptors.empty())
      Source += "\n";
    Source += "struct " + StorageType +
              " {\n"
              "  void *cache;\n";
    for (size_t I = 0; I < Proof->Descriptors.size(); ++I)
      Source += "  const void *" + IndexedName("descriptor", I) + ";\n";
    Source += "  unsigned char type_reference[" + std::to_string(Length + 1) +
              "];\n"
              "  struct { int32_t relative; uint32_t length; } reference;\n"
              "  unsigned state;\n"
              "};\n"
              "static struct " +
              StorageType + " " + StorageName +
              ";\n"
              "static void " +
              InitializeName +
              "(void) {\n"
              "  if (__atomic_load_n(&" +
              StorageName +
              ".state, __ATOMIC_ACQUIRE) != 2) {\n"
              "    unsigned expected = 0;\n"
              "    if (__atomic_compare_exchange_n(&" +
              StorageName +
              ".state, &expected, 1, 0, __ATOMIC_ACQUIRE, "
              "__ATOMIC_ACQUIRE)) {\n";
    for (size_t I = 0; I < Proof->Descriptors.size(); ++I)
      Source += "      " + StorageName + "." + IndexedName("descriptor", I) +
                " = " + IndexedName(DescriptorName, I) + ";\n";
    size_t DescriptorIndex = 0;
    for (size_t I = 0; I < Proof->TypeReference.size();) {
      if (DescriptorIndex < Proof->Descriptors.size() &&
          Proof->Descriptors[DescriptorIndex].Offset == I) {
        const std::string Index =
            DescriptorIndex ? "_" + std::to_string(DescriptorIndex) : "";
        Source += "      " + StorageName + ".type_reference[" +
                  std::to_string(I) +
                  "] = 2;\n"
                  "      intptr_t descriptor_delta" +
                  Index + " = (const unsigned char *)&" + StorageName + "." +
                  IndexedName("descriptor", DescriptorIndex) +
                  " - (const unsigned char *)&" + StorageName +
                  ".type_reference[" + std::to_string(I + 1) +
                  "];\n"
                  "      if (descriptor_delta" +
                  Index + " < INT32_MIN || descriptor_delta" + Index +
                  " > INT32_MAX) __builtin_trap();\n"
                  "      int32_t descriptor_relative" +
                  Index + " = (int32_t)descriptor_delta" + Index +
                  ";\n"
                  "      __builtin_memcpy(&" +
                  StorageName + ".type_reference[" + std::to_string(I + 1) +
                  "], &descriptor_relative" + Index + ", 4);\n";
        ++DescriptorIndex;
        I += 5;
        continue;
      }
      Source +=
          "      " + StorageName + ".type_reference[" + std::to_string(I) +
          "] = " +
          std::to_string(static_cast<unsigned char>(Proof->TypeReference[I])) +
          ";\n";
      ++I;
    }
    Source += "      " + StorageName + ".type_reference[" +
              std::to_string(Length) +
              "] = 0;\n"
              "      intptr_t reference_delta = (const unsigned char *)&" +
              StorageName + ".type_reference[0] - (const unsigned char *)&" +
              StorageName +
              ".reference.relative;\n"
              "      if (reference_delta < INT32_MIN || reference_delta > "
              "INT32_MAX) __builtin_trap();\n"
              "      " +
              StorageName +
              ".reference.relative = (int32_t)reference_delta;\n"
              "      " +
              StorageName + ".reference.length = " + std::to_string(Length) +
              ";\n"
              "      __atomic_store_n(&" +
              StorageName +
              ".state, 2, __ATOMIC_RELEASE);\n"
              "    } else {\n"
              "      while (__atomic_load_n(&" +
              StorageName +
              ".state, __ATOMIC_ACQUIRE) != 2) {}\n"
              "    }\n"
              "  }\n"
              "}\n"
              "uintptr_t " +
              CacheName +
              "(void) {\n"
              "  " +
              InitializeName +
              "();\n"
              "  return (uintptr_t)&" +
              StorageName +
              ".cache;\n"
              "}\n"
              "uintptr_t " +
              ReferenceName +
              "(void) {\n"
              "  " +
              InitializeName +
              "();\n"
              "  return (uintptr_t)&" +
              StorageName + ".reference;\n}\n";
  }
  return Source;
}

inline std::string renderObjCSwiftNominalDescriptorHelpers(
    const BinaryImage &Image, const std::map<va_t, std::string> &Descriptors,
    std::set<std::string> &SharedFunctions) {
  std::string Source;
  for (const auto &[Address, Symbol] : Descriptors) {
    const auto Expected =
        objc_binding_detail::swiftNominalDescriptorAddressHint(Image, Address);
    if (!Expected || Expected->TargetName != Symbol)
      throw std::runtime_error("Swift nominal descriptor is no longer valid");
    const std::string Stem =
        "neverd_swift_nominal_descriptor_" + llvm::utohexstr(Address, true);
    SharedFunctions.insert(Stem + "_address");
    Source += "\nextern unsigned char " + Stem + "_bytes[] __asm__(\"" +
              Symbol + "\");\n";
    Source += "uintptr_t " + Stem +
              "_address(void) {\n"
              "  return (uintptr_t)" +
              Stem + "_bytes;\n}\n";
  }
  return Source;
}

inline std::string renderObjCSwiftNominalMetadataHelpers(
    const BinaryImage &Image, const std::map<va_t, std::string> &Metadata,
    std::set<std::string> &SharedFunctions) {
  std::string Source;
  for (const auto &[Address, Symbol] : Metadata) {
    const auto Expected =
        objc_binding_detail::swiftNominalMetadataAddressHint(Image, Address);
    if (!Expected || Expected->TargetName != Symbol)
      throw std::runtime_error("Swift nominal metadata is no longer valid");
    const std::string Stem =
        "neverd_swift_nominal_metadata_" + llvm::utohexstr(Address, true);
    SharedFunctions.insert(Stem + "_address");
    Source += "\nextern unsigned char " + Stem + "_bytes[] __asm__(\"" +
              Symbol + "\");\n";
    Source += "uintptr_t " + Stem +
              "_address(void) {\n"
              "  return (uintptr_t)" +
              Stem + "_bytes;\n}\n";
  }
  return Source;
}

inline std::string renderObjCSwiftWitnessCacheHelpers(
    const BinaryImage &Image, const std::map<va_t, va_t> &Caches,
    const std::map<va_t, const HighFunc *> &Functions,
    std::set<std::string> &SharedFunctions) {
  std::string Source;
  if (!Caches.empty())
    Source +=
        "\nextern void *neverd_swift_get_witness_table(void *, void *, void *) "
        "__asm__(\"_swift_getWitnessTable\");\n";
  for (const auto &[CacheAddress, AccessorAddress] : Caches) {
    const auto Function = Functions.find(AccessorAddress);
    std::array<std::string, 2> Globals;
    const auto Current =
        Function == Functions.end() || !Function->second
            ? std::nullopt
            : objc_binding_detail::swiftWitnessCacheAddressHint(
                  *Function->second, Image, CacheAddress, &Globals);
    if (!Current)
      throw std::runtime_error("Swift witness cache is no longer valid");
    const std::string CacheStem =
        "neverd_swift_witness_cache_" + llvm::utohexstr(CacheAddress, true);
    const std::string CacheName = CacheStem + "_address";
    const std::string AccessorName = "neverd_swift_witness_accessor_" +
                                     llvm::utohexstr(AccessorAddress, true);
    const std::string ConformanceName = CacheStem + "_conformance";
    const std::string MetadataName = CacheStem + "_metadata";
    SharedFunctions.insert(CacheName);
    SharedFunctions.insert(AccessorName);
    Source += "\nextern unsigned char " + ConformanceName + "[] __asm__(\"_" +
              Globals[0] +
              "\");\n"
              "extern unsigned char " +
              MetadataName + "[] __asm__(\"_" + Globals[1] +
              "\");\n"
              "static void *" +
              CacheStem +
              ";\n"
              "uintptr_t " +
              CacheName +
              "(void) {\n"
              "  return (uintptr_t)&" +
              CacheStem +
              ";\n}\n"
              "uintptr_t " +
              AccessorName +
              "(void) {\n"
              "  void *value = " +
              CacheStem +
              ";\n"
              "  if (value)\n"
              "    return (uintptr_t)value;\n"
              "  value = neverd_swift_get_witness_table(\n"
              "      " +
              ConformanceName +
              ",\n"
              "      " +
              MetadataName +
              ", (void *)0);\n"
              "  __atomic_store_n(&" +
              CacheStem +
              ", value, __ATOMIC_RELEASE);\n"
              "  return (uintptr_t)value;\n}\n";
  }
  return Source;
}

inline std::string
renderObjCLocalStorageHelpers(const BinaryImage &Image,
                              const std::map<va_t, uint64_t> &Storage,
                              std::set<std::string> &SharedFunctions) {
  std::string Source;
  for (const auto &[Address, Width] : Storage) {
    auto Hint = objc_binding_detail::localStorageHint(Image, Address, Width);
    if (!Hint && Width == 8)
      Hint = objc_binding_detail::oncePredicateStorageHint(Image, Address);
    if (!Hint)
      throw std::runtime_error("local-storage initializer is no longer valid");
    const auto *Bytes = Image.readVA(Address, Width);
    if (!Bytes)
      continue;
    const std::string Name =
        "neverd_local_storage_" + llvm::utohexstr(Address, true) + "_address";
    SharedFunctions.insert(Name);
    if (const auto Target = objc_binding_detail::localStringPointerInitializer(
            Image, Address, Width)) {
      const auto ObjectName = "neverd_objc_constant_string_" +
                              llvm::utohexstr(*Target, true) + "_address";
      // The shared cell is initialized before its address is exposed. After
      // that, ordinary source loads/stores observe the same mutable storage;
      // even a later null store must never trigger initialization again.
      Source +=
          "\nuintptr_t " + Name +
          "(void) {\n"
          "  extern uintptr_t " +
          ObjectName +
          "(void);\n"
          "  static _Alignas(16) uintptr_t storage;\n"
          "  static unsigned state;\n"
          "  if (__atomic_load_n(&state, __ATOMIC_ACQUIRE) != 2) {\n"
          "    unsigned expected = 0;\n"
          "    if (__atomic_compare_exchange_n(&state, &expected, 1, 0, "
          "__ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {\n"
          "      storage = " +
          ObjectName +
          "();\n"
          "      __atomic_store_n(&state, 2, __ATOMIC_RELEASE);\n"
          "    } else {\n"
          "      while (__atomic_load_n(&state, __ATOMIC_ACQUIRE) != 2) {}\n"
          "    }\n  }\n  return (uintptr_t)&storage;\n}\n";
      continue;
    }
    Source += "\nuintptr_t " + Name +
              "(void) {\n"
              "  static _Alignas(16) unsigned char storage[" +
              std::to_string(Width) + "] = { ";
    bool Any = false;
    for (uint64_t I = 0; I < Width; ++I) {
      if (!Bytes[I])
        continue;
      if (Any)
        Source += ", ";
      Source += "[" + std::to_string(I) + "] = " + std::to_string(Bytes[I]);
      Any = true;
    }
    if (!Any)
      Source += "0";
    Source += " };\n  return (uintptr_t)storage;\n}\n";
  }
  return Source;
}

inline std::string
renderObjCSwiftSmallStringHelpers(const BinaryImage &Image,
                                  const std::set<va_t> &Storage,
                                  std::set<std::string> &SharedFunctions) {
  std::string Source;
  for (const auto Address : Storage) {
    if (!objc_binding_detail::swiftSmallStringStorageHint(Image, Address))
      throw std::runtime_error("Swift small-string storage is no longer valid");
    const auto *Bytes = Image.readVA(Address, 16);
    if (!Bytes)
      throw std::runtime_error("Swift small-string bytes are unavailable");
    const std::string Name = "neverd_swift_small_string_" +
                             llvm::utohexstr(Address, true) + "_address";
    SharedFunctions.insert(Name);
    Source += "\nuintptr_t " + Name +
              "(void) {\n  static const _Alignas(16) unsigned char "
              "storage[16] = { ";
    bool Any = false;
    for (unsigned I = 0; I < 16; ++I) {
      if (!Bytes[I])
        continue;
      if (Any)
        Source += ", ";
      Source += "[" + std::to_string(I) + "] = " + std::to_string(Bytes[I]);
      Any = true;
    }
    if (!Any)
      Source += "0";
    Source += " };\n  return (uintptr_t)storage;\n}\n";
  }
  return Source;
}

} // namespace neverd::sdk
#endif
