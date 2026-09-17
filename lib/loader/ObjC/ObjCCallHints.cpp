#include "neverd/loader/ObjC/ObjCCallHints.h"

#include "../MachO/DarwinRuntimeImport.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/AArch64Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/DarwinRuntimeCalls.h"
#include "neverd/loader/ObjC/ObjCFormattedCalls.h"
#include "neverd/loader/ObjC/ObjCSourceDeclarations.h"
#include "neverd/loader/Swift/SwiftRuntimeCalls.h"
#include "neverd/loader/Swift/SwiftStringCalls.h"
#include "neverd/object/SectionNames.h"

#include "llvm/Support/Endian.h"

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
};

std::optional<Dispatch> veneer(const BinaryImage &Image, va_t Address) {
  if (Image.Arch == Arch::X64) {
    const auto *Bytes = code(Image, Address, 6);
    if (!Bytes || Bytes[0] != 0xff || Bytes[1] != 0x25)
      return std::nullopt;
    auto Slot = addSigned(Address + 6,
                          int32_t(llvm::support::endian::read32le(Bytes + 2)));
    const auto Name = Slot ? importAt(Image, *Slot) : std::string();
    return Name.empty() ? std::nullopt
                        : std::optional<Dispatch>({Name, {}, 0, *Slot});
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
  Result.ImportSlot = *Slot;
  // A selector-loading veneer only has a proven ABI for message dispatch.
  if (Result.SelectorSlot && Result.Name != "objc_msgSend" &&
      Result.Name != "objc_msgSendSuper2")
    return std::nullopt;
  return Result.Name.empty() ? std::nullopt
                             : std::optional<Dispatch>(std::move(Result));
}

struct Value {
  enum class Kind {
    Number,
    Selector,
    Import,
    Receiver,
    IvarOffset,
    FieldAddress,
    Frame
  };
  Kind TheKind = Kind::Number;
  uint64_t Number = 0;
  std::string Name;
  std::optional<ObjCReceiverTypeHint> Object;
  bool operator==(const Value &Other) const {
    return std::tie(TheKind, Number, Name, Object) ==
           std::tie(Other.TheKind, Other.Number, Other.Name, Other.Object);
  }
};
using Key = std::tuple<VnodeSpace, uint64_t, uint16_t>;
Key key(const NdVar &V) { return {V.Space, V.Offset, V.Size}; }

constexpr int64_t FrameOffsetLimit = 1048576;

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

  void invalidateFrameRange(int64_t Offset, unsigned Size) {
    for (auto It = FrameSlots.begin(); It != FrameSlots.end();)
      if (It->first.first < Offset + Size &&
          Offset < It->first.first + It->first.second)
        It = FrameSlots.erase(It);
      else
        ++It;
  }

  void merge(const CallFacts &Other) {
    for (auto It = Values.begin(); It != Values.end();) {
      const auto Found = Other.Values.find(It->first);
      if (Found == Other.Values.end() || !(It->second == Found->second))
        It = Values.erase(It);
      else
        ++It;
    }
    FrameBytes.insert(Other.FrameBytes.begin(), Other.FrameBytes.end());
    if (FrameEscaped || Other.FrameEscaped) {
      escapeFrame();
      return;
    }
    for (auto It = FrameSlots.begin(); It != FrameSlots.end();) {
      const auto Found = Other.FrameSlots.find(It->first);
      if (Found == Other.FrameSlots.end() || !(It->second == Found->second))
        It = FrameSlots.erase(It);
      else
        ++It;
    }
  }
};

std::optional<ObjCReceiverTypeHint> receiver(const Value &V) {
  return V.TheKind == Value::Kind::Receiver ? V.Object : std::nullopt;
}
} // namespace

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

std::map<va_t, SourceCallTypeHint>
buildObjCSourceCallHints(const BinaryImage &Image, const LowFunc &Function) {
  std::map<va_t, SourceCallTypeHint> Result;
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64))
    return Result;
  const auto &TRI = getTargetRegInfo(Image.Arch);
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
    auto Clobber = [&](const SourceFunctionTypeHint *Signature) {
      const bool KnownABI = Signature && Signature->HasExplicitABI;
      if (!KnownABI) {
        State.escapeFrame();
      } else {
        int64_t ArgumentBytes = 0;
        const int64_t ReturnAddressBytes = Image.Arch == Arch::X64 ? 8 : 0;
        auto CheckArgument = [&](const SourceABIValueLocation &Location) {
          if (Location.Kind == SourceABICarrierKind::IntegerRegister ||
              Location.Kind == SourceABICarrierKind::FloatingRegister) {
            if (State.mayBeFrame(
                    NdVar::reg(Location.RegisterOffset, Location.ValueBytes)))
              State.escapeFrame();
          } else if (Location.Kind == SourceABICarrierKind::Stack) {
            const int64_t Begin =
                Location.EntryStackOffset - ReturnAddressBytes;
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
        if (!KnownABI || Space != VnodeSpace::REG ||
            (!(Offset == TRI.StackPointer && Size == 8) &&
             !TRI.isCallPreserved(Offset, Size)))
          It = Values.erase(It);
        else
          ++It;
      }
      for (auto It = State.FrameBytes.begin(); It != State.FrameBytes.end();)
        if (!KnownABI || It->first != VnodeSpace::REG ||
            !PreservedBytes.count(It->second))
          It = State.FrameBytes.erase(It);
        else
          ++It;
    };
    for (const auto &Op : Block.Ops) {
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
        std::optional<Dispatch> Target;
        auto V = Op.NumInputs ? Read(Op.Inputs[0]) : std::nullopt;
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
        if (Target && !Target->Selector.empty()) {
          std::optional<ObjCReceiverTypeHint> Receiver;
          ObjCReceiverDeclaration Declaration;
          const auto Self = Read(NdVar::reg(TRI.IntParamRegs[0], 8));
          if (Self && Target->Name == "objc_msgSend")
            Receiver = receiver(*Self);
          if (Receiver) {
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
              auto Hint = Format && Format->TheKind == Value::Kind::Number
                              ? objcFormattedSourceCallHint(
                                    Image, Target->Selector, Format->Number)
                              : std::nullopt;
              if (Hint) {
                Hint->TargetAddress =
                    V && V->TheKind == Value::Kind::Number ? V->Number : 0;
                Hint->SelectorReferenceAddress = Target->SelectorSlot;
                BlockHints.emplace(Op.Addr, std::move(*Hint));
              }
            }
          }
        }
        // Only a bound Darwin ABI establishes which physical views survive.
        // Unknown calls may use another convention and invalidate every fact.
        const auto Bound = BlockHints.find(Op.Addr);
        std::optional<ObjCReceiverTypeHint> ReturnedReceiver;
        if (Bound != BlockHints.end() &&
            Bound->second.CallKind == SourceCallTypeHint::Kind::ObjCMessage &&
            Bound->second.Receiver)
          ReturnedReceiver = objcReceiverCallResultTypeHint(
              Image, *Bound->second.Receiver, Bound->second.Selector);
        Clobber(Bound != BlockHints.end() ? &Bound->second.Signature : nullptr);
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
      if (Op.Opcode == NdOp::STORE) {
        if (Op.NumInputs == 2 && State.mayBeFrame(Op.Inputs[1]))
          State.escapeFrame();
        const auto Address = Op.NumInputs == 2 && PlainMemory
                                 ? Read(Op.Inputs[0])
                                 : std::nullopt;
        const auto Stack = Read(NdVar::reg(TRI.StackPointer, 8));
        if (!Address || Address->TheKind != Value::Kind::Frame || !Stack ||
            Stack->TheKind != Value::Kind::Frame) {
          State.FrameSlots.clear();
        } else if (!State.FrameEscaped) {
          const auto Offset = static_cast<int64_t>(Address->Number);
          const auto Size = Op.Inputs[1].Size;
          State.invalidateFrameRange(Offset, Size);
          const auto Stored = Read(Op.Inputs[1]);
          if (Stored && Size && Size <= 8 &&
              Offset >= static_cast<int64_t>(Stack->Number) &&
              Offset <= -static_cast<int64_t>(Size))
            State.FrameSlots[{Offset, Size}] = *Stored;
        }
        if (State.FrameSlots.size() > 4096)
          return std::nullopt;
        continue;
      }
      if (Op.Opcode == NdOp::ATOMIC_XCHG || Op.Opcode == NdOp::ATOMIC_ADD ||
          Op.Opcode == NdOp::ATOMIC_CMPXCHG) {
        State.FrameSlots.clear();
        for (unsigned I = 1; I < Op.NumInputs; ++I)
          if (State.mayBeFrame(Op.Inputs[I]))
            State.escapeFrame();
      }
      bool FrameDerived = false;
      if (Op.Opcode != NdOp::LOAD)
        for (unsigned I = 0; I < Op.NumInputs; ++I)
          FrameDerived |= State.mayBeFrame(Op.Inputs[I]);
      std::optional<Value> Out;
      if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1)
        Out = Read(Op.Inputs[0]);
      else if ((Op.Opcode == NdOp::INT_ZEXT || Op.Opcode == NdOp::INT_SEXT) &&
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
        if (A && B && Op.Output.Size == 8 && Op.Inputs[0].Size == 8 &&
            Op.Inputs[1].Size == 8 &&
            ((A->TheKind == Value::Kind::Frame &&
              B->TheKind == Value::Kind::Number) ||
             (Op.Opcode == NdOp::INT_ADD && B->TheKind == Value::Kind::Frame &&
              A->TheKind == Value::Kind::Number))) {
          if (B->TheKind == Value::Kind::Frame)
            std::swap(A, B);
          Out = adjustedFrame(*A, B->Number, Op.Opcode == NdOp::INT_SUB);
        } else if (A && B && A->TheKind == Value::Kind::Number &&
                   B->TheKind == Value::Kind::Number)
          Out = Value{Value::Kind::Number,
                      Op.Opcode == NdOp::INT_ADD ? A->Number + B->Number
                                                 : A->Number - B->Number,
                      {}};
        else if (A && B && Op.Opcode == NdOp::INT_ADD && Op.Output.Size == 8 &&
                 Op.Inputs[0].Size == 8 && Op.Inputs[1].Size == 8) {
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
        if (Address && Address->TheKind == Value::Kind::Frame && PlainMemory &&
            !State.FrameEscaped) {
          const auto Found = State.FrameSlots.find(
              {static_cast<int64_t>(Address->Number), Op.Output.Size});
          if (Found != State.FrameSlots.end())
            Out = Found->second;
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
          }
        }
      }
      if (!Op.Output.Size)
        continue;
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
      if (Op.Output.isReg() && Op.Output.Offset < TRI.StackPointer + 8 &&
          TRI.StackPointer < Op.Output.Offset + Op.Output.Size) {
        const auto Stack = Read(NdVar::reg(TRI.StackPointer, 8));
        if (!Stack || Stack->TheKind != Value::Kind::Frame) {
          State.FrameSlots.clear();
        } else {
          const auto Begin = static_cast<int64_t>(Stack->Number);
          for (auto It = State.FrameSlots.begin();
               It != State.FrameSlots.end();)
            if (It->first.first < Begin)
              It = State.FrameSlots.erase(It);
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
