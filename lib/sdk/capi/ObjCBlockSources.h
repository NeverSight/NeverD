#ifndef NEVERD_SDK_CAPI_OBJCBLOCKSOURCES_H
#define NEVERD_SDK_CAPI_OBJCBLOCKSOURCES_H

#include "ObjCSourceBindings.h"

#include "neverd/ir/high/HighSourceFlow.h"
#include "neverd/loader/ObjC/ObjCBlocks.h"
#include "neverd/loader/ReadOnlyBytes.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"

#include <limits>
#include <stdexcept>
#include <tuple>
#include <type_traits>

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
  std::map<const HighExpr *, uint64_t> HeaderConstants;
};
struct ObjCBlockSourcePlan {
  std::map<va_t, ObjCBlockLiteral> Globals;
  std::map<va_t, ObjCBlockDescriptor> Descriptors;
  std::map<va_t, SourceFunctionTypeHint> InvokeHints;
  std::map<va_t, SourceFunctionTypeHint> HelperHints;
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
constexpr uint64_t StrongObjectFieldFlag = 3;
constexpr uint64_t StrongBlockFieldFlag = 7;
struct Invalid : std::runtime_error {
  using std::runtime_error::runtime_error;
};
struct Value {
  enum Kind {
    Scalar,
    Number,
    ImageBits,
    Frame,
    Context,
    Invoke,
    Isa,
    PointerBits,
    UnprovenIdentity
  } K = Scalar;
  int64_t Offset = 0;
  uint64_t Bits = 0;
  std::string Name;
  const HighExpr *Producer = nullptr;
};
inline bool pointerIdentity(const Value &V) {
  return V.K == Value::Frame || V.K == Value::Context || V.K == Value::Invoke ||
         V.K == Value::Isa || V.K == Value::PointerBits ||
         V.K == Value::UnprovenIdentity;
}
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
  if (!D.InvokeTypeHint || !D.Limitations.empty() || D.LiteralSize < 32 ||
      D.LiteralSize > (1u << 20) ||
      bool(D.CopyHelper) != bool(D.DisposeHelper) ||
      (D.CopyHelper && (!D.CopyTypeHint || !D.DisposeTypeHint)))
    return false;
  return std::all_of(D.Captures.begin(), D.Captures.end(), [&](const auto &R) {
    return R.StorageKind == ObjCBlockCaptureRange::Kind::NonObjectBytes ||
           (R.StorageKind == ObjCBlockCaptureRange::Kind::Strong &&
            D.CopyHelper);
  });
}
inline bool sameDescriptor(const ObjCBlockDescriptor &A,
                           const ObjCBlockDescriptor &B) {
  return A.LiteralSize == B.LiteralSize && A.Signature == B.Signature &&
         A.CopyHelper == B.CopyHelper && A.DisposeHelper == B.DisposeHelper &&
         A.LayoutValue == B.LayoutValue && A.LayoutBytes == B.LayoutBytes &&
         ((A.Flags ^ B.Flags) & ~((UINT32_C(1) << 23) | (UINT32_C(1) << 28))) ==
             0 &&
         A.InvokeTypeHint && B.InvokeTypeHint &&
         objc_projection_detail::sameHint(*A.InvokeTypeHint, *B.InvokeTypeHint);
}

/// The value evaluator is deliberately limited to proven pointer copies,
/// byte offsets and private frame spills. Scalar expressions cannot acquire
/// a context/frame identity merely because they contain an equal integer.
class Values {
  const BinaryImage &Image;
  const HighFunc &Function;
  const ImportStorageSlotCollection &Imports;
  size_t EvaluationBudget = 1000000;
  std::set<int64_t> FrameIdentityBytes;

public:
  struct Facts {
    std::map<Identity, Value> Locals;
    std::map<std::pair<int64_t, unsigned>, Value> FrameValues;
    std::set<int64_t> FrameIdentityBytes;
    size_t size() const {
      return Locals.size() + FrameValues.size() + FrameIdentityBytes.size();
    }
  };
  std::map<Identity, Value> Locals;
  std::map<std::pair<int64_t, unsigned>, Value> FrameValues;
  std::function<Value(const HighExpr &, const std::vector<Value> &)> Call;
  std::function<Value(const Value &, unsigned)> ContextRead;
  Values(const ObjCBlockSourceContext &Source, const HighFunc &F,
         std::optional<size_t> Context = std::nullopt)
      : Image(Source.Image), Function(F), Imports(Source.Imports) {
    if (Context) {
      MedVar Parameter;
      Parameter.Kind = MedVar::Param;
      Parameter.Id = *Context;
      Locals[objc_projection_detail::localIdentity(Parameter)] = {
          Value::Context};
    }
  }
  Values(ObjCBlockSourceContext &&, const HighFunc &,
         std::optional<size_t> = std::nullopt) = delete;
  Facts facts() const { return {Locals, FrameValues, FrameIdentityBytes}; }
  void swap(Facts &F) {
    Locals.swap(F.Locals);
    FrameValues.swap(F.FrameValues);
    FrameIdentityBytes.swap(F.FrameIdentityBytes);
  }
  bool frameContainsPointerIdentity() const {
    return !FrameIdentityBytes.empty();
  }
  void restore(const Facts &F) {
    Locals = F.Locals;
    FrameValues = F.FrameValues;
    FrameIdentityBytes = F.FrameIdentityBytes;
  }
  /// Consumer proofs need address identity, not the producer expression used
  /// to materialize a block header. Scalar disagreements become unknown;
  /// possible pointer identities remain poisoned until completely overwritten.
  static bool merge(Facts &Into, const Facts &From) {
    bool Changed = false;
    auto MergeValues = [&](auto &Dest, const auto &Source) {
      std::set<typename std::decay_t<decltype(Dest)>::key_type> Keys;
      for (const auto &[Key, V] : Dest)
        Keys.insert(Key);
      for (const auto &[Key, V] : Source)
        Keys.insert(Key);
      for (const auto &Key : Keys) {
        const auto D = Dest.find(Key);
        const auto S = Source.find(Key);
        const Value A = D == Dest.end() ? Value{} : D->second;
        const Value B = S == Source.end() ? Value{} : S->second;
        if (A.K == B.K && A.Offset == B.Offset && A.Bits == B.Bits &&
            A.Name == B.Name)
          continue;
        const Value Joined = pointerIdentity(A) || pointerIdentity(B)
                                 ? Value{Value::UnprovenIdentity}
                                 : Value{};
        if (A.K == Joined.K && !A.Offset && !A.Bits && A.Name.empty())
          continue;
        Changed = true;
        Dest[Key] = Joined;
      }
    };
    MergeValues(Into.Locals, From.Locals);
    MergeValues(Into.FrameValues, From.FrameValues);
    const auto Before = Into.FrameIdentityBytes.size();
    Into.FrameIdentityBytes.insert(From.FrameIdentityBytes.begin(),
                                   From.FrameIdentityBytes.end());
    for (auto &[Range, V] : Into.FrameValues) {
      const auto Byte = Into.FrameIdentityBytes.lower_bound(Range.first);
      if (!pointerIdentity(V) && Byte != Into.FrameIdentityBytes.end() &&
          *Byte < Range.first + Range.second) {
        V = {Value::UnprovenIdentity};
        Changed = true;
      }
    }
    return Changed || Before != Into.FrameIdentityBytes.size();
  }
  static Value established(Value V) {
    if (V.K == Value::UnprovenIdentity)
      throw Invalid("block address has inconsistent reaching identities");
    return V;
  }
  Value eval(const ExprPtr &E, unsigned Depth = 0) {
    if (!E || Depth > 128 || !EvaluationBudget-- ||
        (!E->Type && E->Kind != ExprKind::Call))
      throw Invalid("block source has an incomplete or excessive expression");
    // Effect-only calls have no value type; their operands and call boundary
    // still participate in the ownership/escape proof.
    const unsigned Bytes = E->Type ? E->Type->Size : 0;
    if (E->Kind == ExprKind::Const && scalarWidth(Bytes))
      return {Value::Number, 0, E->ConstVal & mask(Bytes), {}, E.get()};
    if (E->Kind == ExprKind::Var) {
      const auto Found =
          Locals.find(objc_projection_detail::localIdentity(E->Var));
      if (Found != Locals.end())
        return established(Found->second);
      if (E->Var.Kind == MedVar::Param && E->Var.RenameTag < 0)
        return {};
      if (E->Var.Kind == MedVar::Reg && E->Var.RenameTag < 0 &&
          E->Var.SSAVer == 0 &&
          E->Var.RegOff == getTargetRegInfo(Image.Arch).StackPointer &&
          Bytes == 8)
        return {Value::Frame, 0, 0, {}, E.get()};
      return {};
    }
    std::vector<Value> Inputs;
    for (const auto &Input : E->Operands)
      Inputs.push_back(eval(Input, Depth + 1));
    if (E->Kind == ExprKind::Call)
      return Call ? Call(*E, Inputs) : Value{};
    // PointerBits packs one exact source kind and byte interval in Bits. It
    // remains tainted through ordinary scalar operations. Only complete,
    // ordered slices from the same identity can become an address again.
    auto PointerPiece = [](const Value &V)
        -> std::optional<std::tuple<Value::Kind, unsigned, unsigned>> {
      if (V.K != Value::PointerBits || !V.Bits)
        return std::nullopt;
      const auto Source = static_cast<Value::Kind>(V.Bits & 255);
      const unsigned Start = (V.Bits >> 8) & 255;
      const unsigned Size = (V.Bits >> 16) & 255;
      if ((Source != Value::Frame && Source != Value::Context &&
           Source != Value::Invoke) ||
          !Size || Start + Size > 8)
        return std::nullopt;
      return std::tuple{Source, Start, Size};
    };
    auto PointerSlice = [&](const Value &V, unsigned Start, unsigned Size,
                            const HighExpr *Producer) -> std::optional<Value> {
      Value::Kind Source = V.K;
      unsigned ExistingStart = 0, ExistingSize = 8;
      if (V.K == Value::PointerBits) {
        auto Piece = PointerPiece(V);
        if (!Piece)
          return std::nullopt;
        std::tie(Source, ExistingStart, ExistingSize) = *Piece;
      } else if (V.K != Value::Frame && V.K != Value::Context &&
                 V.K != Value::Invoke) {
        return std::nullopt;
      }
      if (!Size || Start > ExistingSize || Size > ExistingSize - Start)
        return std::nullopt;
      Start += ExistingStart;
      if (!Start && Size == 8)
        return Value{Source, V.Offset, 0, V.Name, Producer};
      return Value{Value::PointerBits, V.Offset,
                   uint64_t(Source) | (uint64_t(Start) << 8) |
                       (uint64_t(Size) << 16),
                   V.Name, Producer};
    };
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
      // Deferred image bytes are an ordinary loaded value, not a pointer
      // identity. A width conversion loses the raw header-byte recipe while
      // preserving the scalar expression for the source emitter.
      if (V.K == Value::Frame || V.K == Value::Context ||
          V.K == Value::Invoke || V.K == Value::Isa ||
          V.K == Value::PointerBits) {
        if (Bytes <= E->Operands[0]->Type->Size)
          if (auto Slice = PointerSlice(V, 0, Bytes, E.get()))
            return *Slice;
        return {Value::PointerBits, 0, 0, {}, E.get()};
      }
      if (V.K != Value::Scalar && V.K != Value::ImageBits)
        throw Invalid("block pointer is consumed through a partial value");
      return {};
    }
    if (E->Kind == ExprKind::BinOp && Inputs.size() == 2 &&
        E->Op == NdOp::SUBBYTES && Inputs[1].K == Value::Number &&
        E->Operands[0]->Type &&
        Inputs[1].Bits <= std::numeric_limits<unsigned>::max() &&
        Bytes <= E->Operands[0]->Type->Size &&
        Inputs[1].Bits <= E->Operands[0]->Type->Size - Bytes)
      if (auto Slice = PointerSlice(Inputs[0], Inputs[1].Bits, Bytes, E.get()))
        return *Slice;
    if (E->Kind == ExprKind::BinOp && Inputs.size() == 2 &&
        E->Op == NdOp::CONCAT) {
      const auto High = PointerPiece(Inputs[0]);
      const auto Low = PointerPiece(Inputs[1]);
      if (High && Low && std::get<0>(*High) == std::get<0>(*Low) &&
          Inputs[0].Offset == Inputs[1].Offset &&
          Inputs[0].Name == Inputs[1].Name &&
          std::get<1>(*Low) + std::get<2>(*Low) == std::get<1>(*High) &&
          std::get<2>(*High) + std::get<2>(*Low) == Bytes) {
        const auto Source = std::get<0>(*Low);
        const unsigned Start = std::get<1>(*Low);
        const unsigned Size = std::get<2>(*High) + std::get<2>(*Low);
        if (!Start && Size == 8)
          return {Source, Inputs[1].Offset, 0, Inputs[1].Name, E.get()};
        return {Value::PointerBits, Inputs[1].Offset,
                uint64_t(Source) | (uint64_t(Start) << 8) |
                    (uint64_t(Size) << 16),
                Inputs[1].Name, E.get()};
      }
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
          return established(Found->second);
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
      if (Address.K == Value::Number && scalarWidth(Bytes)) {
        // Defer byte reading until a scalar header field consumes this load.
        // Loaded bits never supply an invoke/descriptor address identity.
        return {Value::ImageBits, 0, Address.Bits, {}, E.get()};
      }
      if (pointerIdentity(Address))
        throw Invalid(
            "pointer-derived block bits are used as a memory address");
    }
    for (const auto &V : Inputs)
      if (V.K == Value::UnprovenIdentity)
        throw Invalid("block address has inconsistent reaching identities");
      else if (pointerIdentity(V))
        return {
            Value::PointerBits, 0, 0, {}, V.Producer ? V.Producer : E.get()};
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

/// Transfer every reachable emitted edge to a bounded fixed point. A failed
/// transfer or join invalidates the entire proof, including earlier visits.
template <typename Facts, typename Transfer, typename Join>
void proveSourceFlow(const HighFunc &F, Facts Initial, Transfer Evaluate,
                     Join Merge) {
  const auto Graph = buildHighSourceFlowGraph(F);
  if (!Graph.Diagnostics.Complete || Graph.Nodes.empty())
    throw Invalid("block source has incomplete source control flow");
  constexpr size_t MaxWork = 1000000, MaxFacts = 262144;
  size_t Work = 0, FactCount = 0;
  std::vector<std::optional<Facts>> Incoming(Graph.Nodes.size());
  std::vector<bool> Queued(Graph.Nodes.size());
  std::vector<size_t> Pending{Graph.Entry};
  Incoming[Graph.Entry] = std::move(Initial);
  FactCount = Incoming[Graph.Entry]->size();
  Queued[Graph.Entry] = true;
  auto Spend = [&](size_t Count) {
    if (Count > MaxWork - Work)
      throw Invalid("block source exceeds its flow proof budget");
    Work += Count;
  };
  while (!Pending.empty()) {
    const auto Index = Pending.back();
    Pending.pop_back();
    Queued[Index] = false;
    Spend(1 + Incoming[Index]->size());
    auto Output = *Incoming[Index];
    Evaluate(Output, Graph.Nodes[Index]);
    for (auto Next : Graph.Nodes[Index].Successors) {
      auto &Input = Incoming[Next];
      const auto Before = Input ? Input->size() : 0;
      Spend(1 + Before + Output.size());
      const bool Changed = !Input || Merge(*Input, Output);
      if (!Input)
        Input = Output;
      FactCount = FactCount - Before + Input->size();
      if (FactCount > MaxFacts)
        throw Invalid("block source exceeds its flow storage budget");
      if (Changed && !Queued[Next]) {
        Queued[Next] = true;
        Pending.push_back(Next);
      }
    }
  }
}

/// Prove that a context pointer is neither returned nor exposed to memory or
/// unknown callees. A descriptor-backed invoke may read only known capture
/// bytes. Forwarding consumers may read only the invoke pointer at byte 16.
inline bool noEscape(const ObjCBlockSourceContext &Source,
                     const std::map<va_t, const HighFunc *> &Functions,
                     va_t Entry, size_t Parameter,
                     const std::set<uint64_t> *Initialized,
                     std::set<std::pair<va_t, size_t>> &Active,
                     std::string &Reason,
                     const std::set<uint64_t> *WritableStrongFields = nullptr) {
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
      // These two runtime modes write one strong pointer field. Byref and
      // weak fields have different lifetime rules and receive no permission.
      if (WritableStrongFields && B &&
          B->CallKind == CallKind::DarwinRuntimeCall &&
          B->TargetName == "_Block_object_assign" && Arguments.size() == 3 &&
          Arguments[0].K == Value::Context && Arguments[0].Offset >= 32 &&
          WritableStrongFields->count(Arguments[0].Offset) &&
          (Arguments[1].K == Value::Scalar || Arguments[1].K == Value::Number ||
           Arguments[1].K == Value::ImageBits) &&
          Arguments[2].K == Value::Number &&
          (Arguments[2].Bits == StrongObjectFieldFlag ||
           Arguments[2].Bits == StrongBlockFieldFlag) &&
          objcSourceCallBound(E, Source.Image, Functions))
        return {};
      if (B && B->CallKind == CallKind::BlockInvoke &&
          Arguments.size() == B->Signature.Parameters.size() &&
          !Arguments.empty() && Arguments[0].K == Value::Context &&
          Arguments[0].Offset == 0) {
        for (size_t I = 1; I < Arguments.size(); ++I)
          if (pointerIdentity(Arguments[I]))
            throw Invalid("block invocation exposes a context address as an "
                          "explicit argument");
        return {};
      }
      for (size_t I = 0; I < Arguments.size(); ++I) {
        const auto &A = Arguments[I];
        // An invoke's own fresh stack storage is not the block context. It can
        // be passed to a bound call while the frame contains no context,
        // invoke, ISA, or other pointer identity. Once any such identity has
        // been stored, an unbounded frame pointer could expose it and remains
        // rejected conservatively.
        if (A.K == Value::Frame && !State.frameContainsPointerIdentity())
          continue;
        if (A.K == Value::Frame || A.K == Value::Invoke || A.K == Value::Isa ||
            A.K == Value::PointerBits || A.K == Value::UnprovenIdentity)
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
    auto Evaluate = [&](const HighSourceFlowNode &Node) {
      if (Node.Test) {
        if (pointerIdentity(State.eval(Node.Test)))
          throw Invalid("block-derived pointer bits control source flow");
        return;
      }
      if (!Node.Statement)
        return;
      const auto &S = *Node.Statement;
      switch (S.Kind) {
      case StmtKind::Nop:
      case StmtKind::Block:
      case StmtKind::If:
      case StmtKind::IfElse:
      case StmtKind::While:
      case StmtKind::For:
      case StmtKind::DoWhile:
      case StmtKind::Switch:
      case StmtKind::Goto:
      case StmtKind::Break:
      case StmtKind::Continue:
        // The shared graph owns body, test, goto, and loop transfer order.
        break;
      case StmtKind::Assign:
        State.assign(S);
        break;
      case StmtKind::Call:
        (void)State.eval(S.CallExpr);
        break;
      case StmtKind::ExprStmt:
        if (pointerIdentity(State.eval(S.Val)))
          throw Invalid("block-derived pointer bits remain observable");
        break;
      case StmtKind::Store: {
        auto Address = State.eval(S.StoreAddr), V = State.eval(S.StoreVal);
        if (S.MemoryOrdering != NdMemoryOrdering::None ||
            S.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
            !S.StoreVal || !S.StoreVal->Type)
          throw Invalid("block consumer has an unknown memory effect");
        const auto Bytes = S.StoreVal->Type->Size;
        if (Address.K == Value::Frame) {
          State.storeFrame(Address, Bytes, V);
          break;
        }
        // A complete image range is disjoint from the private frame/context.
        // It can receive ordinary values, but never a private pointer. The
        // source-data owner still has to validate and relocate the actual
        // storage; this escape proof does not authorize arbitrary aliases.
        if (Address.K != Value::Number || pointerIdentity(V) || !Bytes ||
            Bytes - 1 > InvalidVA - Address.Bits ||
            !Source.Image.isDataAddress(Address.Bits) ||
            !Source.Image.isDataAddress(Address.Bits + Bytes - 1) ||
            Source.Image.getSegmentFor(Address.Bits) !=
                Source.Image.getSegmentFor(Address.Bits + Bytes - 1) ||
            Source.Image.getSectionFor(Address.Bits) !=
                Source.Image.getSectionFor(Address.Bits + Bytes - 1))
          throw Invalid(
              "block consumer store has no disjoint image provenance");
        break;
      }
      case StmtKind::Return:
        if (S.RetVal) {
          auto Observable = S.RetVal;
          const auto SourceReturn =
              F.SourceTypeHint ? F.SourceTypeHint->ReturnType : nullptr;
          if (SourceReturn && SourceReturn->Kind != NdTypeKind::Void &&
              SourceReturn->Size) {
            // A narrow source result may leave the high bytes of its physical
            // return carrier undefined. Ignore only an explicit high/low
            // CONCAT wrapper; the declared low bytes remain fully checked.
            while (Observable && Observable->Type &&
                   Observable->Type->Size > SourceReturn->Size &&
                   Observable->Kind == ExprKind::BinOp &&
                   Observable->Op == NdOp::CONCAT &&
                   Observable->Operands.size() == 2 &&
                   Observable->Operands[1] && Observable->Operands[1]->Type &&
                   Observable->Operands[1]->Type->Size >= SourceReturn->Size)
              Observable = Observable->Operands[1];
          }
          auto V = State.eval(Observable);
          if (pointerIdentity(V))
            throw Invalid(
                "block consumer returns a context or private frame address");
        }
        break;
      default:
        throw Invalid("block consumer has unmodeled control flow");
      }
    };
    proveSourceFlow(
        F, State.facts(),
        [&](Values::Facts &Facts, const HighSourceFlowNode &Node) {
          State.swap(Facts);
          Evaluate(Node);
          State.swap(Facts);
        },
        Values::merge);
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
  if (Plan.HelperHints.count(Invoke) ||
      (D != Plan.Descriptors.end() && !sameDescriptor(D->second, Descriptor)) ||
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
  for (const auto &[Entry, Hint] :
       {std::pair{Descriptor.CopyHelper, &Descriptor.CopyTypeHint},
        std::pair{Descriptor.DisposeHelper, &Descriptor.DisposeTypeHint}}) {
    if (!Entry)
      continue;
    auto [Existing, Added] = Plan.HelperHints.emplace(Entry, **Hint);
    if ((!Added &&
         !objc_projection_detail::sameHint(Existing->second, **Hint)) ||
        Plan.InvokeHints.count(Entry) || Plan.Rejections.count(Entry)) {
      Plan.Rejections[Entry] =
          "block helper has conflicting descriptor ABI evidence";
      Plan.HelperHints.erase(Entry);
      return false;
    }
  }
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
  struct ConstructionFacts {
    Values::Facts Values;
    std::map<int64_t, Byte> Memory;
    bool SawIsa = false;
    size_t size() const { return Values.size() + Memory.size() + 1; }
  } Current;
  auto &Memory = Current.Memory;
  auto Merge = [](ConstructionFacts &Into, const ConstructionFacts &From) {
    bool Changed = Values::merge(Into.Values, From.Values);
    Changed |= !Into.SawIsa && From.SawIsa;
    Into.SawIsa |= From.SawIsa;
    for (auto It = Into.Memory.begin(); It != Into.Memory.end();) {
      auto Other = From.Memory.find(It->first);
      if (Other == From.Memory.end()) {
        It = Into.Memory.erase(It);
        Changed = true;
        continue;
      }
      auto &A = It->second;
      const auto &B = Other->second;
      ++It;
      if (A.Index == B.Index && A.Width == B.Width && A.V.K == B.V.K &&
          A.V.Offset == B.V.Offset && A.V.Bits == B.V.Bits &&
          A.V.Name == B.V.Name && A.V.Producer == B.V.Producer)
        continue;
      Byte Joined{{}, 0, 1};
      if (pointerIdentity(A.V) || pointerIdentity(B.V))
        Joined.V.K = Value::UnprovenIdentity;
      else if (A.V.K == Value::Number && B.V.K == Value::Number &&
               ((A.V.Bits >> (A.Index * 8)) & 255) ==
                   ((B.V.Bits >> (B.Index * 8)) & 255)) {
        Joined.V.K = Value::Number;
        Joined.V.Bits = (A.V.Bits >> (A.Index * 8)) & 255;
      }
      // The byte is initialized on every reaching path. A scalar capture may
      // have different values, but header addresses still need one complete
      // producer recipe; a scalar merge cannot authorize a pointer field.
      if (A.Index != Joined.Index || A.Width != Joined.Width ||
          A.V.K != Joined.V.K || A.V.Bits != Joined.V.Bits || A.V.Offset ||
          !A.V.Name.empty() || A.V.Producer) {
        A = Joined;
        Changed = true;
      }
    }
    return Changed;
  };
  std::map<int64_t, ObjCStackBlockSource> Blocks;
  std::map<const HighExpr *, uint64_t> PooledHeaderValues;
  bool SawIsa = false;
  try {
    Values State(Source, Function);
    auto UntouchedEntryPointer = [&](const ExprPtr &Expr,
                                     auto &&Visit) -> bool {
      if (!Expr || !Expr->Type || Expr->Type->Size != 8)
        return false;
      if (Expr->Kind == ExprKind::Var)
        return Expr->Var.Kind == MedVar::Param && Expr->Var.SSAVer == 0 &&
               Expr->Var.RenameTag < 0 &&
               Expr->Var.Id < Function.Params.size() &&
               !State.Locals.count(
                   objc_projection_detail::localIdentity(Expr->Var));
      if ((Expr->Kind == ExprKind::Cast || Expr->Kind == ExprKind::BitCast) &&
          Expr->Operands.size() == 1)
        return Visit(Expr->Operands[0], Visit);
      return false;
    };
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
        if (B == Memory.end() || (B->second.V.K != Value::Number &&
                                  B->second.V.K != Value::ImageBits))
          throw Invalid(
              "block flags and reserved bytes are not completely initialized");
        uint64_t ValueBits = B->second.V.Bits;
        if (B->second.V.K == Value::ImageBits) {
          auto Found = PooledHeaderValues.find(B->second.V.Producer);
          if (Found == PooledHeaderValues.end()) {
            auto Data =
                readImmutableImageBytes(Image, ValueBits, B->second.Width);
            if (!Data)
              throw Invalid(
                  "block header load has no immutable scalar byte binding");
            ValueBits = 0;
            for (unsigned J = 0; J < B->second.Width; ++J)
              ValueBits |= uint64_t((*Data)[J]) << (8 * J);
            Found = PooledHeaderValues.emplace(B->second.V.Producer, ValueBits)
                        .first;
          }
          ValueBits = Found->second;
        }
        Bits |= ((ValueBits >> (B->second.Index * 8)) & 255) << (I * 8);
      }
      return Bits;
    };
    auto Constructed = [&](int64_t Base) -> ObjCStackBlockSource {
      const auto Isa = Word(Base), Invoke = Word(Base + 16),
                 Descriptor = Word(Base + 24);
      if (Isa.K != Value::Isa || Isa.Name != "_NSConcreteStackBlock" ||
          Invoke.K != Value::Number || !Image.isCodeAddress(Invoke.Bits) ||
          Descriptor.K != Value::Number || !Isa.Producer || !Invoke.Producer ||
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
      for (unsigned I = 0; I < 8; ++I) {
        const auto &V = Memory.at(Base + 8 + I).V;
        if (V.K == Value::ImageBits)
          Result.HeaderConstants.emplace(V.Producer,
                                         PooledHeaderValues.at(V.Producer));
      }
      for (uint64_t Offset = 32; Offset < D->LiteralSize; ++Offset) {
        auto B = Memory.find(Base + static_cast<int64_t>(Offset));
        if (B == Memory.end())
          continue;
        if (pointerIdentity(B->second.V))
          throw Invalid("block capture retains an unproven context or private "
                        "frame address");
        Result.InitializedCaptures.insert(Offset);
      }
      for (const auto &Capture : D->Captures) {
        if (Capture.StorageKind == ObjCBlockCaptureRange::Kind::NonObjectBytes)
          continue;
        for (uint64_t I = 0; I < Capture.Size; ++I)
          if (!Result.InitializedCaptures.count(Capture.Offset + I))
            throw Invalid(
                "block ownership field is not completely initialized");
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
      std::map<int64_t, unsigned> InvalidatedBlocks;
      auto BorrowsDisjointSuperRecord = [&](size_t Parameter,
                                            int64_t Offset) -> bool {
        const auto &Binding = E.SourceCallHint;
        if (Parameter != 0 || !Binding ||
            Binding->CallKind != CallKind::ObjCSuper2 ||
            Binding->TargetName != "objc_msgSendSuper2" ||
            E.CallTarget != "objc_msgSendSuper2" ||
            !objcSourceCallBound(E, Image, Functions, nullptr, nullptr,
                                 &Function) ||
            !frameRange(Function, Offset, 16))
          return false;
        const auto InBlock = [&](int64_t Byte) {
          for (const auto &[Base, Block] : Blocks)
            if (Byte >= Base && Byte - Base < static_cast<int64_t>(
                                                  Block.Descriptor.LiteralSize))
              return true;
          return false;
        };
        // The runtime reads only the two words of objc_super. Both must be
        // initialized and separate from every live block, including one whose
        // pointer identity survived a control-flow join.
        for (int64_t Byte = Offset; Byte < Offset + 16; ++Byte) {
          const auto Found = Memory.find(Byte);
          if (InBlock(Byte) || Found == Memory.end() ||
              Found->second.Width != 8 ||
              Found->second.Index != unsigned((Byte - Offset) % 8) ||
              pointerIdentity(Found->second.V))
            return false;
        }
        for (const auto &[Byte, Stored] : Memory)
          if (pointerIdentity(Stored.V) && !InBlock(Byte))
            return false;
        return true;
      };
      for (size_t I = 0; I < Arguments.size(); ++I) {
        if (Arguments[I].K != Value::Frame)
          continue;
        const auto Header = Memory.find(Arguments[I].Offset);
        const bool ExactBlockBase =
            Header != Memory.end() && Header->second.Index == 0 &&
            Header->second.Width == 8 && Header->second.V.K == Value::Isa &&
            Header->second.V.Name == "_NSConcreteStackBlock";
        if (!ExactBlockBase) {
          if (BorrowsDisjointSuperRecord(I, Arguments[I].Offset))
            continue;
          if (Blocks.count(Arguments[I].Offset) ||
              State.frameContainsPointerIdentity())
            throw Invalid("stack block construction exposes a nonliteral "
                          "private frame address");
          continue;
        }
        auto Block = Constructed(Arguments[I].Offset);
        const auto &Binding = E.SourceCallHint;
        std::string Error;
        std::set<std::pair<va_t, size_t>> Active;
        const bool Direct =
            Binding && Binding->CallKind == CallKind::BlockInvoke && I == 0;
        // The copy runtime preserves the stack literal and creates owned heap
        // storage through the recovered helpers. A typed pointer parameter
        // alone does not prove this behavior for an arbitrary consumer.
        const bool Runtime =
            Binding && I == 0 &&
            ((Binding->CallKind == CallKind::ObjCRuntimeCall &&
              Binding->TargetName == "objc_retainBlock") ||
             (Binding->CallKind == CallKind::DarwinRuntimeCall &&
              Binding->TargetName == "_Block_copy")) &&
            objcSourceCallBound(E, Image, Functions);
        std::optional<SourceFunctionTypeHint> Consumer;
        if (Binding && Binding->CallKind == CallKind::DarwinRuntimeCall) {
          const auto Contract =
              darwinBlockParameterContract(Image, Binding->TargetAddress, I);
          if (Contract)
            Consumer = Contract->Signature;
        } else if (Binding && Binding->CallKind == CallKind::ObjCMessage) {
          const auto Contract = objcBlockParameterContract(Image, *Binding, I);
          if (Contract)
            Consumer = Contract->Signature;
        }
        const bool DeclaredConsumer =
            Consumer && Block.Descriptor.InvokeTypeHint &&
            objc_projection_detail::sameHint(*Block.Descriptor.InvokeTypeHint,
                                             *Consumer) &&
            objcSourceCallBound(E, Image, Functions);
        if (!Direct && !Runtime && !DeclaredConsumer &&
            (!Binding || Binding->CallKind != CallKind::Native ||
             E.IsIndirectCall ||
             !noEscape(Source, Functions, Binding->TargetAddress, I, nullptr,
                       Active, Error))) {
          if (Error.empty() && Binding) {
            if (Binding->CallKind == CallKind::ObjCMessage &&
                !Binding->Selector.empty())
              Error = "Objective-C selector " + Binding->Selector;
            else if (!Binding->TargetName.empty())
              Error = "consumer " + Binding->TargetName;
          }
          if (Error.empty())
            Error = "unbound or indirect call";
          throw Invalid(
              "stack block flows to a consumer without a lifetime proof: " +
              Error);
        }
        if (DeclaredConsumer)
          InvalidatedBlocks.emplace(Block.FrameOffset,
                                    Block.Descriptor.LiteralSize);
        auto Previous = Blocks.find(Block.FrameOffset);
        if (Previous != Blocks.end() &&
            (Previous->second.InvokeEntry != Block.InvokeEntry ||
             !sameDescriptor(Previous->second.Descriptor, Block.Descriptor) ||
             Previous->second.InitializedCaptures != Block.InitializedCaptures))
          throw Invalid("stack block storage is reused with inconsistent "
                        "construction evidence");
        if (Previous == Blocks.end()) {
          Blocks.emplace(Block.FrameOffset, std::move(Block));
        } else {
          auto &Known = Previous->second;
          for (const auto &[Expression, Reference] : Block.References) {
            auto [It, Fresh] = Known.References.emplace(Expression, Reference);
            if (!Fresh && (It->second.Kind != Reference.Kind ||
                           It->second.Address != Reference.Address ||
                           It->second.Name != Reference.Name))
              throw Invalid("block header expression has conflicting roles");
          }
          for (const auto &[Expression, Bits] : Block.HeaderConstants) {
            auto [It, Fresh] = Known.HeaderConstants.emplace(Expression, Bits);
            if (!Fresh && It->second != Bits)
              throw Invalid("block header expression has conflicting bytes");
          }
        }
      }
      // A lifetime attribute says nothing about writes. Later consumers need
      // fresh construction evidence after an imported nonescaping call.
      for (const auto &[Offset, Size] : InvalidatedBlocks) {
        State.storeFrame(Value{Value::Frame, Offset}, Size, {});
        Memory.erase(Memory.lower_bound(Offset),
                     Memory.lower_bound(Offset + Size));
      }
      return {};
    };
    auto Evaluate = [&](const HighSourceFlowNode &Node) {
      if (Node.Test) {
        (void)State.eval(Node.Test);
        return;
      }
      if (!Node.Statement)
        return;
      const auto &S = *Node.Statement;
      switch (S.Kind) {
      case StmtKind::Nop:
      case StmtKind::Block:
      case StmtKind::If:
      case StmtKind::IfElse:
      case StmtKind::While:
      case StmtKind::For:
      case StmtKind::DoWhile:
      case StmtKind::Switch:
      case StmtKind::Goto:
      case StmtKind::Break:
      case StmtKind::Continue:
        break;
      case StmtKind::Assign:
        State.assign(S);
        break;
      case StmtKind::Call:
        (void)State.eval(S.CallExpr);
        break;
      case StmtKind::ExprStmt:
        (void)State.eval(S.Val);
        break;
      case StmtKind::Store: {
        auto Address = State.eval(S.StoreAddr), V = State.eval(S.StoreVal);
        Current.SawIsa |=
            V.K == Value::Isa && V.Name == "_NSConcreteStackBlock";
        SawIsa |= Current.SawIsa;
        if (Address.K != Value::Frame) {
          if (V.K == Value::Frame)
            throw Invalid("stack address escapes to nonlocal storage");
          const bool ImageStore =
              Address.K == Value::Number && S.StoreVal && S.StoreVal->Type &&
              scalarWidth(S.StoreVal->Type->Size) &&
              S.MemoryOrdering == NdMemoryOrdering::None &&
              S.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
              isFileBackedWritableImageRange(Image, Address.Bits,
                                             S.StoreVal->Type->Size);
          // A caller cannot pass an address inside this invocation's fresh
          // private frame. This applies only to an unchanged entry parameter;
          // a loaded pointer or a reassigned parameter may alias the literal.
          const bool EntryStore =
              Address.K == Value::Scalar && !pointerIdentity(V) && S.StoreVal &&
              S.StoreVal->Type && scalarWidth(S.StoreVal->Type->Size) &&
              S.MemoryOrdering == NdMemoryOrdering::None &&
              S.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
              UntouchedEntryPointer(S.StoreAddr, UntouchedEntryPointer);
          if (Current.SawIsa && !ImageStore && !EntryStore)
            throw Invalid(
                "stack block construction has an unknown aliasing write");
          break;
        }
        if (!S.StoreVal || !S.StoreVal->Type ||
            S.MemoryOrdering != NdMemoryOrdering::None ||
            S.MemoryAddressSpace != NdMemoryAddressSpace::Default)
          throw Invalid("block construction has an unsupported memory write");
        const unsigned Bytes = S.StoreVal->Type->Size;
        if (!Bytes)
          throw Invalid("block construction has an unsupported memory write");
        // Wide frame initialization is common for unrelated local arrays.
        // Preserve its byte coverage as poisoned identity: a disjoint later
        // block remains discoverable, while any header or owned capture that
        // relies on these untyped bytes still fails closed.
        const Value Stored =
            scalarWidth(Bytes) ? V : Value{Value::UnprovenIdentity};
        State.storeFrame(Address, Bytes, Stored);
        for (unsigned I = 0; I < Bytes; ++I)
          Memory[Address.Offset + I] = {Stored, I, Bytes};
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
    };
    Current.Values = State.facts();
    proveSourceFlow(
        Function, Current,
        [&](ConstructionFacts &Facts, const HighSourceFlowNode &Node) {
          // The transfer owns its Output copy. Borrow those maps as scratch
          // instead of copying every byte/local fact twice per graph node.
          // An exception abandons the whole proof; normal exits swap back the
          // identical result before the worklist joins or charges its size.
          std::swap(Current, Facts);
          State.swap(Current.Values);
          Evaluate(Node);
          State.swap(Current.Values);
          std::swap(Current, Facts);
        },
        Merge);
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
  auto Apply = [&](const auto &Hints) {
    for (const auto &[Entry, Hint] : Hints) {
      if (Plan.Rejections.count(Entry))
        continue;
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
  };
  Apply(Plan.InvokeHints);
  Apply(Plan.HelperHints);
  return Changed;
}

inline std::string objcBlockInvokeName(va_t Entry) {
  return "neverd_block_invoke_" + llvm::utohexstr(Entry, true);
}
inline std::string objcBlockOwnershipHelperName(va_t Entry) {
  return "neverd_block_helper_" + llvm::utohexstr(Entry, true);
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
    std::map<const HighExpr *, uint64_t> HeaderConstants;
    auto RequireOwnership = [&](const ObjCBlockDescriptor &Descriptor,
                                const std::set<uint64_t> &Initialized) {
      std::set<uint64_t> WritableStrongFields;
      for (const auto &Capture : Descriptor.Captures)
        if (Capture.StorageKind == ObjCBlockCaptureRange::Kind::Strong &&
            Capture.Offset % 8 == 0 && Capture.Size % 8 == 0)
          for (uint64_t I = 0; I < Capture.Size; I += 8)
            WritableStrongFields.insert(Capture.Offset + I);
      for (const auto &[Entry, Parameter] :
           {std::pair{Descriptor.CopyHelper, size_t(0)},
            std::pair{Descriptor.CopyHelper, size_t(1)},
            std::pair{Descriptor.DisposeHelper, size_t(0)}}) {
        if (!Entry)
          continue;
        auto Found = Functions.find(Entry);
        auto Hint = Plan.HelperHints.find(Entry);
        if (Plan.Rejections.count(Entry) || Found == Functions.end() ||
            Hint == Plan.HelperHints.end() || !Found->second->SourceTypeHint ||
            !objc_projection_detail::sameHint(*Found->second->SourceTypeHint,
                                              Hint->second))
          throw Invalid(
              "block ownership helper has no descriptor-bound native function");
        std::string Reason;
        std::set<std::pair<va_t, size_t>> Active;
        if (!noEscape(Source, Functions, Entry, Parameter, &Initialized, Active,
                      Reason,
                      Entry == Descriptor.CopyHelper && Parameter == 0
                          ? &WritableStrongFields
                          : nullptr))
          throw Invalid("block ownership helper capture proof failed: " +
                        Reason);
        Result.Dependencies.insert(Entry);
      }
    };
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
    };
    if (auto Stack = Plan.StackBlocks.find(Function.Entry);
        Stack != Plan.StackBlocks.end()) {
      for (const auto &Block : Stack->second) {
        RequireInvoke(Block.InvokeEntry, Block.Descriptor,
                      Block.InitializedCaptures);
        RequireOwnership(Block.Descriptor, Block.InitializedCaptures);
        RequireDescriptor(Block.Descriptor);
        HeaderConstants.insert(Block.HeaderConstants.begin(),
                               Block.HeaderConstants.end());
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
      if (auto Bits = HeaderConstants.find(Original.get());
          Bits != HeaderConstants.end()) {
        if (!Original->Type || Original->Type->Kind != NdTypeKind::Int)
          throw Invalid(
              "block header constant has an unsupported scalar representation");
        *Expression = *HighExpr::makeConst(Bits->second, Original->Type->Size);
        Expression->Type = Original->Type;
        return Expression;
      }
      std::optional<ObjCBlockAddressBinding> Address;
      if (auto Found = References.find(Original.get());
          Found != References.end())
        Address = Found->second;
      else if (Original->Kind == ExprKind::Const && Original->Type &&
               Original->Type->Size == 8) {
        if (auto Global = Plan.Globals.find(Original->ConstVal);
            Global != Plan.Globals.end()) {
          RequireDescriptor(Global->second.Descriptor);
          RequireInvoke(Global->second.InvokeEntry, Global->second.Descriptor,
                        {});
          RequireOwnership(Global->second.Descriptor, {});
          Result.Literals.insert(Global->first);
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
    if (Plan.InvokeHints.count(Function.Entry) ||
        Plan.HelperHints.count(Function.Entry)) {
      Result.Function.Name = Plan.InvokeHints.count(Function.Entry)
                                 ? objcBlockInvokeName(Function.Entry)
                                 : objcBlockOwnershipHelperName(Function.Entry);
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
  if (Binding.DoesNotReturn || Hint.Architecture != Image.Arch ||
      !validateSourceABI(Hint, Error))
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
           Function->second->SourceTypeHint->Convention ==
               SourceFunctionTypeHint::ConventionKind::C &&
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
inline std::string renderObjCBlockSourceHelpers(
    const ObjCBlockSourcePlan &Plan, const std::set<va_t> &Descriptors,
    const std::set<va_t> &Literals, std::set<std::string> &SharedFunctions) {
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
    for (va_t LiteralAddress : Literals) {
      const auto Global = Plan.Globals.find(LiteralAddress);
      if (Global == Plan.Globals.end())
        throw Invalid("block helper literal has no source plan");
      if (Global->second.Descriptor.Address != Address)
        continue;
      Globals.push_back(&Global->second);
    }
    const bool Layout = D.Flags & (UINT32_C(1) << 31);
    const std::string Name = objcBlockHelperName(false, Address);
    SharedFunctions.insert(Name);
    OS << "\nuintptr_t " << Name << "(void) {\n"
       << "  _Static_assert(sizeof(void *) == 8, \"Block source requires "
          "64-bit pointers\");\n"
       << "  struct neverd_block_descriptor { uint64_t reserved, size;";
    if (D.CopyHelper)
      OS << " void (*copy)(void *, void *); void (*dispose)(void *);";
    OS << " const char *signature;";
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
       << D.LiteralSize;
    if (D.CopyHelper) {
      const auto CopyName = objcBlockOwnershipHelperName(D.CopyHelper);
      const auto DisposeName = objcBlockOwnershipHelperName(D.DisposeHelper);
      SharedFunctions.insert(CopyName);
      SharedFunctions.insert(DisposeName);
      OS << ", &" << CopyName << ", &" << DisposeName;
    }
    OS << ", " << String(D.Signature);
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
         << (Layout ? 32 : 24) + (D.CopyHelper ? 16 : 0) + I * 32 << "; }\n";
    }
  }
  return Source;
}

} // namespace neverd::sdk
#endif
