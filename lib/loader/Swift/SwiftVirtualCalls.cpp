#include "neverd/loader/Swift/SwiftVirtualCalls.h"

#include "../MachO/DarwinRuntimeImport.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/AArch64Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ObjC/ObjCSourceDeclarations.h"

#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <utility>

namespace neverd {
namespace {
bool overlaps(const NdVar &Left, const NdVar &Right) {
  if (Left.Space != Right.Space || !Left.Size || !Right.Size)
    return false;
  return Left.Offset <= Right.Offset ? Right.Offset - Left.Offset < Left.Size
                                     : Left.Offset - Right.Offset < Right.Size;
}

struct Definition {
  enum class Kind { Constant, Entry, Operation } TheKind = Kind::Constant;
  NdVar Value;
  const LowOp *Op = nullptr;
  size_t Index = 0;
};

class BlockTrace {
  const BinaryImage &Image;
  const LowFunc &Function;
  const LowBlock &Block;
  size_t Budget = 4096;

public:
  BlockTrace(const BinaryImage &Image, const LowFunc &Function,
             const LowBlock &Block)
      : Image(Image), Function(Function), Block(Block) {}

  std::optional<Definition> resolve(const NdVar &Value, size_t Before,
                                    va_t UseAddress, unsigned Depth = 0) {
    if (!Budget-- || Depth > 48 || Value.Size != 8 || Before > Block.Ops.size())
      return std::nullopt;
    if (Value.isConst())
      return Definition{Definition::Kind::Constant, Value};
    if (!Value.isReg() && !Value.isTemp())
      return std::nullopt;
    for (size_t Index = Before; Index-- > 0;) {
      const LowOp &Op = Block.Ops[Index];
      if (Value.isTemp() && Op.Addr != UseAddress)
        return std::nullopt;
      if ((Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
          Value.isReg() &&
          !getTargetRegInfo(Image.Arch)
               .isCallPreserved(Value.Offset, Value.Size))
        return std::nullopt;
      if (!overlaps(Op.Output, Value))
        continue;
      if (Op.Output != Value || Op.MemoryOrdering != NdMemoryOrdering::None ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        return std::nullopt;
      if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1)
        return resolve(Op.Inputs[0], Index, Op.Addr, Depth + 1);
      return Definition{Definition::Kind::Operation, Value, &Op, Index};
    }
    if (Value.isReg() && Block.StartAddr == Function.Entry &&
        Block.Preds.empty())
      return Definition{Definition::Kind::Entry, Value};
    return std::nullopt;
  }

  std::optional<Definition> input(const Definition &D, unsigned Index) {
    if (D.TheKind != Definition::Kind::Operation || Index >= D.Op->NumInputs)
      return std::nullopt;
    return resolve(D.Op->Inputs[Index], D.Index, D.Op->Addr);
  }

  std::optional<Definition> loadAddress(const Definition &D) {
    if (D.TheKind != Definition::Kind::Operation ||
        D.Op->Opcode != NdOp::LOAD ||
        D.Op->MemoryOrdering != NdMemoryOrdering::None ||
        D.Op->MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return std::nullopt;
    const auto Memory = lowMemoryOperands(*D.Op);
    if (!Memory.Complete || !Memory.Address || Memory.AccessSize != 8)
      return std::nullopt;
    return resolve(*Memory.Address, D.Index, D.Op->Addr);
  }

  std::optional<std::pair<Definition, uint64_t>>
  positiveAdd(const Definition &D) {
    if (D.TheKind != Definition::Kind::Operation ||
        D.Op->Opcode != NdOp::INT_ADD || D.Op->NumInputs != 2)
      return std::nullopt;
    auto A = input(D, 0), B = input(D, 1);
    if (!A || !B)
      return std::nullopt;
    if (A->TheKind == Definition::Kind::Constant &&
        A->Value.Provenance == ConstantAddressProvenance::Scalar)
      return std::pair{*B, A->Value.Offset};
    if (B->TheKind == Definition::Kind::Constant &&
        B->Value.Provenance == ConstantAddressProvenance::Scalar)
      return std::pair{*A, B->Value.Offset};
    return std::nullopt;
  }

  bool same(const Definition &A, const Definition &B) const {
    if (A.TheKind != B.TheKind)
      return false;
    if (A.TheKind == Definition::Kind::Operation)
      return A.Op == B.Op;
    return A.Value == B.Value;
  }

  std::optional<va_t> literalDataAddress(const Definition &D) {
    if (D.TheKind == Definition::Kind::Constant &&
        isExactAddressProvenance(D.Value.Provenance) &&
        Image.isDataAddress(D.Value.Offset))
      return D.Value.Offset;
    const auto Add = positiveAdd(D);
    if (!Add || Add->first.TheKind != Definition::Kind::Constant ||
        Add->first.Value.Provenance !=
            ConstantAddressProvenance::AddressFragment ||
        (Add->first.Value.Offset & 0xfff) || Add->second > 0xfff ||
        Add->first.Value.Offset > UINT64_MAX - Add->second)
      return std::nullopt;
    const va_t Address = Add->first.Value.Offset + Add->second;
    return Image.isDataAddress(Address) ? std::optional<va_t>(Address)
                                        : std::nullopt;
  }

  bool ivarReceiver(const Definition &Context, llvm::StringRef ClassName) {
    auto Address = loadAddress(Context);
    if (!Address || Address->TheKind != Definition::Kind::Operation ||
        Address->Op->Opcode != NdOp::INT_ADD || Address->Op->NumInputs != 2)
      return false;
    for (unsigned I = 0; I < 2; ++I) {
      auto Self = input(*Address, I);
      auto Offset = input(*Address, 1 - I);
      if (!Self || !Offset || Self->TheKind != Definition::Kind::Entry ||
          Self->Value != NdVar::reg(a64reg::X0, 8))
        continue;
      auto Slot = loadAddress(*Offset);
      if (!Slot)
        continue;
      const auto Address = literalDataAddress(*Slot);
      if (!Address)
        continue;
      const auto Found = Image.ObjCSourceReferences.find(*Address);
      if (Found != Image.ObjCSourceReferences.end() &&
          Found->second.TheKind == ObjCSourceReference::Kind::IvarOffset &&
          Found->second.ClassName == ClassName)
        return true;
    }
    return false;
  }

  std::optional<va_t> maskImport(const Definition &D) {
    auto GlobalAddress = loadAddress(D);
    if (!GlobalAddress)
      return std::nullopt;
    auto Slot = loadAddress(*GlobalAddress);
    if (!Slot)
      return std::nullopt;
    const auto ImportSlot = literalDataAddress(*Slot);
    if (!ImportSlot)
      return std::nullopt;
    const auto Import = darwinRuntimeImport(Image, *ImportSlot);
    const auto Bind = Image.DyldBindSlots.find(*ImportSlot);
    return Import && *Import == "_swift_isaMask" &&
                   Bind != Image.DyldBindSlots.end() &&
                   darwinExportModuleMatches(
                       "/usr/lib/swift/libswiftCore.dylib", Bind->second.Module)
               ? ImportSlot
               : std::nullopt;
  }

  std::optional<std::pair<va_t, uint32_t>>
  virtualTarget(const Definition &Target, const Definition &Context,
                llvm::StringRef ClassName) {
    auto Address = loadAddress(Target);
    if (!Address)
      return std::nullopt;
    auto BaseAndOffset = positiveAdd(*Address);
    if (!BaseAndOffset || BaseAndOffset->second < 0x40 ||
        BaseAndOffset->second > 0x1000 || BaseAndOffset->second % 8 ||
        BaseAndOffset->first.TheKind != Definition::Kind::Operation ||
        BaseAndOffset->first.Op->Opcode != NdOp::INT_AND ||
        BaseAndOffset->first.Op->NumInputs != 2 ||
        !ivarReceiver(Context, ClassName))
      return std::nullopt;
    const auto &Masked = BaseAndOffset->first;
    for (unsigned I = 0; I < 2; ++I) {
      auto Mask = input(Masked, I), Isa = input(Masked, 1 - I);
      if (!Mask || !Isa)
        continue;
      const auto Import = maskImport(*Mask);
      const auto IsaAddress = loadAddress(*Isa);
      if (Import && IsaAddress && same(*IsaAddress, Context))
        return std::pair{*Import, uint32_t(BaseAndOffset->second)};
    }
    return std::nullopt;
  }
};

std::optional<uint8_t> exactVoidMethodCallWindow(const BinaryImage &Image,
                                                 const LowBlock &Block,
                                                 size_t Index) {
  if (Index < 3)
    return std::nullopt;
  const bool ZeroArguments =
      Index >= 5 && Block.Ops[Index - 3].Opcode == NdOp::COPY &&
      Block.Ops[Index - 3].Output == NdVar::reg(a64reg::X0, 8) &&
      Block.Ops[Index - 3].NumInputs == 1 &&
      Block.Ops[Index - 3].Inputs[0].isConst() &&
      Block.Ops[Index - 3].Inputs[0].Offset == 0 &&
      Block.Ops[Index - 3].Inputs[0].Size == 8 &&
      Block.Ops[Index - 2].Opcode == NdOp::COPY &&
      Block.Ops[Index - 2].Output == NdVar::reg(a64reg::X1, 8) &&
      Block.Ops[Index - 2].NumInputs == 1 &&
      Block.Ops[Index - 2].Inputs[0].isConst() &&
      Block.Ops[Index - 2].Inputs[0].Offset == 0 &&
      Block.Ops[Index - 2].Inputs[0].Size == 8;
  const auto &Retain = Block.Ops[Index - (ZeroArguments ? 5 : 3)];
  const auto &Saved = Block.Ops[Index - (ZeroArguments ? 4 : 2)];
  const auto &Link = Block.Ops[Index - 1];
  if (Retain.Opcode != NdOp::CALL ||
      Retain.Output != NdVar::reg(a64reg::X0, 8) || Retain.NumInputs != 1 ||
      !Retain.Inputs[0].isConst() || Saved.Opcode != NdOp::COPY ||
      Saved.Output != NdVar::reg(a64reg::X19, 8) || Saved.NumInputs != 1 ||
      Saved.Inputs[0] != NdVar::reg(a64reg::X0, 8) ||
      Link.Opcode != NdOp::COPY || Link.Output != NdVar::reg(a64reg::X30, 8) ||
      Link.NumInputs != 1 || !Link.Inputs[0].isConst() ||
      Link.Inputs[0].Offset != Block.Ops[Index].Addr + 4)
    return std::nullopt;
  const auto *Import = Image.findImportStubAt(Retain.Inputs[0].Offset);
  return Import && Import->Name == "_objc_retain" &&
                 darwinExportModuleMatches("/usr/lib/libobjc.A.dylib",
                                           Import->Module)
             ? std::optional<uint8_t>(ZeroArguments ? 2 : 0)
             : std::nullopt;
}

std::optional<SourceCallTypeHint> canonicalAccessor(const BinaryImage &Image,
                                                    va_t Entry, va_t CallSite,
                                                    va_t IsaMaskImport,
                                                    uint32_t Slot,
                                                    uint8_t ZeroArgumentWords) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 || !Entry ||
      !CallSite || !IsaMaskImport || Slot < 0x40 || Slot > 0x1000 || Slot % 8 ||
      (ZeroArgumentWords != 0 && ZeroArgumentWords != 2) ||
      !Image.isCodeAddress(Entry) || !Image.isCodeAddress(CallSite))
    return std::nullopt;
  const auto Import = darwinRuntimeImport(Image, IsaMaskImport);
  const auto Bind = Image.DyldBindSlots.find(IsaMaskImport);
  if (!Import || *Import != "_swift_isaMask" ||
      Bind == Image.DyldBindSlots.end() ||
      !darwinExportModuleMatches("/usr/lib/swift/libswiftCore.dylib",
                                 Bind->second.Module))
    return std::nullopt;
  const ObjCMethod *Method = nullptr;
  for (const auto &M : Image.ObjCMethods)
    if (M.Implementation == Entry) {
      if (Method)
        return std::nullopt;
      Method = &M;
    }
  if (!Method || Method->IsClassMethod || Method->Selector.empty() ||
      !Method->TypeHint)
    return std::nullopt;
  const bool DoubleGetter = Method->TypeEncoding == "d16@0:8";
  const bool BoolGetter = Method->TypeEncoding == "B16@0:8";
  const bool DoubleSetter = Method->TypeEncoding == "v24@0:8d16";
  const bool BoolSetter = Method->TypeEncoding == "v20@0:8B16";
  const bool VoidMethod = Method->TypeEncoding == "v16@0:8" &&
                          Method->Selector.find(':') == std::string::npos;
  const bool Setter = DoubleSetter || BoolSetter;
  const bool Floating = DoubleGetter || DoubleSetter;
  if (ZeroArgumentWords && !VoidMethod)
    return std::nullopt;
  if (!DoubleGetter && !BoolGetter && !DoubleSetter && !BoolSetter &&
      !VoidMethod)
    return std::nullopt;
  const auto Bytes = Image.readVA(CallSite, 4);
  constexpr std::array<uint8_t, 4> BlrX21 = {0xa0, 0x02, 0x3f, 0xd6};
  constexpr std::array<uint8_t, 4> BlrX22 = {0xc0, 0x02, 0x3f, 0xd6};
  const auto &ExpectedCall = BoolSetter ? BlrX22 : BlrX21;
  if (!Bytes || !std::equal(ExpectedCall.begin(), ExpectedCall.end(), Bytes))
    return std::nullopt;
  const auto EntryType = objcMethodSourceTypeHint(Image, Entry);
  if (!EntryType || !EntryType->ReturnType ||
      EntryType->Parameters.size() != (Setter ? 3U : 2U))
    return std::nullopt;
  const TypeRef &ValueType =
      Setter ? EntryType->Parameters.back().Type : EntryType->ReturnType;
  if ((VoidMethod || Setter) && EntryType->ReturnType->Kind != NdTypeKind::Void)
    return std::nullopt;
  if (!VoidMethod &&
      (!ValueType ||
       (Floating
            ? (ValueType->Kind != NdTypeKind::Float || ValueType->Size != 8)
            : (ValueType->Kind != NdTypeKind::Int || ValueType->Size != 1 ||
               ValueType->IsSigned))))
    return std::nullopt;
  const Symbol *Symbol = nullptr;
  for (const auto &S : Image.Symbols)
    if (S.Addr == Entry && S.IsFunc) {
      if (Symbol)
        return std::nullopt;
      Symbol = &S;
    }
  if (!Symbol || !llvm::StringRef(Symbol->Name).starts_with("_$s"))
    return std::nullopt;
  const llvm::StringRef Name = Symbol->Name;
  const bool MatchesSymbol =
      VoidMethod
          ? Name.ends_with(std::to_string(Method->Selector.size()) +
                           Method->Selector + "yyFTo")
          : (Floating
                 ? (Name.ends_with(Setter ? "12CoreGraphics7CGFloatVvsTo"
                                          : "12CoreGraphics7CGFloatVvgTo") ||
                    Name.ends_with(Setter ? "SdvsTo" : "SdvgTo"))
                 : Name.ends_with(Setter ? "SbvsTo" : "SbvgTo"));
  if (!MatchesSymbol)
    return std::nullopt;
  SourceCallTypeHint Hint;
  Hint.CallKind = SourceCallTypeHint::Kind::SwiftVirtual;
  Hint.TargetName = "swift_virtual";
  Hint.Virtual = SourceCallTypeHint::SwiftVirtualEvidence{
      Entry, CallSite, IsaMaskImport, Slot, ZeroArgumentWords};
  Hint.Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Hint.Signature.ReturnType =
      (Setter || VoidMethod)
          ? NdType::makeVoid()
          : (Floating ? NdType::makeFloat(8) : NdType::makeInt(1, false));
  if (Setter)
    Hint.Signature.Parameters.push_back(
        {"value", Floating ? NdType::makeFloat(8) : NdType::makeInt(1, false)});
  for (uint8_t I = 0; I < ZeroArgumentWords; ++I)
    Hint.Signature.Parameters.push_back({"zero", NdType::makeInt(8, false)});
  Hint.Signature.Parameters.push_back(
      {"self", NdType::makePtr(NdType::makeVoid())});
  Hint.Signature.Parameters.back().TheRole =
      SourceParameterTypeHint::Role::SwiftContext;
  std::string Diagnostic;
  return assignDarwinSwiftSourceABI(Hint.Signature, Image.Arch, Diagnostic)
             ? std::optional<SourceCallTypeHint>(std::move(Hint))
             : std::nullopt;
}
} // namespace

bool isSwiftVirtualSourceCallHint(const BinaryImage &Image,
                                  const SourceCallTypeHint &Hint) {
  if (Hint.CallKind != SourceCallTypeHint::Kind::SwiftVirtual ||
      !Hint.Virtual || Hint.TargetAddress ||
      Hint.TargetName != "swift_virtual" || Hint.ValueWitness ||
      Hint.BooleanResult || Hint.DoesNotReturn || Hint.WeakImport ||
      Hint.ReturnedArgument || Hint.RuntimeObjCResultType || Hint.Receiver ||
      Hint.Format || Hint.NilTerminated || Hint.SwiftTypeMetadata ||
      !Hint.Selector.empty() || !Hint.OwnerClass.empty() ||
      Hint.SelectorReferenceAddress || !Hint.BorrowedByteInputs.empty() ||
      !Hint.SwiftStringInputs.empty() || Hint.SelectorResultUse ||
      Hint.SelectorResultTypeUse || Hint.SelectorArgumentTypeUse ||
      Hint.SelectorForwardingUse || Hint.SelectorArgumentStorageUse ||
      Hint.ObjCIndirectResultStorage || Hint.ByteCount ||
      Hint.ImmutablePointerSlot)
    return false;
  const auto Expected = canonicalAccessor(
      Image, Hint.Virtual->MethodEntry, Hint.Virtual->CallSite,
      Hint.Virtual->IsaMaskImport, Hint.Virtual->VtableByteOffset,
      Hint.Virtual->ZeroArgumentWords);
  return Expected && Expected->Virtual == Hint.Virtual &&
         equalSourceABIs(Expected->Signature, Hint.Signature);
}

std::map<va_t, SourceCallTypeHint>
buildSwiftVirtualCallHints(const BinaryImage &Image, const LowFunc &Function) {
  std::map<va_t, SourceCallTypeHint> Result;
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
      Function.Blocks.size() != 1 || Function.Blocks[0].Ops.size() > 2048)
    return Result;
  const auto &Block = Function.Blocks[0];
  if (Block.StartAddr != Function.Entry || !Block.Preds.empty())
    return Result;
  const ObjCMethod *Method = nullptr;
  for (const auto &Candidate : Image.ObjCMethods)
    if (Candidate.Implementation == Function.Entry) {
      if (Method)
        return Result;
      Method = &Candidate;
    }
  if (!Method)
    return Result;
  size_t CallsAtSite = 0;
  for (size_t I = 0; I < Block.Ops.size(); ++I) {
    const auto &Op = Block.Ops[I];
    if (Op.Opcode != NdOp::INDIR_CALL || Op.NumInputs != 1 ||
        Op.Inputs[0] != NdVar::reg(Method->TypeEncoding == "v20@0:8B16"
                                       ? a64reg::X22
                                       : a64reg::X21,
                                   8) ||
        Op.MemoryOrdering != NdMemoryOrdering::None ||
        Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      continue;
    CallsAtSite = 0;
    for (const auto &Other : Block.Ops)
      if ((Other.Opcode == NdOp::CALL || Other.Opcode == NdOp::INDIR_CALL) &&
          Other.Addr == Op.Addr)
        ++CallsAtSite;
    if (CallsAtSite != 1)
      continue;
    uint8_t ZeroArgumentWords = 0;
    if (Method->TypeEncoding == "v16@0:8") {
      const auto Window = exactVoidMethodCallWindow(Image, Block, I);
      if (!Window)
        continue;
      ZeroArgumentWords = *Window;
    }
    BlockTrace Trace(Image, Function, Block);
    auto Target = Trace.resolve(Op.Inputs[0], I, Op.Addr);
    auto Context = Trace.resolve(NdVar::reg(a64reg::X20, 8), I, Op.Addr);
    if (!Target || !Context)
      continue;
    const auto Virtual =
        Trace.virtualTarget(*Target, *Context, Method->ClassName);
    if (!Virtual)
      continue;
    auto Hint =
        canonicalAccessor(Image, Function.Entry, Op.Addr, Virtual->first,
                          Virtual->second, ZeroArgumentWords);
    if (Hint)
      Result.emplace(Op.Addr, std::move(*Hint));
  }
  return Result;
}
} // namespace neverd
