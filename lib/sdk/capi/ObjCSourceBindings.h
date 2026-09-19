#ifndef NEVERD_SDK_CAPI_OBJCSOURCEBINDINGS_H
#define NEVERD_SDK_CAPI_OBJCSOURCEBINDINGS_H

#include "../../ir/high/pass/HighFrameAddress.h"
#include "../../loader/ObjC/ObjCRuntimeData.h"
#include "BorrowedByteSources.h"
#include "CStringStorageSources.h"
#include "ObjCConstantObjectSources.h"
#include "ObjCProfileStorage.h"
#include "ObjCReadOnlyScalarSources.h"
#include "ObjCSourceProjection.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/DarwinRuntimeCalls.h"
#include "neverd/loader/ObjC/ObjCCallHints.h"
#include "neverd/loader/ObjC/ObjCFormattedCalls.h"
#include "neverd/loader/ObjC/ObjCSourceDeclarations.h"
#include "neverd/loader/Swift/SwiftMetadata.h"
#include "neverd/loader/Swift/SwiftRuntimeCalls.h"
#include "neverd/loader/Swift/SwiftStringCalls.h"

#include "llvm/ADT/StringExtras.h"

#include <optional>

namespace neverd::sdk {

struct ObjCSourceBindingResult {
  HighFunc Function;
  std::string Limitation;
  std::set<va_t> Dependencies;
  std::set<std::string> InstanceLayoutClasses;
  std::set<std::string> RuntimeProtocols;
  std::set<va_t> AssociationKeys;
  std::set<va_t> KVOContexts;
  std::set<va_t> StaticIdentities;
  std::map<va_t, uint64_t> LocalStorageExtents;
  std::set<va_t> ProfileCounterSections;
  std::set<va_t> ConstantStrings;
  std::set<va_t> ConstantObjects;
  std::map<va_t, uint32_t> ConstantObjectTables;
  std::set<BorrowedByteRange> BorrowedBytes;
  std::set<va_t> CStringSections, CStringPointerSlots;
  SourceProjectionDiagnostics Diagnostics{};
};

namespace objc_binding_detail {

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
         !Binding.SelectorArgumentTypeUse &&
         !Binding.SelectorArgumentStorageUse && !Binding.ByteCount &&
         bool(Binding.Format) == bool(Expected.Format) &&
         (!Binding.Format ||
          (Binding.Format->FixedCount == Expected.Format->FixedCount &&
           Binding.Format->Syntax == Expected.Format->Syntax &&
           Binding.Format->FormatParameter ==
               Expected.Format->FormatParameter &&
           Binding.Format->FormatAddress == Expected.Format->FormatAddress)) &&
         Binding.BorrowedByteInputs == Expected.BorrowedByteInputs &&
         Binding.SwiftStringInputs == Expected.SwiftStringInputs &&
         objc_projection_detail::sameHint(Binding.Signature,
                                          Expected.Signature);
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

inline std::optional<SourceCallTypeHint>
associationKeyHint(const BinaryImage &Image, va_t Address) {
  if (Image.Format != BinaryFormat::MachO || Image.Bits != Bitness::Bits64 ||
      !Address)
    return std::nullopt;
  const auto *Section = Image.getSectionFor(Address);
  const auto *Segment = Image.getSegmentFor(Address);
  if (!Section || !Segment || !Section->isReadable() ||
      !Segment->isReadable() ||
      ((Section->isWritable() || Segment->isWritable()) &&
       !Segment->ReadOnlyAfterRelocations) ||
      Section->isExecutable() || Segment->isExecutable() ||
      !Image.readVA(Address, 1))
    return std::nullopt;
  const bool CString = (Section->Type & llvm::MachO::SECTION_TYPE) ==
                       llvm::MachO::S_CSTRING_LITERALS;
  const Symbol *Identity = nullptr;
  if (CString) {
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
staticIdentityHint(const BinaryImage &Image, va_t Address) {
  const auto *Symbol = uniqueWritableDataSymbol(Image, Address, 8);
  const auto Value =
      Symbol ? objc::RuntimeData(Image).localPointer(Address) : std::nullopt;
  if (!Symbol || !Value || *Value != Address ||
      !Image.MachOResolvedChainedPointerSlots.count(Address))
    return std::nullopt;
  SourceCallTypeHint Hint;
  Hint.CallKind = SourceCallTypeHint::Kind::RuntimeStaticIdentity;
  Hint.TargetAddress = Address;
  Hint.TargetName = Symbol->Name;
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

// Darwin dispatch_once_t (also used by swift_once) is an intptr_t, initialized
// to zero in static storage. A completed token cannot be transplanted without
// its initialized state. Keep token storage shared through the normal helpers.
inline std::optional<SourceCallTypeHint>
oncePredicateStorageHint(const BinaryImage &Image, va_t Address) {
  if (Image.Format != BinaryFormat::MachO || Image.Bits != Bitness::Bits64 ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64) || Address % 8)
    return std::nullopt;
  auto Hint = localStorageHint(Image, Address, 8);
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

inline std::optional<uint64_t> constantAddress(const HighExpr &Expression,
                                               unsigned Depth = 0);

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
      Hint.Selector != Selector || Hint.Format || Hint.SelectorResultUse ||
      Hint.SelectorArgumentTypeUse || Hint.SelectorArgumentStorageUse ||
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

/// A profile-counter address may cross a native source-call boundary only
/// when the complete typed callee proves that this parameter is used solely
/// as the exact address of bounded numeric loads/stores.  The source ABI's
/// pointer type alone says nothing about pointee width or escape behavior.
inline std::optional<va_t>
nativeProfileCounterArgument(const HighFunc &Function, size_t Parameter,
                             va_t Address, const ObjCProfileStorage &Storage) {
  if (!Function.SourceTypeHint ||
      Function.Params.size() != Function.SourceTypeHint->Parameters.size() ||
      Parameter >= Function.Params.size() || !Function.Params[Parameter].Type ||
      Function.Params[Parameter].Type->Kind != NdTypeKind::Ptr ||
      !Function.SourceTypeHint->Parameters[Parameter].Type ||
      Function.SourceTypeHint->Parameters[Parameter].Type->Kind !=
          NdTypeKind::Ptr ||
      !equalSourceTypes(Function.Params[Parameter].Type,
                        Function.SourceTypeHint->Parameters[Parameter].Type))
    return std::nullopt;
  for (size_t I = 0; I < Function.Params.size(); ++I)
    if (Function.Params[I].Name !=
            Function.SourceTypeHint->Parameters[I].Name ||
        !equalSourceTypes(Function.Params[I].Type,
                          Function.SourceTypeHint->Parameters[I].Type))
      return std::nullopt;

  size_t Budget = 100000;
  bool Valid = true, Used = false;
  std::optional<va_t> Base;
  auto Contains = [&](const auto &Self, const ExprPtr &Expression,
                      unsigned Depth) -> bool {
    if (!Expression || !Budget || Depth > 200)
      return false;
    --Budget;
    if (Expression->Kind == ExprKind::Var &&
        Expression->Var.Kind == MedVar::Param && Expression->Var.Id >= 0 &&
        static_cast<size_t>(Expression->Var.Id) == Parameter)
      return true;
    for (const auto &Operand : Expression->Operands)
      if (Self(Self, Operand, Depth + 1))
        return true;
    return false;
  };
  auto Access = [&](const ExprPtr &Pointer, const TypeRef &Type,
                    NdMemoryOrdering Ordering,
                    NdMemoryAddressSpace AddressSpace) {
    if (!Valid || !Contains(Contains, Pointer, 0))
      return;
    if (!exactParameterValue(Pointer, Parameter) || !Type ||
        Type->Kind == NdTypeKind::Ptr ||
        !localStorageAccessTypeSupported(Type) ||
        Ordering != NdMemoryOrdering::None ||
        AddressSpace != NdMemoryAddressSpace::Default) {
      Valid = false;
      return;
    }
    const auto Section = Storage.sectionFor(Address, Type->Size);
    if (!Section || (Base && *Base != *Section)) {
      Valid = false;
      return;
    }
    Base = *Section;
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
             Expression->MemoryOrdering, Expression->MemoryAddressSpace);
      if (Contains(Contains, Expression->Operands[0], 0))
        return;
    }
    if (Expression->Kind == ExprKind::Store &&
        Expression->Operands.size() == 2 && Expression->Operands[1]) {
      Access(Expression->Operands[0], Expression->Operands[1]->Type,
             Expression->MemoryOrdering, Expression->MemoryAddressSpace);
      if (Contains(Contains, Expression->Operands[0], 0)) {
        Scan(Expression->Operands[1], Depth + 1);
        return;
      }
    }
    if (Expression->Kind == ExprKind::Var &&
        Expression->Var.Kind == MedVar::Param && Expression->Var.Id >= 0 &&
        static_cast<size_t>(Expression->Var.Id) == Parameter) {
      Valid = false;
      return;
    }
    for (const auto &Operand : Expression->Operands)
      Scan(Operand, Depth + 1);
  };
  walkStmts(Function.Body, [&](const HighStmt &Statement) {
    const bool DirectStore = Statement.Kind == StmtKind::Store &&
                             Statement.StoreVal &&
                             Contains(Contains, Statement.StoreAddr, 0);
    if (DirectStore)
      Access(Statement.StoreAddr, Statement.StoreVal->Type,
             Statement.MemoryOrdering, Statement.MemoryAddressSpace);
    forEachExpr(Statement, [&](const ExprPtr &Expression) {
      if (DirectStore && Expression == Statement.StoreAddr)
        return;
      Scan(Expression, 0);
    });
  });
  return Valid && Used && Budget ? Base : std::nullopt;
}

struct ClassObjectIdentity {
  SourceCallTypeHint::Kind Kind;
  std::string Name;
};

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
  const auto ClassObjects = classObjectIdentities(Image);
  const auto ScalarLoads = readOnlyScalarLoadPlans(Function, Image);
  const auto ObjectPointerLoads =
      readOnlyObjectPointerLoadPlans(Function, Image);
  const auto ObjectPointerConsumers =
      readOnlyObjectPointerLoadConsumers(Function, ObjectPointerLoads);
  const auto DirectLocalStorage =
      directLocalStorageAccessExtents(Function, Image);
  const auto KVOCallbackParameter =
      kvoCallbackContextParameter(Function, Image);
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
    if (!Operand || !Type || Ordering != NdMemoryOrdering::None ||
        AddressSpace != NdMemoryAddressSpace::Default ||
        !localStorageAccessTypeSupported(Type))
      return false;
    const auto Address = constantAddress(*Operand);
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
    if ((AddressContext || ObjectAddress) && !MemoryAddress &&
        !NumericOperand &&
        !(Original->Kind == ExprKind::Const &&
          Original->ConstProvenance == ConstantAddressProvenance::Scalar)) {
      const auto Address = constantAddress(*Original);
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
    if (Expression->Kind == ExprKind::Call && Expression->SourceCallHint &&
        Expression->SourceCallHint->CallKind ==
            SourceCallTypeHint::Kind::Native)
      Result.Dependencies.insert(Expression->SourceCallHint->TargetAddress);
    for (size_t Index = 0; Index < Expression->Operands.size(); ++Index) {
      auto &Operand = Expression->Operands[Index];
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
        auto Hint =
            Address ? associationKeyHint(Image, *Address) : std::nullopt;
        if (Hint) {
          auto Key = HighExpr::makeCall({}, 0, {});
          Key->Type = Operand->Type;
          Key->SourceCallHint =
              std::make_shared<SourceCallTypeHint>(std::move(*Hint));
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
        auto Hint = Address ? kvoContextHint(Image, *Address) : std::nullopt;
        if (Hint) {
          auto Context = HighExpr::makeCall({}, 0, {});
          Context->Type = Operand->Type;
          Context->SourceCallHint =
              std::make_shared<SourceCallTypeHint>(std::move(*Hint));
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
          if (const auto *Symbol = uniqueWritableDataSymbol(Image, *Address, 1))
            Width = swiftStaticScalarStorageWidth(Symbol->Name);
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
          const auto Object =
              Address ? ClassObjects.find(*Address) : ClassObjects.end();
          if (Object != ClassObjects.end()) {
            auto Binding = std::make_shared<SourceCallTypeHint>();
            Binding->CallKind = Object->second.Kind;
            Binding->TargetAddress = Object->first;
            Binding->TargetName = Object->second.Name;
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

inline bool
objcSourceCallBound(const HighExpr &Expression, const BinaryImage &Image,
                    const std::map<va_t, const HighFunc *> &Functions,
                    const ObjCProfileStorage *ProfileStorage = nullptr,
                    const std::set<const HighExpr *> *ReadOnlyHelpers = nullptr,
                    const HighFunc *ContainingFunction = nullptr) {
  using namespace objc_binding_detail;
  if (Expression.Kind != ExprKind::Call || !Expression.SourceCallHint ||
      Expression.IntrinsicId != Intrinsic::None ||
      Expression.MemoryOrdering != NdMemoryOrdering::None ||
      Expression.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  const auto &Binding = *Expression.SourceCallHint;
  const auto &Hint = Binding.Signature;
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
       Hint.Origin != SourceFunctionTypeHint::OriginKind::ObjCSDK))
    return false;
  if (Binding.SelectorArgumentTypeUse &&
      (Binding.CallKind != SourceCallTypeHint::Kind::ObjCMessage ||
       Binding.Receiver || Binding.Format || Binding.SelectorResultUse ||
       Binding.SelectorArgumentStorageUse ||
       Hint.Origin != SourceFunctionTypeHint::OriginKind::ObjCSDK))
    return false;
  if (Binding.SelectorArgumentStorageUse &&
      (Binding.CallKind != SourceCallTypeHint::Kind::ObjCMessage ||
       Binding.Receiver || Binding.Format || Binding.SelectorResultUse ||
       Binding.SelectorArgumentTypeUse ||
       Hint.Origin != SourceFunctionTypeHint::OriginKind::ObjCSDK))
    return false;
  if (Binding.Format &&
      Binding.CallKind != SourceCallTypeHint::Kind::ObjCMessage &&
      Binding.CallKind != SourceCallTypeHint::Kind::DarwinRuntimeCall)
    return false;
  if (Binding.ValueWitness &&
      Binding.CallKind != SourceCallTypeHint::Kind::SwiftValueWitness)
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
      Binding.CallKind != SourceCallTypeHint::Kind::DarwinRuntimeCall)
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
    const auto Expected = associationKeyHint(Image, Binding.TargetAddress);
    return Expected && Binding.TargetName == Expected->TargetName &&
           Binding.Selector.empty() && Binding.OwnerClass.empty() &&
           !Binding.SelectorReferenceAddress &&
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
    const auto Expected = staticIdentityHint(Image, Binding.TargetAddress);
    return Expected && Binding.TargetName == Expected->TargetName &&
           Binding.Selector.empty() && Binding.OwnerClass.empty() &&
           !Binding.SelectorReferenceAddress && !Binding.ByteCount &&
           objc_projection_detail::sameHint(Expected->Signature, Hint);
  }
  if (Binding.CallKind ==
      SourceCallTypeHint::Kind::RuntimeLocalStorageAddress) {
    const auto Expected =
        localStorageHint(Image, Binding.TargetAddress, Binding.ByteCount);
    return Expected && Binding.TargetName == Expected->TargetName &&
           Binding.Selector.empty() && Binding.OwnerClass.empty() &&
           !Binding.SelectorReferenceAddress &&
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
  if (Binding.Format) {
    const auto &Format = *Binding.Format;
    const bool Message =
        Binding.CallKind == SourceCallTypeHint::Kind::ObjCMessage;
    const auto Expected =
        Message ? objcFormattedSourceCallHint(Image, Binding.Selector,
                                              Format.FormatAddress)
                : darwinFormattedSourceCallHint(Image, Binding.TargetAddress,
                                                Format.FormatAddress);
    if (!Expected || !Expected->Format || Binding.DoesNotReturn ||
        Format.Syntax != Expected->Format->Syntax ||
        Format.FixedCount != Expected->Format->FixedCount ||
        Format.FormatParameter != Expected->Format->FormatParameter ||
        Format.FormatParameter >= Expression.Operands.size() ||
        !objc_projection_detail::sameHint(Hint, Expected->Signature) ||
        (!Message && !runtimeBindingMatches(Binding, *Expected)))
      return false;
    auto Argument = Expression.Operands[Format.FormatParameter];
    unsigned Depth = 0;
    while (Argument && Argument->Kind == ExprKind::Cast &&
           Argument->Operands.size() == 1 && Argument->Type &&
           Argument->Type->Size == 8 && Depth++ < 32)
      Argument = Argument->Operands.front();
    if (!Argument)
      return false;
    if (Argument->Kind == ExprKind::Call && Argument->SourceCallHint &&
        Argument->SourceCallHint->CallKind ==
            SourceCallTypeHint::Kind::RuntimeConstantString)
      return Argument->Operands.empty() &&
             Argument->SourceCallHint->TargetAddress == Format.FormatAddress &&
             objcSourceCallBound(*Argument, Image, Functions);
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
        return *Base == Format.FormatAddress;
      if (Argument->Kind == ExprKind::BinOp && Argument->Op == NdOp::INT_ADD &&
          Argument->Operands.size() == 2) {
        for (unsigned I = 0; I < 2; ++I) {
          const auto Base = CStringBase(Argument->Operands[I]);
          const auto Offset = constantAddress(*Argument->Operands[1 - I]);
          if (Base && Offset && *Base <= InvalidVA - *Offset)
            return *Base + *Offset == Format.FormatAddress;
        }
      }
    }
    return constantAddress(*Argument) == Format.FormatAddress;
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
    const auto Expected =
        Binding.CallKind == SourceCallTypeHint::Kind::ObjCSuper2
            ? objcSuperSourceTypeHint(Image, Binding.Selector,
                                      *Binding.Receiver)
            : objcReceiverSourceTypeHint(Image, Binding.Selector,
                                         *Binding.Receiver);
    if (!Expected.HasDeclaration || !Expected.Signature ||
        !objc_projection_detail::sameHint(Hint, *Expected.Signature))
      return false;
  } else if (Hint.Origin == SourceFunctionTypeHint::OriginKind::ObjCSDK) {
    const auto Expected =
        Binding.SelectorResultUse
            ? objcSelectorSourceTypeHintForResultUse(Image, Binding.Selector,
                                                     *Binding.SelectorResultUse)
        : Binding.SelectorArgumentTypeUse
            ? objcSelectorSourceTypeHintForArgumentTypeUse(
                  Image, Binding.Selector, *Binding.SelectorArgumentTypeUse)
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
              "  static unsigned char identity;\n"
              "  return (uintptr_t)&identity;\n}\n";
  }
  return Source;
}

inline std::string
renderObjCLocalStorageHelpers(const BinaryImage &Image,
                              const std::map<va_t, uint64_t> &Storage,
                              std::set<std::string> &SharedFunctions) {
  std::string Source;
  for (const auto &[Address, Width] : Storage) {
    if (!objc_binding_detail::localStorageHint(Image, Address, Width))
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

} // namespace neverd::sdk
#endif
