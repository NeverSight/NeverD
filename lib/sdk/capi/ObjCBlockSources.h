#ifndef NEVERD_SDK_CAPI_OBJCBLOCKSOURCES_H
#define NEVERD_SDK_CAPI_OBJCBLOCKSOURCES_H

#include "ObjCSourceBindings.h"

#include "neverd/loader/ObjC/ObjCBlocks.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"

#include <limits>
#include <stdexcept>

namespace neverd::sdk {

struct ObjCBlockAddressBinding {
  SourceCallTypeHint::Kind Kind = SourceCallTypeHint::Kind::NativeAddress;
  va_t Address = 0;
  std::string Name;
};
struct ObjCStackBlockSource {
  int64_t FrameOffset = 0;
  va_t InvokeEntry = 0;
  ObjCBlockDescriptor Descriptor;
  std::set<uint64_t> InitializedCaptures;
  std::map<const HighExpr *, ObjCBlockAddressBinding> References;
};
struct ObjCBlockSourcePlan {
  std::map<va_t, ObjCBlockLiteral> Globals;
  std::map<va_t, ObjCBlockDescriptor> Descriptors;
  std::map<va_t, SourceFunctionTypeHint> InvokeHints;
  std::map<va_t, std::vector<ObjCStackBlockSource>> StackBlocks;
  std::map<va_t, std::string> Rejections;
};
struct ObjCBlockSourceBindingResult {
  HighFunc Function;
  std::string Limitation;
  std::set<va_t> Dependencies, Descriptors, Literals;
};

/// Import evidence belongs to one export of an unchanged loaded image. Share
/// it across functions and pipeline rounds, but keep expression and frame
/// proofs local to each result. Construct a new context after image changes.
struct ObjCBlockSourceContext {
  const BinaryImage &Image;
  const ImportStorageSlotCollection Imports;

  explicit ObjCBlockSourceContext(const BinaryImage &I)
      : Image(I), Imports(I.collectImportStorageSlots()) {}
};

namespace objc_block_source_detail {
using Identity = objc_projection_detail::LocalIdentity;
using CallKind = SourceCallTypeHint::Kind;
struct Invalid : std::runtime_error {
  using std::runtime_error::runtime_error;
};
struct Value {
  enum Kind { Scalar, Number, Frame, Context, Invoke, Isa } K = Scalar;
  int64_t Offset = 0;
  uint64_t Bits = 0;
  std::string Name;
  const HighExpr *Producer = nullptr;
};
inline bool scalarWidth(unsigned Bytes) {
  return Bytes == 1 || Bytes == 2 || Bytes == 4 || Bytes == 8;
}
inline uint64_t mask(unsigned Bytes) {
  return Bytes == 8 ? UINT64_MAX : (UINT64_C(1) << (Bytes * 8)) - 1;
}
inline std::string isaName(llvm::StringRef Name) {
  if (Name.starts_with("__"))
    Name = Name.drop_front();
  return Name == "_NSConcreteStackBlock" || Name == "_NSConcreteGlobalBlock"
             ? Name.str()
             : std::string{};
}
inline bool plain(const HighExpr &E) {
  return E.MemoryOrdering == NdMemoryOrdering::None &&
         E.MemoryAddressSpace == NdMemoryAddressSpace::Default;
}
inline bool frameRange(const HighFunc &F, int64_t Offset, unsigned Bytes) {
  return F.FrameSize <= (1u << 24) && F.FrameHeadroom <= (1u << 24) && Bytes &&
         Offset >= -static_cast<int64_t>(F.FrameSize) &&
         Offset <= static_cast<int64_t>(F.FrameHeadroom) &&
         Bytes <= static_cast<int64_t>(F.FrameHeadroom) - Offset;
}
inline bool validDescriptor(const ObjCBlockDescriptor &D) {
  if (!D.InvokeTypeHint || !D.Limitations.empty() || D.CopyHelper ||
      D.DisposeHelper || D.LiteralSize < 32 || D.LiteralSize > (1u << 20))
    return false;
  return std::all_of(D.Captures.begin(), D.Captures.end(), [](const auto &R) {
    return R.StorageKind == ObjCBlockCaptureRange::Kind::NonObjectBytes;
  });
}
inline bool sameDescriptor(const ObjCBlockDescriptor &A,
                           const ObjCBlockDescriptor &B) {
  return A.LiteralSize == B.LiteralSize && A.Signature == B.Signature &&
         A.LayoutValue == B.LayoutValue && A.LayoutBytes == B.LayoutBytes &&
         ((A.Flags ^ B.Flags) & (UINT32_C(1) << 31)) == 0 && A.InvokeTypeHint &&
         B.InvokeTypeHint &&
         objc_projection_detail::sameHint(*A.InvokeTypeHint, *B.InvokeTypeHint);
}

/// The value evaluator is deliberately limited to proven pointer copies,
/// byte offsets and private frame spills. Scalar expressions cannot acquire
/// a context/frame identity merely because they contain an equal integer.
class Values {
  const BinaryImage &Image;
  const HighFunc &Function;
  std::optional<size_t> ContextParameter;
  const ImportStorageSlotCollection &Imports;
  size_t EvaluationBudget = 1000000;
  std::set<int64_t> FrameIdentityBytes;

public:
  std::map<Identity, Value> Locals;
  std::map<std::pair<int64_t, unsigned>, Value> FrameValues;
  std::function<Value(const HighExpr &, const std::vector<Value> &)> Call;
  std::function<Value(const Value &, unsigned)> ContextRead;
  Values(const ObjCBlockSourceContext &Source, const HighFunc &F,
         std::optional<size_t> Context = std::nullopt)
      : Image(Source.Image), Function(F), ContextParameter(Context),
        Imports(Source.Imports) {}
  Values(ObjCBlockSourceContext &&, const HighFunc &,
         std::optional<size_t> = std::nullopt) = delete;
  Value eval(const ExprPtr &E, unsigned Depth = 0) {
    if (!E || Depth > 128 || !E->Type || !EvaluationBudget--)
      throw Invalid("block source has an incomplete or excessive expression");
    const unsigned Bytes = E->Type->Size;
    if (E->Kind == ExprKind::Const && scalarWidth(Bytes))
      return {Value::Number, 0, E->ConstVal & mask(Bytes), {}, E.get()};
    if (E->Kind == ExprKind::Var) {
      if (E->Var.Kind == MedVar::Param && E->Var.RenameTag < 0)
        return ContextParameter && E->Var.Id >= 0 &&
                       static_cast<size_t>(E->Var.Id) == *ContextParameter
                   ? Value{Value::Context, 0, 0, {}, E.get()}
                   : Value{};
      if (E->Var.Kind == MedVar::Reg && E->Var.RenameTag < 0 &&
          E->Var.SSAVer == 0 &&
          E->Var.RegOff == getTargetRegInfo(Image.Arch).StackPointer &&
          Bytes == 8)
        return {Value::Frame, 0, 0, {}, E.get()};
      auto Found = Locals.find(objc_projection_detail::localIdentity(E->Var));
      return Found == Locals.end() ? Value{} : Found->second;
    }
    std::vector<Value> Inputs;
    for (const auto &Input : E->Operands)
      Inputs.push_back(eval(Input, Depth + 1));
    if (E->Kind == ExprKind::Call)
      return Call ? Call(*E, Inputs) : Value{};
    if ((E->Kind == ExprKind::Cast || E->Kind == ExprKind::BitCast ||
         E->Kind == ExprKind::UnaryOp) &&
        Inputs.size() == 1 &&
        (E->Kind != ExprKind::UnaryOp || E->Op == NdOp::INT_ZEXT ||
         E->Op == NdOp::INT_SEXT)) {
      auto V = Inputs[0];
      if (V.K == Value::Number && scalarWidth(Bytes) &&
          scalarWidth(E->Operands[0]->Type->Size)) {
        const unsigned From = E->Operands[0]->Type->Size;
        const bool Signed = E->Kind == ExprKind::UnaryOp
                                ? E->Op == NdOp::INT_SEXT
                                : E->Operands[0]->Type->IsSigned;
        if (Signed && Bytes > From &&
            (V.Bits & (UINT64_C(1) << (From * 8 - 1))))
          V.Bits |= ~mask(From);
        V.Bits &= mask(Bytes);
        V.Producer = E.get();
        return V;
      }
      if (Bytes == 8 && E->Operands[0]->Type->Size == 8)
        return V;
      if (V.K != Value::Scalar)
        throw Invalid("block pointer is consumed through a partial value");
      return {};
    }
    if (E->Kind == ExprKind::BinOp && Inputs.size() == 2 &&
        (E->Op == NdOp::INT_ADD || E->Op == NdOp::INT_SUB)) {
      auto A = Inputs[0], B = Inputs[1];
      if (E->Op == NdOp::INT_ADD && A.K == Value::Number)
        std::swap(A, B);
      if ((A.K == Value::Frame || A.K == Value::Context) &&
          B.K == Value::Number && Bytes == 8) {
        const int64_t Delta = static_cast<int64_t>(B.Bits);
        if (Delta < -(1 << 24) || Delta > (1 << 24) || A.Offset < -(1 << 24) ||
            A.Offset > (1 << 24))
          throw Invalid("block byte offset exceeds its proof bound");
        A.Offset += E->Op == NdOp::INT_SUB ? -Delta : Delta;
        return A;
      }
      if (A.K == Value::Number && B.K == Value::Number && scalarWidth(Bytes))
        return {Value::Number,
                0,
                (E->Op == NdOp::INT_ADD ? A.Bits + B.Bits : A.Bits - B.Bits) &
                    mask(Bytes),
                {},
                E.get()};
    }
    if (E->Kind == ExprKind::Load && Inputs.size() == 1 && plain(*E)) {
      const auto Address = Inputs[0];
      if (Address.K == Value::Frame) {
        auto Found = FrameValues.find({Address.Offset, Bytes});
        if (Found != FrameValues.end())
          return Found->second;
        auto Identity = FrameIdentityBytes.lower_bound(Address.Offset);
        if (Identity != FrameIdentityBytes.end() &&
            *Identity < Address.Offset + Bytes)
          throw Invalid("partial frame read loses block address provenance");
        return {};
      }
      if (Address.K == Value::Context)
        return ContextRead
                   ? ContextRead(Address, Bytes)
                   : throw Invalid(
                         "block context memory read is not established");
      if (Address.K == Value::Number && Bytes == 8) {
        auto Found = Imports.Slots.find(Address.Bits);
        if (Found != Imports.Slots.end() &&
            !Imports.Conflicts.count(Address.Bits) && !Found->second.Addend) {
          auto Name = isaName(Found->second.Name);
          if (!Name.empty())
            return {Value::Isa, 0, Address.Bits, std::move(Name), E.get()};
        }
      }
    }
    for (const auto &V : Inputs)
      if (V.K == Value::Frame || V.K == Value::Context || V.K == Value::Invoke)
        throw Invalid(
            "block pointer escapes its proven byte-address operations");
    return {};
  }
  void assign(const HighStmt &S) {
    if (!S.Dst || S.Dst->Kind != ExprKind::Var)
      throw Invalid("block proof requires a direct SSA assignment");
    Locals[objc_projection_detail::localIdentity(S.Dst->Var)] = eval(S.Val);
  }
  void storeFrame(const Value &Address, unsigned Bytes, const Value &V) {
    if (Address.K != Value::Frame ||
        !frameRange(Function, Address.Offset, Bytes))
      throw Invalid("block frame store exceeds its recovered frame storage");
    for (auto It = FrameValues.begin(); It != FrameValues.end();) {
      const auto [Offset, Size] = It->first;
      if (Offset < Address.Offset + Bytes && Address.Offset < Offset + Size)
        It = FrameValues.erase(It);
      else
        ++It;
    }
    if (Bytes > EvaluationBudget)
      throw Invalid("block frame proof exceeds its byte budget");
    EvaluationBudget -= Bytes;
    const bool Identity = V.K == Value::Frame || V.K == Value::Context ||
                          V.K == Value::Invoke || V.K == Value::Isa;
    for (unsigned I = 0; I < Bytes; ++I) {
      const int64_t Byte = Address.Offset + I;
      if (Identity)
        FrameIdentityBytes.insert(Byte);
      else
        FrameIdentityBytes.erase(Byte);
    }
    FrameValues[{Address.Offset, Bytes}] = V;
  }
};

/// Prove that a context pointer is neither returned nor exposed to memory or
/// unknown callees. A descriptor-backed invoke may read only known capture
/// bytes. Forwarding consumers may read only the invoke pointer at byte 16.
inline bool noEscape(const ObjCBlockSourceContext &Source,
                     const std::map<va_t, const HighFunc *> &Functions,
                     va_t Entry, size_t Parameter,
                     const std::set<uint64_t> *Initialized,
                     std::set<std::pair<va_t, size_t>> &Active,
                     std::string &Reason) {
  try {
    if (Active.size() >= 16 || !Active.insert({Entry, Parameter}).second)
      throw Invalid("block consumer recursion is not established");
    struct Pop {
      decltype(Active) A;
      std::pair<va_t, size_t> Key;
      ~Pop() { A.erase(Key); }
    } Pop{Active, {Entry, Parameter}};
    auto Found = Functions.find(Entry);
    if (Found == Functions.end() || Parameter >= Found->second->Params.size())
      throw Invalid("block consumer has no recovered parameter binding");
    const auto &F = *Found->second;
    Values State(Source, F, Parameter);
    State.ContextRead = [&](const Value &Address, unsigned Bytes) -> Value {
      if (!Initialized && Address.Offset == 16 && Bytes == 8)
        return {Value::Invoke};
      if (!Initialized || Address.Offset < 32 || !scalarWidth(Bytes))
        throw Invalid("block invoke reads an unknown context field");
      for (unsigned I = 0; I < Bytes; ++I)
        if (!Initialized->count(static_cast<uint64_t>(Address.Offset) + I))
          throw Invalid("block invoke reads uninitialized capture storage");
      return {};
    };
    State.Call = [&](const HighExpr &E,
                     const std::vector<Value> &Arguments) -> Value {
      const auto &B = E.SourceCallHint;
      if (B && B->CallKind == CallKind::BlockInvoke &&
          Arguments.size() == B->Signature.Parameters.size() &&
          !Arguments.empty() && Arguments[0].K == Value::Context &&
          Arguments[0].Offset == 0) {
        for (size_t I = 1; I < Arguments.size(); ++I)
          if (Arguments[I].K == Value::Context ||
              Arguments[I].K == Value::Frame || Arguments[I].K == Value::Invoke)
            throw Invalid("block invocation exposes a context address as an "
                          "explicit argument");
        return {};
      }
      for (size_t I = 0; I < Arguments.size(); ++I) {
        const auto &A = Arguments[I];
        if (A.K == Value::Frame || A.K == Value::Invoke)
          throw Invalid("block consumer exposes private context storage");
        if (A.K != Value::Context)
          continue;
        if (A.Offset || !B || B->CallKind != CallKind::Native ||
            E.IsIndirectCall ||
            !noEscape(Source, Functions, B->TargetAddress, I, nullptr, Active,
                      Reason))
          throw Invalid(
              "block address flows to an unproven synchronous consumer");
      }
      return {};
    };
    if (F.Body.size() > 65536)
      throw Invalid("block consumer exceeds its statement budget");
    for (const auto &S : F.Body) {
      if (!S.Body.empty() || !S.ElseBody.empty() || !S.DefaultBody.empty() ||
          !S.Cases.empty() || !S.EHClauseBodies.empty())
        throw Invalid("block consumer requires unsupported control-flow proof");
      switch (S.Kind) {
      case StmtKind::Nop:
        break;
      case StmtKind::Assign:
        State.assign(S);
        break;
      case StmtKind::Call:
        (void)State.eval(S.CallExpr);
        break;
      case StmtKind::Store: {
        auto Address = State.eval(S.StoreAddr), V = State.eval(S.StoreVal);
        if (S.MemoryOrdering != NdMemoryOrdering::None ||
            S.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
            !S.StoreVal || !S.StoreVal->Type)
          throw Invalid("block consumer has an unknown memory effect");
        // All nonlocal writes are conservative failures: an unknown alias
        // cannot be used as proof that the context remains private.
        State.storeFrame(Address, S.StoreVal->Type->Size, V);
        break;
      }
      case StmtKind::Return:
        if (S.RetVal) {
          auto V = State.eval(S.RetVal);
          if (V.K == Value::Context || V.K == Value::Frame ||
              V.K == Value::Invoke)
            throw Invalid(
                "block consumer returns a context or private frame address");
        }
        break;
      default:
        throw Invalid("block consumer has unmodeled control flow");
      }
    }
    return true;
  } catch (const Invalid &Error) {
    Reason = Error.what();
    return false;
  }
}

inline bool publish(ObjCBlockSourcePlan &Plan,
                    const ObjCBlockDescriptor &Descriptor, va_t Invoke) {
  if (!validDescriptor(Descriptor) || Plan.Rejections.count(Invoke) ||
      Plan.Rejections.count(Descriptor.Address))
    return false;
  auto D = Plan.Descriptors.find(Descriptor.Address);
  auto H = Plan.InvokeHints.find(Invoke);
  if ((D != Plan.Descriptors.end() && !sameDescriptor(D->second, Descriptor)) ||
      (H != Plan.InvokeHints.end() &&
       !objc_projection_detail::sameHint(H->second,
                                         *Descriptor.InvokeTypeHint))) {
    Plan.Rejections[Invoke] =
        "block invoke has conflicting descriptor ABI evidence";
    Plan.InvokeHints.erase(Invoke);
    return false;
  }
  Plan.Descriptors.emplace(Descriptor.Address, Descriptor);
  Plan.InvokeHints.emplace(Invoke, *Descriptor.InvokeTypeHint);
  return true;
}

inline std::vector<ObjCStackBlockSource>
stackBlocks(const ObjCBlockSourceContext &Source, const HighFunc &Function,
            const std::map<va_t, const HighFunc *> &Functions,
            std::string &Reason) {
  const auto &Image = Source.Image;
  struct Byte {
    Value V;
    unsigned Index = 0, Width = 0;
  };
  std::map<int64_t, Byte> Memory;
  std::map<int64_t, ObjCStackBlockSource> Blocks;
  bool SawIsa = false;
  try {
    Values State(Source, Function);
    auto Word = [&](int64_t Address) -> Value {
      auto Begin = Memory.find(Address);
      if (Begin == Memory.end() || Begin->second.Index ||
          Begin->second.Width != 8)
        throw Invalid("block header pointer is not completely initialized");
      for (unsigned I = 0; I < 8; ++I) {
        auto B = Memory.find(Address + I);
        if (B == Memory.end() || B->second.Index != I || B->second.Width != 8 ||
            B->second.V.Producer != Begin->second.V.Producer)
          throw Invalid("block header pointer has conflicting partial writes");
      }
      return Begin->second.V;
    };
    auto Integer = [&](int64_t Address, unsigned Bytes) -> uint64_t {
      uint64_t Bits = 0;
      for (unsigned I = 0; I < Bytes; ++I) {
        auto B = Memory.find(Address + I);
        if (B == Memory.end() || B->second.V.K != Value::Number)
          throw Invalid(
              "block flags and reserved bytes are not completely initialized");
        Bits |= ((B->second.V.Bits >> (B->second.Index * 8)) & 255) << (I * 8);
      }
      return Bits;
    };
    auto Constructed = [&](int64_t Base) -> ObjCStackBlockSource {
      const auto Isa = Word(Base), Invoke = Word(Base + 16),
                 Descriptor = Word(Base + 24);
      if (Isa.K != Value::Isa || Isa.Name != "_NSConcreteStackBlock" ||
          Invoke.K != Value::Number || !Image.isCodeAddress(Invoke.Bits) ||
          Descriptor.K != Value::Number || !Invoke.Producer ||
          !Descriptor.Producer || Integer(Base + 12, 4) != 0)
        throw Invalid("block literal header has no complete "
                      "stack/invoke/descriptor identity");
      const auto Flags = static_cast<uint32_t>(Integer(Base + 8, 4));
      if ((Flags & (1u << 28)) && !(Flags & (1u << 23)))
        throw Invalid(
            "stack block concrete class disagrees with its storage flags");
      std::string Error;
      auto D = readObjCBlockDescriptor(Image, Descriptor.Bits, Flags, Error);
      if (!D || !validDescriptor(*D))
        throw Invalid(
            D ? "block ownership, capture layout, or ABI is unsupported"
              : Error);
      if (!frameRange(Function, Base, D->LiteralSize))
        throw Invalid("block literal exceeds recovered frame storage");
      ObjCStackBlockSource Result;
      Result.FrameOffset = Base;
      Result.InvokeEntry = Invoke.Bits;
      Result.Descriptor = *D;
      for (uint64_t Offset = 32; Offset < D->LiteralSize; ++Offset) {
        auto B = Memory.find(Base + static_cast<int64_t>(Offset));
        if (B == Memory.end())
          continue;
        if (B->second.V.K == Value::Frame || B->second.V.K == Value::Context ||
            B->second.V.K == Value::Invoke || B->second.V.K == Value::Isa)
          throw Invalid("block capture retains an unproven context or private "
                        "frame address");
        Result.InitializedCaptures.insert(Offset);
      }
      Result.References.emplace(
          Isa.Producer, ObjCBlockAddressBinding{CallKind::RuntimeBlockIsa,
                                                Isa.Bits, Isa.Name});
      Result.References.emplace(
          Invoke.Producer,
          ObjCBlockAddressBinding{CallKind::NativeAddress, Invoke.Bits, {}});
      Result.References.emplace(
          Descriptor.Producer,
          ObjCBlockAddressBinding{
              CallKind::RuntimeBlockDescriptor, Descriptor.Bits, {}});
      return Result;
    };
    State.Call = [&](const HighExpr &E,
                     const std::vector<Value> &Arguments) -> Value {
      for (size_t I = 0; I < Arguments.size(); ++I) {
        if (Arguments[I].K != Value::Frame)
          continue;
        auto Block = Constructed(Arguments[I].Offset);
        const auto &Binding = E.SourceCallHint;
        std::string Error;
        std::set<std::pair<va_t, size_t>> Active;
        const bool Direct =
            Binding && Binding->CallKind == CallKind::BlockInvoke && I == 0;
        if (!Direct && (!Binding || Binding->CallKind != CallKind::Native ||
                        E.IsIndirectCall ||
                        !noEscape(Source, Functions, Binding->TargetAddress, I,
                                  nullptr, Active, Error)))
          throw Invalid(
              "stack block flows to an unproven synchronous consumer: " +
              Error);
        auto Previous = Blocks.find(Block.FrameOffset);
        if (Previous != Blocks.end() &&
            (Previous->second.InvokeEntry != Block.InvokeEntry ||
             !sameDescriptor(Previous->second.Descriptor, Block.Descriptor) ||
             Previous->second.InitializedCaptures != Block.InitializedCaptures))
          throw Invalid("stack block storage is reused with inconsistent "
                        "construction evidence");
        Blocks[Block.FrameOffset] = std::move(Block);
      }
      return {};
    };
    if (Function.Body.size() > 65536)
      throw Invalid("block construction exceeds its statement budget");
    for (const auto &S : Function.Body) {
      if (!S.Body.empty() || !S.ElseBody.empty() || !S.DefaultBody.empty() ||
          !S.Cases.empty() || !S.EHClauseBodies.empty())
        throw Invalid("stack block construction requires straight-line "
                      "initialization proof");
      switch (S.Kind) {
      case StmtKind::Nop:
        break;
      case StmtKind::Assign:
        State.assign(S);
        break;
      case StmtKind::Call:
        (void)State.eval(S.CallExpr);
        break;
      case StmtKind::Store: {
        auto Address = State.eval(S.StoreAddr), V = State.eval(S.StoreVal);
        SawIsa |= V.K == Value::Isa && V.Name == "_NSConcreteStackBlock";
        if (Address.K != Value::Frame) {
          if (V.K == Value::Frame)
            throw Invalid("stack address escapes to nonlocal storage");
          if (SawIsa)
            throw Invalid(
                "stack block construction has an unknown aliasing write");
          break;
        }
        if (!S.StoreVal || !S.StoreVal->Type ||
            !scalarWidth(S.StoreVal->Type->Size) ||
            S.MemoryOrdering != NdMemoryOrdering::None ||
            S.MemoryAddressSpace != NdMemoryAddressSpace::Default)
          throw Invalid("block construction has an unsupported memory write");
        const unsigned Bytes = S.StoreVal->Type->Size;
        State.storeFrame(Address, Bytes, V);
        for (unsigned I = 0; I < Bytes; ++I)
          Memory[Address.Offset + I] = {V, I, Bytes};
        break;
      }
      case StmtKind::Return:
        if (S.RetVal && State.eval(S.RetVal).K == Value::Frame)
          throw Invalid(
              "stack block construction returns private frame storage");
        break;
      default:
        throw Invalid("stack block construction has unmodeled control flow");
      }
    }
    std::vector<ObjCStackBlockSource> Result;
    for (auto &[Offset, Block] : Blocks)
      Result.push_back(std::move(Block));
    return Result;
  } catch (const Invalid &Error) {
    if (SawIsa)
      Reason = Error.what();
    return {};
  }
}

} // namespace objc_block_source_detail

inline ObjCBlockSourcePlan
discoverObjCBlockSources(const ObjCBlockSourceContext &Source,
                         const PipelineResult &Result) {
  using namespace objc_block_source_detail;
  const auto &Image = Source.Image;
  ObjCBlockSourcePlan Plan;
  if (Result.SourceImage != &Image)
    throw std::invalid_argument(
        "block source evidence belongs to another image");
  const objc::RuntimeData Data(Image);
  for (const auto &Block : findObjCGlobalBlocks(Image)) {
    // Global captured data could itself contain relocations or pointer values;
    // only a complete capture-free global literal is currently reconstructed.
    if (Block.StorageKind != ObjCBlockLiteral::Kind::Global ||
        Block.Descriptor.LiteralSize != 32 ||
        !Block.Descriptor.Captures.empty() ||
        Data.u32(Block.Address + 12) != 0 ||
        !publish(Plan, Block.Descriptor, Block.InvokeEntry))
      continue;
    Plan.Globals.emplace(Block.Address, Block);
  }
  std::map<va_t, const HighFunc *> Functions;
  for (const auto &Function : Result.HighFuncs)
    Functions.emplace(Function.Entry, &Function);
  for (const auto &Function : Result.HighFuncs) {
    std::string Error;
    auto Blocks = stackBlocks(Source, Function, Functions, Error);
    if (!Error.empty())
      Plan.Rejections[Function.Entry] = std::move(Error);
    for (auto &Block : Blocks)
      if (publish(Plan, Block.Descriptor, Block.InvokeEntry))
        Plan.StackBlocks[Function.Entry].push_back(std::move(Block));
  }
  return Plan;
}

inline ObjCBlockSourcePlan
discoverObjCBlockSources(const BinaryImage &Image,
                         const PipelineResult &Result) {
  return discoverObjCBlockSources(ObjCBlockSourceContext(Image), Result);
}

inline size_t applyObjCBlockInvokeHints(const ObjCBlockSourcePlan &Plan,
                                        PipelineOptions &Options) {
  size_t Changed = 0;
  for (const auto &[Entry, Hint] : Plan.InvokeHints) {
    auto Existing = Options.SourceTypeHints.find(Entry);
    if (Existing != Options.SourceTypeHints.end()) {
      if (objc_projection_detail::sameHint(Existing->second, Hint))
        continue;
      if (Existing->second.Origin !=
          SourceFunctionTypeHint::OriginKind::NativeAnalysis)
        continue;
      Existing->second = Hint;
    } else
      Options.SourceTypeHints.emplace(Entry, Hint);
    ++Changed;
  }
  return Changed;
}

inline std::string objcBlockInvokeName(va_t Entry) {
  return "neverd_block_invoke_" + llvm::utohexstr(Entry, true);
}
inline std::string objcBlockHelperName(bool Literal, va_t Address) {
  return std::string(Literal ? "neverd_block_literal_"
                             : "neverd_block_descriptor_") +
         llvm::utohexstr(Address, true) + "_address";
}

/// Bind only references whose literal construction or loaded-image identity
/// has already been established. Preserve every raw frame/capture write.
inline ObjCBlockSourceBindingResult bindObjCBlockSourceReferences(
    const HighFunc &Function, const ObjCBlockSourceContext &Source,
    const ObjCBlockSourcePlan &Plan,
    const std::map<va_t, const HighFunc *> &Functions) {
  using namespace objc_block_source_detail;
  const auto &Image = Source.Image;
  ObjCBlockSourceBindingResult Result{Function, {}, {}, {}, {}};
  try {
    if (auto Rejected = Plan.Rejections.find(Function.Entry);
        Rejected != Plan.Rejections.end())
      throw Invalid(Rejected->second);
    std::map<const HighExpr *, ObjCBlockAddressBinding> References;
    auto RequireInvoke = [&](va_t Entry, const ObjCBlockDescriptor &Descriptor,
                             const std::set<uint64_t> &Initialized) {
      auto Found = Functions.find(Entry);
      auto Hint = Plan.InvokeHints.find(Entry);
      if (Plan.Rejections.count(Entry) || Found == Functions.end() ||
          Hint == Plan.InvokeHints.end() || !Found->second->SourceTypeHint ||
          !Descriptor.InvokeTypeHint ||
          !objc_projection_detail::sameHint(*Found->second->SourceTypeHint,
                                            *Descriptor.InvokeTypeHint) ||
          !objc_projection_detail::sameHint(Hint->second,
                                            *Descriptor.InvokeTypeHint))
        throw Invalid(
            "block invoke has no complete descriptor-bound native function");
      std::string Reason;
      std::set<std::pair<va_t, size_t>> Active;
      if (!noEscape(Source, Functions, Entry, 0, &Initialized, Active, Reason))
        throw Invalid("block invoke capture proof failed: " + Reason);
      Result.Dependencies.insert(Entry);
    };
    auto RequireDescriptor = [&](const ObjCBlockDescriptor &Descriptor) {
      auto Found = Plan.Descriptors.find(Descriptor.Address);
      if (Plan.Rejections.count(Descriptor.Address) ||
          Found == Plan.Descriptors.end() ||
          !sameDescriptor(Found->second, Descriptor))
        throw Invalid("block descriptor has conflicting source evidence");
      Result.Descriptors.insert(Descriptor.Address);
      // A shared descriptor helper owns all its known global literals. Keeping
      // this set stable makes the same global object identical across methods.
      for (const auto &[Address, Global] : Plan.Globals) {
        if (Global.Descriptor.Address != Descriptor.Address)
          continue;
        RequireInvoke(Global.InvokeEntry, Global.Descriptor, {});
        Result.Literals.insert(Address);
      }
    };
    if (auto Stack = Plan.StackBlocks.find(Function.Entry);
        Stack != Plan.StackBlocks.end()) {
      for (const auto &Block : Stack->second) {
        RequireInvoke(Block.InvokeEntry, Block.Descriptor,
                      Block.InitializedCaptures);
        RequireDescriptor(Block.Descriptor);
        for (const auto &[Expression, Binding] : Block.References) {
          auto [Existing, Added] = References.emplace(Expression, Binding);
          if (!Added && (Existing->second.Kind != Binding.Kind ||
                         Existing->second.Address != Binding.Address ||
                         Existing->second.Name != Binding.Name))
            throw Invalid(
                "block source expression has conflicting address identities");
        }
      }
    }
    std::map<const HighExpr *, ExprPtr> Copies;
    size_t Budget = 1000000;
    std::function<ExprPtr(const ExprPtr &, unsigned)> Copy;
    Copy = [&](const ExprPtr &Original, unsigned Depth) -> ExprPtr {
      if (!Original)
        return nullptr;
      if (Depth > 200 || !Budget--)
        throw Invalid("block source projection exceeds its expression budget");
      if (auto Existing = Copies.find(Original.get()); Existing != Copies.end())
        return Existing->second;
      auto Expression = std::make_shared<HighExpr>(*Original);
      Copies.emplace(Original.get(), Expression);
      std::optional<ObjCBlockAddressBinding> Address;
      if (auto Found = References.find(Original.get());
          Found != References.end())
        Address = Found->second;
      else if (Original->Kind == ExprKind::Const && Original->Type &&
               Original->Type->Size == 8) {
        if (auto Global = Plan.Globals.find(Original->ConstVal);
            Global != Plan.Globals.end()) {
          RequireDescriptor(Global->second.Descriptor);
          Address = ObjCBlockAddressBinding{
              CallKind::RuntimeBlockLiteral, Global->first, {}};
        }
      }
      if (Address) {
        auto Binding = std::make_shared<SourceCallTypeHint>();
        Binding->CallKind = Address->Kind;
        Binding->TargetAddress = Address->Address;
        Binding->TargetName = Address->Name;
        auto &Hint = Binding->Signature;
        Hint.Architecture = Image.Arch;
        Hint.HasExplicitABI = true;
        Hint.ReturnType = NdType::makePtr(NdType::makeVoid());
        Hint.ReturnLocation = {SourceABICarrierKind::IntegerRegister,
                               getTargetRegInfo(Image.Arch).IntReturnReg, 0, 8};
        Expression->Kind = ExprKind::Call;
        Expression->CallAddr = 0;
        Expression->CallTarget.clear();
        Expression->SourceCallHint = std::move(Binding);
        Expression->Operands.clear();
        Expression->IsIndirectCall = false;
        return Expression;
      }
      for (auto &Operand : Expression->Operands)
        Operand = Copy(Operand, Depth + 1);
      return Expression;
    };
    std::function<void(std::vector<HighStmt> &, unsigned)> Walk;
    Walk = [&](std::vector<HighStmt> &Body, unsigned Depth) {
      if (Depth > 200)
        throw Invalid(
            "block source projection exceeds its control-flow budget");
      for (auto &Statement : Body) {
        forEachExpr(Statement, [&](ExprPtr &E) { E = Copy(E, 0); });
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
    if (Plan.InvokeHints.count(Function.Entry)) {
      Result.Function.Name = objcBlockInvokeName(Function.Entry);
      Result.Function.DebugName.clear();
      Result.Function.SourceFile.clear();
    }
  } catch (const Invalid &Error) {
    Result.Limitation = Error.what();
  }
  return Result;
}

inline ObjCBlockSourceBindingResult bindObjCBlockSourceReferences(
    const HighFunc &Function, const BinaryImage &Image,
    const ObjCBlockSourcePlan &Plan,
    const std::map<va_t, const HighFunc *> &Functions) {
  return bindObjCBlockSourceReferences(Function, ObjCBlockSourceContext(Image),
                                       Plan, Functions);
}

inline bool
objcBlockSourceCallBound(const HighExpr &Expression,
                         const ObjCBlockSourceContext &Source,
                         const ObjCBlockSourcePlan &Plan,
                         const std::map<va_t, const HighFunc *> &Functions) {
  using namespace objc_block_source_detail;
  const auto &Image = Source.Image;
  if (Expression.Kind != ExprKind::Call || !Expression.SourceCallHint ||
      Expression.IntrinsicId != Intrinsic::None ||
      Expression.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  const auto &Binding = *Expression.SourceCallHint;
  const auto &Hint = Binding.Signature;
  std::string Error;
  if (Hint.Architecture != Image.Arch || !validateSourceABI(Hint, Error))
    return false;
  if (Binding.CallKind == CallKind::BlockInvoke)
    return Hint.Parameters.size() == Expression.Operands.size() &&
           !Hint.Parameters.empty() && Hint.Parameters[0].Type &&
           Hint.Parameters[0].Type->Kind == NdTypeKind::Ptr &&
           (Hint.Origin == SourceFunctionTypeHint::OriginKind::BlockRuntime ||
            Hint.Origin == SourceFunctionTypeHint::OriginKind::NativeAnalysis);
  if (Expression.IsIndirectCall || !Hint.Parameters.empty() ||
      !Expression.Operands.empty() || !Hint.ReturnType ||
      Hint.ReturnType->Kind != NdTypeKind::Ptr || Hint.ReturnType->Size != 8)
    return false;
  switch (Binding.CallKind) {
  case CallKind::NativeAddress: {
    auto Function = Functions.find(Binding.TargetAddress);
    auto Invoke = Plan.InvokeHints.find(Binding.TargetAddress);
    return Function != Functions.end() && Invoke != Plan.InvokeHints.end() &&
           Function->second->SourceTypeHint &&
           objc_projection_detail::sameHint(*Function->second->SourceTypeHint,
                                            Invoke->second);
  }
  case CallKind::RuntimeBlockIsa: {
    const auto &Imports = Source.Imports;
    auto Slot = Imports.Slots.find(Binding.TargetAddress);
    return Slot != Imports.Slots.end() && !Slot->second.Addend &&
           !Imports.Conflicts.count(Binding.TargetAddress) &&
           !isaName(Slot->second.Name).empty() &&
           isaName(Slot->second.Name) == Binding.TargetName;
  }
  case CallKind::RuntimeBlockDescriptor:
    return Plan.Descriptors.count(Binding.TargetAddress) &&
           !Plan.Rejections.count(Binding.TargetAddress);
  case CallKind::RuntimeBlockLiteral:
    return Plan.Globals.count(Binding.TargetAddress);
  default:
    return false;
  }
}

inline bool
objcBlockSourceCallBound(const HighExpr &Expression, const BinaryImage &Image,
                         const ObjCBlockSourcePlan &Plan,
                         const std::map<va_t, const HighFunc *> &Functions) {
  return objcBlockSourceCallBound(Expression, ObjCBlockSourceContext(Image),
                                  Plan, Functions);
}

/// All data definitions are function-local, while the three generated function
/// identities are explicitly shared by the Objective-C source assembler.
inline std::string
renderObjCBlockSourceHelpers(const ObjCBlockSourcePlan &Plan,
                             const std::set<va_t> &Descriptors,
                             std::set<std::string> &SharedFunctions) {
  using namespace objc_block_source_detail;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  auto String = [](llvm::StringRef Bytes) {
    std::string Result = "\"";
    for (unsigned char Byte : Bytes.bytes()) {
      Result += "\\x";
      Result += "0123456789abcdef"[Byte >> 4];
      Result += "0123456789abcdef"[Byte & 15];
    }
    return Result + "\"";
  };
  for (va_t Address : Descriptors) {
    const auto &D = Plan.Descriptors.at(Address);
    if (!validDescriptor(D))
      throw Invalid("block helper requires a complete scalar descriptor");
    std::vector<const ObjCBlockLiteral *> Globals;
    for (const auto &[LiteralAddress, G] : Plan.Globals)
      if (G.Descriptor.Address == Address)
        Globals.push_back(&G);
    const bool Layout = D.Flags & (UINT32_C(1) << 31);
    const std::string Name = objcBlockHelperName(false, Address);
    SharedFunctions.insert(Name);
    OS << "\nuintptr_t " << Name << "(void) {\n"
       << "  _Static_assert(sizeof(void *) == 8, \"Block source requires "
          "64-bit pointers\");\n"
       << "  struct neverd_block_descriptor { uint64_t reserved, size; const "
          "char *signature;";
    if (Layout)
      OS << " uintptr_t layout;";
    OS << " };\n  struct neverd_block_literal { void *isa; uint32_t flags, "
          "reserved; void (*invoke)(void); const struct "
          "neverd_block_descriptor *descriptor; };\n"
       << "  struct neverd_block_storage { struct neverd_block_descriptor "
          "descriptor;";
    if (!Globals.empty())
      OS << " struct neverd_block_literal literals[" << Globals.size() << "];";
    OS << " };\n";
    if (!Globals.empty())
      OS << "  extern void *_NSConcreteGlobalBlock[];\n";
    if (Layout && !D.LayoutBytes.empty()) {
      OS << "  static const unsigned char layout[] = {";
      for (uint8_t Byte : D.LayoutBytes)
        OS << static_cast<unsigned>(Byte) << ",";
      OS << "0};\n";
    }
    OS << "  static const struct neverd_block_storage storage = { {0, "
       << D.LiteralSize << ", " << String(D.Signature);
    if (Layout) {
      OS << ", ";
      if (!D.LayoutBytes.empty())
        OS << "(uintptr_t)layout";
      else
        OS << D.LayoutValue;
    }
    OS << "}";
    if (!Globals.empty()) {
      OS << ", {";
      for (const auto *Global : Globals) {
        SharedFunctions.insert(objcBlockInvokeName(Global->InvokeEntry));
        OS << "{(void *)_NSConcreteGlobalBlock, " << Global->Descriptor.Flags
           << "U, 0, (void (*)(void))&"
           << objcBlockInvokeName(Global->InvokeEntry)
           << ", &storage.descriptor},";
      }
      OS << "}";
    }
    OS << " };\n  return (uintptr_t)&storage.descriptor;\n}\n";
    for (size_t I = 0; I < Globals.size(); ++I) {
      const auto LiteralName = objcBlockHelperName(true, Globals[I]->Address);
      SharedFunctions.insert(LiteralName);
      OS << "uintptr_t " << LiteralName << "(void) { return " << Name << "() + "
         << (Layout ? 32 : 24) + I * 32 << "; }\n";
    }
  }
  return Source;
}

} // namespace neverd::sdk
#endif
