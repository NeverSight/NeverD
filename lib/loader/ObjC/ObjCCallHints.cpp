#include "neverd/loader/ObjC/ObjCCallHints.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/object/SectionNames.h"

#include "llvm/Support/Endian.h"

#include <optional>
#include <tuple>

namespace neverd {
namespace {
bool sameType(const TypeRef &A, const TypeRef &B, unsigned Depth = 0) {
  if (!A || !B || Depth > 16 || A->Kind != B->Kind || A->Size != B->Size ||
      A->IsSigned != B->IsSigned)
    return false;
  return A->Kind != NdTypeKind::Ptr ||
         sameType(A->Pointee, B->Pointee, Depth + 1);
}

bool sameSignature(const SourceFunctionTypeHint &A,
                   const SourceFunctionTypeHint &B) {
  if (!sameType(A.ReturnType, B.ReturnType) ||
      A.Parameters.size() != B.Parameters.size())
    return false;
  for (size_t I = 0; I < A.Parameters.size(); ++I)
    if (!sameType(A.Parameters[I].Type, B.Parameters[I].Type))
      return false;
  return true;
}

std::optional<SourceFunctionTypeHint>
selectorSignature(const BinaryImage &Image, llvm::StringRef Name) {
  std::optional<SourceFunctionTypeHint> Result;
  // The receiver's dynamic class is generally unknown. Every matching runtime
  // declaration must agree, including unsupported declarations; never select
  // whichever implementation happened to be visited first.
  for (const auto &Method : Image.ObjCMethods) {
    if (Method.Selector != Name)
      continue;
    if (!Method.TypeHint)
      return std::nullopt;
    std::string Diagnostic;
    auto Hint = *Method.TypeHint;
    if (!assignDarwinObjCSourceABI(Hint, Image.Arch, Diagnostic) ||
        (Result && !sameSignature(*Result, Hint)))
      return std::nullopt;
    Result = std::move(Hint);
  }
  return Result;
}

std::string runtimeName(llvm::StringRef Name) {
  Name.consume_front("_");
  return Name == "objc_msgSend" || Name == "objc_msgSendSuper2" ? Name.str()
                                                                : std::string();
}

std::string importAt(const BinaryImage &Image, va_t Slot) {
  auto It = Image.ImportPtrSlots.find(Slot);
  return It == Image.ImportPtrSlots.end() ? std::string()
                                          : runtimeName(It->second);
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
};

std::optional<Dispatch> veneer(const BinaryImage &Image, va_t Address) {
  if (Image.Arch == Arch::X64) {
    const auto *Bytes = code(Image, Address, 6);
    if (!Bytes || Bytes[0] != 0xff || Bytes[1] != 0x25)
      return std::nullopt;
    auto Slot = addSigned(Address + 6,
                          int32_t(llvm::support::endian::read32le(Bytes + 2)));
    const auto Name = Slot ? importAt(Image, *Slot) : std::string();
    return Name.empty() ? std::nullopt : std::optional<Dispatch>({Name, {}, 0});
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
    Address += 8;
    Bytes = code(Image, Address, 12);
    if (!Bytes)
      return std::nullopt;
  }
  auto Page = adrp(Word(0), Address, 16);
  auto Slot = Page ? ldrSlot(Word(1), *Page, 16) : std::nullopt;
  if (!Slot || Word(2) != 0xd61f0200u) // BR x16
    return std::nullopt;
  Result.Name = importAt(Image, *Slot);
  return Result.Name.empty() ? std::nullopt
                             : std::optional<Dispatch>(std::move(Result));
}

struct Value {
  enum class Kind { Number, Selector, Import };
  Kind TheKind = Kind::Number;
  uint64_t Number = 0;
  std::string Name;
};
using Key = std::tuple<VnodeSpace, uint64_t, uint16_t>;
Key key(const NdVar &V) { return {V.Space, V.Offset, V.Size}; }
} // namespace

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

std::map<va_t, SourceCallTypeHint>
buildObjCSourceCallHints(const BinaryImage &Image, const LowFunc &Function) {
  std::map<va_t, SourceCallTypeHint> Result;
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64))
    return Result;
  const auto &TRI = getTargetRegInfo(Image.Arch);
  for (const auto &Block : Function.Blocks) {
    std::map<Key, Value> Values;
    va_t PreviousAddress = InvalidVA;
    auto Read = [&](const NdVar &V) -> std::optional<Value> {
      if (V.Space == VnodeSpace::CONST)
        return Value{Value::Kind::Number, V.Offset, {}};
      auto It = Values.find(key(V));
      return It == Values.end() ? std::nullopt
                                : std::optional<Value>(It->second);
    };
    for (const auto &Op : Block.Ops) {
      if (Op.Addr != PreviousAddress) {
        for (auto It = Values.begin(); It != Values.end();)
          if (std::get<0>(It->first) == VnodeSpace::TEMP)
            It = Values.erase(It);
          else
            ++It;
        PreviousAddress = Op.Addr;
      }
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        std::optional<Dispatch> Target;
        auto V = Op.NumInputs ? Read(Op.Inputs[0]) : std::nullopt;
        if (V && V->TheKind == Value::Kind::Import)
          Target = Dispatch{V->Name, {}, 0};
        else if (V && V->TheKind == Value::Kind::Number) {
          if (Op.Opcode == NdOp::INDIR_CALL &&
              Op.Inputs[0].Space == VnodeSpace::CONST) {
            auto Name = importAt(Image, V->Number);
            if (!Name.empty())
              Target = Dispatch{Name, {}, 0};
          } else if (Op.Opcode == NdOp::CALL) {
            Target = veneer(Image, V->Number);
          }
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
        if (Target && !Target->Selector.empty()) {
          if (auto Signature = selectorSignature(Image, Target->Selector)) {
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
            Result.emplace(Op.Addr, std::move(Hint));
          }
        }
        // A call may overwrite any runtime reference. Requiring a fresh local
        // definition is conservative even for values kept in callee-saved regs.
        Values.clear();
        continue;
      }
      std::optional<Value> Out;
      if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1)
        Out = Read(Op.Inputs[0]);
      else if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
               Op.NumInputs == 2) {
        auto A = Read(Op.Inputs[0]);
        auto B = Read(Op.Inputs[1]);
        if (A && B && A->TheKind == Value::Kind::Number &&
            B->TheKind == Value::Kind::Number)
          Out = Value{Value::Kind::Number,
                      Op.Opcode == NdOp::INT_ADD ? A->Number + B->Number
                                                 : A->Number - B->Number,
                      {}};
      } else if (Op.Opcode == NdOp::LOAD && Op.NumInputs == 1 &&
                 Op.Output.Size == 8) {
        auto Address = Read(Op.Inputs[0]);
        if (Address && Address->TheKind == Value::Kind::Number) {
          auto Ref = Image.ObjCSourceReferences.find(Address->Number);
          if (Ref != Image.ObjCSourceReferences.end() &&
              Ref->second.TheKind == ObjCSourceReference::Kind::Selector &&
              Ref->second.Size == 8)
            Out = Value{Value::Kind::Selector, 0, Ref->second.Name};
          else if (auto Name = importAt(Image, Address->Number); !Name.empty())
            Out = Value{Value::Kind::Import, 0, std::move(Name)};
        }
      }
      if (!Op.Output.Size)
        continue;
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
        } else if (Op.Output.Size == 8)
          Values.emplace(key(Op.Output), std::move(*Out));
      }
    }
  }
  return Result;
}
} // namespace neverd
