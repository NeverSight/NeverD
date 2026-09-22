#include "neverd/loader/ObjC/ObjCCallHints.h"

#include "../MachO/DarwinRuntimeImport.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/AArch64Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/DarwinImportVeneer.h"
#include "neverd/loader/MachO/DarwinRuntimeCalls.h"
#include "neverd/loader/MachO/SourceRegisterCopy.h"
#include "neverd/loader/ObjC/ObjCBlocks.h"
#include "neverd/loader/ObjC/ObjCConstantStrings.h"
#include "neverd/loader/ObjC/ObjCFormattedCalls.h"
#include "neverd/loader/ObjC/ObjCSentinelCalls.h"
#include "neverd/loader/ObjC/ObjCSourceDeclarations.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/loader/Swift/SwiftRuntimeCalls.h"
#include "neverd/loader/Swift/SwiftStringCalls.h"
#include "neverd/object/SectionNames.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <array>
#include <deque>
#include <optional>
#include <tuple>

namespace neverd {
namespace {

std::string importAt(const BinaryImage &Image, va_t Slot) {
  // The one catalogued optional Darwin call must retain weak linkage. The
  // ordinary import-name path below intentionally rejects every weak import,
  // so recognize this exact contract before entering that strong-only path.
  if (const auto Weak = darwinRuntimeSourceCallHint(Image, Slot);
      Weak && Weak->WeakImport)
    return Weak->TargetName;
  const auto Import = darwinRuntimeImport(Image, Slot);
  if (!Import)
    return {};
  llvm::StringRef Name(*Import);
  Name.consume_front("_");
  if (Name == "objc_msgSend" || Name == "objc_msgSendSuper2" ||
      objcRuntimeSourceCallHint(Image, Slot) ||
      darwinRuntimeSourceCallHint(Image, Slot) ||
      darwinRuntimeFormatDeclaration(Image, Slot) ||
      swiftStringSourceCallHint(Image, Slot) ||
      swiftRuntimeSourceCallHint(Image, Slot))
    return Name.str();
  return {};
}

const uint8_t *code(const BinaryImage &Image, va_t Address, size_t Size) {
  const auto *Section = Image.getSectionFor(Address);
  const auto *Segment = Image.getSegmentFor(Address);
  if (!Section || !Segment || !Section->isExecutable() ||
      !Segment->isExecutable() || Address > InvalidVA - Size ||
      !rangeInBounds(Address - Section->VA, Size, Section->FileSz) ||
      !rangeInBounds(Address - Segment->VA, Size, Segment->FileSz))
    return nullptr;
  return Image.readVA(Address, Size);
}

std::optional<va_t> addSigned(va_t Base, int64_t Offset) {
  if ((Offset < 0 && Base < uint64_t(-Offset)) ||
      (Offset >= 0 && Base > InvalidVA - uint64_t(Offset)))
    return std::nullopt;
  return Offset < 0 ? Base - uint64_t(-Offset) : Base + uint64_t(Offset);
}

std::optional<va_t> adrp(uint32_t Instruction, va_t PC, unsigned Register) {
  if ((Instruction & 0x9f00001f) != (0x90000000u | Register))
    return std::nullopt;
  const uint32_t Imm =
      ((Instruction >> 29) & 3) | (((Instruction >> 5) & 0x7ffff) << 2);
  const int64_t Signed = (Imm & 0x100000) ? int64_t(Imm) - 0x200000 : Imm;
  return addSigned(PC & ~va_t(4095), Signed * 4096);
}

std::optional<va_t> ldrSlot(uint32_t Instruction, va_t Page,
                            unsigned Register) {
  if ((Instruction & 0xffc003ff) != (0xf9400000u | (Register << 5) | Register))
    return std::nullopt;
  return addSigned(Page, ((Instruction >> 10) & 4095) * 8);
}

struct Dispatch {
  std::string Name;
  std::string Selector;
  va_t SelectorSlot = 0;
  va_t ImportSlot = 0;
  bool LoadsSelector = false;
};

std::optional<Dispatch> veneerStorage(const BinaryImage &Image, va_t Address) {
  if (Image.Arch == Arch::X64) {
    const auto *Bytes = code(Image, Address, 6);
    if (!Bytes || Bytes[0] != 0xff || Bytes[1] != 0x25)
      return std::nullopt;
    auto Slot = addSigned(Address + 6,
                          int32_t(llvm::support::endian::read32le(Bytes + 2)));
    return Slot ? std::optional<Dispatch>({{}, {}, 0, *Slot}) : std::nullopt;
  }
  if (Image.Arch != Arch::AArch64)
    return std::nullopt;
  const auto *Bytes = code(Image, Address, 12);
  if (!Bytes)
    return std::nullopt;
  auto Word = [&](unsigned I) {
    return llvm::support::endian::read32le(Bytes + I * 4);
  };
  Dispatch Result;
  // ld's selector-specific stubs load x1, then perform the ordinary import
  // veneer. Symbols are deliberately irrelevant, so stripped stubs work too.
  if (auto Page = adrp(Word(0), Address, 1)) {
    auto Slot = ldrSlot(Word(1), *Page, 1);
    if (!Slot)
      return std::nullopt;
    auto Reference = Image.ObjCSourceReferences.find(*Slot);
    if (Reference == Image.ObjCSourceReferences.end() ||
        Reference->second.TheKind != ObjCSourceReference::Kind::Selector ||
        Reference->second.Size != 8)
      return std::nullopt;
    Result.Selector = Reference->second.Name;
    Result.SelectorSlot = *Slot;
    Result.LoadsSelector = true;
    Address += 8;
    Bytes = code(Image, Address, 12);
    if (!Bytes)
      return std::nullopt;
  }
  auto Page = adrp(Word(0), Address, 16);
  auto Slot = Page ? ldrSlot(Word(1), *Page, 16) : std::nullopt;
  if (!Slot || Word(2) != 0xd61f0200u) // BR x16
    return std::nullopt;
  Result.ImportSlot = *Slot;
  return Result;
}

std::optional<Dispatch> veneer(const BinaryImage &Image, va_t Address) {
  auto Result = veneerStorage(Image, Address);
  if (!Result)
    return std::nullopt;
  Result->Name = importAt(Image, Result->ImportSlot);
  // A selector-loading veneer only has a proven ABI for message dispatch.
  if (Result->SelectorSlot && Result->Name != "objc_msgSend" &&
      Result->Name != "objc_msgSendSuper2")
    return std::nullopt;
  return Result->Name.empty() ? std::nullopt : Result;
}

struct BlockIdentity {
  va_t CopySite = 0, Descriptor = 0, Invoke = 0;
  uint32_t Flags = 0;
  bool operator==(const BlockIdentity &) const = default;
};

struct Value {
  enum class Kind {
    Number,
    NumberSet,
    Selector,
    Import,
    Receiver,
    IvarOffset,
    FieldAddress,
    Frame,
    BlockIsa,
    ImageBytes,
    CopiedBlock,
    BlockInvoke,
    SourceParameter
  };
  Kind TheKind = Kind::Number;
  uint64_t Number = 0;
  std::string Name;
  std::optional<ObjCReceiverTypeHint> Object;
  std::optional<BlockIdentity> Block;
  va_t SourceMethodEntry = 0;
  SourceABIValueLocation SourceLocation;
  std::vector<uint64_t> AlternativeNumbers;
  bool operator==(const Value &Other) const {
    return std::tie(TheKind, Number, Name, Object, Block, SourceMethodEntry,
                    SourceLocation.Kind, SourceLocation.RegisterOffset,
                    SourceLocation.EntryStackOffset, SourceLocation.ValueBytes,
                    SourceLocation.ExtendTo32Bits, AlternativeNumbers) ==
           std::tie(Other.TheKind, Other.Number, Other.Name, Other.Object,
                    Other.Block, Other.SourceMethodEntry,
                    Other.SourceLocation.Kind,
                    Other.SourceLocation.RegisterOffset,
                    Other.SourceLocation.EntryStackOffset,
                    Other.SourceLocation.ValueBytes,
                    Other.SourceLocation.ExtendTo32Bits,
                    Other.AlternativeNumbers);
  }
};

std::optional<Value> mergeNumberCandidates(const Value &Left,
                                           const Value &Right) {
  const auto IsNumber = [](const Value &Candidate) {
    return Candidate.TheKind == Value::Kind::Number ||
           Candidate.TheKind == Value::Kind::NumberSet;
  };
  if (!IsNumber(Left) || !IsNumber(Right))
    return std::nullopt;
  std::vector<uint64_t> Candidates{Left.Number, Right.Number};
  Candidates.insert(Candidates.end(), Left.AlternativeNumbers.begin(),
                    Left.AlternativeNumbers.end());
  Candidates.insert(Candidates.end(), Right.AlternativeNumbers.begin(),
                    Right.AlternativeNumbers.end());
  llvm::sort(Candidates);
  Candidates.erase(std::unique(Candidates.begin(), Candidates.end()),
                   Candidates.end());
  if (Candidates.empty() || Candidates.size() > 64)
    return std::nullopt;
  Value Result;
  Result.TheKind =
      Candidates.size() == 1 ? Value::Kind::Number : Value::Kind::NumberSet;
  Result.Number = Candidates.front();
  Result.AlternativeNumbers.assign(Candidates.begin() + 1, Candidates.end());
  return Result;
}
using Key = std::tuple<VnodeSpace, uint64_t, uint16_t>;
Key key(const NdVar &V) { return {V.Space, V.Offset, V.Size}; }

constexpr int64_t FrameOffsetLimit = 1048576;

bool fitsUnsignedValue(uint64_t Value, unsigned Bytes) {
  return Bytes && Bytes <= 8 &&
         (Bytes == 8 || Value < (UINT64_C(1) << (Bytes * 8)));
}

struct LocalResultUse {
  SourceABIValueLocation Location;
  std::optional<NdTypeKind> TypeKind;
};

std::optional<LocalResultUse> localResultUse(const LowFunc &Function,
                                             size_t InitialBlock,
                                             size_t CallIndex,
                                             const TargetRegInfo &TRI,
                                             const BinaryImage &Image) {
  struct Alias {
    NdVar Value;
    uint16_t SourceOffset = 0;
  };
  auto Overlap = [](const NdVar &A, const NdVar &B) {
    return A.Space == VnodeSpace::REG && B.Space == VnodeSpace::REG && A.Size &&
           B.Size && A.Offset < B.Offset + B.Size &&
           B.Offset < A.Offset + A.Size;
  };
  auto KnownCallSignature = [&](const LowOp &Op)
      -> std::optional<SourceFunctionTypeHint> {
    if (Op.Opcode != NdOp::CALL || !Op.NumInputs ||
        Op.Inputs[0].Space != VnodeSpace::CONST)
      return std::nullopt;
    const auto Target = veneer(Image, Op.Inputs[0].Offset);
    if (!Target)
      return std::nullopt;
    if (Target->Name == "objc_msgSend" && !Target->Selector.empty())
      return objcSelectorSourceTypeHint(Image, Target->Selector);
    for (auto Hint : {objcRuntimeSourceCallHint(Image, Target->ImportSlot),
                      swiftRuntimeSourceCallHint(Image, Target->ImportSlot),
                      darwinRuntimeSourceCallHint(Image, Target->ImportSlot),
                      swiftStringSourceCallHint(Image, Target->ImportSlot)})
      if (Hint)
        return Hint->Signature;
    return std::nullopt;
  };
  std::map<int, size_t> Blocks;
  for (size_t I = 0; I < Function.Blocks.size(); ++I)
    if (!Blocks.emplace(Function.Blocks[I].Id, I).second)
      return std::nullopt;
  if (InitialBlock >= Function.Blocks.size())
    return std::nullopt;
  auto Scan = [&](SourceABICarrierKind Kind, uint64_t Begin,
                  uint16_t FullWidth) -> std::optional<LocalResultUse> {
    if (!FullWidth)
      return std::nullopt;
    struct State {
      size_t Block = 0;
      size_t FirstOp = 0;
      std::vector<Alias> Aliases;
    };
    std::vector<State> Work{{InitialBlock, CallIndex + 1,
                             {{NdVar::reg(Begin, FullWidth), 0}}}};
    using AliasKey = std::tuple<uint64_t, uint16_t, uint16_t>;
    std::set<std::tuple<size_t, size_t, std::vector<AliasKey>>> Seen;
    std::optional<LocalResultUse> Result;
    bool Ambiguous = false;
    size_t Budget = 4096;
    auto Observe = [&](LocalResultUse Use) {
      const auto Same = [](const LocalResultUse &A,
                           const LocalResultUse &B) {
        return A.Location.Kind == B.Location.Kind &&
               A.Location.RegisterOffset == B.Location.RegisterOffset &&
               A.Location.ValueBytes == B.Location.ValueBytes &&
               A.TypeKind == B.TypeKind;
      };
      if (Result && !Same(*Result, Use))
        Ambiguous = true;
      else if (!Result)
        Result = std::move(Use);
    };
    while (!Work.empty() && !Ambiguous) {
      auto Current = std::move(Work.back());
      Work.pop_back();
      if (!Budget-- || Current.Block >= Function.Blocks.size())
        return std::nullopt;
      std::vector<AliasKey> Keys;
      for (const auto &Alias : Current.Aliases)
        Keys.emplace_back(Alias.Value.Offset, Alias.Value.Size,
                          Alias.SourceOffset);
      llvm::sort(Keys);
      Keys.erase(std::unique(Keys.begin(), Keys.end()), Keys.end());
      if (!Seen.emplace(Current.Block, Current.FirstOp, std::move(Keys)).second)
        continue;
      const auto &Block = Function.Blocks[Current.Block];
      bool Used = false;
      for (size_t I = Current.FirstOp; I < Block.Ops.size() && !Used; ++I) {
        const auto &Op = Block.Ops[I];
        const bool Copy = Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
                          Op.Output.Space == VnodeSpace::REG;
        std::optional<Alias> Copied;
        if (Copy)
          for (const auto &Alias : Current.Aliases)
            if (Op.Inputs[0].Space == VnodeSpace::REG &&
                Op.Inputs[0].Offset == Alias.Value.Offset &&
                Op.Inputs[0].Size == Alias.Value.Size) {
              Copied = Alias;
              break;
            }
        for (uint8_t J = 0; J < Op.NumInputs && !Used; ++J) {
          const auto &Input = Op.Inputs[J];
          for (const auto &Alias : Current.Aliases) {
            if (!Overlap(Input, Alias.Value) || (Copied && J == 0))
              continue;
            const uint64_t AliasBegin =
                std::max<uint64_t>(Input.Offset, Alias.Value.Offset);
            const uint64_t AliasEnd =
                std::min<uint64_t>(Input.Offset + Input.Size,
                                   Alias.Value.Offset + Alias.Value.Size);
            LocalResultUse Use;
            Use.Location.Kind = Kind;
            Use.Location.RegisterOffset =
                Begin + Alias.SourceOffset +
                (AliasBegin - Alias.Value.Offset);
            Use.Location.ValueBytes =
                static_cast<uint16_t>(AliasEnd - AliasBegin);
            Observe(std::move(Use));
            Used = true;
            break;
          }
        }
        if (Used)
          break;
        if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
            Op.Opcode == NdOp::INTRINSIC) {
          const auto Signature = KnownCallSignature(Op);
          if (!Signature)
            return std::nullopt;
          std::optional<LocalResultUse> ArgumentUse;
          for (const auto &Parameter : sourceABIParameters(*Signature)) {
            const auto &Location = Parameter.Location;
            if (Location.Kind != SourceABICarrierKind::IntegerRegister &&
                Location.Kind != SourceABICarrierKind::FloatingRegister)
              continue;
            for (const auto &Alias : Current.Aliases) {
              if (Alias.Value.Offset != Location.RegisterOffset ||
                  Alias.Value.Size < Location.ValueBytes ||
                  Alias.SourceOffset > FullWidth - Location.ValueBytes)
                continue;
              if (!Parameter.Type ||
                  (Parameter.Type->Kind != NdTypeKind::Int &&
                   Parameter.Type->Kind != NdTypeKind::Ptr &&
                   Parameter.Type->Kind != NdTypeKind::Float) ||
                  ArgumentUse)
                return std::nullopt;
              ArgumentUse = LocalResultUse{
                  {Kind, Begin + Alias.SourceOffset, 0, Location.ValueBytes},
                  Parameter.Type->Kind};
            }
          }
          if (ArgumentUse) {
            Observe(std::move(*ArgumentUse));
            Used = true;
            break;
          }
          for (auto It = Current.Aliases.begin();
               It != Current.Aliases.end();) {
            const auto Preserved = TRI.callPreservedPrefixSize(
                It->Value.Offset, It->Value.Size);
            if (!Preserved)
              It = Current.Aliases.erase(It);
            else {
              It->Value.Size = Preserved;
              ++It;
            }
          }
        }
        if (Op.Output.Space == VnodeSpace::REG && Op.Output.Size)
          std::erase_if(Current.Aliases, [&](const Alias &Alias) {
            return Overlap(Op.Output, Alias.Value);
          });
        if (Copied && Op.Output.Size == Copied->Value.Size)
          Current.Aliases.push_back({Op.Output, Copied->SourceOffset});
        if (Current.Aliases.empty())
          break;
      }
      if (Used || Current.Aliases.empty())
        continue;
      for (int Successor : Block.Succs) {
        const auto Found = Blocks.find(Successor);
        if (Found == Blocks.end())
          return std::nullopt;
        const auto &Child = Function.Blocks[Found->second];
        if (!llvm::is_contained(Child.Preds, Block.Id))
          return std::nullopt;
        Work.push_back({Found->second, 0, Current.Aliases});
      }
    }
    return Ambiguous ? std::nullopt : Result;
  };
  const auto Integer = Scan(SourceABICarrierKind::IntegerRegister,
                            TRI.IntReturnReg, TRI.FullRegWidth);
  const auto FPWidth = !TRI.hasFPReturnReg() ? 0
                       : TRI.FPABIRegWidth
                           ? TRI.FPABIRegWidth
                           : TRI.maxRegisterWidth(TRI.FPReturnReg);
  const auto Floating =
      Scan(SourceABICarrierKind::FloatingRegister, TRI.FPReturnReg, FPWidth);
  return Integer && Floating ? std::nullopt : Integer ? Integer : Floating;
}

std::optional<Value> adjustedFrame(Value Base, uint64_t Amount, bool Subtract) {
  const auto Delta = static_cast<int64_t>(Amount);
  if (Delta < -FrameOffsetLimit || Delta > FrameOffsetLimit)
    return std::nullopt;
  const auto Offset =
      static_cast<int64_t>(Base.Number) + (Subtract ? -Delta : Delta);
  if (Offset < -FrameOffsetLimit || Offset > FrameOffsetLimit)
    return std::nullopt;
  Base.Number = static_cast<uint64_t>(Offset);
  return Base;
}

struct CallFacts {
  std::map<Key, Value> Values;
  std::map<std::pair<int64_t, unsigned>, Value> FrameSlots;
  // Receiver and declared source-parameter identities need less frame
  // knowledge than block construction does.  A pointer to a higher-addressed
  // frame object cannot reach storage wholly below that object's base through
  // the source ABI without a backwards/out-of-object access.  Keep those
  // lower spills while ordinary FrameSlots retain the stricter whole-frame
  // escape rule.
  std::map<std::pair<int64_t, unsigned>, Value> TypedFrameSlots;
  std::optional<int64_t> EscapedTypedFrameFloor;
  bool TypedFrameFullyEscaped = false;
  // Exact values are must facts. Pointer-derived bytes are may facts: a
  // conflicting predecessor or partial alias must not erase a possible escape.
  std::set<std::pair<VnodeSpace, uint64_t>> FrameBytes;
  bool FrameEscaped = false;

  bool operator==(const CallFacts &) const = default;

  bool mayBeFrame(const NdVar &V) const {
    if (!V.Size || V.isConst())
      return false;
    const auto It = FrameBytes.lower_bound({V.Space, V.Offset});
    return It != FrameBytes.end() && It->first == V.Space &&
           It->second - V.Offset < V.Size;
  }

  bool writeFrameBytes(const NdVar &V, bool Tainted) {
    if (!V.Size)
      return true;
    if (V.Offset > InvalidVA - V.Size || V.Size > 4096)
      return false;
    FrameBytes.erase(FrameBytes.lower_bound({V.Space, V.Offset}),
                     FrameBytes.lower_bound({V.Space, V.Offset + V.Size}));
    if (Tainted)
      for (unsigned I = 0; I < V.Size; ++I)
        FrameBytes.emplace(V.Space, V.Offset + I);
    return FrameBytes.size() <= 4096;
  }

  void escapeFrame() {
    FrameEscaped = true;
    FrameSlots.clear();
  }

  bool typedFrameRangePrivate(int64_t Offset, unsigned Size) const {
    if (TypedFrameFullyEscaped || !Size || Offset > INT64_MAX - Size)
      return false;
    return !EscapedTypedFrameFloor ||
           Offset + static_cast<int64_t>(Size) <= *EscapedTypedFrameFloor;
  }

  void invalidateTypedFrameRange(int64_t Offset, unsigned Size) {
    for (auto It = TypedFrameSlots.begin(); It != TypedFrameSlots.end();)
      if (It->first.first < Offset + Size &&
          Offset < It->first.first + It->first.second)
        It = TypedFrameSlots.erase(It);
      else
        ++It;
  }

  void escapeTypedFrame() {
    TypedFrameFullyEscaped = true;
    EscapedTypedFrameFloor.reset();
    TypedFrameSlots.clear();
  }

  void escapeTypedFrameFrom(const std::optional<Value> &Address) {
    if (!Address || Address->TheKind != Value::Kind::Frame) {
      escapeTypedFrame();
      return;
    }
    const auto Offset = static_cast<int64_t>(Address->Number);
    if (!EscapedTypedFrameFloor || Offset < *EscapedTypedFrameFloor)
      EscapedTypedFrameFloor = Offset;
    for (auto It = TypedFrameSlots.begin(); It != TypedFrameSlots.end();)
      if (!typedFrameRangePrivate(It->first.first, It->first.second))
        It = TypedFrameSlots.erase(It);
      else
        ++It;
  }

  void forgetCopiedBlocks() {
    for (auto It = Values.begin(); It != Values.end();)
      if (It->second.Block)
        It = Values.erase(It);
      else
        ++It;
    for (auto It = FrameSlots.begin(); It != FrameSlots.end();)
      if (It->second.Block)
        It = FrameSlots.erase(It);
      else
        ++It;
  }

  void invalidateFrameRange(int64_t Offset, unsigned Size) {
    for (auto It = FrameSlots.begin(); It != FrameSlots.end();)
      if (It->first.first < Offset + Size &&
          Offset < It->first.first + It->first.second)
        It = FrameSlots.erase(It);
      else
        ++It;
  }

  void merge(const CallFacts &Other) {
    // A lost alias must not survive elsewhere as a supposedly private copied
    // block. Drop this family on any inconsistent reaching alias, including
    // an alias present on only one predecessor.
    auto ConflictingBlocks = [](const auto &A, const auto &B) {
      for (const auto &[K, V] : A) {
        const auto Found = B.find(K);
        if (V.Block && (Found == B.end() || !(V == Found->second)))
          return true;
      }
      return false;
    };
    const bool LostBlockAlias =
        ConflictingBlocks(Values, Other.Values) ||
        ConflictingBlocks(Other.Values, Values) ||
        ConflictingBlocks(FrameSlots, Other.FrameSlots) ||
        ConflictingBlocks(Other.FrameSlots, FrameSlots);
    for (auto It = Values.begin(); It != Values.end();) {
      const auto Found = Other.Values.find(It->first);
      if (Found == Other.Values.end()) {
        It = Values.erase(It);
        continue;
      }
      if (!(It->second == Found->second)) {
        if (auto Merged = mergeNumberCandidates(It->second, Found->second)) {
          It->second = std::move(*Merged);
          ++It;
          continue;
        }
      }
      if (!(It->second == Found->second))
        It = Values.erase(It);
      else
        ++It;
    }
    FrameBytes.insert(Other.FrameBytes.begin(), Other.FrameBytes.end());
    TypedFrameFullyEscaped |= Other.TypedFrameFullyEscaped;
    if (Other.EscapedTypedFrameFloor &&
        (!EscapedTypedFrameFloor ||
         *Other.EscapedTypedFrameFloor < *EscapedTypedFrameFloor))
      EscapedTypedFrameFloor = Other.EscapedTypedFrameFloor;
    if (TypedFrameFullyEscaped) {
      EscapedTypedFrameFloor.reset();
      TypedFrameSlots.clear();
    } else {
      for (auto It = TypedFrameSlots.begin(); It != TypedFrameSlots.end();) {
        const auto Found = Other.TypedFrameSlots.find(It->first);
        if (Found == Other.TypedFrameSlots.end() ||
            !(It->second == Found->second) ||
            !typedFrameRangePrivate(It->first.first, It->first.second))
          It = TypedFrameSlots.erase(It);
        else
          ++It;
      }
    }
    if (FrameEscaped || Other.FrameEscaped) {
      escapeFrame();
      if (LostBlockAlias)
        forgetCopiedBlocks();
      return;
    }
    for (auto It = FrameSlots.begin(); It != FrameSlots.end();) {
      const auto Found = Other.FrameSlots.find(It->first);
      if (Found == Other.FrameSlots.end() || !(It->second == Found->second))
        It = FrameSlots.erase(It);
      else
        ++It;
    }
    if (LostBlockAlias)
      forgetCopiedBlocks();
  }
};

std::optional<ObjCReceiverTypeHint> receiver(const Value &V) {
  return V.TheKind == Value::Kind::Receiver ? V.Object : std::nullopt;
}

bool isObjCInitFamily(llvm::StringRef Selector) {
  if (!Selector.consume_front("init"))
    return false;
  return Selector.empty() || Selector.front() < 'a' || Selector.front() > 'z';
}
} // namespace

std::optional<va_t> darwinImportVeneerSlot(const BinaryImage &Image,
                                           va_t Address) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64) ||
      (Image.Arch == Arch::AArch64 && Address % 4) ||
      !readImmutableCodeBytes(Image, Address,
                              Image.Arch == Arch::AArch64 ? 12 : 6))
    return std::nullopt;
  const auto Target = veneerStorage(Image, Address);
  return Target && !Target->LoadsSelector
             ? std::optional<va_t>(Target->ImportSlot)
             : std::nullopt;
}

std::optional<SourceCallTypeHint>
objcRuntimeSourceCallHint(const BinaryImage &Image, va_t ImportSlot) {
  const auto Import = darwinRuntimeImport(Image, ImportSlot);
  if (!Import)
    return std::nullopt;
  llvm::StringRef Name(*Import);
  if (!Name.consume_front("_"))
    return std::nullopt;
  std::optional<unsigned> ArgumentRegister;
  llvm::StringRef Canonical = Name;
  for (llvm::StringRef Operation : {"objc_retain", "objc_release"}) {
    if (!Name.starts_with((Operation + "_x").str()))
      continue;
    const auto Suffix = Name.drop_front(Operation.size() + 2);
    unsigned Register;
    if (Image.Arch != Arch::AArch64 || Suffix.getAsInteger(10, Register) ||
        Suffix != std::to_string(Register) ||
        !(Register <= 15 || (Register >= 19 && Register <= 28)))
      return std::nullopt;
    Canonical = Operation;
    ArgumentRegister = Register;
  }
  SourceCallTypeHint Result;
  Result.CallKind = SourceCallTypeHint::Kind::ObjCRuntimeCall;
  Result.TargetAddress = ImportSlot;
  Result.TargetName = Canonical.str();
  auto &Signature = Result.Signature;
  Signature.Origin = SourceFunctionTypeHint::OriginKind::ObjCRuntime;
  const auto Object = NdType::makePtr(NdType::makeVoid());
  const auto Slot = NdType::makePtr(Object);
  // These ARC runtime contracts return their argument, including nil.
  // https://clang.llvm.org/docs/AutomaticReferenceCounting.html#runtime-support
  const bool ReturnsArgument =
      Canonical == "objc_retain" || Canonical == "objc_autorelease" ||
      Canonical == "objc_autoreleaseReturnValue" ||
      Canonical == "objc_retainAutorelease" ||
      Canonical == "objc_retainAutoreleaseReturnValue" ||
      Canonical == "objc_retainAutoreleasedReturnValue" ||
      Canonical == "objc_unsafeClaimAutoreleasedReturnValue";
  if (ReturnsArgument || Canonical == "objc_retainBlock" ||
      Canonical == "objc_alloc" || Canonical == "objc_allocWithZone" ||
      Canonical == "objc_alloc_init" || Canonical == "objc_opt_new" ||
      Canonical == "objc_opt_self" || Canonical == "objc_opt_class") {
    Signature.ReturnType = Object;
    Signature.Parameters = {{"object", Object}};
  } else if (Canonical == "objc_release" ||
             Canonical == "objc_autoreleasePoolPop") {
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"object", Object}};
  } else if (Canonical == "objc_autoreleasePoolPush") {
    Signature.ReturnType = Object;
  } else if (Canonical == "objc_opt_isKindOfClass" ||
             Canonical == "objc_opt_respondsToSelector") {
    const auto Bind = Image.DyldBindSlots.find(ImportSlot);
    if (Bind == Image.DyldBindSlots.end() ||
        Bind->second.Module != "/usr/lib/libobjc.A.dylib")
      return std::nullopt;
    // These runtime queries return BOOL and preserve custom overrides.
    // https://github.com/apple-oss-distributions/objc4/blob/main/runtime/objc-internal.h
    // Carry its byte unchanged. ARM64 BOOL is unsigned and extends to W0;
    // x86_64 defines only AL, with signed/boolean conversions in the caller.
    Signature.ReturnType = NdType::makeInt(1, false);
    Signature.Parameters = {{"object", Object}, {"query", Object}};
  } else if (Canonical == "objc_storeStrong" || Canonical == "objc_storeWeak" ||
             Canonical == "objc_initWeak") {
    Signature.ReturnType =
        Canonical == "objc_storeStrong" ? NdType::makeVoid() : Object;
    Signature.Parameters = {{"slot", Slot}, {"object", Object}};
  } else if (Canonical == "objc_loadWeak" ||
             Canonical == "objc_loadWeakRetained" ||
             Canonical == "objc_destroyWeak") {
    Signature.ReturnType =
        Canonical == "objc_destroyWeak" ? NdType::makeVoid() : Object;
    Signature.Parameters = {{"slot", Slot}};
  } else if (Canonical == "objc_copyWeak" || Canonical == "objc_moveWeak") {
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"destination", Slot}, {"source", Slot}};
  } else if (Canonical == "objc_getAssociatedObject") {
    Signature.ReturnType = Object;
    Signature.Parameters = {{"object", Object}, {"key", Object}};
  } else if (Canonical == "objc_setAssociatedObject") {
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"object", Object},
                            {"key", Object},
                            {"value", Object},
                            {"policy", NdType::makeInt(8, false)}};
  } else if (Canonical == "objc_removeAssociatedObjects" ||
             Canonical == "objc_enumerationMutation") {
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"object", Object}};
  } else if (Canonical == "objc_getProperty") {
    const auto Bind = Image.DyldBindSlots.find(ImportSlot);
    if (Bind == Image.DyldBindSlots.end() ||
        Bind->second.Module != "/usr/lib/libobjc.A.dylib")
      return std::nullopt;
    // objc4/runtime/objc-accessors.mm: id, SEL, ptrdiff_t, BOOL. Preserve the
    // runtime operation: an atomic getter retains under the property lock and
    // autoreleases its result. Reading the ivar directly is not equivalent.
    Signature.ReturnType = Object;
    Signature.Parameters = {
        {"object", Object},
        {"selector", Object},
        {"offset", NdType::makeInt(8)},
        {"atomic", NdType::makeInt(1, Image.Arch == Arch::X64)}};
  } else if (Canonical == "objc_setProperty_atomic" ||
             Canonical == "objc_setProperty_nonatomic" ||
             Canonical == "objc_setProperty_atomic_copy" ||
             Canonical == "objc_setProperty_nonatomic_copy") {
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"object", Object},
                            {"selector", Object},
                            {"value", Object},
                            {"offset", NdType::makeInt(8)}};
  } else {
    return std::nullopt;
  }
  std::string Diagnostic;
  if (!assignDarwinScalarSourceABI(Signature, Image.Arch, Diagnostic))
    return std::nullopt;
  if (ArgumentRegister)
    Signature.Parameters[0].Location.RegisterOffset =
        a64reg::X0 + *ArgumentRegister * 8;
  const auto Bind = Image.DyldBindSlots.find(ImportSlot);
  if (ReturnsArgument && Bind != Image.DyldBindSlots.end() &&
      Bind->second.Module == "/usr/lib/libobjc.A.dylib")
    Result.ReturnedArgument = 0;
  // These routines preserve the corresponding message's overrides. Use its
  // declared result type, never an allocation or pointer-identity assumption.
  if (Bind != Image.DyldBindSlots.end() &&
      Bind->second.Module == "/usr/lib/libobjc.A.dylib") {
    if (Canonical == "objc_alloc")
      Result.RuntimeObjCResultType = {0, {"alloc"}};
    else if (Canonical == "objc_allocWithZone")
      Result.RuntimeObjCResultType = {0, {"allocWithZone:"}};
    else if (Canonical == "objc_alloc_init")
      Result.RuntimeObjCResultType = {0, {"alloc", "init"}};
    else if (Canonical == "objc_opt_new")
      Result.RuntimeObjCResultType = {0, {"new"}};
  }
  return Result;
}

bool objcSelectorStubOverwritesCommand(const BinaryImage &Image, va_t Address) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64)
    return false;
  const auto *Section = Image.getSectionFor(Address);
  if (!Section || Section->Name != section_names::macho::ObjCStubs)
    return false;
  const auto Target = veneer(Image, Address);
  return Target && Target->Name == "objc_msgSend" &&
         Target->SelectorSlot != 0 && !Target->Selector.empty();
}

std::optional<SourceCallTypeHint>
objcSelectorStubSentinelSourceCallHint(const BinaryImage &Image, va_t Address,
                                       const ObjCReceiverTypeHint &Receiver,
                                       llvm::ArrayRef<va_t> Objects) {
  const auto Target = veneer(Image, Address);
  if (!Target || Target->Name != "objc_msgSend" || !Target->SelectorSlot)
    return std::nullopt;
  const auto Import = Image.DyldBindSlots.find(Target->ImportSlot);
  if (Import == Image.DyldBindSlots.end() ||
      Import->second.Name != "_objc_msgSend" || Import->second.Addend ||
      Import->second.WeakImport ||
      Import->second.Module != "/usr/lib/libobjc.A.dylib" ||
      std::find(Image.DynInfo.NeededLibs.begin(),
                Image.DynInfo.NeededLibs.end(), Import->second.Module) ==
          Image.DynInfo.NeededLibs.end())
    return std::nullopt;
  auto Hint =
      objcSentinelSourceCallHint(Image, Target->Selector, Receiver, Objects);
  if (Hint) {
    Hint->TargetAddress = Address;
    Hint->SelectorReferenceAddress = Target->SelectorSlot;
  }
  return Hint;
}

std::optional<SourceCallTypeHint>
objcSelectorStubDynamicFormatSourceCallHint(const BinaryImage &Image,
                                            va_t Address) {
  if (!objcSelectorStubOverwritesCommand(Image, Address))
    return std::nullopt;
  const auto Target = veneer(Image, Address);
  if (!Target || Target->Name != "objc_msgSend" || Target->Selector.empty() ||
      !Target->SelectorSlot)
    return std::nullopt;
  auto Hint =
      objcDynamicFormatWithoutArgumentsSourceCallHint(Image, Target->Selector);
  if (!Hint || !Hint->Format ||
      Hint->Format->FixedCount != Hint->Signature.Parameters.size())
    return std::nullopt;
  Hint->TargetAddress = Address;
  Hint->SelectorReferenceAddress = Target->SelectorSlot;
  return Hint;
}

std::map<va_t, SourceCallTypeHint>
buildObjCSourceCallHints(const BinaryImage &Image, const LowFunc &Function) {
  std::map<va_t, SourceCallTypeHint> Result;
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64))
    return Result;
  const auto &TRI = getTargetRegInfo(Image.Arch);
  const auto RegisterCopies = sourceRegisterCopies(Image, Function);
  const size_t Count = Function.Blocks.size();
  if (Count > 16384)
    return {};
  std::map<int, size_t> BlockIndices;
  for (size_t I = 0; I < Function.Blocks.size(); ++I)
    if (!BlockIndices.emplace(Function.Blocks[I].Id, I).second)
      return {};
  // Only reciprocal ordinary edges carry machine-state facts. Explicit
  // entry roles and exceptional entries contribute an unknown incoming state.
  // Unvisited predecessors are the dataflow bottom, not an unknown value;
  // later backedges can invalidate provisional facts before publication.
  std::vector<std::vector<size_t>> Parents(Count), Children(Count);
  std::vector<bool> Roots(Count), ExceptionalRoots(Count), Queued(Count);
  std::map<va_t, unsigned> CallOccurrences;
  for (size_t I = 0; I < Count; ++I) {
    const auto &Block = Function.Blocks[I];
    Roots[I] = Block.StartAddr == Function.Entry || Block.Preds.empty() ||
               Function.ModuleAnalysisRoots.count(Block.StartAddr) ||
               Function.OrdinaryModuleAnalysisRoots.count(Block.StartAddr) ||
               !Block.ExceptionalPreds.empty();
    ExceptionalRoots[I] = !Block.ExceptionalPreds.empty();
    for (int Parent : Block.Preds) {
      const auto Found = BlockIndices.find(Parent);
      if (Found == BlockIndices.end() ||
          !Function.Blocks[Found->second].hasSucc(Block.Id))
        return {};
      Parents[I].push_back(Found->second);
    }
    for (int Child : Block.Succs) {
      const auto Found = BlockIndices.find(Child);
      if (Found == BlockIndices.end() ||
          !llvm::is_contained(Function.Blocks[Found->second].Preds, Block.Id))
        return {};
      Children[I].push_back(Found->second);
    }
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
        ++CallOccurrences[Op.Addr];
  }
  for (const auto &Block : Function.Blocks)
    for (const auto &Edge : Block.ExceptionalSuccs) {
      if (Edge.BlockId < 0)
        continue; // An unwind outside this function has no local successor.
      const auto Found = BlockIndices.find(Edge.BlockId);
      if (Found == BlockIndices.end())
        return {};
      Roots[Found->second] = true;
      ExceptionalRoots[Found->second] = true;
    }
  using Facts = CallFacts;
  using Hints = std::map<va_t, SourceCallTypeHint>;
  Facts EntryFacts;
  if (llvm::count_if(Function.Blocks, [&](const auto &Block) {
        return Block.StartAddr == Function.Entry;
      }) == 1) {
    const auto Stack = NdVar::reg(TRI.StackPointer, 8);
    EntryFacts.Values.emplace(key(Stack), Value{Value::Kind::Frame, 0, {}});
    EntryFacts.writeFrameBytes(Stack, true);
    if (const auto Receiver = objcMethodReceiverTypeHint(Image, Function.Entry))
      EntryFacts.Values.emplace(key(NdVar::reg(TRI.IntParamRegs[0], 8)),
                                Value{Value::Kind::Receiver, 0, {}, *Receiver});
    if (const auto Signature = objcMethodSourceTypeHint(Image, Function.Entry))
      for (const auto &Parameter : Signature->Parameters) {
        const auto &Type = Parameter.Type;
        const auto &Location = Parameter.Location;
        if (!Type || Type->Kind != NdTypeKind::Ptr || Type->Size != 8 ||
            !Type->Pointee || Type->Pointee->Kind != NdTypeKind::Ptr ||
            Type->Pointee->Size != 8 ||
            Location.Kind != SourceABICarrierKind::IntegerRegister ||
            Location.ValueBytes != 8)
          continue;
        Value V;
        V.TheKind = Value::Kind::SourceParameter;
        V.SourceMethodEntry = Function.Entry;
        V.SourceLocation = Location;
        EntryFacts.Values.emplace(key(NdVar::reg(Location.RegisterOffset, 8)),
                                  std::move(V));
      }
  }
  // Enumerate preserved physical bytes through the authoritative ABI policy.
  // This retains upper bytes of partial integer aliases and only the preserved
  // prefix of a vector register. A balanced declared call also preserves SP.
  std::set<uint64_t> PreservedBytes;
  auto AddPreserved = [&](uint64_t Register) {
    const auto Size =
        TRI.callPreservedPrefixSize(Register, TRI.maxRegisterWidth(Register));
    for (unsigned I = 0; I < Size; ++I)
      PreservedBytes.insert(Register + I);
  };
  for (auto Register : TRI.CalleeSaveRegs)
    AddPreserved(Register);
  for (unsigned I = 0; I < TRI.VecRegCount; ++I)
    AddPreserved(TRI.VecRegBase + I * TRI.VecRegStride);
  for (unsigned I = 0; I < 8; ++I)
    PreservedBytes.insert(TRI.StackPointer + I);
  using ReceiverKey =
      std::tuple<ObjCReceiverTypeHint::OriginKind, va_t, std::string, bool,
                 std::vector<ObjCReceiverTypeHint::TypeStep>, std::string>;
  std::map<ReceiverKey, ObjCReceiverDeclaration> ReceiverDeclarations;
  auto Transfer = [&](size_t Index, Facts State,
                      Hints &BlockHints) -> std::optional<Facts> {
    auto &Values = State.Values;
    const auto &Block = Function.Blocks[Index];
    va_t PreviousAddress = InvalidVA;
    auto Read = [&](const NdVar &V) -> std::optional<Value> {
      if (V.Space == VnodeSpace::CONST)
        return Value{Value::Kind::Number, V.Offset, {}};
      auto It = Values.find(key(V));
      return It == Values.end() ? std::nullopt
                                : std::optional<Value>(It->second);
    };
    auto CopiedBlockInput = [&](const NdVar &V) {
      for (const auto &[K, Fact] : Values)
        if (Fact.Block && std::get<0>(K) == V.Space &&
            std::get<1>(K) < V.Offset + V.Size &&
            V.Offset < std::get<1>(K) + std::get<2>(K))
          return true;
      return false;
    };
    auto StackBlock = [&](const Value &Address,
                          va_t CopySite) -> std::optional<Value> {
      if (Address.TheKind != Value::Kind::Frame || State.FrameEscaped)
        return std::nullopt;
      const int64_t Base = static_cast<int64_t>(Address.Number);
      auto Word = [&](int64_t Offset, unsigned Size) -> std::optional<Value> {
        const auto It = State.FrameSlots.find({Base + Offset, Size});
        return It == State.FrameSlots.end() ? std::nullopt
                                            : std::optional<Value>(It->second);
      };
      auto Bits = [&](const std::optional<Value> &V,
                      unsigned Size) -> std::optional<uint64_t> {
        if (!V)
          return std::nullopt;
        if (V->TheKind == Value::Kind::Number)
          return V->Number;
        if (V->TheKind != Value::Kind::ImageBytes)
          return std::nullopt;
        auto Bytes = readImmutableImageBytes(Image, V->Number, Size);
        if (!Bytes)
          return std::nullopt;
        uint64_t Result = 0;
        for (unsigned I = 0; I < Size; ++I)
          Result |= uint64_t((*Bytes)[I]) << (8 * I);
        return Result;
      };
      const auto Isa = Word(0, 8), Invoke = Word(16, 8), D = Word(24, 8);
      auto Flags = Bits(Word(8, 8), 8);
      if (!Flags) {
        const auto Low = Bits(Word(8, 4), 4), High = Bits(Word(12, 4), 4);
        if (Low && High)
          Flags = (*Low & UINT32_MAX) | (*High << 32);
      }
      if (!Isa || Isa->TheKind != Value::Kind::BlockIsa || !Invoke ||
          Invoke->TheKind != Value::Kind::Number ||
          !Image.isCodeAddress(Invoke->Number) || !D ||
          D->TheKind != Value::Kind::Number || !Flags || *Flags > UINT32_MAX ||
          ((*Flags & (1u << 28)) && !(*Flags & (1u << 23))))
        return std::nullopt;
      std::string Error;
      const auto Descriptor =
          readObjCBlockDescriptor(Image, D->Number, uint32_t(*Flags), Error);
      if (!Descriptor || !Descriptor->InvokeTypeHint ||
          !Descriptor->Limitations.empty())
        return std::nullopt;
      Value V{Value::Kind::CopiedBlock};
      V.Block =
          BlockIdentity{CopySite, D->Number, Invoke->Number, uint32_t(*Flags)};
      return V;
    };
    auto Clobber = [&](const SourceFunctionTypeHint *Signature,
                       bool PreserveReceiverRegisters = false) {
      const bool KnownABI = Signature && Signature->HasExplicitABI;
      if (!KnownABI) {
        for (const auto &[K, V] : Values) {
          const auto &[Space, Offset, Size] = K;
          if (Space != VnodeSpace::REG ||
              (Offset == TRI.StackPointer && Size == 8) ||
              V.TheKind != Value::Kind::Frame)
            continue;
          State.escapeTypedFrameFrom(V);
        }
        for (const auto &[Space, Byte] : State.FrameBytes) {
          if (Space != VnodeSpace::REG ||
              (TRI.StackPointer <= Byte && Byte < TRI.StackPointer + 8))
            continue;
          const bool Exact = llvm::any_of(Values, [&](const auto &Entry) {
            const auto &[K, V] = Entry;
            const auto &[ValueSpace, Offset, Size] = K;
            return ValueSpace == Space && V.TheKind == Value::Kind::Frame &&
                   Offset <= Byte && Byte < Offset + Size;
          });
          if (!Exact) {
            State.escapeTypedFrame();
            break;
          }
        }
        State.escapeFrame();
      } else {
        int64_t ArgumentBytes = 0;
        const int64_t ReturnAddressBytes = Image.Arch == Arch::X64 ? 8 : 0;
        auto CheckArgument = [&](const SourceABIValueLocation &Location) {
          if (Location.Kind == SourceABICarrierKind::IntegerRegister ||
              Location.Kind == SourceABICarrierKind::FloatingRegister ||
              Location.Kind == SourceABICarrierKind::IndirectResultPointer) {
            const auto Argument =
                NdVar::reg(Location.RegisterOffset, Location.ValueBytes);
            if (State.mayBeFrame(Argument)) {
              State.escapeTypedFrameFrom(Read(Argument));
              State.escapeFrame();
            }
            if (CopiedBlockInput(
                    NdVar::reg(Location.RegisterOffset, Location.ValueBytes)))
              State.forgetCopiedBlocks();
          } else if (Location.Kind == SourceABICarrierKind::Stack) {
            const int64_t Begin =
                Location.EntryStackOffset - ReturnAddressBytes;
            if (Begin < 0 || Begin > 4096 || Location.ValueBytes > 4096 - Begin)
              State.escapeTypedFrame();
            if (Begin < 0 || Begin > 4096 || Location.ValueBytes > 4096 - Begin)
              State.escapeFrame();
            else
              ArgumentBytes =
                  std::max(ArgumentBytes, Begin + Location.ValueBytes);
          }
        };
        for (const auto &Parameter : Signature->Parameters) {
          CheckArgument(Parameter.Location);
          for (const auto &Component : Parameter.Components)
            CheckArgument(Component);
        }
        if (Signature->ReturnLocation.Kind ==
            SourceABICarrierKind::IndirectResultPointer)
          CheckArgument(Signature->ReturnLocation);
        const auto Stack = Read(NdVar::reg(TRI.StackPointer, 8));
        if (!Stack || Stack->TheKind != Value::Kind::Frame) {
          State.FrameSlots.clear();
        } else {
          // Exclude outgoing argument storage and its alignment padding. The
          // callee may also overwrite everything below the current call SP.
          const auto Begin = static_cast<int64_t>(Stack->Number) +
                             ((ArgumentBytes + 15) & -int64_t(16));
          for (auto It = State.FrameSlots.begin();
               It != State.FrameSlots.end();)
            if (It->first.first < Begin)
              It = State.FrameSlots.erase(It);
            else
              ++It;
        }
      }
      for (auto It = Values.begin(); It != Values.end();) {
        const auto &[Space, Offset, Size] = It->first;
        // The exact libobjc dispatch entry still obeys Darwin's preserved
        // register contract when its selector ABI is unavailable. Keep only
        // receiver type identity there; unknown arguments can escape the
        // frame, and no other value fact crosses an unbound call.
        const bool PreservedReceiver =
            PreserveReceiverRegisters &&
            It->second.TheKind == Value::Kind::Receiver &&
            Space == VnodeSpace::REG && TRI.isCallPreserved(Offset, Size);
        const bool StackIdentity = Space == VnodeSpace::REG &&
                                   Offset == TRI.StackPointer && Size == 8 &&
                                   It->second.TheKind == Value::Kind::Frame;
        if ((!KnownABI && !PreservedReceiver && !StackIdentity) ||
            (KnownABI && (Space != VnodeSpace::REG ||
                          (!(Offset == TRI.StackPointer && Size == 8) &&
                           !TRI.isCallPreserved(Offset, Size)))))
          It = Values.erase(It);
        else
          ++It;
      }
      for (auto It = State.FrameBytes.begin(); It != State.FrameBytes.end();)
        if (It->first != VnodeSpace::REG ||
            ((!KnownABI && !(TRI.StackPointer <= It->second &&
                             It->second < TRI.StackPointer + 8)) ||
             (KnownABI && !PreservedBytes.count(It->second))))
          It = State.FrameBytes.erase(It);
        else
          ++It;
    };
    for (size_t OpIndex = 0; OpIndex < Block.Ops.size(); ++OpIndex) {
      const auto &Op = Block.Ops[OpIndex];
      if (Op.Addr != PreviousAddress) {
        for (auto It = Values.begin(); It != Values.end();)
          if (std::get<0>(It->first) == VnodeSpace::TEMP)
            It = Values.erase(It);
          else
            ++It;
        for (auto It = State.FrameBytes.begin(); It != State.FrameBytes.end();)
          if (It->first == VnodeSpace::TEMP)
            It = State.FrameBytes.erase(It);
          else
            ++It;
        PreviousAddress = Op.Addr;
      }
      if (Op.Opcode == NdOp::INTRINSIC) {
        Clobber(nullptr);
        continue;
      }
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        if (CallOccurrences.at(Op.Addr) != 1) {
          Clobber(nullptr);
          continue;
        }
        if (const auto Site = sourceCallOccurrenceKey(Op); Site) {
          const auto Found = RegisterCopies.find(*Site);
          if (Found != RegisterCopies.end()) {
            struct Snapshot {
              uint64_t Destination;
              std::optional<Value> Fact;
              std::array<bool, 8> Frame;
            };
            std::vector<Snapshot> Snapshots;
            for (const auto &[Destination, Source] : Found->second.Registers) {
              Snapshot Copy{Destination, std::nullopt, {}};
              if (const auto *Entry =
                      std::get_if<SourceEntryRegister>(&Source)) {
                Copy.Fact = Read(NdVar::reg(Entry->Offset, 8));
                for (unsigned I = 0; I < 8; ++I)
                  Copy.Frame[I] = State.FrameBytes.count(
                      {VnodeSpace::REG, Entry->Offset + I});
              } else {
                Copy.Fact =
                    Value{Value::Kind::Number,
                          std::get<SourceConstantStringAddress>(Source).Address,
                          {}};
              }
              Snapshots.push_back(std::move(Copy));
            }
            for (auto &Copy : Snapshots) {
              for (auto It = Values.begin(); It != Values.end();)
                if (std::get<0>(It->first) == VnodeSpace::REG &&
                    std::get<1>(It->first) < Copy.Destination + 8 &&
                    Copy.Destination <
                        std::get<1>(It->first) + std::get<2>(It->first))
                  It = Values.erase(It);
                else
                  ++It;
              if (Copy.Fact)
                Values.emplace(key(NdVar::reg(Copy.Destination, 8)),
                               std::move(*Copy.Fact));
              for (unsigned I = 0; I < 8; ++I) {
                const auto Byte =
                    std::pair{VnodeSpace::REG, Copy.Destination + I};
                State.FrameBytes.erase(Byte);
                if (Copy.Frame[I])
                  State.FrameBytes.insert(Byte);
              }
            }
            if (State.FrameBytes.size() > 4096)
              return std::nullopt;
            continue;
          }
        }
        std::optional<Dispatch> Target;
        auto V = Op.NumInputs ? Read(Op.Inputs[0]) : std::nullopt;
        if (Op.Opcode == NdOp::INDIR_CALL && V && V->Block &&
            V->TheKind == Value::Kind::BlockInvoke) {
          const auto Receiver = Read(NdVar::reg(TRI.IntParamRegs[0], 8));
          std::string Error;
          const auto D = readObjCBlockDescriptor(Image, V->Block->Descriptor,
                                                 V->Block->Flags, Error);
          if (Receiver && Receiver->TheKind == Value::Kind::CopiedBlock &&
              Receiver->Number == 0 && Receiver->Block == V->Block && D &&
              D->InvokeTypeHint && D->Limitations.empty()) {
            SourceCallTypeHint Hint;
            Hint.CallKind = SourceCallTypeHint::Kind::BlockInvoke;
            Hint.Signature = *D->InvokeTypeHint;
            Clobber(&Hint.Signature);
            BlockHints.emplace(Op.Addr, std::move(Hint));
            continue;
          }
        }
        if (V && V->TheKind == Value::Kind::Import)
          Target = Dispatch{V->Name, {}, 0, V->Number};
        else if (V && V->TheKind == Value::Kind::Number) {
          if (Op.Opcode == NdOp::INDIR_CALL &&
              Op.Inputs[0].Space == VnodeSpace::CONST) {
            auto Name = importAt(Image, V->Number);
            if (!Name.empty())
              Target = Dispatch{Name, {}, 0, V->Number};
          } else if (Op.Opcode == NdOp::CALL) {
            Target = veneer(Image, V->Number);
          }
        }
        if (Target) {
          auto Runtime = objcRuntimeSourceCallHint(Image, Target->ImportSlot);
          if (!Runtime)
            Runtime = swiftRuntimeSourceCallHint(Image, Target->ImportSlot);
          if (!Runtime)
            Runtime = darwinRuntimeSourceCallHint(Image, Target->ImportSlot);
          if (!Runtime)
            Runtime = swiftStringSourceCallHint(Image, Target->ImportSlot);
          if (!Runtime) {
            const auto Declaration =
                darwinRuntimeFormatDeclaration(Image, Target->ImportSlot);
            if (Declaration) {
              const auto &Location =
                  Declaration->Signature
                      .Parameters[Declaration->FormatParameter]
                      .Location;
              const auto Format =
                  Location.Kind == SourceABICarrierKind::IntegerRegister
                      ? Read(NdVar::reg(Location.RegisterOffset, 8))
                      : std::nullopt;
              if (Format && Format->TheKind == Value::Kind::Number)
                Runtime = darwinFormattedSourceCallHint(
                    Image, Target->ImportSlot, Format->Number);
            }
          }
          if (Runtime) {
            std::optional<Value> ReturnedReceiver;
            const auto &Signature = Runtime->Signature;
            const auto &Return = Signature.ReturnLocation;
            const auto Bind = Image.DyldBindSlots.find(Runtime->TargetAddress);
            const bool CopiesStackBlock =
                Bind != Image.DyldBindSlots.end() &&
                ((Runtime->CallKind ==
                      SourceCallTypeHint::Kind::ObjCRuntimeCall &&
                  Runtime->TargetName == "objc_retainBlock" &&
                  Bind->second.Module == "/usr/lib/libobjc.A.dylib") ||
                 (Runtime->CallKind ==
                      SourceCallTypeHint::Kind::DarwinRuntimeCall &&
                  Runtime->TargetName == "_Block_copy" &&
                  darwinExportModuleMatches("/usr/lib/libSystem.B.dylib|/usr/"
                                            "lib/system/libsystem_blocks.dylib",
                                            Bind->second.Module)));
            if (CopiesStackBlock && Signature.Parameters.size() == 1 &&
                Signature.Parameters[0].Location.Kind ==
                    SourceABICarrierKind::IntegerRegister &&
                Signature.Parameters[0].Location.ValueBytes == 8 &&
                Return.Kind == SourceABICarrierKind::IntegerRegister &&
                Return.ValueBytes == 8) {
              const auto Input = Read(NdVar::reg(
                  Signature.Parameters[0].Location.RegisterOffset, 8));
              if (Input)
                ReturnedReceiver = StackBlock(*Input, Op.Addr);
            }
            if (Runtime->ReturnedArgument &&
                *Runtime->ReturnedArgument < Signature.Parameters.size()) {
              const auto &Argument =
                  Signature.Parameters[*Runtime->ReturnedArgument].Location;
              if (Argument.Kind == SourceABICarrierKind::IntegerRegister &&
                  Argument.ValueBytes == 8 &&
                  Return.Kind == SourceABICarrierKind::IntegerRegister &&
                  Return.ValueBytes == 8) {
                auto Input = Read(NdVar::reg(Argument.RegisterOffset, 8));
                if (Input && receiver(*Input))
                  ReturnedReceiver = std::move(Input);
              }
            }
            if (Runtime->RuntimeObjCResultType) {
              const auto &Effect = *Runtime->RuntimeObjCResultType;
              if (Effect.ReceiverArgument < Signature.Parameters.size() &&
                  Return.Kind == SourceABICarrierKind::IntegerRegister &&
                  Return.ValueBytes == 8) {
                const auto &Argument =
                    Signature.Parameters[Effect.ReceiverArgument].Location;
                const auto Input =
                    Argument.Kind == SourceABICarrierKind::IntegerRegister &&
                            Argument.ValueBytes == 8
                        ? Read(NdVar::reg(Argument.RegisterOffset, 8))
                        : std::nullopt;
                auto Type = Input ? receiver(*Input) : std::nullopt;
                for (const auto &Selector : Effect.Selectors)
                  if (Type)
                    Type =
                        objcReceiverCallResultTypeHint(Image, *Type, Selector);
                if (Type)
                  ReturnedReceiver =
                      Value{Value::Kind::Receiver, 0, {}, std::move(Type)};
              }
            }
            Clobber(&Signature);
            if (ReturnedReceiver)
              Values.emplace(key(NdVar::reg(Return.RegisterOffset, 8)),
                             std::move(*ReturnedReceiver));
            BlockHints.emplace(Op.Addr, std::move(*Runtime));
            continue;
          }
        }
        // A known import whose format contract is unresolved is still that
        // C routine. Selector-shaped register contents cannot turn it into
        // Objective-C message dispatch.
        if (Target && Target->Name != "objc_msgSend" &&
            Target->Name != "objc_msgSendSuper2") {
          Clobber(nullptr);
          continue;
        }
        if (Target && Target->Selector.empty()) {
          NdVar Selector;
          Selector.Space = VnodeSpace::REG;
          Selector.Offset = TRI.IntParamRegs[1];
          Selector.Size = 8;
          auto Name = Read(Selector);
          if (Name && Name->TheKind == Value::Kind::Selector)
            Target->Selector = Name->Name;
        }
        std::optional<ObjCReceiverTypeHint> SuperInitReceiver;
        if (Target && !Target->Selector.empty()) {
          std::optional<ObjCReceiverTypeHint> Receiver;
          ObjCReceiverDeclaration Declaration;
          const auto Self = Read(NdVar::reg(TRI.IntParamRegs[0], 8));
          if (Self && Target->Name == "objc_msgSend")
            Receiver = receiver(*Self);
          if (Self && Target->Name == "objc_msgSendSuper2" &&
              Self->TheKind == Value::Kind::Frame) {
            const int64_t Base = static_cast<int64_t>(Self->Number);
            auto StoredReceiver = [&](int64_t Offset) -> std::optional<Value> {
              const auto Slot = std::pair{Offset, 8U};
              const auto Exact = State.FrameSlots.find(Slot);
              if (Exact != State.FrameSlots.end())
                return Exact->second;
              const auto Typed = State.TypedFrameSlots.find(Slot);
              return Typed != State.TypedFrameSlots.end() &&
                             State.typedFrameRangePrivate(Offset, 8)
                         ? std::optional<Value>(Typed->second)
                         : std::nullopt;
            };
            const auto DynamicValue = StoredReceiver(Base);
            const auto ClassValue = StoredReceiver(Base + 8);
            const auto Dynamic = DynamicValue ? receiver(*DynamicValue)
                                              : std::nullopt;
            const auto CurrentClass = ClassValue ? receiver(*ClassValue)
                                                 : std::nullopt;
            if (Dynamic && isObjCInitFamily(Target->Selector))
              SuperInitReceiver = Dynamic;
            // objc_msgSendSuper2 selects the declaration from current_class;
            // the receiver word affects object identity, not the call ABI.
            // It may be an untyped result even when the exact class slot is
            // independently proven.
            if (CurrentClass && CurrentClass->IsClassMethod &&
                CurrentClass->Steps.empty()) {
              Receiver = CurrentClass;
              Declaration = objcSuperSourceTypeHint(Image, Target->Selector,
                                                    *CurrentClass);
            }
          }
          if (Receiver && Target->Name == "objc_msgSend") {
            auto [It, Inserted] = ReceiverDeclarations.try_emplace(std::tuple{
                Receiver->Origin, Receiver->Address, Receiver->ClassName,
                Receiver->IsClassMethod, Receiver->Steps, Target->Selector});
            if (Inserted)
              It->second = objcReceiverSourceTypeHint(Image, Target->Selector,
                                                      *Receiver);
            Declaration = It->second;
          }
          const bool Qualified = Declaration.HasDeclaration &&
                                 !Declaration.RequiresGlobalAgreement;
          auto Signature =
              Qualified ? Declaration.Signature
                        : objcSelectorSourceTypeHint(Image, Target->Selector);
          std::optional<SourceABIValueLocation> SelectorResultUse;
          std::optional<NdTypeKind> SelectorResultTypeUse;
          std::optional<SourceCallTypeHint::SelectorArgumentTypeEvidence>
              SelectorArgumentTypeUse;
          std::optional<SourceCallTypeHint::SelectorArgumentStorageEvidence>
              SelectorArgumentStorageUse;
          if (!Signature && !Qualified)
            if (const auto Required =
                    localResultUse(Function, Index, OpIndex, TRI, Image)) {
              Signature = objcSelectorSourceTypeHintForResultUse(
                  Image, Target->Selector, Required->Location,
                  Required->TypeKind);
              if (Signature) {
                SelectorResultUse = Required->Location;
                SelectorResultTypeUse = Required->TypeKind;
              }
            }
          if (!Signature && !Qualified) {
            for (size_t Parameter = 2; Parameter < TRI.IntParamRegs.size();
                 ++Parameter) {
              const auto Argument =
                  Read(NdVar::reg(TRI.IntParamRegs[Parameter], 8));
              if (!Argument ||
                  Argument->TheKind != Value::Kind::SourceParameter)
                continue;
              SourceCallTypeHint::SelectorArgumentTypeEvidence Evidence;
              Evidence.Parameter = static_cast<unsigned>(Parameter);
              Evidence.MethodEntry = Argument->SourceMethodEntry;
              Evidence.Source = Argument->SourceLocation;
              auto Candidate = objcSelectorSourceTypeHintForArgumentTypeUse(
                  Image, Target->Selector, Evidence);
              if (!Candidate)
                continue;
              if (SelectorArgumentTypeUse) {
                Signature.reset();
                SelectorArgumentTypeUse.reset();
                break;
              }
              Signature = std::move(Candidate);
              SelectorArgumentTypeUse = Evidence;
            }
          }
          if (!Signature && !Qualified) {
            for (size_t Parameter = 2; Parameter < TRI.IntParamRegs.size();
                 ++Parameter) {
              const auto Argument =
                  Read(NdVar::reg(TRI.IntParamRegs[Parameter], 8));
              if (!Argument || Argument->TheKind != Value::Kind::Frame)
                continue;
              const int64_t Offset = static_cast<int64_t>(Argument->Number);
              SourceCallTypeHint::SelectorArgumentStorageEvidence Evidence;
              Evidence.Parameter = static_cast<unsigned>(Parameter);
              Evidence.FrameOffset = Offset;
              auto Candidate = objcSelectorSourceTypeHintForArgumentStorageUse(
                  Image, Target->Selector, Evidence);
              if (!Candidate)
                continue;
              if (SelectorArgumentStorageUse) {
                Signature.reset();
                SelectorArgumentStorageUse.reset();
                break;
              }
              Signature = std::move(Candidate);
              SelectorArgumentStorageUse = Evidence;
            }
          }
          if (Signature) {
            SourceCallTypeHint Hint;
            Hint.CallKind = Target->Name == "objc_msgSendSuper2"
                                ? SourceCallTypeHint::Kind::ObjCSuper2
                                : SourceCallTypeHint::Kind::ObjCMessage;
            Hint.Signature = std::move(*Signature);
            Hint.TargetAddress =
                V && V->TheKind == Value::Kind::Number ? V->Number : 0;
            Hint.TargetName = Target->Name;
            Hint.Selector = Target->Selector;
            Hint.SelectorReferenceAddress = Target->SelectorSlot;
            Hint.SelectorResultUse = SelectorResultUse;
            Hint.SelectorResultTypeUse = SelectorResultTypeUse;
            Hint.SelectorArgumentTypeUse = SelectorArgumentTypeUse;
            Hint.SelectorArgumentStorageUse = SelectorArgumentStorageUse;
            if (Qualified)
              Hint.Receiver = std::move(Receiver);
            BlockHints.emplace(Op.Addr, std::move(Hint));
          } else if (Target->Name == "objc_msgSend") {
            const auto Declaration =
                objcSelectorFormatDeclaration(Image, Target->Selector);
            if (Declaration) {
              const auto &Location =
                  Declaration->Signature
                      .Parameters[Declaration->FormatParameter]
                      .Location;
              auto Format =
                  Location.Kind == SourceABICarrierKind::IntegerRegister
                      ? Read(NdVar::reg(Location.RegisterOffset, 8))
                      : std::nullopt;
              std::optional<SourceCallTypeHint> Hint;
              if (Format && Format->TheKind == Value::Kind::Number)
                Hint = objcFormattedSourceCallHint(
                    Image, Target->Selector, Format->Number);
              else if (Format &&
                       Format->TheKind == Value::Kind::NumberSet) {
                std::vector<va_t> Candidates{Format->Number};
                Candidates.insert(Candidates.end(),
                                  Format->AlternativeNumbers.begin(),
                                  Format->AlternativeNumbers.end());
                Hint = objcFormattedSourceCallHint(Image, Target->Selector,
                                                   Candidates);
              }
              if (Hint) {
                Hint->TargetAddress =
                    V && V->TheKind == Value::Kind::Number ? V->Number : 0;
                Hint->SelectorReferenceAddress = Target->SelectorSlot;
                BlockHints.emplace(Op.Addr, std::move(*Hint));
              }
            }
            if (!BlockHints.count(Op.Addr) && Receiver &&
                Target->SelectorSlot && V &&
                V->TheKind == Value::Kind::Number &&
                objcSentinelReceiverValid(Image, Target->Selector, *Receiver)) {
              const auto Objects = [&]() -> std::optional<std::vector<va_t>> {
                const auto First = Read(NdVar::reg(TRI.IntParamRegs[2], 8));
                if (!First || First->TheKind != Value::Kind::Number)
                  return std::nullopt;
                // sentinel(0,1) permits firstObject itself to be nil. Do not
                // require SP, inspect a tail slot, or introduce a tail load.
                if (!First->Number)
                  return std::vector<va_t>{};
                if (!readObjCConstantString(Image, First->Number))
                  return std::nullopt;
                const auto Stack = Read(NdVar::reg(TRI.StackPointer, 8));
                if (!Stack || Stack->TheKind != Value::Kind::Frame ||
                    State.FrameEscaped)
                  return std::nullopt;
                const int64_t Base = static_cast<int64_t>(Stack->Number);
                std::vector<va_t> Result{First->Number};
                for (;;) {
                  const int64_t Offset =
                      Base + 8 * static_cast<int64_t>(Result.size() - 1);
                  const auto Slot = State.FrameSlots.find({Offset, 8U});
                  if (Slot == State.FrameSlots.end() ||
                      Slot->second.TheKind != Value::Kind::Number)
                    return std::nullopt;
                  if (!Slot->second.Number)
                    return Result; // No reads beyond the first definite nil.
                  if (Result.size() == 61 ||
                      !readObjCConstantString(Image, Slot->second.Number))
                    return std::nullopt;
                  Result.push_back(Slot->second.Number);
                }
              }();
              if (Objects)
                if (auto Hint = objcSelectorStubSentinelSourceCallHint(
                        Image, V->Number, *Receiver, *Objects))
                  BlockHints.emplace(Op.Addr, std::move(*Hint));
            }
          }
        }
        // Only a bound Darwin ABI establishes which ordinary physical views
        // survive. A returning call must restore SP; typed frame spills below
        // every frame address visible to an unknown convention remain private.
        const auto Bound = BlockHints.find(Op.Addr);
        std::optional<ObjCReceiverTypeHint> ReturnedReceiver;
        if (Bound != BlockHints.end() &&
            Bound->second.CallKind == SourceCallTypeHint::Kind::ObjCMessage &&
            Bound->second.Receiver)
          ReturnedReceiver = objcReceiverCallResultTypeHint(
              Image, *Bound->second.Receiver, Bound->second.Selector);
        if (Bound != BlockHints.end() && SuperInitReceiver &&
            Bound->second.CallKind == SourceCallTypeHint::Kind::ObjCSuper2 &&
            Bound->second.Signature.ReturnType &&
            Bound->second.Signature.ReturnType->Kind == NdTypeKind::Ptr)
          ReturnedReceiver = std::move(SuperInitReceiver);
        const bool AuthenticatedMessageDispatch =
            Target && (Target->Name == "objc_msgSend" ||
                       Target->Name == "objc_msgSendSuper2");
        Clobber(Bound != BlockHints.end() ? &Bound->second.Signature : nullptr,
                AuthenticatedMessageDispatch);
        if (ReturnedReceiver) {
          const auto &Location = Bound->second.Signature.ReturnLocation;
          if (Location.Kind == SourceABICarrierKind::IntegerRegister &&
              Location.ValueBytes == 8)
            Values.emplace(
                key(NdVar::reg(Location.RegisterOffset, 8)),
                Value{
                    Value::Kind::Receiver, 0, {}, std::move(ReturnedReceiver)});
        }
        continue;
      }
      const bool PlainMemory =
          Op.MemoryOrdering == NdMemoryOrdering::None &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default;
      if (Op.Output.Size && CopiedBlockInput(Op.Output)) {
        const auto Overwritten = Read(Op.Output);
        if (Op.Output.Size != 8 || !Overwritten || !Overwritten->Block)
          State.forgetCopiedBlocks();
      }
      bool UsesCopiedBlock = false;
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        UsesCopiedBlock |= CopiedBlockInput(Op.Inputs[I]);
      const bool KeepsBlockIdentity =
          Op.Output.Size == 8 &&
          ((Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
            Op.Inputs[0].Size == 8) ||
           ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
            Op.NumInputs == 2 && Op.Inputs[0].Size == 8 &&
            Op.Inputs[1].Size == 8));
      const bool ReadsBlockField = Op.Opcode == NdOp::LOAD &&
                                   Op.NumInputs == 1 &&
                                   Op.Inputs[0].Size == 8 && PlainMemory;
      if (UsesCopiedBlock && !KeepsBlockIdentity && !ReadsBlockField)
        State.forgetCopiedBlocks();
      if (Op.Opcode == NdOp::STORE) {
        if (Op.NumInputs == 2 && State.mayBeFrame(Op.Inputs[1])) {
          State.escapeTypedFrameFrom(Read(Op.Inputs[1]));
          State.escapeFrame();
        }
        const auto Address = Op.NumInputs == 2 && PlainMemory
                                 ? Read(Op.Inputs[0])
                                 : std::nullopt;
        const auto Stack = Read(NdVar::reg(TRI.StackPointer, 8));
        const bool ImageStore =
            Address && Address->TheKind == Value::Kind::Number &&
            isFileBackedWritableImageRange(Image, Address->Number,
                                           Op.Inputs[1].Size);
        if (!ImageStore &&
            (!Address || Address->TheKind != Value::Kind::Frame ||
             State.FrameEscaped))
          State.forgetCopiedBlocks();
        if (!Address || Address->TheKind != Value::Kind::Frame || !Stack ||
            Stack->TheKind != Value::Kind::Frame) {
          if (!ImageStore) {
            State.FrameSlots.clear();
            State.escapeTypedFrame();
          }
        } else {
          const auto Offset = static_cast<int64_t>(Address->Number);
          const auto Size = Op.Inputs[1].Size;
          State.invalidateTypedFrameRange(Offset, Size);
          const auto Stored = Read(Op.Inputs[1]);
          if (Stored && Size == 8 &&
              (Stored->TheKind == Value::Kind::Receiver ||
               Stored->TheKind == Value::Kind::SourceParameter) &&
              Offset >= static_cast<int64_t>(Stack->Number) &&
              Offset <= -static_cast<int64_t>(Size) &&
              State.typedFrameRangePrivate(Offset, Size))
            State.TypedFrameSlots[{Offset, Size}] = *Stored;
          if (State.TypedFrameSlots.size() > 4096)
            return std::nullopt;
          if (State.FrameEscaped)
            continue;
          State.invalidateFrameRange(Offset, Size);
          if (Stored && Size && Size <= 8 &&
              Offset >= static_cast<int64_t>(Stack->Number) &&
              Offset <= -static_cast<int64_t>(Size))
            State.FrameSlots[{Offset, Size}] = *Stored;
        }
        if (State.FrameSlots.size() > 4096 ||
            State.TypedFrameSlots.size() > 4096)
          return std::nullopt;
        continue;
      }
      if (Op.Opcode == NdOp::ATOMIC_XCHG || Op.Opcode == NdOp::ATOMIC_ADD ||
          Op.Opcode == NdOp::ATOMIC_CMPXCHG) {
        State.FrameSlots.clear();
        State.escapeTypedFrame();
        State.forgetCopiedBlocks();
        for (unsigned I = 1; I < Op.NumInputs; ++I)
          if (State.mayBeFrame(Op.Inputs[I]))
            State.escapeFrame();
      }
      bool FrameDerived = false;
      if (Op.Opcode != NdOp::LOAD)
        for (unsigned I = 0; I < Op.NumInputs; ++I)
          FrameDerived |= State.mayBeFrame(Op.Inputs[I]);
      std::optional<Value> Out;
      std::optional<Value> LowImageBytes;
      // SIMD scalar loads can explicitly zero the upper vector lane. Keep
      // only the unchanged low eight-byte image recipe, not a 16-byte value
      // or a pointer fact manufactured by a widening conversion.
      if (Op.Opcode == NdOp::INT_ZEXT && Op.NumInputs == 1 &&
          Op.Inputs[0].Size == 8 && Op.Output.Size == 16) {
        const auto Input = Read(Op.Inputs[0]);
        if (Input && Input->TheKind == Value::Kind::ImageBytes)
          LowImageBytes = Input;
      }
      if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1)
        Out = Read(Op.Inputs[0]);
      else if (Op.Opcode == NdOp::SELECT && Op.NumInputs == 3 &&
               Op.Output.Size == Op.Inputs[1].Size &&
               Op.Output.Size == Op.Inputs[2].Size) {
        const auto TrueValue = Read(Op.Inputs[1]);
        const auto FalseValue = Read(Op.Inputs[2]);
        if (TrueValue && FalseValue)
          Out = mergeNumberCandidates(*TrueValue, *FalseValue);
      } else if ((Op.Opcode == NdOp::INT_ZEXT || Op.Opcode == NdOp::INT_SEXT) &&
                 Op.NumInputs == 1 && Op.Inputs[0].Size == 4 &&
                 Op.Output.Size == 8) {
        const auto Input = Read(Op.Inputs[0]);
        // Validated runtime field offsets are bounded by the instance layout.
        // Preserve the identity only for the exact 32-bit offset carrier.
        if (Input && Input->TheKind == Value::Kind::IvarOffset) {
          const auto Ref = Image.ObjCSourceReferences.find(Input->Number);
          if (Ref != Image.ObjCSourceReferences.end() && Ref->second.Size == 4)
            Out = Input;
        }
      } else if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
                 Op.NumInputs == 2) {
        auto A = Read(Op.Inputs[0]);
        auto B = Read(Op.Inputs[1]);
        // LowIR uses a narrow unsigned immediate for ordinary SP adjustments
        // (for example AArch64 SUB Xsp, #96 carries a four-byte constant).
        // Preserve the full pointer lane; the bounded numeric offset need not
        // itself occupy an eight-byte carrier.
        if (A && B && Op.Output.Size == 8 &&
            ((A->TheKind == Value::Kind::Frame && Op.Inputs[0].Size == 8 &&
              B->TheKind == Value::Kind::Number &&
              fitsUnsignedValue(B->Number, Op.Inputs[1].Size)) ||
             (Op.Opcode == NdOp::INT_ADD && B->TheKind == Value::Kind::Frame &&
              Op.Inputs[1].Size == 8 && A->TheKind == Value::Kind::Number &&
              fitsUnsignedValue(A->Number, Op.Inputs[0].Size)))) {
          if (B->TheKind == Value::Kind::Frame)
            std::swap(A, B);
          Out = adjustedFrame(*A, B->Number, Op.Opcode == NdOp::INT_SUB);
        } else if (A && B && A->TheKind == Value::Kind::Number &&
                   B->TheKind == Value::Kind::Number)
          Out = Value{Value::Kind::Number,
                      Op.Opcode == NdOp::INT_ADD ? A->Number + B->Number
                                                 : A->Number - B->Number,
                      {}};
        else if (A && B && Op.Output.Size == 8 && Op.Inputs[0].Size == 8 &&
                 Op.Inputs[1].Size == 8 &&
                 A->TheKind == Value::Kind::CopiedBlock && A->Block &&
                 B->TheKind == Value::Kind::Number) {
          Out = adjustedFrame(*A, B->Number, Op.Opcode == NdOp::INT_SUB);
        } else if (A && B && Op.Opcode == NdOp::INT_ADD &&
                   Op.Output.Size == 8 && Op.Inputs[0].Size == 8 &&
                   Op.Inputs[1].Size == 8) {
          if (B->TheKind == Value::Kind::Receiver)
            std::swap(A, B);
          const auto Base = receiver(*A);
          const auto Field =
              !Base ? std::nullopt
              : B->TheKind == Value::Kind::Number
                  ? objcReceiverFieldTypeHint(Image, *Base, B->Number)
              : B->TheKind == Value::Kind::IvarOffset
                  ? objcReceiverIvarTypeHint(Image, *Base, B->Number)
                  : std::nullopt;
          if (Field)
            Out = Value{Value::Kind::FieldAddress, 0, {}, *Field};
        }
      } else if (Op.Opcode == NdOp::LOAD && Op.NumInputs == 1) {
        auto Address = Read(Op.Inputs[0]);
        if (Address && Address->TheKind == Value::Kind::CopiedBlock &&
            Address->Block && Address->Number == 16 && PlainMemory &&
            Op.Output.Size == 8) {
          Out = *Address;
          Out->TheKind = Value::Kind::BlockInvoke;
          Out->Number = 0;
        } else if (Address && Address->TheKind == Value::Kind::Frame &&
                   PlainMemory) {
          const auto Slot = std::pair{static_cast<int64_t>(Address->Number),
                                      unsigned(Op.Output.Size)};
          const auto Typed = State.TypedFrameSlots.find(Slot);
          if (Typed != State.TypedFrameSlots.end() &&
              State.typedFrameRangePrivate(Slot.first, Slot.second))
            Out = Typed->second;
          else if (!State.FrameEscaped) {
            const auto Found = State.FrameSlots.find(Slot);
            if (Found != State.FrameSlots.end())
              Out = Found->second;
          }
        } else if (Address && Address->TheKind == Value::Kind::FieldAddress &&
                   Op.Output.Size == 8 && Address->Object)
          Out = Value{Value::Kind::Receiver, 0, {}, Address->Object};
        else if (Address && Address->TheKind == Value::Kind::Receiver &&
                 Op.Output.Size == 8) {
          const auto Field =
              objcReceiverFieldTypeHint(Image, *Address->Object, 0);
          if (Field)
            Out = Value{Value::Kind::Receiver, 0, {}, *Field};
        } else if (Address && Address->TheKind == Value::Kind::Number) {
          auto Ref = Image.ObjCSourceReferences.find(Address->Number);
          if (Ref != Image.ObjCSourceReferences.end() &&
              Ref->second.Address == Address->Number &&
              Ref->second.TheKind == ObjCSourceReference::Kind::IvarOffset &&
              (Ref->second.Size == 4 || Ref->second.Size == 8) &&
              Op.Output.Size == Ref->second.Size)
            Out = Value{Value::Kind::IvarOffset, Address->Number, {}};
          else if (Op.Output.Size == 8) {
            if (Ref != Image.ObjCSourceReferences.end() &&
                Ref->second.TheKind == ObjCSourceReference::Kind::Selector &&
                Ref->second.Size == 8)
              Out = Value{Value::Kind::Selector, 0, Ref->second.Name};
            else if (Ref != Image.ObjCSourceReferences.end() &&
                     Ref->second.TheKind == ObjCSourceReference::Kind::Class &&
                     Ref->second.Size == 8 &&
                     Ref->second.Address == Address->Number &&
                     !Ref->second.Name.empty())
              Out = Value{Value::Kind::Receiver,
                          0,
                          {},
                          ObjCReceiverTypeHint{
                              ObjCReceiverTypeHint::OriginKind::ClassReference,
                              Address->Number, Ref->second.Name, true}};
            else if (auto Name = importAt(Image, Address->Number);
                     !Name.empty())
              Out =
                  Value{Value::Kind::Import, Address->Number, std::move(Name)};
            else if (const auto Data =
                         darwinRuntimeGlobalAddressHint(Image, Address->Number);
                     Data && Data->TargetName == "_NSConcreteStackBlock")
              Out = Value{Value::Kind::BlockIsa, Address->Number};
          }
          if (!Out && PlainMemory && Op.Output.Size && Op.Output.Size <= 8)
            Out = Value{Value::Kind::ImageBytes, Address->Number};
        }
      }
      if (!Op.Output.Size)
        continue;
      if (UsesCopiedBlock && KeepsBlockIdentity && (!Out || !Out->Block))
        State.forgetCopiedBlocks();
      if (!State.writeFrameBytes(Op.Output, FrameDerived))
        return std::nullopt;
      // Kill all overlapping physical aliases, not just the queried width.
      for (auto It = Values.begin(); It != Values.end();)
        if (std::get<0>(It->first) == Op.Output.Space &&
            std::get<1>(It->first) < Op.Output.Offset + Op.Output.Size &&
            Op.Output.Offset < std::get<1>(It->first) + std::get<2>(It->first))
          It = Values.erase(It);
        else
          ++It;
      if (Out && Op.Output.Size <= 8) {
        if (Out->TheKind == Value::Kind::Number) {
          if (Op.Output.Size < 8)
            Out->Number &= (uint64_t(1) << (Op.Output.Size * 8)) - 1;
          Values.emplace(key(Op.Output), std::move(*Out));
        } else if (Op.Output.Size == 8 ||
                   (Out->TheKind == Value::Kind::IvarOffset &&
                    Op.Output.Size == 4 &&
                    Image.ObjCSourceReferences.at(Out->Number).Size == 4))
          Values.emplace(key(Op.Output), std::move(*Out));
        if (Values.size() > 4096)
          return std::nullopt;
      }
      if (LowImageBytes) {
        auto Prefix = Op.Output;
        Prefix.Size = 8;
        Values.emplace(key(Prefix), std::move(*LowImageBytes));
        if (Values.size() > 4096)
          return std::nullopt;
      }
      if (Op.Output.isReg() && Op.Output.Offset < TRI.StackPointer + 8 &&
          TRI.StackPointer < Op.Output.Offset + Op.Output.Size) {
        const auto Stack = Read(NdVar::reg(TRI.StackPointer, 8));
        if (!Stack || Stack->TheKind != Value::Kind::Frame) {
          State.FrameSlots.clear();
          State.escapeTypedFrame();
        } else {
          const auto Begin = static_cast<int64_t>(Stack->Number);
          for (auto It = State.FrameSlots.begin();
               It != State.FrameSlots.end();)
            if (It->first.first < Begin)
              It = State.FrameSlots.erase(It);
            else
              ++It;
          for (auto It = State.TypedFrameSlots.begin();
               It != State.TypedFrameSlots.end();)
            if (It->first.first < Begin)
              It = State.TypedFrameSlots.erase(It);
            else
              ++It;
        }
      }
    }
    for (auto It = Values.begin(); It != Values.end();)
      if (std::get<0>(It->first) == VnodeSpace::TEMP)
        It = Values.erase(It);
      else
        ++It;
    for (auto It = State.FrameBytes.begin(); It != State.FrameBytes.end();)
      if (It->first == VnodeSpace::TEMP)
        It = State.FrameBytes.erase(It);
      else
        ++It;
    return State;
  };
  std::vector<std::optional<Facts>> Exits(Count);
  std::vector<Hints> Bindings(Count);
  std::deque<size_t> Work;
  for (size_t I = 0; I < Count; ++I)
    if (Roots[I]) {
      Work.push_back(I);
      Queued[I] = true;
    }
  size_t Remaining = 1048576;
  while (!Work.empty()) {
    const auto Index = Work.front();
    Work.pop_front();
    Queued[Index] = false;
    const auto Cost = std::max(size_t(1), Function.Blocks[Index].Ops.size());
    if (Cost > Remaining)
      return {}; // Never publish a partially converged proof.
    Remaining -= Cost;
    Facts Incoming;
    bool Initialized = Roots[Index];
    if (Function.Blocks[Index].StartAddr == Function.Entry &&
        !ExceptionalRoots[Index])
      Incoming = EntryFacts;
    for (size_t Parent : Parents[Index]) {
      if (!Exits[Parent])
        continue;
      if (!Initialized) {
        Incoming = *Exits[Parent];
        Initialized = true;
      } else {
        Incoming.merge(*Exits[Parent]);
      }
    }
    if (!Initialized)
      continue;
    if (Incoming.FrameBytes.size() > 4096)
      return {};
    Bindings[Index].clear();
    auto Out = Transfer(Index, std::move(Incoming), Bindings[Index]);
    if (!Out)
      return {};
    if (Exits[Index] && *Exits[Index] == *Out)
      continue;
    Exits[Index] = std::move(Out);
    for (size_t Child : Children[Index])
      if (!Queued[Child]) {
        Queued[Child] = true;
        Work.push_back(Child);
      }
  }
  for (auto &Block : Bindings)
    for (auto &[Address, Hint] : Block)
      if (CallOccurrences[Address] == 1)
        Result.emplace(Address, std::move(Hint));
  return Result;
}
} // namespace neverd
